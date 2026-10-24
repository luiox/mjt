#include "pysdk_jasm.hpp"

// pysdk_jasm.cpp — Jasm 文本面实现（libjhasm 消费；无 pybind，绑定在 bind_facade）。

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/jasm/resource_jasm_service.hpp"
#include "mjbase/model/resource_model.hpp"

#include <charconv>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mjt::pysdk {

using mjbase::jasm::Diagnostic;
using mjbase::jasm::DiagnosticSeverity;
using mjbase::jasm::DiagnosticStage;
using mjbase::jasm::JvmJasmContext;
using mjbase::jasm::JvmJasmService;
using mjbase::jasm::ValidationMode;
using mjbase::model::ClassResource;
using mjbase::model::IResource;
using mjbase::model::ManifestResource;
using mjbase::model::ResourceContainer;
using mjbase::model::UnknownResource;

bool JasmReport::ok() const noexcept
{
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity == 0) return false;
    }
    return true;
}

namespace {

std::string toUtf8(const ca::str::Utf16String& value)
{
    return value.to_utf8_string().to_std_string();
}

ca::str::Utf16String toUtf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::vector<JasmDiagnostic> toJasmDiagnostics(const std::vector<Diagnostic>& diagnostics)
{
    std::vector<JasmDiagnostic> out;
    out.reserve(diagnostics.size());
    for (const auto& diagnostic : diagnostics) {
        JasmDiagnostic converted;
        converted.severity = static_cast<int>(diagnostic.severity);
        converted.stage    = static_cast<int>(diagnostic.stage);
        converted.message  = toUtf8(diagnostic.message);
        if (diagnostic.location.has_value()) {
            converted.line   = diagnostic.location->line;
            converted.column = diagnostic.location->column;
        }
        out.push_back(std::move(converted));
    }
    return out;
}

/// 全部 error 级诊断拍平成一行（读侧 fail-closed 异常文案）。
/// Diagnostic 不可拷贝（Utf16String 成员），只读引用逐条转写。
std::string flattenErrors(const std::vector<Diagnostic>& diagnostics)
{
    std::string out;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.severity != DiagnosticSeverity::Error) continue;
        if (!out.empty()) out += "; ";
        out += toUtf8(diagnostic.message);
    }
    return out.empty() ? "jasm failure" : out;
}

/// 类名 → 条目 URI（重名落选者不可寻址，与 class_bytes 同纪律）。
std::string jasmTargetUri(const FacadeSession& s, const std::string& name)
{
    if (s.duplicateUris.count(name)) {
        throw StaleRefError("ambiguous duplicate class name: " + name);
    }
    return facadeClassUri(s, name);
}

const ClassResource* classResourceAt(const FacadeSession& s, const std::string& uri)
{
    const auto* resource = s.container->getResource(uri);
    if (!resource || !ca::core::tag_cast::isa<ClassResource>(resource)) return nullptr;
    return ca::core::tag_cast::cast<const ClassResource>(resource);
}

/// 容器深拷贝（validate 的事务隔离面：试汇编发生在拷贝上，绝不落原容器）。
ResourceContainer cloneContainer(const ResourceContainer& container)
{
    ResourceContainer out;
    container.visitResources([&](std::string_view uri, const IResource& resource) {
        if (const auto* cr = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource)) {
            out.addResource(std::string(uri),
                            std::make_unique<ClassResource>(std::string(uri), cr->get()));
        }
        else if (const auto* mr = ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource)) {
            out.addResource(std::string(uri), std::make_unique<ManifestResource>(mr->get()));
        }
        else if (const auto* ur = ca::core::tag_cast::dyn_cast<const UnknownResource>(&resource)) {
            out.addResource(std::string(uri),
                            std::make_unique<UnknownResource>(std::string(uri), ur->get()));
        }
    });
    return out;
}

JasmClassText disassembleClass(const JvmJasmService& jasm, const std::vector<ca::u8>& bytes)
{
    const auto result = jasm.disassemble(bytes);
    JasmClassText text;
    if (!result.isOk()) {
        throw std::runtime_error("class jasm: " + flattenErrors(result.diagnostics));
    }
    text.internalName = result.value->internalName.to_std_string();
    text.source       = toUtf8(result.value->source);
    return text;
}

/// sourceName 缺省口径（与 MCP 侧一致）：<类名>.jasm。
std::string defaultSourceName(const std::string& name, const std::string& sourceName)
{
    return sourceName.empty() ? name + ".jasm" : sourceName;
}

