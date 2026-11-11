#include "container_io.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <ostream>

#include "libca/core/tag_cast.hpp"
#include "libca/zip/checksum.hpp"

#include "jzip/decompressor.hpp"
#include "jzip/zip_io.hpp"

#include "mjbase/model/resource_helper.hpp"

#include "edit_support.hpp"
#include "export_manifest.hpp"

namespace mjt::workspace {

/// @file container_io.cpp
/// @brief importer/exporter 注册表实现：jar / classfile 两行默认注册。
///
/// jar 导出行 = #74 保真口径（自装 zip：条目集 = 容器条目集、未编辑
/// MANIFEST 逐字节复制源字节、DOS 纪元时间戳可重现、临时文件 + rename
/// 原子落盘）；classfile 行 = ResourceHelper::import/exportFromClassFile 的
/// 原子化包装。新格式经 ContainerIoRegistry::registerEntry 插入。

std::filesystem::path fsPath(const std::string& text)
{
    return std::filesystem::u8path(text);
}

std::string pathToUtf8(const std::filesystem::path& path)
{
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(u8.c_str()), u8.size());
}

WorkspaceErrorInfo modelFailure(const std::string& operation, model::ModelError modelError)
{
    switch (modelError)
    {
    case model::ModelError::NotFound:
        return error(WorkspaceError::NotFound, operation, "archive or entry not found");
    case model::ModelError::PathEscape:
        return error(WorkspaceError::InvalidArgument, operation,
                     "archive entry escapes target dir");
    default:
        return error(WorkspaceError::DataLoss, operation, model::to_string(modelError));
    }
}

std::unique_ptr<model::IResource> cloneResource(const model::IResource& resource)
{
    if (const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource))
        return std::make_unique<model::ClassResource>(locationText(*classResource),
                                                      classResource->get());
    if (const auto* manifestResource =
            ca::core::tag_cast::dyn_cast<const model::ManifestResource>(&resource))
        return std::make_unique<model::ManifestResource>(manifestResource->get());
    if (const auto* unknownResource =
            ca::core::tag_cast::dyn_cast<const model::UnknownResource>(&resource))
        return std::make_unique<model::UnknownResource>(locationText(*unknownResource),
                                                        unknownResource->get());
    return nullptr;
}

namespace {

/// @brief 源 MANIFEST 原始字节旁路捕获（#74）：包住真实 reader，读遍内
///        首个 META-INF/MANIFEST.MF 条目字节原样留存。
class SourceManifestCapture final : public mjbase::jar::IJarReader
{
public:
    SourceManifestCapture(std::unique_ptr<mjbase::jar::IJarReader> inner,
                          std::optional<std::vector<ca::u8>>& sink)
        : inner_(std::move(inner))
        , sink_(sink)
    {}
    ca::core::Result<void, mjbase::jar::JarError> read(
        const std::function<void(const ca::str::Utf8String&, const std::vector<ca::u8>&)>&
            consumer) override
    {
        return inner_->read(
            [&](const ca::str::Utf8String& name, const std::vector<ca::u8>& data) {
                capture(name, data);
                consumer(name, data);
            });
    }
    ca::core::Result<void, mjbase::jar::JarError> readWithProgress(
        const mjbase::jar::IJarReader::ProgressConsumer& consumer) override
    {
        return inner_->readWithProgress(
            [&](int index, int total, const ca::str::Utf8String& name,
                const std::vector<ca::u8>& data) {
                capture(name, data);
                consumer(index, total, name, data);
            });
    }

private:
    void capture(const ca::str::Utf8String& name, const std::vector<ca::u8>& data)
    {
        if (sink_.has_value()) return;
        if (name.ref() ==
            ca::str::Utf8StringRef::from_cstr(mjbase::jar::JarUtil::kManifestFileName))
            sink_ = data;
    }
    std::unique_ptr<mjbase::jar::IJarReader> inner_;
    std::optional<std::vector<ca::u8>>&      sink_;
};

/// @brief 导出目的地校验：空/覆写源/父目录缺失/已存在四类拒绝。
WorkspaceResult<std::filesystem::path> validateExportDestination(
    const std::string& sourcePath, const std::string& outputPath,
    const std::string& operation = "export JAR",
    const std::string& sourceDescription = "input JAR")
{
    namespace fs = std::filesystem;
    if (outputPath.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, operation, "output path is empty"));

