#include "resource_io.hpp"

#include <filesystem>
#include <fstream>
#include <memory>

#include "libca/core/datatype.hpp"

#include <archive.h>
#include <archive_entry.h>

namespace mjt::base {

//── JarFileImporter ──────────────────────────────────────────

namespace {

//按 entry 名/内容分拣为 Manifest / Class / Unknown（import 的两个重载共用）。
void classifyAndAdd(IResourceContainer& container, const std::string& entryName,
                    const std::vector<ca::u8>& data)
{
    if (entryName == JarUtil::kManifestFileName) {
        container.addResource(
            entryName, std::make_unique<ManifestResource>(ManifestResource::fromBytes(data)));
        return;
    }
    if (JarUtil::isClassFile(entryName) && JarUtil::isClassFile(data)) {
        //混淆器可能把类 entry 命名为 *.class/（尾斜杠），剥掉后再用作 URI/location，
        //避免尾斜杠泄漏到类名派生与容器 key。
        auto normalized = entryName;
        if (normalized.size() >= 7 &&
            normalized.compare(normalized.size() - 7, 7, ".class/") == 0)
            normalized.erase(normalized.size() - 1);
        container.addResource(normalized, std::make_unique<ClassResource>(normalized, data));
        return;
    }
    container.addResource(entryName, std::make_unique<UnknownResource>(entryName, data));
}

}   //namespace

//遍历 jar 条目，根据类型分拣为 ManifestResource / ClassResource / UnknownResource
void JarFileImporter::importResource(IResourceContainer& container)
{
    reader_->read([&](const std::string& entryName, const std::vector<ca::u8>& data) {
        classifyAndAdd(container, entryName, data);
    });
}

//带进度的导入：每个条目回调 progress，单次解析无重复 IO（见 jar_reader readWithProgress）。
void JarFileImporter::importResourceWithProgress(IResourceContainer&     container,
                                                 const ProgressCallback& progress)
{
    reader_->readWithProgress(
        [&](int index, int total, const std::string& entryName, const std::vector<ca::u8>& data) {
            classifyAndAdd(container, entryName, data);
            if (progress) progress(index, total, entryName);
        });
}

//── JarFileExporter ──────────────────────────────────────────

//遍历容器中所有资源，按类型写入 jar 条目
void JarFileExporter::exportResource(IResourceContainer& container)
{
    writer_->write([&](IEntryWriter& entryWriter) {
        container.visitResources([&](const std::string&, const IResource& resource) {
            if (auto* cls = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource)) {
                entryWriter.writeEntry(cls->getLocation(), cls->get());
            }
            else if (auto* mf = ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource)) {
                entryWriter.writeEntry(JarUtil::kManifestFileName, mf->get().toBytes());
            }
            else if (auto* unk = ca::core::tag_cast::dyn_cast<const UnknownResource>(&resource)) {
                entryWriter.writeEntry(unk->getLocation(), unk->get());
            }
        });
    });
}

//── SimpleClassFileExporter ──────────────────────────────────

SimpleClassFileExporter::SimpleClassFileExporter(std::string uri, std::string path)
    : uri_(std::move(uri))
    , path_(std::move(path))
{}

//按 URI 查找 ClassResource 并写出到磁盘；空 URI 时使用容器中唯一的资源
void SimpleClassFileExporter::exportResource(IResourceContainer& container)
{
    const ClassResource* classRes = nullptr;
    if (uri_.empty()) {
        //未指定 URI，取容器中唯一的资源（必须恰好一个且为 ClassResource）
        if (container.size() != 1)
            throw std::runtime_error("uri is empty and container size is not 1");
        container.visitResources([&](const std::string&, const IResource& res) {
            classRes = ca::core::tag_cast::dyn_cast<const ClassResource>(&res);
        });
        if (!classRes) throw std::runtime_error("the only resource is not ClassResource");
    }
    else {
        auto* res = container.getResource(uri_);
        if (!res) throw std::runtime_error("uri not found in container: " + uri_);
        classRes = ca::core::tag_cast::dyn_cast<const ClassResource>(res);
        if (!classRes) throw std::runtime_error("resource is not ClassResource: " + uri_);
    }
    std::ofstream out(std::filesystem::u8path(path_), std::ios::binary);
    if (!out) throw std::runtime_error("failed to open: " + path_);
    const auto& data = classRes->get();
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) throw std::runtime_error("failed to flush: " + path_);
    out.close();
    if (!out) throw std::runtime_error("failed to close: " + path_);
}

