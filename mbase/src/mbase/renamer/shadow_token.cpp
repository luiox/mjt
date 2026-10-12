#include "mbase/renamer/shadow_token.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace mjt::base {
namespace {

using ca::str::Utf8String;
using ca::str::Utf8StringRef;

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isLowerHex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool isShadowSegmentChar(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

///恰好 8 位小写 hex；返回结束下标（失败返回 npos 语义 0 并置 ok=false）。
bool take8Hex(const std::string& input, std::size_t& cursor, std::string& out)
{
    if (cursor + 8 > input.size()) return false;
    for (std::size_t i = 0; i < 8; ++i) {
        if (!isLowerHex(input[cursor + i])) return false;
    }
    out = input.substr(cursor, 8);
    cursor += 8;
    return true;
}

Utf8String diag(const std::string& text)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(text.data()), text.size());
}

Utf8String toUtf8String(Utf8StringRef view)
{
    return Utf8String::from_data(view.data(), view.byte_length());
}

///诊断里的长名截断（脏名可达 ~1600 字符，消息保可读；给出总长供定位）。
std::string brief(std::string_view value)
{
    constexpr std::size_t kMax = 96;
    if (value.size() <= kMax) return std::string(value);
    return std::string(value.substr(0, kMax)) + "…(" + std::to_string(value.size()) +
           " bytes total)";
}

std::string quoted(std::string_view value)
{
    return "'" + brief(value) + "'";
}

///类条目按 (Class_ 序号, token) 定位：影子名精确 `Class_<index>`（不规范化，
///手滑前导零查无此条 → unknown-handle 响亮报错），token 必须逐字节相等。
const ShadowAliasEntry* findClassByHandle(const ShadowAliasTable& table,
                                          const std::string&      index,
                                          const std::string&      token,
                                          bool&                   shadowNameExisted)
{
    const auto shadow = "Class_" + index;
    shadowNameExisted = false;
    for (const auto& entry : table.findByShadowName(
             Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(shadow.data()),
                                      shadow.size()))) {
        if (entry.kind != ShadowKind::Class) continue;
        shadowNameExisted = true;
        if (entry.token8.byte_length() == token.size() &&
            std::equal(token.begin(),
                       token.end(),
                       entry.token8.data())) {
            return &entry;
        }
    }
    return nullptr;
}

void appendRepairAction(ShadowSlotError error, std::string& text)
{
    switch (error) {
    case ShadowSlotError::UnknownHandle:
        text += "; re-fetch the reference with raw_to_shadow/shadow_to_raw "
                "(stale sidecar or wrong token)";
        break;
    case ShadowSlotError::MalformedHandle:
        text += "; rebuild the reference per ${Class_N.token8}[.member_shadow.token8] syntax";
        break;
    case ShadowSlotError::RawNotFound:
        text += "; do not hand-type obfuscated names, fetch the reference with raw_to_shadow";
        break;
    case ShadowSlotError::AmbiguousMember:
        text += "; supply the raw descriptor to disambiguate the overload "
                "(descriptor token grammar lands in T3; pass the raw descriptor for now)";
        break;
    case ShadowSlotError::MissingSidecar:
        text += "; generate a shadow sidecar first (MCP shadow_generate, or `mjt shadow -i <jar>`)";
        break;
    case ShadowSlotError::None:
        break;
    }
}

ShadowSlotResult failure(ShadowSlotError error, const std::string& detail)
{
    ShadowSlotResult result;
    result.error   = error;
    std::string text = shadowSlotErrorCode(error);
    text += ": ";
    text += detail;
    appendRepairAction(error, text);
    result.message = diag(text);
    return result;
}

///raw/EscapedLiteral 共用：恒等放行 + （可选）存在性核验。
ShadowSlotResult resolveRawIdentity(const ShadowAliasTable& table,
                                    const std::string&      name,
                                    bool                    member,
                                    const std::string&      owner,
                                    bool                    verifyRaw)
{
    ShadowSlotResult result;
    result.raw = diag(name);
    if (!verifyRaw || table.empty()) return result;

    const auto view = Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(name.data()),
                                               name.size());
    if (member) {
        const auto ownerView =
            Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(owner.data()), owner.size());
        for (const auto& entry : table.findByRawName(view)) {
            if (entry.kind == ShadowKind::Class) continue;
            if (!entry.owner.equals(ownerView)) continue;
            result.kind = entry.kind;
            return result;
        }
        return failure(ShadowSlotError::RawNotFound,
                       "no member named " + quoted(name) + " is registered under owner " +
                           quoted(owner) + " in the loaded shadow sidecar");
    }
    if (table.classEntry(view) != nullptr) return result;
    return failure(ShadowSlotError::RawNotFound,
                   "bare name " + quoted(name) +
                       " is not registered in the loaded shadow sidecar");
}

}   //namespace