    std::error_code filesystemError;
    const auto      output = fs::absolute(fsPath(outputPath), filesystemError);
    if (filesystemError)
        return ca::core::Err(error(WorkspaceError::InvalidArgument, operation, outputPath));
    const auto source = fs::absolute(fsPath(sourcePath), filesystemError);
    if (!filesystemError && source == output) {
        return ca::core::Err(error(WorkspaceError::PermissionDenied, operation,
                                   "output must not overwrite the " + sourceDescription));
    }
    const auto parent = output.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        return ca::core::Err(
            error(WorkspaceError::NotFound, operation, "output parent does not exist"));
    }
    if (fs::exists(output))
        return ca::core::Err(error(WorkspaceError::AlreadyExists, operation, pathToUtf8(output)));
    return ca::core::Ok(output);
}

/// @brief 导出条目载荷：条目名 = 容器 URI 原始字节，data = 资源字节。
struct FidelityJarEntry
{
    std::string         name;
    std::vector<ca::u8> data;
};

/// @brief 收集保真导出条目：容器资源原样展开，MANIFEST 用保真决策字节。
WorkspaceResult<std::vector<FidelityJarEntry>> collectFidelityJarEntries(
    model::ResourceContainer& container, const model::ResourceContainer* baseline,
    const std::optional<std::vector<ca::u8>>& sourceManifestBytes)
{
    auto manifestBytes = resolveExportManifestBytes(container, baseline, sourceManifestBytes);
    if (manifestBytes.is_err()) return ca::core::Err(std::move(manifestBytes).unwrap_err());
    auto resolvedManifest = std::move(manifestBytes).unwrap();

    std::vector<FidelityJarEntry> entries;
    WorkspaceErrorInfo            collectError;
    container.visitResources([&](std::string_view uri, const model::IResource& resource) {
        if (collectError.is_err()) return;
        if (ca::core::tag_cast::dyn_cast<const model::ManifestResource>(&resource) != nullptr) {
            // MANIFEST 条目：决策器对含 ManifestResource 的容器必返回字节。
            if (resolvedManifest.has_value())
                entries.push_back({std::string(uri), std::move(*resolvedManifest)});
            return;
        }
        auto bytes = bytesOf(resource);
        if (bytes.is_err()) {
            collectError = std::move(bytes).unwrap_err();
            return;
        }
        entries.push_back({std::string(uri), std::move(bytes).unwrap()});
    });
    if (collectError.is_err()) return ca::core::Err(std::move(collectError));
    std::sort(entries.begin(), entries.end(), [](const FidelityJarEntry& left,
                                                 const FidelityJarEntry& right) {
        return left.name < right.name;
    });
    return ca::core::Ok(std::move(entries));
}

