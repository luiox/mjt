#include "workspace.hpp"
#include "summary_scan.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <type_traits>
#include <tuple>
#include <unordered_set>
#include <utility>

#include <jbytecode/jbytecode.hpp>
#include <jclassfile/io/class_file_reader.hpp>
#include <jclassfile/model/class_file.hpp>
#include <jclassfile/model/const_pool.hpp>
#include <jclassfile/model/constant/const_ref.hpp>
#include <jclassfile/model/constant/cp_class.hpp>
#include <jclassfile/model/constant/cp_double.hpp>
#include <jclassfile/model/constant/cp_dynamic.hpp>
#include <jclassfile/model/constant/cp_entry.hpp>
#include <jclassfile/model/constant/cp_float.hpp>
#include <jclassfile/model/constant/cp_int.hpp>
#include <jclassfile/model/constant/cp_invoke_dynamic.hpp>
#include <jclassfile/model/constant/cp_long.hpp>
#include <jclassfile/model/constant/cp_method_handle.hpp>
#include <jclassfile/model/constant/cp_method_type.hpp>
#include <jclassfile/model/constant/cp_module.hpp>
#include <jclassfile/model/constant/cp_name_type.hpp>
#include <jclassfile/model/constant/cp_package.hpp>
#include <jclassfile/model/constant/cp_string.hpp>
#include <jclassfile/model/constant/cp_utf8.hpp>
#include <jclassfile/model/constant/placeholders.hpp>
#include <jclassfile/model/attribute/code_attribute.hpp>
#include <jclassfile/model/instruction/instruction.hpp>
#include <jclassfile/model/instruction/cp_ref_instruction.hpp>
#include <jclassfile/model/instruction/int_operand_instruction.hpp>
#include <jclassfile/model/instruction/iinc_instruction.hpp>
#include <jclassfile/model/instruction/multi_a_new_array_instruction.hpp>
#include <jclassfile/model/instruction/wide_instruction.hpp>
#include <jclassfile/model/instruction/table_switch_instruction.hpp>
#include <jclassfile/model/instruction/lookup_switch_instruction.hpp>
#include <jclassfile/model/field.hpp>
#include <jclassfile/model/method.hpp>
#include <jclassfile/util/opcode_util.hpp>
#include "libca/core/tag_cast.hpp"

#include "mbase/jasm/resource_jasm_service.hpp"
#include "mbase/asm/common_super_class_provider.hpp"
#include "mbase/asm/classpath/call_graph.hpp"
#include "mbase/asm/classpath/resolved_call_graph.hpp"
#include "mbase/hierarchy/class_hierarchy.hpp"
#include "mbase/hierarchy/method_hierarchy.hpp"
#include "mbase/mapping/identifier_util.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/model/resource_helper.hpp"
#include "mbase/model/resource_model.hpp"
#include "mbase/reference/reference_index.hpp"
#include "mbase/renamer/rename_engine.hpp"
#include "mbase/i18n.hpp"
#include "mbase/pass/pass_name_registry.hpp"
#include "mbase/pass/pass_registry.hpp"
#include "mbase/spi/pass_manager.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base::workspace {
namespace {

using jbytecode::tree::AnnotationNode;

ca::Status error(ca::StatusCode code, const std::string& operation, const std::string& detail)
{
    return ca::ErrStatus(code, operation + ": " + detail);
}

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string str(ca::str::Utf8StringRef value)
{
    return value.to_std_string();
}

std::string resourceTypeName(ResourceType type)
{
    switch (type) {
    case ResourceType::Class: return "class";
    case ResourceType::Manifest: return "manifest";
    case ResourceType::Jar: return "jar";
    case ResourceType::Unknown: return "resource";
    case ResourceType::SIZE: break;
    }
    return "resource";
}

ca::StatusOr<std::vector<ca::u8>> bytesOf(const IResource& resource)
{
    if (const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource)) {
        return ca::Ok(classResource->get());
    }
    if (const auto* manifestResource = ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource)) {
        return ca::Ok(manifestResource->get().toBytes());
    }
    if (const auto* unknownResource = ca::core::tag_cast::dyn_cast<const UnknownResource>(&resource)) {
        return ca::Ok(unknownResource->get());
    }
    return ca::Err(error(ca::StatusCode::UNIMPLEMENTED,
                         "read resource",
                         "unsupported resource type at " + resource.getLocation()));
}

std::unique_ptr<IResource> cloneResource(const IResource& resource)
{
    if (const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource)) {
        return std::make_unique<ClassResource>(classResource->getLocation(), classResource->get());
    }
    if (const auto* manifestResource = ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource)) {
        return std::make_unique<ManifestResource>(manifestResource->get());
    }
    if (const auto* unknownResource = ca::core::tag_cast::dyn_cast<const UnknownResource>(&resource)) {
        return std::make_unique<UnknownResource>(unknownResource->getLocation(),
                                                 unknownResource->get());
    }
    return nullptr;
}

ca::StatusOr<std::unique_ptr<ResourceContainer>> cloneContainer(const ResourceContainer& source)
{
    auto        result = std::make_unique<ResourceContainer>();
    ca::Status  err    = ca::OkStatus();
    source.visitResources([&](const std::string& uri, const IResource& resource) {
        if (err.is_err()) return;   //visitResources 回调返回 void 无法早停，出错后仅排空剩余调用
        auto copied = cloneResource(resource);
        if (!copied) {
            ///原实现在此 throw runtime_error，绕过 StatusOr 错误传播（#WORKSPACE-1）。
            ///改为捕获错误标志，循环外统一转 ca::Err。
            err = error(ca::StatusCode::UNIMPLEMENTED, "clone container",
                        "unsupported resource type at " + uri);
            return;
        }
        result->addResource(uri, std::move(copied));
    });
    if (err.is_err()) return ca::Err(std::move(err));
    return ca::Ok(std::move(result));
}

void appendAnnotationElements(std::vector<AnnotationElement>& target,
                              const std::vector<AnnotationNode::Element>& source)
{
    for (const auto& element : source) {
        AnnotationElement output;
        output.name = str(element.name);
        output.tag  = element.tag;
        switch (element.tag) {
        case 'I': output.value = std::to_string(element.ival); break;
        case 'J': output.value = std::to_string(element.lval); break;
        case 'F': output.value = std::to_string(element.fval); break;
        case 'D': output.value = std::to_string(element.dval); break;
        default: output.value = element.sval.to_std_string(); break;
        }
        if (element.nested) {
            output.annotationDescriptor = str(element.nested->desc);
            appendAnnotationElements(output.children, element.nested->elements);
        }
        target.push_back(std::move(output));
    }
}

void appendAnnotations(std::vector<AnnotationInfo>& target,
                       const std::vector<AnnotationNode*>& source, bool visible)
{
    for (const auto* annotation : source) {
        if (!annotation) continue;
        AnnotationInfo output;
        output.descriptor = str(annotation->desc);
        output.visible    = visible;
        appendAnnotationElements(output.elements, annotation->elements);
        target.push_back(std::move(output));
    }
}

void appendAllAnnotations(std::vector<AnnotationInfo>& target,
                          const std::vector<AnnotationNode*>& visible,
                          const std::vector<AnnotationNode*>& invisible,
                          const std::vector<AnnotationNode*>& visibleType,
                          const std::vector<AnnotationNode*>& invisibleType)
{
    appendAnnotations(target, visible, true);
    appendAnnotations(target, invisible, false);
    appendAnnotations(target, visibleType, true);
    appendAnnotations(target, invisibleType, false);
}

///@brief 把 jbytecode FieldNode.value（ca::core::Any）序列化为 ConstantValueInfo。
///@details readConst（class_reader.cpp:1661）产生的 Any 类型：Integer→int、
///Long→i64、Float→float、Double→double、String→Utf8StringRef。text 用 %d/%lld/%g
///渲染、字符串保留原文，与 mjt info 的 showFieldsWithValues 中性渲染对齐（无后缀/
///无引号），调用方按需补后缀。
ConstantValueInfo constantValueFromAny(const ca::core::Any& value)
{
    ConstantValueInfo output;
    char              tmp[64];
    if (auto* v = value.as<int>()) {
        output.kind = ConstantValueInfo::Kind::Integer;
        std::snprintf(tmp, sizeof(tmp), "%d", *v);
        output.text = tmp;
    }
    else if (auto* v = value.as<ca::i64>()) {
        output.kind = ConstantValueInfo::Kind::Long;
        std::snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(*v));
        output.text = tmp;
    }
    else if (auto* v = value.as<long long>()) {
        output.kind = ConstantValueInfo::Kind::Long;
        std::snprintf(tmp, sizeof(tmp), "%lld", *v);
        output.text = tmp;
    }
    else if (auto* v = value.as<float>()) {
        output.kind = ConstantValueInfo::Kind::Float;
        std::snprintf(tmp, sizeof(tmp), "%g", *v);
        output.text = tmp;
    }
    else if (auto* v = value.as<double>()) {
        output.kind = ConstantValueInfo::Kind::Double;
        std::snprintf(tmp, sizeof(tmp), "%g", *v);
        output.text = tmp;
    }
    else if (auto* v = value.as<ca::str::Utf8StringRef>()) {
        output.kind = ConstantValueInfo::Kind::String;
        output.text = v->to_std_string();
    }
    else if (auto* v = value.as<std::string>()) {
        output.kind = ConstantValueInfo::Kind::String;
        output.text = *v;
    }
    return output;
}

FieldInfo makeFieldInfo(const jbytecode::tree::FieldNode& field)
{
    FieldInfo output;
    output.name       = str(field.name);
    output.descriptor = str(field.desc);
    output.signature  = str(field.signature);
    output.access     = field.access;
    if (field.value.has_value()) {
        output.constantValue = constantValueFromAny(field.value);
    }
    appendAllAnnotations(output.annotations,
                         field.visibleAnnotations,
                         field.invisibleAnnotations,
                         field.visibleTypeAnnotations,
                         field.invisibleTypeAnnotations);
    return output;
}

MethodInfo makeMethodInfo(const jbytecode::tree::MethodNode& method)
{
    MethodInfo output;
    output.name             = str(method.name);
    output.descriptor       = str(method.desc);
    output.signature        = str(method.signature);
    output.access           = method.access;
    output.maxStack         = method.maxStack;
    output.maxLocals        = method.maxLocals;
    output.instructionCount = method.instructions.size();
    output.tryCatchCount    = method.tryCatchBlocks.size();
    for (const auto& exception : method.exceptions) output.exceptions.push_back(str(exception));
    for (const auto* local : method.localVariables) {
        if (!local) continue;
        output.localVariables.push_back(
            {str(local->name), str(local->descriptor), str(local->signature), local->index});
    }
    for (ca::i32 index = 0; index < method.instructions.size(); ++index) {
        const auto* instruction = method.instructions.get(index);
        const auto* line = ca::core::tag_cast::dyn_cast<const jbytecode::tree::LineNumberNode>(instruction);
        if (line) output.lineNumbers.push_back({index, line->line});
    }
    appendAllAnnotations(output.annotations,
                         method.visibleAnnotations,
                         method.invisibleAnnotations,
                         method.visibleTypeAnnotations,
                         method.invisibleTypeAnnotations);
    appendAnnotations(output.annotations, method.visibleParameterAnnotations, true);
    appendAnnotations(output.annotations, method.invisibleParameterAnnotations, false);
    return output;
}

std::string instructionNodeType(const jbytecode::tree::AbstractInsnNode& instruction)
{
    using Node = jbytecode::tree::AbstractInsnNode;
    switch (instruction.getType()) {
    case Node::INSN: return "insn";
    case Node::INT_INSN: return "int";
    case Node::VAR_INSN: return "var";
    case Node::TYPE_INSN: return "type";
    case Node::FIELD_INSN: return "field";
    case Node::METHOD_INSN: return "method";
    case Node::INVOKE_DYNAMIC_INSN: return "invokedynamic";
    case Node::JUMP_INSN: return "jump";
    case Node::LABEL: return "label";
    case Node::LDC_INSN: return "ldc";
    case Node::IINC_INSN: return "iinc";
    case Node::TABLESWITCH_INSN: return "tableswitch";
    case Node::LOOKUPSWITCH_INSN: return "lookupswitch";
    case Node::MULTIANEWARRAY_INSN: return "multianewarray";
    case Node::FRAME: return "frame";
    case Node::LINE: return "line";
    default: return "unknown";
    }
}

std::string instructionMnemonic(const jbytecode::tree::AbstractInsnNode& instruction)
{
    const auto opcode = instruction.getOpcode();
    if (opcode < 0 || opcode >= 200 || !jbytecode::util::Printer::OPCODES[opcode]) return {};
    std::string result = jbytecode::util::Printer::OPCODES[opcode];
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool matchesText(const std::string& value, const std::string& query,
                 const SearchOptions& options, const std::regex* compiledRegex)
{
    if (options.matchMode == SearchMatchMode::Regex)
        return compiledRegex && std::regex_search(value, *compiledRegex);
    const auto comparableValue = options.caseSensitive ? value : lowercase(value);
    const auto comparableQuery = options.caseSensitive ? query : lowercase(query);
    if (options.matchMode == SearchMatchMode::Exact) return comparableValue == comparableQuery;
    return comparableValue.find(comparableQuery) != std::string::npos;
}

ca::Status validateSearchOptions(const std::string& query, const SearchOptions& options,
                                 const std::string& operation)
{
    if (query.empty())
        return error(ca::StatusCode::INVALID_ARGUMENT, operation, "query must not be empty");
    if (query.size() > 1024)
        return error(ca::StatusCode::INVALID_ARGUMENT, operation, "query exceeds 1024 bytes");
    if (!options.memberKind.empty() && options.memberKind != "field" && options.memberKind != "method")
        return error(ca::StatusCode::INVALID_ARGUMENT,
                     operation,
                     "member kind must be field or method");
    if (options.opcode < -1 || options.opcode >= 200)
        return error(ca::StatusCode::INVALID_ARGUMENT, operation, "opcode must be between 0 and 199");
    return ca::OkStatus();
}

ca::StatusOr<std::optional<std::regex>> compileSearchRegex(const std::string& query,
                                                            const SearchOptions& options,
                                                            const std::string& operation)
{
    auto status = validateSearchOptions(query, options, operation);
    if (status.is_err()) return ca::Err(status);
    if (options.matchMode != SearchMatchMode::Regex)
        return ca::Ok(std::optional<std::regex>{});
    try {
        auto flags = std::regex_constants::ECMAScript;
        if (!options.caseSensitive) flags |= std::regex_constants::icase;
        return ca::Ok(std::optional<std::regex>(std::in_place, query, flags));
    }
    catch (const std::regex_error& exception) {
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT, operation, exception.what()));
    }
}

bool matchesSearchScope(const std::string& owner, const SearchOptions& options)
{
    if (!options.owner.empty() && owner != options.owner) return false;
    return options.packagePrefix.empty() || owner.rfind(options.packagePrefix, 0) == 0;
}

bool matchesAnyText(const std::vector<std::string>& values, const std::string& query,
                    const SearchOptions& options, const std::regex* compiledRegex)
{
    return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
        return matchesText(value, query, options, compiledRegex);
    });
}

template<typename T>
std::string numberText(T value)
{
    std::ostringstream output;
    if constexpr (std::is_floating_point_v<T>) {
        output << std::setprecision(std::numeric_limits<T>::max_digits10);
    }
    output << value;
    return output.str();
}

bool matchesNumber(const std::string& value, const std::string& query)
{
    if (value == query) return true;
    try {
        std::size_t valueOffset = 0;
        std::size_t queryOffset = 0;
        const auto parsedValue = std::stold(value, &valueOffset);
        const auto parsedQuery = std::stold(query, &queryOffset);
        return valueOffset == value.size() && queryOffset == query.size() && parsedValue == parsedQuery;
    }
    catch (const std::exception&) {
        return false;
    }
}

ca::Status validateNumberQuery(const std::string& query)
{
    try {
        std::size_t offset = 0;
        const auto parsed = std::stold(query, &offset);
        (void)parsed;
        if (offset == query.size()) return ca::OkStatus();
    }
    catch (const std::exception&) {
    }
    return error(ca::StatusCode::INVALID_ARGUMENT,
                 "search numbers",
                 "query must be a complete numeric value");
}

void sortAndDeduplicateSearchHits(std::vector<SearchHit>& hits)
{
    std::sort(hits.begin(), hits.end(), [](const SearchHit& left, const SearchHit& right) {
        return std::tie(left.resourceUri,
                        left.owner,
                        left.name,
                        left.descriptor,
                        left.instructionIndex,
                        left.kind,
                        left.value) <
               std::tie(right.resourceUri,
                        right.owner,
                        right.name,
                        right.descriptor,
                        right.instructionIndex,
                        right.kind,
                        right.value);
    });
    hits.erase(std::unique(hits.begin(), hits.end(), [](const SearchHit& left, const SearchHit& right) {
                   return left.resourceUri == right.resourceUri && left.kind == right.kind &&
                          left.owner == right.owner && left.name == right.name &&
                          left.descriptor == right.descriptor &&
                          left.instructionIndex == right.instructionIndex && left.value == right.value;
               }),
               hits.end());
}

std::string instructionSearchText(const jbytecode::tree::AbstractInsnNode& instruction)
{
    std::ostringstream output;
    output << instructionMnemonic(instruction) << ' ' << instructionNodeType(instruction) << ' '
           << instruction.getOpcode();
    if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::IntInsnNode>(&instruction)) {
        output << ' ' << node->operand;
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::VarInsnNode>(&instruction)) {
        output << ' ' << node->var;
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::TypeInsnNode>(&instruction)) {
        output << ' ' << str(node->desc);
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::FieldInsnNode>(&instruction)) {
        output << ' ' << str(node->owner) << '.' << str(node->name) << str(node->desc);
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::MethodInsnNode>(&instruction)) {
        output << ' ' << str(node->owner) << '.' << str(node->name) << str(node->desc);
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::InvokeDynamicInsnNode>(&instruction)) {
        output << ' ' << str(node->name) << str(node->desc) << ' ' << node->bsm.toString();
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::IincInsnNode>(&instruction)) {
        output << ' ' << node->var << ' ' << node->incr;
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::MultiANewArrayInsnNode>(&instruction)) {
        output << ' ' << str(node->desc) << ' ' << node->dims;
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::LdcInsnNode>(&instruction)) {
        if (const auto* value = node->cst.as<std::string>()) output << ' ' << *value;
        else if (const auto* value = node->cst.as<ca::str::Utf8StringRef>()) output << ' ' << str(*value);
        else if (const auto* value = node->cst.as<ca::i32>()) output << ' ' << *value;
        else if (const auto* value = node->cst.as<ca::i64>()) output << ' ' << *value;
        else if (const auto* value = node->cst.as<float>()) output << ' ' << numberText(*value);
        else if (const auto* value = node->cst.as<double>()) output << ' ' << numberText(*value);
    }
    return output.str();
}

