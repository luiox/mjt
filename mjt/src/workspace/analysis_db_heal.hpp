/// @file        analysis_db_heal.hpp
/// @brief       open 期身份迁移自愈——库行名对齐台账重放后的工作副本。
///
/// 崩溃窗背景：真改名的落账序是「台账事务 → AnalysisDb::migrateIdentities」，
/// 迁移失败不回滚编辑（与 recordMethodVersions 同一崩溃窗纪律，随
/// MutationResult::identity 披露）。若进程恰崩在两者之间，重开时台账已把
/// 改名重放进工作副本，而库行名滞后——紧接着的整库导入按名 upsert 会给
/// 新名裂出一行新 ID，击穿「ID 与句柄稳定」的落库承诺。本模块在导入前
/// 把库行名对齐工作副本，窗口关闭：
/// - 阶段一（注记补账）：head 链各事务的 identity 注记按链序逐批重放
///   migrateIdentities。幂等由 miss=skip 语义保证（已迁移库重放 = 全跳过零
///   写入）；撞名批不失败——交给阶段二的方向无关对账兜底。
/// - 阶段二（类名路径对账）：类行经 entry_id → 条目 path 锚定工作副本同类，
///   行名 ≠ 副本名即补一条 (库行名 → 副本名) 迁移。方向无关：既覆盖 undo
///   方向（反迁移失败的滞后差），也覆盖注记被 compact 进基线的场景。撞名
///   = 副本内同名冲突，真数据冲突 fail-closed。
/// - 阶段三（成员覆盖层对账）：renames 覆盖层成员行按 (owner, desc, kind)
///   锚定副本成员（覆盖层 ID 键控、迁移零触碰，行恒指向现役成员）；现行名
///   ≠ 副本名且锚定唯一时补迁移，同描述符多候选计歧义跳过。
///
/// 已知留档：真名成员改名（无覆盖层行）在 undo 方向且注记已被 compact 的
/// 组合窗本模块不覆盖——阶段三只认覆盖层行；该窗口需要 compact 前置门或
/// 全量成员对账，成本不成比例，暂以统计披露。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/project/analysis_db.hpp"

namespace mjt::workspace {

class WorkspaceBase;

/// @brief 自愈失败来源。
enum class AnalysisHealErrorKind : ca::u8
{
    Workspace, ///< 工作区读取（类清单/类信息）失败
    Database,  ///< 分析数据库读/迁移失败
};

/// @brief 自愈诊断（message 为 std::string：Result 错误通道要求可拷贝）。
struct AnalysisHealError
{
    AnalysisHealErrorKind kind = AnalysisHealErrorKind::Database;
    std::string           message;
};

/// @brief 一次自愈的规模统计（全零 = 库与台账本就一致，常态）。
struct AnalysisHealStats
{
    /// @brief head 链上携带身份注记的事务批数（阶段一输入规模）。
    ca::usize annotationBatches = 0;
    /// @brief 阶段一实际改写的类/成员行数（含后续阶段累计）。
    ca::usize migratedClasses = 0;
    ca::usize migratedMembers = 0;
    /// @brief migrateIdentities 未命中跳过的规格数（幂等重放的常态主项）。
    ca::usize skipped = 0;
    /// @brief 注记重放撞名批数（不失败，转阶段二/三兜底）。
    ca::usize collisionBatches = 0;
    /// @brief 阶段二路径对账修正的类行数。
    ca::usize reconciledClasses = 0;
    /// @brief 阶段三覆盖层对账修正的成员行数。
    ca::usize reconciledMembers = 0;
    /// @brief 阶段三同 (desc, kind) 多候选、无法锚定的成员行数。
    ca::usize ambiguousSkipped = 0;
};

/// @brief 把库行名对齐台账重放后的工作副本（须在 ingestIntoAnalysisDb 前）。
///
/// 要求 @p db 已以读写方式打开、@p workspace 已完成 bindLedgerStore 重放。
/// 阶段二撞名返回错误（fail-closed：打开侧据此拒开，宁停不裂）；其余失败
/// 仅在库读写面自身故障时出现。
ca::Result<AnalysisHealStats, AnalysisHealError> healAnalysisDbIdentities(
    const WorkspaceBase& workspace, mjbase::project::AnalysisDb& db);

}   // namespace mjt::workspace