/// @brief jar 导出行：按保真口径把容器物化为 jar（原子写：临时文件 + rename）。
///
/// 不再经 mjbase::model::ResourceHelper::exportToJar：其三处副作用与导出保真
/// 契约冲突——1) ensureMultiReleaseManifest 检测到 versions/ 条目就给 MANIFEST
/// 注入 Multi-Release:true；2) ManifestResource 经 Manifest::toBytes 重序列
/// 化，行尾恒 CRLF、主属性段外尾随内容被丢弃；3) JarWriter 写出条目名经路径
/// 规范化改写（折叠/拒写，mjbase#167）。本侧自装 zip：条目集 = 容器条目集
/// （源目录条目原样保留、不新增占位）；MANIFEST 字节经 resolveExportManifestBytes
/// 决策（无编辑逐字节复制源字节，有编辑物化编辑）；条目名按容器 URI 原始
/// 字节直写；压缩/头装配口径与 mjbase JarWriter 一致（DEFLATE + CRC32 +
/// UTF-8 flag 0x0800；空载荷含目录条目 STORED），时间戳取 DOS 纪元固定值
/// 保证可重现。
WorkspaceErrorInfo jarExport(const ExportSpec& spec, const std::string& outputPath)
{
    namespace fs = std::filesystem;
    auto output = validateExportDestination(spec.sourcePath, outputPath);
    if (output.is_err()) return std::move(output).unwrap_err();
    const auto destination = std::move(output).unwrap();

    auto collected = collectFidelityJarEntries(*spec.working, spec.original,
                                               spec.sourceManifestBytes != nullptr
                                                   ? *spec.sourceManifestBytes
                                                   : std::nullopt);
    if (collected.is_err()) return std::move(collected).unwrap_err();
    const auto entries = std::move(collected).unwrap();

    std::error_code filesystemError;
    auto            temporary = destination;
    temporary += ".mjt-tmp-" + std::to_string(spec.revision);
    if (fs::exists(temporary))
        return error(WorkspaceError::AlreadyExists, "export JAR", pathToUtf8(temporary));
    try {
        // DOS 纪元（1980-01-01 00:00:00）固定时间戳：产物字节可重现，JVM
        // 不读条目时间戳。
        constexpr ca::u16 kDosEpochTime = 0;
        constexpr ca::u16 kDosEpochDate = 0x0021;
        jzip::ZipArchive  archive;
        for (const auto& entry : entries) {
            auto local = std::make_unique<jzip::LocalFileHeader>();
            local->setFileName(entry.name);
            local->setGeneralPurposeBitFlag(0x0800);   // bit11 = UTF-8
            local->setLastModFileTime(kDosEpochTime);
            local->setLastModFileDate(kDosEpochDate);
            local->setOffset(0);
            if (entry.data.empty()) {
                // 零载荷条目（源目录条目等）STORED 原样保留。
                local->setCompressionMethod(0);
                local->setCompressedSize(0);
                local->setUncompressedSize(0);
                local->setCrc32(0);
            }
            else {
                ca::zip::Crc32 crc;
                crc.update(entry.data.data(), entry.data.size());
                auto compressed = jzip::DeflateCompressor().compress(entry.data);
                local->setCompressionMethod(8);
                local->setCompressedSize(static_cast<ca::u32>(compressed.size()));
                local->setUncompressedSize(static_cast<ca::i64>(entry.data.size()));
                local->setCrc32(crc.value());
                local->setFileData(std::move(compressed));
            }
            archive.addPart(std::move(local));
        }
        const auto archiveBytes = jzip::ZipIO::writeDirect(archive);
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out)
            return error(WorkspaceError::Internal, "export JAR",
                         "failed to open temporary archive: " + pathToUtf8(temporary));
        out.write(reinterpret_cast<const char*>(archiveBytes.data()),
                  static_cast<std::streamsize>(archiveBytes.size()));
        out.flush();
        if (!out) {
            out.close();
            fs::remove(temporary, filesystemError);
            return error(WorkspaceError::Internal, "export JAR",
                         "failed to write temporary archive");
        }
        out.close();
        if (!fs::exists(temporary) || fs::file_size(temporary) == 0) {
            fs::remove(temporary, filesystemError);
            return error(WorkspaceError::Internal, "export JAR",
                         "temporary archive was not written");
        }
        fs::rename(temporary, destination, filesystemError);
        if (filesystemError) {
            fs::remove(temporary, filesystemError);
            return error(WorkspaceError::Internal, "export JAR", "atomic rename failed");
        }
        return WorkspaceErrorInfo{};
    }
    catch (const std::exception& exception) {
        fs::remove(temporary, filesystemError);
        return error(WorkspaceError::Internal, "export JAR", exception.what());
    }
}