std::vector<std::string> instructionNumbers(const jbytecode::tree::AbstractInsnNode& instruction)
{
    using jbytecode::core::Opcodes;
    std::vector<std::string> result;
    const auto opcode = instruction.getOpcode();
    switch (opcode) {
    case Opcodes::ICONST_M1: result.push_back("-1"); break;
    case Opcodes::ICONST_0:
    case Opcodes::LCONST_0:
    case Opcodes::FCONST_0:
    case Opcodes::DCONST_0: result.push_back("0"); break;
    case Opcodes::ICONST_1:
    case Opcodes::LCONST_1:
    case Opcodes::FCONST_1:
    case Opcodes::DCONST_1: result.push_back("1"); break;
    case Opcodes::ICONST_2:
    case Opcodes::FCONST_2: result.push_back("2"); break;
    case Opcodes::ICONST_3: result.push_back("3"); break;
    case Opcodes::ICONST_4: result.push_back("4"); break;
    case Opcodes::ICONST_5: result.push_back("5"); break;
    default: break;
    }
    if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::IntInsnNode>(&instruction)) {
        result.push_back(numberText(node->operand));
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::IincInsnNode>(&instruction)) {
        result.push_back(numberText(node->incr));
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::TableSwitchInsnNode>(&instruction)) {
        result.push_back(numberText(node->min));
        result.push_back(numberText(node->max));
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::LookupSwitchInsnNode>(&instruction)) {
        for (const auto key : node->keys) result.push_back(numberText(key));
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::MultiANewArrayInsnNode>(&instruction)) {
        result.push_back(numberText(node->dims));
    }
    else if (const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::LdcInsnNode>(&instruction)) {
        if (const auto* value = node->cst.as<ca::i32>()) result.push_back(numberText(*value));
        else if (const auto* value = node->cst.as<ca::i64>()) result.push_back(numberText(*value));
        else if (const auto* value = node->cst.as<float>()) result.push_back(numberText(*value));
        else if (const auto* value = node->cst.as<double>()) result.push_back(numberText(*value));
    }
    return result;
}

ca::StatusOr<ClassInfo> decodeClass(const std::string& uri, const ClassResource& resource)
{
    try {
        ca::str::Utf8StringArena     arena;
        jbytecode::tree::ClassNode   node;
        jbytecode::core::ClassReader reader(arena, resource.get());
        reader.accept(&node, 0);
        //跨结构 ref 提升门禁（治理方案 §2.4-2，DEV-1 接线点一）：debug 下抽样断言
        //ClassNode 的字符串视图确属本 arena；release 为空实现零开销。
        jbytecode::tree::debugAssertOwnedBy(node, arena);

        ClassInfo output;
        output.resourceUri  = uri;
        output.internalName = str(node.name);
        output.superName    = str(node.superName);
        output.signature    = str(node.signature);
        output.access       = node.access;
        output.version      = node.version;
        for (const auto& interfaceName : node.interfaces) output.interfaces.push_back(str(interfaceName));
        for (const auto* field : node.fields) {
            if (field) output.fields.push_back(makeFieldInfo(*field));
        }
        for (const auto* method : node.methods) {
            if (method) output.methods.push_back(makeMethodInfo(*method));
        }
        for (const auto* inner : node.innerClasses) {
            if (inner && !inner->name.is_empty()) output.innerClasses.push_back(str(inner->name));
        }
        std::sort(output.innerClasses.begin(), output.innerClasses.end());
        output.innerClasses.erase(std::unique(output.innerClasses.begin(), output.innerClasses.end()),
                                  output.innerClasses.end());
        appendAllAnnotations(output.annotations,
                             node.visibleAnnotations,
                             node.invisibleAnnotations,
                             node.visibleTypeAnnotations,
                             node.invisibleTypeAnnotations);
        return ca::Ok(std::move(output));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::DATA_LOSS, "decode class " + uri, exception.what()));
    }
}

class ClassSummaryVisitor final : public jbytecode::core::ClassVisitor
{
public:
    explicit ClassSummaryVisitor(ClassSummary& output)
        : jbytecode::core::ClassVisitor(jbytecode::core::Opcodes::ASM9)
        , output_(output)
    {}

    void visit(int version, int access, ca::str::Utf8StringRef name, ca::str::Utf8StringRef,
               ca::str::Utf8StringRef                     superName,
               const std::vector<ca::str::Utf8StringRef>& interfaces) override
    {
        output_.internalName   = str(name);
        output_.superName      = str(superName);
        output_.access         = access;
        output_.version        = version;
        output_.interfaceCount = interfaces.size();
    }

    jbytecode::core::FieldVisitor* visitField(int, ca::str::Utf8StringRef, ca::str::Utf8StringRef,
                                              ca::str::Utf8StringRef, const ca::core::Any&) override
    {
        ++output_.fieldCount;
        return nullptr;
    }

    jbytecode::core::MethodVisitor* visitMethod(int, ca::str::Utf8StringRef, ca::str::Utf8StringRef,
                                                ca::str::Utf8StringRef,
                                                const std::vector<ca::str::Utf8StringRef>&) override
    {
        ++output_.methodCount;
        return nullptr;
    }

private:
    ClassSummary& output_;
};

std::vector<ByteDiff> diffContainers(const ResourceContainer& before,
                                     const ResourceContainer& after)
{
    std::set<std::string> uris;
    before.visitResources([&](const std::string& uri, const IResource&) { uris.insert(uri); });
    after.visitResources([&](const std::string& uri, const IResource&) { uris.insert(uri); });

    std::vector<ByteDiff> diffs;
    for (const auto& uri : uris) {
        const auto*         beforeResource = before.getResource(uri);
        const auto*         afterResource  = after.getResource(uri);
        std::vector<ca::u8> beforeBytes;
        std::vector<ca::u8> afterBytes;
        if (beforeResource) {
            auto bytes = bytesOf(*beforeResource);
            if (bytes.is_err()) continue;
            beforeBytes = std::move(bytes).unwrap();
        }
        if (afterResource) {
            auto bytes = bytesOf(*afterResource);
            if (bytes.is_err()) continue;
            afterBytes = std::move(bytes).unwrap();
        }
        if (beforeBytes == afterBytes) continue;

        ca::usize start = 0;
        while (start < beforeBytes.size() && start < afterBytes.size() &&
               beforeBytes[start] == afterBytes[start]) {
            ++start;
        }
        ca::usize beforeEnd = beforeBytes.size();
        ca::usize afterEnd  = afterBytes.size();
        while (beforeEnd > start && afterEnd > start &&
               beforeBytes[beforeEnd - 1] == afterBytes[afterEnd - 1]) {
            --beforeEnd;
            --afterEnd;
        }
        ByteDiff diff;
        diff.uri        = uri;
        diff.beforeSize = beforeBytes.size();
        diff.afterSize  = afterBytes.size();
        diff.ranges.push_back({start, beforeEnd - start, afterEnd - start});
        diffs.push_back(std::move(diff));
    }
    return diffs;
}

bool hasClassExtension(const std::string& uri)
{
    constexpr char kExtension[] = ".class";
    if (uri.size() < sizeof(kExtension) - 1) return false;
    const auto offset = uri.size() - (sizeof(kExtension) - 1);
    for (ca::usize index = 0; index < sizeof(kExtension) - 1; ++index) {
        const auto character = static_cast<unsigned char>(uri[offset + index]);
        if (std::tolower(character) != kExtension[index]) return false;
    }
    return true;
}

std::string internalNameFromClassUri(const std::string& uri)
{
    constexpr char kMultiReleasePrefix[] = "META-INF/versions/";
    std::string path = uri;
    if (path.rfind(kMultiReleasePrefix, 0) == 0) {
        const auto versionEnd = path.find('/', sizeof(kMultiReleasePrefix) - 1);
        if (versionEnd == std::string::npos || versionEnd + 1 >= path.size()) return {};
        const auto version = path.substr(sizeof(kMultiReleasePrefix) - 1,
                                         versionEnd - (sizeof(kMultiReleasePrefix) - 1));
        if (version.empty() ||
            !std::all_of(version.begin(), version.end(), [](unsigned char character) {
                return std::isdigit(character) != 0;
            }))
            return {};
        path = path.substr(versionEnd + 1);
    }
    if (!hasClassExtension(path)) return {};
    path.resize(path.size() - std::string(".class").size());
    return path;
}

std::vector<JarRepairIssue> scanJarRepairIssues(const ResourceContainer& container,
                                                 const JarRepairRequest& request,
                                                 ca::usize& classCount)
{
    jasm::JvmJasmService       validator;
    std::vector<JarRepairIssue> issues;
    container.visitResources([&](const std::string& uri, const IResource& resource) {
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (classResource) {
            ++classCount;
            const auto validated = validator.validate(classResource->get());
            if (!validated.isOk()) {
                issues.push_back({uri,
                                  "invalid_class",
                                  "class bytes did not pass Jasm validation",
                                  classResource->get().size(),
                                  request.removeInvalidClassEntries});
            }
        }
        else if (hasClassExtension(uri)) {
            auto bytes = bytesOf(resource);
            issues.push_back({uri,
                              "invalid_class_entry",
                              "a .class entry was not recognized as valid class bytes",
                              bytes.is_ok() ? bytes.unwrap().size() : 0,
                              request.removeInvalidClassEntries});
        }
        if (uri.find("corrupt_output_stream") != std::string::npos) {
            auto bytes = bytesOf(resource);
            issues.push_back({uri,
                              "corrupt_output_marker",
                              "resource name matches a known corrupt output marker",
                              bytes.is_ok() ? bytes.unwrap().size() : 0,
                              request.removeCorruptOutputMarkers});
        }
    });
    std::sort(issues.begin(), issues.end(), [](const JarRepairIssue& left, const JarRepairIssue& right) {
        if (left.uri != right.uri) return left.uri < right.uri;
        return left.kind < right.kind;
    });
    return issues;
}

void removeSelectedRepairIssues(ResourceContainer& candidate,
                                const std::vector<JarRepairIssue>& issues)
{
    auto& resources = candidate.getResources();
    for (const auto& issue : issues) {
        if (issue.selectedForRemoval) resources.erase(issue.uri);
    }
}

bool hasSelectedRepairIssues(const std::vector<JarRepairIssue>& issues)
{
    return std::any_of(issues.begin(), issues.end(), [](const JarRepairIssue& issue) {
        return issue.selectedForRemoval;
    });
}

std::vector<std::string> changedClassNames(const ResourceContainer& container,
                                           const std::vector<ByteDiff>& diffs)
{
    std::vector<std::string> result;
    for (const auto& diff : diffs) {
        const auto* resource = ca::core::tag_cast::dyn_cast<const ClassResource>(container.getResource(diff.uri));
        if (!resource) continue;
        try {
            ca::str::Utf8StringArena     arena;
            jbytecode::core::ClassReader reader(arena, resource->get());
            result.push_back(str(reader.getClassName()));
        }
        catch (...) {
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void appendUniqueImpact(std::vector<ReferenceImpact>& impacts, std::unordered_set<std::string>& seen,
                        const std::string& kind, const std::string& target)
{
    const std::string key = kind + "\n" + target;
    if (!seen.insert(key).second) return;
    impacts.push_back({kind, target});
}

std::vector<ReferenceImpact> collectImpacts(ResourceContainer& container,
                                            const std::vector<std::string>& classes)
{
    try {
        ca::str::Utf8StringArena arena;
        ReferenceIndex            index;
        index.build(container, arena);
        std::vector<ReferenceImpact> impacts;
        std::unordered_set<std::string> seen;
        for (const auto& owner : classes) {
            for (const auto& reference : index.methodReferencesFromClass(owner)) {
                std::string kind = "method";
                if (reference.kind == MethodReferenceKind::Reflection)
                    kind = "reflection_method_lookup";
                else if (reference.kind == MethodReferenceKind::ReflectionExecution)
                    kind = "reflection_method_execution";
                else if (reference.kind == MethodReferenceKind::MethodHandle)
                    kind = "method_handle";
                else if (reference.kind == MethodReferenceKind::Lambda)
                    kind = "lambda";
                appendUniqueImpact(impacts,
                                   seen,
                                   kind,
                                   reference.target.owner + "." + reference.target.name +
                                       reference.target.desc);
            }
            for (const auto& reference : index.fieldReferencesFromClass(owner)) {
                std::string kind = "field";
                if (reference.kind == FieldReferenceKind::Reflection)
                    kind = "reflection_field_lookup";
                else if (reference.kind == FieldReferenceKind::ReflectionAccess)
                    kind = "reflection_field_access";
                else if (reference.kind == FieldReferenceKind::MethodHandle)
                    kind = "method_handle";
                appendUniqueImpact(impacts,
                                   seen,
                                   kind,
                                   reference.target.owner + "." + reference.target.name +
                                       reference.target.desc);
            }
            for (const auto& reference : index.typeReferencesFromClass(owner)) {
                appendUniqueImpact(impacts,
                                   seen,
                                   reference.kind == TypeReferenceKind::ReflectionString
                                       ? "reflection_class_string"
                                       : "type",
                                   reference.targetType);
            }
        }
        std::sort(impacts.begin(), impacts.end(), [](const ReferenceImpact& left,
                                                      const ReferenceImpact& right) {
            if (left.kind != right.kind) return left.kind < right.kind;
            return left.target < right.target;
        });
        return impacts;
    }
    catch (...) {
        return {};
    }
}

std::string methodReferenceKind(MethodReferenceKind kind)
{
    switch (kind) {
    case MethodReferenceKind::Direct: return "direct";
    case MethodReferenceKind::MethodHandle: return "method_handle";
    case MethodReferenceKind::Lambda: return "lambda";
    case MethodReferenceKind::Reflection: return "reflection";
    case MethodReferenceKind::ReflectionExecution: return "reflection_execution";
    }
    return "unknown";
}

std::string fieldReferenceKind(FieldReferenceKind kind)
{
    switch (kind) {
    case FieldReferenceKind::Direct: return "direct";
    case FieldReferenceKind::MethodHandle: return "method_handle";
    case FieldReferenceKind::Reflection: return "reflection";
    case FieldReferenceKind::ReflectionAccess: return "reflection_access";
    }
    return "unknown";
}

std::string methodTarget(const MethodRefKey& target)
{
    return target.owner + "." + target.name + target.desc;
}

std::string fieldTarget(const FieldRefKey& target)
{
    return target.owner + "." + target.name + target.desc;
}

ReferenceInfo referenceInfo(const ReferenceSite& site, const std::string& target,
                            const std::string& kind)
{
    return {site.sourceClass, site.sourceMethod, site.sourceDesc, site.insnIndex, target, kind};
}

void sortReferences(std::vector<ReferenceInfo>& references)
{
    std::sort(references.begin(), references.end(), [](const ReferenceInfo& left,
                                                       const ReferenceInfo& right) {
        if (left.sourceClass != right.sourceClass) return left.sourceClass < right.sourceClass;
        if (left.sourceMethod != right.sourceMethod) return left.sourceMethod < right.sourceMethod;
        if (left.sourceDescriptor != right.sourceDescriptor)
            return left.sourceDescriptor < right.sourceDescriptor;
        if (left.instructionIndex != right.instructionIndex)
            return left.instructionIndex < right.instructionIndex;
        if (left.kind != right.kind) return left.kind < right.kind;
        return left.target < right.target;
    });
}

void hashFingerprintBytes(ca::u64& state, const ca::u8* data, ca::usize size)
{
    constexpr ca::u64 kPrime = 1099511628211ULL;
    for (ca::usize index = 0; index < size; ++index) {
        state ^= data[index];
        state *= kPrime;
    }
}

void hashFingerprintLength(ca::u64& state, ca::usize value)
{
    const auto stableValue = static_cast<ca::u64>(value);
    for (ca::usize index = 0; index < sizeof(stableValue); ++index) {
        const auto byte = static_cast<ca::u8>((stableValue >> (index * 8U)) & 0xffU);
        hashFingerprintBytes(state, &byte, 1);
    }
}

ca::StatusOr<std::string> fingerprintContainer(const ResourceContainer& container)
{
    constexpr ca::u64 kOffsetBasis = 14695981039346656037ULL;
    ca::u64           state = kOffsetBasis;
    std::vector<std::string> uris;
    container.visitResources([&](const std::string& uri, const IResource&) { uris.push_back(uri); });
    std::sort(uris.begin(), uris.end());
    for (const auto& uri : uris) {
        const auto* resource = container.getResource(uri);
        if (!resource) continue;
        auto bytes = bytesOf(*resource);
        if (bytes.is_err()) return ca::Err(bytes.unwrap_err());
        const auto& content = bytes.unwrap();
        hashFingerprintLength(state, uri.size());
        hashFingerprintBytes(state,
                             reinterpret_cast<const ca::u8*>(uri.data()),
                             uri.size());
        hashFingerprintLength(state, content.size());
        hashFingerprintBytes(state, content.data(), content.size());
    }
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << state;
    return ca::Ok(output.str());
}

std::string fingerprintBytes(const std::vector<ca::u8>& bytes)
{
    constexpr ca::u64 kOffsetBasis = 14695981039346656037ULL;
    ca::u64           state = kOffsetBasis;
    hashFingerprintLength(state, bytes.size());
    hashFingerprintBytes(state, bytes.data(), bytes.size());
    std::ostringstream output;
    output << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << state;
    return output.str();
}

ca::StatusOr<std::string> normalizedRegularFile(const std::string& path,
                                                 const std::string& operation)
{
    namespace fs = std::filesystem;
    if (path.empty())
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT, operation, "path is empty"));
    std::error_code filesystemError;
    const auto absolute = fs::absolute(path, filesystemError).lexically_normal();
    if (filesystemError)
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT, operation, filesystemError.message()));
    const auto status = fs::status(absolute, filesystemError);
    if (filesystemError || !fs::exists(status))
        return ca::Err(error(ca::StatusCode::NOT_FOUND, operation, absolute.string()));
    if (!fs::is_regular_file(status))
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             operation,
                             "path is not a regular file"));
    return ca::Ok(absolute.string());
}

ca::StatusOr<std::filesystem::path> validateExportDestination(
    const std::string& sourcePath, const std::string& outputPath,
    const std::string& operation = "export JAR",
    const std::string& sourceDescription = "input JAR")
{
    namespace fs = std::filesystem;
    if (outputPath.empty())
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT, operation, "output path is empty"));

    std::error_code filesystemError;
    const auto output = fs::absolute(fs::u8path(outputPath), filesystemError);
    if (filesystemError)
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT, operation, outputPath));
    const auto source = fs::absolute(fs::u8path(sourcePath), filesystemError);
    if (!filesystemError && source == output) {
        return ca::Err(error(ca::StatusCode::PERMISSION_DENIED,
                             operation,
                             "output must not overwrite the " + sourceDescription));
    }
    const auto parent = output.parent_path();
    if (!parent.empty() && !fs::exists(parent)) {
        return ca::Err(error(ca::StatusCode::NOT_FOUND,
                             operation,
                             "output parent does not exist"));
    }
    if (fs::exists(output))
        return ca::Err(error(ca::StatusCode::ALREADY_EXISTS, operation, output.u8string()));
    return ca::Ok(output);
}

ca::Status exportContainerToJar(ResourceContainer& container, const std::string& sourcePath,
                                const std::string& outputPath, ca::u64 revision)
{
    namespace fs = std::filesystem;
    auto output = validateExportDestination(sourcePath, outputPath);
    if (output.is_err()) return output.unwrap_err();

    std::error_code filesystemError;
    auto temporary = output.unwrap();
    temporary += ".mjt-tmp-" + std::to_string(revision);
    if (fs::exists(temporary))
        return error(ca::StatusCode::ALREADY_EXISTS, "export JAR", temporary.u8string());
    try {
        ResourceHelper::exportToJar(container, temporary.u8string());
        if (!fs::exists(temporary) || fs::file_size(temporary) == 0) {
            fs::remove(temporary, filesystemError);
            return error(ca::StatusCode::INTERNAL, "export JAR", "temporary archive was not written");
        }
        fs::rename(temporary, output.unwrap(), filesystemError);
        if (filesystemError) {
            fs::remove(temporary, filesystemError);
            return error(ca::StatusCode::INTERNAL, "export JAR", "atomic rename failed");
        }
        return ca::OkStatus();
    }
    catch (const std::exception& exception) {
        fs::remove(temporary, filesystemError);
        return error(ca::StatusCode::INTERNAL, "export JAR", exception.what());
    }
}

