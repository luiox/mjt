#include "service.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "json.hpp"
#include "shared/status_convert.hpp"
#include "limits.hpp"
#include "parser.hpp"
#include "shared/shadow_display.hpp"
#include "tool/shadow_slot.hpp"
#include "mjbase/mapping/shadow_alias_format.hpp"
#include "mjbase/renamer/shadow_alias.hpp"

namespace mjt::mcp {

using namespace detail;
using mjbase::workspace::SearchHit;
using mjbase::workspace::SearchOptions;
using mjbase::workspace::IWorkspace;

std::optional<Json> McpToolService::callWorkspaceQueryTool(
    const std::string& name, const Json& arguments, IWorkspace* current)
{
    if (name == "workspace_info") {
        Json support = Json::array();
        for (const auto& item : current->supportingResources())
            support.push_back(supportingResourceJson(item));
        return toolResult({{"workspace_id", arguments.at("workspace_id")},
                           {"source_path", current->sourcePath()},
                           {"revision", current->revision()},
                           {"dirty", current->isDirty()},
                           {"exportable", current->canExport()},
                           {"supporting_resources", std::move(support)}});
    }
    if (name == "workspace_modified_classes") {
        auto modified = current->modifiedClasses();
        if (modified.is_err())
            { const auto err = shared::toCaStatus(modified.unwrap_err()); return toolError("list modified classes failed", &err); }
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
        return toolResult(page(std::move(values), arguments));
    }
    if (name == "workspace_add_support") {
        auto result = current->addSupportingJar(arguments.value("path", std::string{}));
        if (result.is_err()) { const auto err = shared::toCaStatus(result.unwrap_err()); return toolError("add supporting resource failed", &err); }
        return toolResult({{"supporting_resource", supportingResourceJson(result.unwrap())}});
    }
    if (name == "workspace_list_support") {
        Json values = Json::array();
        for (const auto& item : current->supportingResources())
            values.push_back(supportingResourceJson(item));
        return toolResult(page(std::move(values), arguments));
    }
    if (name == "workspace_remove_support") {
        auto result = current->removeSupportingJar(arguments.value("support_id", std::string{}));
        if (result.is_err()) { const auto err = shared::toCaStatus(result.unwrap_err()); return toolError("remove supporting resource failed", &err); }
        return toolResult({{"removed", supportingResourceJson(result.unwrap())}});
    }
    if (name == "workspace_resources") {
        auto resources = current->listResources(arguments.value("include_classes", true));
        if (resources.is_err()) { const auto err = shared::toCaStatus(resources.unwrap_err()); return toolError("list resources failed", &err); }
        Json values = Json::array();
        for (const auto& item : resources.unwrap())
            values.push_back({{"uri", item.uri}, {"type", item.type}, {"size", item.size}});
        return toolResult(page(std::move(values), arguments));
    }
    if (name == "resource_read") {
        auto content = current->readResource(arguments.value("uri", std::string{}));
        if (content.is_err()) { const auto err = shared::toCaStatus(content.unwrap_err()); return toolError("read resource failed", &err); }
        auto value = std::move(content).unwrap();
        auto offset = arguments.value("offset", static_cast<ca::usize>(0));
        if (!parseCursor(arguments, value.bytes.size(), offset)) {
            return toolError("cursor must be an unsigned byte offset");
        }
        const auto requested = arguments.value("limit", kMaximumResourceChunkSize);
        if (requested == 0) return toolError("limit must be greater than zero");
        const auto limit = std::min(requested, kMaximumResourceChunkSize);
        offset = std::min(offset, value.bytes.size());
        const auto end = std::min(value.bytes.size(), offset + limit);
        std::vector<ca::u8> chunk(value.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                  value.bytes.begin() + static_cast<std::ptrdiff_t>(end));
        Json result = {{"uri", value.summary.uri},
                       {"type", value.summary.type},
                       {"size", value.summary.size},
                       {"offset", offset},
                       {"encoding", "base64"},
                       {"content", base64(chunk)}};
        if (end < value.bytes.size()) {
            //N3 字节分页侧车：与列表分页（page/classSummaryPage 的 P2-3 契约）
            //同口径五键 {partial, reason:"truncated", received, total, cursor}——
            //received=本页实际字节数，total=资源总字节数，cursor=下一页字节偏移
            //（与 next_cursor 同值）。仅截断路径写入，完整页零变化。
            const auto cursor = std::to_string(end);
            result["next_cursor"] = cursor;
            result["partial"]     = true;
            result["reason"]      = "truncated";
            result["received"]    = chunk.size();
            result["total"]       = value.bytes.size();
            result["cursor"]      = cursor;
        }
        return toolResult(std::move(result));
    }
    if (name == "resource_manifest") {
        auto attributes = current->manifestAttributes();
        if (attributes.is_err()) { const auto err = shared::toCaStatus(attributes.unwrap_err()); return toolError("read manifest failed", &err); }
        Json values = Json::array();
        for (const auto& item : attributes.unwrap()) values.push_back({{"key", item.key}, {"value", item.value}});
        return toolResult({{"attributes", std::move(values)}});
    }
    if (name == "resource_strings") {
        auto strings = current->resourceStrings(arguments.value("minimum_length", static_cast<ca::usize>(4)));
        if (strings.is_err()) { const auto err = shared::toCaStatus(strings.unwrap_err()); return toolError("extract resource strings failed", &err); }
        Json values = Json::array();
        for (const auto& item : strings.unwrap())
            values.push_back({{"uri", item.uri}, {"offset", item.offset}, {"value", item.value}});
        return toolResult(page(std::move(values), arguments));
    }
    if (name == "search_classes" || name == "search_members" || name == "search_strings" ||
        name == "search_numbers" || name == "search_instructions") {
        const auto query = arguments.value("query", std::string{});
        SearchOptions options;
        std::string parseError;
        if (!parseSearchOptions(arguments, options, parseError)) return toolError(parseError);
        auto hits =
            name == "search_classes"
                ? current->searchClasses(query, options)
                : name == "search_members"
                      ? current->searchMembers(query, options)
                      : name == "search_strings"
                            ? current->searchStrings(query, options)
                            : name == "search_numbers" ? current->searchNumbers(query, options)
                                                       : current->searchInstructions(query, options);
        if (hits.is_err()) { const auto err = shared::toCaStatus(hits.unwrap_err()); return toolError("workspace search failed", &err); }
        Json values = Json::array();
        for (const auto& hit : hits.unwrap()) values.push_back(searchHitJson(hit));
        return toolResult(page(std::move(values), arguments));
    }
    if (name == "class_list") {
        auto classes = current->listClassSummaries();
        if (classes.is_err()) { const auto err = shared::toCaStatus(classes.unwrap_err()); return toolError("list classes failed", &err); }
        return toolResult(classSummaryPage(classes.unwrap(), arguments));
    }
    if (name == "class_get") {
        //T2 name 槽：class 整串 = Ref（${Class_N.token8}）或 raw 二选一；
        //sidecar 活跃时 raw 缺表 → raw-not-found（§6 四分类），Ref 展开失败按
        //四分类报错；无 sidecar 时 raw 零漂移（空表恒等直通）。
        std::string className;
        Json        slotError;
        if (!slot::resolveClassArgument(*current, arguments, "class", true, className, slotError))
            return slotError;
        auto value = current->classInfo(className);
        if (value.is_err()) { const auto err = shared::toCaStatus(value.unwrap_err()); return toolError("get class failed", &err); }
        return toolResult(classJson(value.unwrap()));
    }
    if (name == "class_bytes") {
        std::string className;
        Json        slotError;
        if (!slot::resolveClassArgument(*current, arguments, "class", true, className, slotError))
            return slotError;
        auto content = current->readClassBytes(className);
        if (content.is_err()) { const auto err = shared::toCaStatus(content.unwrap_err()); return toolError("read class bytes failed", &err); }
        auto value = std::move(content).unwrap();
        auto offset = arguments.value("offset", static_cast<ca::usize>(0));
        if (!parseCursor(arguments, value.bytes.size(), offset)) {
            return toolError("cursor must be an unsigned byte offset");
        }
        const auto requested = arguments.value("limit", kMaximumResourceChunkSize);
        if (requested == 0) return toolError("limit must be greater than zero");
        const auto limit = std::min(requested, kMaximumResourceChunkSize);
        offset = std::min(offset, value.bytes.size());
        const auto end = std::min(value.bytes.size(), offset + limit);
        std::vector<ca::u8> chunk(value.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                  value.bytes.begin() + static_cast<std::ptrdiff_t>(end));
        Json result = {{"resource_uri", value.resourceUri},
                       {"internal_name", value.internalName},
                       {"fingerprint", value.fingerprint},
                       {"size", value.bytes.size()},
                       {"offset", offset},
                       {"encoding", "base64"},
                       {"content", base64(chunk)}};
        if (end < value.bytes.size()) {
            //同 resource_read：N3 字节分页侧车五键，仅截断路径写入，完整页零变化。
            const auto cursor = std::to_string(end);
            result["next_cursor"] = cursor;
            result["partial"]     = true;
            result["reason"]      = "truncated";
            result["received"]    = chunk.size();
            result["total"]       = value.bytes.size();
            result["cursor"]      = cursor;
        }
        return toolResult(std::move(result));
    }
    if (name == "class_jasm") {
        std::string className;
        Json        slotError;
        if (!slot::resolveClassArgument(*current, arguments, "class", true, className, slotError))
            return slotError;
        auto selected = current->classInfo(className);
        if (selected.is_err())
            { const auto err = shared::toCaStatus(selected.unwrap_err()); return toolError("select class for Jasm failed", &err); }
        auto value = current->getJasm(selected.unwrap().resourceUri);
        if (!value.isOk()) {
            Json diagnostics = Json::array();
            for (const auto& item : value.diagnostics) diagnostics.push_back(diagnosticJson(item));
            return toolResult({{"diagnostics", std::move(diagnostics)}}, true);
        }
        return toolResult({{"internal_name", value.value->internalName.to_std_string()},
                           {"source", utf8(value.value->source.ref())}});
    }
    if (name == "method_get" || name == "method_bytecode" || name == "method_jasm") {
        //T2 name 槽：owner=ClassRef/raw、name=MemberRef/raw（desc 不进
        //句柄，MemberRef 命中时自表回填；raw 保持既有 desc 必填契约）。
        const auto slots = slot::loadWorkspaceShadowTable(*current);
        std::string ownerRaw;
        std::string memberName;
        std::string descriptor;
        Json        slotError;
        if (!slot::resolveMethodSlots(slots, arguments, true, ownerRaw, memberName, descriptor, slotError))
            return slotError;
        if (name == "method_get") {
            auto value = current->methodInfo(ownerRaw, memberName, descriptor);
            if (value.is_err()) { const auto err = shared::toCaStatus(value.unwrap_err()); return toolError("get method failed", &err); }
            return toolResult(methodJson(value.unwrap()));
        }
        if (name == "method_bytecode") {
            auto value = current->methodBytecode(ownerRaw, memberName, descriptor);
            if (value.is_err()) { const auto err = shared::toCaStatus(value.unwrap_err()); return toolError("get method bytecode failed", &err); }
            Json instructions = Json::array();
            for (const auto& item : value.unwrap().instructions)
                instructions.push_back({{"index", item.index},
                                        {"opcode", item.opcode},
                                        {"mnemonic", item.mnemonic},
                                        {"node_type", item.nodeType}});
            //指令摘要只有 index/opcode/mnemonic/node_type,操作数级证据在
            //jasm 全文;两份一次取齐,调用方不必退回自写解析器。
            Json payload{{"method", methodJson(value.unwrap().method)},
                         {"instructions", std::move(instructions)}};
            auto jasm = current->getMethodJasm(ownerRaw, memberName, descriptor);
            if (jasm.isOk()) {
                payload["jasm"] = utf8(jasm.value->source.ref());
            } else {
                Json diagnostics = Json::array();
                for (const auto& item : jasm.diagnostics) diagnostics.push_back(diagnosticJson(item));
                payload["jasm_diagnostics"] = std::move(diagnostics);
            }
            return toolResult(std::move(payload));
        }
        auto value = current->getMethodJasm(ownerRaw, memberName, descriptor);
        if (!value.isOk()) {
            Json diagnostics = Json::array();
            for (const auto& item : value.diagnostics) diagnostics.push_back(diagnosticJson(item));
            return toolResult({{"diagnostics", std::move(diagnostics)}}, true);
        }
        return toolResult({{"internal_name", value.value->internalName.to_std_string()},
                           {"name", value.value->name.to_std_string()},
                           {"descriptor", value.value->descriptor.to_std_string()},
                           {"source", utf8(value.value->source.ref())}});
    }
    if (name == "shadow_to_raw" || name == "raw_to_shadow") {
        //shadow 双向查询：shadow_to_raw = 显示名 → 真名；
        //raw_to_shadow = 真名 → 显示名。sidecar 缺省解析 `<jar>.mjt-shadow`，
        //`file` 参数显式覆盖。机械占位名跨类重号，命中返回列表。
        //T2：matches[] 增发 `ref` 引用句柄（§2 形态，缺 token/手工名
        //省略列）；shadow_to_raw 接受整串 Ref 输入（handle → 条目反查，四分类
        //同名槽口径）；`owner` 过滤接受 ClassRef。
        const auto lookupName  = arguments.value("name", std::string{});
        const auto fileArg     = arguments.value("file", std::string{});
        const auto ownerFilter = arguments.value("owner", std::string{});
        const auto kindFilter  = arguments.value("kind", std::string{});
        if (lookupName.empty()) return toolError("name must be a non-empty string");
        const auto sidecarPath =
            fileArg.empty() ? mjt::resolveShadowPath(current->sourcePath(), "") : fileArg;
        std::error_code fsError;
        if (!std::filesystem::exists(sidecarPath, fsError)) {
            return toolError("shadow sidecar not found: " + sidecarPath +
                             " (generate it with `mjt shadow -i <jar>`)");
        }
        auto sidecar = mjbase::mapping::readShadowAliasFile(
            ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(sidecarPath.data()),
                                              sidecarPath.size()));
        if (sidecar.is_err()) {
            auto       err     = std::move(sidecar).unwrap_err();
            const auto message = err.message.to_std_string();
            return toolError("read shadow sidecar failed: " + message);
        }
        const auto table =
            mjbase::renamer::ShadowAliasTable::fromMapping(std::move(sidecar).unwrap());
        std::vector<mjbase::renamer::ShadowAliasEntry> matches;
        mjbase::renamer::ShadowSlotResult              handleResult;
        auto handleEntry =
            mjbase::renamer::resolveShadowHandleEntry(table, lookupName, handleResult);
        if (handleEntry.has_value()) {
            //整串 Ref 输入：仅 shadow_to_raw 语义成立（handle → 真名）。
            if (name != "shadow_to_raw") {
                return toolError(
                    "raw_to_shadow expects a raw name; pass the reference to shadow_to_raw");
            }
            matches.push_back(*handleEntry);
        }
        else if (handleResult.error != mjbase::renamer::ShadowSlotError::None) {
            return toolError(handleResult.message);
        }
        else {
            matches = (name == "shadow_to_raw") ? table.findByShadowName(lookupName)
                                                : table.findByRawName(lookupName);
        }
        std::string ownerFilterRaw = ownerFilter;
        if (!ownerFilterRaw.empty()) {
            //owner 过滤槽同 name 槽口径：Ref → 解析为真实名（四分类同源）。
            const auto ownerForm = mjbase::renamer::parseShadowRef(ownerFilterRaw).form;
            if (ownerForm == mjbase::renamer::ShadowRefForm::ClassRef ||
                ownerForm == mjbase::renamer::ShadowRefForm::MemberRef) {
                mjbase::renamer::ShadowSlotResult ownerResult;
                const auto ownerEntry =
                    mjbase::renamer::resolveShadowHandleEntry(table, ownerFilterRaw, ownerResult);
                if (!ownerEntry.has_value()) {
                    if (ownerResult.error != mjbase::renamer::ShadowSlotError::None)
                        return toolError(ownerResult.message);
                    return toolError("owner filter must be a raw internal name or a class "
                                     "reference like ${Class_N.token8}");
                }
                ownerFilterRaw = std::string(ownerEntry->rawName);
            }
        }
        auto kindLabelOf = [](mjbase::renamer::ShadowKind kind) {
            switch (kind) {
            case mjbase::renamer::ShadowKind::Class: return "class";
            case mjbase::renamer::ShadowKind::Field: return "field";
            case mjbase::renamer::ShadowKind::Method: return "method";
            }
            return "unknown";
        };
        //条目字段是表内池的借用视图：进 Json 前统一转 std::string 值。
        auto toStd = [](std::string_view value) { return std::string(value); };
        Json values = Json::array();
        for (const auto& entry : matches) {
            if (!ownerFilterRaw.empty() && toStd(entry.owner) != ownerFilterRaw) continue;
            if (!kindFilter.empty() && kindLabelOf(entry.kind) != kindFilter) continue;
            Json match = {{"kind", kindLabelOf(entry.kind)},
                          {"owner", toStd(entry.owner)},
                          {"name", toStd(entry.rawName)},
                          {"desc", toStd(entry.desc)},
                          {"shadow", toStd(entry.shadowName)}};
            const auto ref = mjbase::renamer::shadowRefHandleOf(table, entry);
            if (!ref.empty()) match["ref"] = ref;
            values.push_back(std::move(match));
        }
        return toolResult(
            {{"file", sidecarPath}, {"count", values.size()}, {"matches", std::move(values)}});
    }
    return std::nullopt;
}

}   //namespace mjt::mcp