/// @brief jar 导入行：单次读取遍内旁路捕获源 MANIFEST + 原始/工作双快照。
WorkspaceResult<ImportedArtifact> jarImport(
    const std::string& path, ca::str::Utf8StringRef pathRef,
    const mjbase::jar::IJarReader::EntryNameDecoder& decoder)
{
    (void)pathRef;
    // 前置校验（旧 openJar 同款）：目录冒充 .jar 等形态异常定性为
    // InvalidArgument——交给 jar reader 会统一落 Io/UnsupportedFormat 兜底
    // 映成 DataLoss。
    if (path.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "open JAR", "path is empty"));
    namespace fs = std::filesystem;
    std::error_code filesystemError;
    const auto      status = fs::status(fsPath(path), filesystemError);
    if (filesystemError || !fs::exists(status))
        return ca::core::Err(error(WorkspaceError::NotFound, "open JAR", path));
    if (!fs::is_regular_file(status))
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "open JAR", "path is not a regular file"));
    try {
        model::ResourceContainer loaded;
        // 单次读取遍内旁路捕获源 MANIFEST 原始字节（#74 导出保真）。
        std::optional<std::vector<ca::u8>> sourceManifest;
        auto                               reader =
            std::make_unique<mjbase::jar::JvmJarReader>(utf8Ref(path));
        if (decoder) reader->setEntryNameDecoder(decoder);
        auto imported = model::ResourceHelper::importFromJar(
            loaded, path, std::make_unique<SourceManifestCapture>(std::move(reader),
                                                                  sourceManifest));
        if (imported.is_err())
            return ca::core::Err(modelFailure("open JAR", std::move(imported).unwrap_err()));
        auto original = cloneContainer(loaded);
        if (original.is_err()) return ca::core::Err(std::move(original).unwrap_err());
        auto working = cloneContainer(loaded);
        if (working.is_err()) return ca::core::Err(std::move(working).unwrap_err());

        ImportedArtifact artifact;
        artifact.original            = std::move(original).unwrap();
        artifact.working             = std::move(working).unwrap();
        artifact.sourceManifestBytes = std::move(sourceManifest);
        return ca::core::Ok(std::move(artifact));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::DataLoss, "open JAR", exception.what()));
    }
}

/// @brief classfile 导出行：单资源原子落盘（临时文件 + rename）。
WorkspaceErrorInfo classFileExport(const ExportSpec& spec, const std::string& outputPath)
{
    namespace fs = std::filesystem;
    auto output = validateExportDestination(spec.sourcePath, outputPath, "export class",
                                            "input class");
    if (output.is_err()) return std::move(output).unwrap_err();
    const auto destination = std::move(output).unwrap();

    std::error_code filesystemError;
    auto            temporary = destination;
    temporary += ".mjbase-class-tmp-" + std::to_string(spec.revision);
    if (fs::exists(temporary))
        return error(WorkspaceError::AlreadyExists, "export class", pathToUtf8(temporary));
    try {
        auto written = model::ResourceHelper::exportToClassFile(*spec.working,
                                                                pathToUtf8(temporary),
                                                                spec.primaryClassUri);
        if (written.is_err()) {
            fs::remove(temporary, filesystemError);
            return modelFailure("export class", std::move(written).unwrap_err());
        }
        if (!fs::exists(temporary) || fs::file_size(temporary) == 0) {
            fs::remove(temporary, filesystemError);
            return error(WorkspaceError::Internal, "export class",
                         "temporary classfile was not written");
        }
        fs::rename(temporary, destination, filesystemError);
        if (filesystemError) {
            fs::remove(temporary, filesystemError);
            return error(WorkspaceError::Internal, "export class", "atomic rename failed");
        }
        return WorkspaceErrorInfo{};
    }
    catch (const std::exception& exception) {
        fs::remove(temporary, filesystemError);
        return error(WorkspaceError::Internal, "export class", exception.what());
    }
}

