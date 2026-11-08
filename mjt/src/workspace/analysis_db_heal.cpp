/// @file        analysis_db_heal.cpp
/// @brief       open 期身份迁移自愈实现（设计面见 analysis_db_heal.hpp）。
#include "analysis_db_heal.hpp"

#include <map>
#include <string>
#include <utility>

#include "libca/str/utf8_string.hpp"

#include "mjbase/project/edit_ledger.hpp"
#include "workspace.hpp"
#include "workspace_types.hpp"

namespace mjt::workspace {
namespace {

///std::string → Utf8String（与 analysis_ingest 的 utf8() 同口径）。
ca::str::Utf8String utf8(const std::string& value)
{
    return ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

///LedgerIdentityRename → DbIdentityRename（与 workspace 的编辑期迁移同口径：
///类/成员目标映射与字段克隆一字不差，重放面与编辑面行为对齐）。
mjbase::project::DbIdentityRename toDbRename(const project::LedgerIdentityRename& spec)
{
    mjbase::project::DbIdentityRename rename;
    rename.target = spec.isMember ? mjbase::project::RenameTargetKind::Member
                                  : mjbase::project::RenameTargetKind::Class;
    rename.memberKind = spec.isMethod ? mjbase::project::DbMemberKind::Method
                                      : mjbase::project::DbMemberKind::Field;
    rename.owner      = spec.owner.clone();
    rename.name       = spec.name.clone();
    rename.desc       = spec.desc.clone();
    rename.newName    = spec.newName.clone();
    rename.newDesc    = spec.newDesc.clone();
    return rename;
}

///单批迁移结果。
enum class BatchOutcome
{
    Applied,   ///< 已执行（含全部 miss=skip 的幂等空转）
    Collision, ///< 撞名/批内重复（Invalid）——库态与批次描述态交错
    Failed,    ///< 库面自身故障（错误通道已填）
};

///一批迁移并累计统计。Applied 时 migrated* 计入 stats；Collision 语义随
///调用方阶段而定（阶段一兜底计数、阶段二/三 fail-closed）。
BatchOutcome migrateBatch(mjbase::project::AnalysisDb&                 db,
                          std::vector<mjbase::project::DbIdentityRename> batch,
                          AnalysisHealStats&                            stats,
                          AnalysisHealError&                            error)
{
    auto migrated = db.migrateIdentities(std::move(batch));
    if (migrated.is_err()) {
        auto failure = std::move(migrated).unwrap_err();
        if (failure.kind == mjbase::project::AnalysisDbErrorKind::Invalid)
            return BatchOutcome::Collision;
        error.kind    = AnalysisHealErrorKind::Database;
        error.message = "migrate identities: " + failure.message.to_std_string();
        return BatchOutcome::Failed;
    }
    const auto result = std::move(migrated).unwrap();
    stats.migratedClasses += result.migratedClasses;
    stats.migratedMembers += result.migratedMembers;
    stats.skipped         += result.skipped;
    return BatchOutcome::Applied;
}

///fail-closed 包装：阶段二/三的撞名是副本/库真名冲突，宁拒开不裂 ID。
bool requireApplied(BatchOutcome outcome, const char* stage, AnalysisHealError& error)
{
    if (outcome == BatchOutcome::Applied) return true;
    if (outcome == BatchOutcome::Failed) return false;
    error.kind    = AnalysisHealErrorKind::Database;
    error.message = std::string(stage) + ": identity collision (working copy and database "
                                     "disagree on live names; refusing to split ids)";
    return false;
}

}   // namespace

ca::Result<AnalysisHealStats, AnalysisHealError> healAnalysisDbIdentities(
    const WorkspaceBase& workspace, mjbase::project::AnalysisDb& db)
{
    AnalysisHealStats stats;
    AnalysisHealError error;

    //阶段一：head 链注记按链序逐批补账。批序即历史序（链式改名 A→B→C
    //的中间态依赖批间先后）；miss=skip 使已迁移库重放零写入。撞名批兜底
    //计数后继续——阶段二/三的方向无关对账接管。
    for (auto& batch : workspace.headIdentitySpecBatches()) {
        ++stats.annotationBatches;
        std::vector<mjbase::project::DbIdentityRename> renames;
        renames.reserve(batch.size());
        for (const auto& spec : batch) renames.push_back(toDbRename(spec));
        const auto outcome = migrateBatch(db, std::move(renames), stats, error);
        if (outcome == BatchOutcome::Failed) return ca::core::Err(std::move(error));
        if (outcome == BatchOutcome::Collision) ++stats.collisionBatches;
    }

    //阶段二：类名路径对账。副本侧 uri → 现行名；库侧 entry_id → 条目 path
    //锚定同类行，行名 ≠ 副本名即补 (库行名 → 副本名)。方向无关——undo 方向
    //的滞后差与注记被 compact 进基线的残差同式覆盖。
    {
        auto summaries = workspace.listClassSummaries();
        if (summaries.is_err())
            return ca::core::Err(AnalysisHealError{
                AnalysisHealErrorKind::Workspace,
                "list class summaries: " + std::move(summaries).unwrap_err().message});
        std::map<std::string, std::string> byUri;   ///< resourceUri → 副本现行名
        for (const auto& summary : std::move(summaries).unwrap())
            byUri[summary.resourceUri.to_std_string()] = summary.internalName.to_std_string();

        auto inputs = db.listInputs();
        if (inputs.is_err()) {
            error.kind    = AnalysisHealErrorKind::Database;
            error.message = "list inputs: " +
                            std::move(inputs).unwrap_err().message.to_std_string();
            return ca::core::Err(std::move(error));
        }
        std::map<ca::i64, std::string> entryPath;   ///< entry_id → 条目 path
        for (const auto& input : std::move(inputs).unwrap()) {
            auto entries = db.listEntries(input.inputId);
            if (entries.is_err()) {
                error.kind    = AnalysisHealErrorKind::Database;
                error.message = "list entries: " +
                                std::move(entries).unwrap_err().message.to_std_string();
                return ca::core::Err(std::move(error));
            }
            for (const auto& entry : std::move(entries).unwrap())
                entryPath[entry.entryId] = entry.path.to_std_string();
        }

        auto classes = db.listClasses();
        if (classes.is_err()) {
            error.kind    = AnalysisHealErrorKind::Database;
            error.message =
                "list classes: " + std::move(classes).unwrap_err().message.to_std_string();
            return ca::core::Err(std::move(error));
        }
        std::vector<mjbase::project::DbIdentityRename> reconcile;
        for (const auto& row : std::move(classes).unwrap()) {
            if (row.entryId == 0) continue;   //未关联条目：无路径锚，交阶段三
            const auto path = entryPath.find(row.entryId);
            if (path == entryPath.end()) continue;
            const auto live = byUri.find(path->second);
            if (live == byUri.end()) continue;   //副本侧类已不存在：导入期墓碑面
            const auto rowName = row.internalName.to_std_string();
            if (rowName == live->second) continue;
            mjbase::project::DbIdentityRename spec;
            spec.target  = mjbase::project::RenameTargetKind::Class;
            spec.name    = row.internalName.clone();
            spec.newName = utf8(live->second);
            reconcile.push_back(std::move(spec));
        }
        if (!reconcile.empty()) {
            const auto before = stats.migratedClasses;
            if (!requireApplied(migrateBatch(db, std::move(reconcile), stats, error),
                                "class path reconcile", error))
                return ca::core::Err(std::move(error));
            stats.reconciledClasses = stats.migratedClasses - before;
        }
    }

    //阶段三：成员覆盖层对账。覆盖层行 ID 键控、迁移零触碰，(owner, desc,
    // kind) 锚定副本成员：现行名 ≠ 副本名且锚定唯一 → 补迁移；owner 不在
    // 副本或同描述符多候选 → 歧义跳过（统计披露）。
    {
        auto renames = db.listRenames();
        if (renames.is_err()) {
            error.kind    = AnalysisHealErrorKind::Database;
            error.message =
                "list renames: " + std::move(renames).unwrap_err().message.to_std_string();
            return ca::core::Err(std::move(error));
        }
        std::vector<mjbase::project::DbIdentityRename> reconcile;
        for (const auto& record : std::move(renames).unwrap()) {
            if (record.target != mjbase::project::RenameTargetKind::Member) continue;
            auto info = workspace.classInfo(record.owner.ref());
            if (info.is_err()) {
                //owner 不在副本：覆盖层残行（类已删/更名未愈的组合态），
                //导入期整库对账面处理，此处只计披露。
                ++stats.ambiguousSkipped;
                continue;
            }
            const auto cls  = std::move(info).unwrap();
            const auto desc = record.desc.to_std_string();
            const auto name = record.name.to_std_string();
            //候选 = 同描述符成员；名字命中即一致；唯一候选且名不命中即补账。
            ca::usize   candidates = 0;
            bool        nameHit    = false;
            std::string anchored;
            const auto  consider = [&](const ca::str::Utf8String& memberName,
                                       const ca::str::Utf8String& memberDesc) {
                if (memberDesc.to_std_string() != desc) return;
                ++candidates;
                if (memberName.to_std_string() == name) nameHit = true;
                else anchored = memberName.to_std_string();
            };
            if (record.memberKind == mjbase::project::DbMemberKind::Method)
                for (const auto& method : cls.methods)
                    consider(method.name, method.descriptor);
            else
                for (const auto& field : cls.fields) consider(field.name, field.descriptor);
            if (nameHit || candidates == 0) continue;
            if (candidates > 1) {
                ++stats.ambiguousSkipped;
                continue;
            }
            mjbase::project::DbIdentityRename spec;
            spec.target     = mjbase::project::RenameTargetKind::Member;
            spec.memberKind = record.memberKind;
            spec.owner      = record.owner.clone();
            spec.name       = record.name.clone();
            spec.desc       = record.desc.clone();
            spec.newName    = utf8(anchored);
            reconcile.push_back(std::move(spec));
        }
        if (!reconcile.empty()) {
            const auto before = stats.migratedMembers;
            if (!requireApplied(migrateBatch(db, std::move(reconcile), stats, error),
                                "member overlay reconcile", error))
                return ca::core::Err(std::move(error));
            stats.reconciledMembers = stats.migratedMembers - before;
        }
    }

    return ca::core::Ok(stats);
}

}   // namespace mjt::workspace
