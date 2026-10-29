#include "tool/shadow_slot.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "shared/shadow_display.hpp"
#include "tool/json.hpp"

#include "mjbase/mapping/shadow_alias_format.hpp"
#include "workspace/i_workspace.hpp"

namespace mjt::mcp::slot {

namespace {

using mjbase::renamer::ShadowAliasTable;
using mjbase::renamer::ShadowSlotError;

///resolver 结果 → 结构化 toolError（message 已含码字前缀与修复动作）。
///mjbase 侧诊断消息是 std::string，旧栈为 Utf8String。
Json errorOf(const mjbase::renamer::ShadowSlotResult& result)
{
    return detail::toolError(result.message);
}

}   //namespace

Json slotError(const std::string& message)
{
    return detail::toolError(message);
}

WorkspaceShadowTable loadWorkspaceShadowTable(mjt::workspace::IWorkspace& workspace,
                                              const std::string& file)
{
    WorkspaceShadowTable slots;
    slots.path = file.empty() ? mjt::resolveShadowPath(workspace.sourcePath(), "") : file;
    std::error_code fsError;
    if (!std::filesystem::exists(slots.path, fsError)) {
        slots.note = "absent: no shadow sidecar at " + slots.path;
        return slots;
    }
    auto sidecar = mjbase::mapping::readShadowAliasFile(
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(slots.path.data()),
                                          slots.path.size()));
    if (sidecar.is_err()) {
        slots.note = "invalid: read shadow sidecar failed: " +
                     std::move(sidecar).unwrap_err().message.to_std_string();
        return slots;
    }
    slots.table   = ShadowAliasTable::fromMapping(std::move(sidecar).unwrap());
    slots.present = true;
    return slots;
}

bool resolveClassSlot(const WorkspaceShadowTable& slots,
                      const std::string&          input,
                      bool                        verifyRaw,
                      SlotName&                   out,
                      Json&                       error)
{
    if (input.empty())   //空选择器直通：由 workspace 给出既有 canonical 错误
    {
        out.raw = input;
        return true;
    }
    mjbase::renamer::ShadowSlotQuery query;
    query.position  = mjbase::renamer::ShadowSlotPosition::Name;
    query.verifyRaw = verifyRaw;
    const auto result =
        mjbase::renamer::resolveShadowSlot(slots.table, input, query);
    if (result.error != ShadowSlotError::None) {
        error = errorOf(result);
        return false;
    }
    out.raw        = result.raw;
    out.fromHandle = result.fromHandle;
    return true;
}

bool resourceUriOfResolved(mjt::workspace::IWorkspace& workspace,
                           const std::string&                    internalName,
                           std::string&                          uri,
                           Json&                                 error)
{
    auto info = workspace.classInfo(internalName);
    if (info.is_err()) {
        error = slotError("unknown-handle: reference resolved to " + internalName +
                          " but the class is not loadable in this workspace: " +
                          info.unwrap_err().message);
        return false;
    }
    uri = info.unwrap().resourceUri;
    return true;
}

bool resolveMemberSlot(const WorkspaceShadowTable& slots,
                       const std::string&          ownerRaw,
                       const std::string&          input,
                       const std::string&          descHint,
                       bool                        verifyRaw,
                       SlotName&                   out,
                       Json&                       error)
{
    if (input.empty())   //空成员名直通：由 workspace 给出既有 canonical 错误
    {
        out.raw = input;
        return true;
    }
    mjbase::renamer::ShadowSlotQuery query;
    query.position  = mjbase::renamer::ShadowSlotPosition::Name;
    query.member    = true;
    query.owner     = ownerRaw;
    query.descHint  = descHint;
    query.verifyRaw = verifyRaw;
    const auto result = mjbase::renamer::resolveShadowSlot(slots.table, input, query);
    if (result.error != ShadowSlotError::None) {
        error = errorOf(result);
        return false;
    }
    out.raw        = result.raw;
    out.descriptor = result.descriptor;
    out.fromHandle = result.fromHandle;
    //句柄解析成功但调用方另给了不一致的 desc：调用方输入矛盾（句柄本身
    //有效），按普通参数错误报出，不入四分类（四分类只判解析失败）。
    if (result.fromHandle && !descHint.empty() && descHint != out.descriptor) {
        error = slotError("descriptor '" + descHint + "' does not match the descriptor '" +
                          out.descriptor + "' recorded for the resolved member reference; "
                          "omit descriptor or pass the recorded one");
        return false;
    }
    return true;
}

