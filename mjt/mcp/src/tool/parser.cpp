#include "parser.hpp"

#include <limits>
#include <unordered_set>
#include <utility>

#include "limits.hpp"

namespace mjt::mcp::detail {

using mjbase::workspace::ClassMutation;
using mjbase::workspace::ClassMutationKind;
using mjbase::workspace::ConstantKind;
using mjbase::workspace::ConstantSpec;
using mjbase::workspace::HandleSpec;
using mjbase::workspace::InstructionKind;
using mjbase::workspace::JarRepairRequest;
using mjbase::workspace::MutationRequest;
using mjbase::workspace::PipelineRequest;
using mjbase::workspace::RenameKind;
using mjbase::workspace::RenameRequest;
using mjbase::workspace::SearchMatchMode;
using mjbase::workspace::SearchOptions;
using mjbase::workspace::WorkflowReferenceKind;
using mjbase::workspace::WorkflowReferenceQuery;

bool parseSearchOptions(const Json& arguments, SearchOptions& output, std::string& error)
{
    const auto mode = arguments.value("match_mode", std::string{"contains"});
    //prefix 档：mjbase 只有 contains/exact/regex，先按
    //contains 取超集，mjt 侧再按命中行判别文本收紧（query.cpp filterPrefixHits）。
    if (mode == "contains" || mode == "prefix") output.matchMode = SearchMatchMode::Contains;
    else if (mode == "exact") output.matchMode = SearchMatchMode::Exact;
    else if (mode == "regex") output.matchMode = SearchMatchMode::Regex;
    else {
        error = "match_mode must be contains, exact, prefix or regex";
        return false;
    }
    output.caseSensitive = arguments.value("case_sensitive", false);
    output.owner = arguments.value("owner", std::string{});
    output.packagePrefix = arguments.value("package_prefix", std::string{});
    output.memberKind = arguments.value("member_kind", std::string{});
    output.opcode = arguments.value("opcode", -1);
    return true;
}


MutationRequest mutationRequest(const Json& arguments)
{
    MutationRequest request;
    if (arguments.contains("expected_revision")) {
        //schema 层（contract.cpp mutationOptions）已带 minimum: 0；此处为运行时最后
        //防线：负数经 get<ca::u64> 会静默回绕成巨大无符号值，浮点/字符串不能按
        //revision 解释，必须显式拒绝。接受 unsigned 或非负 signed（C++ 内部构造的
        //非负 int 是 signed，不能误伤）。抛 Json::exception 由 service.call 统一
        //映射为 INVALID_ARGUMENT。
        const auto& revision = arguments.at("expected_revision");
        const bool nonNegativeInteger =
            revision.is_number_unsigned() ||
            (revision.is_number_integer() && revision.get<ca::i64>() >= 0);
        if (!nonNegativeInteger) {
            throw Json::type_error::create(
                302, "expected_revision must be a non-negative integer", nullptr);
        }
        request.expectedRevision = revision.get<ca::u64>();
    }
    request.dryRun = arguments.value("dry_run", false);
    request.label = arguments.value("label", std::string{});
    //validation 三档：relaxed（默认，JVM 赋值协变 + 只校验受影响类）/
    //strict（精确匹配）/ off（跳过门禁）。非法值走与 expected_revision 相同的
    //Json type_error 通道，由 service.call 统一映射 INVALID_ARGUMENT。
    if (arguments.contains("validation")) {
        const auto& mode = arguments.at("validation");
        const auto& value = mode.is_string() ? mode.get_ref<const std::string&>() : std::string{};
        if (value == "relaxed")
            request.validationMode = mjbase::jasm::ValidationMode::Relaxed;
        else if (value == "strict")
            request.validationMode = mjbase::jasm::ValidationMode::Strict;
        else if (value == "off")
            request.validationMode = mjbase::jasm::ValidationMode::Off;
        else {
            throw Json::type_error::create(
                302, "validation must be one of relaxed, strict, off", nullptr);
        }
    }
    return request;
}

bool parseHandle(const Json& value, HandleSpec& output, std::string& error)
{
    if (!value.is_object()) {
        error = "handle must be an object";
        return false;
    }
    output.tag                = value.value("tag", 0);
    output.owner              = value.value("owner", std::string{});
    output.name               = value.value("name", std::string{});
    output.descriptor         = value.value("descriptor", std::string{});
    output.interfaceReference = value.value("interface", false);
    return true;
}

bool parseConstant(const Json& value, ConstantSpec& output, std::string& error,
                   ca::usize depth = 0)
{
    if (!value.is_object()) {
        error = "constant must be an object";
        return false;
    }
    if (depth > kMaximumConstantDynamicDepth) {
        error = "constant dynamic nesting exceeds 8";
        return false;
    }
    const auto kind = value.value("kind", std::string{});
    if (kind == "string") output.kind = ConstantKind::String;
    else if (kind == "int") output.kind = ConstantKind::Integer;
    else if (kind == "long") output.kind = ConstantKind::Long;
    else if (kind == "float") output.kind = ConstantKind::Float;
    else if (kind == "double") output.kind = ConstantKind::Double;
    else if (kind == "type") output.kind = ConstantKind::Type;
    else if (kind == "handle") output.kind = ConstantKind::Handle;
    else if (kind == "dynamic") output.kind = ConstantKind::Dynamic;
    else {
        error = "unsupported constant kind: " + kind;
        return false;
    }
    output.stringValue = value.value("string_value", std::string{});
    output.intValue    = value.value("int_value", 0);
    output.longValue   = value.value("long_value", static_cast<ca::i64>(0));
    output.floatValue  = value.value("float_value", 0.0F);
    output.doubleValue = value.value("double_value", 0.0);
    output.descriptor  = value.value("descriptor", std::string{});
    output.name        = value.value("name", std::string{});
    if (value.contains("handle") && !parseHandle(value.at("handle"), output.handle, error))
        return false;
    if (value.contains("bootstrap_method") &&
        !parseHandle(value.at("bootstrap_method"), output.bootstrapMethod, error))
        return false;
    if (value.contains("bootstrap_arguments")) {
        if (!value.at("bootstrap_arguments").is_array()) {
            error = "bootstrap_arguments must be an array";
            return false;
        }
        for (const auto& argument : value.at("bootstrap_arguments")) {
            ConstantSpec parsed;
            if (!parseConstant(argument, parsed, error, depth + 1)) return false;
            output.bootstrapArguments.push_back(std::move(parsed));
        }
    }
    return true;
}

bool parseInstruction(const Json& value, mjbase::workspace::InstructionSpec& output,
                      std::string& error)
{
    if (!value.is_object()) {
        error = "instruction must be an object";
        return false;
    }
    const auto kind = value.value("kind", std::string("simple"));
    if (kind == "simple") output.kind = InstructionKind::Simple;
    else if (kind == "int") output.kind = InstructionKind::Int;
    else if (kind == "var") output.kind = InstructionKind::Var;
    else if (kind == "type") output.kind = InstructionKind::Type;
    else if (kind == "field") output.kind = InstructionKind::Field;
    else if (kind == "method") output.kind = InstructionKind::Method;
    else if (kind == "jump") output.kind = InstructionKind::Jump;
    else if (kind == "label") output.kind = InstructionKind::Label;
    else if (kind == "ldc_string") output.kind = InstructionKind::LdcString;
    else if (kind == "ldc_int") output.kind = InstructionKind::LdcInt;
    else if (kind == "ldc_long") output.kind = InstructionKind::LdcLong;
    else if (kind == "ldc_float") output.kind = InstructionKind::LdcFloat;
    else if (kind == "ldc_double") output.kind = InstructionKind::LdcDouble;
    else if (kind == "ldc_type") output.kind = InstructionKind::LdcType;
    else if (kind == "ldc_handle") output.kind = InstructionKind::LdcHandle;
    else if (kind == "ldc_dynamic") output.kind = InstructionKind::LdcDynamic;
    else if (kind == "invokedynamic") output.kind = InstructionKind::InvokeDynamic;
    else if (kind == "iinc") output.kind = InstructionKind::Iinc;
    else if (kind == "tableswitch") output.kind = InstructionKind::TableSwitch;
    else if (kind == "lookupswitch") output.kind = InstructionKind::LookupSwitch;
    else if (kind == "multianewarray") output.kind = InstructionKind::MultiANewArray;
    else if (kind == "line") output.kind = InstructionKind::Line;
    else {
        error = "unsupported instruction kind: " + kind;
        return false;
    }
    output.opcode = value.value("opcode", -1);
    output.operand = value.value("operand", 0);
    output.increment = value.value("increment", 0);
    output.interfaceInvocation = value.value("interface", false);
    output.owner = value.value("owner", std::string{});
    output.name = value.value("name", std::string{});
    output.descriptor = value.value("descriptor", std::string{});
    output.stringValue = value.value("string_value", std::string{});
    output.longValue = value.value("long_value", static_cast<ca::i64>(0));
    output.floatValue = value.value("float_value", 0.0F);
    output.doubleValue = value.value("double_value", 0.0);
    output.label = value.value("label", std::string{});
    output.defaultLabel = value.value("default_label", std::string{});
    output.minimum = value.value("minimum", 0);
    output.maximum = value.value("maximum", -1);
    output.dimensions = value.value("dimensions", 0);
    output.line = value.value("line", 0);
    if (value.contains("labels")) {
        if (!value.at("labels").is_array()) {
            error = "instruction labels must be an array";
            return false;
        }
        output.labels = value.at("labels").get<std::vector<std::string>>();
    }
    if (value.contains("keys")) {
        if (!value.at("keys").is_array()) {
            error = "instruction keys must be an array";
            return false;
        }
        output.keys = value.at("keys").get<std::vector<ca::i32>>();
    }
    if (value.contains("handle") && !parseHandle(value.at("handle"), output.handle, error))
        return false;
    if (value.contains("bootstrap_method") &&
        !parseHandle(value.at("bootstrap_method"), output.bootstrapMethod, error))
        return false;
    if (value.contains("bootstrap_arguments")) {
        if (!value.at("bootstrap_arguments").is_array()) {
            error = "bootstrap_arguments must be an array";
            return false;
        }
        for (const auto& argument : value.at("bootstrap_arguments")) {
            ConstantSpec parsed;
            if (!parseConstant(argument, parsed, error)) return false;
            output.bootstrapArguments.push_back(std::move(parsed));
        }
    }
    return true;
}

bool parseAccessFlags(const Json& values, mjbase::workspace::AnnotationTarget target,
                      ca::i32& output, std::string& error)
{
    //JVM access 位（JVMS 表 4.1/4.4/4.5/4.6/4.7）：mbase Opcodes::ACC_* 的
    //mjbase 对应物是 workspace_types 的 kAcc* 常量集；此处按旧名起别名，保
    //调用点零漂移。
    constexpr ca::u32 ACC_PUBLIC       = mjbase::workspace::kAccPublic;
    constexpr ca::u32 ACC_PRIVATE      = mjbase::workspace::kAccPrivate;
    constexpr ca::u32 ACC_PROTECTED    = mjbase::workspace::kAccProtected;
    constexpr ca::u32 ACC_STATIC       = mjbase::workspace::kAccStatic;
    constexpr ca::u32 ACC_FINAL        = mjbase::workspace::kAccFinal;
    constexpr ca::u32 ACC_SUPER        = mjbase::workspace::kAccSuper;
    constexpr ca::u32 ACC_SYNCHRONIZED = mjbase::workspace::kAccSynchronized;
    constexpr ca::u32 ACC_VOLATILE     = mjbase::workspace::kAccVolatile;
    constexpr ca::u32 ACC_BRIDGE       = mjbase::workspace::kAccBridge;
    constexpr ca::u32 ACC_TRANSIENT    = mjbase::workspace::kAccTransient;
    constexpr ca::u32 ACC_VARARGS      = mjbase::workspace::kAccVarargs;
    constexpr ca::u32 ACC_NATIVE       = mjbase::workspace::kAccNative;
    constexpr ca::u32 ACC_INTERFACE    = mjbase::workspace::kAccInterface;
    constexpr ca::u32 ACC_ABSTRACT     = mjbase::workspace::kAccAbstract;
    constexpr ca::u32 ACC_STRICT       = mjbase::workspace::kAccStrict;
    constexpr ca::u32 ACC_SYNTHETIC    = mjbase::workspace::kAccSynthetic;
    constexpr ca::u32 ACC_ANNOTATION   = mjbase::workspace::kAccAnnotation;
    constexpr ca::u32 ACC_ENUM         = mjbase::workspace::kAccEnum;
    constexpr ca::u32 ACC_MODULE       = mjbase::workspace::kAccModule;
    constexpr ca::u32 ACC_RECORD       = mjbase::workspace::kAccRecord;
    constexpr ca::u32 ACC_SEALED       = mjbase::workspace::kAccSealed;
    constexpr ca::u32 ACC_DEPRECATED   = mjbase::workspace::kAccDeprecated;
    if (!values.is_array()) {
        error = "access_flags must be an array";
        return false;
    }
    std::unordered_set<std::string> seen;
    for (const auto& value : values) {
        if (!value.is_string()) {
            error = "each access_flags item must be a string";
            return false;
        }
        const auto name = value.get<std::string>();
        if (!seen.insert(name).second) {
            error = "duplicate access flag: " + name;
            return false;
        }
        ca::i32 flag = 0;
        if (name == "public") flag = ACC_PUBLIC;
        else if (name == "private") flag = ACC_PRIVATE;
        else if (name == "protected") flag = ACC_PROTECTED;
        else if (name == "static") flag = ACC_STATIC;
        else if (name == "final") flag = ACC_FINAL;
        else if (name == "synthetic") flag = ACC_SYNTHETIC;
        else if (name == "deprecated") flag = ACC_DEPRECATED;
        else if (target == mjbase::workspace::AnnotationTarget::Class && name == "super")
            flag = ACC_SUPER;
        else if (target == mjbase::workspace::AnnotationTarget::Class && name == "interface")
            flag = ACC_INTERFACE;
        else if (target == mjbase::workspace::AnnotationTarget::Class && name == "abstract")
            flag = ACC_ABSTRACT;
        else if (target == mjbase::workspace::AnnotationTarget::Class && name == "annotation")
            flag = ACC_ANNOTATION;
        else if (target == mjbase::workspace::AnnotationTarget::Class && name == "enum")
            flag = ACC_ENUM;
        else if (target == mjbase::workspace::AnnotationTarget::Class && name == "module")
            flag = ACC_MODULE;
        else if (target == mjbase::workspace::AnnotationTarget::Class && name == "record")
            flag = ACC_RECORD;
        else if (target == mjbase::workspace::AnnotationTarget::Class && name == "sealed")
            flag = ACC_SEALED;
        else if (target == mjbase::workspace::AnnotationTarget::Field && name == "volatile")
            flag = ACC_VOLATILE;
        else if (target == mjbase::workspace::AnnotationTarget::Field && name == "transient")
            flag = ACC_TRANSIENT;
        else if (target == mjbase::workspace::AnnotationTarget::Field && name == "enum")
            flag = ACC_ENUM;
        else if (target == mjbase::workspace::AnnotationTarget::Method && name == "synchronized")
            flag = ACC_SYNCHRONIZED;
        else if (target == mjbase::workspace::AnnotationTarget::Method && name == "bridge")
            flag = ACC_BRIDGE;
        else if (target == mjbase::workspace::AnnotationTarget::Method && name == "varargs")
            flag = ACC_VARARGS;
        else if (target == mjbase::workspace::AnnotationTarget::Method && name == "native")
            flag = ACC_NATIVE;
        else if (target == mjbase::workspace::AnnotationTarget::Method && name == "abstract")
            flag = ACC_ABSTRACT;
        else if (target == mjbase::workspace::AnnotationTarget::Method && name == "strict")
            flag = ACC_STRICT;
        else {
            error = "access flag is not valid for this target: " + name;
            return false;
        }
        output |= flag;
    }
    return true;
}

bool parseMutation(const Json& arguments, ClassMutation& mutation, std::string& error)
{
    mutation.classUri = arguments.value("class_uri", std::string{});
    const auto kind = arguments.value("kind", std::string{});
    //set_class_access → change_class_access 严格切换：旧整体替换语义
    //会丢 ACC_SUPER 且静默吞掉不可表达的 flag，工具面不再接受旧词。
    if (kind == "set_class_access") {
        error = "mutation kind set_class_access was replaced by change_class_access "
                "(incremental set/clear flag lists; unmentioned bits are preserved)";
        return false;
    }
    static const std::array<std::pair<const char*, ClassMutationKind>, 15> kinds = {{{"change_class_access", ClassMutationKind::ChangeClassAccess},
                                                                                      {"set_field_access", ClassMutationKind::SetFieldAccess},
                                                                                      {"set_method_access", ClassMutationKind::SetMethodAccess},
                                                                                      {"add_field", ClassMutationKind::AddField},
                                                                                      {"remove_field", ClassMutationKind::RemoveField},
                                                                                      {"add_method", ClassMutationKind::AddMethod},
                                                                                      {"remove_method", ClassMutationKind::RemoveMethod},
                                                                                      {"add_marker_annotation", ClassMutationKind::AddMarkerAnnotation},
                                                                                      {"remove_annotation", ClassMutationKind::RemoveAnnotation},
                                                                                      {"rename_local", ClassMutationKind::RenameLocal},
                                                                                      {"replace_try_catch_blocks", ClassMutationKind::ReplaceTryCatchBlocks},
                                                                                      {"replace_method_body", ClassMutationKind::ReplaceMethodBody},
                                                                                      {"insert_instruction", ClassMutationKind::InsertInstruction},
                                                                                      {"replace_instruction", ClassMutationKind::ReplaceInstruction},
                                                                                      {"remove_instruction", ClassMutationKind::RemoveInstruction}}};
    const auto it = std::find_if(kinds.begin(), kinds.end(), [&](const auto& item) { return kind == item.first; });
    if (it == kinds.end()) {
        error = "unsupported mutation kind: " + kind;
        return false;
    }
    mutation.kind = it->second;
    //change_class_access：payload 是 set/clear flag 名单（JVM 小写助记，
    //"deprecated" 由引擎映射为 Deprecated 属性而非访问位）；位掩码 access/
    //access_flags 对本 kind 不适用，显式拒绝避免静默忽略。
    if (mutation.kind == ClassMutationKind::ChangeClassAccess) {
        if (arguments.contains("access") || arguments.contains("access_flags")) {
            error = "change_class_access takes set/clear flag lists; "
                    "access/access_flags are not valid for it";
            return false;
        }
        const auto parseFlagList = [&](const char* key, std::vector<std::string>& out) {
            if (!arguments.contains(key)) return true;
            if (!arguments.at(key).is_array()) {
                error = std::string(key) + " must be an array of flag names";
                return false;
            }
            for (const auto& flag : arguments.at(key)) {
                if (!flag.is_string() || flag.get_ref<const std::string&>().empty()) {
                    error = std::string(key) + " items must be non-empty flag names";
                    return false;
                }
                out.push_back(flag.get_ref<const std::string&>());
            }
            return true;
        };
        if (!parseFlagList("set", mutation.setFlags) || !parseFlagList("clear", mutation.clearFlags))
            return false;
    }
    mutation.member = {arguments.value("member_name", std::string{}),
                       arguments.value("member_descriptor", std::string{})};
    if (arguments.contains("access") && arguments.contains("access_flags")) {
        error = "access and access_flags are mutually exclusive";
        return false;
    }
    mutation.access = arguments.value("access", 0);
    if (arguments.contains("access_flags")) {
        mjbase::workspace::AnnotationTarget accessTarget;
        if (mutation.kind == ClassMutationKind::SetFieldAccess ||
                 mutation.kind == ClassMutationKind::AddField)
            accessTarget = mjbase::workspace::AnnotationTarget::Field;
        else if (mutation.kind == ClassMutationKind::SetMethodAccess ||
                 mutation.kind == ClassMutationKind::AddMethod)
            accessTarget = mjbase::workspace::AnnotationTarget::Method;
        else {
            error = "access_flags is only valid for access setters and member additions";
            return false;
        }
        if (!parseAccessFlags(arguments.at("access_flags"), accessTarget, mutation.access, error))
            return false;
    }
    mutation.instructionIndex = arguments.value("instruction_index", -1);
    mutation.insertAfter = arguments.value("insert_after", false);
    mutation.localIndex = arguments.value("local_index", -1);
    mutation.localName = arguments.value("local_name", std::string{});
    mutation.replacementLocalName = arguments.value("replacement_local_name", std::string{});
    mutation.annotationDescriptor = arguments.value("annotation_descriptor", std::string{});
    mutation.annotationVisible = arguments.value("annotation_visible", true);
    const auto annotationTarget = arguments.value("annotation_target", std::string("class"));
    if (annotationTarget == "class") mutation.annotationTarget = mjbase::workspace::AnnotationTarget::Class;
    else if (annotationTarget == "field") mutation.annotationTarget = mjbase::workspace::AnnotationTarget::Field;
    else if (annotationTarget == "method") mutation.annotationTarget = mjbase::workspace::AnnotationTarget::Method;
    else {
        error = "unsupported annotation target: " + annotationTarget;
        return false;
    }
    mutation.signature = arguments.value("signature", std::string{});
    if (arguments.contains("exceptions")) {
        if (!arguments.at("exceptions").is_array()) {
            error = "exceptions must be an array";
            return false;
        }
        mutation.exceptions = arguments.at("exceptions").get<std::vector<std::string>>();
    }
    if (arguments.contains("field_value")) {
        mutation.hasFieldValue = true;
        if (!parseConstant(arguments.at("field_value"), mutation.fieldValue, error)) return false;
    }
    if (arguments.contains("existing_labels")) {
        if (!arguments.at("existing_labels").is_array()) {
            error = "existing_labels must be an array";
            return false;
        }
        for (const auto& value : arguments.at("existing_labels")) {
            if (!value.is_object()) {
                error = "each existing_labels item must be an object";
                return false;
            }
            mutation.existingLabels.push_back(
                {value.value("id", std::string{}), value.value("instruction_index", -1)});
        }
    }
    if (arguments.contains("instruction") && !parseInstruction(arguments.at("instruction"), mutation.instruction, error))
        return false;
    if (arguments.contains("initial_instructions")) {
        //与兄弟分支（try_catch_blocks 等）一致的类型防御；schema 层通常已拦截。
        if (!arguments.at("initial_instructions").is_array()) {
            error = "initial_instructions must be an array";
            return false;
        }
        for (const auto& value : arguments.at("initial_instructions")) {
            mjbase::workspace::InstructionSpec spec;
            if (!parseInstruction(value, spec, error)) return false;
            mutation.initialInstructions.push_back(std::move(spec));
        }
    }
    if (arguments.contains("try_catch_blocks")) {
        if (!arguments.at("try_catch_blocks").is_array()) {
            error = "try_catch_blocks must be an array";
            return false;
        }
        for (const auto& value : arguments.at("try_catch_blocks")) {
            if (!value.is_object()) {
                error = "each try_catch_blocks item must be an object";
                return false;
            }
            mutation.tryCatchBlocks.push_back(
                {value.value("start_instruction", -1),
                 value.value("end_instruction", -1),
                 value.value("handler_instruction", -1),
                 value.value("type", std::string{}),
                 value.value("start_label", std::string{}),
                 value.value("end_label", std::string{}),
                 value.value("handler_label", std::string{})});
        }
    }
    if (arguments.contains("local_variables")) {
        if (!arguments.at("local_variables").is_array()) {
            error = "local_variables must be an array";
            return false;
        }
        for (const auto& value : arguments.at("local_variables")) {
            if (!value.is_object()) {
                error = "each local_variables item must be an object";
                return false;
            }
            mutation.localVariables.push_back({value.value("name", std::string{}),
                                               value.value("descriptor", std::string{}),
                                               value.value("signature", std::string{}),
                                               value.value("start_label", std::string{}),
                                               value.value("end_label", std::string{}),
                                               value.value("index", -1)});
        }
    }
    return true;
}

bool parseRename(const Json& arguments, RenameRequest& output, std::string& error)
{
    const auto kind = arguments.value("kind", std::string{});
    if (kind == "class") output.kind = RenameKind::Class;
    else if (kind == "package") output.kind = RenameKind::Package;
    else if (kind == "field") output.kind = RenameKind::Field;
    else if (kind == "method") output.kind = RenameKind::Method;
    else if (kind == "local") output.kind = RenameKind::Local;
    else {
        error = "unsupported rename kind: " + kind;
        return false;
    }
    output.owner = arguments.value("owner", std::string{});
    output.methodName = arguments.value("method_name", std::string{});
    output.name = arguments.value("name", std::string{});
    output.descriptor = arguments.value("descriptor", std::string{});
    output.newName = arguments.value("new_name", std::string{});
    output.localIndex = arguments.value("local_index", -1);
    output.cascadeOverrides = arguments.value("cascade_overrides", false);
    const auto policy = arguments.value("policy", std::string("all"));
    if (policy == "bytecode") output.policy = mjbase::asm_remapper::RemapPolicy::bytecodeOnly();
    else if (policy == "reflection") output.policy = mjbase::asm_remapper::RemapPolicy::bytecodeAndReflectionStrings();
    else if (policy == "metadata") output.policy = mjbase::asm_remapper::RemapPolicy::bytecodeAndMetadata();
    else if (policy == "all") output.policy = mjbase::asm_remapper::RemapPolicy::all();
    else {
        error = "unsupported remap policy: " + policy;
        return false;
    }
    return true;
}

bool parsePipelineRequest(const Json& arguments, PipelineRequest& output, std::string& error)
{
    output.pipeline = arguments.value("pipeline", std::string{});
    output.jdkPath  = arguments.value("jdk_path", std::string{});
    if (arguments.contains("passes")) {
        const auto& passes = arguments.at("passes");
        if (!passes.is_array()) {
            error = "passes must be an array of registered pass names";
            return false;
        }
        for (const auto& value : passes) {
            if (!value.is_string()) {
                error = "each passes item must be a string";
                return false;
            }
            output.passNames.push_back(value.get<std::string>());
        }
    }
    if (!output.pipeline.empty() && !output.passNames.empty()) {
        error = "pipeline and passes are mutually exclusive";
        return false;
    }
    return true;
}

bool parseWorkflowReferences(const Json& arguments, std::vector<WorkflowReferenceQuery>& output,
                             std::string& error)
{
    if (!arguments.contains("references")) return true;
    const auto& values = arguments.at("references");
    if (!values.is_array()) {
        error = "references must be an array";
        return false;
    }
    for (const auto& value : values) {
        if (!value.is_object()) {
            error = "each references item must be an object";
            return false;
        }
        WorkflowReferenceQuery query;
        const auto kind = value.value("kind", std::string{});
        if (kind == "class") query.kind = WorkflowReferenceKind::Class;
        else if (kind == "field") query.kind = WorkflowReferenceKind::Field;
        else if (kind == "method") query.kind = WorkflowReferenceKind::Method;
        else {
            error = "unsupported workflow reference kind: " + kind;
            return false;
        }
        query.owner = value.value("owner", std::string{});
        query.name = value.value("name", std::string{});
        query.descriptor = value.value("descriptor", std::string{});
        output.push_back(std::move(query));
    }
    return true;
}

bool parseWorkflowDecompileClasses(const Json& arguments, std::vector<std::string>& output,
                                   bool& batch, std::string& error)
{
    const auto single = arguments.value("decompile_class", std::string{});
    batch = arguments.contains("decompile_classes");
    if (!batch) {
        if (!single.empty()) output.push_back(single);
        return true;
    }
    if (!single.empty()) {
        error = "decompile_class and decompile_classes are mutually exclusive";
        return false;
    }
    const auto& values = arguments.at("decompile_classes");
    if (!values.is_array()) {
        error = "decompile_classes must be an array";
        return false;
    }
    if (values.empty() || values.size() > kMaximumWorkflowDecompileClasses) {
        error = "decompile_classes must contain between 1 and " +
                std::to_string(kMaximumWorkflowDecompileClasses) + " class names";
        return false;
    }
    for (const auto& value : values) {
        if (!value.is_string() || value.get<std::string>().empty()) {
            error = "each decompile_classes item must be a non-empty string";
            return false;
        }
        output.push_back(value.get<std::string>());
    }
    return true;
}

bool parseJarRepairRequest(const Json& arguments, JarRepairRequest& output, std::string& error)
{
    const auto parseFlag = [&](const char* name, bool& target) {
        if (!arguments.contains(name)) return true;
        const auto& value = arguments.at(name);
        if (!value.is_boolean()) {
            error = std::string(name) + " must be a boolean";
            return false;
        }
        target = value.get<bool>();
        return true;
    };
    return parseFlag("remove_invalid_class_entries", output.removeInvalidClassEntries) &&
           parseFlag("remove_corrupt_output_markers", output.removeCorruptOutputMarkers);
}

}   //namespace mjt::mcp::detail
