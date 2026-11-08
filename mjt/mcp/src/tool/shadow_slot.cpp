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

///std::string → 借用视图（B3 接口面 Utf8StringRef 参数化适配；克隆前提：
///源串在调用期内存活）。
ca::str::Utf8StringRef utf8Ref(const std::string& value) noexcept
{
    return ca::str::Utf8StringRef::from_data(
        value.empty() ? nullptr : reinterpret_cast<const ca::u8*>(value.data()), value.size());
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
    slots.path = file.empty() ? mjt::resolveShadowPath(workspace.sourcePath().to_std_string(), "") : file;
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
    auto info = workspace.classInfo(utf8Ref(internalName));
    if (info.is_err()) {
        error = slotError("unknown-handle: reference resolved to " + internalName +
                          " but the class is not loadable in this workspace: " +
                          info.unwrap_err().message);
        return false;
    }
    // ClassInfo 携 move-only Utf8String：unwrap 走移动重载。
    uri = std::move(info).unwrap().resourceUri.to_std_string();
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
    auto       descInput  = arguments.value("descriptor", std::string{});
    //描述符槽：desc 带 `${` 引用形状 → 内嵌类 handle 展开，展开值参与
    //成员 descHint 核对与输出；raw desc 原样直通（契约零漂移）。
    std::string resolvedDesc;
    if (!resolveDescriptorArgument(slots, descInput, resolvedDesc, error)) return false;
    descInput = std::move(resolvedDesc);
    SlotName  ownerSlot;
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
    auto info = workspace.classInfo(utf8Ref(ownerRaw));
    if (info.is_err()) return true;
    const auto& clazz = std::move(info).unwrap();
    //候选枚举机（同源复用）：该名字在目标池的全部去重描述符；另一池的同名
    //命中用于零命中提示（方法/字段名互串是高频笔误）。DTO 文本字段为
    //Utf8String：visit 以借用视图收参，候选表落 std::string。
    const auto collect = [&](bool inFieldPool, std::vector<std::string>& out) {
        const auto visit = [&](const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc) {
            if (name != memberName) return;
            const auto descText = desc.to_std_string();
            if (std::find(out.begin(), out.end(), descText) == out.end())
                out.push_back(descText);
        };
        if (inFieldPool)
            for (const auto& item : clazz.fields) visit(item.name.ref(), item.descriptor.ref());
        else
            for (const auto& item : clazz.methods) visit(item.name.ref(), item.descriptor.ref());
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
    auto info = workspace.classInfo(utf8Ref(ownerRaw));
    if (info.is_err()) return true;
    const auto& clazz = std::move(info).unwrap();
    //同名候选枚举（与 backfill 同一收集面）；描述符命中其一说明失败另有
    //其因（如 owner 解析错位），不适配。visit 以借用视图收参（同 backfill）。
    std::vector<std::string> candidates;
    const auto visit = [&](const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc) {
        const auto descText = desc.to_std_string();
        if (name == memberName &&
            std::find(candidates.begin(), candidates.end(), descText) == candidates.end())
            candidates.push_back(descText);
    };
    if (field)
        for (const auto& item : clazz.fields) visit(item.name.ref(), item.descriptor.ref());
    else
        for (const auto& item : clazz.methods) visit(item.name.ref(), item.descriptor.ref());
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

bool resolveDescriptorArgument(const WorkspaceShadowTable& slots,
                               const std::string&          input,
                               std::string&                out,
                               Json&                       error)
{
    out = input;
    //非引用形状（raw 描述符）直通：文法校验与 canonical 错误面在下游，
    //既有契约零漂移。
    if (input.empty() || input.find("${") == std::string::npos) return true;
    mjbase::renamer::ShadowSlotQuery query;
    query.position = mjbase::renamer::ShadowSlotPosition::Descriptor;
    const auto result = mjbase::renamer::resolveShadowSlot(slots.table, input, query);
    if (result.error != ShadowSlotError::None) {
        error = errorOf(result);
        return false;
    }
    out = result.raw;
    return true;
}

bool resolveJasmBodyArgument(const WorkspaceShadowTable& slots,
                             const std::string&          input,
                             std::string&                out,
                             Json&                       error)
{
    out = input;
    if (input.empty() || input.find("${") == std::string::npos) return true;
    mjbase::renamer::ShadowSlotQuery query;
    query.position = mjbase::renamer::ShadowSlotPosition::JasmBody;
    const auto result = mjbase::renamer::resolveShadowSlot(slots.table, input, query);
    if (result.error != ShadowSlotError::None) {
        error = errorOf(result);
        return false;
    }
    out = result.raw;
    return true;
}

namespace {

///Json 字符串槽改写的取值/写回通用体：先拷出输入再解析（Json 引用失效
///防御——解析产物可能别名输入串），成功后写回。
bool rewriteClassValue(const WorkspaceShadowTable& slots, Json& value, Json& error)
{
    if (!value.is_string()) return true;
    const auto input = value.get_ref<const std::string&>();
    SlotName   resolved;
    //verifyRaw=false：开放类名面（exceptions/catch type/owner 等）允许表外
    //raw 直通——JDK 类等外部类型不因无 sidecar 登记而拒收。
    if (!resolveClassSlot(slots, input, false, resolved, error)) return false;
    value = std::move(resolved.raw);
    return true;
}

bool rewriteDescriptorValue(const WorkspaceShadowTable& slots, Json& value, Json& error)
{
    if (!value.is_string()) return true;
    std::string resolved;
    if (!resolveDescriptorArgument(slots, value.get_ref<const std::string&>(), resolved, error))
        return false;
    value = std::move(resolved);
    return true;
}

///成员名槽改写（owner/descHint 为同对象内已展开的值；raw 直通不核表）。
bool rewriteMemberValue(const WorkspaceShadowTable& slots,
                        const std::string&          ownerRaw,
                        const std::string&          descHint,
                        Json&                       value,
                        Json&                       error)
{
    if (!value.is_string()) return true;
    const auto input = value.get_ref<const std::string&>();
    SlotName   resolved;
    if (!resolveMemberSlot(slots, ownerRaw, input, descHint, false, resolved, error))
        return false;
    value = std::move(resolved.raw);
    return true;
}

///handle 子对象（field_value 与指令的 handle / bootstrap_method）：
///descriptor → Descriptor 槽；owner → Name 槽；name → 成员槽
///（descHint = 同对象展开后的 descriptor）。
bool rewriteHandleObject(const WorkspaceShadowTable& slots, Json& object, Json& error)
{
    if (!object.is_object()) return true;
    if (object.contains("descriptor") &&
        !rewriteDescriptorValue(slots, object.at("descriptor"), error))
        return false;
    if (object.contains("owner") && !rewriteClassValue(slots, object.at("owner"), error))
        return false;
    if (object.contains("name")) {
        if (!rewriteMemberValue(slots,
                                object.value("owner", std::string{}),
                                object.value("descriptor", std::string{}),
                                object.at("name"),
                                error))
            return false;
    }
    return true;
}

///field_value 常量树：kind=type 的 descriptor（裸内部类名 → Name 槽；ldc
///类常量的 CP 名就是内部名）、kind=dynamic 的 descriptor（真描述符 →
///Descriptor 槽）、kind=handle 的 owner+name+descriptor、handle/
///bootstrap_method 子对象、bootstrap_arguments 递归。dynamic 的 name 是
///linkage 字面量（新符号），不接成员槽。
bool rewriteConstantValue(const WorkspaceShadowTable& slots, Json& value, Json& error)
{
    if (!value.is_object()) return true;
    const auto kind = value.value("kind", std::string{});
    if (value.contains("descriptor")) {
        if (kind == "type") {
            if (!rewriteClassValue(slots, value.at("descriptor"), error)) return false;
        }
        else if (!rewriteDescriptorValue(slots, value.at("descriptor"), error))
            return false;
    }
    if (kind == "handle") {
        if (value.contains("owner") && !rewriteClassValue(slots, value.at("owner"), error))
            return false;
        if (value.contains("name")) {
            if (!rewriteMemberValue(slots,
                                    value.value("owner", std::string{}),
                                    value.value("descriptor", std::string{}),
                                    value.at("name"),
                                    error))
                return false;
        }
    }
    if (value.contains("handle") && !rewriteHandleObject(slots, value.at("handle"), error))
        return false;
    if (value.contains("bootstrap_method") &&
        !rewriteHandleObject(slots, value.at("bootstrap_method"), error))
        return false;
    if (value.contains("bootstrap_arguments")) {
        auto& arguments = value.at("bootstrap_arguments");
        if (arguments.is_array()) {
            for (auto& argument : arguments) {
                if (!rewriteConstantValue(slots, argument, error)) return false;
            }
        }
    }
    return true;
}

///指令对象：owner 全类指令接 Name 槽；descriptor 按位置分槽——type/
///ldc_type 是裸内部类名（Name 槽），field/method/multianewarray 等是真
///描述符（Descriptor 槽）；name 仅 field/method 指令接成员槽——
///ldc_dynamic/invokedynamic 的 name 是 call site linkage 字面量，不按成员
///解析。handle/bootstrap_method 子对象同 field_value。
bool rewriteInstructionValue(const WorkspaceShadowTable& slots, Json& value, Json& error)
{
    if (!value.is_object()) return true;
    const auto kind = value.value("kind", std::string{"simple"});
    if (value.contains("descriptor")) {
        if (kind == "type" || kind == "ldc_type") {
            if (!rewriteClassValue(slots, value.at("descriptor"), error)) return false;
        }
        else if (!rewriteDescriptorValue(slots, value.at("descriptor"), error))
            return false;
    }
    if (value.contains("owner") && !rewriteClassValue(slots, value.at("owner"), error))
        return false;
    if ((kind == "field" || kind == "method") && value.contains("name")) {
        if (!rewriteMemberValue(slots,
                                value.value("owner", std::string{}),
                                value.value("descriptor", std::string{}),
                                value.at("name"),
                                error))
            return false;
    }
    if (value.contains("handle") && !rewriteHandleObject(slots, value.at("handle"), error))
        return false;
    if (value.contains("bootstrap_method") &&
        !rewriteHandleObject(slots, value.at("bootstrap_method"), error))
        return false;
    return true;
}

}   //namespace

bool resolveStructuredMutationRefs(const WorkspaceShadowTable& slots,
                                   Json&                      args,
                                   Json&                      error)
{
    if (!args.is_object()) return true;
    //member_descriptor 全 kind 提升：add_field/add_method 的新成员类型描述符
    //内嵌类名也是引用语义（member_name 的「新符号字面量」豁免不覆盖类型）。
    if (args.contains("member_descriptor") &&
        !rewriteDescriptorValue(slots, args.at("member_descriptor"), error))
        return false;
    if (args.contains("annotation_descriptor") &&
        !rewriteDescriptorValue(slots, args.at("annotation_descriptor"), error))
        return false;
    if (args.contains("exceptions")) {
        auto& exceptions = args.at("exceptions");
        if (exceptions.is_array()) {
            for (auto& item : exceptions) {
                if (!rewriteClassValue(slots, item, error)) return false;
            }
        }
    }
    if (args.contains("try_catch_blocks")) {
        auto& blocks = args.at("try_catch_blocks");
        if (blocks.is_array()) {
            for (auto& item : blocks) {
                if (!item.is_object() || !item.contains("type")) continue;
                if (!rewriteClassValue(slots, item.at("type"), error)) return false;
            }
        }
    }
    if (args.contains("field_value") &&
        !rewriteConstantValue(slots, args.at("field_value"), error))
        return false;
    if (args.contains("instruction") &&
        !rewriteInstructionValue(slots, args.at("instruction"), error))
        return false;
    if (args.contains("initial_instructions")) {
        auto& instructions = args.at("initial_instructions");
        if (instructions.is_array()) {
            for (auto& item : instructions) {
                if (!rewriteInstructionValue(slots, item, error)) return false;
            }
        }
    }
    if (args.contains("local_variables")) {
        auto& variables = args.at("local_variables");
        if (variables.is_array()) {
            for (auto& item : variables) {
                //signature 刻意不接（泛型 `<>` Descriptor 槽 fail-loud，v2 挂账）。
                if (!item.is_object() || !item.contains("descriptor")) continue;
                if (!rewriteDescriptorValue(slots, item.at("descriptor"), error)) return false;
            }
        }
    }
    return true;
}

namespace {

///rebind 收尾公共体：回填报告面；表有实际改动（rebound > 0）才写回
///sidecar。写回失败不回滚表（进程内表已正确，下次成功写回即收敛），
///warning 带回调用方回显。
void finishShadowSync(const WorkspaceShadowTable& slots, ShadowSyncReport& report)
{
    report.present = slots.present;
    report.path    = slots.path;
    if (!slots.present || report.rebound == 0) return;
    //written 取非 const：ShadowAliasFileError 携 move-only Utf8String，const
    //左值的 unwrap_err() 会落到拷贝重载（删除函数）。
    auto written =
        mjbase::mapping::writeShadowAliasFile(utf8Ref(slots.path), slots.table.toMapping());
    if (written.is_err()) {
        report.warning = "shadow sidecar rebind applied but write-back failed: " +
                         std::move(written).unwrap_err().message.to_std_string();
    }
}

}   //namespace

ShadowSyncReport syncShadowAfterRename(mjt::workspace::IWorkspace&           workspace,
                                       const mjt::workspace::RenameRequest& request)
{
    ShadowSyncReport report;
    auto             slots = loadWorkspaceShadowTable(workspace);
    std::vector<mjbase::renamer::RawRenameSpec> specs;
    switch (request.kind) {
    case mjt::workspace::RenameKind::Class:
        specs.push_back({mjbase::renamer::ShadowKind::Class,
                         {},
                         {},
                         request.owner.to_std_string(),
                         request.newName.to_std_string()});
        break;
    case mjt::workspace::RenameKind::Field:
    case mjt::workspace::RenameKind::Method:
        specs.push_back({request.kind == mjt::workspace::RenameKind::Field
                             ? mjbase::renamer::ShadowKind::Field
                             : mjbase::renamer::ShadowKind::Method,
                         request.owner.to_std_string(),
                         request.descriptor.to_std_string(),
                         request.name.to_std_string(),
                         request.newName.to_std_string()});
        break;
    case mjt::workspace::RenameKind::Package:
    {
        //包级规格按表内类条目前缀展开为逐类规格；成员 owner 由类规格级联
        //改写（rebindShadowRawNames 的处理序契约）。
        const auto prefix = request.owner.to_std_string() + "/";
        for (const auto& entry : slots.table.entries()) {
            if (entry.kind != mjbase::renamer::ShadowKind::Class) continue;
            if (entry.rawName.substr(0, prefix.size()) != prefix) continue;
            specs.push_back({mjbase::renamer::ShadowKind::Class,
                             {},
                             {},
                             std::string(entry.rawName),
                             request.newName.to_std_string() +
                                 std::string(entry.rawName.substr(prefix.size() - 1))});
        }
        break;
    }
    case mjt::workspace::RenameKind::Local:
        //局部变量不在 shadow 表（表只覆盖 class/field/method），零操作。
        break;
    }
    report.rebound = mjbase::renamer::rebindShadowRawNames(slots.table, specs);
    finishShadowSync(slots, report);
    return report;
}

ShadowSyncReport syncShadowAfterRemap(mjt::workspace::IWorkspace& workspace,
                                      const mjbase::asm_remapper::MemberRemapPlan& plan,
                                      const mjbase::mapping::IntermediateMapping&  mappings)
{
    ShadowSyncReport report;
    auto             slots = loadWorkspaceShadowTable(workspace);
    if (!slots.present) {
        report.path = slots.path;
        return report;
    }
    std::vector<mjbase::renamer::RawRenameSpec> specs;
    //类规格：工作区类逐一经 plan 映射（显式类映射 + 包规则展开的最终面）。
    auto classes = workspace.listClasses();
    if (classes.is_err()) {
        report.present = true;
        report.path    = slots.path;
        report.warning = "shadow sync skipped: list classes failed: " +
                         std::move(classes).unwrap_err().message;
        return report;
    }
    for (const auto& clazz : std::move(classes).unwrap()) {
        const auto internal = clazz.internalName.to_std_string();
        const auto mapped   = plan.mapInternalName(internal);
        if (mapped == internal) continue;
        specs.push_back(
            {mjbase::renamer::ShadowKind::Class, {}, {}, internal, std::string(mapped)});
    }
    //成员规格：mapping 解析产物枚举；owner 填类改名后的新内部名（类规格
    //先行级联改写成员 owner 的处理序契约），desc 用旧值——表条目身份键仍
    //按旧 desc 匹配（desc 内嵌类名随类改名不重写是 mjbase rebind 既有语义）。
    for (const auto& [owner, fields] : mappings.getFields()) {
        for (const auto& field : fields) {
            specs.push_back({mjbase::renamer::ShadowKind::Field,
                             std::string(plan.mapInternalName(owner)),
                             field.desc.to_std_string(),
                             field.oldName.to_std_string(),
                             field.newName.to_std_string()});
        }
    }
    for (const auto& [owner, methods] : mappings.getMethods()) {
        for (const auto& method : methods) {
            specs.push_back({mjbase::renamer::ShadowKind::Method,
                             std::string(plan.mapInternalName(owner)),
                             method.desc.to_std_string(),
                             method.oldName.to_std_string(),
                             method.newName.to_std_string()});
        }
    }
    report.rebound = mjbase::renamer::rebindShadowRawNames(slots.table, specs);
    finishShadowSync(slots, report);
    return report;
}

}   //namespace mjt::mcp::slot
