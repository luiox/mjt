#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/model/resource_container.hpp"

#include "workspace_types.hpp"

namespace mjt::workspace {

/// @file container_io.hpp
/// @brief 主制品的格式中立导入导出：importer/exporter 注册表。
///
/// ArtifactKind/jar 命名面（openJar/openClass/exportJar/exportClass）的退役
/// 替身：主会话（WorkspaceBase）只认 IResourceContainer 与 URI——打开按
/// 路径扩展名路由到注册行，导出按当前格式路由；新格式（dex/apk…）经
/// registerEntry 插入，WorkspaceBase 头签名零变化（核心不变量）。
///
/// jar 导出行承载 #74 导出保真口径（条目集一致 / 未编辑 MANIFEST 逐字节
/// 复制 / DOS 纪元时间戳可重现），不经 mjbase ResourceHelper::exportToJar。

/// @brief 主制品格式（ArtifactKind 退役后的格式中立替身）。
enum class ContainerFormat : ca::u8
{
    None = 0,
    Jar,
    ClassFile,
};

/// @brief 格式能力声明（主会话按位查询，不再 switch 枚举猜 jar 假设）。
struct ContainerCapabilities final
{
    bool hasManifest = false;
};

/// @brief importer 产物：不可变原始快照 + 可修改工作副本 + 保真基线素材。
struct ImportedArtifact final
{
    std::unique_ptr<model::ResourceContainer> original;
    std::unique_ptr<model::ResourceContainer> working;
    /// 源 jar 的 META-INF/MANIFEST.MF 原始字节（#74 旁路捕获）；nullopt =
    /// 源无该条目或格式无 manifest 面。
    std::optional<std::vector<ca::u8>> sourceManifestBytes;
    /// 单 class 工作区的稳定资源 URI；容器型格式为空。
    ca::str::Utf8String primaryClassUri;
    /// 打开期验证门作用域（classfile 格式 = 本类；容器型格式为空——沿旧
    /// openJar/openClass 门禁口径）。
    std::vector<std::string> gateUris;
};

/// @brief exporter 所需的工作区事实（自由函数只认容器与 URI）。
struct ExportSpec final
{
    model::ResourceContainer*                working = nullptr;
    const model::ResourceContainer*          original = nullptr;
    const std::optional<std::vector<ca::u8>>* sourceManifestBytes = nullptr;
    std::string                              sourcePath;
    std::string                              primaryClassUri;
    ca::u64                                  revision = 0;
};

/// @brief importer 函数指针：路径 → 原始快照 + 工作副本。
///
/// decoder 为 jar 族的条目名宽容解码回调（#1110；其余格式忽略；空 = 原始
/// 字节直通零漂移）。
using ContainerImportFn = WorkspaceResult<ImportedArtifact> (*)(
    const std::string& path, ca::str::Utf8StringRef pathRef,
    const mjbase::jar::IJarReader::EntryNameDecoder& decoder);

/// @brief exporter 函数指针：工作区事实 → 原子落盘目标路径。
using ContainerExportFn = WorkspaceErrorInfo (*)(const ExportSpec& spec,
                                                 const std::string& outputPath);

/// @brief 注册表行：格式 + 扩展名路由键 + 能力位 + 导入/导出实现。
struct ContainerIoEntry final
{
    ContainerFormat        format;
    const char*            extension;
    ContainerCapabilities  capabilities;
    ContainerImportFn      import = nullptr;
    ContainerExportFn      exportFn = nullptr;
};

/// @brief 进程级注册表（默认行 = jar + classfile；追加可覆盖同键行）。
///
/// Meyers 单例初始化线程安全；registerEntry 本身不加锁——扩展注册发生在
/// 进程启动 / 测试装配期（单线程面），运行中格式集不可变。
class ContainerIoRegistry final
{
public:
    static ContainerIoRegistry& instance();

    /// @brief 按格式查行；未注册返回 nullptr。
    const ContainerIoEntry* find(ContainerFormat format) const noexcept;
    /// @brief 按路径扩展名查行（小写比较）；未注册返回 nullptr。
    const ContainerIoEntry* findByExtension(const std::string& path) const noexcept;
    /// @brief 追加注册行（同格式/同扩展名覆盖旧行——stub 注入面）。
    void registerEntry(ContainerIoEntry entry);

private:
    ContainerIoRegistry();

    std::vector<ContainerIoEntry> entries_;
};

/// @brief 按路径扩展名解析格式（未注册扩展 → None）。
ContainerFormat formatForPath(const std::string& path) noexcept;

/// @brief UTF-8 窄串 → filesystem path（Windows 上 path(std::string) 按 ACP
///        解码，非 ASCII 必乱；与 model/resource_io.cpp 的 u8path 口径一致）。
std::filesystem::path fsPath(const std::string& text);

/// @brief filesystem path → UTF-8 窄串（path::string() 在 Windows 走 ACP，
///        非 ASCII 会抛 code-page 异常；API 边界契约是 UTF-8）。
std::string pathToUtf8(const std::filesystem::path& path);

/// @brief 单资源深拷贝（class/manifest/unknown；其余类型返回空）。
std::unique_ptr<model::IResource> cloneResource(const model::IResource& resource);

/// @brief 容器深拷贝（逐资源 clone；不支持类型报 Unimplemented）。
WorkspaceResult<std::unique_ptr<model::ResourceContainer>> cloneContainer(
    const model::ResourceContainer& source);

/// @brief model 模块错误 → workspace 错误值（缺失 → NotFound、zip-slip →
///        InvalidArgument、其余 DataLoss 兜底）。
WorkspaceErrorInfo modelFailure(const std::string& operation, model::ModelError modelError);

}   // namespace mjt::workspace
