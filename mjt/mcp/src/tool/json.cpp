#include "json.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <utility>

#include "limits.hpp"

namespace mjt::mcp::detail {

using mjt::base::workspace::AnnotationElement;
using mjt::base::workspace::AnnotationInfo;
using mjt::base::workspace::ClassInfo;
using mjt::base::workspace::ClassSummary;
using mjt::base::workspace::FieldInfo;
using mjt::base::workspace::ImpactAnalysis;
using mjt::base::workspace::JarRepairResult;
using mjt::base::workspace::MethodInfo;
using mjt::base::workspace::MutationResult;
using mjt::base::workspace::SearchHit;
using mjt::base::workspace::SupportingResourceInfo;
using mjt::base::workspace::WorkflowReferenceKind;
using mjt::base::workspace::WorkflowResult;
using mjt::base::workspace::IWorkspace;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string utf8(ca::str::Utf16StringRef value)
{
    return value.to_utf8_string().to_std_string();
}

Json statusJson(const ca::Status& status)
{
    return {{"ok", status.is_ok()},
            {"code", static_cast<int>(status.code())},
            {"message", status.to_string()}};
}

Json diagnosticJson(const mjt::base::jasm::Diagnostic& diagnostic)
{
    Json result = {{"severity", diagnostic.severity == mjt::base::jasm::DiagnosticSeverity::Error
                                      ? "error"
                                      : "warning"},
                   {"stage", static_cast<int>(diagnostic.stage)},
                   {"message", utf8(diagnostic.message.ref())}};
    if (diagnostic.location) {
        result["location"] = {{"line", diagnostic.location->line},
                              {"column", diagnostic.location->column},
                              {"length", diagnostic.location->length}};
    }
    return result;
}

Json annotationElementJson(const AnnotationElement& element)
{
    Json children = Json::array();
    for (const auto& child : element.children) children.push_back(annotationElementJson(child));
    return {{"name", element.name},
            {"tag", std::string(1, element.tag)},
            {"value", element.value},
            {"annotation_descriptor", element.annotationDescriptor},
            {"children", std::move(children)}};
}

Json annotationsJson(const std::vector<AnnotationInfo>& annotations)
{
    Json result = Json::array();
    for (const auto& annotation : annotations) {
        Json elements = Json::array();
        for (const auto& element : annotation.elements) elements.push_back(annotationElementJson(element));
        result.push_back({{"descriptor", annotation.descriptor},
                          {"visible", annotation.visible},
                          {"elements", std::move(elements)}});
    }
    return result;
}

Json fieldJson(const FieldInfo& field)
{
    return {{"name", field.name},
            {"descriptor", field.descriptor},
            {"signature", field.signature},
            {"access", field.access},
            {"annotations", annotationsJson(field.annotations)}};
}

Json methodJson(const MethodInfo& method)
{
    Json exceptions = Json::array();
    for (const auto& exception : method.exceptions) exceptions.push_back(exception);
    Json locals = Json::array();
    for (const auto& local : method.localVariables) {
        locals.push_back({{"name", local.name},
                          {"descriptor", local.descriptor},
                          {"signature", local.signature},
                          {"index", local.index}});
    }
    Json lines = Json::array();
    for (const auto& line : method.lineNumbers)
        lines.push_back({{"instruction_index", line.instructionIndex}, {"line", line.line}});
    return {{"name", method.name},
            {"descriptor", method.descriptor},
            {"signature", method.signature},
            {"access", method.access},
            {"max_stack", method.maxStack},
            {"max_locals", method.maxLocals},
            {"instruction_count", method.instructionCount},
            {"try_catch_count", method.tryCatchCount},
            {"exceptions", std::move(exceptions)},
            {"locals", std::move(locals)},
            {"lines", std::move(lines)},
            {"annotations", annotationsJson(method.annotations)}};
}

Json supportingResourceJson(const SupportingResourceInfo& support)
{
    return {{"id", support.id},
            {"path", support.path},
            {"fingerprint", support.fingerprint},
            {"resource_count", support.resourceCount},
            {"class_count", support.classCount}};
}

Json classJson(const ClassInfo& clazz)
{
    Json interfaces = Json::array();
    Json fields = Json::array();
    Json methods = Json::array();
    Json innerClasses = Json::array();
    for (const auto& item : clazz.interfaces) interfaces.push_back(item);
    for (const auto& field : clazz.fields) fields.push_back(fieldJson(field));
    for (const auto& method : clazz.methods) methods.push_back(methodJson(method));
    for (const auto& item : clazz.innerClasses) innerClasses.push_back(item);
    return {{"resource_uri", clazz.resourceUri},
            {"internal_name", clazz.internalName},
            {"super_name", clazz.superName},
            {"signature", clazz.signature},
            {"access", clazz.access},
            {"version", clazz.version},
            {"interfaces", std::move(interfaces)},
            {"fields", std::move(fields)},
            {"methods", std::move(methods)},
            {"inner_classes", std::move(innerClasses)},
            {"annotations", annotationsJson(clazz.annotations)}};
}

Json classSummaryJson(const ClassSummary& clazz)
{
    return {{"resource_uri", clazz.resourceUri},
            {"internal_name", clazz.internalName},
            {"super_name", clazz.superName},
            {"access", clazz.access},
            {"version", clazz.version},
            {"interface_count", clazz.interfaceCount},
            {"field_count", clazz.fieldCount},
            {"method_count", clazz.methodCount}};
}

Json searchHitJson(const SearchHit& hit)
{
    return {{"resource_uri", hit.resourceUri},
            {"kind", hit.kind},
            {"owner", hit.owner},
            {"name", hit.name},
            {"descriptor", hit.descriptor},
            {"instruction_index", hit.instructionIndex},
            {"value", hit.value}};
}

Json mutationJson(const MutationResult& mutation)
{
    Json diagnostics = Json::array();
    Json diffs = Json::array();
    Json classes = Json::array();
    Json references = Json::array();
    for (const auto& diagnostic : mutation.diagnostics) diagnostics.push_back(diagnosticJson(diagnostic));
    for (const auto& diff : mutation.byteDiffs) {
        Json ranges = Json::array();
        for (const auto& range : diff.ranges) {
            ranges.push_back({{"offset", range.offset},
                              {"before_length", range.beforeLength},
                              {"after_length", range.afterLength}});
        }
        diffs.push_back({{"uri", diff.uri},
                         {"before_size", diff.beforeSize},
                         {"after_size", diff.afterSize},
                         {"ranges", std::move(ranges)}});
    }
    for (const auto& item : mutation.affectedClasses) classes.push_back(item);
    for (const auto& reference : mutation.affectedReferences)
        references.push_back({{"kind", reference.kind}, {"target", reference.target}});
    return {{"status", statusJson(mutation.status)},
            {"revision", mutation.revision},
            {"committed", mutation.committed},
            {"exportable", mutation.exportable},
            {"diagnostics", std::move(diagnostics)},
            {"byte_diffs", std::move(diffs)},
            {"affected_classes", std::move(classes)},
            {"affected_references", std::move(references)}};
}

Json jarRepairJson(const JarRepairResult& repair)
{
    Json result = mutationJson(repair.mutation);
    Json issues = Json::array();
    for (const auto& issue : repair.issues) {
        issues.push_back({{"uri", issue.uri},
                          {"kind", issue.kind},
                          {"message", issue.message},
                          {"size", issue.size},
                          {"selected_for_removal", issue.selectedForRemoval}});
    }
    result["resource_count"] = repair.resourceCount;
    result["class_count"] = repair.classCount;
    result["issues"] = std::move(issues);
    return result;
}

Json referenceJson(const mjt::base::workspace::ReferenceInfo& reference)
{
    return {{"source", {{"class", reference.sourceClass},
                         {"method", reference.sourceMethod},
                         {"descriptor", reference.sourceDescriptor},
                         {"instruction_index", reference.instructionIndex},
                         {"supporting", reference.supporting},
                         {"support_id", reference.supportId}}},
            {"target", reference.target},
            {"kind", reference.kind}};
}

const char* workflowReferenceKindName(WorkflowReferenceKind kind)
{
    switch (kind) {
    case WorkflowReferenceKind::Class: return "class";
    case WorkflowReferenceKind::Field: return "field";
    case WorkflowReferenceKind::Method: return "method";
    }
    return "unknown";
}

Json workflowJson(const WorkflowResult& workflow)
{
    Json stages = Json::array();
    Json references = Json::array();
    for (const auto& stage : workflow.stages)
        stages.push_back({{"name", stage.name}, {"status", statusJson(stage.status)}});
    for (const auto& report : workflow.references) {
        Json before = Json::array();
        Json after = Json::array();
        for (const auto& reference : report.before) before.push_back(referenceJson(reference));
        for (const auto& reference : report.after) after.push_back(referenceJson(reference));
        references.push_back({{"query", {{"kind", workflowReferenceKindName(report.query.kind)},
                                            {"owner", report.query.owner},
                                            {"name", report.query.name},
                                            {"descriptor", report.query.descriptor}}},
                              {"before", std::move(before)},
                              {"after", std::move(after)}});
    }
    return {{"mutation", mutationJson(workflow.mutation)},
            {"input_fingerprint", workflow.inputFingerprint},
            {"candidate_fingerprint", workflow.candidateFingerprint},
            {"export_path", workflow.exportPath},
            {"exported", workflow.exported},
            {"stages", std::move(stages)},
            {"references", std::move(references)}};
}

Json toolResult(Json payload, bool isError)
{
    const auto text = payload.dump();
    return {{"content", Json::array({{{"type", "text"}, {"text", text}}})},
            {"structuredContent", std::move(payload)},
            {"isError", isError}};
}

Json toolError(const std::string& message, const ca::Status* status)
{
    Json payload = {{"error", message}};
    if (status) payload["status"] = statusJson(*status);
    return toolResult(std::move(payload), true);
}

std::string base64(const std::vector<ca::u8>& value)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((value.size() + 2) / 3) * 4);
    for (ca::usize index = 0; index < value.size(); index += 3) {
        const auto first = value[index];
        const auto second = index + 1 < value.size() ? value[index + 1] : 0;
        const auto third = index + 2 < value.size() ? value[index + 2] : 0;
        result.push_back(kAlphabet[first >> 2]);
        result.push_back(kAlphabet[((first & 0x03) << 4) | (second >> 4)]);
        result.push_back(index + 1 < value.size() ? kAlphabet[((second & 0x0f) << 2) | (third >> 6)] : '=');
        result.push_back(index + 2 < value.size() ? kAlphabet[third & 0x3f] : '=');
    }
    return result;
}

