/// @file        analysis_edit.hpp
/// @brief       编辑流 → 分析数据库的方法指纹版本桥（#132「怎么接进数据库」）。
///
/// 职责边界：workspace 负责编辑提交（commit 锚点持有编辑后容器字节），
/// mjbase::project::AnalysisDb 负责方法版本事实（method_versions，schema v8）。
/// 本桥把两者接起来：对每个受影响 class，从编辑后容器的最新字节重解析 DOM
/// （jbc::core::readClass，与 summary_scan/reference_scan 同一重解析面），逐
/// 方法调 mjbase::analysis::semanticFp 算新指纹，按库内稳定 id（findClass /
/// listMembers 解析 class_id/member_id）落 recordMethodVersion（单方法面）或
/// recordMethodVersions（整类批量面）。
///
/// 覆盖面契约（提交锚点统一口径，见 workspace.cpp 的
/// recordMethodVersionsForEdit）：
/// - 整类替换/重汇编（applyJasm/applyClassBytes/replaceClassBytes/pipeline/
///   remap/rename/revert/restore）→ 每个受影响类全方法重算，批量面落库；
///   语义未动的成员走 mjbase 幂等跳过（unchanged），不产生伪版本行。
/// - 单方法寻址的结构化 mutation（applyClassMutation 的换体/指令级 kinds）→
///   聚焦成员单方法面落库；类级与字段级 kind 不聚焦（批量面兜底，全部
///   unchanged）。
/// - 库内没有成员身份（类/方法未登记，如新增方法、未跑 ingest 的库）→ 跳过
///   计数，不落行：编辑流只对已登记成员记账，身份登记仍归导入桥
///   （analysis_ingest）所有。
#pragma once

#include <string>
#include <vector>

#include "libca/str/utf8_string.hpp"

#include "mjbase/model/resource_container.hpp"
#include "workspace_types.hpp"

namespace mjbase::project {
class AnalysisDb;   ///< 分析数据库连接（analysis_db.hpp；此处仅前置声明）
}   // namespace mjbase::project

namespace mjt::workspace {

/// @brief 编辑提交后按受影响类落方法指纹版本。
///
/// @p db 需以读写方式打开；@p container 是编辑提交后的最新容器；@p classUris
/// 是本次字节变化的 class 资源 URI（非 class 资源跳过）；@p focus 非空且命中
/// 某个受影响类时走单方法面（只记聚焦成员），否则每个受影响类走批量面。
/// db 写失败置 failed 并即停（fail-fast，部分落库如实披露在计数里）。
MethodVersionRecordStats recordMethodVersionsAfterEdit(
    mjbase::project::AnalysisDb&             db,
    const model::ResourceContainer&          container,
    const std::vector<ca::str::Utf8String>&  classUris,
    const EditedMethodFocus*                 focus = nullptr);

}   // namespace mjt::workspace