ca::Status exportContainerToClass(ResourceContainer& container, const std::string& classUri,
                                  const std::string& sourcePath, const std::string& outputPath,
                                  ca::u64 revision)
{
    namespace fs = std::filesystem;
    auto output = validateExportDestination(sourcePath,
                                            outputPath,
                                            "export class",
                                            "input class");
    if (output.is_err()) return output.unwrap_err();

    std::error_code filesystemError;
    auto temporary = output.unwrap();
    temporary += ".mbase-class-tmp-" + std::to_string(revision);
    if (fs::exists(temporary))
        return error(ca::StatusCode::ALREADY_EXISTS, "export class", temporary.u8string());
    try {
        ResourceHelper::exportToClassFile(container, temporary.u8string(), classUri);
        if (!fs::exists(temporary) || fs::file_size(temporary) == 0) {
            fs::remove(temporary, filesystemError);
            return error(ca::StatusCode::INTERNAL,
                         "export class",
                         "temporary classfile was not written");
        }
        fs::rename(temporary, output.unwrap(), filesystemError);
        if (filesystemError) {
            fs::remove(temporary, filesystemError);
            return error(ca::StatusCode::INTERNAL, "export class", "atomic rename failed");
        }
        return ca::OkStatus();
    }
    catch (const std::exception& exception) {
        fs::remove(temporary, filesystemError);
        return error(ca::StatusCode::INTERNAL, "export class", exception.what());
    }
}

void removeWorkflowExport(const WorkflowResult& result)
{
    if (!result.exported || result.exportPath.empty()) return;
    std::error_code filesystemError;
    std::filesystem::remove(std::filesystem::u8path(result.exportPath), filesystemError);
}

ca::StatusOr<std::filesystem::path> createWorkflowCandidateJarPath()
{
    namespace fs = std::filesystem;
    static std::atomic<ca::u64> sequence{0};
    std::error_code             filesystemError;
    const auto root = fs::temp_directory_path(filesystemError);
    if (filesystemError) {
        return ca::Err(error(ca::StatusCode::INTERNAL,
                             "workflow candidate artifact",
                             filesystemError.message()));
    }
    for (ca::usize attempt = 0; attempt < 16; ++attempt) {
        const auto id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                        "-" + std::to_string(sequence.fetch_add(1));
        const auto path = root / ("mjt-workflow-candidate-" + id + ".jar");
        if (!fs::exists(path)) return ca::Ok(path);
    }
    return ca::Err(error(ca::StatusCode::ALREADY_EXISTS,
                         "workflow candidate artifact",
                         "cannot allocate a unique temporary JAR"));
}

class ScopedWorkflowCandidateJar final
{
public:
    explicit ScopedWorkflowCandidateJar(std::filesystem::path path)
        : path_(std::move(path))
    {}

    ~ScopedWorkflowCandidateJar()
    {
        std::error_code filesystemError;
        std::filesystem::remove(path_, filesystemError);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

ca::Status validateWorkflowReferenceQuery(const WorkflowReferenceQuery& query)
{
    if (query.owner.empty()) {
        return error(ca::StatusCode::INVALID_ARGUMENT,
                     "workflow xref",
                     "owner is required");
    }
    if (query.kind == WorkflowReferenceKind::Class) return ca::OkStatus();
    if (query.name.empty() || query.descriptor.empty()) {
        return error(ca::StatusCode::INVALID_ARGUMENT,
                     "workflow xref",
                     "member name and descriptor are required");
    }
    return ca::OkStatus();
}

ca::StatusOr<std::vector<ReferenceInfo>> workflowReferences(
    ResourceContainer& container, const WorkflowReferenceQuery& query)
{
    const auto queryStatus = validateWorkflowReferenceQuery(query);
    if (queryStatus.is_err()) return ca::Err(queryStatus);
    try {
        ca::str::Utf8StringArena arena;
        ReferenceIndex            index;
        index.build(container, arena);
        std::vector<ReferenceInfo> result;
        switch (query.kind) {
        case WorkflowReferenceKind::Class:
            for (const auto& reference : index.typeReferencesTo(query.owner)) {
                result.push_back(referenceInfo(reference.site, reference.targetType, "type"));
            }
            break;
        case WorkflowReferenceKind::Field:
            for (const auto& reference : index.fieldReferencesTo(query.owner,
                                                                 query.name,
                                                                 query.descriptor)) {
                result.push_back(referenceInfo(reference.site,
                                               fieldTarget(reference.target),
                                               fieldReferenceKind(reference.kind)));
            }
            break;
        case WorkflowReferenceKind::Method:
            for (const auto& reference : index.methodReferencesTo(query.owner,
                                                                  query.name,
                                                                  query.descriptor)) {
                result.push_back(referenceInfo(reference.site,
                                               methodTarget(reference.target),
                                               methodReferenceKind(reference.kind)));
            }
            break;
        }
        sortReferences(result);
        return ca::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::INTERNAL, "workflow xref", exception.what()));
    }
}

void sortUnique(std::vector<std::string>& values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

template<typename T>
jasm::Result<T> jasmFailure(const std::string& message)
{
    jasm::Result<T> result;
    result.diagnostics.push_back(
        {jasm::DiagnosticSeverity::Error, jasm::DiagnosticStage::Resource, utf16(message), std::nullopt});
    return result;
}

namespace jcm = jclassfile::model;

///@brief CpTag 数值 → 与 JVM 规范对齐的常量名（移植自 cmd_info cpTagName）。
std::string cpTagName(jcm::CpTag tag)
{
    switch (tag) {
    case jcm::CpTag::INTERNAL: return "Internal";
    case jcm::CpTag::UTF8: return "Utf8";
    case jcm::CpTag::INTEGER: return "Integer";
    case jcm::CpTag::FLOAT: return "Float";
    case jcm::CpTag::LONG: return "Long";
    case jcm::CpTag::DOUBLE: return "Double";
    case jcm::CpTag::CLASS: return "Class";
    case jcm::CpTag::STRING: return "String";
    case jcm::CpTag::FIELD_REF: return "FieldRef";
    case jcm::CpTag::METHOD_REF: return "MethodRef";
    case jcm::CpTag::INTERFACE_METHOD_REF: return "InterfaceMethodRef";
    case jcm::CpTag::NAME_TYPE: return "NameAndType";
    case jcm::CpTag::METHOD_HANDLE: return "MethodHandle";
    case jcm::CpTag::METHOD_TYPE: return "MethodType";
    case jcm::CpTag::DYNAMIC: return "Dynamic";
    case jcm::CpTag::INVOKE_DYNAMIC: return "InvokeDynamic";
    case jcm::CpTag::MODULE: return "Module";
    case jcm::CpTag::PACKAGE: return "Package";
    default: return "?";
    }
}

///@brief 解析 FieldRef/MethodRef/InterfaceMethodRef → owner.name:desc 文本。
std::string describeConstRef(const jcm::ConstRef* ref)
{
    std::string resolved;
    if (ref->classRef() && ref->classRef()->name()) resolved = ref->classRef()->name()->text();
    resolved += ".";
    if (ref->nameType() && ref->nameType()->name()) resolved += ref->nameType()->name()->text();
    resolved += ":";
    if (ref->nameType() && ref->nameType()->type()) resolved += ref->nameType()->type()->text();
    return resolved;
}

///@brief 指令操作数文本（移植自 mjt class_render operandText）。
///返回紧跟助记符后的操作数，如 "#5"/"42"/"3, 1"/"#5 dims=2"。无操作数返回空串。
std::string instructionOperandText(const jcm::Instruction* insn, const jcm::ConstPool* pool)
{
    if (!insn) return "";
    char tmp[64];
    switch (insn->kind()) {
    case jcm::InsnKind::CpRef:
    {
        const auto* entry = static_cast<const jcm::CpRefInstruction*>(insn)->getEntry();
        if (entry && pool) {
            std::snprintf(tmp, sizeof(tmp), "#%d", entry->index());
            return tmp;
        }
        return "#?";
    }
    case jcm::InsnKind::IntOperand:
        std::snprintf(tmp, sizeof(tmp), "%d", static_cast<const jcm::IntOperandInstruction*>(insn)->getOperand());
        return tmp;
    case jcm::InsnKind::Iinc:
        std::snprintf(tmp,
                      sizeof(tmp),
                      "%d, %d",
                      static_cast<const jcm::IincInstruction*>(insn)->getVar(),
                      static_cast<const jcm::IincInstruction*>(insn)->getIncrement());
        return tmp;
    case jcm::InsnKind::MultiANewArray:
    {
        const auto* mana = static_cast<const jcm::MultiANewArrayInstruction*>(insn);
        std::snprintf(tmp,
                      sizeof(tmp),
                      "#%d dims=%d",
                      mana->getDescriptor() ? mana->getDescriptor()->index() : 0,
                      mana->getDimensions());
        return tmp;
    }
    case jcm::InsnKind::Wide:
    {
        const auto* backing = static_cast<const jcm::WideInstruction*>(insn)->getBacking();
        return backing ? instructionOperandText(backing, pool) : "";
    }
    default: return "";
    }
}

///@brief 指令操作数可读注释（移植自 mjt class_render operandComment）。
///仅对 CpRef 类指令生成（owner.name:desc / "str" / class 名 / 字面值），各标识符段
///过 IdentifierUtil 可读化。数值格式严格照搬：Long 带 L，Double/Float 无后缀。
std::string instructionOperandComment(const jcm::Instruction* insn, const jcm::ConstPool* pool)
{
    if (!insn || insn->kind() != jcm::InsnKind::CpRef) return "";
    const auto* entry = static_cast<const jcm::CpRefInstruction*>(insn)->getEntry();
    if (!entry || !pool) return "";
    char tmp[64];
    switch (entry->tag()) {
    case jcm::CpTag::FIELD_REF:
    case jcm::CpTag::METHOD_REF:
    case jcm::CpTag::INTERFACE_METHOD_REF:
    {
        const auto*  ref = static_cast<const jcm::ConstRef*>(entry);
        std::string  s;
        if (ref->classRef() && ref->classRef()->name())
            s = mjt::base::IdentifierUtil::printReadable(ref->classRef()->name()->text());
        s += ".";
        if (ref->nameType() && ref->nameType()->name())
            s += mjt::base::IdentifierUtil::printReadable(ref->nameType()->name()->text());
        s += ":";
        if (ref->nameType() && ref->nameType()->type())
            s += mjt::base::IdentifierUtil::printReadable(ref->nameType()->type()->text());
        return s;
    }
    case jcm::CpTag::STRING:
    {
        const auto* text = static_cast<const jcm::CpString*>(entry)->value();
        if (text) {
            return "\"" + mjt::base::IdentifierUtil::printReadable(text->text()) + "\"";
        }
        return "";
    }
    case jcm::CpTag::CLASS:
    {
        const auto* name = static_cast<const jcm::CpClass*>(entry)->name();
        return name ? mjt::base::IdentifierUtil::printReadable(name->text()) : "";
    }
    case jcm::CpTag::INTEGER:
        std::snprintf(tmp, sizeof(tmp), "%d", static_cast<const jcm::CpInt*>(entry)->value());
        return tmp;
    case jcm::CpTag::FLOAT:
        std::snprintf(tmp, sizeof(tmp), "%g", static_cast<const jcm::CpFloat*>(entry)->value());
        return tmp;
    case jcm::CpTag::LONG:
        std::snprintf(tmp,
                      sizeof(tmp),
                      "%lldL",
                      static_cast<long long>(static_cast<const jcm::CpLong*>(entry)->value()));
        return tmp;
    case jcm::CpTag::DOUBLE:
        std::snprintf(tmp, sizeof(tmp), "%g", static_cast<const jcm::CpDouble*>(entry)->value());
        return tmp;
    default: return "";
    }
}

///@brief switch 指令的目标 PC（移植自 mjt class_render showSwitchDispatch 的 PC 计算）。
///非switch返回 false。defaultPc 与 cases 已基于指令 PC 算出绝对目标。
bool instructionSwitchTargets(const jcm::Instruction* insn,
                              ca::i32                 pc,
                              ca::i32&                defaultPc,
                              std::vector<SwitchCaseInfo>& cases)
{
    if (insn->kind() == jcm::InsnKind::TableSwitch) {
        const auto* ts = static_cast<const jcm::TableSwitchInstruction*>(insn);
        defaultPc      = pc + ts->getDefault();
        const auto& offsets = ts->getOffsets();
        cases.reserve(offsets.size());
        for (std::size_t k = 0; k < offsets.size(); ++k) {
            cases.push_back({ts->getLow() + static_cast<ca::i32>(k), pc + offsets[k]});
        }
        return true;
    }
    if (insn->kind() == jcm::InsnKind::LookupSwitch) {
        const auto* ls = static_cast<const jcm::LookupSwitchInstruction*>(insn);
        defaultPc      = pc + ls->getDefault();
        const auto& keys    = ls->getKeys();
        const auto& offsets = ls->getOffsets();
        cases.reserve(offsets.size());
        for (std::size_t k = 0; k < offsets.size(); ++k) {
            cases.push_back({keys[k], pc + offsets[k]});
        }
        return true;
    }
    return false;
}

///@brief 单个常量池条目 → ConstantPoolEntryInfo 的渲染。
///@details 移植自 mjt cmd_info.cpp 的 showConstantsRaw：非引用类条目总是填 text；
///引用类条目仅 resolve=true 时填解析后的 owner.name:desc。损坏/占位条目标 malformed。
ConstantPoolEntryInfo describeCpEntry(const jcm::CpEntry* entry, ca::usize index, bool resolve)
{
    ConstantPoolEntryInfo output;
    output.index = index;
    char tmp[64];
    switch (entry->tag()) {
    case jcm::CpTag::UTF8:
        output.tag  = "Utf8";
        output.text = static_cast<const jcm::CpUtf8*>(entry)->text();
        break;
    case jcm::CpTag::INTEGER:
        output.tag = "Integer";
        std::snprintf(tmp, sizeof(tmp), "%d", static_cast<const jcm::CpInt*>(entry)->value());
        output.text = tmp;
        break;
    case jcm::CpTag::FLOAT:
        output.tag = "Float";
        std::snprintf(tmp, sizeof(tmp), "%g", static_cast<const jcm::CpFloat*>(entry)->value());
        output.text = tmp;
        break;
    case jcm::CpTag::LONG:
        output.tag = "Long";
        std::snprintf(tmp,
                      sizeof(tmp),
                      "%lld",
                      static_cast<long long>(static_cast<const jcm::CpLong*>(entry)->value()));
        output.text = tmp;
        break;
    case jcm::CpTag::DOUBLE:
        output.tag = "Double";
        std::snprintf(tmp, sizeof(tmp), "%g", static_cast<const jcm::CpDouble*>(entry)->value());
        output.text = tmp;
        break;
    case jcm::CpTag::STRING:
    {
        output.tag = "String";
        const auto* text = static_cast<const jcm::CpString*>(entry)->value();
        if (text) output.text = text->text();
        break;
    }
    case jcm::CpTag::CLASS:
    {
        output.tag = "Class";
        const auto* name = static_cast<const jcm::CpClass*>(entry)->name();
        if (name) output.text = name->text();
        break;
    }
    case jcm::CpTag::FIELD_REF:
        output.tag = "FieldRef";
        if (resolve) output.text = describeConstRef(static_cast<const jcm::ConstRef*>(entry));
        break;
    case jcm::CpTag::METHOD_REF:
        output.tag = "MethodRef";
        if (resolve) output.text = describeConstRef(static_cast<const jcm::ConstRef*>(entry));
        break;
    case jcm::CpTag::INTERFACE_METHOD_REF:
        output.tag = "InterfaceMethodRef";
        if (resolve) output.text = describeConstRef(static_cast<const jcm::ConstRef*>(entry));
        break;
    case jcm::CpTag::NAME_TYPE:
    {
        output.tag = "NameAndType";
        if (resolve) {
            const auto* nt = static_cast<const jcm::CpNameType*>(entry);
            if (nt->name()) output.text = nt->name()->text();
            output.text += ":";
            if (nt->type()) output.text += nt->type()->text();
        }
        break;
    }
    case jcm::CpTag::METHOD_HANDLE:
    {
        output.tag = "MethodHandle";
        std::snprintf(tmp, sizeof(tmp), "kind=%d", static_cast<const jcm::CpMethodHandle*>(entry)->kind());
        output.text = tmp;
        break;
    }
    case jcm::CpTag::METHOD_TYPE:
    {
        output.tag = "MethodType";
        const auto* desc = static_cast<const jcm::CpMethodType*>(entry)->descriptor();
        if (desc) output.text = desc->text();
        break;
    }
    case jcm::CpTag::DYNAMIC:
    case jcm::CpTag::INVOKE_DYNAMIC:
    {
        const auto* dyn = static_cast<const jcm::ConstDynamic*>(entry);
        output.tag      = entry->tag() == jcm::CpTag::DYNAMIC ? "Dynamic" : "InvokeDynamic";
        std::snprintf(tmp, sizeof(tmp), "bsm=%d", dyn->bsmIndex());
        output.text = tmp;
        if (resolve && dyn->nameType() && dyn->nameType()->name()) {
            output.text += " ";
            output.text += dyn->nameType()->name()->text();
            output.text += ":";
            if (dyn->nameType()->type()) output.text += dyn->nameType()->type()->text();
        }
        break;
    }
    case jcm::CpTag::MODULE:
    {
        output.tag = "Module";
        const auto* name = static_cast<const jcm::CpModule*>(entry)->name();
        if (name) output.text = name->text();
        break;
    }
    case jcm::CpTag::PACKAGE:
    {
        output.tag = "Package";
        const auto* name = static_cast<const jcm::CpPackage*>(entry)->packageName();
        if (name) output.text = name->text();
        break;
    }
    default:
        //INTERNAL 等无数据条目：tag 名留给调用方按需补，这里仅判 malformed。
        break;
    }
    if (output.tag.empty()) output.tag = cpTagName(entry->tag());
    if (output.text.empty() &&
        (entry->tag() == jcm::CpTag::INTERNAL || jcm::Placeholders::containsPlaceholder(entry))) {
        output.malformed = true;
    }
    return output;
}

}   //namespace

//自匿名命名空间移出（内部链接 → 外部链接）：summary_scan.hpp 的声明需要跨 TU
//调用（jbc 对拍测试与后端接线）；所依赖的 visitor/error 助手仍在匿名域。
ca::StatusOr<ClassSummary> decodeClassSummary(const std::string& uri, const ClassResource& resource)
{
    try {
        ca::str::Utf8StringArena arena;
        ClassSummary             output;
        output.resourceUri = uri;
        ClassSummaryVisitor          visitor(output);
        jbytecode::core::ClassReader reader(arena, resource.get());
        reader.accept(&visitor,
                      jbytecode::core::ClassReader::SKIP_CODE |
                          jbytecode::core::ClassReader::SKIP_DEBUG |
                          jbytecode::core::ClassReader::SKIP_FRAMES);
        return ca::Ok(std::move(output));
    }
    catch (const std::exception& exception) {
        return ca::Err(
            error(ca::StatusCode::DATA_LOSS, "decode class summary " + uri, exception.what()));
    }
}