bool decodeBase64(const std::string& encoded, std::vector<ca::u8>& output, std::string& error)
{
    constexpr ca::usize kMaximumEncodedClassSize =
        ((mjt::base::workspace::kMaximumClassReplacementSize + 2) / 3) * 4;
    if (encoded.empty()) {
        error = "content_base64 must not be empty";
        return false;
    }
    if (encoded.size() > kMaximumEncodedClassSize || encoded.size() % 4 != 0) {
        error = "content_base64 length is invalid or exceeds the class size limit";
        return false;
    }
    const auto value = [](unsigned char character) -> ca::i32 {
        if (character >= 'A' && character <= 'Z') return character - 'A';
        if (character >= 'a' && character <= 'z') return character - 'a' + 26;
        if (character >= '0' && character <= '9') return character - '0' + 52;
        if (character == '+') return 62;
        if (character == '/') return 63;
        return -1;
    };
    output.clear();
    output.reserve((encoded.size() / 4) * 3);
    for (ca::usize index = 0; index < encoded.size(); index += 4) {
        const bool last = index + 4 == encoded.size();
        const auto c0 = static_cast<unsigned char>(encoded[index]);
        const auto c1 = static_cast<unsigned char>(encoded[index + 1]);
        const auto c2 = static_cast<unsigned char>(encoded[index + 2]);
        const auto c3 = static_cast<unsigned char>(encoded[index + 3]);
        const auto v0 = value(c0);
        const auto v1 = value(c1);
        const bool pad2 = c2 == '=';
        const bool pad3 = c3 == '=';
        if (v0 < 0 || v1 < 0 || (!last && (pad2 || pad3)) || (pad2 && !pad3) ||
            (!pad2 && value(c2) < 0) || (!pad3 && value(c3) < 0)) {
            error = "content_base64 is not strict RFC 4648 base64";
            output.clear();
            return false;
        }
        const auto v2 = pad2 ? 0 : value(c2);
        const auto v3 = pad3 ? 0 : value(c3);
        if ((pad2 && (v1 & 0x0f) != 0) || (pad3 && !pad2 && (v2 & 0x03) != 0)) {
            error = "content_base64 has non-zero trailing bits";
            output.clear();
            return false;
        }
        output.push_back(static_cast<ca::u8>((v0 << 2) | (v1 >> 4)));
        if (!pad2) output.push_back(static_cast<ca::u8>((v1 << 4) | (v2 >> 2)));
        if (!pad3) output.push_back(static_cast<ca::u8>((v2 << 6) | v3));
    }
    if (output.size() > mjt::base::workspace::kMaximumClassReplacementSize) {
        error = "decoded class exceeds the class size limit";
        output.clear();
        return false;
    }
    return true;
}

