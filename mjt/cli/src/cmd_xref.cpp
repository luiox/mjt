/// @file        cmd_xref.cpp
/// @brief       `mjt xref` 命令核心：类/成员反查（to / from / both，有界输出）。
///
/// 地基：workspace ReferenceScan（直写引用索引：方法调用 / 字段访问 / 显式类型
/// 指令 / LDC class / class 头声明；畸形 class 跳过）。to = 编码目标命中（类型
/// 引用目标 + 方法/字段引用 owner，成员选择器收缩到 name[/descriptor] 精确
/// 目标）；from = 该类全部方法发出的引用（成员选择器过滤 sourceMethod）；both
/// = 两向并集（自引用边按全键去重）。输出必经 --limit 截断（默认 200）——
/// 混淆 jar 反查可达百万行，无界输出是事故。
#include "commands.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "shared/i18n_errors.hpp"
#include "shared/json_util.hpp"
#include "shared/output_sink.hpp"
#include "workspace/workspace.hpp"
#include "workspace/analysis_ops.hpp"
#include "workspace/dex_domain.hpp"
#include "workspace/workspace_types.hpp"

namespace mjt::cli {
namespace {

/// Utf8String/UTF-8 std::string → 借用视图（与 info / strings 同款构造）。
ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// XrefError 便捷构造（i18n 渲染）。
XrefError xrefError(XrefError::Kind kind, const char* key,
                    std::initializer_list<std::string_view> args = {})
{
    XrefError error;
    error.kind    = kind;
    error.message = shared::trfStr(key, args);
    return error;
}

/// ASCII 小写归一（parseDecompilerBackendName 同款，后端名/方向名共用口径）。
std::string asciiLower(const std::string& value)
{
    std::string out = value;
    for (auto& character : out)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return out;
}

/// 成员选择器解析："name" 或 "name(desc)"（desc = 自首个 '(' 起的完整后缀，
/// 如 run(I)V——JVM 全描述符形）。返回是否成功（name 通过 @p name /
/// @p descriptor 带出，descriptor 可为空 = 跨描述符匹配）。
bool parseMemberSelector(const std::string& member, std::string& name, std::string& descriptor)
{
    const auto paren = member.find('(');
    if (paren == std::string::npos) {
        name       = member;
        descriptor = "";
        return !name.empty();
    }
    name       = member.substr(0, paren);
    descriptor = member.substr(paren);
    return !name.empty() && descriptor.find(')') != std::string::npos;
}

/// 引用目标文本：方法/字段 = owner.name(desc)（workspace xrefsTo* 同形）。
std::string methodTargetText(const workspace::MethodRefKey& target)
{
    return target.owner.to_std_string() + "." + target.name.to_std_string() +
           target.descriptor.to_std_string();
}

std::string fieldTargetText(const workspace::FieldRefKey& target)
{
    return target.owner.to_std_string() + "." + target.name.to_std_string() +
           target.descriptor.to_std_string();
}

/// 引用事实压入（kind = method / field / type 的 CLI 切面命名）。
void pushReference(std::vector<workspace::ReferenceInfo>& out,
                   const workspace::ReferenceSite& site, std::string_view target,
                   std::string_view kind)
{
    out.push_back(workspace::referenceInfo(site, target, kind));
}

void pushReference(std::vector<workspace::ReferenceInfo>& out,
                   const workspace::MethodReference& reference)
{
    const auto kind = reference.kind == workspace::MethodReferenceKind::Direct
                          ? "method" : workspace::methodReferenceKind(reference.kind);
    pushReference(out, reference.site, methodTargetText(reference.target), kind);
}

void pushReference(std::vector<workspace::ReferenceInfo>& out,
                   const workspace::FieldReference& reference)
{
    const auto kind = reference.kind == workspace::FieldReferenceKind::Direct
                          ? "field" : workspace::fieldReferenceKind(reference.kind);
    pushReference(out, reference.site, fieldTargetText(reference.target), kind);
}

/// 全键去重（both 方向自引用边会同时命中 to 与 from 两集）。
void deduplicateReferences(std::vector<workspace::ReferenceInfo>& references)
{
    workspace::sortReferences(references);
    references.erase(
        std::unique(references.begin(), references.end(),
                    [](const workspace::ReferenceInfo& left,
                       const workspace::ReferenceInfo& right) {
                        return left.sourceClass == right.sourceClass &&
                               left.sourceMethod == right.sourceMethod &&
                               left.sourceDescriptor == right.sourceDescriptor &&
                               left.instructionIndex == right.instructionIndex &&
                               left.kind == right.kind && left.target == right.target;
                    }),
        references.end());
}

}   // namespace

ca::Result<XrefReport, XrefError> collectXref(const std::string& jarPath,
                                              const std::string& className,
                                              const std::string& member,
                                              const std::string& direction, ca::i64 limit)
{
    const std::string normalizedDirection = asciiLower(direction);
    if (normalizedDirection != "to" && normalizedDirection != "from" &&
        normalizedDirection != "both")
        return ca::Err(xrefError(XrefError::Kind::InvalidArgument,
                                 "mjt.cli.xref.err.invalid_direction", {direction}));
    if (limit < 1)
        return ca::Err(xrefError(XrefError::Kind::InvalidArgument, "mjt.cli.xref.err.invalid_limit",
                                 {std::to_string(limit)}));

    std::string memberName;
    std::string memberDescriptor;
    if (!member.empty() && !parseMemberSelector(member, memberName, memberDescriptor))
        return ca::Err(xrefError(XrefError::Kind::InvalidArgument,
                                 "mjt.cli.xref.err.invalid_member", {member}));

    workspace::WorkspaceBase ws;
    auto opened = ws.open(refOf(jarPath));
    if (opened.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(opened).unwrap_err();
        return ca::Err(xrefError(XrefError::Kind::Runtime, "mjt.cli.xref.err.open_failed",
                                 {info.message}));
    }

    // 类选择器存在性检查：DEX 类由 DEX 域枚举。
    const auto* dex = ws.dexDomain();
    auto summaries = dex ? dex->listClassSummaries()
                         : workspace::listClassSummaries(*ws.workingContainer());
    if (summaries.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(summaries).unwrap_err();
        return ca::Err(xrefError(XrefError::Kind::Runtime, "mjt.cli.xref.err.open_failed",
                                 {info.message}));
    }
    bool classPresent = false;
    // unwrap 只走移动重载（Result 携 move-only ClassSummary：lvalue unwrap
    // 的拷贝路径不实例化，MSVC/g++ 同实证）。
    for (const auto& summary : std::move(summaries).unwrap()) {
        if (summary.internalName.to_std_string() == className) {
            classPresent = true;
            break;
        }
    }
    if (!classPresent)
        return ca::Err(xrefError(XrefError::Kind::ClassNotFound,
                                 "mjt.cli.xref.err.class_not_found", {className}));

    auto scan = ws.referenceScan();
    if (scan.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(scan).unwrap_err();
        return ca::Err(xrefError(XrefError::Kind::Runtime, "mjt.cli.xref.err.scan_failed",
                                 {info.message}));
    }
    auto index = std::move(scan).unwrap();

    const auto classRef = refOf(className);
    const auto nameRef  = refOf(memberName);
    const auto descRef  = refOf(memberDescriptor);

    std::vector<workspace::ReferenceInfo> result;
    const bool wantTo   = normalizedDirection == "to" || normalizedDirection == "both";
    const bool wantFrom = normalizedDirection == "from" || normalizedDirection == "both";

    // to：编码目标命中。成员选择器在场时收缩为 owner+name[/descriptor] 的精确
    // 方法/字段目标查询；类型引用只在类级语义下参与（类型引用不指向成员）。
    if (wantTo) {
        if (member.empty()) {
            for (const auto& reference : index.typeReferencesTo(classRef))
                pushReference(result, reference.site, reference.targetType.ref(), "type");
            for (const auto& reference : index.methodReferences()) {
                if (reference.target.owner != className.c_str()) continue;
                pushReference(result, reference);
            }
            for (const auto& reference : index.fieldReferences()) {
                if (reference.target.owner != className.c_str()) continue;
                pushReference(result, reference);
            }
        }
        else {
            if (memberDescriptor.empty()) {
                for (const auto& reference : index.methodReferences()) {
                    if (reference.target.owner != className.c_str() || reference.target.name != memberName.c_str())
                        continue;
                    pushReference(result, reference);
                }
                for (const auto& reference : index.fieldReferences()) {
                    if (reference.target.owner != className.c_str() || reference.target.name != memberName.c_str())
                        continue;
                    pushReference(result, reference);
                }
            }
            else {
                for (const auto& reference : index.methodReferencesTo(classRef, nameRef, descRef))
                    pushReference(result, reference);
                for (const auto& reference : index.fieldReferencesTo(classRef, nameRef, descRef))
                    pushReference(result, reference);
            }
        }
    }

    // from：该类方法体发出的全部引用；成员选择器过滤 sourceMethod[/Descriptor]。
    if (wantFrom) {
        const auto matchesSourceMember = [&](const workspace::ReferenceSite& site) {
            return member.empty() ||
                   (site.sourceMethod == nameRef &&
                    (memberDescriptor.empty() || site.sourceDescriptor == descRef));
        };
        for (const auto& reference : index.methodReferencesFromClass(classRef)) {
            if (!matchesSourceMember(reference.site)) continue;
            pushReference(result, reference);
        }
        for (const auto& reference : index.fieldReferencesFromClass(classRef)) {
            if (!matchesSourceMember(reference.site)) continue;
            pushReference(result, reference);
        }
        for (const auto& reference : index.typeReferencesFromClass(classRef)) {
            if (!matchesSourceMember(reference.site)) continue;
            pushReference(result, reference.site, reference.targetType.ref(), "type");
        }
        for (const auto& reference : index.dynamicReferences()) {
            if (reference.site.sourceClass != classRef || !matchesSourceMember(reference.site))
                continue;
            pushReference(result, reference.site, reference.target.ref(), "dynamic");
        }
    }

    deduplicateReferences(result);

    XrefReport report;
    report.jarPath   = jarPath;
    report.className = className;
    report.member    = member;
    report.direction = normalizedDirection;
    report.limit     = limit;
    report.total     = static_cast<ca::i64>(result.size());
    report.truncated = report.total > limit;
    result.resize(static_cast<std::size_t>(std::min<ca::i64>(report.total, limit)));
    report.references = std::move(result);
    return ca::Ok(std::move(report));
}

void renderXrefText(const XrefReport& report, OutputSink& sink)
{
    sink.puts(shared::trfStr("mjt.cli.xref.title", {report.jarPath}));
    sink.puts(shared::trfStr("mjt.cli.xref.class", {report.className}));
    if (!report.member.empty())
        sink.puts(shared::trfStr("mjt.cli.xref.member", {report.member}));
    sink.puts(shared::trfStr("mjt.cli.xref.direction", {report.direction}));
    std::string counted = std::to_string(static_cast<ca::i64>(report.references.size())) +
                          " " + shared::trfStr("mjt.cli.xref.of") + " " + std::to_string(report.total);
    if (report.truncated)
        // 截断提示带全量数：用户按 total 放大 --limit 一步到位。
        counted += " (" + shared::trfStr("mjt.cli.xref.truncated", {std::to_string(report.total)}) + ")";
    sink.puts(shared::trfStr("mjt.cli.xref.counted", {counted}));
    for (const auto& reference : report.references) {
        std::string source = reference.sourceClass.to_std_string();
        if (reference.sourceMethod.is_empty()) {
            source += " " + shared::trfStr("mjt.cli.xref.class_header");
        }
        else {
            source += "." + reference.sourceMethod.to_std_string() +
                      reference.sourceDescriptor.to_std_string() + "@" +
                      std::to_string(reference.instructionIndex);
        }
        std::string line = "  [" + reference.kind.to_std_string() + "] " + source + " -> " +
                           reference.target.to_std_string();
        if (reference.supporting)
            line += " [supporting:" + reference.supportId.to_std_string() + "]";
        sink.puts(line);
    }
}

void renderXrefJson(const XrefReport& report, OutputSink& sink)
{
    std::string out = "{\n";
    out += "  \"jar\": \"" + json::escape(report.jarPath) + "\",\n";
    out += "  \"class\": \"" + json::escape(report.className) + "\",\n";
    if (!report.member.empty())
        out += "  \"member\": \"" + json::escape(report.member) + "\",\n";
    out += "  \"direction\": \"" + json::escape(report.direction) + "\",\n";
    out += "  \"limit\": " + std::to_string(report.limit) + ",\n";
    out += "  \"total\": " + std::to_string(report.total) + ",\n";
    out += std::string("  \"truncated\": ") + (report.truncated ? "true" : "false") + ",\n";
    out += "  \"references\": [";
    for (std::size_t index = 0; index < report.references.size(); ++index) {
        const auto& reference = report.references[index];
        if (index != 0) out += ", ";
        out += "\n    {\"source_class\": \"" + json::escape(reference.sourceClass.to_std_string()) +
               "\", \"source_method\": \"" + json::escape(reference.sourceMethod.to_std_string()) +
               "\", \"source_descriptor\": \"" +
               json::escape(reference.sourceDescriptor.to_std_string()) +
               "\", \"instruction_index\": " + std::to_string(reference.instructionIndex) +
               ", \"kind\": \"" + json::escape(reference.kind.to_std_string()) +
               "\", \"target\": \"" + json::escape(reference.target.to_std_string()) + "\"}";
    }
    if (!report.references.empty()) out += "\n  ";
    out += "]\n}";
    sink.puts(out);
}

}   // namespace mjt::cli