struct WorkspaceBase::Snapshot
{
    std::unique_ptr<ResourceContainer> container;
    std::string                        label;
};

struct WorkspaceBase::SupportingResource
{
    std::string                        id;
    std::string                        path;
    std::string                        fingerprint;
    std::unique_ptr<ResourceContainer> container;
};

WorkspaceBase::WorkspaceBase()
    : ownedArena_(std::make_unique<ca::str::Utf8StringArena>())
    , analysisManager_(std::make_unique<GlobalAnalysisManager>(*ownedArena_))
{
    arena_ = ownedArena_.get();
}

WorkspaceBase::~WorkspaceBase() = default;

ca::Status WorkspaceBase::openJar(const std::string& path)
{
    //缺省空 decoder = 原始字节直通（族缺省零漂移，见 2 形参重载）。
    return openJar(path, {});
}

ca::Status WorkspaceBase::openJar(const std::string& path,
                                  const IJarReader::EntryNameDecoder& decoder)
{
    if (path.empty()) return error(ca::StatusCode::INVALID_ARGUMENT, "open JAR", "path is empty");
    namespace fs = std::filesystem;
    std::error_code filesystemError;
    const auto      status = fs::status(fs::u8path(path), filesystemError);
    if (filesystemError || !fs::exists(status))
        return error(ca::StatusCode::NOT_FOUND, "open JAR", path);
    if (!fs::is_regular_file(status))
        return error(ca::StatusCode::INVALID_ARGUMENT, "open JAR", "path is not a regular file");

    try {
        ResourceContainer loaded;
        ResourceHelper::importFromJar(loaded, path, decoder);
        auto original = cloneContainer(loaded);
        if (original.is_err()) return original.unwrap_err();
        auto working = cloneContainer(loaded);
        if (working.is_err()) return working.unwrap_err();

        original_ = std::move(original).unwrap();
        working_  = std::move(working).unwrap();
        artifactKind_ = ArtifactKind::Jar;
        sourcePath_ = path;
        primaryClassUri_.clear();
        supporting_.clear();
        undo_.clear();
        redo_.clear();
        savepoints_.clear();
        revision_ = 0;
        nextSupportingId_ = 1;
        analysisManager_->clearAll();
        return ca::OkStatus();
    }
    catch (const std::exception& exception) {
        return error(ca::StatusCode::DATA_LOSS, "open JAR", exception.what());
    }
}

ca::Status WorkspaceBase::openClass(const std::string& path)
{
    if (path.empty()) return error(ca::StatusCode::INVALID_ARGUMENT, "open class", "path is empty");
    namespace fs = std::filesystem;
    std::error_code filesystemError;
    const auto      status = fs::status(fs::u8path(path), filesystemError);
    if (filesystemError || !fs::exists(status))
        return error(ca::StatusCode::NOT_FOUND, "open class", path);
    if (!fs::is_regular_file(status))
        return error(ca::StatusCode::INVALID_ARGUMENT, "open class", "path is not a regular file");

    try {
        ResourceContainer imported;
        ResourceHelper::importFromClassFile(imported, path);
        const auto* importedClass = ca::core::tag_cast::dyn_cast<const ClassResource>(imported.getResource(path));
        if (!importedClass) {
            return error(ca::StatusCode::DATA_LOSS,
                         "open class",
                         "class resource was not imported");
        }
        auto info = decodeClass(path, *importedClass);
        if (info.is_err()) return info.unwrap_err();
        if (info.unwrap().internalName.empty()) {
            return error(ca::StatusCode::DATA_LOSS,
                         "open class",
                         "class has an empty internal name");
        }

        const auto uri = info.unwrap().internalName + ".class";
        ResourceContainer loaded;
        loaded.addResource(uri,
                           std::make_unique<ClassResource>(uri, importedClass->get()));
        std::vector<jasm::Diagnostic> diagnostics;
        const auto validation = validateCandidate(loaded, diagnostics);
        if (validation.is_err()) {
            return error(ca::StatusCode::DATA_LOSS,
                         "open class",
                         "class did not pass strict validation");
        }

        auto original = cloneContainer(loaded);
        if (original.is_err()) return original.unwrap_err();
        auto working = cloneContainer(loaded);
        if (working.is_err()) return working.unwrap_err();

        original_ = std::move(original).unwrap();
        working_  = std::move(working).unwrap();
        artifactKind_ = ArtifactKind::Class;
        sourcePath_ = path;
        primaryClassUri_ = uri;
        supporting_.clear();
        undo_.clear();
        redo_.clear();
        savepoints_.clear();
        revision_ = 0;
        nextSupportingId_ = 1;
        analysisManager_->clearAll();
        return ca::OkStatus();
    }
    catch (const std::exception& exception) {
        return error(ca::StatusCode::DATA_LOSS, "open class", exception.what());
    }
}

ca::StatusOr<SupportingResourceInfo> WorkspaceBase::addSupportingJar(const std::string& path)
{
    auto loaded = ensureLoaded("add supporting JAR");
    if (loaded.is_err()) return ca::Err(loaded);
    auto normalized = normalizedRegularFile(path, "add supporting JAR");
    if (normalized.is_err()) return ca::Err(normalized.unwrap_err());
    for (const auto& support : supporting_) {
        if (support.path == normalized.unwrap()) {
            return ca::Err(error(ca::StatusCode::ALREADY_EXISTS,
                                 "add supporting JAR",
                                 "supporting path is already attached"));
        }
    }
    try {
        auto container = std::make_unique<ResourceContainer>();
        ResourceHelper::importFromJar(*container, normalized.unwrap());
        auto fingerprint = fingerprintContainer(*container);
        if (fingerprint.is_err()) return ca::Err(fingerprint.unwrap_err());
        for (const auto& support : supporting_) {
            if (support.fingerprint == fingerprint.unwrap()) {
                return ca::Err(error(ca::StatusCode::ALREADY_EXISTS,
                                     "add supporting JAR",
                                     "supporting content is already attached"));
            }
        }

        SupportingResourceInfo info;
        info.id            = "support-" + std::to_string(nextSupportingId_++);
        info.path          = normalized.unwrap();
        info.fingerprint   = fingerprint.unwrap();
        info.resourceCount = container->size();
        container->visitResources([&](const std::string&, const IResource& resource) {
            if (resource.getType() == ResourceType::Class) ++info.classCount;
        });
        supporting_.push_back({info.id, info.path, info.fingerprint, std::move(container)});
        analysisManager_->clearAll();
        return ca::Ok(std::move(info));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::DATA_LOSS, "add supporting JAR", exception.what()));
    }
}

ca::StatusOr<SupportingResourceInfo> WorkspaceBase::removeSupportingJar(const std::string& id)
{
    auto loaded = ensureLoaded("remove supporting JAR");
    if (loaded.is_err()) return ca::Err(loaded);
    if (id.empty()) {
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             "remove supporting JAR",
                             "support ID is empty"));
    }
    const auto found = std::find_if(supporting_.begin(), supporting_.end(), [&](const auto& support) {
        return support.id == id;
    });
    if (found == supporting_.end())
        return ca::Err(error(ca::StatusCode::NOT_FOUND, "remove supporting JAR", id));

    SupportingResourceInfo info;
    info.id            = found->id;
    info.path          = found->path;
    info.fingerprint   = found->fingerprint;
    info.resourceCount = found->container->size();
    found->container->visitResources([&](const std::string&, const IResource& resource) {
        if (resource.getType() == ResourceType::Class) ++info.classCount;
    });
    supporting_.erase(found);
    analysisManager_->clearAll();
    return ca::Ok(std::move(info));
}

bool WorkspaceBase::isLoaded() const noexcept
{
    return working_ != nullptr && original_ != nullptr;
}

ArtifactKind WorkspaceBase::artifactKind() const noexcept
{
    return artifactKind_;
}

const std::string& WorkspaceBase::sourcePath() const noexcept
{
    return sourcePath_;
}

const std::string& WorkspaceBase::primaryClassUri() const noexcept
{
    return primaryClassUri_;
}

ca::u64 WorkspaceBase::revision() const noexcept
{
    return revision_;
}

bool WorkspaceBase::isDirty() const
{
    if (!isLoaded()) return false;
    return !diffContainers(*original_, *working_).empty();
}

bool WorkspaceBase::canExport() const noexcept
{
    return isLoaded();
}

std::vector<PassDescription> WorkspaceBase::availablePasses()
{
    registerBuiltinPasses();
    std::vector<PassDescription> result;
    for (const auto& pass : PassNameRegistry::instance().list()) {
        const char* scope = pass.scope == PassScope::Method  ? "method"
                            : pass.scope == PassScope::Class ? "class"
                                                              : "global";
        result.push_back({pass.name, tr(pass.description.c_str()), scope});
    }
    return result;
}

std::vector<PipelineDescription> WorkspaceBase::availablePipelines()
{
    registerBuiltinPipelines();
    std::vector<PipelineDescription> result;
    for (const auto& pipeline : PassRegistry::instance().listPipelines())
        result.push_back({pipeline.name, tr(pipeline.description.c_str())});
    return result;
}

ca::Status WorkspaceBase::ensureLoaded(const std::string& operation) const
{
    if (!isLoaded())
        return error(ca::StatusCode::FAILED_PRECONDITION, operation, "workspace is not loaded");
    return ca::OkStatus();
}

bool WorkspaceBase::accepts(const MutationRequest& request) const
{
    return request.expectedRevision == kAnyRevision || request.expectedRevision == revision_;
}

MutationResult WorkspaceBase::revisionConflict() const
{
    MutationResult result;
    result.status = error(ca::StatusCode::ABORTED,
                          "mutate workspace",
                          "expected revision does not match current revision");
    result.revision   = revision_;
    result.exportable = canExport();
    return result;
}

MutationResult WorkspaceBase::stateFailure(ca::Status status) const
{
    MutationResult result;
    result.status     = std::move(status);
    result.revision   = revision_;
    result.exportable = canExport();
    return result;
}

ResourceContainer* WorkspaceBase::workingContainer() noexcept
{
    return working_.get();
}

const ResourceContainer* WorkspaceBase::workingContainer() const noexcept
{
    return working_.get();
}

std::unique_ptr<ResourceContainer> WorkspaceBase::cloneWorking() const
{
    auto clone = cloneContainer(*working_);
    if (clone.is_err()) throw std::runtime_error(clone.unwrap_err().to_string());
    return std::move(clone).unwrap();
}

std::unique_ptr<ResourceContainer> WorkspaceBase::cloneOriginal() const
{
    auto clone = cloneContainer(*original_);
    if (clone.is_err()) throw std::runtime_error(clone.unwrap_err().to_string());
    return std::move(clone).unwrap();
}

ca::StatusOr<std::unique_ptr<ResourceContainer>> WorkspaceBase::analysisClassContainer() const
{
    auto status = ensureLoaded("build analysis classpath");
    if (status.is_err()) return ca::Err(status);
    try {
        auto result = std::make_unique<ResourceContainer>();
        std::unordered_set<std::string> classNames;

        auto appendClasses = [&](const ResourceContainer& source, const std::string& prefix,
                                 bool primary) {
            source.visitResources([&](const std::string& uri, const IResource& resource) {
                const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
                if (!classResource) return;
                try {
                    ca::str::Utf8StringArena     arena;
                    jbytecode::core::ClassReader reader(arena, classResource->get());
                    const auto internalName = str(reader.getClassName());
                    if (!primary && !classNames.insert(internalName).second) return;
                    if (primary) classNames.insert(internalName);
                    const std::string targetUri = primary ? uri : prefix + uri;
                    result->addResource(targetUri,
                                        std::make_unique<ClassResource>(classResource->getLocation(),
                                                                        classResource->get()));
                }
                catch (...) {
                    //A malformed supporting class cannot supply hierarchy facts. Keep the primary
                    //workspace readable and let class-specific calls report their own diagnostics.
                }
            });
        };

        appendClasses(*working_, {}, true);
        for (ca::usize index = 0; index < supporting_.size(); ++index) {
            appendClasses(*supporting_[index].container,
                          "__mjt_support_" + std::to_string(index) + "/",
                          false);
        }
        return ca::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::INTERNAL,
                             "build analysis classpath",
                             exception.what()));
    }
}

std::unordered_map<std::string, std::string> WorkspaceBase::supportingClassIds() const
{
    std::unordered_set<std::string> primary;
    working_->visitResources([&](const std::string&, const IResource& resource) {
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!classResource) return;
        try {
            ca::str::Utf8StringArena     arena;
            jbytecode::core::ClassReader reader(arena, classResource->get());
            primary.insert(str(reader.getClassName()));
        }
        catch (...) {
        }
    });

    std::unordered_map<std::string, std::string> result;
    for (const auto& support : supporting_) {
        support.container->visitResources([&](const std::string&, const IResource& resource) {
            const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
            if (!classResource) return;
            try {
                ca::str::Utf8StringArena     arena;
                jbytecode::core::ClassReader reader(arena, classResource->get());
                const auto name = str(reader.getClassName());
                if (primary.find(name) == primary.end()) result.emplace(name, support.id);
            }
            catch (...) {
            }
        });
    }
    return result;
}

void WorkspaceBase::annotateReferenceSources(std::vector<ReferenceInfo>& references) const
{
    const auto supportingClasses = supportingClassIds();
    for (auto& reference : references) {
        const auto source = supportingClasses.find(reference.sourceClass);
        if (source == supportingClasses.end()) continue;
        reference.supporting = true;
        reference.supportId  = source->second;
    }
}

ca::StatusOr<std::vector<ResourceSummary>> WorkspaceBase::listResources(bool includeClasses) const
{
    auto status = ensureLoaded("list resources");
    if (status.is_err()) return ca::Err(status);
    std::vector<ResourceSummary> result;
    working_->visitResources([&](const std::string& uri, const IResource& resource) {
        if (!includeClasses && resource.getType() == ResourceType::Class) return;
        auto bytes = bytesOf(resource);
        if (bytes.is_err()) throw std::runtime_error(bytes.unwrap_err().to_string());
        result.push_back({uri, resourceTypeName(resource.getType()), bytes.unwrap().size()});
    });
    std::sort(result.begin(), result.end(), [](const ResourceSummary& left, const ResourceSummary& right) {
        return left.uri < right.uri;
    });
    return ca::Ok(std::move(result));
}

ca::StatusOr<ResourceContent> WorkspaceBase::readResource(const std::string& uri) const
{
    auto status = ensureLoaded("read resource");
    if (status.is_err()) return ca::Err(status);
    const auto* resource = working_->getResource(uri);
    if (!resource) return ca::Err(error(ca::StatusCode::NOT_FOUND, "read resource", uri));
    auto bytes = bytesOf(*resource);
    if (bytes.is_err()) return ca::Err(bytes.unwrap_err());
    ResourceContent result;
    result.summary = {uri, resourceTypeName(resource->getType()), bytes.unwrap().size()};
    result.bytes   = std::move(bytes).unwrap();
    return ca::Ok(std::move(result));
}

ca::StatusOr<ClassBytesInfo> WorkspaceBase::readClassBytes(const std::string& uriOrInternalName) const
{
    auto status = ensureLoaded("read class bytes");
    if (status.is_err()) return ca::Err(status);
    std::string uri;
    const auto* exact = working_->getResource(uriOrInternalName);
    if (exact && uriOrInternalName.size() >= 6 &&
        uriOrInternalName.compare(uriOrInternalName.size() - 6, 6, ".class") == 0) {
        uri = uriOrInternalName;
    }
    else {
        auto resolved = resolveClassUri(*working_, uriOrInternalName);
        if (resolved.is_err()) return ca::Err(resolved.unwrap_err());
        uri = std::move(resolved).unwrap();
    }
    const auto* resource = working_->getResource(uri);
    if (!resource) return ca::Err(error(ca::StatusCode::NOT_FOUND, "read class bytes", uri));
    auto bytes = bytesOf(*resource);
    if (bytes.is_err()) return ca::Err(bytes.unwrap_err());

    ClassBytesInfo result;
    result.resourceUri = uri;
    result.bytes       = std::move(bytes).unwrap();
    result.fingerprint = fingerprintBytes(result.bytes);
    try {
        ca::str::Utf8StringArena     arena;
        jbytecode::core::ClassReader reader(arena, result.bytes);
        result.internalName = str(reader.getClassName());
    }
    catch (...) {
        //Raw bytes remain useful for damaged class diagnostics when selected by exact URI.
    }
    return ca::Ok(std::move(result));
}

ca::StatusOr<std::string> WorkspaceBase::resolveClassUri(const ResourceContainer& container,
                                                      const std::string& uriOrInternalName) const
{
    if (uriOrInternalName.empty())
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT, "resolve class", "selector is empty"));
    if (ca::core::tag_cast::isa<ClassResource>(container.getResource(uriOrInternalName))) return ca::Ok(uriOrInternalName);
    try {
        ca::str::Utf8StringArena arena;
        std::string              match;
        container.visitResources([&](const std::string& uri, const IResource& resource) {
            const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
            if (!classResource || !match.empty()) return;
            jbytecode::core::ClassReader reader(arena, classResource->get());
            if (str(reader.getClassName()) == uriOrInternalName) match = uri;
        });
        if (match.empty()) return ca::Err(error(ca::StatusCode::NOT_FOUND, "resolve class", uriOrInternalName));
        return ca::Ok(std::move(match));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::DATA_LOSS, "resolve class", exception.what()));
    }
}

ca::StatusOr<std::vector<ClassInfo>> WorkspaceBase::listClasses() const
{
    auto status = ensureLoaded("list classes");
    if (status.is_err()) return ca::Err(status);
    std::vector<ClassInfo> result;
    working_->visitResources([&](const std::string& uri, const IResource& resource) {
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!classResource) return;
        auto decoded = decodeClass(uri, *classResource);
        if (decoded.is_err()) throw std::runtime_error(decoded.unwrap_err().to_string());
        result.push_back(std::move(decoded).unwrap());
    });
    std::sort(result.begin(), result.end(), [](const ClassInfo& left, const ClassInfo& right) {
        if (left.internalName != right.internalName) return left.internalName < right.internalName;
        return left.resourceUri < right.resourceUri;
    });
    return ca::Ok(std::move(result));
}

ca::StatusOr<std::vector<ClassSummary>> WorkspaceBase::listClassSummaries() const
{
    auto status = ensureLoaded("list class summaries");
    if (status.is_err()) return ca::Err(status);
    std::vector<ClassSummary> result;
    working_->visitResources([&](const std::string& uri, const IResource& resource) {
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!classResource) return;
        //mbase→libjbc 薄切片接线（--with_mbase_jbc=y）：summary 扫描走 jbc 后端，
        //与 legacy 字段级对拍等价（jbc_summary_scan_test.cpp 为回归门）。
#ifdef MBASE_WITH_JBC_SUMMARY
        auto decoded = decodeClassSummaryJbc(uri, *classResource);
#else
        auto decoded = decodeClassSummary(uri, *classResource);
#endif
        if (decoded.is_err()) throw std::runtime_error(decoded.unwrap_err().to_string());
        result.push_back(std::move(decoded).unwrap());
    });
    std::sort(
        result.begin(), result.end(), [](const ClassSummary& left, const ClassSummary& right) {
            if (left.internalName != right.internalName)
                return left.internalName < right.internalName;
            return left.resourceUri < right.resourceUri;
        });
    return ca::Ok(std::move(result));
}