bool parseCursor(const Json& arguments, ca::usize total, ca::usize& offset)
{
    if (!arguments.contains("cursor")) return true;
    //非 schema 校验路径（如内部调用）传入非字符串时 get_ref 会抛 type_error；
    //与空串一样按非法 cursor 处理，退化到空尾页。
    if (!arguments.at("cursor").is_string()) return false;
    const auto& cursor = arguments.at("cursor").get_ref<const std::string&>();
    if (cursor.empty()) return false;
    ca::usize parsed = 0;
    const auto result = std::from_chars(cursor.data(), cursor.data() + cursor.size(), parsed);
    if (result.ec != std::errc() || result.ptr != cursor.data() + cursor.size()) return false;
    offset = std::min(parsed, total);
    return true;
}

Json page(Json values, const Json& arguments)
{
    const auto total = values.size();
    ca::usize offset = arguments.value("offset", static_cast<ca::usize>(0));
    //非法/越界 cursor 一律退化到空尾页（offset=total），与原 try/catch 语义一致；
    //统一走 parseCursor 后不再有裸 stoull 抛异常的风险。
    parseCursor(arguments, total, offset);
    ca::usize limit = arguments.value("limit", static_cast<ca::usize>(100));
    limit = std::min(limit, kMaximumPageSize);
    offset = std::min(offset, total);
    const auto end = std::min(total, offset + limit);
    Json items = Json::array();
    for (ca::usize index = offset; index < end; ++index) items.push_back(std::move(values[index]));
    const auto received = items.size();
    Json result = {{"items", std::move(items)}, {"offset", offset}, {"total", total}};
    if (end < total) {
        //P2-3 partial 侧车：分页截断时结果文档自带 partial 元数据，下游抓取产物
        //可据此区分「截断续页」与「完整空/小结果」。仅截断路径写入，完整页
        //（无 next_cursor）零变化。
        const auto cursor = std::to_string(end);
        result["next_cursor"] = cursor;
        result["partial"] = true;
        result["reason"] = "truncated";
        result["received"] = received;
        result["cursor"] = cursor;
    }
    return result;
}