/// validation 档位（与 MCP validation 键同词表）；非法值走参数错误。
ValidationMode parseValidation(const std::string& value)
{
    if (value == "relaxed") return ValidationMode::Relaxed;
    if (value == "strict") return ValidationMode::Strict;
    if (value == "off") return ValidationMode::Off;
    throw std::invalid_argument("validation must be one of relaxed, strict, off");
}

/// validate = 在容器拷贝上试汇编（名称校验 + 候选工作区复验，零提交）。
JasmReport validateOnClone(FacadeSession& s, const std::string& uri, const std::string& name,
                           const std::string& sourceName, const std::string& source,
                           ValidationMode validation)
{
    JvmJasmContext context;
    context.validationMode = validation;
    ResourceContainer  scratch   = cloneContainer(*s.container);
    const auto         assembled = mjbase::jasm::ResourceJasmService(scratch)
                                    .assemble(uri, toUtf16(defaultSourceName(name, sourceName))
                                                       .ref(),
                                              toUtf16(source).ref(), context);
    JasmReport report;
    report.diagnostics = toJasmDiagnostics(assembled.diagnostics);
    if (assembled.value.has_value()) {
        report.internalName = assembled.value->internalName.to_std_string();
        report.written      = assembled.value->bytes.size();
    }
    return report;
}

/// apply = 事务式替换 ClassResource（内部校验全过才 set）；成功后该类缓存树
/// 作废（外部改字节同款纪律——脏树未 write_back 的编辑就此让位）。
JasmReport applyInPlace(FacadeSession& s, const std::string& uri, const std::string& name,
                        const std::string& sourceName, const std::string& source,
                        ValidationMode validation)
{
    JvmJasmContext context;
    context.validationMode = validation;
    const auto assembled = mjbase::jasm::ResourceJasmService(*s.container)
                               .assemble(uri,
                                         toUtf16(defaultSourceName(name, sourceName)).ref(),
                                         toUtf16(source).ref(), context);
    JasmReport report;
    report.diagnostics = toJasmDiagnostics(assembled.diagnostics);
    if (assembled.isOk()) {
        report.internalName = assembled.value->internalName.to_std_string();
        report.written      = assembled.value->bytes.size();
        s.trees.erase(name);
    }
    return report;
}

JasmReport validateOrApply(FacadeSession& s, const std::string& uri, const std::string& name,
                           const std::string& sourceName, const std::string& source, bool apply,
                           ValidationMode validation)
{
    return apply ? applyInPlace(s, uri, name, sourceName, source, validation)
                 : validateOnClone(s, uri, name, sourceName, source, validation);
}

// ── unified diff（镜像 mjt/mcp/src/tool/diff.cpp 的解析与应用语义：严格
//    匹配、无 fuzz；两侧实现必须同进退，语义漂移即 bug） ──────────────────

enum class DiffLineKind
{
    Context,
    Removed,
    Added
};

struct JasmDiffLine
{
    DiffLineKind kind = DiffLineKind::Context;
    std::string  text;
};

struct JasmDiffHunk
{
    ca::usize              oldStart = 0, oldCount = 1;
    ca::usize              newStart = 0, newCount = 1;
    std::vector<JasmDiffLine> lines;
};

struct JasmDiffResult
{
    std::string text;
    std::string error;
    ca::usize   failedHunk = 0;
};

std::vector<std::string> diffSplitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string::size_type   pos = 0;
    while (pos < text.size()) {
        const auto next = text.find('\n', pos);
        if (next == std::string::npos) {
            lines.push_back(text.substr(pos));
            return lines;
        }
        lines.push_back(text.substr(pos, next - pos));
        pos = next + 1;
    }
    return lines;
}