ca::StatusOr<ClassInfo> WorkspaceBase::classInfo(const std::string& uriOrInternalName) const
{
    auto status = ensureLoaded("get class info");
    if (status.is_err()) return ca::Err(status);
    auto uri = resolveClassUri(*working_, uriOrInternalName);
    if (uri.is_err()) return ca::Err(uri.unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const ClassResource>(working_->getResource(uri.unwrap()));
    if (!resource) return ca::Err(error(ca::StatusCode::NOT_FOUND, "get class info", uri.unwrap()));
    return decodeClass(uri.unwrap(), *resource);
}

ca::StatusOr<MethodInfo> WorkspaceBase::methodInfo(const std::string& owner, const std::string& name,
                                                const std::string& descriptor) const
{
    if (name.empty() || descriptor.empty()) {
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             "get method info",
                             "name and descriptor are required"));
    }
    auto decoded = classInfo(owner);
    if (decoded.is_err()) return ca::Err(decoded.unwrap_err());
    for (const auto& method : decoded.unwrap().methods) {
        if (method.name == name && method.descriptor == descriptor) return ca::Ok(method);
    }
    return ca::Err(error(ca::StatusCode::NOT_FOUND,
                         "get method info",
                         owner + "." + name + descriptor));
}

ca::StatusOr<FieldInfo> WorkspaceBase::fieldInfo(const std::string& owner, const std::string& name,
                                              const std::string& descriptor) const
{
    auto status = ensureLoaded("get field info");
    if (status.is_err()) return ca::Err(status);
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             "get field info",
                             "owner, name and descriptor are required"));
    }
    auto uri = resolveClassUri(*working_, owner);
    if (uri.is_err()) return ca::Err(uri.unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const ClassResource>(working_->getResource(uri.unwrap()));
    if (!resource) return ca::Err(error(ca::StatusCode::NOT_FOUND, "get field info", owner));
    try {
        ca::str::Utf8StringArena     arena;
        jbytecode::tree::ClassNode   node;
        jbytecode::core::ClassReader reader(arena, resource->get());
        reader.accept(&node, 0);
        for (const auto* field : node.fields) {
            if (field && str(field->name) == name && str(field->desc) == descriptor)
                return ca::Ok(makeFieldInfo(*field));
        }
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::DATA_LOSS, "get field info", exception.what()));
    }
    return ca::Err(error(ca::StatusCode::NOT_FOUND,
                         "get field info",
                         owner + "." + name + descriptor));
}

ca::StatusOr<std::vector<ConstantPoolEntryInfo>> WorkspaceBase::constantPool(const std::string& uriOrInternalName,
                                                                         bool resolve) const
{
    auto status = ensureLoaded("get constant pool");
    if (status.is_err()) return ca::Err(status);
    auto uri = resolveClassUri(*working_, uriOrInternalName);
    if (uri.is_err()) return ca::Err(uri.unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const ClassResource>(working_->getResource(uri.unwrap()));
    if (!resource) return ca::Err(error(ca::StatusCode::NOT_FOUND, "get constant pool", uri.unwrap()));
    //用 jclassfile 容错解析（setSkipUnknownPoolTags），与 searchStrings 一致，
    //使损坏 class 的可用条目仍能枚举。
    try {
        jclassfile::io::ClassFileReader reader;
        reader.setSkipUnknownPoolTags(true);
        const auto                 classFile = reader.read(resource->get());
        const auto&                pool      = classFile.getPool();
        std::vector<ConstantPoolEntryInfo> result;
        for (ca::usize index = 1; index < pool.size(); ++index) {
            const auto* entry = pool.get(static_cast<int>(index));
            if (entry == nullptr) {
                //Long/Double 占用的第二槽：保留索引占位（tag/text 空），使结果条目数
                //等于 pool.size-1，与 JVM 常量池槽数一致；调用方渲染时按空 tag 跳过。
                result.push_back({index, {}, {}, false});
                continue;
            }
            result.push_back(describeCpEntry(entry, index, resolve));
        }
        return ca::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::DATA_LOSS, "get constant pool", exception.what()));
    }
}

ca::StatusOr<MethodInstructions> WorkspaceBase::methodInstructions(const std::string& owner,
                                                               const std::string& name,
                                                               const std::string& descriptor,
                                                               bool               resolve) const
{
    auto status = ensureLoaded("get method instructions");
    if (status.is_err()) return ca::Err(status);
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             "get method instructions",
                             "owner, name and descriptor are required"));
    }
    auto uri = resolveClassUri(*working_, owner);
    if (uri.is_err()) return ca::Err(uri.unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const ClassResource>(working_->getResource(uri.unwrap()));
    if (!resource) return ca::Err(error(ca::StatusCode::NOT_FOUND, "get method instructions", owner));
    //用 jclassfile 容错解析（与 mjt info/disasm 原后端一致），保留 CP 索引与字节 PC，
    //使渲染输出与改前逐字节对齐。
    try {
        jclassfile::io::ClassFileReader reader;
        reader.setSkipUnknownPoolTags(true);
        const auto  classFile = reader.read(resource->get());
        const auto& pool      = classFile.getPool();
        for (const auto* method : classFile.getMethods()) {
            if (!method || !method->name() || method->name()->text() != name || !method->type() ||
                method->type()->text() != descriptor)
                continue;
            const jcm::CodeAttribute* codeAttr = nullptr;
            for (const auto* attr : method->attributes()) {
                if (attr && attr->isCodeAttribute()) {
                    codeAttr = static_cast<const jcm::CodeAttribute*>(attr);
                    break;
                }
            }
            if (!codeAttr) return ca::Err(error(ca::StatusCode::NOT_FOUND,
                                                "get method instructions",
                                                "method has no Code attribute"));
            MethodInstructions result;
            result.maxStack  = codeAttr->maxStack();
            result.maxLocals = codeAttr->maxLocals();
            ca::i32 pc       = 0;
            for (const auto* insn : codeAttr->instructions()) {
                if (!insn) continue;
                InstructionDetailInfo info;
                info.pc         = pc;
                info.opcode     = insn->getOpcode();
                const char* insnName = jclassfile::util::OpcodeUtil::getOpcodeName(insn->getOpcode());
                info.mnemonic   = insnName ? insnName : "?";
                info.operandText = instructionOperandText(insn, &pool);
                if (resolve) info.operandComment = instructionOperandComment(insn, &pool);
                const auto cpAccesses = insn->cpAccesses();
                if (!cpAccesses.empty() && cpAccesses[0]) info.cpIndex = cpAccesses[0]->index();
                ca::i32                   defaultPc = -1;
                std::vector<SwitchCaseInfo> cases;
                if (instructionSwitchTargets(insn, pc, defaultPc, cases)) {
                    info.isSwitch        = true;
                    info.switchDefaultPc = defaultPc;
                    info.switchCases     = std::move(cases);
                }
                result.instructions.push_back(std::move(info));
                const auto size = insn->computeSize();
                pc += (size > 0) ? size : 1;
            }
            return ca::Ok(std::move(result));
        }
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::DATA_LOSS, "get method instructions", exception.what()));
    }
    return ca::Err(error(ca::StatusCode::NOT_FOUND,
                         "get method instructions",
                         owner + "." + name + descriptor));
}

ca::StatusOr<MethodBytecode> WorkspaceBase::methodBytecode(const std::string& owner,
                                                        const std::string& name,
                                                        const std::string& descriptor) const
{
    auto status = ensureLoaded("get method bytecode");
    if (status.is_err()) return ca::Err(status);
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             "get method bytecode",
                             "owner, name and descriptor are required"));
    }
    auto uri = resolveClassUri(*working_, owner);
    if (uri.is_err()) return ca::Err(uri.unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const ClassResource>(working_->getResource(uri.unwrap()));
    if (!resource) return ca::Err(error(ca::StatusCode::NOT_FOUND, "get method bytecode", owner));
    try {
        ca::str::Utf8StringArena     arena;
        jbytecode::tree::ClassNode   node;
        jbytecode::core::ClassReader reader(arena, resource->get());
        reader.accept(&node, 0);
        for (const auto* method : node.methods) {
            if (!method || str(method->name) != name || str(method->desc) != descriptor) continue;
            MethodBytecode result;
            result.method = makeMethodInfo(*method);
            for (ca::i32 index = 0; index < method->instructions.size(); ++index) {
                const auto* instruction = method->instructions.get(index);
                result.instructions.push_back(
                    {index,
                     instruction->getOpcode(),
                     instructionMnemonic(*instruction),
                     instructionNodeType(*instruction)});
            }
            return ca::Ok(std::move(result));
        }
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::DATA_LOSS, "get method bytecode", exception.what()));
    }
    return ca::Err(error(ca::StatusCode::NOT_FOUND,
                         "get method bytecode",
                         owner + "." + name + descriptor));
}

jasm::Result<jasm::JvmJasmMethodDisassembly> WorkspaceBase::getMethodJasm(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    auto uri = resolveClassUri(*working_, owner);
    if (uri.is_err()) {
        jasm::Result<jasm::JvmJasmMethodDisassembly> result;
        result.diagnostics.push_back({jasm::DiagnosticSeverity::Error,
                                      jasm::DiagnosticStage::Resource,
                                      utf16(uri.unwrap_err().to_string()),
                                      std::nullopt});
        return result;
    }
    const auto* resource = ca::core::tag_cast::dyn_cast<const ClassResource>(working_->getResource(uri.unwrap()));
    if (!resource)
        return jasmFailure<jasm::JvmJasmMethodDisassembly>("Class resource not found: " + owner);
    const auto nameRef = ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(name.data()), name.size());
    const auto descriptorRef = ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(descriptor.data()), descriptor.size());
    return jasm::JvmJasmService{}.disassembleMethod(resource->get(), nameRef, descriptorRef);
}

ca::StatusOr<std::vector<ManifestAttribute>> WorkspaceBase::manifestAttributes() const
{
    auto status = ensureLoaded("read manifest");
    if (status.is_err()) return ca::Err(status);
    const ManifestResource* manifest = nullptr;
    working_->visitResources([&](const std::string&, const IResource& resource) {
        if (!manifest) manifest = ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource);
    });
    if (!manifest) return ca::Err(error(ca::StatusCode::NOT_FOUND, "read manifest", "MANIFEST.MF"));
    std::vector<ManifestAttribute> result;
    result.reserve(manifest->get().getMainAttributes().size());
    for (const auto& [key, value] : manifest->get().getMainAttributes()) result.push_back({key, value});
    std::sort(result.begin(), result.end(), [](const ManifestAttribute& left,
                                               const ManifestAttribute& right) {
        return left.key < right.key;
    });
    return ca::Ok(std::move(result));
}

ca::StatusOr<std::vector<ResourceStringInfo>> WorkspaceBase::resourceStrings(ca::usize minimumLength) const
{
    auto status = ensureLoaded("extract resource strings");
    if (status.is_err()) return ca::Err(status);
    if (minimumLength == 0)
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             "extract resource strings",
                             "minimum length must be positive"));
    try {
        std::vector<ResourceStringInfo> result;
        working_->visitResources([&](const std::string& uri, const IResource& resource) {
            if (resource.getType() == ResourceType::Class) return;
            auto bytes = bytesOf(resource);
            if (bytes.is_err()) return;
            const auto& value = bytes.unwrap();
            ca::usize start = 0;
            for (ca::usize index = 0; index <= value.size(); ++index) {
                const bool printable = index < value.size() && value[index] >= 0x20 && value[index] <= 0x7e;
                if (printable) continue;
                if (index - start >= minimumLength) {
                    result.push_back({uri,
                                      start,
                                      std::string(reinterpret_cast<const char*>(value.data() + start),
                                                  index - start)});
                }
                start = index + 1;
            }
        });
        std::sort(result.begin(), result.end(), [](const ResourceStringInfo& left,
                                                   const ResourceStringInfo& right) {
            if (left.uri != right.uri) return left.uri < right.uri;
            if (left.offset != right.offset) return left.offset < right.offset;
            return left.value < right.value;
        });
        return ca::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::INTERNAL, "extract resource strings", exception.what()));
    }
}

ca::StatusOr<std::vector<SearchHit>> WorkspaceBase::searchClasses(
    const std::string& query, const SearchOptions& options) const
{
    auto status = ensureLoaded("search classes");
    if (status.is_err()) return ca::Err(status);
    auto compiled = compileSearchRegex(query, options, "search classes");
    if (compiled.is_err()) return ca::Err(compiled.unwrap_err());
    auto compiledRegex = std::move(compiled).unwrap();
    const auto* regex = compiledRegex ? &*compiledRegex : nullptr;

    std::vector<SearchHit> result;
    working_->visitResources([&](const std::string& uri, const IResource& resource) {
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!classResource) return;
        auto decoded = decodeClass(uri, *classResource);
        if (decoded.is_err()) return;
        const auto& info = decoded.unwrap();
        if (!matchesSearchScope(info.internalName, options)) return;
        if (!matchesAnyText({info.internalName, info.resourceUri}, query, options, regex)) {
            return;
        }
        result.push_back({info.resourceUri,
                          "class",
                          info.internalName,
                          {},
                          {},
                          -1,
                          info.internalName});
    });
    sortAndDeduplicateSearchHits(result);
    return ca::Ok(std::move(result));
}

ca::StatusOr<std::vector<SearchHit>> WorkspaceBase::searchMembers(
    const std::string& query, const SearchOptions& options) const
{
    auto status = ensureLoaded("search members");
    if (status.is_err()) return ca::Err(status);
    auto compiled = compileSearchRegex(query, options, "search members");
    if (compiled.is_err()) return ca::Err(compiled.unwrap_err());
    auto compiledRegex = std::move(compiled).unwrap();
    const auto* regex = compiledRegex ? &*compiledRegex : nullptr;

    std::vector<SearchHit> result;
    working_->visitResources([&](const std::string& uri, const IResource& resource) {
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!classResource) return;
        auto decoded = decodeClass(uri, *classResource);
        if (decoded.is_err()) return;
        const auto& info = decoded.unwrap();
        if (!matchesSearchScope(info.internalName, options)) return;
        for (const auto& field : info.fields) {
            if (!options.memberKind.empty() && options.memberKind != "field") continue;
            const auto searchable = info.internalName + " " + field.name + " " +
                                    field.descriptor + " " + field.signature;
            if (!matchesAnyText({info.internalName,
                                 field.name,
                                 field.descriptor,
                                 field.signature,
                                 searchable},
                                query,
                                options,
                                regex)) {
                continue;
            }
            result.push_back({info.resourceUri,
                              "field",
                              info.internalName,
                              field.name,
                              field.descriptor,
                              -1,
                              field.name + field.descriptor});
        }
        for (const auto& method : info.methods) {
            if (!options.memberKind.empty() && options.memberKind != "method") continue;
            const auto searchable = info.internalName + " " + method.name + " " +
                                    method.descriptor + " " + method.signature;
            if (!matchesAnyText({info.internalName,
                                 method.name,
                                 method.descriptor,
                                 method.signature,
                                 searchable},
                                query,
                                options,
                                regex)) {
                continue;
            }
            result.push_back({info.resourceUri,
                              "method",
                              info.internalName,
                              method.name,
                              method.descriptor,
                              -1,
                              method.name + method.descriptor});
        }
    });
    sortAndDeduplicateSearchHits(result);
    return ca::Ok(std::move(result));
}

ca::StatusOr<std::vector<SearchHit>> WorkspaceBase::searchStrings(
    const std::string& query, const SearchOptions& options) const
{
    auto status = ensureLoaded("search strings");
    if (status.is_err()) return ca::Err(status);
    auto compiled = compileSearchRegex(query, options, "search strings");
    if (compiled.is_err()) return ca::Err(compiled.unwrap_err());
    auto compiledRegex = std::move(compiled).unwrap();
    const auto* regex = compiledRegex ? &*compiledRegex : nullptr;

    std::vector<SearchHit> result;
    auto resources = resourceStrings(1);
    if (resources.is_err()) return ca::Err(resources.unwrap_err());
    for (const auto& item : resources.unwrap()) {
        if (!options.owner.empty() || !options.packagePrefix.empty()) continue;
        if (!matchesText(item.value, query, options, regex)) continue;
        result.push_back({item.uri, "resource_string", {}, {}, {}, -1, item.value});
    }

    working_->visitResources([&](const std::string& uri, const IResource& resource) {
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!classResource) return;
        try {
            jclassfile::io::ClassFileReader reader;
            reader.setSkipUnknownPoolTags(true);
            const auto classFile = reader.read(classResource->get());
            const auto& pool = classFile.getPool();
            const auto owner = classFile.getName();
            if (!matchesSearchScope(owner, options)) return;
            for (int index = 1; index < static_cast<int>(pool.size()); ++index) {
                const auto* entry = pool.get(index);
                if (!entry) continue;
                std::string value;
                std::string kind;
                if (entry->tag() == jclassfile::model::CpTag::UTF8) {
                    value = static_cast<const jclassfile::model::CpUtf8*>(entry)->text();
                    kind = "constant_pool_utf8";
                }
                else if (entry->tag() == jclassfile::model::CpTag::STRING) {
                    const auto* text = static_cast<const jclassfile::model::CpString*>(entry)->value();
                    if (!text) continue;
                    value = text->text();
                    kind = "constant_pool_string";
                }
                else {
                    continue;
                }
                if (!matchesText(value, query, options, regex)) continue;
                result.push_back({uri, kind, owner, {}, {}, -1, std::move(value)});
            }
        }
        catch (const std::exception&) {
            ///搜索在敌意或不完整 class 上应返回其余可用命中。
        }
    });
    sortAndDeduplicateSearchHits(result);
    return ca::Ok(std::move(result));
}

ca::StatusOr<std::vector<SearchHit>> WorkspaceBase::searchNumbers(
    const std::string& query, const SearchOptions& options) const
{
    auto status = ensureLoaded("search numbers");
    if (status.is_err()) return ca::Err(status);
    status = validateSearchOptions(query, options, "search numbers");
    if (status.is_err()) return ca::Err(status);
    status = validateNumberQuery(query);
    if (status.is_err()) return ca::Err(status);

    std::vector<SearchHit> result;
    working_->visitResources([&](const std::string& uri, const IResource& resource) {
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!classResource) return;
        try {
            jclassfile::io::ClassFileReader reader;
            reader.setSkipUnknownPoolTags(true);
            const auto classFile = reader.read(classResource->get());
            const auto& pool = classFile.getPool();
            const auto owner = classFile.getName();
            if (!matchesSearchScope(owner, options)) return;
            if (options.opcode < 0) {
                for (int index = 1; index < static_cast<int>(pool.size()); ++index) {
                    const auto* entry = pool.get(index);
                    if (!entry) continue;
                    std::string value;
                    switch (entry->tag()) {
                    case jclassfile::model::CpTag::INTEGER:
                        value = numberText(static_cast<const jclassfile::model::CpInt*>(entry)->value());
                        break;
                    case jclassfile::model::CpTag::FLOAT:
                        value = numberText(static_cast<const jclassfile::model::CpFloat*>(entry)->value());
                        break;
                    case jclassfile::model::CpTag::LONG:
                        value = numberText(static_cast<const jclassfile::model::CpLong*>(entry)->value());
                        break;
                    case jclassfile::model::CpTag::DOUBLE:
                        value = numberText(static_cast<const jclassfile::model::CpDouble*>(entry)->value());
                        break;
                    default: continue;
                    }
                    if (!matchesNumber(value, query)) continue;
                    result.push_back({uri,
                                      "constant_pool_number",
                                      owner,
                                      {},
                                      {},
                                      -1,
                                      std::move(value)});
                }
            }
        }
        catch (const std::exception&) {
            ///常量池读取失败不应阻止随后可解析 class 的搜索。
        }

        try {
            ca::str::Utf8StringArena     arena;
            jbytecode::tree::ClassNode   node;
            jbytecode::core::ClassReader reader(arena, classResource->get());
            reader.accept(&node, 0);
            const auto owner = str(node.name);
            if (!matchesSearchScope(owner, options)) return;
            for (const auto* method : node.methods) {
                if (!method) continue;
                for (ca::i32 index = 0; index < method->instructions.size(); ++index) {
                    const auto* instruction = method->instructions.get(index);
                    if (options.opcode >= 0 && instruction->getOpcode() != options.opcode) continue;
                    for (const auto& value : instructionNumbers(*instruction)) {
                        if (!matchesNumber(value, query)) continue;
                        result.push_back({uri,
                                          "instruction_number",
                                          owner,
                                          str(method->name),
                                          str(method->desc),
                                          index,
                                          value});
                    }
                }
            }
        }
        catch (const std::exception&) {
            ///和常量池路径一样，单个 class 失败只影响它本身。
        }
    });
    sortAndDeduplicateSearchHits(result);
    return ca::Ok(std::move(result));
}

