#include "service.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "diff.hpp"
#include "json.hpp"
#include "parser.hpp"
#include "shared/jar_remapper.hpp"
#include "shared/shadow_display.hpp"
#include "shared/status_convert.hpp"
#include "tool/shadow_slot.hpp"

#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/edit_ledger.hpp"

namespace mjt::mcp {

using namespace detail;
using mjt::workspace::ClassMutation;
using mjt::workspace::PipelineRequest;
using mjt::workspace::RenameRequest;
using mjt::workspace::IWorkspace;

std::optional<Json> McpToolService::callMutationTool(
    const std::string& name, const Json& arguments, ProjectSession& session)
{
    IWorkspace* current = session.workspace.get();
    //container_mount（RFC §8.1 #4 + P5 角色化容器栈）：support 遗留角色走
    //workspace 在册表（xref 跨容器参与面不变）；library/runtime 进会话
    //MountedContainerSet（primary>library>runtime 解析序、shadowed 记录、
    //角色过滤遍历），同时登记 project.json（schema v2 role/parent，parent =
    //primary 登记 path）并附为 workspace 支撑容器（xref/关系分析的既有引擎
    //复用）。卸载 = 重建解析序，被遮蔽副本随之浮现，响应透传 shadowed 变化。
    if (name == "container_mount") {
        const auto action = arguments.value("action", std::string{"mount"});
        if (action != "mount" && action != "detach")
            return toolError("unknown container_mount action: " + action);
        const auto role = arguments.value("role", std::string{"support"});
        const auto mountedRole = role == "library" ? std::optional{mjbase::model::ContainerRole::Library}
                                 : role == "runtime"
                                     ? std::optional{mjbase::model::ContainerRole::Runtime}
                                     : std::nullopt;
        if (action == "mount") {
            if (role == "primary")
                return toolError(
                    "primary container is bound by project_open; container_mount only "
                    "mounts support containers");
            const auto path = arguments.value("path", std::string{});
            if (path.empty()) return toolError("path is required");
            if (!mountedRole.has_value()) {
                if (role != "support")
                    return toolError("container role '" + role + "' is reserved for a later iteration");
                auto result = current->addSupportingJar(path);
                if (result.is_err())
                    { const auto err = shared::toCaStatus(result.unwrap_err()); return toolError("mount container failed", &err); }
                const auto registered =
                    registerSupportInput(session.projectDir, result.unwrap().path, result.unwrap().fingerprint);
                if (registered.is_err()) return toolError("register container failed", &registered);
                return toolResult({{"action", action}, {"container", supportingResourceJson(result.unwrap())}});
            }

            //library/runtime：挂进角色化容器栈（id = 规范化登记 path）。栈 id
            //唯一约束 + workspace 支撑路径唯一约束共同保证工程内不重挂。
            namespace fs = std::filesystem;
            std::error_code fsError;
            const auto normalized = fs::absolute(fs::u8path(path)).lexically_normal();
            if (!fs::is_regular_file(normalized, fsError) || fsError)
                return toolError("container not found: " + normalized.u8string());
            const auto containerId = normalized.u8string();
            if (findMountedContainer(session, containerId) != nullptr)
                return toolError("container already mounted: " + containerId);
            std::string fingerprint;
            const auto fingerprintStatus = containerFingerprint(containerId, fingerprint);
            if (fingerprintStatus.is_err()) return toolError("mount container failed", &fingerprintStatus);

            auto container = std::make_shared<mjbase::model::ResourceContainer>();
            auto imported = mjbase::model::ResourceHelper::importFromJar(*container, containerId);
            if (imported.is_err())
                return toolError(std::string("import container failed: ") +
                                 mjbase::model::to_string(std::move(imported).unwrap_err()));

            //遮蔽快照（明文键集合；shadowedEntries 返回容器内引用，挂载重建
            //会原地清空重填，不能以引用跨 mount 持有）。
            std::unordered_set<std::string> shadowedBefore;
            for (const auto& item : session.containerStack->shadowedEntries())
                shadowedBefore.insert(item.uri.to_std_string() + '\n' +
                                      item.winnerId.to_std_string() + '\n' +
                                      item.shadowedId.to_std_string());
            const auto mounted = session.containerStack->mount(
                containerId, *mountedRole, container.get());
            if (mounted.is_err())
                return toolError(std::string("mount container failed: ") +
                                 mjbase::model::to_string(std::move(mounted).unwrap_err()));

            //workspace 支撑面附加（xref/关系跨容器参与）；失败回滚栈挂载。
            //library/runtime 走角色化登记（一条在册记录，role 只是标签），
            //不再经 addSupportingJar 产生第二套 support 记录。
            auto support = current->addSupportingContainer(containerId, *mountedRole);
            if (support.is_err()) {
                session.containerStack->unmount(containerId);
                const auto err = shared::toCaStatus(support.unwrap_err());
                return toolError("mount container failed", &err);
            }

            //工程登记（schema v2 role + parent = primary 登记 path）。
            const auto registered = registerRoleInput(session.projectDir,
                                                      containerId,
                                                      fingerprint,
                                                      *mountedRole == mjbase::model::ContainerRole::Library
                                                          ? mjbase::project::AnalysisInputRole::Library
                                                          : mjbase::project::AnalysisInputRole::Runtime,
                                                      current->sourcePath());
            if (registered.is_err()) return toolError("register container failed", &registered);

            ProjectSession::MountedContainerRecord record;
            record.id          = containerId;
            record.role        = *mountedRole;
            record.path        = containerId;
            record.fingerprint = fingerprint;
            record.supportId   = support.unwrap().id;
            record.container   = std::move(container);
            session.mountedContainers.push_back(std::move(record));

            //新增遮蔽记录透传（挂载 = 重建；新 first-wins 输家逐对上报）。
            //shadowedEntries 重建序不稳定，按 uri 排序保证响应确定性。
            Json shadowed = Json::array();
            for (const auto& item : session.containerStack->shadowedEntries()) {
                const auto key = item.uri.to_std_string() + '\n' +
                                 item.winnerId.to_std_string() + '\n' +
                                 item.shadowedId.to_std_string();
                if (shadowedBefore.count(key) == 0)
                    shadowed.push_back({{"uri", item.uri.to_std_string()},
                                        {"winner", item.winnerId.to_std_string()},
                                        {"shadowed", item.shadowedId.to_std_string()}});
            }
            std::sort(shadowed.begin(),
                      shadowed.end(),
                      [](const Json& a, const Json& b) {
                          return a.at("uri").get_ref<const std::string&>() <
                                 b.at("uri").get_ref<const std::string&>();
                      });
            Json payload = {{"action", action},
                             {"container", {{"role", role},
                                             {"id", containerId},
                                             {"path", containerId},
                                             {"fingerprint", fingerprint},
                                             {"support_id", support.unwrap().id}}}};
            if (!shadowed.empty()) payload["shadowed"] = std::move(shadowed);
            return toolResult(std::move(payload));
        }
        const auto supportId = arguments.value("support_id", std::string{});
        if (supportId.empty()) return toolError("support_id is required");
        if (!mountedRole.has_value()) {
            auto result = current->removeSupportingJar(supportId);
            if (result.is_err())
                { const auto err = shared::toCaStatus(result.unwrap_err()); return toolError("detach container failed", &err); }
            const auto deregistered = removeSupportInput(session.projectDir, result.unwrap().path);
            if (deregistered.is_err()) return toolError("deregister container failed", &deregistered);
            return toolResult({{"action", action}, {"removed", supportingResourceJson(result.unwrap())}});
        }

        //library/runtime 卸载：workspace 支撑面先行（失败不动栈），随后栈
        //卸载重建解析序——此前被本容器遮蔽（winner = 本容器）且重建后不再被
        //遮蔽的副本浮现，逐对透传。记录字段先物化（record 指向会话在册表，
        //erase 期间会失效）。
        const auto* record = findMountedContainer(session, supportId);
        if (record == nullptr || record->role != *mountedRole)
            return toolError("mounted container not found: " + supportId);
        const auto mountId          = record->id;
        const auto mountPath        = record->path;
        const auto mountRole        = record->role;
        const auto mountFingerprint = record->fingerprint;
        const auto supportRefId     = record->supportId;
        const auto removed = current->removeSupportingJar(supportRefId);
        if (removed.is_err())
            { const auto err = shared::toCaStatus(removed.unwrap_err()); return toolError("detach container failed", &err); }
        //卸载前遮蔽快照（同 mount 分支：引用不可跨 rebuild 持有）。
        std::vector<std::pair<std::string, std::string>> shadowedByMount;
        for (const auto& item : session.containerStack->shadowedEntries()) {
            if (item.winnerId.to_std_string() != mountId) continue;
            shadowedByMount.emplace_back(item.uri.to_std_string(),
                                         item.shadowedId.to_std_string());
        }
        session.containerStack->unmount(mountId);
        const auto deregistered = removeRoleInput(
            session.projectDir,
            mountPath,
            mountRole == mjbase::model::ContainerRole::Library
                ? mjbase::project::AnalysisInputRole::Library
                : mjbase::project::AnalysisInputRole::Runtime);
        if (deregistered.is_err()) return toolError("deregister container failed", &deregistered);
        Json surfaced = Json::array();
        for (const auto& [uri, shadowedContainer] : shadowedByMount) {
            const auto stillShadowed =
                std::any_of(session.containerStack->shadowedEntries().begin(),
                            session.containerStack->shadowedEntries().end(),
                            [&](const mjbase::model::ShadowedEntryRecord& prior) {
                                return prior.uri.to_std_string() == uri &&
                                       prior.shadowedId.to_std_string() == shadowedContainer;
                            });
            if (!stillShadowed)
                surfaced.push_back({{"uri", uri}, {"container", shadowedContainer}});
        }
        //同挂载响应：按 uri 排序保证响应确定性。
        std::sort(surfaced.begin(),
                  surfaced.end(),
                  [](const Json& a, const Json& b) {
                      return a.at("uri").get_ref<const std::string&>() <
                             b.at("uri").get_ref<const std::string&>();
                  });
        session.mountedContainers.erase(
            std::remove_if(session.mountedContainers.begin(),
                           session.mountedContainers.end(),
                           [&](const ProjectSession::MountedContainerRecord& item) {
                               return item.id == mountId;
                           }),
            session.mountedContainers.end());
        Json payload = {{"action", action},
                         {"removed", {{"role", role},
                                       {"id", mountId},
                                       {"path", mountPath},
                                       {"fingerprint", mountFingerprint}}}};
        if (!surfaced.empty()) payload["surfaced"] = std::move(surfaced);
        return toolResult(std::move(payload));
    }
    //xref_query / edit_apply 的选择器在入口翻译成内部分支名并复入：分支体与
    //响应形状与旧细粒度工具一致，只有寻址面换名（RFC §8.2 折叠口径）。
    if (name == "xref_query") {
        //TODO(analysis-cache): xref 整索引槽（AnalysisCacheStore.lookupXref/
        //storeXref）本轮未接。mjt 的 xref 面是逐成员按需查询（查询粒度 ≪ 整
        //索引一槽），槽键不含查询坐标；若把查询坐标塞进 profile 会让每次查询
        //各占一槽并互相触发同 produced_by 陈旧槽退役，缓存互相踩踏。接入前
        //提是 mjbase 侧暴露「按查询坐标的条目级 xref 缓存」或 mjt 侧自持整
        //索引重建面（ReferenceIndex::build + 序列化），二者都不是本卡范围。
        //分支选择器按键统一裁决收敛（target=method/field →
        //owner/name/descriptor；target=class → class），分支键白名单在此检查；
        //键表来自 branchKeysFor（单一事实源，与 contract 校验层共用）。
        const auto kind = arguments.value("kind", std::string{});
        std::string op;
        if (kind == "relations") {
            const auto branch = branchKeysFor("xref_query", "relations");
            if (auto error = checkBranchKeys(
                    "xref_query kind=relations", arguments, true, branch->selectors, {}))
                return std::move(*error);
            op = "class_relations";
        }
        else if (kind == "overrides") {
            const auto branch = branchKeysFor("xref_query", "overrides");
            if (auto error = checkBranchKeys("xref_query kind=overrides",
                                             arguments,
                                             true,
                                             branch->selectors,
                                             {}))
                return std::move(*error);
            op = "method_overrides";
        }
        else if (kind == "references") {
            const auto target    = arguments.value("target", std::string{});
            const auto direction = arguments.value("direction", std::string{"to"});
            if (target.empty() ||
                (target != "method" && target != "field" && target != "class"))
                return toolError(selectorKeyError(
                    "target", target, {"method", "field", "class"}));
            if (direction != "to" && direction != "from")
                return toolError(
                    selectorKeyError("direction", direction, {"to", "from"}));
            if (direction == "from" && target != "method")
                return toolError("xref_query direction=from requires target=method; got " +
                                 target);
            const auto branch = branchKeysFor("xref_query", "references", target, direction);
            if (!branch.has_value())
                return toolError(selectorKeyError(
                    "target", target, {"method", "field", "class"}));
            //分支标签带 target/direction，错误文案可定位到具体分支。
            const auto label = "xref_query kind=references target=" + target +
                               " direction=" + direction;
            if (auto error = checkBranchKeys(label,
                                             arguments,
                                             true,
                                             branch->selectors,
                                             branch->supported,
                                             branch->optionalSelectors))
                return std::move(*error);
            if (direction == "to") {
                op = target == "method"   ? "xref_method"
                     : target == "field"  ? "xref_field"
                                          : "xref_class";
            }
            else {
                op = "method_callees";
            }
        }
        else {
            return toolError(selectorKeyError(
                "kind", kind, {"references", "relations", "overrides"}));
        }
        return callMutationTool(op, arguments, session);
    }
    if (name == "edit_apply") {
        const auto kind     = arguments.value("kind", std::string{});
        const bool dryRun   = arguments.value("dry_run", false);
        Json translatedArgs = arguments;
        std::string op;
        if (kind == "structured") {
            op = "patch_apply";
            //结构化 ClassMutation 的 15 种变更种类改名 mutation_kind：kind 槽
            //已被 edit_apply 工具级选择器占用。
            if (arguments.contains("mutation_kind"))
                translatedArgs["kind"] = arguments.at("mutation_kind");
        }
        else if (kind == "jasm") {
            op = dryRun ? "patch_validate_jasm" : "patch_apply_jasm";
        }
        else if (kind == "jasm_diff") {
            op = dryRun ? "patch_validate_jasm_diff" : "patch_apply_jasm_diff";
        }
        else if (kind == "method_jasm") {
            op = dryRun ? "method_validate_jasm" : "method_apply_jasm";
        }
        else if (kind == "method_jasm_diff") {
            op = dryRun ? "method_validate_jasm_diff" : "method_apply_jasm_diff";
        }
        else if (kind == "class_bytes") {
            op = "patch_replace_class_bytes";
        }
        else if (kind == "pipeline") {
            //opt_preview/opt_apply 的合并面（pass_run 随旧工具一并收敛）：
            //空 pipeline/passes 兜底 default-optimize；dry_run 即 preview。
            PipelineRequest pipeline;
            std::string     parseError;
            if (!parsePipelineRequest(arguments, pipeline, parseError)) return toolError(parseError);
            if (pipeline.pipeline.empty() && pipeline.passNames.empty())
                pipeline.pipeline = "default-optimize";
            auto request = mutationRequest(arguments);
            request.dryRun = dryRun || request.dryRun;
            const auto result = current->applyPipeline(pipeline, request);
            return toolResult(mutationJson(result), result.status.is_err());
        }
        else if (kind == "rename") {
            op = dryRun ? "rename_analyze" : "rename_apply";
            //rename 目标种类改名 rename_kind（同 mutation_kind 的占位冲突处理）。
            if (arguments.contains("rename_kind"))
                translatedArgs["kind"] = arguments.at("rename_kind");
        }
        else if (kind == "mapping") {
            op = dryRun ? "mapping_analyze" : "mapping_apply";
        }
        else {
            return toolError("unknown edit_apply kind: " + kind);
        }
        return callMutationTool(op, translatedArgs, session);
    }
    if (name == "edit_history") {
        const auto action = arguments.value("action", std::string{});
        //validation 只作用于走账本变更门禁的动作（undo/redo/restore/
        //revert_class）；查询与压缩动作不吃它，显式拒绝避免静默忽略。
        if (arguments.contains("validation") && action != "undo" && action != "redo" &&
            action != "restore" && action != "revert_class")
            return toolError("validation is only valid for undo/redo/restore/revert_class");
        if (action == "savepoint") {
            //savepoint = 台账书签：零字节代价、不动 revision；flush 后以书签行
            //落盘（跨会话存活是相对旧克隆实现的已声明语义变化）。
            const auto bookmarkName = arguments.value("name", std::string{});
            const auto status = current->createSavepoint(bookmarkName);
            if (status.is_err()) {
                const auto err = shared::toCaStatus(status.unwrap_err());
                return toolError("create savepoint failed", &err);
            }
            return toolResult({{"action", action},
                               {"name", bookmarkName},
                               {"revision", current->revision()},
                               {"created", true}});
        }
        if (action == "restore" || action == "undo" || action == "redo") {
            const auto request = mutationRequest(arguments);
            const auto result = action == "restore"
                                    ? current->restoreSavepoint(arguments.value("name", std::string{}), request)
                                    : action == "undo" ? current->undo(request)
                                                       : current->redo(request);
            return toolResult(mutationJson(result), result.status.is_err());
        }
        if (action == "revert_class") {
            std::string classUri;
            Json        slotError;
            if (!slot::resolveClassUriArgument(*current, arguments, "class_uri", classUri, slotError))
                return slotError;
            const auto result = current->revertClass(classUri, mutationRequest(arguments));
            return toolResult(mutationJson(result), result.status.is_err());
        }
        if (action == "compact") {
            //手动压缩（RFC §6.4 / 设计文档 §5.2）：保留参数显式给定时生效，
            //缺省 = 书签全保留 + undo 深度承诺按配置；引擎在压缩前置 flush。
            mjbase::project::LedgerCompactRequest request;
            if (arguments.contains("keep_recent_bookmarks"))
                request.keepRecentBookmarks =
                    arguments.value("keep_recent_bookmarks", static_cast<ca::u64>(0));
            if (arguments.contains("keep_undo_depth"))
                request.keepUndoDepth =
                    arguments.value("keep_undo_depth", static_cast<ca::u64>(0));
            auto report = session.workspace->compactLedger(request);
            if (report.is_err()) {
                const auto err = shared::toCaStatus(std::move(report).unwrap_err());
                return toolError("compact ledger failed", &err);
            }
            auto value = std::move(report).unwrap();
            Json payload = {{"action", action},
                            {"executed", value.executed},
                            {"retention_txn", value.retentionTxn},
                            {"dropped_transactions", value.droppedTransactions},
                            {"revision", current->revision()}};
            if (!value.skippedReason.is_empty())
                payload["skipped_reason"] = value.skippedReason.to_std_string();
            if (!value.baselineSection.is_empty())
                payload["baseline_section"] = value.baselineSection.to_std_string();
            if (!value.baselineDigest.is_empty())
                payload["baseline_digest"] = value.baselineDigest.to_std_string();
            Json droppedBookmarks = Json::array();
            for (const auto& item : value.droppedBookmarks)
                droppedBookmarks.push_back(item.to_std_string());
            payload["dropped_bookmarks"] = std::move(droppedBookmarks);
            Json released = Json::array();
            for (const auto& item : value.released) released.push_back(item.to_std_string());
            payload["released"] = std::move(released);
            return toolResult(std::move(payload));
        }
        if (action == "list") {
            //台账列览：先 flush 把 pending 落盘（幂等、不动 revision），磁盘上
            //的活动链因此与内存头一致；再按锚/事务/书签装载序分页返回。
            const auto flushed = session.workspace->flushLedger();
            if (flushed.is_err()) {
                const auto err = shared::toCaStatus(flushed.unwrap_err());
                return toolError("flush ledger failed", &err);
            }
            auto ledger = readPersistedLedger(session.projectDir);
            if (ledger.is_err()) {
                const auto status = projectStoreStatus(std::move(ledger).unwrap_err(), "read ledger");
                return toolError("read ledger failed", &status);
            }
            const auto opLabel = [](mjbase::project::LedgerEntryOp op) {
                switch (op) {
                case mjbase::project::LedgerEntryOp::Replace: return "replace";
                case mjbase::project::LedgerEntryOp::Add: return "add";
                case mjbase::project::LedgerEntryOp::Remove: return "remove";
                }
                return "unknown";
            };
            Json values = Json::array();
            //unwrap() 右值重载按值弹出：先物化快照再遍历，不能在临时对象上
            //链式取 rows 引用（range-for 只延长最外层临时，中间对象先析构）。
            auto snapshot = std::move(ledger).unwrap();
            for (const auto& row : snapshot.rows) {
                if (row.kind == mjbase::project::LedgerRow::Kind::Transaction) {
                    Json entries = Json::array();
                    for (const auto& entry : row.transaction.entries) {
                        Json item = {{"uri", entry.uri.to_std_string()},
                                     {"op", opLabel(entry.op)},
                                     {"before", entry.before.to_std_string()},
                                     {"after", entry.after.to_std_string()}};
                        if (!entry.payloadRef.is_empty())
                            item["payload_ref"] = entry.payloadRef.to_std_string();
                        entries.push_back(std::move(item));
                    }
                    values.push_back({{"row", "txn"},
                                      {"txn", row.transaction.txn},
                                      {"parent", row.transaction.parent},
                                      {"label", row.transaction.label.to_std_string()},
                                      {"base_container", row.transaction.baseContainer.to_std_string()},
                                      {"produced_by", row.transaction.producedBy.to_std_string()},
                                      {"created_at", row.transaction.createdAt.to_std_string()},
                                      {"entries", std::move(entries)}});
                    continue;
                }
                if (row.kind == mjbase::project::LedgerRow::Kind::Anchor) {
                    Json anchor = {{"row", "anchor"},
                                   {"txn", row.anchor.txn},
                                   {"baseline_section", row.anchor.baselineSection.to_std_string()},
                                   {"container", row.anchor.container.to_std_string()},
                                   {"compacted_txns", row.anchor.compactedTxns},
                                   {"created_at", row.anchor.createdAt.to_std_string()}};
                    if (row.anchor.previousTxn != 0) {
                        anchor["previous_txn"]       = row.anchor.previousTxn;
                        anchor["previous_container"] = row.anchor.previousContainer.to_std_string();
                    }
                    values.push_back(std::move(anchor));
                    continue;
                }
                values.push_back({{"row", "bookmark"},
                                  {"name", row.bookmark.name.to_std_string()},
                                  {"txn", row.bookmark.txn},
                                  {"created_at", row.bookmark.createdAt.to_std_string()}});
            }
            //台账列览的行键是三类行（txn/anchor/bookmark）的键并集，
            //静态声明供 fields 校验。
            Json paged;
        std::string pageError;
        if (!page(std::move(values), arguments, paged, pageError,
                  {"base_container", "baseline_section", "compacted_txns", "container",
                   "created_at", "entries", "label", "name", "parent",
                   "previous_container", "previous_txn", "produced_by", "row", "txn"}))
            return toolError(pageError);
        return toolResult(std::move(paged));
        }
        if (action == "modified_classes") {
            auto modified = current->modifiedClasses();
            if (modified.is_err()) {
                const auto err = shared::toCaStatus(modified.unwrap_err());
                return toolError("list modified classes failed", &err);
            }
            Json values = Json::array();
            for (const auto& item : modified.unwrap()) {
            values.push_back({{"resource_uri", item.resourceUri},
                              {"original_internal_name", item.originalInternalName},
                              {"current_internal_name", item.currentInternalName},
                              {"original_fingerprint", item.originalFingerprint},
                              {"current_fingerprint", item.currentFingerprint},
                              {"original_size", item.originalSize},
                              {"current_size", item.currentSize}});
            }
            Json paged;
        std::string pageError;
        if (!page(std::move(values), arguments, paged, pageError,
                  {"current_fingerprint", "current_internal_name", "current_size",
                   "original_fingerprint", "original_internal_name", "original_size",
                   "resource_uri"}))
            return toolError(pageError);
        return toolResult(std::move(paged));
        }
        return toolError("unknown edit_history action: " + action);
    }
    if (name == "xref_method" || name == "xref_field" || name == "xref_class" ||
        name == "method_callees") {
        //name 槽：xref/callees 族 raw **直通**（verifyRaw=false——
        //表外合法目标如 java/lang/Object 的引用查询零漂移），Ref 形状照常
        //全量解析（unknown-handle 等四分类不受直通位影响）。
        const auto slots = slot::loadWorkspaceShadowTable(*current);
        std::string owner;
        std::string member;
        std::string descriptor;
        Json        slotError;
        if (!slot::resolveMethodSlots(slots, arguments, false, owner, member, descriptor, slotError))
            return slotError;
        //descriptor 省略时按宿主类名称解析回填（target=field 走字段池，
        //其余走方法池）——唯一命中即用；名字不存在报错（复用既有的候选枚举
        //机）；真重载报歧义并列出全部候选（同一 diagnostics 格式）。显式
        //传 descriptor 时行为零变化。
        const bool fieldSelector = name == "xref_field";
        if (descriptor.empty() &&
            !slot::backfillMemberDescriptor(*current, owner, member, fieldSelector, descriptor, slotError))
            return slotError;
        std::string className;
        if (name == "xref_class") {
            Json classError;
            if (!slot::resolveClassArgument(*current, arguments, "class", false, className, classError))
                return classError;
        }
        //descriptor 诊断——owner+name 在宿主类真实存在而传入 descriptor
        //不匹配时，mjbase 查询只会静默返回空集；此处按 classInfo 收集该名字
        //的全部真实描述符，匹配失败则在响应上附 diagnostics 候选列表。类不
        //存在或名字不存在时不诊断（NOT_FOUND 一类错误本身已明确）。
        std::vector<std::string> descriptorCandidates;
        if (name != "xref_class") {
            auto info = current->classInfo(owner);
            if (info.is_ok()) {
                const auto& clazz = info.unwrap();
                const auto addCandidate = [&](const std::string& candidate) {
                    if (std::find(descriptorCandidates.begin(),
                                  descriptorCandidates.end(),
                                  candidate) == descriptorCandidates.end())
                        descriptorCandidates.push_back(candidate);
                };
                for (const auto& method : clazz.methods)
                    if (method.name == member) addCandidate(method.descriptor);
                for (const auto& field : clazz.fields)
                    if (field.name == member) addCandidate(field.descriptor);
            }
        }
        //显式 if-else 分派替代原 3 层嵌套三元，避免重复读取 arguments 与拷贝；
        //用 lambda 单次构造返回。
        auto query = [&]() -> mjt::workspace::WorkspaceResult<
                             std::vector<mjt::workspace::ReferenceInfo>> {
            if (name == "xref_method") {
                return current->xrefsToMethod(owner, member, descriptor);
            }
            if (name == "xref_field") {
                return current->xrefsToField(owner, member, descriptor);
            }
            if (name == "xref_class") {
                return current->xrefsToClass(className);
            }
            return current->calleesOfMethod(owner, member, descriptor,
                                            arguments.value("resolved", false));
        };
        auto references = query();
        if (references.is_err()) {
            const auto err = shared::toCaStatus(references.unwrap_err());
            return toolError("reference query failed", &err);
        }
        auto items = std::move(references).unwrap();

        //role 过滤（RFC 0001 P5）：站点归属按「primary = 非支撑站点；支撑站
        //点 = workspace 在册 id → 挂载栈角色对号」判定，未挂载的支撑站点归
        //support（遗留角色）。显式过滤值之外的请求在 schema 已被拒绝。
        const auto roleFilter = arguments.value("role", std::string{});
        if (!roleFilter.empty() && roleFilter != "primary" && roleFilter != "support") {
            const auto wanted = roleFilter == "library" ? mjbase::model::ContainerRole::Library
                                                        : mjbase::model::ContainerRole::Runtime;
            const auto inventory = mountedClassInventory(session, wanted);
            std::unordered_set<std::string> roleSupportIds;
            for (const auto& record : session.mountedContainers)
                if (record.role == wanted) roleSupportIds.insert(record.supportId);
            items.erase(std::remove_if(items.begin(),
                                       items.end(),
                                       [&](const mjt::workspace::ReferenceInfo& item) {
                                           //保留 = 站点在本角色容器内；其余全部剔除。
                                           if (!item.supporting) return true;
                                           if (roleSupportIds.count(item.supportId) == 0)
                                               return true;
                                           return inventory.find(item.sourceClass) ==
                                                  inventory.end();
                                       }),
                        items.end());
        }
        else if (roleFilter == "primary") {
            items.erase(std::remove_if(items.begin(),
                                       items.end(),
                                       [](const mjt::workspace::ReferenceInfo& item) {
                                           return item.supporting;
                                       }),
                        items.end());
        }
        else if (roleFilter == "support") {
            items.erase(std::remove_if(items.begin(),
                                       items.end(),
                                       [&](const mjt::workspace::ReferenceInfo& item) {
                                           if (!item.supporting) return false;
                                           return findMountedContainer(session, item.supportId) ==
                                                  nullptr;
                                       }),
                        items.end());
        }

        Json values = Json::array();
        for (const auto& item : items) values.push_back(referenceJson(item));
        Json paged;
        std::string pageError;
        //引用行键静态全集（referenceJson 形状），fields 校验不依赖结果
        //集——空引用集 + 合法键照常返回空 rows。
        if (!page(std::move(values), arguments, paged, pageError, {"kind", "source", "target"}))
            return toolError(pageError);
        if (!descriptorCandidates.empty() &&
            std::find(descriptorCandidates.begin(),
                      descriptorCandidates.end(),
                      descriptor) == descriptorCandidates.end()) {
            std::string message =
                "descriptor not found for " + owner + "." + member + "; candidates: ";
            for (std::size_t index = 0; index < descriptorCandidates.size(); ++index) {
                if (index > 0) message += ", ";
                message += descriptorCandidates[index];
            }
            paged["diagnostics"] = Json::array({message});
        }
        return toolResult(std::move(paged));
    }
    if (name == "class_relations") {
        std::string className;
        Json        slotError;
        if (!slot::resolveClassArgument(*current, arguments, "class", true, className, slotError))
            return slotError;
        auto value = current->classRelations(className);
        if (value.is_err()) {
            const auto err = shared::toCaStatus(value.unwrap_err());
            return toolError("get class relations failed", &err);
        }
        const auto array = [](const std::vector<std::string>& source) {
            Json result = Json::array();
            for (const auto& item : source) result.push_back(item);
            return result;
        };
        const auto& relations = value.unwrap();
        Json sources = Json::array();
        for (const auto& source : relations.sources) {
            sources.push_back({{"class", source.internalName},
                               {"supporting", source.supporting},
                               {"support_id", source.supportId}});
        }
        return toolResult({{"superclasses", array(relations.superclasses)},
                           {"interfaces", array(relations.interfaces)},
                           {"direct_subclasses", array(relations.directSubclasses)},
                           {"all_subclasses", array(relations.allSubclasses)},
                           {"implementors", array(relations.implementors)},
                           {"sources", std::move(sources)}});
    }
    if (name == "method_overrides") {
        const auto slots = slot::loadWorkspaceShadowTable(*current);
        std::string owner;
        std::string member;
        std::string descriptor;
        Json        slotError;
        if (!slot::resolveMethodSlots(slots, arguments, true, owner, member, descriptor, slotError))
            return slotError;
        auto value = current->overridesOfMethod(owner, member, descriptor);
        if (value.is_err()) {
            const auto err = shared::toCaStatus(value.unwrap_err());
            return toolError("get method overrides failed", &err);
        }
        Json values = Json::array();
        for (const auto& item : value.unwrap())
            values.push_back({{"owner", item.owner},
                              {"name", item.name},
                              {"descriptor", item.descriptor},
                              {"primary", item.primary},
                              {"supporting", item.supporting},
                              {"support_id", item.supportId}});
        return toolResult({{"overrides", std::move(values)}});
    }
    if (name == "patch_validate_jasm" || name == "patch_apply_jasm") {
        //name 槽：uri = Ref → 精确资源 URI；raw → 内部名核表后
        //原样透传（URI 契约零漂移）。
        std::string uri;
        Json        slotError;
        if (!slot::resolveClassUriArgument(*current, arguments, "uri", uri, slotError))
            return slotError;
        const auto source = arguments.value("source", std::string{});
        const auto sourceName = arguments.value("source_name", uri + ".jasm");
        const auto result = name == "patch_validate_jasm"
                                ? current->validateJasm(uri,
                                                        utf16(sourceName).ref(),
                                                        utf16(source).ref(),
                                                        mutationRequest(arguments))
                                : current->applyJasm(uri,
                                                     utf16(sourceName).ref(),
                                                     utf16(source).ref(),
                                                     mutationRequest(arguments));
        return toolResult(mutationJson(result), result.status.is_err());
    }
    if (name == "patch_validate_jasm_diff" || name == "patch_apply_jasm_diff") {
        std::string uri;
        Json        slotError;
        if (!slot::resolveClassUriArgument(*current, arguments, "uri", uri, slotError))
            return slotError;
        const auto diffText   = arguments.value("diff", std::string{});
        const auto sourceName = arguments.value("source_name", uri + ".jasm");
        auto jasm = current->getJasm(uri);
        if (!jasm.isOk()) {
            Json diagnostics = Json::array();
            for (const auto& item : jasm.diagnostics) diagnostics.push_back(diagnosticJson(item));
            return toolResult({{"diagnostics", std::move(diagnostics)}}, true);
        }
        const auto base = utf8(jasm.value->source.ref());
        auto applied = applyUnifiedDiff(base, diffText);
        if (!applied.error.empty()) {
            Json payload = {{"status", statusJson(ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                                                                 applied.error))},
                            {"failed_hunk", applied.failedHunk}};
            return toolResult(std::move(payload), true);
        }
        const auto result = name == "patch_validate_jasm_diff"
                                ? current->validateJasm(uri,
                                                        utf16(sourceName).ref(),
                                                        utf16(applied.text).ref(),
                                                        mutationRequest(arguments))
                                : current->applyJasm(uri,
                                                     utf16(sourceName).ref(),
                                                     utf16(applied.text).ref(),
                                                     mutationRequest(arguments));
        return toolResult(mutationJson(result), result.status.is_err());
    }
    if (name == "method_validate_jasm" || name == "method_apply_jasm" ||
        name == "method_validate_jasm_diff" || name == "method_apply_jasm_diff") {
        //name 槽：owner/name 同 method 三元组口径（desc 自表回填），
        //后续 source_name 拼装与 classInfo 都用解析后的 raw 名。
        const auto slots = slot::loadWorkspaceShadowTable(*current);
        std::string owner;
        std::string mname;
        std::string descriptor;
        Json        slotError;
        if (!slot::resolveMethodSlots(slots, arguments, true, owner, mname, descriptor, slotError))
            return slotError;
        const auto sourceName = arguments.value("source_name", owner + "." + mname + ".jasm");
        //1. 取单方法规范 Jasm（与 class Jasm 内方法段逐字节一致）。
        auto method = current->getMethodJasm(owner, mname, descriptor);
        if (!method.isOk()) {
            Json diagnostics = Json::array();
            for (const auto& item : method.diagnostics) diagnostics.push_back(diagnosticJson(item));
            return toolResult({{"diagnostics", std::move(diagnostics)}}, true);
        }
        const auto oldMethodSource = utf8(method.value->source.ref());
        //2. 解析最终的新方法文本：全量直接用 source；diff 先应用到 oldMethodSource。
        std::string newMethodSource;
        if (name == "method_validate_jasm" || name == "method_apply_jasm") {
            newMethodSource = arguments.value("source", std::string{});
        } else {
            const auto diffText = arguments.value("diff", std::string{});
            auto applied = applyUnifiedDiff(oldMethodSource, diffText);
            if (!applied.error.empty()) {
                Json payload = {{"status", statusJson(ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                                                                     applied.error))},
                                {"failed_hunk", applied.failedHunk}};
                return toolResult(std::move(payload), true);
            }
            newMethodSource = applied.text;
        }
        //3. 取整 class Jasm 作 base，把旧方法文本精确替换为新方法文本。
        auto classInfo = current->classInfo(owner);
        if (classInfo.is_err()) {
            const auto err = shared::toCaStatus(classInfo.unwrap_err());
            return toolError("select class for method jasm failed", &err);
        }
        auto jasm = current->getJasm(classInfo.unwrap().resourceUri);
        if (!jasm.isOk()) {
            Json diagnostics = Json::array();
            for (const auto& item : jasm.diagnostics) diagnostics.push_back(diagnosticJson(item));
            return toolResult({{"diagnostics", std::move(diagnostics)}}, true);
        }
        const auto classSource = utf8(jasm.value->source.ref());
        const auto first = classSource.find(oldMethodSource);
        if (first == std::string::npos) {
            return toolResult({{"status", statusJson(ca::ErrStatus(ca::StatusCode::INTERNAL,
                                                                   "method source not found in class Jasm"))}},
                              true);
        }
        const auto second = classSource.find(oldMethodSource, first + 1);
        if (second != std::string::npos) {
            return toolResult({{"status", statusJson(ca::ErrStatus(ca::StatusCode::INTERNAL,
                                                                   "method source is ambiguous in class Jasm"))}},
                              true);
        }
        std::string newClassSource = classSource;
        newClassSource.replace(first, oldMethodSource.size(), newMethodSource);
        const auto result =
            name == "method_validate_jasm" || name == "method_validate_jasm_diff"
                ? current->validateJasm(classInfo.unwrap().resourceUri,
                                        utf16(sourceName).ref(),
                                        utf16(newClassSource).ref(),
                                        mutationRequest(arguments))
                : current->applyJasm(classInfo.unwrap().resourceUri,
                                     utf16(sourceName).ref(),
                                     utf16(newClassSource).ref(),
                                     mutationRequest(arguments));
        return toolResult(mutationJson(result), result.status.is_err());
    }
    if (name == "patch_replace_class_bytes") {
        std::vector<ca::u8> bytes;
        std::string parseError;
        if (!decodeBase64(arguments.value("content_base64", std::string{}), bytes, parseError))
            return toolError(parseError);
        std::string classUri;
        Json        slotError;
        if (!slot::resolveClassUriArgument(*current, arguments, "class_uri", classUri, slotError))
            return slotError;
        const auto result = current->replaceClassBytes(classUri, bytes, mutationRequest(arguments));
        return toolResult(mutationJson(result), result.status.is_err());
    }
    if (name == "patch_apply") {
        //name 槽：class_uri = Ref → 精确资源 URI / raw → 核表后
        //原样；member_name = MemberRef → 真名（desc 与 member_descriptor 核对）。
        //解析值写回参数副本，parseMutation 及下游零改动。
        Json resolvedArgs = arguments;
        {
            std::string classUri;
            Json        slotError;
            if (!slot::resolveClassUriArgument(*current, arguments, "class_uri", classUri, slotError))
                return slotError;
            resolvedArgs["class_uri"] = classUri;
            //member_name 只在「寻址既有成员」的 kind 上做 name 槽解析
            //（12 kind 全覆盖——5 访问/删除/换体 kind +
            //rename_local / replace_try_catch_blocks / insert|replace|remove_
            //instruction（findMethod 寻址）+ annotation 两 kind 在
            //annotation_target=field|method 时经 findField/findMethod 寻址）；
            //add_field/add_method 的 member_name 是新符号字面量（sidecar 先于
            //它存在），与 rename 的 new_name 同性质：不核表、不解析；
            //annotation_target=class 时 member 不参与寻址，同样不解析。
            const auto mutationKind = arguments.value("kind", std::string{});
            const bool annotationMemberIsReference =
                (mutationKind == "add_marker_annotation" ||
                 mutationKind == "remove_annotation") &&
                (arguments.value("annotation_target", std::string{}) == "field" ||
                 arguments.value("annotation_target", std::string{}) == "method");
            const bool memberIsReference =
                mutationKind == "set_field_access" ||
                mutationKind == "set_method_access" ||
                mutationKind == "remove_field" ||
                mutationKind == "remove_method" ||
                mutationKind == "replace_method_body" ||
                mutationKind == "rename_local" ||
                mutationKind == "replace_try_catch_blocks" ||
                mutationKind == "insert_instruction" ||
                mutationKind == "replace_instruction" ||
                mutationKind == "remove_instruction" ||
                annotationMemberIsReference;
            const auto memberName = arguments.value("member_name", std::string{});
            if (memberIsReference && !memberName.empty()) {
                const auto slots = slot::loadWorkspaceShadowTable(*current);
                const auto ownerInternal =
                    mjt::internalNameOfUri(classUri);
                //描述符槽：member_descriptor 带 `${` 引用形状 → 内嵌类 handle
                //展开；展开值参与成员核对并覆盖回 resolvedArgs（parseMutation
                //读展开值，handle 字面量不落下游）。
                std::string memberDescriptor;
                if (!slot::resolveDescriptorArgument(
                        slots,
                        arguments.value("member_descriptor", std::string{}),
                        memberDescriptor,
                        slotError))
                    return slotError;
                resolvedArgs["member_descriptor"] = memberDescriptor;
                slot::SlotName memberSlot;
                if (!slot::resolveMemberSlot(slots,
                                             ownerInternal,
                                             memberName,
                                             memberDescriptor,
                                             true,
                                             memberSlot,
                                             slotError))
                    return slotError;
                resolvedArgs["member_name"] = memberSlot.raw;
            }
        }
        ClassMutation mutation;
        std::string   parseError;
        if (!parseMutation(resolvedArgs, mutation, parseError)) return toolError(parseError);
        const auto result = current->applyClassMutation(mutation, mutationRequest(arguments));
        return toolResult(mutationJson(result), result.status.is_err());
    }
    if (name == "rename_analyze" || name == "rename_apply") {
        //name 槽：kind=class → owner；kind=field/method →
        //owner+name+descriptor；kind=local → owner+method_name（name 是局部
        //变量名，非类/成员引用，不接线）。kind=package 的 owner 是包前缀
        //（非类引用），不核表直通；new_name 恒不碰（新名字面量）。
        Json resolvedArgs = arguments;
        {
            const auto kind = arguments.value("kind", std::string{});
            const bool classShapedOwner =
                kind == "class" || kind == "field" || kind == "method" || kind == "local";
            std::string owner;
            Json        slotError;
            if (classShapedOwner) {
                const auto slots = slot::loadWorkspaceShadowTable(*current);
                if (!slot::resolveClassArgument(*current, arguments, "owner", true, owner, slotError))
                    return slotError;
                resolvedArgs["owner"] = owner;
            }
            if (kind == "field" || kind == "method") {
                const auto     slots = slot::loadWorkspaceShadowTable(*current);
                std::string    member;
                std::string    descriptor;
                if (!slot::resolveMethodSlots(slots, arguments, true, owner, member, descriptor, slotError))
                    return slotError;
                resolvedArgs["owner"]      = owner;
                resolvedArgs["name"]       = member;
                resolvedArgs["descriptor"] = descriptor;
            }
            else if (kind == "local") {
                //method_name 是成员槽；name/descriptor 保持调用方原值（局部
                //变量名与其类型描述符不是类/成员引用）。
                const auto     slots = slot::loadWorkspaceShadowTable(*current);
                const auto     methodName = arguments.value("method_name", std::string{});
                slot::SlotName methodSlot;
                if (!slot::resolveMemberSlot(slots,
                                             owner,
                                             methodName,
                                             arguments.value("descriptor", std::string{}),
                                             true,
                                             methodSlot,
                                             slotError))
                    return slotError;
                resolvedArgs["method_name"] = methodSlot.raw;
            }
        }
        RenameRequest rename;
        std::string   parseError;
        if (!parseRename(resolvedArgs, rename, parseError)) return toolError(parseError);
        if (name == "rename_apply") {
            const auto result = current->applyRename(rename, mutationRequest(arguments));
            return toolResult(mutationJson(result), result.status.is_err());
        }
        const auto analysis = current->analyzeRename(rename);
        Json mappings = Json::array();
        Json overrides = Json::array();
        for (const auto& item : analysis.mappings)
            mappings.push_back({{"kind", item.kind}, {"before", item.before}, {"after", item.after}});
        for (const auto& item : analysis.overrideComponent)
            overrides.push_back({{"owner", item.owner},
                                 {"name", item.name},
                                 {"descriptor", item.descriptor},
                                 {"primary", item.primary},
                                 {"supporting", item.supporting},
                                 {"support_id", item.supportId}});
        return toolResult({{"status", statusJson(shared::toCaStatus(analysis.status))},
                           {"ready_to_apply", analysis.readyToApply},
                           {"requires_override_review", analysis.requiresOverrideReview},
                           {"mappings", std::move(mappings)},
                           {"overrides", std::move(overrides)},
                           {"impact", impactJson(analysis.impact)}},
                          analysis.status.is_err());
    }
    if (name == "mapping_analyze" || name == "mapping_apply") {
        WorkspaceRemapRequest remap;
        remap.format      = arguments.value("format", std::string{});
        remap.mappingText = arguments.value("mapping_text", std::string{});
        remap.policy      = arguments.value("policy", std::string{"bytecode-only"});
        if (name == "mapping_analyze") {
            const auto analysis = analyzeWorkspaceRemap(*current, remap);
            return toolResult(impactJson(analysis), analysis.status.is_err());
        }
        const auto result = applyWorkspaceRemap(*current, remap, mutationRequest(arguments));
        return toolResult(mutationJson(result), result.status.is_err());
    }
    if (name == "project_export") {
        //导出前 flush 挂点：台账与分析缓存与 close 同点落盘（同一崩溃窗语义）。
        const auto cacheFlushed = flushAnalysisCache(session);
        if (cacheFlushed.is_err()) return toolError("flush analysis cache failed", &cacheFlushed);
        const auto status = current->exportJar(arguments.value("path", std::string{}));
        if (status.is_err()) {
            const auto err = shared::toCaStatus(status.unwrap_err());
            return toolError("export project failed", &err);
        }
        return toolResult({{"exported", true}, {"revision", current->revision()}});
    }
    return std::nullopt;
}

}   //namespace mjt::mcp