bool resolveMethodSlots(const WorkspaceShadowTable& slots,
                        const Json&                 arguments,
                        bool                        verifyRaw,
                        std::string&                ownerRaw,
                        std::string&                name,
                        std::string&                descriptor,
                        Json&                       error)
{
    const auto ownerInput = arguments.value("owner", std::string{});
    const auto nameInput  = arguments.value("name", std::string{});
    const auto descInput  = arguments.value("descriptor", std::string{});
    SlotName    ownerSlot;
    if (!resolveClassSlot(slots, ownerInput, verifyRaw, ownerSlot, error)) return false;
    ownerRaw = ownerSlot.raw;
    SlotName nameSlot;
    if (!resolveMemberSlot(slots, ownerRaw, nameInput, descInput, verifyRaw, nameSlot, error))
        return false;
    //MemberRef 的 desc 不进句柄（工具查表）：调用方省略 desc 时回填表内值；
    //raw 恒等路径不回填（保持既有 desc 必填契约，零漂移）。
    name       = nameSlot.raw;
    descriptor = (nameSlot.fromHandle && descInput.empty()) ? nameSlot.descriptor : descInput;
    return true;
}

bool backfillMemberDescriptor(mjt::workspace::IWorkspace& workspace,
                              const std::string&             ownerRaw,
                              const std::string&             memberName,
                              bool                           field,
                              std::string&                   descriptor,
                              Json&                          error)
{
    //空名直通：由 workspace 给出既有 canonical 错误。
    if (memberName.empty()) return true;
    //owner 类不可解析（表外类直通查询、类名拼错）：无操作放行，descriptor
    //保持为空，由下游查询给出 canonical 错误/空集。
    auto info = workspace.classInfo(ownerRaw);
    if (info.is_err()) return true;
    const auto& clazz = info.unwrap();
    //候选枚举机（同源复用）：该名字在目标池的全部去重描述符；另一池的同名
    //命中用于零命中提示（方法/字段名互串是高频笔误）。
    const auto collect = [&](bool inFieldPool, std::vector<std::string>& out) {
        const auto visit = [&](const std::string& name, const std::string& desc) {
            if (name != memberName) return;
            if (std::find(out.begin(), out.end(), desc) == out.end()) out.push_back(desc);
        };
        if (inFieldPool)
            for (const auto& item : clazz.fields) visit(item.name, item.descriptor);
        else
            for (const auto& item : clazz.methods) visit(item.name, item.descriptor);
    };
    std::vector<std::string> candidates;
    collect(field, candidates);
    if (candidates.size() == 1) {
        descriptor = candidates.front();
        return true;
    }
    std::vector<std::string> otherKind;
    collect(!field, otherKind);
    const auto kindLabel = field ? "field" : "method";
    std::string message;
    if (candidates.empty()) {
        message = std::string("no ") + kindLabel + " named \"" + memberName + "\" in " +
                  ownerRaw;
        if (!otherKind.empty()) {
            message += std::string("; a ") + (field ? "method" : "field") +
                       " with this name exists (descriptor";
            for (std::size_t index = 0; index < otherKind.size(); ++index) {
                message += index == 0 ? " " : ", ";
                message += otherKind[index];
            }
            message += ")";
        }
    }
    else {
        message = std::string("descriptor is ambiguous for ") + ownerRaw + "." + memberName +
                  "; candidates: ";
        for (std::size_t index = 0; index < candidates.size(); ++index) {
            if (index > 0) message += ", ";
            message += candidates[index];
        }
    }
    //diagnostics 数组统一格式（消息字符串数组）。
    error = detail::toolResult({{"error", message}, {"diagnostics", Json::array({message})}},
                               true);
    return false;
}