ca::StatusOr<std::vector<SearchHit>> WorkspaceBase::searchInstructions(
    const std::string& query, const SearchOptions& options) const
{
    auto status = ensureLoaded("search instructions");
    if (status.is_err()) return ca::Err(status);
    auto compiled = compileSearchRegex(query, options, "search instructions");
    if (compiled.is_err()) return ca::Err(compiled.unwrap_err());
    auto compiledRegex = std::move(compiled).unwrap();
    const auto* regex = compiledRegex ? &*compiledRegex : nullptr;

    std::vector<SearchHit> result;
    working_->visitResources([&](const std::string& uri, const IResource& resource) {
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!classResource) return;
        try {
            ca::str::Utf8StringArena     arena;
            jbytecode::tree::ClassNode   node;
            jbytecode::core::ClassReader reader(arena, classResource->get());
            reader.accept(&node, 0);
            const auto owner = str(node.name);
            if (!matchesSearchScope(owner, options)) return;
            for (const auto* method : node.methods) {
                if (!method) continue;
                for (ca::i32 index = 0; index < method->instructions.size(); ++index) {
                    const auto* instruction = method->instructions.get(index);
                    if (options.opcode >= 0 && instruction->getOpcode() != options.opcode) continue;
                    const auto value = instructionSearchText(*instruction);
                    if (!matchesText(value, query, options, regex)) continue;
                    result.push_back({uri,
                                      "instruction",
                                      owner,
                                      str(method->name),
                                      str(method->desc),
                                      index,
                                      value});
                }
            }
        }
        catch (const std::exception&) {
            ///见搜索契约：坏 class 的诊断由独立 JAR 修复/验证工具负责。
        }
    });
    sortAndDeduplicateSearchHits(result);
    return ca::Ok(std::move(result));
}

ca::StatusOr<std::vector<ReferenceInfo>> WorkspaceBase::xrefsToMethod(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    auto status = ensureLoaded("xref method");
    if (status.is_err()) return ca::Err(status);
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             "xref method",
                             "owner, name and descriptor are required"));
    }
    try {
        ca::str::Utf8StringArena arena;
        auto analysisClasses = analysisClassContainer();
        if (analysisClasses.is_err()) return ca::Err(analysisClasses.unwrap_err());
        auto analysisContainer = std::move(analysisClasses).unwrap();
        ReferenceIndex            index;
        index.build(*analysisContainer, arena);
        std::vector<ReferenceInfo> result;
        for (const auto& reference : index.methodReferencesTo(owner, name, descriptor)) {
            result.push_back(referenceInfo(reference.site,
                                           methodTarget(reference.target),
                                           methodReferenceKind(reference.kind)));
        }
        annotateReferenceSources(result);
        sortReferences(result);
        return ca::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::INTERNAL, "xref method", exception.what()));
    }
}

ca::StatusOr<std::vector<ReferenceInfo>> WorkspaceBase::xrefsToField(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    auto status = ensureLoaded("xref field");
    if (status.is_err()) return ca::Err(status);
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             "xref field",
                             "owner, name and descriptor are required"));
    }
    try {
        ca::str::Utf8StringArena arena;
        auto analysisClasses = analysisClassContainer();
        if (analysisClasses.is_err()) return ca::Err(analysisClasses.unwrap_err());
        auto analysisContainer = std::move(analysisClasses).unwrap();
        ReferenceIndex            index;
        index.build(*analysisContainer, arena);
        std::vector<ReferenceInfo> result;
        for (const auto& reference : index.fieldReferencesTo(owner, name, descriptor)) {
            result.push_back(referenceInfo(reference.site,
                                           fieldTarget(reference.target),
                                           fieldReferenceKind(reference.kind)));
        }
        annotateReferenceSources(result);
        sortReferences(result);
        return ca::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::INTERNAL, "xref field", exception.what()));
    }
}

ca::StatusOr<std::vector<ReferenceInfo>> WorkspaceBase::xrefsToClass(
    const std::string& internalName) const
{
    auto status = ensureLoaded("xref class");
    if (status.is_err()) return ca::Err(status);
    if (internalName.empty())
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT, "xref class", "class is empty"));
    try {
        ca::str::Utf8StringArena arena;
        auto analysisClasses = analysisClassContainer();
        if (analysisClasses.is_err()) return ca::Err(analysisClasses.unwrap_err());
        auto analysisContainer = std::move(analysisClasses).unwrap();
        ReferenceIndex            index;
        index.build(*analysisContainer, arena);
        std::vector<ReferenceInfo> result;
        for (const auto& reference : index.typeReferencesTo(internalName)) {
            result.push_back(referenceInfo(reference.site, reference.targetType, "type"));
        }
        annotateReferenceSources(result);
        sortReferences(result);
        return ca::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::INTERNAL, "xref class", exception.what()));
    }
}

ca::StatusOr<std::vector<ReferenceInfo>> WorkspaceBase::calleesOfMethod(
    const std::string& owner, const std::string& name, const std::string& descriptor, bool resolved) const
{
    auto status = ensureLoaded("list callees");
    if (status.is_err()) return ca::Err(status);
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             "list callees",
                             "owner, name and descriptor are required"));
    }
    try {
        ca::str::Utf8StringArena arena;
        auto analysisClasses = analysisClassContainer();
        if (analysisClasses.is_err()) return ca::Err(analysisClasses.unwrap_err());
        auto analysisContainer = std::move(analysisClasses).unwrap();
        ReferenceIndex            index;
        index.build(*analysisContainer, arena);
        std::vector<ReferenceInfo> result;
        if (!resolved) {
            for (const auto& reference : index.methodReferencesFrom(owner, name, descriptor)) {
                result.push_back(referenceInfo(reference.site,
                                               methodTarget(reference.target),
                                               methodReferenceKind(reference.kind)));
            }
        }
        else {
            CallGraph       direct;
            ClassHierarchy  classes;
            MethodHierarchy methods;
            direct.buildFromIndex(index);
            classes.build(*analysisContainer, arena);
            methods.build(classes);
            ResolvedCallGraph graph;
            graph.build(index, direct, classes, methods);
            for (const auto& edge : graph.edgesFrom(owner, name, descriptor)) {
                result.push_back(referenceInfo(edge.site,
                                               methodTarget(edge.resolvedTarget),
                                               edge.expanded ? "resolved_override" : "direct"));
            }
        }
        annotateReferenceSources(result);
        sortReferences(result);
        return ca::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::INTERNAL, "list callees", exception.what()));
    }
}

ca::StatusOr<ClassRelations> WorkspaceBase::classRelations(const std::string& internalName) const
{
    auto status = ensureLoaded("get class relations");
    if (status.is_err()) return ca::Err(status);
    if (internalName.empty()) {
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             "get class relations",
                             "class is empty"));
    }
    try {
        auto analysisClasses = analysisClassContainer();
        if (analysisClasses.is_err()) return ca::Err(analysisClasses.unwrap_err());
        auto analysisContainer = std::move(analysisClasses).unwrap();
        ca::str::Utf8StringArena arena;
        ClassHierarchy           hierarchy;
        hierarchy.build(*analysisContainer, arena);
        const auto index = hierarchy.indexOf(arena.intern(internalName.c_str()));
        if (index == INVALID_CLASS)
            return ca::Err(error(ca::StatusCode::NOT_FOUND, "get class relations", internalName));

        ClassRelations result;
        for (auto current = hierarchy.superClass(index); current != INVALID_CLASS;
             current = hierarchy.superClass(current)) {
            result.superclasses.push_back(str(hierarchy.nameOf(current)));
        }
        for (const auto ancestor : hierarchy.ancestors(index)) {
            if (ancestor == index || !hierarchy.isInterface(ancestor)) continue;
            result.interfaces.push_back(str(hierarchy.nameOf(ancestor)));
        }
        for (ClassIndex candidate = 0;
             candidate < static_cast<ClassIndex>(hierarchy.classCount());
             ++candidate) {
            const auto& entry = hierarchy.entry(candidate);
            if (!entry.classNode || candidate == index) continue;
            if (hierarchy.superClass(candidate) == index)
                result.directSubclasses.push_back(str(entry.name));
            if (hierarchy.isSubType(candidate, index)) {
                if (hierarchy.isInterface(index) && !hierarchy.isInterface(candidate)) {
                    result.implementors.push_back(str(entry.name));
                }
                if (!hierarchy.isInterface(index)) result.allSubclasses.push_back(str(entry.name));
            }
        }
        sortUnique(result.superclasses);
        sortUnique(result.interfaces);
        sortUnique(result.directSubclasses);
        sortUnique(result.allSubclasses);
        sortUnique(result.implementors);
        std::set<std::string> relatedClasses{internalName};
        relatedClasses.insert(result.superclasses.begin(), result.superclasses.end());
        relatedClasses.insert(result.interfaces.begin(), result.interfaces.end());
        relatedClasses.insert(result.directSubclasses.begin(), result.directSubclasses.end());
        relatedClasses.insert(result.allSubclasses.begin(), result.allSubclasses.end());
        relatedClasses.insert(result.implementors.begin(), result.implementors.end());
        const auto supportingClasses = supportingClassIds();
        for (const auto& name : relatedClasses) {
            const auto source = supportingClasses.find(name);
            result.sources.push_back({name,
                                      source != supportingClasses.end(),
                                      source == supportingClasses.end() ? std::string{} : source->second});
        }
        return ca::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::INTERNAL, "get class relations", exception.what()));
    }
}

ca::StatusOr<std::vector<MethodOverrideInfo>> WorkspaceBase::overridesOfMethod(
    const std::string& owner, const std::string& name, const std::string& descriptor) const
{
    auto status = ensureLoaded("get method overrides");
    if (status.is_err()) return ca::Err(status);
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::Err(error(ca::StatusCode::INVALID_ARGUMENT,
                             "get method overrides",
                             "owner, name and descriptor are required"));
    }
    try {
        auto analysisClasses = analysisClassContainer();
        if (analysisClasses.is_err()) return ca::Err(analysisClasses.unwrap_err());
        auto analysisContainer = std::move(analysisClasses).unwrap();

        //override 闭包游走收敛 renamer 只读视图 RenameContext（luiox/
        //P3）：与引擎共用同一 ClassHierarchy/MethodHierarchy 构建与组件解析，
        //不再自持一份并查集游走副本。primary/supporting 装饰与 NOT_FOUND 错误
        //语义是工作区面特有约定，留在本层。
        auto context = RenameContext::build(*analysisContainer);
        if (context.is_err()) return ca::Err(std::move(context).unwrap_err());
        auto view = std::move(context).unwrap();

        const auto viewRef = [](const std::string& value) {
            return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                                     value.size());
        };
        const auto ownerRef = viewRef(owner);
        if (!view->classExists(ownerRef))
            return ca::Err(error(ca::StatusCode::NOT_FOUND, "get method overrides", owner));

        auto component = view->overrideComponentOf(ownerRef, viewRef(name), viewRef(descriptor));
        if (component.is_err())
            return ca::Err(error(ca::StatusCode::NOT_FOUND,
                                 "get method overrides",
                                 owner + "." + name + descriptor));

        const auto supportingClasses = supportingClassIds();

        std::vector<MethodOverrideInfo> result;
        for (const auto& member : std::move(component).unwrap()) {
            const auto methodOwner = member.owner.to_std_string();
            const auto support     = supportingClasses.find(methodOwner);
            result.push_back({methodOwner,
                              member.name.to_std_string(),
                              member.desc.to_std_string(),
                              methodOwner == owner && member.name.equals(viewRef(name)) &&
                                  member.desc.equals(viewRef(descriptor)),
                              support != supportingClasses.end(),
                              support == supportingClasses.end() ? std::string{} : support->second});
        }
        std::sort(result.begin(), result.end(), [](const MethodOverrideInfo& left,
                                                   const MethodOverrideInfo& right) {
            if (left.owner != right.owner) return left.owner < right.owner;
            if (left.name != right.name) return left.name < right.name;
            return left.descriptor < right.descriptor;
        });
        return ca::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::INTERNAL, "get method overrides", exception.what()));
    }
}

jasm::Result<jasm::JvmJasmDisassembly> WorkspaceBase::getJasm(const std::string& uri) const
{
    if (!isLoaded()) return jasmFailure<jasm::JvmJasmDisassembly>("workspace is not loaded");
    jasm::ResourceJasmService service(*working_, *analysisManager_);
    return service.disassemble(uri);
}

MutationResult WorkspaceBase::validateJasm(const std::string& uri, ca::str::Utf16StringRef sourceName,
                                       ca::str::Utf16StringRef source,
                                       const MutationRequest& request)
{
    auto status = ensureLoaded("validate JASM");
    if (status.is_err()) return stateFailure(status);
    if (!accepts(request)) return revisionConflict();
    try {
        auto candidate = cloneWorking();
        ca::str::Utf8StringArena candidateArena;
        GlobalAnalysisManager    candidateManager(candidateArena);
        ClassMutationService service(*candidate, candidateManager);
        auto assembled = service.applyJasm(uri, sourceName, source);

        MutationResult result;
        result.revision    = revision_;
        result.exportable  = canExport();
        result.diagnostics = std::move(assembled.diagnostics);
        if (!assembled.isOk()) {
            result.status = error(ca::StatusCode::INVALID_ARGUMENT,
                                  "validate JASM",
                                  "candidate class did not pass validation");
            return result;
        }
        result.status           = ca::OkStatus();
        result.byteDiffs        = diffContainers(*working_, *candidate);
        result.affectedClasses  = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL, "validate JASM", exception.what()));
    }
}

MutationResult WorkspaceBase::commit(std::unique_ptr<ResourceContainer> candidate,
                                 std::vector<jasm::Diagnostic> diagnostics,
                                 const MutationRequest& request)
{
    if (!accepts(request)) return revisionConflict();
    auto before = cloneWorking();
    auto diffs  = diffContainers(*before, *candidate);

    MutationResult result;
    result.status      = ca::OkStatus();
    result.diagnostics = std::move(diagnostics);
    result.exportable  = true;
    result.byteDiffs   = std::move(diffs);
    if (result.byteDiffs.empty()) {
        result.revision = revision_;
        return result;
    }

    result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
    result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
    undo_.push_back({std::move(before), request.label});
    redo_.clear();
    working_ = std::move(candidate);
    analysisManager_->clearAll();
    ++revision_;
    result.revision  = revision_;
    result.committed = true;
    return result;
}

MutationResult WorkspaceBase::applyJasm(const std::string& uri, ca::str::Utf16StringRef sourceName,
                                    ca::str::Utf16StringRef source,
                                    const MutationRequest& request)
{
    if (request.dryRun) return validateJasm(uri, sourceName, source, request);
    auto status = ensureLoaded("apply JASM");
    if (status.is_err()) return stateFailure(status);
    if (!accepts(request)) return revisionConflict();
    try {
        auto candidate = cloneWorking();
        ca::str::Utf8StringArena candidateArena;
        GlobalAnalysisManager    candidateManager(candidateArena);
        ClassMutationService service(*candidate, candidateManager);
        auto assembled = service.applyJasm(uri, sourceName, source);
        if (!assembled.isOk()) {
            MutationResult result;
            result.status = error(ca::StatusCode::INVALID_ARGUMENT,
                                  "apply JASM",
                                  "candidate class did not pass validation");
            result.revision    = revision_;
            result.exportable  = canExport();
            result.diagnostics = std::move(assembled.diagnostics);
            return result;
        }
        return commit(std::move(candidate), std::move(assembled.diagnostics), request);
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL, "apply JASM", exception.what()));
    }
}

MutationResult WorkspaceBase::validateClassBytes(const std::string& uri,
                                             const std::vector<ca::u8>& classBytes,
                                             const MutationRequest& request)
{
    auto status = ensureLoaded("validate class bytes");
    if (status.is_err()) return stateFailure(status);
    if (!accepts(request)) return revisionConflict();
    if (classBytes.empty()) {
        return stateFailure(error(ca::StatusCode::INVALID_ARGUMENT,
                                  "validate class bytes",
                                  "candidate class is empty"));
    }
    try {
        auto candidate = cloneWorking();
        const auto* current = ca::core::tag_cast::dyn_cast<const ClassResource>(candidate->getResource(uri));
        if (!current) {
            return stateFailure(error(ca::StatusCode::NOT_FOUND,
                                      "validate class bytes",
                                      "class resource not found: " + uri));
        }
        candidate->replaceResource(uri, std::make_unique<ClassResource>(uri, classBytes));

        MutationResult result;
        result.revision   = revision_;
        result.exportable = canExport();
        result.status     = validateCandidate(*candidate, result.diagnostics);
        if (result.status.is_err()) return result;
        result.byteDiffs          = diffContainers(*working_, *candidate);
        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL,
                                  "validate class bytes",
                                  exception.what()));
    }
}

MutationResult WorkspaceBase::applyClassBytes(const std::string& uri,
                                          const std::vector<ca::u8>& classBytes,
                                          const MutationRequest& request)
{
    if (request.dryRun) return validateClassBytes(uri, classBytes, request);
    auto status = ensureLoaded("apply class bytes");
    if (status.is_err()) return stateFailure(status);
    if (!accepts(request)) return revisionConflict();
    if (classBytes.empty()) {
        return stateFailure(error(ca::StatusCode::INVALID_ARGUMENT,
                                  "apply class bytes",
                                  "candidate class is empty"));
    }
    try {
        auto candidate = cloneWorking();
        const auto* current = ca::core::tag_cast::dyn_cast<const ClassResource>(candidate->getResource(uri));
        if (!current) {
            return stateFailure(error(ca::StatusCode::NOT_FOUND,
                                      "apply class bytes",
                                      "class resource not found: " + uri));
        }
        candidate->replaceResource(uri, std::make_unique<ClassResource>(uri, classBytes));
        std::vector<jasm::Diagnostic> diagnostics;
        status = validateCandidate(*candidate, diagnostics);
        if (status.is_err()) {
            MutationResult result;
            result.status      = std::move(status);
            result.revision    = revision_;
            result.exportable  = canExport();
            result.diagnostics = std::move(diagnostics);
            return result;
        }
        return commit(std::move(candidate), std::move(diagnostics), request);
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL,
                                  "apply class bytes",
                                  exception.what()));
    }
}

