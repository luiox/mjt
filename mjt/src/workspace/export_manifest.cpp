/// @file        export_manifest.cpp
/// @brief       导出 MANIFEST 保真决策（#74：无编辑逐字节复制源字节，有编辑
///              只物化编辑，不做 Multi-Release 推断注入）。
#include "export_manifest.hpp"

#include "libca/core/tag_cast.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/jar/manifest.hpp"
#include "mjbase/model/resource_model.hpp"

namespace mjt::workspace {
namespace {

using model::IResource;
using model::ManifestResource;
using model::ResourceContainer;
using model::ResourceType;

/// @brief 取容器内首个 ManifestResource（容器按 URI 键控，MANIFEST 至多一个）。
const ManifestResource* findManifest(const ResourceContainer& container)
{
    const ManifestResource* manifest = nullptr;
    container.visitResources([&](std::string_view, const IResource& resource) {
        if (manifest == nullptr)
            manifest = ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource);
    });
    return manifest;
}

/// @brief 容器内是否存在 JEP 238 版本化 class 条目（版本段 ≥ 9）。
bool hasVersionedClassEntries(const ResourceContainer& container)
{
    bool found = false;
    container.visitResources([&](std::string_view uri, const IResource& resource) {
        if (resource.getType() == ResourceType::Class &&
            mjbase::model::multiReleaseVersionOfClassUri(uri) >= 9)
            found = true;
    });
    return found;
}

/// @brief 无 Manifest 资源时的最小 MANIFEST 字节（现状收口契约，与 mjbase
///        ensureMultiReleaseManifest 的无 Manifest 分支同字节：CRLF 行尾）。
std::vector<ca::u8> minimalMultiReleaseManifestBytes()
{
    mjbase::jar::Manifest manifest;
    manifest.setMainAttribute(ca::str::Utf8StringRef::from_cstr("Manifest-Version"),
                              ca::str::Utf8StringRef::from_cstr("1.0"));
    manifest.setMainAttribute(ca::str::Utf8StringRef::from_cstr("Multi-Release"),
                              ca::str::Utf8StringRef::from_cstr("true"));
    return manifest.toBytes();
}

}   // namespace

WorkspaceResult<std::optional<std::vector<ca::u8>>> resolveExportManifestBytes(
    const ResourceContainer& container, const ResourceContainer* baseline,
    const std::optional<std::vector<ca::u8>>& sourceManifestBytes)
{
    const ManifestResource* manifest = findManifest(container);
    if (manifest == nullptr) {
        // 源/工作副本都没有 MANIFEST 条目：版本化类在场时按现状契约补最小
        // MANIFEST（缺属性 JVM 9+ 忽略整棵 versions 树），否则产物不携带
        // MANIFEST——与源条目集一致。
        if (hasVersionedClassEntries(container))
            return ca::core::Ok(std::optional<std::vector<ca::u8>>(
                minimalMultiReleaseManifestBytes()));
        return ca::core::Ok(std::optional<std::vector<ca::u8>>{});
    }

    // 无 MANIFEST 编辑（工作副本属性模型与打开时基线逐项一致）→ 源条目
    // 原始字节逐字节复制：行尾（源可为 LF）、续行折行、主属性段外尾随内容
    // 全部原样。sourceManifestBytes 缺失仅在非 jar 捕获面出现（防御回退：
    // 模型序列化，不做任何属性注入）。
    if (baseline != nullptr) {
        const ManifestResource* baseManifest = findManifest(*baseline);
        if (baseManifest != nullptr && baseManifest->get().getMainAttributes() ==
                                               manifest->get().getMainAttributes()) {
            if (sourceManifestBytes.has_value()) return ca::core::Ok(sourceManifestBytes);
            return ca::core::Ok(
                std::optional<std::vector<ca::u8>>(manifest->get().toBytes()));
        }
    }

    // 有 MANIFEST 编辑（属性集/序有差异，或基线无 MANIFEST 而工作副本新增）：
    // 物化工作副本模型——插入序保序（setMainAttribute 既有键原位覆写）、
    // 不注入不重排；序列化按 JAR 规范 CRLF 行尾。
    return ca::core::Ok(std::optional<std::vector<ca::u8>>(manifest->get().toBytes()));
}

}   // namespace mjt::workspace