/// @brief classfile 导入行：importFromClassFile + 内名规范 URI 重装。
///
/// 验证门不在此处（importer 是无状态算子）：gateUris 上抛，由会话的
/// validateCandidate 管线收口（与旧 openClass 门禁口径一致）。
WorkspaceResult<ImportedArtifact> classFileImport(
    const std::string& path, ca::str::Utf8StringRef pathRef,
    const mjbase::jar::IJarReader::EntryNameDecoder& decoder)
{
    (void)pathRef;
    (void)decoder;
    // 前置校验（旧 openClass 同款）：文件缺失/形态异常在此定性——交给
    // importFromClassFile 会把打开失败落成 ModelError::Io，兜底映射为
    // DataLoss，向调用方呈现"数据损坏"而非"文件不存在"。
    if (path.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "open class", "path is empty"));
    namespace fs = std::filesystem;
    std::error_code filesystemError;
    const auto      status = fs::status(fsPath(path), filesystemError);
    if (filesystemError || !fs::exists(status))
        return ca::core::Err(error(WorkspaceError::NotFound, "open class", path));
    if (!fs::is_regular_file(status))
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "open class", "path is not a regular file"));
    try {
        model::ResourceContainer imported;
        auto importedStatus = model::ResourceHelper::importFromClassFile(imported, path);
        if (importedStatus.is_err())
            return ca::core::Err(
                modelFailure("open class", std::move(importedStatus).unwrap_err()));
        const auto* importedClass =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(imported.getResource(path));
        if (importedClass == nullptr) {
            return ca::core::Err(
                error(WorkspaceError::DataLoss, "open class", "class resource was not imported"));
        }
        // 内名规范 URI（骨架解析；与容器内 class 条目同一 URI 语义）。
        const std::string internalName = internalNameOfBytes(importedClass->get());
        if (internalName.empty()) {
            return ca::core::Err(
                error(WorkspaceError::DataLoss, "open class", "class has an empty internal name"));
        }
        const auto               uri = internalName + ".class";
        model::ResourceContainer loaded;
        loaded.addResource(uri,
                           std::make_unique<model::ClassResource>(uri, importedClass->get()));
        auto original = cloneContainer(loaded);
        if (original.is_err()) return ca::core::Err(std::move(original).unwrap_err());
        auto working = cloneContainer(loaded);
        if (working.is_err()) return ca::core::Err(std::move(working).unwrap_err());

        ImportedArtifact artifact;
        artifact.original = std::move(original).unwrap();
        artifact.working  = std::move(working).unwrap();
        // class 工件无 jar 保真面。
        artifact.primaryClassUri = utf8(uri);
        artifact.gateUris        = {uri};
        return ca::core::Ok(std::move(artifact));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::DataLoss, "open class", exception.what()));
    }
}

}   // namespace

WorkspaceResult<std::unique_ptr<model::ResourceContainer>> cloneContainer(
    const model::ResourceContainer& source)
{
    auto               result = std::make_unique<model::ResourceContainer>();
    WorkspaceErrorInfo err;
    source.visitResources([&](std::string_view uri, const model::IResource& resource) {
        if (err.is_err()) return;
        auto copied = cloneResource(resource);
        if (!copied) {
            err = error(WorkspaceError::Unimplemented, "clone container",
                        "unsupported resource type at " + std::string(uri));
            return;
        }
        result->addResource(uri, std::move(copied));
    });
    if (err.is_err()) return ca::core::Err(std::move(err));
    return ca::core::Ok(std::move(result));
}

ContainerIoRegistry& ContainerIoRegistry::instance()
{
    static ContainerIoRegistry registry;
    return registry;
}

const ContainerIoEntry* ContainerIoRegistry::find(ContainerFormat format) const noexcept
{
    for (const auto& entry : entries_)
        if (entry.format == format) return &entry;
    return nullptr;
}

const ContainerIoEntry* ContainerIoRegistry::findByExtension(
    const std::string& path) const noexcept
{
    const auto dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 == path.size()) return nullptr;
    std::string extension = path.substr(dot + 1);
    for (char& character : extension)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    for (const auto& entry : entries_)
        if (extension == entry.extension) return &entry;
    return nullptr;
}

void ContainerIoRegistry::registerEntry(ContainerIoEntry entry)
{
    for (auto& existing : entries_) {
        if (existing.format == entry.format || std::string_view(existing.extension) ==
                                                   std::string_view(entry.extension)) {
            existing = std::move(entry);
            return;
        }
    }
    entries_.push_back(std::move(entry));
}

ContainerIoRegistry::ContainerIoRegistry()
{
    entries_.push_back({ContainerFormat::Jar, "jar", {true}, &jarImport, &jarExport});
    entries_.push_back({ContainerFormat::ClassFile, "class", {false}, &classFileImport,
                       &classFileExport});
}

ContainerFormat formatForPath(const std::string& path) noexcept
{
    const auto* entry = ContainerIoRegistry::instance().findByExtension(path);
    return entry != nullptr ? entry->format : ContainerFormat::None;
}

}   // namespace mjt::workspace