bool memberDescriptorMismatch(mjt::workspace::IWorkspace& workspace,
                              const std::string&             ownerRaw,
                              const std::string&             memberName,
                              const std::string&             descriptor,
                              bool                           field,
                              Json&                          error)
{
    //空名/空描述符不适配：canonical 错误面在下游。
    if (memberName.empty() || descriptor.empty()) return true;
    auto info = workspace.classInfo(ownerRaw);
    if (info.is_err()) return true;
    const auto& clazz = info.unwrap();
    //同名候选枚举（与 backfill 同一收集面）；描述符命中其一说明失败另有
    //其因（如 owner 解析错位），不适配。
    std::vector<std::string> candidates;
    const auto visit = [&](const std::string& name, const std::string& desc) {
        if (name == memberName &&
            std::find(candidates.begin(), candidates.end(), desc) == candidates.end())
            candidates.push_back(desc);
    };
    if (field)
        for (const auto& item : clazz.fields) visit(item.name, item.descriptor);
    else
        for (const auto& item : clazz.methods) visit(item.name, item.descriptor);
    if (candidates.empty()) return true;
    if (std::find(candidates.begin(), candidates.end(), descriptor) != candidates.end())
        return true;
    const auto kindLabel = field ? "field" : "method";
    std::string message = std::string("descriptor \"") + descriptor +
                          "\" does not match any " + kindLabel + " named \"" + memberName +
                          "\" in " + ownerRaw + "; candidates: ";
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        if (index > 0) message += ", ";
        message += candidates[index];
    }
    error = detail::toolResult({{"error", message}, {"diagnostics", Json::array({message})}},
                               true);
    return false;
}

bool resolveClassArgument(mjt::workspace::IWorkspace& workspace,
                          const Json&                           arguments,
                          const std::string&                    key,
                          bool                                  verifyRaw,
                          std::string&                          out,
                          Json&                                 error)
{
    const auto input = arguments.value(key, std::string{});
    if (input.empty()) {
        out = input;
        return true;
    }
    const auto slots = loadWorkspaceShadowTable(workspace);
    SlotName   resolved;
    if (!resolveClassSlot(slots, input, verifyRaw, resolved, error)) return false;
    out = resolved.raw;
    return true;
}

bool resolveClassUriArgument(mjt::workspace::IWorkspace& workspace,
                             const Json&                           arguments,
                             const std::string&                    key,
                             std::string&                          out,
                             Json&                                 error)
{
    const auto input = arguments.value(key, std::string{});
    if (input.empty()) {
        out = input;
        return true;
    }
    const auto form = mjbase::renamer::parseShadowRef(input).form;
    const bool refShaped = form == mjbase::renamer::ShadowRefForm::ClassRef ||
                           form == mjbase::renamer::ShadowRefForm::MemberRef ||
                           form == mjbase::renamer::ShadowRefForm::Malformed;
    const auto slots = loadWorkspaceShadowTable(workspace);
    if (!refShaped) {
        //raw：内部名核表（URI 归一），命中后原样透传（URI 契约零漂移）。
        const auto internal = mjt::internalNameOfUri(input);
        if (!slots.present) {
            out = input;
            return true;
        }
        if (slots.table.classEntry(internal) == nullptr) {
            error = slotError(mjbase::renamer::shadowSlotErrorCode(
                                  mjbase::renamer::ShadowSlotError::RawNotFound) +
                              std::string(": bare selector '") + input +
                              "' is not registered in the loaded shadow sidecar; do not "
                              "hand-type obfuscated names, fetch the reference with "
                              "raw_to_shadow");
            return false;
        }
        out = input;
        return true;
    }
    SlotName resolved;
    if (!resolveClassSlot(slots, input, true, resolved, error)) return false;
    return resourceUriOfResolved(workspace, resolved.raw, out, error);
}

}   //namespace mjt::mcp::slot