//── SimpleClassFileImporter ──────────────────────────────────

//从磁盘读取 class 文件字节并作为 ClassResource 加入容器
void SimpleClassFileImporter::importResource(IResourceContainer& container)
{
    std::ifstream in(std::filesystem::u8path(path_), std::ios::binary);
    if (!in) throw std::runtime_error("failed to open: " + path_);
    in.seekg(0, std::ios::end);
    auto size = static_cast<std::size_t>(in.tellg());
    in.seekg(0, std::ios::beg);
    std::vector<ca::u8> bytes(size);
    if (size > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    container.addResource(path_, std::make_unique<ClassResource>(path_, std::move(bytes)));
}

//── ZipUtil ──────────────────────────────────────────────────

//将归档条目解压到 targetDir。
//
//使用 libarchive 的"自动嗅探"能力：support_filter_all + support_format_all
//在 archive_read_open_filename 后会按内容（而非后缀）识别压缩与归档格式，
//因此天然覆盖 zip / tar / tar.xz / tar.gz 等组合，无需手工 detectFormat。
namespace {

//判断 child 是否位于 parent 目录之下（含 parent 自身）。两边都先规范化。
//用 lexically+字符串前缀比较，避免 find("/a/b" in "/a/bc") 这类误命中。
bool isUnderDir(const std::filesystem::path& child, const std::filesystem::path& parent)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path        c = fs::weakly_canonical(child, ec);
    fs::path        p = fs::weakly_canonical(parent, ec);
    if (ec) return false;
    //lexically_relative：若 c 不在 p 下，结果以 ".." 开头或为空。
    auto rel = c.lexically_relative(p);
    if (rel.empty()) return false;
    for (const auto& seg : rel) {
        if (seg == "..") return false;
    }
    return true;
}

//把单个条目写到磁盘（等价于 archive_read_extract，但显式落到 targetDir，
//避免依赖进程 CWD）。返回 ARCHIVE_OK 表示继续下一条目，否则返回 archive
//错误码；具体错误信息通过 errRef 回传。
int writeEntryToDisk(struct archive* a, struct archive_entry* entry,
                     const std::filesystem::path& targetDir, std::string& errRef)
{
    namespace fs      = std::filesystem;
    const char* pname = archive_entry_pathname(entry);
    if (!pname) return ARCHIVE_OK;   //跳过无名条目
    fs::path out = targetDir / pname;

    //防目录穿越：条目名可能含 ".." 或绝对路径，解析后必须仍在 targetDir 之下。
    if (!isUnderDir(out, targetDir)) {
        errRef = "entry escapes target dir: " + std::string(pname);
        return ARCHIVE_FAILED;
    }

    if (archive_entry_filetype(entry) == AE_IFDIR) {
        std::error_code ec;
        fs::create_directories(out, ec);
    }
    else {
        std::error_code ec;
        fs::create_directories(out.parent_path(), ec);
        const void* buff   = nullptr;
        size_t      size   = 0;
        la_int64_t  offset = 0;
        //header 已由外层 read_next_header 读出，这里用 read_data_block 流式写文件内容。
        std::ofstream ofs(out, std::ios::binary);
        if (!ofs) {
            errRef = "cannot open output: " + out.string();
            return ARCHIVE_FAILED;
        }
        while (true) {
            int r = archive_read_data_block(a, &buff, &size, &offset);
            if (r == ARCHIVE_EOF) break;
            if (r != ARCHIVE_OK) {
                errRef =
                    archive_error_string(a) ? archive_error_string(a) : "read_data_block failed";
                return r;
            }
            if (size > 0)
                ofs.write(static_cast<const char*>(buff), static_cast<std::streamsize>(size));
        }
    }
    //保持文件权限位（best-effort，失败忽略——CI/解压场景不强求权限还原）。
    std::error_code ign;
    fs::permissions(out, static_cast<fs::perms>(archive_entry_perm(entry)), ign);
    return ARCHIVE_OK;
}

//打开归档并逐条目解压到 target（绝对路径，已 canonical）。onlyEntry 非空时仅解压
//pathname 与之完全匹配的那一个条目，其余跳过；找不到匹配条目则抛异常。
void extractArchiveTo(const std::string& archivePath, const std::filesystem::path& target,
                      const std::string* onlyEntry)
{
    struct archive* a = archive_read_new();
    if (!a) throw std::runtime_error("archive_read_new failed");
    //RAII：无论成败都释放 archive 句柄。
    std::unique_ptr<struct archive, decltype(&archive_read_free)> guard(a, archive_read_free);

    archive_read_support_filter_all(a);   //xz / gzip / bzip2 / zstd ...
    archive_read_support_format_all(a);   //zip / tar / ...

    int r = archive_read_open_filename(a, archivePath.c_str(), 10240);
    if (r != ARCHIVE_OK) {
        throw std::runtime_error("open archive failed: " + archivePath + " — " +
                                 (archive_error_string(a) ? archive_error_string(a) : ""));
    }

    //逐条目读取。read_next_header 正常读完返回 ARCHIVE_EOF（循环自然退出），
    //只有返回 ARCHIVE_FATAL 等才视为失败——不要用 archive_errno 事后判断，
    //它是"最近一次错误"，正常读完未必清零，会误报。
    struct archive_entry* entry = nullptr;
    std::string           err;
    bool                  found = false;
    for (;;) {
        int hr = archive_read_next_header(a, &entry);
        if (hr == ARCHIVE_EOF) break;
        if (hr != ARCHIVE_OK) {
            throw std::runtime_error(
                "read header failed: " +
                std::string(archive_error_string(a) ? archive_error_string(a) : ""));
        }
        if (onlyEntry) {
            const char* pname = archive_entry_pathname(entry);
            //未读的条目数据会在下次 read_next_header 时被 libarchive 自动跳过。
            if (!pname || *onlyEntry != pname) continue;
            found = true;
        }
        if (writeEntryToDisk(a, entry, target, err) != ARCHIVE_OK) {
            const char* pname = archive_entry_pathname(entry);
            throw std::runtime_error("extract failed at '" + std::string(pname ? pname : "?") +
                                     "': " + err);
        }
        if (onlyEntry) break;   //目标条目已提取，无需继续扫描
    }
    if (onlyEntry && !found)
        throw std::runtime_error("entry not found in archive '" + archivePath + "': " + *onlyEntry);
}

}   //namespace

//使用 libarchive 解压到目标目录（支持 zip / tar / tar.xz / tar.gz 等）
void ZipUtil::extract(const std::string& archivePath, const std::string& targetDir)
{
    namespace fs = std::filesystem;
    fs::create_directories(targetDir);
    extractArchiveTo(archivePath, fs::weakly_canonical(targetDir), nullptr);
}

void ZipUtil::unzip(const std::string& zipPath, const std::string& targetDir)
{
    extract(zipPath, targetDir);
}

//从压缩包 path 中提取名为 file 的单个条目到 destination 目录（保留条目内相对路径结构）。
//file 需与归档内条目 pathname 完全匹配（如 "com/foo/Bar.class"）。
void ZipUtil::extractFile(const std::string& path, const std::string& file,
                          const std::string& destination)
{
    namespace fs = std::filesystem;
    fs::create_directories(destination);
    extractArchiveTo(path, fs::weakly_canonical(destination), &file);
}

}   //namespace mjt::base