Json classSummaryPage(const std::vector<ClassSummary>& values, const Json& arguments)
{
    const auto total  = values.size();
    ca::usize  offset = arguments.value("offset", static_cast<ca::usize>(0));
    parseCursor(arguments, total, offset);
    const auto limit =
        std::min(arguments.value("limit", static_cast<ca::usize>(100)), kMaximumPageSize);
    offset                  = std::min(offset, total);
    const auto requestedEnd = std::min(total, offset + limit);

    Json      items        = Json::array();
    ca::usize encodedBytes = 512;
    ca::usize end          = offset;
    for (; end < requestedEnd; ++end) {
        auto       item      = classSummaryJson(values[end]);
        const auto itemBytes = item.dump().size() + 1;
        if (!items.empty() && encodedBytes + itemBytes > kMaximumClassListPayloadBytes) break;
        encodedBytes += itemBytes;
        items.push_back(std::move(item));
    }

    const auto received = items.size();
    Json result = {{"items", std::move(items)}, {"offset", offset}, {"total", total}};
    if (end < total) {
        //同 page：截断路径写 partial 侧车（P2-3），完整页零变化。received 取
        //实际编码进本页的条数（payload 字节预算可早于 limit 提前截断）。
        const auto cursor = std::to_string(end);
        result["next_cursor"] = cursor;
        result["partial"] = true;
        result["reason"] = "truncated";
        result["received"] = received;
        result["cursor"] = cursor;
    }
    return result;
}

