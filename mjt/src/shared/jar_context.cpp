#include "jar_context.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>

#include "libca/fs/file_util.hpp"
#include "libca/i18n/i18n.hpp"
#include "mbase/jar/jar_reader.hpp"
#include "mbase/model/resource_io.hpp"
#include "mbase/model/resource_helper.hpp"
#include "libca/core/tag_cast.hpp"

using namespace mjt::base;

namespace {

bool isClassFilePath(const std::string& path)
{
    //path 是 FileUtil::list_files 结果（libca 全库窄串 UTF-8 契约，头注口径），
    //显式 u8path 不经 ACP 隐式窄转码（u8path 清扫；扩展名比较为 ASCII 字面量，
    //零漂移）。
    const std::filesystem::path fsPath = std::filesystem::u8path(path);
    auto                  ext = fsPath.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return ext == ".class";
}

std::string relativeResourceUri(const std::filesystem::path& root,
                                const std::filesystem::path& file)
{
    std::error_code ec;
    auto            relative = std::filesystem::relative(file, root, ec);
    if (ec) relative = file.filename();
    //容器 URI 面向 UTF-8 符号名（与 jar 条目名同域）：generic_u8string 返回
    //UTF-8 字节（C++17 std::string）；原 generic_string 按 ACP 再编码，非
    //UTF-8 ACP 进程里把合法 UTF-8 目录导入的类名转 mojibake。
    return relative.generic_u8string();
}

bool importClassDirectory(ResourceContainer& container, const std::string& path, std::string& error)
{
    auto files = ca::fs::FileUtil::list_files(path, true);
    if (files.is_err()) {
        error = ca::i18n::tr("mjt.shared.jctx.err.list_dir_failed");
        return false;
    }

    //path 为 open 的 argv 目录值（UTF-8 契约；u8path 清扫锚点），list_files
    //结果同为 libca UTF-8 窄串契约——两侧一律显式 u8path，路径代数在宽域精确
    //进行，URI 经 generic_u8string 回 UTF-8 字节域。
    const auto root = std::filesystem::u8path(path);
    for (const auto& file : files.unwrap()) {
        if (!isClassFilePath(file)) continue;

        auto bytes = ca::fs::FileUtil::read_all_bytes(file);
        if (bytes.is_err()) {
            error = ca::i18n::trf("mjt.shared.jctx.err.read_class_failed", {file.c_str()}).c_str();
            return false;
        }

        auto data = bytes.unwrap();
        //空 buffer 短路：对空数据做 as_ptr+0 指针运算是技术性 UB，直接
        //落一个空 class 资源（后续阶段按 truncated 处理），语义不变。
        std::vector<ca::u8> content;
        if (data.remaining() > 0) {
            content.assign(data.as_ptr(), data.as_ptr() + data.remaining());
        }
        auto                uri = relativeResourceUri(root, std::filesystem::u8path(file));
        container.addResource(uri, std::make_unique<ClassResource>(uri, std::move(content)));
    }

    if (container.size() == 0) {
        error = ca::i18n::tr("mjt.shared.jctx.err.no_class_files");
        return false;
    }
    return true;
}

}   //namespace

//RTTI 已禁用，使用 ca::core::tag_cast::dyn_cast 替代 dynamic_cast
//注意：ca::core::tag_cast::dyn_cast 只返回 const 指针；需要 mutable 时需用 isa + cast 组合

bool JarContext::open(const std::string& path, const IJarReader::EntryNameDecoder& decoder)
{
    //入口清空陈旧错误；失败时重置容器，避免半成品残留（二次 open 失败后成功
    //的场景下 error 会返回上一次的消息）。
    lastError_.clear();
    container   = std::make_unique<ResourceContainer>();
    jarPath     = path;

    if (ca::fs::FileUtil::is_directory(path)) {
        if (!importClassDirectory(
                *static_cast<ResourceContainer*>(container.get()), path, lastError_)) {
            container.reset();
            return false;
        }
        return true;
    }

    try {
        //用 JvmJarReader 打开（兼容损坏 JAR）；decoder 在移动前注入（opt-in
        //宽容解码，未注入 = 原始字节直通零漂移）。
        auto reader = std::make_unique<JvmJarReader>(path);
        if (decoder) reader->setEntryNameDecoder(decoder);
        ResourceHelper::importFromJar(
            *static_cast<ResourceContainer*>(container.get()), path, std::move(reader));
    }
    catch (const std::exception& e) {
        lastError_ = ca::i18n::trf("mjt.shared.jctx.err.open_jar_failed", {e.what()}).c_str();
        container.reset();
        return false;
    }
    catch (...) {
        //非 std::exception 异常与上面同等处理：报错 + 重置容器，避免半成品残留。
        lastError_ = ca::i18n::tr("mjt.shared.jctx.err.open_jar_failed_unknown");
        container.reset();
        return false;
    }

    if (container->size() == 0) {
        lastError_ = ca::i18n::tr("mjt.shared.jctx.err.empty_jar");
        container.reset();
        return false;
    }

    return true;
}

void JarContext::visitClasses(
    std::function<void(const std::string&, const ClassResource&)> fn) const
{
    if (!container) return;

    container->visitResources([&](const std::string& uri, const IResource& res) {
        if (res.getType() == ResourceType::Class) {
            const auto* cls = ca::core::tag_cast::dyn_cast<const ClassResource>(&res);
            if (cls) {
                fn(uri, *cls);
            }
        }
    });
}

void JarContext::transformClasses(std::function<void(const std::string&, ClassResource&)> fn)
{
    if (!container) return;

    //IResourceContainer 没有直接的 mutable 遍历接口。
    //v1 方案：通过 URI 列表逐个 getResource + cast。
    //由于 dyn_cast 只返回 const，用 isa + cast 组合。
    container->visitResources([&](const std::string& uri, const IResource& res) {
        if (res.getType() == ResourceType::Class && ca::core::tag_cast::isa<const ClassResource>(&res)) {
            auto* raw = container->getResource(uri);
            //getResource 按设计必命中（uri 来自同一容器的遍历），仍做判空防御：
            //失败时跳过而不是对空指针解引用。
            if (raw == nullptr) return;
            auto* cls = ca::core::tag_cast::cast<ClassResource>(raw);
            fn(uri, *cls);
        }
    });
}
