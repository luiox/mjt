#include "json.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <utility>

#include "limits.hpp"
#include "shared/status_convert.hpp"

namespace mjt::mcp::detail {

using mjt::workspace::AnnotationElement;
using mjt::workspace::AnnotationInfo;
using mjt::workspace::ClassInfo;
using mjt::workspace::ClassSummary;
using mjt::workspace::FieldInfo;
using mjt::workspace::ImpactAnalysis;
using mjt::workspace::JarRepairResult;
using mjt::workspace::MethodInfo;
using mjt::workspace::MutationResult;
using mjt::workspace::SearchHit;
using mjt::workspace::SupportingResourceInfo;
using mjt::workspace::WorkflowReferenceKind;
using mjt::workspace::WorkflowResult;
using mjt::workspace::IWorkspace;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string utf8(ca::str::Utf16StringRef value)
{
    return value.to_utf8_string().to_std_string();
}

std::string lowercased(const std::string& value)
{
    auto lowered = value;
    std::transform(lowered.begin(),
                   lowered.end(),
                   lowered.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return lowered;
}

Json statusJson(const ca::Status& status)
{
    return {{"ok", status.is_ok()},
            {"code", static_cast<int>(status.code())},
            {"message", status.to_string()}};
}

namespace {

/// @brief 阶段稳定名与归因门：编辑门（workspace 候选聚合校验）与重建
///        管线（jasm 汇编/反汇编/validate）分属不同 gate，客户端据此
///        区分诊断来源（两者都随 validation 三档开关）。
std::pair<const char*, const char*> stageNameAndGate(mjbase::jasm::DiagnosticStage stage)
{
    using mjbase::jasm::DiagnosticStage;
    switch (stage) {
    case DiagnosticStage::Assemble: return {"assemble", "rebuild"};
    case DiagnosticStage::Disassemble: return {"disassemble", "rebuild"};
    case DiagnosticStage::Validate: return {"validate", "rebuild"};
    case DiagnosticStage::Resource: return {"resource", "resource"};
    case DiagnosticStage::EditGate: return {"edit_gate", "edit"};
    }
    return {"unknown", "unknown"};
}

}   // namespace

Json diagnosticJson(const mjbase::jasm::Diagnostic& diagnostic)
{
    const auto [stageName, gate] = stageNameAndGate(diagnostic.stage);
    Json result = {{"severity", diagnostic.severity == mjbase::jasm::DiagnosticSeverity::Error
                                      ? "error"
                                      : "warning"},
                   {"stage", static_cast<int>(diagnostic.stage)},
                   {"stage_name", stageName},
                   {"gate", gate},
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
    //DTO 文本字段已换型 Utf8String：Json 投影显式 to_std_string（nlohmann
    //会把带 begin/end 的类型吃成码点数组，禁止隐式直通）。
    return {{"name", element.name.to_std_string()},
            {"tag", std::string(1, element.tag)},
            {"value", element.value.to_std_string()},
            {"annotation_descriptor", element.annotationDescriptor.to_std_string()},
            {"children", std::move(children)}};
}

Json annotationsJson(const std::vector<AnnotationInfo>& annotations)
{
    Json result = Json::array();
    for (const auto& annotation : annotations) {
        Json elements = Json::array();
        for (const auto& element : annotation.elements) elements.push_back(annotationElementJson(element));
        result.push_back({{"descriptor", annotation.descriptor.to_std_string()},
                          {"visible", annotation.visible},
                          {"elements", std::move(elements)}});
    }
    return result;
}

Json fieldJson(const FieldInfo& field)
{
    return {{"name", field.name.to_std_string()},
            {"descriptor", field.descriptor.to_std_string()},
            {"signature", field.signature.to_std_string()},
            {"access", field.access},
            {"annotations", annotationsJson(field.annotations)}};
}

Json methodJson(const MethodInfo& method)
{
    Json exceptions = Json::array();
    for (const auto& exception : method.exceptions)
        exceptions.push_back(exception.to_std_string());
    Json locals = Json::array();
    for (const auto& local : method.localVariables) {
        locals.push_back({{"name", local.name.to_std_string()},
                          {"descriptor", local.descriptor.to_std_string()},
                          {"signature", local.signature.to_std_string()},
                          {"index", local.index}});
    }
    Json lines = Json::array();
    for (const auto& line : method.lineNumbers)
        lines.push_back({{"instruction_index", line.instructionIndex}, {"line", line.line}});
    return {{"name", method.name.to_std_string()},
            {"descriptor", method.descriptor.to_std_string()},
            {"signature", method.signature.to_std_string()},
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
    //role 透传：support 是缺省遗留值，library/runtime 由角色化登记写入。
    return {{"role", support.role.is_empty() ? "support" : support.role.to_std_string()},
            {"id", support.id.to_std_string()},
            {"path", support.path.to_std_string()},
            {"fingerprint", support.fingerprint.to_std_string()},
            {"resource_count", support.resourceCount},
            {"class_count", support.classCount}};
}

Json classJson(const ClassInfo& clazz)
{
    Json interfaces = Json::array();
    Json fields = Json::array();
    Json methods = Json::array();
    Json innerClasses = Json::array();
    for (const auto& item : clazz.interfaces) interfaces.push_back(item.to_std_string());
    for (const auto& field : clazz.fields) fields.push_back(fieldJson(field));
    for (const auto& method : clazz.methods) methods.push_back(methodJson(method));
    for (const auto& item : clazz.innerClasses) innerClasses.push_back(item.to_std_string());
    return {{"resource_uri", clazz.resourceUri.to_std_string()},
            {"class", clazz.internalName.to_std_string()},
            {"super_name", clazz.superName.to_std_string()},
            {"signature", clazz.signature.to_std_string()},
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
    //键统一：类级响应行的类名键一律 class（内部名），不再用 internal_name。
    return {{"resource_uri", clazz.resourceUri.to_std_string()},
            {"class", clazz.internalName.to_std_string()},
            {"super_name", clazz.superName.to_std_string()},
            {"access", clazz.access},
            {"version", clazz.version},
            {"interface_count", clazz.interfaceCount},
            {"field_count", clazz.fieldCount},
            {"method_count", clazz.methodCount}};
}

bool parseInstructionHitValue(const std::string& value, InstructionHitParts& output)
{
    //mjbase 拼接口径：`<mnemonic> <node_type> <opcode>[ 操作数…]`，单空格
    //分词；node_type 槽由 mjbase instructionNodeType 语义归类（invoke/field/
    //ldc/cp_ref/…），此处只消费不重建，跳过。
    output = InstructionHitParts{};
    std::vector<std::string> tokens;
    std::string::size_type begin = 0;
    while (begin <= value.size()) {
        auto end = value.find(' ', begin);
        if (end == std::string::npos) end = value.size();
        if (end > begin) tokens.push_back(value.substr(begin, end - begin));
        begin = end + 1;
    }
    if (tokens.size() < 3) return false;
    output.mnemonic = tokens[0];
    if (!tokens[2].empty() &&
        std::all_of(tokens[2].begin(), tokens[2].end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        })) {
        for (std::size_t index = 3; index < tokens.size(); ++index) {
            if (index > 3) output.operand += ' ';
            output.operand += tokens[index];
        }
    }
    else {
        //token[2] 不是 opcode（防御）：把 token[2..] 都当操作数。
        for (std::size_t index = 2; index < tokens.size(); ++index) {
            if (index > 2) output.operand += ' ';
            output.operand += tokens[index];
        }
    }
    //cst 只对 CP 支持的助记符有意义：常量/引用摘要 = 操作数原文。
    static const std::array<const char*, 17> kConstantPoolOps = {
        "ldc", "ldc_w", "ldc2_w", "getstatic", "putstatic", "getfield", "putfield",
        "invokevirtual", "invokespecial", "invokestatic", "invokeinterface",
        "invokedynamic", "new", "anewarray", "checkcast", "instanceof", "multianewarray"};
    const bool constantBacked =
        std::any_of(kConstantPoolOps.begin(), kConstantPoolOps.end(), [&](const char* op) {
            return output.mnemonic == op;
        });
    output.cst = constantBacked ? output.operand : std::string{};
    return true;
}

std::optional<BranchKeys> branchKeysFor(const std::string& tool,
                                        const std::string& kind,
                                        const std::string& target,
                                        const std::string& direction)
{
    //分支合法键表（单一事实源）：contract 校验层与分发层共用。键序沿用
    //各分支的历史声明序（分页五键在前），错误文案按声明序展示。
    const auto keys = [](std::initializer_list<const char*> values) {
        std::vector<std::string> out;
        for (const auto* value : values) out.push_back(value);
        return out;
    };
    if (tool == "class_query") {
        //分支定位键契约（键统一裁决：类定位键 class、成员定位键 owner+name+
        //descriptor、资源键 uri）；bytes/resource_bytes 的 limit/cursor/offset
        //是字节分块键，list 上是分页键。
        BranchKeys branch;
        if (kind == "list")
            branch.supported = keys({"cursor", "offset", "limit", "page_size", "fields"});
        else if (kind == "class" || kind == "bytes" || kind == "jasm") {
            branch.selectors = keys({"class"});
            if (kind == "bytes") branch.supported = keys({"cursor", "offset", "limit"});
        }
        else if (kind == "resource_bytes") {
            branch.selectors = keys({"uri"});
            branch.supported = keys({"cursor", "offset", "limit"});
        }
        else if (kind == "method" || kind == "bytecode" || kind == "method_jasm") {
            //descriptor 可省略——省略时按宿主类名称解析回填（唯一命中
            //即用；重载歧义/名字不存在报错），降级为可选定位键。
            branch.selectors = keys({"owner", "name"});
            branch.optionalSelectors = keys({"descriptor"});
        }
        else if (kind != "manifest") return std::nullopt;
        return branch;
    }
    if (tool == "search") {
        //shadow 分支不吃分页（响应是 matches 列表不是 page 文档），其余分支
        //带分页五键。
        const std::vector<std::string> common =
            keys({"cursor", "offset", "limit", "page_size", "fields"});
        const auto withCommon = [&](std::initializer_list<const char*> extra) {
            std::vector<std::string> supported = common;
            for (const auto* key : extra) supported.push_back(key);
            return supported;
        };
        BranchKeys branch;
        if (kind == "classes")
            branch.supported = withCommon({"query", "match_mode", "case_sensitive",
                                           "package_prefix", "role"});
        else if (kind == "members")
            branch.supported = withCommon({"query", "match_mode", "case_sensitive", "owner",
                                           "package_prefix", "member_kind", "role"});
        else if (kind == "strings")
            branch.supported = withCommon({"query", "match_mode", "case_sensitive", "owner",
                                           "package_prefix", "string_kind", "role"});
        else if (kind == "numbers")
            branch.supported = withCommon({"query", "case_sensitive", "owner",
                                           "package_prefix", "opcode", "role"});
        else if (kind == "instructions")
            branch.supported = withCommon({"query", "match_mode", "case_sensitive", "owner",
                                           "package_prefix", "opcode", "mnemonic", "operand",
                                           "cst", "role"});
        else if (kind == "resources")
            branch.supported = withCommon({"query", "match_mode", "case_sensitive",
                                           "include_classes", "role"});
        else if (kind == "shadow")
            branch.supported =
                withCommon({"name", "owner", "direction", "symbol_kind", "file", "tsv_path"});
        else return std::nullopt;
        return branch;
    }
    if (tool == "xref_query") {
        BranchKeys branch;
        if (kind == "relations")
            branch.selectors = keys({"class"});
        else if (kind == "overrides")
            branch.selectors = keys({"owner", "name", "descriptor"});
        else if (kind == "references") {
            const std::vector<std::string> common =
                keys({"cursor", "offset", "limit", "page_size", "fields", "role", "direction"});
            if (target == "class") {
                branch.selectors = keys({"class"});
                branch.supported = common;
            }
            else if (target == "method" || target == "field") {
                //descriptor 可省略（名称解析回填，同 class_query 成员
                //kind 口径）。
                branch.selectors = keys({"owner", "name"});
                branch.optionalSelectors = keys({"descriptor"});
                branch.supported = common;
                //resolved 只在 direction=from（callees 展开解析目标）生效。
                if (target == "method" && direction == "from")
                    branch.supported.push_back("resolved");
            }
            else return std::nullopt;
        }
        else return std::nullopt;
        return branch;
    }
    return std::nullopt;
}

std::optional<Json> checkBranchKeys(const std::string& label,
                                    const Json& arguments,
                                    bool selectorAware,
                                    const std::vector<std::string>& selectors,
                                    const std::vector<std::string>& supportedKeys,
                                    const std::vector<std::string>& optionalSelectors)
{
    //定位键词表（键统一裁决）：class/owner/name/descriptor/uri。分支契约外的
    //词表键触发 requires/got 文案；词表外键触发 does not support 文案。
    static const std::array<const char*, 5> kSelectorVocabulary = {
        "class", "owner", "name", "descriptor", "uri"};
    const auto contains = [](const std::vector<std::string>& values, const std::string& key) {
        return std::find(values.begin(), values.end(), key) != values.end();
    };
    const auto quotedList = [](const std::vector<std::string>& values) {
        std::string joined;
        for (const auto& value : values) {
            if (!joined.empty()) joined += ", ";
            joined += value;
        }
        return joined;
    };
    const auto specText = [&]() {
        if (selectors.empty()) return std::string("no selector keys");
        if (selectors.size() == 1) return selectors.front();
        std::string joined;
        for (std::size_t index = 0; index + 1 < selectors.size(); ++index) {
            if (!joined.empty()) joined += ", ";
            joined += selectors[index];
        }
        return joined + " and " + selectors.back();
    }();

    std::vector<std::string> wrongSelectors;
    std::vector<std::string> missingRequired;
    if (selectorAware) {
        for (const auto& selector : selectors) {
            const auto found = arguments.find(selector);
            if (found == arguments.end() || found->is_null()) missingRequired.push_back(selector);
        }
        for (const auto& [key, _] : arguments.items()) {
            (void)_;
            const bool inVocabulary = std::any_of(kSelectorVocabulary.begin(),
                                                  kSelectorVocabulary.end(),
                                                  [&](const char* name) { return key == name; });
            //可选定位键不进 requires spec，也不算契约外定位键。
            if (inVocabulary && !contains(selectors, key) &&
                !contains(optionalSelectors, key))
                wrongSelectors.push_back(key);
        }
    }
    if (!wrongSelectors.empty() || !missingRequired.empty()) {
        std::sort(wrongSelectors.begin(), wrongSelectors.end());
        std::string message = label + " requires " + specText;
        if (!wrongSelectors.empty()) message += "; got " + quotedList(wrongSelectors);
        return toolError(message);
    }

    std::vector<std::string> unsupported;
    for (const auto& [key, _] : arguments.items()) {
        (void)_;
        //kind/target 是分支选择器本身（分支名已含其值），project_id 是会话
        //键，timeout_ms 是工具级超时面（跨分支键，schema 层管取值范围）：四者
        //恒放行；target 的枚举合法性由翻译层在白名单检查前裁定。
        if (key == "project_id" || key == "kind" || key == "target" || key == "timeout_ms")
            continue;
        const bool inVocabulary = std::any_of(kSelectorVocabulary.begin(),
                                              kSelectorVocabulary.end(),
                                              [&](const char* name) { return key == name; });
        if (inVocabulary || contains(supportedKeys, key)) continue;
        unsupported.push_back(key);
    }
    if (!unsupported.empty()) {
        std::sort(unsupported.begin(), unsupported.end());
        auto allowed = supportedKeys;
        allowed.push_back("target");
        allowed.insert(allowed.end(), selectors.begin(), selectors.end());
        allowed.insert(allowed.end(), optionalSelectors.begin(), optionalSelectors.end());
        std::sort(allowed.begin(), allowed.end());
        return toolError(label + " does not support key(s) " + quotedList(unsupported) +
                         "; supported: " + quotedList(allowed));
    }
    return std::nullopt;
}

std::string selectorKeyError(const std::string& key,
                             const std::string& value,
                             const std::vector<std::string>& expected)
{
    std::string joined;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (index > 0) joined += index + 1 == expected.size() ? " or " : ", ";
        joined += expected[index];
    }
    if (value.empty()) return key + ": Missing key (expected " + joined + ")";
    return key + ": Invalid value \"" + value + "\" (expected " + joined + ")";
}

Json searchHitJson(const SearchHit& hit)
{
    //键统一：类级行类名键 = class（内部名），成员级行宿主类键 = owner。
    Json result = {{"resource_uri", hit.resourceUri.to_std_string()},
                   {"kind", hit.kind.to_std_string()}};
    if (hit.kind == "class") result["class"] = hit.owner.to_std_string();
    else result["owner"] = hit.owner.to_std_string();
    result["name"] = hit.name.to_std_string();
    result["descriptor"] = hit.descriptor.to_std_string();
    result["instruction_index"] = hit.instructionIndex;
    result["value"] = hit.value.to_std_string();
    //strings 分面：string_literal = 串常量（ldc 加载的 constant_pool_string
    //与非 class 资源可打印串）；裸 UTF8 行按内容形态细分（classref/descriptor/
    //name）——属性名、方法名、字面量背书不再混进 classref。
    if (hit.kind == "constant_pool_string" || hit.kind == "resource_string")
        result["string_kind"] = "string_literal";
    else if (hit.kind == "constant_pool_utf8")
        result["string_kind"] = utf8StringKind(hit.value.to_std_string());
    //instructions 结构化摘要：从拼接文本 value 拆出 mnemonic/operand/cst，
    //替代拼接文本当唯一判别信息。
    if (hit.kind == "instruction") {
        InstructionHitParts parts;
        if (parseInstructionHitValue(hit.value.to_std_string(), parts)) {
            result["mnemonic"] = parts.mnemonic;
            result["operand"]  = parts.operand;
            result["cst"]      = parts.cst;
        }
    }
    return result;
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
        diffs.push_back({{"uri", diff.uri.to_std_string()},
                         {"before_size", diff.beforeSize},
                         {"after_size", diff.afterSize},
                         {"ranges", std::move(ranges)}});
    }
    for (const auto& item : mutation.affectedClasses) classes.push_back(item.to_std_string());
    for (const auto& reference : mutation.affectedReferences)
        references.push_back({{"kind", reference.kind.to_std_string()},
                              {"target", reference.target.to_std_string()}});
    Json identity = {{"annotated", mutation.identity.annotated},
                     {"migrated_classes", mutation.identity.migratedClasses},
                     {"migrated_members", mutation.identity.migratedMembers},
                     {"skipped", mutation.identity.skipped},
                     {"attempted", mutation.identity.attempted},
                     {"failed", mutation.identity.failed}};
    if (!mutation.identityWarning.empty()) identity["warning"] = mutation.identityWarning;
    // 方法指纹版本与编辑增量同步的计数披露（落库失败不回滚编辑，靠响应面
    // 可见；method_versions 随增量同步批一并补上历史缺档）。
    Json methodVersions = {{"recorded", mutation.methodVersions.recorded},
                           {"unchanged", mutation.methodVersions.unchanged},
                           {"skipped", mutation.methodVersions.skipped},
                           {"failed", mutation.methodVersions.failed}};
    Json analysisSync = {{"entries", mutation.analysisSync.entries},
                         {"classes", mutation.analysisSync.classes},
                         {"members", mutation.analysisSync.members},
                         {"references", mutation.analysisSync.references},
                         {"slices_cleared", mutation.analysisSync.slicesCleared},
                         {"skipped", mutation.analysisSync.skipped},
                         {"failed", mutation.analysisSync.failed}};
    return {{"status", statusJson(shared::toCaStatus(mutation.status))},
            {"revision", mutation.revision},
            {"committed", mutation.committed},
            {"exportable", mutation.exportable},
            {"diagnostics", std::move(diagnostics)},
            {"byte_diffs", std::move(diffs)},
            {"affected_classes", std::move(classes)},
            {"affected_references", std::move(references)},
            {"method_versions", std::move(methodVersions)},
            {"identity", std::move(identity)},
            {"analysis_sync", std::move(analysisSync)}};
}

Json jarRepairJson(const JarRepairResult& repair)
{
    Json result = mutationJson(repair.mutation);
    Json issues = Json::array();
    for (const auto& issue : repair.issues) {
        issues.push_back({{"uri", issue.uri.to_std_string()},
                          {"kind", issue.kind.to_std_string()},
                          {"message", issue.message.to_std_string()},
                          {"size", issue.size},
                          {"selected_for_removal", issue.selectedForRemoval}});
    }
    result["resource_count"] = repair.resourceCount;
    result["class_count"] = repair.classCount;
    result["issues"] = std::move(issues);
    return result;
}

Json referenceJson(const mjt::workspace::ReferenceInfo& reference)
{
    return {{"source", {{"class", reference.sourceClass.to_std_string()},
                         {"method", reference.sourceMethod.to_std_string()},
                         {"descriptor", reference.sourceDescriptor.to_std_string()},
                         {"instruction_index", reference.instructionIndex},
                         {"supporting", reference.supporting},
                         {"support_id", reference.supportId.to_std_string()}}},
            {"target", reference.target.to_std_string()},
            {"kind", reference.kind.to_std_string()}};
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
        stages.push_back({{"name", stage.name.to_std_string()},
                          {"status", statusJson(shared::toCaStatus(stage.status))}});
    for (const auto& report : workflow.references) {
        Json before = Json::array();
        Json after = Json::array();
        for (const auto& reference : report.before) before.push_back(referenceJson(reference));
        for (const auto& reference : report.after) after.push_back(referenceJson(reference));
        references.push_back({{"query", {{"kind", workflowReferenceKindName(report.query.kind)},
                                            {"owner", report.query.owner.to_std_string()},
                                            {"name", report.query.name.to_std_string()},
                                            {"descriptor", report.query.descriptor.to_std_string()}}},
                              {"before", std::move(before)},
                              {"after", std::move(after)}});
    }
    return {{"mutation", mutationJson(workflow.mutation)},
            {"input_fingerprint", workflow.inputFingerprint.to_std_string()},
            {"candidate_fingerprint", workflow.candidateFingerprint.to_std_string()},
            {"export_path", workflow.exportPath.to_std_string()},
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

Json timeoutHints(const std::string& name, const Json& arguments)
{
    //工具级超时的诊断提示（#71）：超时错误不再静默挂 deadline——按工具与
    //分支给收敛/分页/粒度指引，调用方据此改参重试。仅超时路径产出；成功
    //路径零变化。指引只描述真实存在的通道：deadline 四工具转孤儿后结果
    //无处可取，因此不承诺「进度可查」（decompile 的 async job 才有轮询面，
    //见 decompiler.cpp 的超时出口）。
    Json hints = Json::array();
    const auto push = [&hints](const char* text) { hints.push_back(text); };
    const auto timeoutBudget = [&push] {
        push("raise timeout_ms (max 300000) if this corpus legitimately needs longer");
    };
    if (name == "search") {
        push("wide contains scans walk the whole container; narrow with a longer query "
             "or match_mode=prefix/exact (role=library|runtime scopes kind=classes|"
             "resources to mounted containers)");
        push("page through hits with page_size + cursor instead of waiting for the full set");
        timeoutBudget();
    }
    else if (name == "class_query") {
        const auto kind = arguments.value("kind", std::string{});
        if (kind == "list") {
            push("kind=list materializes every class summary before paging; read it in "
                 "page_size + cursor slices and trim rows with fields projection");
            timeoutBudget();
        }
        else if (kind == "jasm") {
            push("full-class jasm renders every method; address single methods with "
                 "kind=method_jasm or kind=bytecode (owner+name+descriptor)");
            push("read raw bytes in chunks with kind=bytes + offset/limit");
            push("for full-text bulk export use the mjt-py SDK channel (import mjt) "
                 "instead of per-class tool calls");
            timeoutBudget();
        }
        else if (kind == "class") {
            push("full-class metadata decodes every method; address one method with "
                 "kind=method (owner+name+descriptor)");
            timeoutBudget();
        }
    }
    else if (name == "xref_query") {
        push("narrow the reference surface: owner/package_prefix filters, or target a "
             "single method/field with owner+name(+descriptor)");
        push("page with limit + cursor; keep resolved=false unless callees need expansion");
        timeoutBudget();
    }
    else if (name == "project_open") {
        push("the open keeps running in the background; the project_id carried by this "
             "error becomes addressable once it completes (poll project_info)");
        timeoutBudget();
    }
    return hints;
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
        ((mjt::workspace::kMaximumClassReplacementSize + 2) / 3) * 4;
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
    if (output.size() > mjt::workspace::kMaximumClassReplacementSize) {
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

namespace {

///分页有效页大小：page_size 优先，其次沿用 limit，缺省 kDefaultPageSize；
///两者互斥。schema 层已限 bounds，此处是分发层兜底。
bool effectivePageSize(const Json& arguments, ca::usize& out, std::string& error)
{
    const bool hasPageSize = arguments.contains("page_size");
    const bool hasLimit    = arguments.contains("limit");
    if (hasPageSize && hasLimit) {
        error = "page_size and limit are mutually exclusive";
        return false;
    }
    out = hasPageSize
              ? arguments.value("page_size", detail::kDefaultPageSize)
              : hasLimit ? arguments.value("limit", detail::kDefaultPageSize)
                         : detail::kDefaultPageSize;
    out = std::min(out, detail::kMaximumPageSize);
    return true;
}

///fields 投影（items 只保留指定
///键。键名合法性按调用方给定的该 kind 静态行键全集判定，不再依赖结果集样
///本——空结果集也有可校验面：合法键照常返回空 rows，非法键报 unknown key
///并列出该 kind 合法键全集（键序排序后展示）。
bool projectFields(const Json& arguments, const std::vector<std::string>& rowKeys,
                   Json& items, std::string& error)
{
    if (!arguments.contains("fields")) return true;
    const auto& fields = arguments.at("fields");
    std::vector<std::string> wanted;
    for (const auto& field : fields) wanted.push_back(field.get<std::string>());
    auto available = rowKeys;
    std::sort(available.begin(), available.end());
    for (const auto& name : wanted) {
        if (std::find(available.begin(), available.end(), name) != available.end()) continue;
        error = "fields: unknown key \"" + name + "\"; available: ";
        for (std::size_t index = 0; index < available.size(); ++index) {
            if (index > 0) error += ", ";
            error += available[index];
        }
        return false;
    }
    Json projected = Json::array();
    for (const auto& item : items) {
        Json row = Json::object();
        for (const auto& name : wanted) {
            if (item.contains(name)) row[name] = item.at(name);
        }
        projected.push_back(std::move(row));
    }
    items = std::move(projected);
    return true;
}

}   //namespace

/// 常量池裸 UTF8 行的分面细分：按内容形态区分描述符 / 类型引用 / 其他名。
/// （"Code"、"decode" 这类属性名/方法名不是类引用，混进 utf8_classref 会让
/// string_kind 过滤失去意义——评估反馈实证。）
std::string utf8StringKind(const std::string& value)
{
    if (value.empty()) return "utf8_name";
    if (value.front() == '(') return "utf8_descriptor";
    if (value.front() == '[') return "utf8_classref";
    if (value.front() == 'L' && value.back() == ';') return "utf8_classref";
    if (value.find('/') != std::string::npos) return "utf8_classref";
    return "utf8_name";
}

bool page(Json values, const Json& arguments, Json& out, std::string& error,
          const std::vector<std::string>& rowKeys)
{
    const auto total = values.size();
    ca::usize offset = arguments.value("offset", static_cast<ca::usize>(0));
    //非法/越界 cursor 一律退化到空尾页（offset=total），与原 try/catch 语义一致；
    //统一走 parseCursor 后不再有裸 stoull 抛异常的风险。
    parseCursor(arguments, total, offset);
    ca::usize limit = 0;
    if (!effectivePageSize(arguments, limit, error)) return false;
    offset = std::min(offset, total);
    const auto end = std::min(total, offset + limit);
    Json items = Json::array();
    for (ca::usize index = offset; index < end; ++index) items.push_back(std::move(values[index]));
    if (!projectFields(arguments, rowKeys, items, error)) return false;
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
    out = std::move(result);
    return true;
}

bool classSummaryPage(const std::vector<ClassSummary>& values, const Json& arguments,
                      Json& out, std::string& error)
{
    //class_summary 行键静态全集：fields 校验按此表判定，不依赖结果
    //集（键序即 classSummaryJson 的声明序，错误文案排序后展示）。
    static const std::vector<std::string> kClassSummaryRowKeys = {
        "resource_uri", "class", "super_name", "access", "version",
        "interface_count", "field_count", "method_count"};
    const auto total  = values.size();
    ca::usize  offset = arguments.value("offset", static_cast<ca::usize>(0));
    parseCursor(arguments, total, offset);
    ca::usize limit = 0;
    if (!effectivePageSize(arguments, limit, error)) return false;
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
    //投影校验按静态行键全集，无需为校验编码全量行（本页字节预算提前
    //截断时可用键不变）。
    if (!projectFields(arguments, kClassSummaryRowKeys, items, error)) return false;

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
    out = std::move(result);
    return true;
}

Json impactJson(const ImpactAnalysis& impact)
{
    Json before = Json::array();
    Json after = Json::array();
    Json diagnostics = Json::array();
    Json risks = Json::array();
    for (const auto& item : impact.beforeReferences)
        before.push_back({{"kind", item.kind.to_std_string()},
                          {"target", item.target.to_std_string()}});
    for (const auto& item : impact.afterReferences)
        after.push_back({{"kind", item.kind.to_std_string()},
                         {"target", item.target.to_std_string()}});
    for (const auto& item : impact.diagnostics) diagnostics.push_back(diagnosticJson(item));
    for (const auto& item : impact.risks) risks.push_back(item.to_std_string());
    Json diffs = Json::array();
    for (const auto& diff : impact.byteDiffs)
        diffs.push_back({{"uri", diff.uri.to_std_string()},
                         {"before_size", diff.beforeSize},
                         {"after_size", diff.afterSize}});
    return {{"status", statusJson(shared::toCaStatus(impact.status))},
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
    //payload 已自带非空 project_id 时不回填：project_open 超时面把预订 id 随
    //错误外透（此时 arguments 里没有 project_id 可读，回填会把它抹成空串）。
    const bool prefilledProjectId = payload.contains("project_id") &&
                                    payload.at("project_id").is_string() &&
                                    !payload.at("project_id").get<std::string>().empty();
    if (!prefilledProjectId) {
        payload["project_id"] = arguments.contains("project_id") &&
                                           arguments.at("project_id").is_string()
                                       ? arguments.at("project_id")
                                       : Json("");
    }
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
