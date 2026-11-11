#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"

#include "workspace_types.hpp"

namespace mjt::workspace {

/// @file jar_ops.hpp
/// @brief jar 专属面的自由函数服务（W2 下沉物）。
///
/// manifest 读取与 jar 修复扫描是无状态算子：输入容器（与修复策略），
/// 输出 DTO——主会话只认 IResourceContainer 与 URI。修复事务本身走
/// EditCommand::JarRepair（edit_pipeline.cpp 单入口）。

/// @brief URI 是否 class 扩展名（大小写不敏感）。
bool hasClassExtension(const std::string& uri);

/// @brief 读容器主属性段（键序稳定）；无 MANIFEST 条目报 NotFound。
WorkspaceResult<std::vector<ManifestAttribute>> manifestAttributes(
    const mjbase::model::ResourceContainer& container);

/// @brief 扫描可修复问题（invalid_class / invalid_class_entry /
///        corrupt_output_marker；selectedForRemoval 按策略旗标预置）。
///
/// @param container 待扫容器（工作副本或候选）。
/// @param request   修复策略（两删除旗标）。
/// @param classCount 出参：class 形态条目计数。
std::vector<JarRepairIssue> scanJarRepairIssues(const mjbase::model::ResourceContainer& container,
                                                const JarRepairRequest& request,
                                                ca::usize&              classCount);

/// @brief 是否存在按策略选中的待删条目。
bool hasSelectedRepairIssues(const std::vector<JarRepairIssue>& issues);

/// @brief 从候选容器删除选中条目（键视图自持：先拷贝待删键再删）。
void removeSelectedRepairIssues(mjbase::model::ResourceContainer&       candidate,
                                const std::vector<JarRepairIssue>& issues);

}   // namespace mjt::workspace