std::string diffJoinLines(const std::vector<std::string>& lines)
{
    std::string out;
    ca::usize   total = lines.size();
    for (const auto& line : lines) total += line.size();
    out.reserve(total);
    for (const auto& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

bool parseHunkRange(const std::string& token, ca::usize& start, ca::usize& count,
                    std::string& error)
{
    const auto       comma    = token.find(',');
    const auto       startStr = token.substr(0, comma);
    if (startStr.empty()) {
        error = "hunk range is missing a start number";
        return false;
    }
    ca::usize parsed = 0;
    const auto res =
        std::from_chars(startStr.data(), startStr.data() + startStr.size(), parsed);
    if (res.ec != std::errc() || res.ptr != startStr.data() + startStr.size()) {
        error = "hunk range start is not a non-negative integer: " + startStr;
        return false;
    }
    start = parsed;
    if (comma == std::string::npos) {
        count = 1;
        return true;
    }
    const auto countStr = token.substr(comma + 1);
    if (countStr.empty()) {
        count = 1;
        return true;
    }
    ca::usize  parsedCount = 0;
    const auto res2 =
        std::from_chars(countStr.data(), countStr.data() + countStr.size(), parsedCount);
    if (res2.ec != std::errc() || res2.ptr != countStr.data() + countStr.size()) {
        error = "hunk range count is not a non-negative integer: " + countStr;
        return false;
    }
    count = parsedCount;
    return true;
}

bool parseHunkHeader(const std::string& line, JasmDiffHunk& hunk, std::string& error)
{
    const auto secondAt = line.find("@@", 2);
    if (line.compare(0, 2, "@@") != 0 || secondAt == std::string::npos) {
        error = "expected hunk header beginning with '@@ ... @@'";
        return false;
    }
    const auto     body = line.substr(2, secondAt - 2);
    std::istringstream iss(body);
    std::string    oldToken;
    std::string    newToken;
    iss >> oldToken >> newToken;
    if (oldToken.empty() || newToken.empty() || oldToken[0] != '-' || newToken[0] != '+') {
        error = "hunk header must contain both -old and +new ranges";
        return false;
    }
    if (!parseHunkRange(oldToken.substr(1), hunk.oldStart, hunk.oldCount, error)) return false;
    if (!parseHunkRange(newToken.substr(1), hunk.newStart, hunk.newCount, error)) return false;
    if (hunk.oldStart == 0 && hunk.oldCount != 0) {
        error = "hunk old range start must be 1-based (0 only allowed for empty file)";
        return false;
    }
    if (hunk.newStart == 0 && hunk.newCount != 0) {
        error = "hunk new range start must be 1-based (0 only allowed for empty file)";
        return false;
    }
    return true;
}

bool parseUnifiedDiff(const std::string& diff, std::vector<JasmDiffHunk>& hunks,
                      std::string& error)
{
    hunks.clear();
    error.clear();
    auto lines = diffSplitLines(diff);
    for (auto& line : lines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
    }
    ca::usize index = 0;
    while (index < lines.size() && lines[index].rfind("@@", 0) != 0) {
        ++index;
    }
    JasmDiffHunk current;
    bool         inHunk = false;
    for (; index < lines.size(); ++index) {
        const auto& line = lines[index];
        if (line.rfind("@@", 0) == 0) {
            if (inHunk) {
                hunks.push_back(std::move(current));
                current = JasmDiffHunk{};
            }
            if (!parseHunkHeader(line, current, error)) {
                error = "hunk #" + std::to_string(hunks.size() + 1) + " header: " + error;
                return false;
            }
            inHunk = true;
            continue;
        }
        if (!inHunk) {
            error = "unexpected line outside a hunk: " + line;
            return false;
        }
        if (line == "\\ No newline at end of file" || line.rfind("\\ ", 0) == 0) continue;
        if (line.empty()) {
            current.lines.push_back({DiffLineKind::Context, {}});
            continue;
        }
        const auto marker = line[0];
        if (marker == ' ') current.lines.push_back({DiffLineKind::Context, line.substr(1)});
        else if (marker == '-')
            current.lines.push_back({DiffLineKind::Removed, line.substr(1)});
        else if (marker == '+') current.lines.push_back({DiffLineKind::Added, line.substr(1)});
        else {
            error = "hunk line has an invalid prefix (must be ' ', '-' or '+'): " + line;
            return false;
        }
    }
    if (inHunk) hunks.push_back(std::move(current));
    return true;
}

JasmDiffResult applyUnifiedDiff(const std::string& base, const std::string& diff)
{
    JasmDiffResult           result;
    std::vector<JasmDiffHunk> hunks;
    if (!parseUnifiedDiff(diff, hunks, result.error)) return result;
    const auto               baseLines = diffSplitLines(base);
    ca::usize                cursor    = 0;
    std::vector<std::string> output;
    output.reserve(baseLines.size());
    for (ca::usize h = 0; h < hunks.size(); ++h) {
        const auto&       hunk     = hunks[h];
        const ca::usize   hunkBase = hunk.oldStart == 0 ? 0 : hunk.oldStart - 1;
        if (hunkBase < cursor) {
            std::ostringstream msg;
            msg << "hunk #" << (h + 1) << " overlaps or regresses: starts at base line "
                << (hunkBase + 1) << " but the previous hunk already consumed through line "
                << cursor;
            result.error      = msg.str();
            result.failedHunk = h + 1;
            return result;
        }
        if (hunkBase > baseLines.size()) {
            std::ostringstream msg;
            msg << "hunk #" << (h + 1) << " starts at base line " << (hunkBase + 1)
                << " but only " << baseLines.size() << " lines are available";
            result.error      = msg.str();
            result.failedHunk = h + 1;
            return result;
        }
        for (; cursor < hunkBase; ++cursor) output.push_back(baseLines[cursor]);
        ca::usize baseProbe = hunkBase;
        for (const auto& entry : hunk.lines) {
            if (entry.kind == DiffLineKind::Added) continue;
            if (baseProbe >= baseLines.size()) {
                std::ostringstream msg;
                msg << "hunk #" << (h + 1) << " expects base line " << (baseProbe + 1)
                    << " but only " << baseLines.size() << " lines are available";
                result.error      = msg.str();
                result.failedHunk = h + 1;
                return result;
            }
            if (baseLines[baseProbe] != entry.text) {
                std::ostringstream msg;
                msg << "hunk #" << (h + 1) << " context mismatch at base line "
                    << (baseProbe + 1) << ": expected " << entry.text << " but found "
                    << baseLines[baseProbe];
                result.error      = msg.str();
                result.failedHunk = h + 1;
                return result;
            }
            ++baseProbe;
        }
        cursor = baseProbe;
        for (const auto& entry : hunk.lines) {
            if (entry.kind == DiffLineKind::Removed) continue;
            output.push_back(entry.text);
        }
    }
    for (; cursor < baseLines.size(); ++cursor) output.push_back(baseLines[cursor]);
    result.text = diffJoinLines(output);
    return result;
}

/// diff 错误 → 单条 Resource 级 error 诊断（内容性错误走报告面，不抛）。
JasmReport diffErrorReport(const std::string& error, ca::usize failedHunk)
{
    JasmReport report;
    JasmDiagnostic diagnostic;
    diagnostic.severity = 0;
    diagnostic.stage    = static_cast<int>(DiagnosticStage::Resource);
    diagnostic.message =
        "diff apply failed (hunk #" + std::to_string(failedHunk) + "): " + error;
    report.diagnostics.push_back(std::move(diagnostic));
    return report;
}

/// 方法级 patch 主体：source（全量）或 diff（相对 method_jasm 产物）定出
/// 新方法文本 → 在类全文里恰好一处拼接 → 走 class 面 validate/apply。
JasmReport methodPatch(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, const std::string& sourceName,
                       const std::string& source, const std::string& diff, bool apply,
                       ValidationMode validation)
{
    const auto method = facadeMethodJasm(s, cls, name, desc);

    std::string newMethodSource;
    if (!source.empty() || diff.empty()) {
        newMethodSource = source;   // 全量形态；两者同空按空体处理（汇编面裁决）
    }
    else {
        const auto applied = applyUnifiedDiff(method.source, diff);
        if (!applied.error.empty()) return diffErrorReport(applied.error, applied.failedHunk);
        newMethodSource = applied.text;
    }

    const auto        uri     = jasmTargetUri(s, cls);
    JvmJasmService    jasm;
    const auto*       resource = classResourceAt(s, uri);
    if (!resource) throw StaleRefError("not a class entry: " + uri);
    const auto        classText = disassembleClass(jasm, resource->get());

    const auto first = classText.source.find(method.source);
    if (first == std::string::npos) {
        JasmDiagnostic diagnostic;
        diagnostic.severity = 0;
        diagnostic.stage    = static_cast<int>(DiagnosticStage::Resource);
        diagnostic.message  = "method source not found in class Jasm";
        JasmReport report;
        report.diagnostics.push_back(std::move(diagnostic));
        return report;
    }
    if (classText.source.find(method.source, first + 1) != std::string::npos) {
        JasmDiagnostic diagnostic;
        diagnostic.severity = 0;
        diagnostic.stage    = static_cast<int>(DiagnosticStage::Resource);
        diagnostic.message  = "method source is ambiguous in class Jasm";
        JasmReport report;
        report.diagnostics.push_back(std::move(diagnostic));
        return report;
    }
    std::string newClassSource = classText.source;
    newClassSource.replace(first, method.source.size(), newMethodSource);

    return validateOrApply(s, uri, cls, defaultSourceName(cls + "." + name, sourceName),
                           newClassSource, apply, validation);
}

}   // namespace

JasmClassText facadeClassJasm(FacadeSession& s, const std::string& name)
{
    const auto*  resource = classResourceAt(s, jasmTargetUri(s, name));
    if (!resource) throw StaleRefError("not a class entry: " + name);
    JvmJasmService jasm;
    return disassembleClass(jasm, resource->get());
}

JasmMethodText facadeMethodJasm(FacadeSession& s, const std::string& cls,
                                const std::string& name, const std::string& desc)
{
    const auto*    resource = classResourceAt(s, jasmTargetUri(s, cls));
    if (!resource) throw StaleRefError("not a class entry: " + cls);
    JvmJasmService jasm;
    const auto     result =
        jasm.disassembleMethod(resource->get(),
                               ca::str::Utf8StringRef::from_data(
                                   reinterpret_cast<const ca::u8*>(name.data()), name.size()),
                               ca::str::Utf8StringRef::from_data(
                                   reinterpret_cast<const ca::u8*>(desc.data()), desc.size()));
    if (!result.isOk()) {
        throw StaleRefError("method jasm: " + cls + "." + name + desc + ": "
                            + flattenErrors(result.diagnostics));
    }
    JasmMethodText text;
    text.internalName = result.value->internalName.to_std_string();
    text.name         = result.value->name.to_std_string();
    text.descriptor   = result.value->descriptor.to_std_string();
    text.source       = toUtf8(result.value->source);
    return text;
}

JasmReport facadeValidateJasm(FacadeSession& s, const std::string& name,
                              const std::string& sourceName, const std::string& source,
                              const std::string& validation)
{
    return validateOnClone(s, jasmTargetUri(s, name), name, sourceName, source,
                           parseValidation(validation));
}

JasmReport facadeApplyJasm(FacadeSession& s, const std::string& name,
                           const std::string& sourceName, const std::string& source,
                           const std::string& validation)
{
    return applyInPlace(s, jasmTargetUri(s, name), name, sourceName, source,
                        parseValidation(validation));
}

JasmReport facadeValidateJasmDiff(FacadeSession& s, const std::string& name,
                                  const std::string& diff, const std::string& validation)
{
    const auto base = facadeClassJasm(s, name).source;
    const auto applied = applyUnifiedDiff(base, diff);
    if (!applied.error.empty()) return diffErrorReport(applied.error, applied.failedHunk);
    return validateOnClone(s, jasmTargetUri(s, name), name, "", applied.text,
                           parseValidation(validation));
}

JasmReport facadeApplyJasmDiff(FacadeSession& s, const std::string& name,
                               const std::string& diff, const std::string& validation)
{
    const auto base     = facadeClassJasm(s, name).source;
    const auto applied = applyUnifiedDiff(base, diff);
    if (!applied.error.empty()) return diffErrorReport(applied.error, applied.failedHunk);
    return applyInPlace(s, jasmTargetUri(s, name), name, "", applied.text,
                        parseValidation(validation));
}

JasmReport facadeValidateMethodJasm(FacadeSession& s, const std::string& cls,
                                    const std::string& name, const std::string& desc,
                                    const std::string& sourceName, const std::string& source,
                                    const std::string& validation)
{
    return methodPatch(s, cls, name, desc, sourceName, source, "", false,
                       parseValidation(validation));
}

JasmReport facadeApplyMethodJasm(FacadeSession& s, const std::string& cls,
                                 const std::string& name, const std::string& desc,
                                 const std::string& sourceName, const std::string& source,
                                 const std::string& validation)
{
    return methodPatch(s, cls, name, desc, sourceName, source, "", true,
                       parseValidation(validation));
}

JasmReport facadeValidateMethodJasmDiff(FacadeSession& s, const std::string& cls,
                                        const std::string& name, const std::string& desc,
                                        const std::string& diff,
                                        const std::string& validation)
{
    return methodPatch(s, cls, name, desc, "", "", diff, false, parseValidation(validation));
}

JasmReport facadeApplyMethodJasmDiff(FacadeSession& s, const std::string& cls,
                                     const std::string& name, const std::string& desc,
                                     const std::string& diff, const std::string& validation)
{
    return methodPatch(s, cls, name, desc, "", "", diff, true, parseValidation(validation));
}

}   // namespace mjt::pysdk