MutationResult WorkspaceBase::validateClassMutation(const ClassMutation& mutation,
                                                const MutationRequest& request)
{
    auto status = ensureLoaded("validate class mutation");
    if (status.is_err()) return stateFailure(status);
    if (!accepts(request)) return revisionConflict();
    try {
        auto candidate = cloneWorking();
        ca::str::Utf8StringArena candidateArena;
        GlobalAnalysisManager    candidateManager(candidateArena);
        ClassMutationService     service(*candidate, candidateManager);
        auto                     outcome = service.apply(mutation);

        MutationResult result;
        result.revision    = revision_;
        result.exportable  = canExport();
        result.diagnostics = std::move(outcome.diagnostics);
        if (outcome.status.is_err()) {
            result.status = std::move(outcome.status);
            return result;
        }
        result.status             = ca::OkStatus();
        result.byteDiffs          = diffContainers(*working_, *candidate);
        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL, "validate class mutation", exception.what()));
    }
}

MutationResult WorkspaceBase::applyClassMutation(const ClassMutation& mutation,
                                             const MutationRequest& request)
{
    if (request.dryRun) return validateClassMutation(mutation, request);
    auto status = ensureLoaded("apply class mutation");
    if (status.is_err()) return stateFailure(status);
    if (!accepts(request)) return revisionConflict();
    try {
        auto candidate = cloneWorking();
        ca::str::Utf8StringArena candidateArena;
        GlobalAnalysisManager    candidateManager(candidateArena);
        ClassMutationService     service(*candidate, candidateManager);
        auto                     outcome = service.apply(mutation);
        if (outcome.status.is_err()) {
            MutationResult result;
            result.status      = std::move(outcome.status);
            result.revision    = revision_;
            result.exportable  = canExport();
            result.diagnostics = std::move(outcome.diagnostics);
            return result;
        }
        return commit(std::move(candidate), std::move(outcome.diagnostics), request);
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL, "apply class mutation", exception.what()));
    }
}

MutationResult WorkspaceBase::replaceClassBytes(const std::string& uriOrInternalName,
                                            const std::vector<ca::u8>& bytes,
                                            const MutationRequest& request)
{
    auto status = ensureLoaded("replace class bytes");
    if (status.is_err()) return stateFailure(status);
    if (!accepts(request)) return revisionConflict();
    if (bytes.empty() || bytes.size() > kMaximumClassReplacementSize)
        return stateFailure(error(ca::StatusCode::INVALID_ARGUMENT,
                                  "replace class bytes",
                                  "class size must be in 1..16777216 bytes"));
    if (bytes.size() < 4 || bytes[0] != 0xca || bytes[1] != 0xfe || bytes[2] != 0xba ||
        bytes[3] != 0xbe)
        return stateFailure(error(ca::StatusCode::INVALID_ARGUMENT,
                                  "replace class bytes",
                                  "candidate does not start with CAFEBABE"));

    auto resolved = resolveClassUri(*working_, uriOrInternalName);
    if (resolved.is_err()) return stateFailure(resolved.unwrap_err());
    const auto uri = std::move(resolved).unwrap();
    const auto expectedInternalName = internalNameFromClassUri(uri);
    if (expectedInternalName.empty())
        return stateFailure(error(ca::StatusCode::INVALID_ARGUMENT,
                                  "replace class bytes",
                                  "target URI is not a canonical class or multi-release class URI"));
    const auto* current = ca::core::tag_cast::dyn_cast<const ClassResource>(working_->getResource(uri));
    if (!current)
        return stateFailure(error(ca::StatusCode::NOT_FOUND, "replace class bytes", uri));

    try {
        ca::str::Utf8StringArena     arena;
        jbytecode::tree::ClassNode   parsed;
        jbytecode::core::ClassReader reader(arena, bytes);
        reader.accept(&parsed, 0);
        const auto actualInternalName = str(parsed.name);
        if (actualInternalName != expectedInternalName)
            return stateFailure(error(ca::StatusCode::INVALID_ARGUMENT,
                                      "replace class bytes",
                                      "candidate internal name does not match target URI"));

        auto candidate = cloneWorking();
        candidate->replaceResource(
            uri, std::make_unique<ClassResource>(current->getLocation(), bytes));
        std::vector<jasm::Diagnostic> diagnostics;
        status = validateCandidate(*candidate, diagnostics);
        if (status.is_err()) {
            MutationResult result = stateFailure(std::move(status));
            result.diagnostics = std::move(diagnostics);
            return result;
        }
        if (!request.dryRun)
            return commit(std::move(candidate), std::move(diagnostics), request);

        MutationResult result;
        result.status             = ca::OkStatus();
        result.revision           = revision_;
        result.exportable         = canExport();
        result.diagnostics        = std::move(diagnostics);
        result.byteDiffs          = diffContainers(*working_, *candidate);
        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(
            error(ca::StatusCode::INVALID_ARGUMENT, "replace class bytes", exception.what()));
    }
}

ca::StatusOr<std::vector<ModifiedClassInfo>> WorkspaceBase::modifiedClasses() const
{
    auto status = ensureLoaded("list modified classes");
    if (status.is_err()) return ca::Err(status);
    try {
        std::vector<ModifiedClassInfo> result;
        for (const auto& diff : diffContainers(*original_, *working_)) {
            const auto* original = ca::core::tag_cast::dyn_cast<const ClassResource>(original_->getResource(diff.uri));
            const auto* current  = ca::core::tag_cast::dyn_cast<const ClassResource>(working_->getResource(diff.uri));
            if (!original && !current) continue;

            ModifiedClassInfo item;
            item.resourceUri = diff.uri;
            if (original) {
                item.originalSize        = original->get().size();
                item.originalFingerprint = fingerprintBytes(original->get());
                try {
                    ca::str::Utf8StringArena     arena;
                    jbytecode::core::ClassReader reader(arena, original->get());
                    item.originalInternalName = str(reader.getClassName());
                }
                catch (...) {}
            }
            if (current) {
                item.currentSize        = current->get().size();
                item.currentFingerprint = fingerprintBytes(current->get());
                try {
                    ca::str::Utf8StringArena     arena;
                    jbytecode::core::ClassReader reader(arena, current->get());
                    item.currentInternalName = str(reader.getClassName());
                }
                catch (...) {}
            }
            result.push_back(std::move(item));
        }
        return ca::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::Err(error(ca::StatusCode::INTERNAL,
                             "list modified classes",
                             exception.what()));
    }
}

ca::Status WorkspaceBase::validateCandidate(const ResourceContainer& candidate,
                                        std::vector<jasm::Diagnostic>& diagnostics) const
{
    jasm::JvmJasmService validator;
    ca::Status           status = ca::OkStatus();
    candidate.visitResources([&](const std::string& uri, const IResource& resource) {
        if (status.is_err()) return;
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!classResource) return;
        auto validated = validator.validate(classResource->get());
        diagnostics.insert(diagnostics.end(),
                           std::make_move_iterator(validated.diagnostics.begin()),
                           std::make_move_iterator(validated.diagnostics.end()));
        if (!validated.isOk())
            status = error(ca::StatusCode::INVALID_ARGUMENT,
                           "validate workspace candidate",
                           "class did not pass validation: " + uri);
    });
    return status;
}

ca::Status WorkspaceBase::runPipeline(ResourceContainer& candidate,
                                  const PipelineRequest& request) const
{
    if (!request.pipeline.empty() && !request.passNames.empty()) {
        return error(ca::StatusCode::INVALID_ARGUMENT,
                     "run pipeline",
                     "pipeline and passNames are mutually exclusive");
    }

    registerBuiltinPipelines();
    registerBuiltinPasses();
    PipelineBuilder builder;
    if (!request.pipeline.empty()) {
        if (!PassRegistry::instance().hasPipeline(request.pipeline)) {
            return error(ca::StatusCode::NOT_FOUND, "run pipeline", request.pipeline);
        }
        builder = PassRegistry::instance().getPipeline(request.pipeline);
    }
    else if (!request.passNames.empty()) {
        for (const auto& name : request.passNames) {
            if (name.empty() || !PassNameRegistry::instance().applyTo(name, builder)) {
                return error(ca::StatusCode::NOT_FOUND, "run pass", name);
            }
        }
    }
    else {
        builder = PassRegistry::instance().getPipeline("default-optimize");
    }

    std::unique_ptr<CommonSuperClassProvider> provider;
    ca::i32                                   writerFlags = 0;
    if (!request.jdkPath.empty()) {
        std::error_code filesystemError;
        if (!std::filesystem::is_directory(request.jdkPath, filesystemError) || filesystemError) {
            return error(ca::StatusCode::NOT_FOUND, "run pipeline", "JDK path does not exist");
        }
        provider = std::make_unique<CommonSuperClassProvider>();
        provider->scan(request.jdkPath);
        writerFlags = jbytecode::core::ClassWriter::COMPUTE_MAXS |
                      jbytecode::core::ClassWriter::COMPUTE_FRAMES;
    }

    auto pipeline = builder.buildNestedPipeline(writerFlags, provider.get());
    AsmGlobalContext context;
    runJarPipeline(candidate, pipeline, context, writerFlags, provider.get());
    return ca::OkStatus();
}

ImpactAnalysis WorkspaceBase::analyzeRemap(const MemberRemapPlan& plan, const RemapPolicy& policy) const
{
    ImpactAnalysis result;
    auto loaded = ensureLoaded("analyze remap");
    if (loaded.is_err()) {
        result.status = std::move(loaded);
        return result;
    }
    try {
        auto candidate = cloneWorking();
        ca::str::Utf8StringArena arena;
        applyResourceRemapPlan(*candidate, arena, plan, policy);
        result.status = validateCandidate(*candidate, result.diagnostics);
        if (result.status.is_err()) return result;
        result.byteDiffs = diffContainers(*working_, *candidate);
        const auto beforeClasses = changedClassNames(*working_, result.byteDiffs);
        const auto afterClasses  = changedClassNames(*candidate, result.byteDiffs);
        result.beforeReferences  = collectImpacts(*working_, beforeClasses);
        result.afterReferences   = collectImpacts(*candidate, afterClasses);
        for (const auto& diff : result.byteDiffs) {
            if (diff.uri.find(".class") == std::string::npos)
                result.risks.push_back("resource changed: " + diff.uri);
        }
        result.exportable = true;
        return result;
    }
    catch (const std::exception& exception) {
        result.status = error(ca::StatusCode::INTERNAL, "analyze remap", exception.what());
        return result;
    }
}

MutationResult WorkspaceBase::applyRemap(const MemberRemapPlan& plan, const RemapPolicy& policy,
                                     const MutationRequest& request)
{
    auto loaded = ensureLoaded("apply remap");
    if (loaded.is_err()) return stateFailure(loaded);
    if (!accepts(request)) return revisionConflict();
    if (request.dryRun) {
        auto analysis = analyzeRemap(plan, policy);
        MutationResult result;
        result.status             = std::move(analysis.status);
        result.revision           = revision_;
        result.exportable         = analysis.exportable;
        result.diagnostics        = std::move(analysis.diagnostics);
        result.byteDiffs          = std::move(analysis.byteDiffs);
        result.affectedReferences = std::move(analysis.afterReferences);
        result.affectedClasses    = changedClassNames(*working_, result.byteDiffs);
        return result;
    }
    try {
        auto candidate = cloneWorking();
        ca::str::Utf8StringArena arena;
        applyResourceRemapPlan(*candidate, arena, plan, policy);
        std::vector<jasm::Diagnostic> diagnostics;
        auto status = validateCandidate(*candidate, diagnostics);
        if (status.is_err()) {
            MutationResult result;
            result.status      = std::move(status);
            result.revision    = revision_;
            result.exportable  = canExport();
            result.diagnostics = std::move(diagnostics);
            return result;
        }
        return commit(std::move(candidate), std::move(diagnostics), request);
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL, "apply remap", exception.what()));
    }
}

MutationResult WorkspaceBase::validatePipeline(const PipelineRequest& pipeline,
                                           const MutationRequest& request)
{
    auto loaded = ensureLoaded("validate pipeline");
    if (loaded.is_err()) return stateFailure(loaded);
    if (!accepts(request)) return revisionConflict();
    try {
        auto candidate = cloneWorking();
        auto status = runPipeline(*candidate, pipeline);
        if (status.is_err()) return stateFailure(std::move(status));

        MutationResult result;
        result.revision   = revision_;
        result.exportable = canExport();
        result.status     = validateCandidate(*candidate, result.diagnostics);
        if (result.status.is_err()) return result;
        result.byteDiffs          = diffContainers(*working_, *candidate);
        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL, "validate pipeline", exception.what()));
    }
}

MutationResult WorkspaceBase::applyPipeline(const PipelineRequest& pipeline,
                                        const MutationRequest& request)
{
    if (request.dryRun) return validatePipeline(pipeline, request);
    auto loaded = ensureLoaded("apply pipeline");
    if (loaded.is_err()) return stateFailure(loaded);
    if (!accepts(request)) return revisionConflict();
    try {
        auto candidate = cloneWorking();
        auto status = runPipeline(*candidate, pipeline);
        if (status.is_err()) return stateFailure(std::move(status));

        std::vector<jasm::Diagnostic> diagnostics;
        status = validateCandidate(*candidate, diagnostics);
        if (status.is_err()) {
            MutationResult result;
            result.status      = std::move(status);
            result.revision    = revision_;
            result.exportable  = canExport();
            result.diagnostics = std::move(diagnostics);
            return result;
        }
        return commit(std::move(candidate), std::move(diagnostics), request);
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL, "apply pipeline", exception.what()));
    }
}

WorkflowResult WorkspaceBase::runWorkflow(const WorkflowRequest& workflow,
                                      const MutationRequest& request)
{
    WorkflowResult result;
    const auto loaded = ensureLoaded("run workflow");
    if (loaded.is_err()) {
        result.mutation = stateFailure(loaded);
        return result;
    }
    if (!accepts(request)) {
        result.mutation = revisionConflict();
        return result;
    }

    const auto fail = [&](const std::string& stage, ca::Status status) {
        result.stages.push_back({stage, status});
        result.mutation = stateFailure(std::move(status));
        return std::move(result);
    };

    try {
        auto inputFingerprint = fingerprintContainer(*working_);
        if (inputFingerprint.is_err()) return fail("fingerprint", inputFingerprint.unwrap_err());
        result.inputFingerprint = std::move(inputFingerprint).unwrap();

        auto candidate = cloneWorking();
        ca::str::Utf8StringArena workflowArena;
        if (workflow.remapPlan) {
            applyResourceRemapPlan(*candidate,
                                   workflowArena,
                                   *workflow.remapPlan,
                                   workflow.remapPolicy);
            result.stages.push_back({"mapping", ca::OkStatus()});
        }
        if (workflow.runPipeline) {
            const auto status = runPipeline(*candidate, workflow.pipeline);
            if (status.is_err()) return fail("pipeline", status);
            result.stages.push_back({"pipeline", ca::OkStatus()});
        }

        std::vector<jasm::Diagnostic> diagnostics;
        const auto validation = validateCandidate(*candidate, diagnostics);
        if (validation.is_err()) {
            result.stages.push_back({"validation", validation});
            result.mutation.status      = validation;
            result.mutation.revision    = revision_;
            result.mutation.exportable  = canExport();
            result.mutation.diagnostics = std::move(diagnostics);
            return result;
        }
        result.stages.push_back({"validation", ca::OkStatus()});

        for (const auto& query : workflow.referenceQueries) {
            auto before = workflowReferences(*working_, query);
            if (before.is_err()) return fail("xref", before.unwrap_err());
            auto after = workflowReferences(*candidate, query);
            if (after.is_err()) return fail("xref", after.unwrap_err());
            result.references.push_back(
                {query, std::move(before).unwrap(), std::move(after).unwrap()});
        }
        if (!workflow.referenceQueries.empty()) result.stages.push_back({"xref", ca::OkStatus()});

        auto candidateFingerprint = fingerprintContainer(*candidate);
        if (candidateFingerprint.is_err()) return fail("fingerprint", candidateFingerprint.unwrap_err());
        result.candidateFingerprint = std::move(candidateFingerprint).unwrap();

        if (workflow.candidateJarConsumer) {
            auto artifactPath = createWorkflowCandidateJarPath();
            if (artifactPath.is_err()) return fail("candidate_artifact", artifactPath.unwrap_err());
            ScopedWorkflowCandidateJar artifact(std::move(artifactPath).unwrap());
            const auto exportStatus = exportContainerToJar(*candidate,
                                                           sourcePath_,
                                                           artifact.path().string(),
                                                           revision_);
            if (exportStatus.is_err()) return fail("candidate_artifact", exportStatus);
            const auto stageStatus = workflow.candidateJarConsumer(artifact.path().string());
            if (stageStatus.is_err()) {
                return fail(workflow.candidateJarStageName.empty()
                                ? "candidate_artifact"
                                : workflow.candidateJarStageName,
                            stageStatus);
            }
            result.stages.push_back({workflow.candidateJarStageName.empty()
                                         ? "candidate_artifact"
                                         : workflow.candidateJarStageName,
                                     ca::OkStatus()});
        }

        if (!workflow.exportPath.empty()) {
            auto destination = validateExportDestination(sourcePath_, workflow.exportPath);
            if (destination.is_err()) return fail("export", destination.unwrap_err());
            result.exportPath = destination.unwrap().string();
            if (!request.dryRun) {
                const auto exportStatus = exportContainerToJar(*candidate,
                                                               sourcePath_,
                                                               result.exportPath,
                                                               revision_);
                if (exportStatus.is_err()) return fail("export", exportStatus);
                result.exported = true;
            }
            result.stages.push_back({request.dryRun ? "export_preview" : "export", ca::OkStatus()});
        }

        if (request.dryRun) {
            result.mutation.status             = ca::OkStatus();
            result.mutation.revision           = revision_;
            result.mutation.exportable         = canExport();
            result.mutation.diagnostics        = std::move(diagnostics);
            result.mutation.byteDiffs          = diffContainers(*working_, *candidate);
            result.mutation.affectedClasses    = changedClassNames(*candidate, result.mutation.byteDiffs);
            result.mutation.affectedReferences = collectImpacts(*candidate,
                                                                 result.mutation.affectedClasses);
            return result;
        }
        result.mutation = commit(std::move(candidate), std::move(diagnostics), request);
        if (result.mutation.status.is_err()) {
            removeWorkflowExport(result);
            result.exported = false;
        }
        return result;
    }
    catch (const std::exception& exception) {
        removeWorkflowExport(result);
        result.exported = false;
        return fail("workflow", error(ca::StatusCode::INTERNAL, "run workflow", exception.what()));
    }
}