ShadowRefParts parseShadowRef(const std::string& input)
{
    ShadowRefParts parts;
    if (input.compare(0, 3, "$${") == 0) {
        parts.form    = ShadowRefForm::EscapedLiteral;
        parts.literal = input.substr(1);
        return parts;
    }
    if (input.compare(0, 2, "${") != 0) {
        parts.form = ShadowRefForm::Raw;
        return parts;
    }

    std::size_t cursor = 2;
    //前缀字面 "Class_"（大小写敏感；手滑 `${CLASS_…}` 属形状违约）。
    if (input.compare(cursor, 6, "Class_") != 0) {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    cursor += 6;
    const auto indexBegin = cursor;
    while (cursor < input.size() && isDigit(input[cursor])) ++cursor;
    if (cursor == indexBegin) {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    parts.index = input.substr(indexBegin, cursor - indexBegin);
    if (cursor >= input.size() || input[cursor] != '.') {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    ++cursor;
    if (!take8Hex(input, cursor, parts.classToken)) {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    if (cursor < input.size() && input[cursor] == '}') {
        if (cursor + 1 != input.size()) {
            parts.form = ShadowRefForm::Malformed;
            return parts;
        }
        parts.form = ShadowRefForm::ClassRef;
        return parts;
    }
    //MemberRef 后缀：`.影子名.token8}`。
    if (cursor >= input.size() || input[cursor] != '.') {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    ++cursor;
    const auto shadowBegin = cursor;
    while (cursor < input.size() && isShadowSegmentChar(input[cursor])) ++cursor;
    if (cursor == shadowBegin) {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    parts.memberShadow = input.substr(shadowBegin, cursor - shadowBegin);
    if (cursor >= input.size() || input[cursor] != '.') {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    ++cursor;
    if (!take8Hex(input, cursor, parts.memberToken)) {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    if (cursor + 1 != input.size() || input[cursor] != '}') {
        parts.form = ShadowRefForm::Malformed;
        return parts;
    }
    parts.form = ShadowRefForm::MemberRef;
    return parts;
}

const char* shadowSlotErrorCode(ShadowSlotError error)
{
    switch (error) {
    case ShadowSlotError::UnknownHandle: return "unknown-handle";
    case ShadowSlotError::MalformedHandle: return "malformed-handle";
    case ShadowSlotError::RawNotFound: return "raw-not-found";
    case ShadowSlotError::AmbiguousMember: return "ambiguous-member";
    case ShadowSlotError::MissingSidecar: return "missing-sidecar";
    case ShadowSlotError::None: return "";
    }
    return "";
}

std::optional<ShadowAliasEntry> resolveShadowHandleEntry(const ShadowAliasTable& table,
                                                         const std::string&      handle,
                                                         ShadowSlotResult&       result)
{
    result = ShadowSlotResult{};
    const auto parts = parseShadowRef(handle);
    if (parts.form != ShadowRefForm::ClassRef && parts.form != ShadowRefForm::MemberRef) {
        return std::nullopt;   //非句柄形状：错误位留空，调用方按非句柄输入分派
    }
    if (table.empty()) {
        result = failure(ShadowSlotError::MissingSidecar,
                         "reference " + quoted(handle) +
                             " requires a loaded shadow sidecar, but none is loaded");
        return std::nullopt;
    }
    bool shadowNameExisted = false;
    const auto* classEntry =
        findClassByHandle(table, parts.index, parts.classToken, shadowNameExisted);
    if (classEntry == nullptr) {
        std::string detail = "no class entry matches ${Class_" + parts.index + "." +
                             parts.classToken + "} in the loaded shadow sidecar";
        if (shadowNameExisted) detail += " (class shadow name exists but token mismatches)";
        result = failure(ShadowSlotError::UnknownHandle, detail);
        return std::nullopt;
    }
    const auto ownerView = classEntry->rawName;
    if (parts.form == ShadowRefForm::ClassRef) {
        result.error      = ShadowSlotError::None;
        result.raw        = toUtf8String(ownerView);
        result.kind       = ShadowKind::Class;
        result.fromHandle = true;
        return *classEntry;
    }

    //二元 handle：类 + 成员影子名定位；成员 token 作后缀核验（防手滑
    //错位到同影子名的另一枚），不参与候选选择。
    std::vector<ShadowAliasEntry> candidates;
    for (const auto& entry : table.findByShadowName(Utf8StringRef::from_data(
             reinterpret_cast<const ca::u8*>(parts.memberShadow.data()),
             parts.memberShadow.size()))) {
        if (entry.kind == ShadowKind::Class) continue;
        if (!entry.owner.equals(ownerView)) continue;
        candidates.push_back(entry);
    }
    if (candidates.empty()) {
        result = failure(ShadowSlotError::UnknownHandle,
                         "member shadow " + quoted(parts.memberShadow) +
                             " is not registered under owner " +
                             brief(ownerView.to_std_string()) + " (" +
                             std::to_string(ownerView.byte_length()) + " bytes)");
        return std::nullopt;
    }
    if (candidates.size() > 1) {
        //重载歧义：二元 handle 区分不了（表内同影子名多条 desc）。
        result = failure(ShadowSlotError::AmbiguousMember,
                         "reference " + quoted(handle) + " matches " +
                             std::to_string(candidates.size()) +
                             " member entries under owner " +
                             brief(ownerView.to_std_string()) +
                             "; the binary handle cannot distinguish overloads");
        return std::nullopt;
    }
    const auto& member = candidates.front();
    const bool  tokenMatches =
        member.token8.byte_length() == parts.memberToken.size() &&
        std::equal(parts.memberToken.begin(), parts.memberToken.end(), member.token8.data());
    if (!tokenMatches) {
        result = failure(ShadowSlotError::UnknownHandle,
                         "member token " + quoted(parts.memberToken) + " mismatches for " +
                             quoted(parts.memberShadow) + " under owner " +
                             brief(ownerView.to_std_string()));
        return std::nullopt;
    }
    result.error      = ShadowSlotError::None;
    result.raw        = toUtf8String(member.rawName);
    result.descriptor = toUtf8String(member.desc);
    result.kind       = member.kind;
    result.fromHandle = true;
    return member;
}

ShadowSlotResult resolveShadowSlot(const ShadowAliasTable& table,
                                   const std::string&      input,
                                   const ShadowSlotQuery&  query)
{
    if (query.position != ShadowSlotPosition::Name) {
        //T3 分支占位：descriptor / jasm body 标识槽文法未接线，fail-loud
        //防意外提前使用；T3 在本收口内替换本分支，不开新入口。
        return failure(ShadowSlotError::MalformedHandle,
                       std::string(query.position == ShadowSlotPosition::Descriptor
                                       ? "descriptor"
                                       : "jasm-body") +
                           " slot does not accept shadow references yet (planned for T3); "
                           "pass the raw form for now");
    }

    const auto parts = parseShadowRef(input);
    switch (parts.form) {
    case ShadowRefForm::Raw:
        return resolveRawIdentity(table, input, query.member, query.owner, query.verifyRaw);

    case ShadowRefForm::EscapedLiteral:
        //`$${…}` = 字面 `${…}`：剥 `$` 后按 raw 语义（恒等/核验同 raw 分支）。
        return resolveRawIdentity(
            table, parts.literal, query.member, query.owner, query.verifyRaw);

    case ShadowRefForm::Malformed:
        if (table.empty()) {
            //引用形状但无表可查：缺 sidecar 硬报错（§6 末行），不静默回落。
            return failure(ShadowSlotError::MissingSidecar,
                           "reference-shaped input " + quoted(input) +
                               " requires a loaded shadow sidecar, but none is loaded");
        }
        return failure(ShadowSlotError::MalformedHandle,
                       quoted(input) + " does not match the reference grammar "
                                       "${Class_N.token8}[.member_shadow.token8]");

    case ShadowRefForm::ClassRef:
    case ShadowRefForm::MemberRef: {
        ShadowSlotResult result;
        const auto       entry = resolveShadowHandleEntry(table, input, result);
        if (!entry.has_value() && result.error == ShadowSlotError::None) {
            //不可达：ClassRef/MemberRef 形状经 resolveShadowHandleEntry 必有
            //结果或错误（空表 → MissingSidecar）。防御性兜底。
            return failure(ShadowSlotError::UnknownHandle,
                           "reference " + quoted(input) + " did not resolve");
        }
        return result;
    }
    }
    return failure(ShadowSlotError::MalformedHandle, "unreachable reference form");
}

std::string shadowRefHandleOf(const ShadowAliasTable& table, const ShadowAliasEntry& entry)
{
    const std::string token = entry.token8.to_std_string();
    if (token.size() != 8) return {};
    if (entry.kind == ShadowKind::Class) {
        //类影子名必须呈 `Class_<纯数字>` 规范形（§2 文法前缀）；手工名不发句柄。
        const std::string shadow = entry.shadowName.to_std_string();
        if (shadow.size() <= 6 || shadow.compare(0, 6, "Class_") != 0) return {};
        for (std::size_t i = 6; i < shadow.size(); ++i) {
            if (!isDigit(shadow[i])) return {};
        }
        return "${" + shadow + "." + token + "}";
    }

    //成员条目：owner 类条目须存在且可发句柄。
    const auto* ownerEntry = table.classEntry(entry.owner);
    if (ownerEntry == nullptr) return {};
    const std::string ownerHandle = shadowRefHandleOf(table, *ownerEntry);
    if (ownerHandle.empty()) return {};
    const std::string memberShadow = entry.shadowName.to_std_string();
    if (memberShadow.empty()) return {};
    for (const char c : memberShadow) {
        if (!isShadowSegmentChar(c)) return {};
    }
    return ownerHandle.substr(0, ownerHandle.size() - 1) + "." + memberShadow + "." + token + "}";
}

}   //namespace mjt::base
