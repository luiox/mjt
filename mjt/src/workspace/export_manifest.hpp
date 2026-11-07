#pragma once

#include <optional>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

#include "workspace_types.hpp"

namespace mjt::workspace {

/// @brief 解析导出 jar 的 META-INF/MANIFEST.MF 字节（#74 导出保真口径）。
///
/// 决策序：
/// 1. 工作副本含 ManifestResource 且属性模型与 @p baseline 逐项一致（无
///    MANIFEST 编辑）→ 返回 @p sourceManifestBytes（源条目原始字节逐字节
///    复制：行尾、续行、主属性段外尾随内容原样保留）。@p sourceManifestBytes
///    缺失（防御面，正常开 jar 路径必捕获）时回退模型序列化。
/// 2. 工作副本含 ManifestResource 但模型有编辑 → 返回工作副本模型序列化
///    （插入序保序、无属性注入；不经 Multi-Release 推断补写）。
/// 3. 工作副本无 ManifestResource：维持既有收口契约——存在 JEP 238 版本化
///    class 条目（META-INF/versions/<N>，N≥9）时补最小 MANIFEST（否则 JVM
///    9+ 忽略整棵 versions 树），字节同 mjbase ensureMultiReleaseManifest 的
///    无 Manifest 分支；无版本化条目 → nullopt（产物不带 MANIFEST 条目，
///    与源一致）。
WorkspaceResult<std::optional<std::vector<ca::u8>>> resolveExportManifestBytes(
    const mjbase::model::ResourceContainer& container,
    const mjbase::model::ResourceContainer* baseline,
    const std::optional<std::vector<ca::u8>>& sourceManifestBytes);

}   // namespace mjt::workspace
