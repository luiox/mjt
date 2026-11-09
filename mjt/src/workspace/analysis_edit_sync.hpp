/// @file        analysis_edit_sync.hpp
/// @brief       编辑流 → 分析数据库的类事实增量同步（查询面 DB-first 配套）。
///
/// 职责边界：workspace 负责编辑提交（commit/undo/redo 锚点持有编辑后容器
/// 字节），mjbase::project::AnalysisDb 负责类事实持久化。本桥在台账事务与
/// 身份迁移落定之后，把受影响类的库事实追平工作副本，使编辑会话内的
/// searchClassesPaged/census 可见域与引擎扫描一致（DB-first 查询面解除
/// 干净会话守卫的前提）：
/// - 条目 sha 重锚：容器活跃清单与库内锚合并——未变更条目沿用库内
///   (path, sha) 锚（不重读字节），变更条目重算 sha 换新锚（旧行墓碑），
///   容器中消失的条目墓碑（整清单口径与导入桥一致，增量只省读字节）。
/// - 类事实 upsert：受影响类按 workspace.classInfo(uri) 现算事实，携带
///   新 entry_id 换锚（classes.entry_id 不换锚则类退出可见域）。
/// - 引用切片：受影响类逐类重扫（ReferenceScan::buildFor 过滤构建），
///   类头走 replaceReferencesFromClass、逐方法走 replaceReferencesFromMember，
///   编辑后消失的方法清空切片（成员身份行按库纪律只增不删，仅清引用）。
/// - 常量事实切片（mjbase schema v12，查询面 strings/numbers DB-first 配套）：
///   逐类抽取（constant_scan 与引擎搜索/整库导入同一事实源）攒
///   DbClassConstantFaces 组，单次 replaceConstantFaceSlices 合批落库
///   （mjbase 0.0.30）——类级两切片与逐方法数值切片一次事务写完，编辑提交
///   的常量面事务数从逐切片 N 次收口为 1 次（windows 每事务 fsync 的可感知
///   延迟）；消失方法同款清空切片纪律（空切片入批）。
///
/// 写入口径：与方法指纹记账（analysis_edit）同一纪律——batchId=0 裸事务、
/// 不开批次（编辑节奏的溯源归台账；归属锚仍是 open 期整库导入批）；库写
/// 失败不回滚编辑（编辑已随台账落定，open 期整库导入是兜底真源），failed
/// 计数披露。跳过面：未绑定库、库内无 sourcePath 输入行（未导入过）、非
/// class 条目或 classInfo 解析失败——如实计数，不报错。
#pragma once

#include <vector>

#include "libca/str/utf8_string.hpp"

#include "mjbase/model/resource_container.hpp"
#include "workspace_types.hpp"

namespace mjbase::project {
class AnalysisDb;   ///< 分析数据库连接（analysis_db.hpp；此处仅前置声明）
}   // namespace mjbase::project

namespace mjt::workspace {

class WorkspaceBase;

/// @brief 编辑提交后把受影响类的分析库事实追平工作副本。
///
/// @p workspace/@p container 为编辑提交后的最新状态（container 即工作副本
/// 容器）；@p changedUris 是本次字节差异的资源 URI（重复折叠，非 class 条目
/// 只参与条目重锚）。db 写失败即停并置 failed（部分写入如实披露在计数里）。
AnalysisEditSyncStats syncAnalysisDbAfterEdit(
    const WorkspaceBase&                   workspace,
    mjbase::project::AnalysisDb&           db,
    const model::ResourceContainer&        container,
    const std::vector<ca::str::Utf8String>& changedUris);

}   // namespace mjt::workspace
