/// @file        cmd_strings.cpp
/// @brief       `mjt strings` 命令核心：常量池字符串抽取（整 jar / 单类）。
///
/// 地基：shared 层 StringExtractor（libjclassfile 读面 + 规范 UTF-8 文本解码，
/// lenient 回调注入与全仓同款）。抽取口径 = 常量池 Utf8 / String / Class 三种
/// 条目（每种条目记一次出现：字符串字面量同时产出 CONSTANT_String 与其背书
/// Utf8，kind 计数在 --json 披露）；--min-len 按规范 UTF-8 字节长度过滤；
/// 值跨类去重（出现次数 = 常量池条目数，跨类累加）。类遍历走 workspace
/// listClassSummaries（畸形 class 骨架解码失败即跳过）+ readResource 字节。
#include "commands.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "shared/decompiler.hpp"
#include "shared/i18n_errors.hpp"
#include "shared/json_util.hpp"
#include "shared/output_sink.hpp"
#include "shared/string_extractor.hpp"
#include "workspace/workspace.hpp"
#include "workspace/analysis_ops.hpp"

namespace mjt::cli {
namespace {

/// Utf8String/UTF-8 std::string → 借用视图（与 info / decompile 同款构造）。
ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// StringsError 便捷构造（i18n 渲染）。
StringsError stringsError(StringsError::Kind kind, const char* key,
                          std::initializer_list<std::string_view> args = {})
{
    StringsError error;
    error.kind    = kind;
    error.message = shared::trfStr(key, args);
    return error;
}

/// 去重表行：出现次数 + 三种常量池条目的 kind 计数。
struct ValueCount
{
    ca::i64 total = 0;
    ca::i64 utf8  = 0;
    ca::i64 str   = 0;
    ca::i64 cls   = 0;
};

/// 文本行内安全转义：控制字符（C0）转 \xNN，\n/\r/\t/\\ 按短名——保证值
/// 恒单行（常量池字符串可含换行）。规范 UTF-8 多字节序列原样保留。
std::string escapeForText(const std::string& value)
{
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(value.size());
    for (const char raw : value) {
        const auto ch = static_cast<unsigned char>(raw);
        switch (ch) {
        case '\n': out += "\\n"; continue;
        case '\r': out += "\\r"; continue;
        case '\t': out += "\\t"; continue;
        case '\\': out += "\\\\"; continue;
        default: break;
        }
        if (ch < 0x20) {
            out += "\\x";
            out += hex[ch >> 4];
            out += hex[ch & 0x0f];
            continue;
        }
        out += raw;
    }
    return out;
}

}   // namespace

ca::Result<StringsReport, StringsError> collectStrings(const std::string& jarPath,
                                                       const std::string& classFilter,
                                                       ca::i64 minimumLen)
{
    if (minimumLen < 1)
        return ca::Err(stringsError(StringsError::Kind::InvalidArgument,
                                    "mjt.cli.strings.err.invalid_min_len",
                                    {std::to_string(minimumLen)}));

    workspace::WorkspaceBase ws;
    auto opened = ws.open(refOf(jarPath));
    if (opened.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(opened).unwrap_err();
        return ca::Err(stringsError(StringsError::Kind::Runtime, "mjt.cli.strings.err.open_failed",
                                    {info.message}));
    }

    auto summaries = workspace::listClassSummaries(*ws.workingContainer());
    if (summaries.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(summaries).unwrap_err();
        return ca::Err(stringsError(StringsError::Kind::Runtime, "mjt.cli.strings.err.open_failed",
                                    {info.message}));
    }
    auto classes = std::move(summaries).unwrap();

    StringsReport report;
    report.jarPath     = jarPath;
    report.classFilter = classFilter;

    // 单类模式：内部名精确匹配（Multi-Release 族全部同名条目都扫）。
    if (!classFilter.empty()) {
        std::vector<workspace::ClassSummary> selected;
        for (auto& summary : classes) {
            if (summary.internalName.to_std_string() == classFilter) selected.push_back(std::move(summary));
        }
        if (selected.empty())
            return ca::Err(stringsError(StringsError::Kind::ClassNotFound,
                                        "mjt.cli.strings.err.class_not_found", {classFilter}));
        classes = std::move(selected);
    }
    report.classesScanned = static_cast<ca::i64>(classes.size());

    // 畸形类计数：jar 内 .class 条目总数（central directory 口径）− 可解析类。
    if (classFilter.empty()) {
        auto totalEntries = countJarClassEntries(jarPath);
        if (totalEntries.is_ok())
            report.classesSkipped = static_cast<ca::i64>(totalEntries.unwrap()) -
                                    report.classesScanned;
    }

    StringExtractor extractor;
    std::map<std::string, ValueCount, std::less<>> unique;
    for (const auto& summary : classes) {
        const std::string internalName = summary.internalName.to_std_string();
        auto content                   = workspace::readResource(
        *ws.workingContainer(), summary.resourceUri.ref());
        if (content.is_err()) continue;   // 类级读取失败按畸形口径跳过
        const auto entries = extractor.extractAll(std::move(content).unwrap().bytes, internalName);
        for (auto& entry : entries) {
            if (static_cast<ca::i64>(entry.value.size()) < minimumLen) continue;
            auto& slot = unique[entry.value];
            ++slot.total;
            switch (entry.type) {
            case StringEntry::Utf8: ++slot.utf8; break;
            case StringEntry::String: ++slot.str; break;
            case StringEntry::Class: ++slot.cls; break;
            }
        }
    }

    report.values.reserve(unique.size());
    for (auto& [value, count] : unique)
        report.values.push_back({count.total, count.utf8, count.str, count.cls, value});
    std::sort(report.values.begin(), report.values.end(),
              [](const StringsValue& left, const StringsValue& right) {
                  if (left.count != right.count) return left.count > right.count;
                  return left.value < right.value;
              });
    report.uniqueValues = static_cast<ca::i64>(report.values.size());
    report.occurrences  = 0;
    for (const auto& item : report.values) report.occurrences += item.count;
    return ca::Ok(std::move(report));
}

void renderStringsText(const StringsReport& report, OutputSink& sink)
{
    sink.puts(shared::trfStr("mjt.cli.strings.title", {report.jarPath}));
    if (!report.classFilter.empty())
        sink.puts(shared::trfStr("mjt.cli.strings.class_filter", {report.classFilter}));
    sink.puts(shared::trfStr("mjt.cli.strings.scanned",
                             {std::to_string(report.classesScanned),
                              std::to_string(report.classesSkipped)}));
    sink.puts(shared::trfStr("mjt.cli.strings.summary",
                             {std::to_string(report.uniqueValues),
                              std::to_string(report.occurrences)}));
    for (const auto& item : report.values)
        sink.puts("  " + std::to_string(item.count) + " " + escapeForText(item.value));
}

void renderStringsJson(const StringsReport& report, OutputSink& sink)
{
    std::string out = "{\n";
    out += "  \"jar\": \"" + json::escape(report.jarPath) + "\",\n";
    if (!report.classFilter.empty())
        out += "  \"class\": \"" + json::escape(report.classFilter) + "\",\n";
    out += "  \"classes_scanned\": " + std::to_string(report.classesScanned) + ",\n";
    out += "  \"classes_skipped\": " + std::to_string(report.classesSkipped) + ",\n";
    out += "  \"occurrences\": " + std::to_string(report.occurrences) + ",\n";
    out += "  \"unique_values\": " + std::to_string(report.uniqueValues) + ",\n";
    out += "  \"values\": [";
    for (std::size_t index = 0; index < report.values.size(); ++index) {
        const auto& item = report.values[index];
        if (index != 0) out += ", ";
        out += "\n    {\"value\": \"" + json::escape(item.value) +
               "\", \"count\": " + std::to_string(item.count) + ", \"kinds\": {";
        std::string kinds;
        if (item.utf8 != 0)
            kinds += (kinds.empty() ? "" : ", ") + std::string("\"utf8\": ") + std::to_string(item.utf8);
        if (item.string != 0)
            kinds += (kinds.empty() ? "" : ", ") + std::string("\"string\": ") + std::to_string(item.string);
        if (item.cls != 0)
            kinds += (kinds.empty() ? "" : ", ") + std::string("\"class\": ") + std::to_string(item.cls);
        out += kinds + "}}";
    }
    if (!report.values.empty()) out += "\n  ";
    out += "]\n}";
    sink.puts(out);
}

}   // namespace mjt::cli
