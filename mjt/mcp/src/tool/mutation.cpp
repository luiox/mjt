#include "service.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "diff.hpp"
#include "json.hpp"
#include "parser.hpp"
#include "shared/jar_remapper.hpp"
#include "shared/shadow_display.hpp"
#include "tool/shadow_slot.hpp"

#include "mjbase/mapping/shadow_alias_format.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/renamer/shadow_alias.hpp"

namespace mjt::mcp {

using namespace detail;
using mjt::base::workspace::ClassMutation;
using mjt::base::workspace::JarRepairRequest;
using mjt::base::workspace::PipelineRequest;
using mjt::base::workspace::RenameRequest;
using mjt::base::workspace::IWorkspace;

std::optional<Json> McpToolService::callMutationTool(
    const std::string& name, const Json& arguments, IWorkspace* current)
{
    if (name == "pass_run" || name == "opt_preview" || name == "opt_apply") {
        PipelineRequest pipeline;
        std::string     parseError;
        if (!parsePipelineRequest(arguments, pipeline, parseError)) return toolError(parseError);
        if (name != "pass_run" && pipeline.pipeline.empty() && pipeline.passNames.empty())
            pipeline.pipeline = "default-optimize";
        auto request = mutationRequest(arguments);
        if (name == "opt_preview") request.dryRun = true;
        const auto result = current->applyPipeline(pipeline, request);
        return toolResult(mutationJson(result), result.status.is_err());
    }
    if (name == "jar_diff") {
        const auto result = current->diffJar(arguments.value("path", std::string{}));
        if (result.status.is_err()) return toolError("diff JAR failed", &result.status);
        Json values = Json::array();
        for (const auto& diff : result.byteDiffs) {
            Json ranges = Json::array();
            for (const auto& range : diff.ranges) {
                ranges.push_back({{"offset", range.offset},
                                  {"before_length", range.beforeLength},
                                  {"after_length", range.afterLength}});
            }
            values.push_back({{"uri", diff.uri},
                              {"before_size", diff.beforeSize},
                              {"after_size", diff.afterSize},
                              {"ranges", std::move(ranges)}});
        }
        auto payload = page(std::move(values), arguments);
        payload["reference_path"] = result.referencePath;
        payload["workspace_only"] = result.workspaceOnly;
        payload["reference_only"] = result.referenceOnly;
        payload["modified"] = result.modified;
        return toolResult(std::move(payload));
    }
    if (name == "jar_fix_analyze" || name == "jar_fix_apply") {
        JarRepairRequest repair;
        std::string      parseError;
        if (!parseJarRepairRequest(arguments, repair, parseError)) return toolError(parseError);
        auto request = mutationRequest(arguments);
        if (name == "jar_fix_analyze") request.dryRun = true;
        const auto result = name == "jar_fix_analyze"
                                ? current->analyzeJarRepair(repair, request)
                                : current->applyJarRepair(repair, request);
        return toolResult(jarRepairJson(result), result.mutation.status.is_err());
    }
    if (name == "xref_method" || name == "method_callers" || name == "xref_field" || name == "xref_class" ||
        name == "method_callees") {
        //T2 name 槽：xref/callees 族 raw **直通**（verifyRaw=false——
        //表外合法目标如 java/lang/Object 的引用查询零漂移），Ref 形状照常
        //全量解析（unknown-handle 等四分类不受直通位影响）。
        const auto slots = slot::loadWorkspaceShadowTable(*current);
        std::string owner;
        std::string member;
        std::string descriptor;
        Json        slotError;
        if (!slot::resolveMethodSlots(slots, arguments, false, owner, member, descriptor, slotError))
            return slotError;
        std::string className;
        if (name == "xref_class") {
            Json classError;
            if (!slot::resolveClassArgument(*current, arguments, "class", false, className, classError))
                return classError;
        }
        //显式 if-else 分派替代原 3 层嵌套三元，避免重复读取 arguments 与拷贝；
        //StatusOr 不可拷贝赋值，用 lambda 单次构造返回。
        auto query = [&]() -> ca::StatusOr<std::vector<mjt::base::workspace::ReferenceInfo>> {
            if (name == "xref_method" || name == "method_callers") {
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
        if (references.is_err()) { const auto err = references.unwrap_err(); return toolError("reference query failed", &err); }
        Json values = Json::array();
        for (const auto& item : references.unwrap()) values.push_back(referenceJson(item));
        return toolResult(page(std::move(values), arguments));
    }
    if (name == "class_relations") {
        std::string className;
        Json        slotError;
        if (!slot::resolveClassArgument(*current, arguments, "class", true, className, slotError))
            return slotError;
        auto value = current->classRelations(className);
        if (value.is_err()) { const auto err = value.unwrap_err(); return toolError("get class relations failed", &err); }
        const auto array = [](const std::vector<std::string>& source) {
            Json result = Json::array();
            for (const auto& item : source) result.push_back(item);
            return result;
        };
        const auto& relations = value.unwrap();
        Json sources = Json::array();
        for (const auto& source : relations.sources) {
            sources.push_back({{"internal_name", source.internalName},
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
        if (value.is_err()) { const auto err = value.unwrap_err(); return toolError("get method overrides failed", &err); }
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
        //T2 name 槽：uri = Ref → 精确资源 URI；raw → 内部名核表后
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
        //T2 name 槽：owner/name 同 method 三元组口径（desc 自表回填），
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
        if (classInfo.is_err())
            { const auto err = classInfo.unwrap_err(); return toolError("select class for method jasm failed", &err); }
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
        //T2 name 槽：class_uri = Ref → 精确资源 URI / raw → 核表后
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
            //（T2 评审 N1 处方：12 kind 全覆盖——5 访问/删除/换体 kind +
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
                slot::SlotName memberSlot;
                if (!slot::resolveMemberSlot(slots,
                                             ownerInternal,
                                             memberName,
                                             arguments.value("member_descriptor", std::string{}),
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
        //T2 name 槽：kind=class → owner；kind=field/method →
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
        return toolResult({{"status", statusJson(analysis.status)},
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
    if (name == "workspace_savepoint") {
        const auto status = current->createSavepoint(arguments.value("name", std::string{}));
        if (status.is_err()) return toolError("create savepoint failed", &status);
        return toolResult({{"revision", current->revision()}, {"created", true}});
    }
    if (name == "workspace_restore" || name == "workspace_revert_class" ||
        name == "workspace_undo" || name == "workspace_redo") {
        const auto request = mutationRequest(arguments);
        std::string classUri;
        if (name == "workspace_revert_class") {
            Json slotError;
            if (!slot::resolveClassUriArgument(*current, arguments, "class_uri", classUri, slotError))
                return slotError;
        }
        const auto result = name == "workspace_restore"
                                ? current->restoreSavepoint(arguments.value("name", std::string{}), request)
                                : name == "workspace_revert_class"
                                      ? current->revertClass(classUri, request)
                                      : name == "workspace_undo" ? current->undo(request) : current->redo(request);
        return toolResult(mutationJson(result), result.status.is_err());
    }
    if (name == "workspace_export") {
        const auto status = current->exportJar(arguments.value("path", std::string{}));
        if (status.is_err()) return toolError("export workspace failed", &status);
        return toolResult({{"exported", true}, {"revision", current->revision()}});
    }
    if (name == "shadow_generate" || name == "shadow_prune") {
        //shadow sidecar 管理：与 `mjt shadow` 同一数据面（mbase 生成器/
        //prune + Shadow-Alias 格式），只写 sidecar 文件、永不改 jar。缺省路径
        //`<jar>.mjt-shadow`，`file` 显式覆盖但不得指向 jar 本身。sidecar 是
        //mjt 自有产物，写出按覆盖语义（重跑即更新），jar 不存在直接报错。
        namespace fs = std::filesystem;
        const auto jarPath = current->sourcePath();
        const auto fileArg = arguments.value("file", std::string{});
        const auto sidecarPath = mjt::resolveShadowPath(jarPath, fileArg);
        if (!fileArg.empty() && sidecarPath == jarPath) {
            return toolError("shadow sidecar path must not be the workspace JAR itself: " +
                             sidecarPath);
        }
        std::error_code fsError;
        if (!fs::exists(jarPath, fsError)) {
            return toolError("workspace JAR not found: " + jarPath);
        }
        mjbase::model::ResourceContainer container;
        //mjbase 侧导入错误走 ModelResult 值语义，不再 throw。
        auto imported = mjbase::model::ResourceHelper::importFromJar(container, jarPath);
        if (imported.is_err()) {
            return toolError(std::string("import workspace JAR failed: ") +
                             mjbase::model::to_string(std::move(imported).unwrap_err()));
        }
        const auto utf8Path = [](const std::string& value) {
            return ca::str::Utf8StringRef::from_data(
                reinterpret_cast<const ca::u8*>(value.data()), value.size());
        };

        mjbase::renamer::ShadowAliasTable table;
        const bool sidecarExists = fs::exists(sidecarPath, fsError);
        if (name == "shadow_prune" && !sidecarExists) {
            return toolError("shadow sidecar not found: " + sidecarPath +
                             " (generate it with `shadow_generate` or `mjt shadow -i <jar>`)");
        }
        if (sidecarExists) {
            auto existing = mjbase::mapping::readShadowAliasFile(utf8Path(sidecarPath));
            if (existing.is_err()) {
                return toolError("read shadow sidecar failed: " +
                                 std::move(existing).unwrap_err().message.to_std_string());
            }
            table = mjbase::renamer::ShadowAliasTable::fromMapping(std::move(existing).unwrap());
        }

        if (name == "shadow_generate") {
            //mjbase 侧补号（backfillTokens）随机源失败走 Err，不再抛异常——
            //已分配的编号仍留在表里，重跑幂等补号。
            auto generated = mjbase::renamer::generateMechanicalShadowNames(container, table);
            if (generated.is_err()) {
                return toolError(std::string("generate shadow names failed: ") +
                                 std::move(generated).unwrap_err());
            }
            const auto stats = std::move(generated).unwrap();
            auto written =
                mjbase::mapping::writeShadowAliasFile(utf8Path(sidecarPath), table.toMapping());
            if (written.is_err()) {
                return toolError("write shadow sidecar failed: " +
                                 std::move(written).unwrap_err().message.to_std_string());
            }
            return toolResult({{"file", sidecarPath},
                               {"assignedClasses", stats.assignedClasses},
                               {"assignedMethods", stats.assignedMethods},
                               {"assignedFields", stats.assignedFields},
                               {"keptEntries", stats.keptEntries},
                               {"totalEntries", table.size()}});
        }

        const auto removed = mjbase::renamer::pruneShadowAliases(container, table);
        if (removed > 0) {
            auto written =
                mjbase::mapping::writeShadowAliasFile(utf8Path(sidecarPath), table.toMapping());
            if (written.is_err()) {
                return toolError("write shadow sidecar failed: " +
                                 std::move(written).unwrap_err().message.to_std_string());
            }
        }
        return toolResult(
            {{"file", sidecarPath}, {"removed", removed}, {"totalEntries", table.size()}});
    }
    return std::nullopt;
}

}   //namespace mjt::mcp
