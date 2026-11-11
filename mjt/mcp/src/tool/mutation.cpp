#include "service.hpp"

#include <algorithm>
#include <functional>
#include <optional>
#include <set>
#include <unordered_map>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "json.hpp"
#include "parser.hpp"
#include "shared/jar_remapper.hpp"
#include "shared/shadow_display.hpp"
#include "shared/status_convert.hpp"
#include "tool/shadow_slot.hpp"

#include "mjbase/asm/remapper/member_remap_plan.hpp"
#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "mjbase/project/edit_ledger.hpp"

#include "workspace/analysis_ops.hpp"
#include "workspace/edit_command.hpp"
#include "workspace/text_diff.hpp"

namespace mjt::mcp {

using namespace detail;
using mjt::workspace::ClassMutation;
using mjt::workspace::EditCommand;
using mjt::workspace::PipelineRequest;
using mjt::workspace::RenameRequest;
using mjt::workspace::WorkspaceBase;
using mjt::workspace::applyEdit;
using mjt::workspace::applyUnifiedDiff;
using mjt::workspace::spliceMethodJasm;

namespace {

///std::string → 借用视图（B3 接口面 Utf8StringRef 参数化适配；克隆前提：
///源串在调用期内存活）。
ca::str::Utf8StringRef utf8Ref(const std::string& value) noexcept
{
    return ca::str::Utf8StringRef::from_data(
        value.empty() ? nullptr : reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

///std::string → owning Utf8String（EditCommand 载荷字段）。
ca::str::Utf8String ownUtf8(const std::string& value)
{
    return ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

}   //namespace

std::optional<Json> McpToolService::callMutationTool(
    const std::string& name, const Json& arguments, ProjectSession& session)
{
    WorkspaceBase* current = session.workspace.get();
    //查询算子面（analysis_ops 自由函数）只读消费工作容器；未加载按旧
    //ensureLoaded 口径报 FailedPrecondition（编辑面各分支本就以已开项目
    //为前提）。
    const auto* working = current != nullptr ? current->workingContainer() : nullptr;
    if (working == nullptr) {
        const auto status = ca::ErrStatus(ca::StatusCode::FAILED_PRECONDITION,
                                          "workspace is not loaded");
        return toolError("workspace mutation failed", &status);
    }
    //dex 会话守卫（apk/裸 dex 只读分析原型）：变更族一律拒绝，白名单只放行
    //引用/层次关系的只读面（xref_query 入口与其内部翻译 op 都在表内）。
    static const std::set<std::string> kDexSessionReadOps = {
        "xref_query", "xref_method", "xref_field", "xref_class",
        "method_callees", "class_relations", "method_overrides"};
    if (current->isDexBacked() && kDexSessionReadOps.count(name) == 0) {
        return toolError("mutation '" + name +
                         "' is not supported for apk/dex input (read-only analysis prototype)");
    }
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
                auto result = current->addSupportingRole(
                    utf8Ref(path), ca::str::Utf8StringRef::from_cstr("support"));
                if (result.is_err())
                    { const auto err = shared::toCaStatus(result.unwrap_err()); return toolError("mount container failed", &err); }
                // SupportingResourceInfo 携 move-only Utf8String：unwrap 走移动
                // 重载，path/fingerprint 落 std::string（登记目录字节路径面）。
                const auto mounted = std::move(result).unwrap();
                const auto registered = registerSupportInput(session.projectDir,
                                                             mounted.path.to_std_string(),
                                                             mounted.fingerprint.to_std_string());
                if (registered.is_err()) return toolError("register container failed", &registered);
                return toolResult({{"action", action}, {"container", supportingResourceJson(mounted)}});
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
            if (session.containerRegistry.findByFingerprint(fingerprint) != nullptr)
                return toolError("supporting content is already attached");

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

            //容器只在此处导入一次：登记表与角色化容器栈共享同一字节视图。

            //工程登记（schema v2 role + parent = primary 登记 path）。
            const auto registered = registerRoleInput(session.projectDir,
                                                      containerId,
                                                      fingerprint,
                                                      *mountedRole == mjbase::model::ContainerRole::Library
                                                          ? mjbase::project::AnalysisInputRole::Library
                                                          : mjbase::project::AnalysisInputRole::Runtime,
                                                      current->sourcePath().to_std_string());
            if (registered.is_err()) return toolError("register container failed", &registered);

            const auto supportId = session.containerRegistry.nextSupportId();
            ProjectSession::MountedContainerRecord record;
            record.id          = containerId;
            record.role        = *mountedRole;
            record.path        = containerId;
            record.fingerprint = fingerprint;
            record.roleLabel   = std::string{mjbase::model::to_string(*mountedRole)};
            record.supportId   = supportId;
            record.container   = container;
            session.containerRegistry.records().push_back(std::move(record));

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
                                             {"support_id",
                                              supportId}}}};
            if (!shadowed.empty()) payload["shadowed"] = std::move(shadowed);
            return toolResult(std::move(payload));
        }
        const auto supportId = arguments.value("support_id", std::string{});
        if (supportId.empty()) return toolError("support_id is required");
        if (!mountedRole.has_value()) {
            auto result = current->removeSupporting(utf8Ref(supportId));
            if (result.is_err())
                { const auto err = shared::toCaStatus(result.unwrap_err()); return toolError("detach container failed", &err); }
            // SupportingResourceInfo 携 move-only Utf8String：unwrap 走移动
            // 重载，path 落 std::string（登记目录字节路径面）。
            const auto removed = std::move(result).unwrap();
            const auto deregistered =
                removeSupportInput(session.projectDir, removed.path.to_std_string());
            if (deregistered.is_err()) return toolError("deregister container failed", &deregistered);
            return toolResult({{"action", action}, {"removed", supportingResourceJson(removed)}});
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
        if (session.containerRegistry.findBySupportId(supportRefId) == nullptr)
            return toolError("detach container failed: support id not found");
        session.containerRegistry.detachBySupportId(supportRefId);
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
        //编辑族单入口：kind 解析为 EditCommand 变体（槽解析后），统一走
        // workspace::applyEdit（dryRun/expectedRevision/验证门/台账落账一条
        // 管道）。rename/mapping 的 dry_run 是分析预览（查询语义，非 validate），
        // 保持各自的预览 JSON 形状，不进编辑管道。
        const auto kind   = arguments.value("kind", std::string{});
        const bool dryRun = arguments.value("dry_run", false);
        auto request = mutationRequest(arguments);
        request.dryRun = dryRun || request.dryRun;

        if (kind == "jasm" || kind == "jasm_diff") {
            //name 槽：uri = Ref → 精确资源 URI；raw → 内部名核表后
            //原样透传（URI 契约零漂移）。diff 形态先取整类 Jasm 作 base 展开。
            std::string uri;
            Json        slotError;
            if (!slot::resolveClassUriArgument(*current, arguments, "uri", uri, slotError))
                return slotError;
            std::string source;
            if (kind == "jasm") {
                source = arguments.value("source", std::string{});
            }
            else {
                const auto diffText = arguments.value("diff", std::string{});
                auto jasm = workspace::getJasm(*current->workingContainer(), utf8Ref(uri));
                if (!jasm.isOk()) {
                    Json diagnostics = Json::array();
                    for (const auto& item : jasm.diagnostics)
                        diagnostics.push_back(diagnosticJson(item));
                    return toolResult({{"diagnostics", std::move(diagnostics)}}, true);
                }
                auto applied = applyUnifiedDiff(utf8(jasm.value->source.ref()), diffText);
                if (!applied.error.empty()) {
                    Json payload = {
                        {"status", statusJson(ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                                                             applied.error))},
                        {"failed_hunk", applied.failedHunk}};
                    return toolResult(std::move(payload), true);
                }
                source = std::move(applied.text);
            }
            EditCommand command;
            command.op = EditCommand::Jasm{
                ownUtf8(uri),
                utf16(arguments.value("source_name", uri + ".jasm")),
                utf16(source)};
            const auto result = applyEdit(*current, command, request);
            return toolResult(mutationJson(result), result.status.is_err());
        }
        if (kind == "method_jasm" || kind == "method_jasm_diff") {
            //name 槽：owner/name 同 method 三元组口径（desc 自表回填）。
            const auto slots = slot::loadWorkspaceShadowTable(*current);
            std::string owner;
            std::string mname;
            std::string descriptor;
            Json        slotError;
            if (!slot::resolveMethodSlots(slots, arguments, true, owner, mname, descriptor, slotError))
                return slotError;
            //1. 单方法 Jasm 是 diff 的 base 与拼接的定位段。
            auto method = workspace::getMethodJasm(*working, utf8Ref(owner), utf8Ref(mname), utf8Ref(descriptor));
            if (!method.isOk()) {
                Json diagnostics = Json::array();
                for (const auto& item : method.diagnostics)
                    diagnostics.push_back(diagnosticJson(item));
                return toolResult({{"diagnostics", std::move(diagnostics)}}, true);
            }
            std::string newMethodSource;
            if (kind == "method_jasm") {
                newMethodSource = arguments.value("source", std::string{});
            }
            else {
                auto applied = applyUnifiedDiff(utf8(method.value->source.ref()),
                                                arguments.value("diff", std::string{}));
                if (!applied.error.empty()) {
                    Json payload = {
                        {"status", statusJson(ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                                                             applied.error))},
                        {"failed_hunk", applied.failedHunk}};
                    return toolResult(std::move(payload), true);
                }
                newMethodSource = std::move(applied.text);
            }
            //2. 方法文本拼接进整类 Jasm（工作区共享预处理面）。
            auto payload = spliceMethodJasm(*current,
                                            utf8Ref(owner),
                                            utf8Ref(mname),
                                            utf8Ref(descriptor),
                                            utf8Ref(newMethodSource),
                                            utf8Ref(arguments.value("source_name", std::string{})));
            if (payload.is_err()) {
                const auto err = shared::toCaStatus(payload.unwrap_err());
                return toolError("splice method jasm failed", &err);
            }
            EditCommand command;
            command.op = std::move(payload).unwrap();
            const auto result = applyEdit(*current, command, request);
            return toolResult(mutationJson(result), result.status.is_err());
        }
        if (kind == "class_bytes") {
            std::vector<ca::u8> bytes;
            std::string parseError;
            if (!decodeBase64(arguments.value("content_base64", std::string{}), bytes, parseError))
                return toolError(parseError);
            std::string classUri;
            Json        slotError;
            if (!slot::resolveClassUriArgument(*current, arguments, "class_uri", classUri, slotError))
                return slotError;
            EditCommand command;
            command.op = EditCommand::ReplaceClassBytes{ownUtf8(classUri), std::move(bytes)};
            const auto result = applyEdit(*current, command, request);
            return toolResult(mutationJson(result), result.status.is_err());
        }
        if (kind == "structured") {
            //name 槽：class_uri = Ref → 精确资源 URI / raw → 核表后原样；
            //member_name = MemberRef → 真名（desc 与 member_descriptor 核对）。
            //解析值写回参数副本，parseMutation 及下游零改动。
            Json resolvedArgs = arguments;
            {
                std::string classUri;
                Json        slotError;
                if (!slot::resolveClassUriArgument(*current, arguments, "class_uri", classUri, slotError))
                    return slotError;
                resolvedArgs["class_uri"] = classUri;
                const auto slots = slot::loadWorkspaceShadowTable(*current);
                //结构化嵌套引用槽就地改写：member_descriptor（全 kind——add_field/
                //add_method 的新成员类型描述符内嵌类名也是引用语义）、
                //annotation_descriptor、exceptions、try_catch_blocks.type、
                //field_value 常量树、instruction 族的 owner/name/descriptor、
                //local_variables.descriptor（signature 泛型槽 v2 挂账不接）。
                if (!slot::resolveStructuredMutationRefs(slots, resolvedArgs, slotError))
                    return slotError;
                //member_name 只在「寻址既有成员」的 kind 上做 name 槽解析
                //（12 kind 全覆盖——5 访问/删除/换体 kind +
                // rename_local / replace_try_catch_blocks / insert|replace|remove_
                // instruction（findMethod 寻址）+ annotation 两 kind 在
                // annotation_target=field|method 时经 findField/findMethod 寻址）；
                //add_field/add_method 的 member_name 是新符号字面量（sidecar 先于
                //它存在），与 rename 的 new_name 同性质：不核表、不解析；
                //annotation_target=class 时 member 不参与寻址，同样不解析。
                //mutation_kind 槽：kind 已被 edit_apply 工具级选择器占用。
                const auto mutationKind = resolvedArgs.value("mutation_kind", std::string{});
                resolvedArgs["kind"]   = mutationKind;
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
                    const auto ownerInternal = mjt::internalNameOfUri(classUri);
                    //member_descriptor 已由结构化槽展开；展开值在此参与成员核对
                    //（handle 字面量不落下游）。
                    slot::SlotName memberSlot;
                    if (!slot::resolveMemberSlot(slots,
                                                 ownerInternal,
                                                 memberName,
                                                 resolvedArgs.value("member_descriptor", std::string{}),
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
            EditCommand command;
            command.op = EditCommand::ClassMutation{std::move(mutation)};
            const auto result = applyEdit(*current, command, request);
            return toolResult(mutationJson(result), result.status.is_err());
        }
        if (kind == "pipeline") {
            //opt_preview/opt_apply 的合并面（pass_run 随旧工具一并收敛）：
            //空 pipeline/passes 兜底 default-optimize；dry_run 即 preview。
            PipelineRequest pipeline;
            std::string     parseError;
            if (!parsePipelineRequest(arguments, pipeline, parseError)) return toolError(parseError);
            if (pipeline.pipeline.is_empty() && pipeline.passNames.empty())
                pipeline.pipeline = ca::str::Utf8String::from_cstr("default-optimize");
            EditCommand command;
            command.op = EditCommand::Pipeline{std::move(pipeline)};
            const auto result = applyEdit(*current, command, request);
            return toolResult(mutationJson(result), result.status.is_err());
        }
        if (kind == "rename") {
            //name 槽：kind=class → owner；kind=field/method →
            //owner+name+descriptor；kind=local → owner+method_name（name 是局部
            //变量名，非类/成员引用，不接线）。kind=package 的 owner 是包前缀
            //（非类引用），不核表直通；new_name 恒不碰（新名字面量）。
            //rename_kind 槽：kind 已被 edit_apply 工具级选择器占用。
            Json resolvedArgs = arguments;
            {
                const auto kind = resolvedArgs.value("rename_kind", std::string{});
                resolvedArgs["kind"] = kind;
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
                    const auto slots = slot::loadWorkspaceShadowTable(*current);
                    std::string member;
                    std::string descriptor;
                    if (!slot::resolveMethodSlots(slots, arguments, true, owner, member, descriptor, slotError))
                        return slotError;
                    resolvedArgs["owner"]      = owner;
                    resolvedArgs["name"]       = member;
                    resolvedArgs["descriptor"] = descriptor;
                }
                else if (kind == "local") {
                    //method_name 是成员槽；name/descriptor 保持调用方原值（局部
                    //变量名与其类型描述符不是类/成员引用）。
                    const auto slots = slot::loadWorkspaceShadowTable(*current);
                    const auto methodName = arguments.value("method_name", std::string{});
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
            if (request.dryRun) {
                //dry = 分析预览（查询语义）：映射表 + override 组件 + 影响。
                const auto analysis = current->analyzeRename(rename);
                Json mappings = Json::array();
                Json overrides = Json::array();
                //RenameAnalysis 文本字段换型 Utf8String：投影显式 to_std_string
                //（nlohmann 会把带 begin/end 的类型吃成码点数组，禁止隐式直通）。
                for (const auto& item : analysis.mappings)
                    mappings.push_back({{"kind", item.kind.to_std_string()},
                                        {"before", item.before.to_std_string()},
                                        {"after", item.after.to_std_string()}});
                for (const auto& item : analysis.overrideComponent)
                    overrides.push_back({{"owner", item.owner.to_std_string()},
                                         {"name", item.name.to_std_string()},
                                         {"descriptor", item.descriptor.to_std_string()},
                                         {"primary", item.primary},
                                         {"supporting", item.supporting},
                                         {"support_id", item.supportId.to_std_string()}});
                return toolResult(
                    {{"status", statusJson(shared::toCaStatus(analysis.status))},
                     {"ready_to_apply", analysis.readyToApply},
                     {"requires_override_review", analysis.requiresOverrideReview},
                     {"mappings", std::move(mappings)},
                     {"overrides", std::move(overrides)},
                     {"impact", impactJson(analysis.impact)}},
                    analysis.status.is_err());
            }
            EditCommand command;
            command.op = EditCommand::Rename{std::move(rename)};
            const auto result = applyEdit(*current, command, request);
            return toolResult(mutationJson(result), result.status.is_err());
        }
        if (kind == "mapping") {
            WorkspaceRemapRequest remap;
            remap.format      = arguments.value("format", std::string{});
            remap.mappingText = arguments.value("mapping_text", std::string{});
            remap.policy      = arguments.value("policy", std::string{"bytecode-only"});
            if (request.dryRun) {
                const auto analysis = analyzeWorkspaceRemap(*current, remap);
                return toolResult(impactJson(analysis), analysis.status.is_err());
            }
            //apply 走 plan 先行（单次解析）：applyWorkspaceRemap 的内部构建面在
            //此展开，避免二次解析 mapping 文本。
            mjbase::asm_remapper::MemberRemapPlan plan;
            mjbase::asm_remapper::RemapPolicy     policy;
            const auto buildStatus = buildWorkspaceRemapPlan(*current, remap, plan, policy, nullptr);
            if (buildStatus.is_err()) {
                mjt::workspace::MutationResult result;
                result.status     = buildStatus;
                result.revision   = current->revision();
                result.exportable = current->canExport();
                return toolResult(mutationJson(result), true);
            }
            EditCommand command;
            command.op = EditCommand::RemapPlan{&plan, std::move(policy)};
            //shadow 落分析库：身份迁移在管道内完成，identity 统计随 mutationJson 回显。
            const auto result = applyEdit(*current, command, request);
            return toolResult(mutationJson(result), result.status.is_err());
        }
        return toolError("unknown edit_apply kind: " + kind);
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
            const auto status = current->createSavepoint(utf8Ref(bookmarkName));
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
                                    ? current->restoreSavepoint(utf8Ref(arguments.value("name", std::string{})), request)
                                    : action == "undo" ? current->undo(request)
                                                       : current->redo(request);
            return toolResult(mutationJson(result), result.status.is_err());
        }
        if (action == "revert_class") {
            std::string classUri;
            Json        slotError;
            if (!slot::resolveClassUriArgument(*current, arguments, "class_uri", classUri, slotError))
                return slotError;
            const auto result = current->revertClass(utf8Ref(classUri), mutationRequest(arguments));
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
            // ModifiedClassInfo 携 move-only Utf8String：unwrap 走移动重载，
            // 投影显式 to_std_string（nlohmann 码点数组陷阱）。
            for (const auto& item : std::move(modified).unwrap()) {
            values.push_back({{"resource_uri", item.resourceUri.to_std_string()},
                              {"original_internal_name",
                               item.originalInternalName.to_std_string()},
                              {"current_internal_name",
                               item.currentInternalName.to_std_string()},
                              {"original_fingerprint", item.originalFingerprint.to_std_string()},
                              {"current_fingerprint", item.currentFingerprint.to_std_string()},
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
            auto info = workspace::classInfo(*working, utf8Ref(owner), current->lvtJarFallbackFlag());
            if (info.is_ok()) {
                // ClassInfo 携 move-only Utf8String：unwrap 走移动重载。
                const auto& clazz = std::move(info).unwrap();
                const auto addCandidate = [&](const ca::str::Utf8StringRef& candidate) {
                    const auto text = candidate.to_std_string();
                    if (std::find(descriptorCandidates.begin(),
                                  descriptorCandidates.end(),
                                  text) == descriptorCandidates.end())
                        descriptorCandidates.push_back(text);
                };
                for (const auto& method : clazz.methods)
                    if (method.name == member) addCandidate(method.descriptor.ref());
                for (const auto& field : clazz.fields)
                    if (field.name == member) addCandidate(field.descriptor.ref());
            }
        }
        //DB-first（#121 查询面换轨的 xref 步）：干净挂载态的 references 四分支
        //直查 analysis.db——开期整库导入与编辑增量同步（commit/undo/redo 挂
        //analysis_edit_sync）把 refs 表追平工作副本，库行与引擎扫描同源
        //（ReferenceScan 同一事实源），免每次查询全容器重扫。守卫三条件，
        //任一不满足回落原引擎路径：
        // (1) 会话分析库在线（非工程会话无库可查）；
        // (2) 无支撑容器挂载——库内 refs 只收主容器类的引用
        //     （analysis_ingest 按 primaryClasses 过滤入库），挂载态的跨容器
        //     命中与 supporting/supportId 标注必须由引擎的合并扫描面回答；
        // (3) method_callees resolved=true——解析展开走 jbc DOM+层次图，无 DB
        //     等价面；选择器不完整（raw 直通留空 owner/name/descriptor/class）
        //     同样回落，由引擎给出既有 canonical 校验错误，错误面零漂移。
        //行序：库行按 id 序返回，mjt 侧 finalizeReferenceRows（sortReferences
        //收口）重排为引擎全序，下游 role 过滤/descriptor 诊断/分页零改动。
        const bool resolvedExpansion =
            name == "method_callees" && arguments.value("resolved", false);
        const bool selectorsComplete =
            name == "xref_class" ? !className.empty()
                                 : (!owner.empty() && !member.empty() && !descriptor.empty());
        const bool referencesFromDb = session.analysisDb != nullptr && selectorsComplete &&
                                      !resolvedExpansion && current->supportingResources().empty();
        std::vector<mjt::workspace::ReferenceInfo> items;
        if (referencesFromDb) {
            auto rows = name == "xref_method"
                            ? session.analysisDb->referencesToMethod(utf8Ref(owner),
                                                                    utf8Ref(member),
                                                                    utf8Ref(descriptor))
                            : name == "xref_field"
                                  ? session.analysisDb->referencesToField(utf8Ref(owner),
                                                                          utf8Ref(member),
                                                                          utf8Ref(descriptor))
                                  : name == "xref_class"
                                        ? session.analysisDb->referencesToClass(utf8Ref(className))
                                        : session.analysisDb->referencesFromMember(
                                              utf8Ref(owner), utf8Ref(member), utf8Ref(descriptor));
            if (rows.is_err()) {
                const auto failure = std::move(rows).unwrap_err();
                const auto code    = failure.kind == mjbase::project::AnalysisDbErrorKind::Schema
                                        ? ca::StatusCode::FAILED_PRECONDITION
                                        : ca::StatusCode::INTERNAL;
                const auto status  = ca::ErrStatus(
                    code, "analysis db reference query failed: " + failure.message.to_std_string());
                return toolError("analysis db reference query failed", &status);
            }
            //行映射（引擎口径逐字对齐，workspace.cpp xrefsTo*/calleesOfMethod）：
            // - insnIndex 静态收窄 i64→i32（扫描面本就是 i32 序号）；
            // - kind 文本按分支恒定：to-method/to-field/from-method → "direct"，
            //   to-class → "type"（引擎 referenceInfo 的字面量）；
            // - target 文本：成员分支 = owner+"."+name+descriptor 拼接、类分支 =
            //   内部名本身（ReferenceInfo.target 的实际拼法）；
            // - supporting=false/supportId 空：守卫保证无支撑容器，标注本就是
            //   无操作（finalizeReferenceRows 内 annotate 空表直通）。
            //referencesToClass 命中 targetClass 的全部行（type/field/method），
            //类分支只取 Type 行；referencesFromMember 命中方法体发出的全部引
            //用（method/field/type），callees 分支只取 Method 行——与引擎
            //typeReferencesTo/methodReferencesFrom 的切片面一致。
            auto records = std::move(rows).unwrap();
            items.reserve(records.size());
            for (auto& row : records) {
                const bool kindRowWanted =
                    name == "xref_class"
                        ? row.kind == mjbase::project::DbReferenceKind::Type
                        : name == "method_callees"
                              ? row.kind == mjbase::project::DbReferenceKind::Method
                              : true;
                if (!kindRowWanted) continue;
                mjt::workspace::ReferenceInfo info;
                info.sourceClass      = std::move(row.sourceClass);
                info.sourceMethod     = std::move(row.sourceMethod);
                info.sourceDescriptor = std::move(row.sourceDescriptor);
                info.instructionIndex = static_cast<ca::i32>(row.insnIndex);
                if (name == "xref_class") {
                    info.target = std::move(row.targetClass);
                    info.kind   = ca::str::Utf8String::from_cstr("type");
                }
                else {
                    const std::string target = row.targetClass.to_std_string() + "." +
                                               row.targetName.to_std_string() +
                                               row.targetDescriptor.to_std_string();
                    info.target = ca::str::Utf8String::from_data_unchecked(
                        reinterpret_cast<const ca::u8*>(target.data()), target.size());
                    info.kind = ca::str::Utf8String::from_cstr("direct");
                }
                items.push_back(std::move(info));
            }
            session.workspace->finalizeReferenceRows(items);
        }
        else {
            //显式 if-else 分派替代原 3 层嵌套三元，避免重复读取 arguments 与拷贝；
            //用 lambda 单次构造返回。
            auto query = [&]() -> mjt::workspace::WorkspaceResult<
                                 std::vector<mjt::workspace::ReferenceInfo>> {
                if (name == "xref_method") {
                    return current->xrefsToMethod(utf8Ref(owner), utf8Ref(member),
                                                  utf8Ref(descriptor));
                }
                if (name == "xref_field") {
                    return current->xrefsToField(utf8Ref(owner), utf8Ref(member),
                                                 utf8Ref(descriptor));
                }
                if (name == "xref_class") {
                    return current->xrefsToClass(utf8Ref(className));
                }
                return current->calleesOfMethod(utf8Ref(owner), utf8Ref(member),
                                                utf8Ref(descriptor),
                                                arguments.value("resolved", false));
            };
            auto references = query();
            if (references.is_err()) {
                const auto err = shared::toCaStatus(references.unwrap_err());
                return toolError("reference query failed", &err);
            }
            items = std::move(references).unwrap();
        }

        //role 过滤（RFC 0001 P5）：站点归属按「primary = 非支撑站点；支撑站
        //点 = workspace 在册 id → 挂载栈角色对号」判定，未挂载的支撑站点归
        //support（遗留角色）。显式过滤值之外的请求在 schema 已被拒绝。
        const auto roleFilter = arguments.value("role", std::string{});
        if (!roleFilter.empty() && roleFilter != "primary" && roleFilter != "support") {
            const auto wanted = roleFilter == "library" ? mjbase::model::ContainerRole::Library
                                                        : mjbase::model::ContainerRole::Runtime;
            const auto inventory = mountedClassInventory(session, wanted);
            std::unordered_set<std::string> roleSupportIds;
            for (const auto& record : session.containerRegistry.records())
                if (record.role == wanted) roleSupportIds.insert(record.supportId);
            items.erase(std::remove_if(items.begin(),
                                       items.end(),
                                       [&](const mjt::workspace::ReferenceInfo& item) {
                                           //保留 = 站点在本角色容器内；其余全部剔除。
                                           //DTO 文本字段换型 Utf8String：键比对显式
                                           //to_std_string。
                                           if (!item.supporting) return true;
                                           if (roleSupportIds.count(item.supportId.to_std_string()) == 0)
                                               return true;
                                           return inventory.find(item.sourceClass.to_std_string()) ==
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
                                           return findMountedContainer(
                                                      session, item.supportId.to_std_string()) ==
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
        // DB-first（与 xref 分支同款守卫）：支撑容器只登记输入行、事实未入
        // 库——有挂载即回退引擎扫描，行序/来源标注零漂移。
        const bool relationsFromDb =
            session.analysisDb != nullptr && current->supportingResources().empty();
        mjt::workspace::WorkspaceResult<mjt::workspace::ClassRelations> value =
            ca::core::Err(mjt::workspace::WorkspaceErrorInfo{});
        if (relationsFromDb) {
            mjbase::project::AnalysisDb& db = *session.analysisDb;
            const auto dbFailure = [&](mjbase::project::AnalysisDbError&& failure) {
                const auto code =
                    failure.kind == mjbase::project::AnalysisDbErrorKind::Schema
                        ? ca::StatusCode::FAILED_PRECONDITION
                        : ca::StatusCode::INTERNAL;
                const auto status =
                    ca::ErrStatus(code, "analysis db class relations query failed: " +
                                            failure.message.to_std_string());
                return toolError("analysis db class relations query failed", &status);
            };
            // 起点门：未登记 → NotFound（引擎 findClass 无效同形）。
            auto found = db.findClass(utf8Ref(className));
            if (found.is_err()) return dbFailure(std::move(found).unwrap_err());
            auto indexRow = std::move(found).unwrap();
            if (!indexRow.has_value()) {
                const auto err = shared::toCaStatus(mjt::workspace::WorkspaceErrorInfo{
                    mjt::workspace::WorkspaceError::NotFound,
                    "get class relations: " + className});
                return toolError("get class relations failed", &err);
            }
            mjt::workspace::ClassRelations relations;
            // super 链：含自身、未登记父类计入链尾（引擎合成 java/lang/Object
            // 链尾同形）；superclasses = 链去自身。
            auto chain = db.superChain(utf8Ref(className));
            if (chain.is_err()) return dbFailure(std::move(chain).unwrap_err());
            for (auto& ancestor : std::move(chain).unwrap()) {
                if (ancestor.to_std_string() != className)
                    relations.superclasses.push_back(std::move(ancestor));
            }
            // 接口闭包：super 链（含自身）各类的直接接口 + 各接口自身超链
            // （传递超接口）；剔自身、剔非接口行（引擎 interfaceClosure∩
            // interface − 自身 语义）。
            const auto interfaceBit = [&](const std::string& class_) {
                auto row = db.findClass(utf8Ref(class_));
                if (row.is_err()) return -1;
                auto resolved = std::move(row).unwrap();
                return resolved.has_value() ? resolved->access : -1;
            };
            // 接口的父接口在 interfaces[] 列表里（classfile 语义），传递闭包
            // 沿 interfaces[] BFS——superChain 只走 superName（extends 链），
            // 不覆盖 interface extends。
            std::set<std::string> interfaceSet;
            std::function<void(const std::string&)> addInterfaceClosure =
                [&](const std::string& iface) {
                    if (!interfaceSet.insert(iface).second) return;
                    auto row = db.findClass(utf8Ref(iface));
                    if (row.is_err()) return;
                    auto resolved = std::move(row).unwrap();
                    if (!resolved.has_value()) return;
                    for (const auto& up : resolved->interfaces)
                        addInterfaceClosure(up.to_std_string());
                };
            const auto addDirectInterfaces = [&](const ca::str::Utf8StringRef class_) {
                auto row = db.findClass(class_);
                if (row.is_err()) return dbFailure(std::move(row).unwrap_err());
                auto resolved = std::move(row).unwrap();
                if (resolved.has_value()) {
                    for (const auto& iface : resolved->interfaces)
                        addInterfaceClosure(iface.to_std_string());
                }
                return Json{};
            };
            for (const auto& ancestor : relations.superclasses) {
                const auto status = addDirectInterfaces(ancestor.ref());
                if (!status.is_null()) return status;
            }
            {
                const auto status = addDirectInterfaces(utf8Ref(className));
                if (!status.is_null()) return status;
            }
            for (const auto& iface : interfaceSet) {
                if (iface == className) continue;
                const auto access = interfaceBit(iface);
                if (access >= 0 && (access & 0x0200) != 0)
                    relations.interfaces.push_back(ownUtf8(iface));
            }
            // 直接子类（extends 边）。
            auto direct = db.directSubclasses(utf8Ref(className));
            if (direct.is_err()) return dbFailure(std::move(direct).unwrap_err());
            relations.directSubclasses = std::move(direct).unwrap();
            // 后代分桶（引擎语义）：接口起点 → 实现者 = 非接口子型；类起点 →
            // 全部后代，实现者空。实现者不走库 implementors 面——其递归 SQL
            // 向上收起点的父接口（只实现 IFoo 的类会泄进 IBar 的实现者，与
            // 引擎 isSubtype 严格向下语义相悖），这里取 listClasses 快照在
            // 内存组合严格语义：C 实现 X ⟺ C 非接口 ∧ X ∈ 闭(C)，闭(C) =
            // C 的 super 链各类直接接口及其传递超接口的并。
            if ((indexRow->access & 0x0200) != 0) {
                auto snapshot = db.listClasses();
                if (snapshot.is_err()) return dbFailure(std::move(snapshot).unwrap_err());
                const auto rows = std::move(snapshot).unwrap();
                // 反向可达：C 实现 X ⟺ C 非接口 ∧ X ∈ 闭(C)（闭(C) = 自身与
                // 祖先链各类直接接口及其传递超接口的并）。从起点接口反向走
                // implements / 子接口 / 子类继承三类前驱边只触结果子图——
                // 正向逐类计算闭包在大库（万级实现者接口）是 O(n·闭包) 放大。
                std::unordered_map<std::string, std::vector<const mjbase::project::DbClassInfo*>>
                    implementersOf;
                std::unordered_map<std::string, std::vector<const mjbase::project::DbClassInfo*>>
                    subclassesOf;
                implementersOf.reserve(rows.size() * 2);
                subclassesOf.reserve(rows.size() * 2);
                for (const auto& row : rows) {
                    for (const auto& iface : row.interfaces)
                        implementersOf[iface.to_std_string()].push_back(&row);
                    if (!row.superName.is_empty())
                        subclassesOf[row.superName.to_std_string()].push_back(&row);
                }
                // BFS 节点 = （待下探的接口 | 已计入实现者的类）。
                std::vector<std::string> implementors;
                std::unordered_set<std::string> visited{className};
                std::vector<std::pair<std::string, bool>> frontier{{className, false}};
                while (!frontier.empty()) {
                    const auto [name, isClass] = frontier.back();
                    frontier.pop_back();
                    const auto& index = isClass ? subclassesOf : implementersOf;
                    const auto found = index.find(name);
                    if (found == index.end()) continue;
                    for (const auto* row : found->second) {
                        // 接口行的 superName（java/lang/Object）不构成类继承边。
                        if (isClass && (row->access & 0x0200) != 0) continue;
                        auto rowName = row->internalName.to_std_string();
                        if (!visited.insert(rowName).second) continue;
                        if ((row->access & 0x0200) != 0)
                            frontier.emplace_back(std::move(rowName), false);
                        else {
                            implementors.push_back(rowName);
                            frontier.emplace_back(std::move(rowName), true);
                        }
                    }
                }
                // 行序 = listClasses 的 internal_name 升序（引擎侧
                // registrationOrder 扫描后 sortUnique 同形）。
                std::sort(implementors.begin(), implementors.end());
                for (const auto& name : implementors)
                    relations.implementors.push_back(ownUtf8(name));
            }
            else {
                auto all = db.allSubclasses(utf8Ref(className));
                if (all.is_err()) return dbFailure(std::move(all).unwrap_err());
                relations.allSubclasses = std::move(all).unwrap();
            }
            // 来源行：关系名全序（引擎 std::set 同形）；守卫保证无支撑挂载，
            // supporting 恒 false / supportId 恒空。
            std::set<std::string> related{className};
            const auto collect = [&related](const std::vector<ca::str::Utf8String>& names) {
                for (const auto& item : names) related.insert(item.to_std_string());
            };
            collect(relations.superclasses);
            collect(relations.interfaces);
            collect(relations.directSubclasses);
            collect(relations.allSubclasses);
            collect(relations.implementors);
            for (const auto& relatedName : related)
                relations.sources.push_back({ownUtf8(relatedName), false, {}});
            value = ca::core::Ok(std::move(relations));
        }
        else {
            value = current->classRelations(utf8Ref(className));
        }
        if (value.is_err()) {
            const auto err = shared::toCaStatus(value.unwrap_err());
            return toolError("get class relations failed", &err);
        }
        // ClassRelations 携 move-only Utf8String：array 收参与投影均显式
        // to_std_string（nlohmann 会把带 begin/end 的类型吃成码点数组）。
        const auto array = [](const std::vector<ca::str::Utf8String>& source) {
            Json result = Json::array();
            for (const auto& item : source) result.push_back(item.to_std_string());
            return result;
        };
        const auto& relations = std::move(value).unwrap();
        Json sources = Json::array();
        for (const auto& source : relations.sources) {
            sources.push_back({{"class", source.internalName.to_std_string()},
                               {"supporting", source.supporting},
                               {"support_id", source.supportId.to_std_string()}});
        }
        return toolResult({{"superclasses", array(relations.superclasses)},
                           {"interfaces", array(relations.interfaces)},
                           {"direct_subclasses", array(relations.directSubclasses)},
                           {"all_subclasses", array(relations.allSubclasses)},
                           {"implementors", array(relations.implementors)},
                           {"sources", std::move(sources)}});
    }
    if (name == "method_overrides") {
        //引擎保留面（W3 裁决）：覆盖组件要并查集 + 子型闭包 + supporting
        //标注，analysis.db 无等价面，组合重实现的成本高于收益——与
        //searchInstructions/xref 支撑回退同为活扫描面，不迁 analysis_ops。
        const auto slots = slot::loadWorkspaceShadowTable(*current);
        std::string owner;
        std::string member;
        std::string descriptor;
        Json        slotError;
        if (!slot::resolveMethodSlots(slots, arguments, true, owner, member, descriptor, slotError))
            return slotError;
        auto value = current->overridesOfMethod(utf8Ref(owner), utf8Ref(member), utf8Ref(descriptor));
        if (value.is_err()) {
            const auto err = shared::toCaStatus(value.unwrap_err());
            return toolError("get method overrides failed", &err);
        }
        Json values = Json::array();
        // MethodOverrideInfo 携 move-only Utf8String：unwrap 走移动重载，投影
        // 显式 to_std_string（nlohmann 码点数组陷阱）。
        for (const auto& item : std::move(value).unwrap())
            values.push_back({{"owner", item.owner.to_std_string()},
                              {"name", item.name.to_std_string()},
                              {"descriptor", item.descriptor.to_std_string()},
                              {"primary", item.primary},
                              {"supporting", item.supporting},
                              {"support_id", item.supportId.to_std_string()}});
        return toolResult({{"overrides", std::move(values)}});
    }
    if (name == "project_export") {
        //导出前 flush 挂点：台账与分析缓存与 close 同点落盘（同一崩溃窗语义）。
        const auto cacheFlushed = flushAnalysisCache(session);
        if (cacheFlushed.is_err()) return toolError("flush analysis cache failed", &cacheFlushed);
        const auto status = current->exportArtifact(utf8Ref(arguments.value("path", std::string{})));
        if (status.is_err()) {
            const auto err = shared::toCaStatus(status.unwrap_err());
            return toolError("export project failed", &err);
        }
        return toolResult({{"exported", true}, {"revision", current->revision()}});
    }
    return std::nullopt;
}

}   //namespace mjt::mcp