WorkflowCandidateCommitResult WorkspaceBase::commitWorkflowCandidateJar(
    const WorkflowCandidateCommitRequest& candidateRequest,
    const MutationRequest& request)
{
    WorkflowCandidateCommitResult result;
    const auto loaded = ensureLoaded("commit workflow candidate JAR");
    if (loaded.is_err()) {
        result.mutation = stateFailure(loaded);
        return result;
    }
    if (!accepts(request)) {
        result.mutation = revisionConflict();
        return result;
    }
    if (candidateRequest.candidateJarPath.empty()) {
        result.mutation = stateFailure(error(ca::StatusCode::INVALID_ARGUMENT,
                                             "commit workflow candidate JAR",
                                             "candidate JAR path is empty"));
        return result;
    }

    const auto removeExport = [&] {
        if (!result.exported || result.exportPath.empty()) return;
        std::error_code filesystemError;
        std::filesystem::remove(result.exportPath, filesystemError);
        result.exported = false;
    };

    try {
        std::error_code filesystemError;
        if (!std::filesystem::is_regular_file(candidateRequest.candidateJarPath, filesystemError) ||
            filesystemError) {
            result.mutation = stateFailure(error(ca::StatusCode::NOT_FOUND,
                                                 "commit workflow candidate JAR",
                                                 candidateRequest.candidateJarPath));
            return result;
        }

        auto candidate = std::make_unique<ResourceContainer>();
        ResourceHelper::importFromJar(*candidate, candidateRequest.candidateJarPath);

        std::vector<jasm::Diagnostic> diagnostics;
        const auto validation = validateCandidate(*candidate, diagnostics);
        if (validation.is_err()) {
            result.mutation.status      = validation;
            result.mutation.revision    = revision_;
            result.mutation.exportable  = canExport();
            result.mutation.diagnostics = std::move(diagnostics);
            return result;
        }

        if (!candidateRequest.exportPath.empty()) {
            auto destination = validateExportDestination(sourcePath_, candidateRequest.exportPath);
            if (destination.is_err()) {
                result.mutation = stateFailure(destination.unwrap_err());
                return result;
            }
            result.exportPath = destination.unwrap().string();
            if (!request.dryRun) {
                const auto exportStatus = exportContainerToJar(*candidate,
                                                               sourcePath_,
                                                               result.exportPath,
                                                               revision_);
                if (exportStatus.is_err()) {
                    result.mutation = stateFailure(exportStatus);
                    return result;
                }
                result.exported = true;
            }
        }

        if (request.dryRun) {
            result.mutation.status             = ca::OkStatus();
            result.mutation.revision           = revision_;
            result.mutation.exportable         = canExport();
            result.mutation.diagnostics        = std::move(diagnostics);
            result.mutation.byteDiffs          = diffContainers(*working_, *candidate);
            result.mutation.affectedClasses    = changedClassNames(*candidate, result.mutation.byteDiffs);
            result.mutation.affectedReferences = collectImpacts(*candidate,
                                                                 result.mutation.affectedClasses);
            return result;
        }

        result.mutation = commit(std::move(candidate), std::move(diagnostics), request);
        if (result.mutation.status.is_err()) removeExport();
        return result;
    }
    catch (const std::exception& exception) {
        removeExport();
        result.mutation = stateFailure(error(ca::StatusCode::INTERNAL,
                                             "commit workflow candidate JAR",
                                             exception.what()));
        return result;
    }
}

JarDiffResult WorkspaceBase::diffJar(const std::string& referencePath) const
{
    JarDiffResult result;
    result.referencePath = referencePath;
    const auto loaded = ensureLoaded("diff JAR");
    if (loaded.is_err()) {
        result.status = loaded;
        return result;
    }
    if (referencePath.empty()) {
        result.status = error(ca::StatusCode::INVALID_ARGUMENT, "diff JAR", "reference path is empty");
        return result;
    }

    try {
        ResourceContainer reference;
        ResourceHelper::importFromJar(reference, referencePath);
        result.byteDiffs = diffContainers(*working_, reference);
        for (const auto& diff : result.byteDiffs) {
            const auto* workspaceResource = working_->getResource(diff.uri);
            const auto* referenceResource = reference.getResource(diff.uri);
            if (!workspaceResource) {
                ++result.referenceOnly;
            }
            else if (!referenceResource) {
                ++result.workspaceOnly;
            }
            else {
                ++result.modified;
            }
        }
        result.status = ca::OkStatus();
        return result;
    }
    catch (const std::exception& exception) {
        result.status = error(ca::StatusCode::DATA_LOSS, "diff JAR", exception.what());
        return result;
    }
}

JarRepairResult WorkspaceBase::analyzeJarRepair(const JarRepairRequest& repair,
                                             const MutationRequest& request) const
{
    JarRepairResult result;
    const auto loaded = ensureLoaded("analyze JAR repair");
    if (loaded.is_err()) {
        result.mutation = stateFailure(loaded);
        return result;
    }
    if (!accepts(request)) {
        result.mutation = revisionConflict();
        return result;
    }

    try {
        auto candidate = cloneWorking();
        result.resourceCount = candidate->size();
        result.issues = scanJarRepairIssues(*candidate, repair, result.classCount);
        result.mutation.revision   = revision_;
        result.mutation.exportable = canExport();
        if (!hasSelectedRepairIssues(result.issues)) return result;
        removeSelectedRepairIssues(*candidate, result.issues);

        result.mutation.status = validateCandidate(*candidate, result.mutation.diagnostics);
        if (result.mutation.status.is_err()) return result;
        result.mutation.byteDiffs          = diffContainers(*working_, *candidate);
        result.mutation.affectedClasses    = changedClassNames(*candidate, result.mutation.byteDiffs);
        result.mutation.affectedReferences = collectImpacts(*candidate, result.mutation.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        result.mutation = stateFailure(error(ca::StatusCode::INTERNAL,
                                             "analyze JAR repair",
                                             exception.what()));
        return result;
    }
}

JarRepairResult WorkspaceBase::applyJarRepair(const JarRepairRequest& repair,
                                           const MutationRequest& request)
{
    if (request.dryRun) return analyzeJarRepair(repair, request);
    JarRepairResult result;
    const auto loaded = ensureLoaded("apply JAR repair");
    if (loaded.is_err()) {
        result.mutation = stateFailure(loaded);
        return result;
    }
    if (!accepts(request)) {
        result.mutation = revisionConflict();
        return result;
    }

    try {
        auto candidate = cloneWorking();
        result.resourceCount = candidate->size();
        result.issues = scanJarRepairIssues(*candidate, repair, result.classCount);
        if (!hasSelectedRepairIssues(result.issues)) {
            result.mutation.revision   = revision_;
            result.mutation.exportable = canExport();
            return result;
        }
        removeSelectedRepairIssues(*candidate, result.issues);

        std::vector<jasm::Diagnostic> diagnostics;
        const auto status = validateCandidate(*candidate, diagnostics);
        if (status.is_err()) {
            result.mutation.status      = status;
            result.mutation.revision    = revision_;
            result.mutation.exportable  = canExport();
            result.mutation.diagnostics = std::move(diagnostics);
            return result;
        }
        result.mutation = commit(std::move(candidate), std::move(diagnostics), request);
        return result;
    }
    catch (const std::exception& exception) {
        result.mutation = stateFailure(error(ca::StatusCode::INTERNAL,
                                             "apply JAR repair",
                                             exception.what()));
        return result;
    }
}

ca::Status WorkspaceBase::buildRenamePlan(const RenameRequest& request, MemberRemapPlan& plan,
                                      std::vector<RenameMapping>& mappings,
                                      std::vector<MethodOverrideInfo>& overrides,
                                      std::vector<std::string>& risks) const
{
    const auto ref = [](const std::string& value) {
        return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                                  value.size());
    };
    if (request.owner.empty() || request.newName.empty())
        return error(ca::StatusCode::INVALID_ARGUMENT,
                     "build rename plan",
                     "owner and new name are required");
    switch (request.kind) {
    case RenameKind::Class:
    {
        auto source = classInfo(request.owner);
        if (source.is_err()) return source.unwrap_err();
        auto existing = classInfo(request.newName);
        if (existing.is_ok() && request.newName != request.owner)
            return error(ca::StatusCode::ALREADY_EXISTS, "rename class", request.newName);
        plan.mapClass(ref(request.owner), ref(request.newName));
        mappings.push_back({"class", request.owner, request.newName});
        return ca::OkStatus();
    }
    case RenameKind::Package:
    {
        const auto classes = listClasses();
        if (classes.is_err()) return classes.unwrap_err();
        std::string oldPrefix = request.owner;
        std::string newPrefix = request.newName;
        while (!oldPrefix.empty() && oldPrefix.back() == '/') oldPrefix.pop_back();
        while (!newPrefix.empty() && newPrefix.back() == '/') newPrefix.pop_back();
        for (const auto& clazz : classes.unwrap()) {
            if (clazz.internalName != oldPrefix &&
                clazz.internalName.rfind(oldPrefix + "/", 0) != 0)
                continue;
            const auto suffix = clazz.internalName.substr(oldPrefix.size());
            const auto mapped = newPrefix + suffix;
            plan.mapClass(ref(clazz.internalName), ref(mapped));
            mappings.push_back({"class", clazz.internalName, mapped});
        }
        if (mappings.empty()) return error(ca::StatusCode::NOT_FOUND, "rename package", request.owner);
        return ca::OkStatus();
    }
    case RenameKind::Field:
    {
        if (request.name.empty() || request.descriptor.empty())
            return error(ca::StatusCode::INVALID_ARGUMENT,
                         "rename field",
                         "name and descriptor are required");
        auto field = fieldInfo(request.owner, request.name, request.descriptor);
        if (field.is_err()) return field.unwrap_err();
        plan.mapField({ref(request.owner),
                       ref(request.name),
                       ref(request.descriptor),
                       ref(request.owner),
                       ref(request.newName),
                       ref(request.descriptor)});
        mappings.push_back({"field",
                            request.owner + "." + request.name + request.descriptor,
                            request.owner + "." + request.newName + request.descriptor});
        return ca::OkStatus();
    }
    case RenameKind::Method:
    {
        if (request.name.empty() || request.descriptor.empty())
            return error(ca::StatusCode::INVALID_ARGUMENT,
                         "rename method",
                         "name and descriptor are required");
        auto method = methodInfo(request.owner, request.name, request.descriptor);
        if (method.is_err()) return method.unwrap_err();
        auto component = overridesOfMethod(request.owner, request.name, request.descriptor);
        if (component.is_err()) return component.unwrap_err();
        overrides = std::move(component).unwrap();
        const bool hasSupporting = std::any_of(overrides.begin(), overrides.end(), [](const auto& item) {
            return item.supporting;
        });
        if (overrides.size() > 1 && !request.cascadeOverrides)
            risks.push_back("method belongs to an override component; set cascadeOverrides after reviewing mappings");
        if (hasSupporting)
            risks.push_back("override component contains supporting-library declarations that cannot be renamed in this workspace");
        for (const auto& item : overrides) {
            if (item.supporting) continue;
            plan.mapMethod({ref(item.owner),
                            ref(item.name),
                            ref(item.descriptor),
                            ref(item.owner),
                            ref(request.newName),
                            ref(item.descriptor)});
            mappings.push_back({"method",
                                item.owner + "." + item.name + item.descriptor,
                                item.owner + "." + request.newName + item.descriptor});
        }
        return ca::OkStatus();
    }
    case RenameKind::Local:
        if (request.methodName.empty() || request.name.empty() || request.descriptor.empty())
            return error(ca::StatusCode::INVALID_ARGUMENT,
                         "rename local",
                         "method name, local name and descriptor are required");
        {
            auto method = methodInfo(request.owner, request.methodName, request.descriptor);
            if (method.is_err()) return method.unwrap_err();
        }
        mappings.push_back({"local", request.name, request.newName});
        return ca::OkStatus();
    }
    return error(ca::StatusCode::INVALID_ARGUMENT, "build rename plan", "unknown rename kind");
}

RenameAnalysis WorkspaceBase::analyzeRename(const RenameRequest& request) const
{
    RenameAnalysis result;
    MemberRemapPlan plan;
    result.status = buildRenamePlan(request,
                                    plan,
                                    result.mappings,
                                    result.overrideComponent,
                                    result.impact.risks);
    if (result.status.is_err()) {
        result.impact.status = result.status;
        return result;
    }
    if (request.kind == RenameKind::Local) {
        result.impact.status = ca::OkStatus();
        result.impact.exportable = canExport();
        result.readyToApply = true;
        return result;
    }
    auto renameRisks = std::move(result.impact.risks);
    result.impact = analyzeRemap(plan, request.policy);
    result.impact.risks.insert(result.impact.risks.end(),
                               std::make_move_iterator(renameRisks.begin()),
                               std::make_move_iterator(renameRisks.end()));
    result.status = result.impact.status;
    result.requiresOverrideReview = !result.impact.risks.empty();
    result.readyToApply = result.status.is_ok() && !result.requiresOverrideReview;
    return result;
}

MutationResult WorkspaceBase::applyRename(const RenameRequest& request,
                                      const MutationRequest& requestOptions)
{
    auto preview = analyzeRename(request);
    if (!preview.status.is_ok()) return stateFailure(preview.status);
    if (!preview.readyToApply) {
        return stateFailure(error(ca::StatusCode::FAILED_PRECONDITION,
                                  "apply rename",
                                  "rename requires override review: " +
                                      (preview.impact.risks.empty() ? std::string("unknown risk")
                                                                    : preview.impact.risks.front())));
    }
    if (request.kind == RenameKind::Local) {
        ClassMutation mutation;
        mutation.classUri             = request.owner;
        mutation.kind                 = ClassMutationKind::RenameLocal;
        mutation.member               = {request.methodName, request.descriptor};
        mutation.localName            = request.name;
        mutation.replacementLocalName = request.newName;
        mutation.localIndex           = request.localIndex;
        return applyClassMutation(mutation, requestOptions);
    }
    MemberRemapPlan plan;
    std::vector<RenameMapping>      mappings;
    std::vector<MethodOverrideInfo> overrides;
    std::vector<std::string>        risks;
    auto status = buildRenamePlan(request, plan, mappings, overrides, risks);
    if (status.is_err()) return stateFailure(std::move(status));
    return applyRemap(plan, request.policy, requestOptions);
}

MutationResult WorkspaceBase::revertClass(const std::string& uri, const MutationRequest& request)
{
    auto status = ensureLoaded("revert class");
    if (status.is_err()) return stateFailure(status);
    if (!accepts(request)) return revisionConflict();
    const auto* original = ca::core::tag_cast::dyn_cast<const ClassResource>(original_->getResource(uri));
    const auto* current  = ca::core::tag_cast::dyn_cast<const ClassResource>(working_->getResource(uri));
    if (!original || !current) {
        return stateFailure(error(ca::StatusCode::NOT_FOUND, "revert class", uri));
    }
    try {
        auto candidate = cloneWorking();
        candidate->replaceResource(uri,
                               std::make_unique<ClassResource>(original->getLocation(), original->get()));
        std::vector<jasm::Diagnostic> diagnostics;
        status = validateCandidate(*candidate, diagnostics);
        if (status.is_err()) {
            MutationResult result = stateFailure(std::move(status));
            result.diagnostics = std::move(diagnostics);
            return result;
        }
        if (!request.dryRun)
            return commit(std::move(candidate), std::move(diagnostics), request);

        MutationResult result;
        result.status             = ca::OkStatus();
        result.revision           = revision_;
        result.exportable         = canExport();
        result.diagnostics        = std::move(diagnostics);
        result.byteDiffs          = diffContainers(*working_, *candidate);
        result.affectedClasses    = changedClassNames(*candidate, result.byteDiffs);
        result.affectedReferences = collectImpacts(*candidate, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL, "revert class", exception.what()));
    }
}

ca::Status WorkspaceBase::createSavepoint(const std::string& name)
{
    auto status = ensureLoaded("create savepoint");
    if (status.is_err()) return status;
    if (name.empty()) return error(ca::StatusCode::INVALID_ARGUMENT, "create savepoint", "name is empty");
    try {
        savepoints_[name] = cloneWorking();
        return ca::OkStatus();
    }
    catch (const std::exception& exception) {
        return error(ca::StatusCode::INTERNAL, "create savepoint", exception.what());
    }
}

MutationResult WorkspaceBase::restoreSavepoint(const std::string& name, const MutationRequest& request)
{
    auto status = ensureLoaded("restore savepoint");
    if (status.is_err()) return stateFailure(status);
    if (!accepts(request)) return revisionConflict();
    auto it = savepoints_.find(name);
    if (it == savepoints_.end())
        return stateFailure(error(ca::StatusCode::NOT_FOUND, "restore savepoint", name));
    try {
        auto candidate = cloneContainer(*it->second);
        if (candidate.is_err()) return stateFailure(candidate.unwrap_err());
        return commit(std::move(candidate).unwrap(), {}, request);
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL, "restore savepoint", exception.what()));
    }
}

MutationResult WorkspaceBase::undo(const MutationRequest& request)
{
    auto status = ensureLoaded("undo");
    if (status.is_err()) return stateFailure(status);
    if (!accepts(request)) return revisionConflict();
    if (undo_.empty()) return stateFailure(error(ca::StatusCode::FAILED_PRECONDITION, "undo", "history is empty"));
    try {
        auto before = cloneWorking();
        auto target = std::move(undo_.back());
        undo_.pop_back();
        auto diffs = diffContainers(*before, *target.container);
        redo_.push_back({std::move(before), target.label});
        working_ = std::move(target.container);
        analysisManager_->clearAll();
        ++revision_;

        MutationResult result;
        result.status             = ca::OkStatus();
        result.revision           = revision_;
        result.committed          = true;
        result.exportable         = true;
        result.byteDiffs          = std::move(diffs);
        result.affectedClasses    = changedClassNames(*working_, result.byteDiffs);
        result.affectedReferences = collectImpacts(*working_, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL, "undo", exception.what()));
    }
}

MutationResult WorkspaceBase::redo(const MutationRequest& request)
{
    auto status = ensureLoaded("redo");
    if (status.is_err()) return stateFailure(status);
    if (!accepts(request)) return revisionConflict();
    if (redo_.empty()) return stateFailure(error(ca::StatusCode::FAILED_PRECONDITION, "redo", "history is empty"));
    try {
        auto before = cloneWorking();
        auto target = std::move(redo_.back());
        redo_.pop_back();
        auto diffs = diffContainers(*before, *target.container);
        undo_.push_back({std::move(before), target.label});
        working_ = std::move(target.container);
        analysisManager_->clearAll();
        ++revision_;

        MutationResult result;
        result.status             = ca::OkStatus();
        result.revision           = revision_;
        result.committed          = true;
        result.exportable         = true;
        result.byteDiffs          = std::move(diffs);
        result.affectedClasses    = changedClassNames(*working_, result.byteDiffs);
        result.affectedReferences = collectImpacts(*working_, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(ca::StatusCode::INTERNAL, "redo", exception.what()));
    }
}

ca::Status WorkspaceBase::exportJar(const std::string& outputPath) const
{
    auto status = ensureLoaded("export JAR");
    if (status.is_err()) return status;
    if (artifactKind_ != ArtifactKind::Jar) {
        return error(ca::StatusCode::FAILED_PRECONDITION,
                     "export JAR",
                     "workspace artifact is not a JAR");
    }
    return exportContainerToJar(*working_, sourcePath_, outputPath, revision_);
}

ca::Status WorkspaceBase::exportClass(const std::string& outputPath) const
{
    auto status = ensureLoaded("export class");
    if (status.is_err()) return status;
    if (artifactKind_ != ArtifactKind::Class || primaryClassUri_.empty()) {
        return error(ca::StatusCode::FAILED_PRECONDITION,
                     "export class",
                     "workspace artifact is not a class");
    }
    return exportContainerToClass(*working_,
                                  primaryClassUri_,
                                  sourcePath_,
                                  outputPath,
                                  revision_);
}

std::vector<SupportingResourceInfo> WorkspaceBase::supportingResources() const
{
    std::vector<SupportingResourceInfo> result;
    for (const auto& support : supporting_) {
        SupportingResourceInfo info;
        info.id            = support.id;
        info.path          = support.path;
        info.fingerprint   = support.fingerprint;
        info.resourceCount = support.container->size();
        support.container->visitResources([&](const std::string&, const IResource& resource) {
            if (resource.getType() == ResourceType::Class) ++info.classCount;
        });
        result.push_back(std::move(info));
    }
    return result;
}

}   //namespace mjt::base::workspace