Json impactJson(const ImpactAnalysis& impact)
{
    Json before = Json::array();
    Json after = Json::array();
    Json diagnostics = Json::array();
    Json risks = Json::array();
    for (const auto& item : impact.beforeReferences)
        before.push_back({{"kind", item.kind}, {"target", item.target}});
    for (const auto& item : impact.afterReferences)
        after.push_back({{"kind", item.kind}, {"target", item.target}});
    for (const auto& item : impact.diagnostics) diagnostics.push_back(diagnosticJson(item));
    for (const auto& item : impact.risks) risks.push_back(item);
    Json diffs = Json::array();
    for (const auto& diff : impact.byteDiffs)
        diffs.push_back({{"uri", diff.uri}, {"before_size", diff.beforeSize}, {"after_size", diff.afterSize}});
    return {{"status", statusJson(impact.status)},
            {"exportable", impact.exportable},
            {"before_references", std::move(before)},
            {"after_references", std::move(after)},
            {"byte_diffs", std::move(diffs)},
            {"diagnostics", std::move(diagnostics)},
            {"risks", std::move(risks)}};
}

Json normalizeToolError(const std::string& name, const Json& arguments, Json result,
                        const IWorkspace* workspace)
{
    if (!result.value("isError", false) || !result.contains("structuredContent")) return result;
    auto& payload = result["structuredContent"];
    payload["operation"] = name;
    payload["workspace_id"] = arguments.contains("workspace_id") &&
                                        arguments.at("workspace_id").is_string()
                                    ? arguments.at("workspace_id")
                                    : Json("");
    payload["revision"] = workspace ? workspace->revision() : 0;
    if (!payload.contains("diagnostics")) payload["diagnostics"] = Json::array();
    if (payload.contains("status") && payload.at("status").is_object() &&
        payload.at("status").contains("code")) {
        payload["status_code"] = payload.at("status").at("code");
    }
    else {
        const auto message = payload.value("error", std::string("tool failed"));
        const auto code = message.rfind("unknown tool:", 0) == 0 ? ca::StatusCode::NOT_FOUND
                                                                 : ca::StatusCode::INVALID_ARGUMENT;
        const auto status = ca::ErrStatus(code, message);
        payload["status"] = statusJson(status);
        payload["status_code"] = static_cast<int>(code);
    }
    //P2-3 partial 侧车：超时路径（DEADLINE_EXCEEDED，反编译/工作流子进程超时）
    //在错误文档自带 partial 元数据，下游抓取产物可据此区分「超时失败」与
    //「即时业务错误」。仅超时路径写入，其余错误与全部成功路径零变化；
    //received/total/cursor 对超时无意义，按侧车 schema 省略。
    if (payload.at("status_code").is_number_integer() &&
        payload.at("status_code").get<int>() == static_cast<int>(ca::StatusCode::DEADLINE_EXCEEDED)) {
        payload["partial"] = true;
        payload["reason"] = "timeout";
    }
    if (result.contains("content") && result.at("content").is_array() &&
        !result.at("content").empty())
        result["content"][0]["text"] = payload.dump();
    return result;
}

}   //namespace mjt::mcp::detail
