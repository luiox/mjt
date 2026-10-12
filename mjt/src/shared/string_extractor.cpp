#include "string_extractor.hpp"

#include <cstring>
#include <vector>

#include "jclassfile/io/class_file_reader.hpp"
#include "jclassfile/model/class_file.hpp"
#include "jclassfile/model/const_pool.hpp"
#include "jclassfile/model/constant/cp_class.hpp"
#include "jclassfile/model/constant/cp_entry.hpp"
#include "jclassfile/model/constant/cp_string.hpp"
#include "jclassfile/model/constant/cp_utf8.hpp"

#include "shared/text_codec.hpp"

namespace jcf = jclassfile::io;
namespace jcm = jclassfile::model;

//── extractAll ─────────────────────────────────────────────────
std::vector<StringEntry> StringExtractor::extractAll(const std::vector<ca::u8>& classBytes,
                                                     const std::string&         className)
{
    std::vector<StringEntry> result;

    try {
        jcf::ClassFileReader reader;
        reader.setSkipUnknownPoolTags(true);
        //反馈 bug2：GBK 窄字节常量池不再整类拒读。严格 modified-UTF-8
        //校验失败的字节交 text 规范路径恢复（合法 UTF-8 原样 / GBK 转码 /
        //U+FFFD 兜底），strings 输出保持单一规范 UTF-8 口径；未注入回调时
        //libjclassfile 保持原抛出行为，其他消费面零影响。
        reader.setLenientTextDecoder([](std::string_view raw) {
            return mjt::shared::text::toCanonicalUtf8(raw);
        });
        auto cf = reader.read(classBytes);

        auto& pool  = cf.getPool();
        auto  count = static_cast<int>(pool.size());
        for (int i = 1; i < count; ++i) {
            const auto* entry = pool.get(i);
            if (!entry) continue;

            switch (entry->tag()) {
            case jcm::CpTag::UTF8:
            {
                auto* utf = static_cast<const jcm::CpUtf8*>(entry);
                if (!utf->text().empty())
                    result.push_back({className, utf->text(), StringEntry::Utf8});
                break;
            }
            case jcm::CpTag::STRING:
            {
                auto* str = static_cast<const jcm::CpString*>(entry);
                if (str->value() && !str->value()->text().empty()) {
                    result.push_back({className, str->value()->text(), StringEntry::String});
                }
                break;
            }
            case jcm::CpTag::CLASS:
            {
                auto* cls = static_cast<const jcm::CpClass*>(entry);
                if (cls->name() && !cls->name()->text().empty()) {
                    result.push_back({className, cls->name()->text(), StringEntry::Class});
                }
                break;
            }
            default: break;
            }
        }
    }
    //仅捕获标准异常（class 解析失败、内存分配失败等）。catch(...) 会吞掉
    //std::bad_alloc 等严重异常，长期运行时难以排查损坏 class，故收窄。
    catch (const std::exception&) {
        return {};
    }

    return result;
}

//── countAll ───────────────────────────────────────────────────
//与 extractAll 同口径（非空 Utf8 / 可解析 String / 可解析 Class），
//只计数不构造 StringEntry。
int StringExtractor::countAll(const std::vector<ca::u8>& classBytes)
{
    int count = 0;
    try {
        jcf::ClassFileReader reader;
        reader.setSkipUnknownPoolTags(true);
        //反馈 bug2：GBK 窄字节常量池不再整类拒读。严格 modified-UTF-8
        //校验失败的字节交 text 规范路径恢复（合法 UTF-8 原样 / GBK 转码 /
        //U+FFFD 兜底），strings 输出保持单一规范 UTF-8 口径；未注入回调时
        //libjclassfile 保持原抛出行为，其他消费面零影响。
        reader.setLenientTextDecoder([](std::string_view raw) {
            return mjt::shared::text::toCanonicalUtf8(raw);
        });
        auto cf = reader.read(classBytes);

        auto& pool  = cf.getPool();
        auto  total = static_cast<int>(pool.size());
        for (int i = 1; i < total; ++i) {
            const auto* entry = pool.get(i);
            if (!entry) continue;

            switch (entry->tag()) {
            case jcm::CpTag::UTF8:
            {
                auto* utf = static_cast<const jcm::CpUtf8*>(entry);
                if (!utf->text().empty()) ++count;
                break;
            }
            case jcm::CpTag::STRING:
            {
                auto* str = static_cast<const jcm::CpString*>(entry);
                if (str->value() && !str->value()->text().empty()) ++count;
                break;
            }
            case jcm::CpTag::CLASS:
            {
                auto* cls = static_cast<const jcm::CpClass*>(entry);
                if (cls->name() && !cls->name()->text().empty()) ++count;
                break;
            }
            default: break;
            }
        }
    }
    catch (const std::exception&) {
        return 0;
    }
    return count;
}

//── extractEncrypted ───────────────────────────────────────────
//Filters extractAll output by heuristic rules

static bool isPrintable(const std::string& s)
{
    for (char c : s) {
        auto uc = static_cast<unsigned char>(c);
        if (uc < 0x20 && uc != '\t' && uc != '\n' && uc != '\r') return false;
        if (uc > 0x7E) return false;
    }
    return true;
}

static bool isBase64Like(const std::string& s, size_t minLength)
{
    if (s.length() < minLength) return false;
    for (char c : s) {
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '+' || c == '/' || c == '='))
            return false;
    }
    return true;
}

static bool isHexLike(const std::string& s, size_t minLength)
{
    if (s.length() < minLength) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return true;
}

static bool isEncrypted(const std::string& s, const EncryptHeuristic& h)
{
    //minLength 钳制为非负：负值经 size_t 转换会变成极大数，把全部字符串滤掉。
    const auto minLength = h.minLength < 0 ? 0 : static_cast<size_t>(h.minLength);
    if (s.length() < minLength) return false;
    if (h.checkInvisible && !isPrintable(s)) return true;
    if (h.checkBase64 && isBase64Like(s, minLength)) return true;
    if (h.checkHex && isHexLike(s, minLength)) return true;
    return false;
}

std::vector<StringEntry> StringExtractor::extractEncrypted(const std::vector<ca::u8>& classBytes,
                                                           const std::string&         className,
                                                           const EncryptHeuristic&    h)
{
    auto                     all = extractAll(classBytes, className);
    std::vector<StringEntry> result;
    for (auto& entry : all) {
        if (isEncrypted(entry.value, h)) {
            result.push_back(std::move(entry));
        }
    }
    return result;
}
