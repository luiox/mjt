#include "class_mutation_service.hpp"

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <jbytecode/jbytecode.hpp>
#include "libca/core/tag_cast.hpp"

#include "mbase/jasm/resource_jasm_service.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/model/resource_model.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base::workspace {
namespace {

ca::Status error(ca::StatusCode code, const std::string& operation, const std::string& detail)
{
    return ca::ErrStatus(code, operation + ": " + detail);
}

ca::str::Utf8StringRef intern(ca::str::Utf8StringArena& arena, const std::string& value)
{
    return arena.intern(value.c_str());
}

jbytecode::tree::MethodNode* findMethod(jbytecode::tree::ClassNode& clazz,
                                        const MemberSelector&       selector)
{
    for (auto* method : clazz.methods) {
        if (method && method->name.to_std_string() == selector.name &&
            method->desc.to_std_string() == selector.descriptor)
            return method;
    }
    return nullptr;
}

jbytecode::tree::FieldNode* findField(jbytecode::tree::ClassNode& clazz,
                                      const MemberSelector&       selector)
{
    for (auto* field : clazz.fields) {
        if (field && field->name.to_std_string() == selector.name &&
            field->desc.to_std_string() == selector.descriptor)
            return field;
    }
    return nullptr;
}

std::vector<jbytecode::tree::AnnotationNode*>* annotationList(jbytecode::tree::ClassNode& clazz,
                                                              const ClassMutation&        mutation,
                                                              garena::Arena**             ownerArena)
{
    switch (mutation.annotationTarget) {
    case AnnotationTarget::Class:
        *ownerArena = &clazz.arena();
        return mutation.annotationVisible ? &clazz.visibleAnnotations : &clazz.invisibleAnnotations;
    case AnnotationTarget::Field:
    {
        auto* field = findField(clazz, mutation.member);
        if (!field) return nullptr;
        *ownerArena = &field->arena();
        return mutation.annotationVisible ? &field->visibleAnnotations
                                          : &field->invisibleAnnotations;
    }
    case AnnotationTarget::Method:
    {
        auto* method = findMethod(clazz, mutation.member);
        if (!method) return nullptr;
        *ownerArena = &method->arena();
        return mutation.annotationVisible ? &method->visibleAnnotations
                                          : &method->invisibleAnnotations;
    }
    }
    return nullptr;
}

using LabelMap = std::unordered_map<std::string, jbytecode::core::Label*>;

bool makeHandle(ca::str::Utf8StringArena& arena, const HandleSpec& spec,
                jbytecode::core::Handle& output, std::string& detail)
{
    if (spec.tag < jbytecode::core::Opcodes::H_GETFIELD ||
        spec.tag > jbytecode::core::Opcodes::H_INVOKEINTERFACE || spec.owner.empty() ||
        spec.name.empty() || spec.descriptor.empty()) {
        detail = "handle requires tag 1..9, owner, name and descriptor";
        return false;
    }
    output = jbytecode::core::Handle(spec.tag,
                                     intern(arena, spec.owner),
                                     intern(arena, spec.name),
                                     intern(arena, spec.descriptor),
                                     spec.interfaceReference);
    return true;
}

bool makeConstant(ca::str::Utf8StringArena& arena, const ConstantSpec& spec, ca::core::Any& output,
                  std::string& detail, ca::usize depth = 0)
{
    using jbytecode::core::ConstantDynamic;
    using jbytecode::core::Handle;
    using jbytecode::core::Type;
    if (depth > 8) {
        detail = "constant dynamic nesting exceeds 8";
        return false;
    }
    switch (spec.kind) {
    case ConstantKind::String: output = ca::core::Any(intern(arena, spec.stringValue)); return true;
    case ConstantKind::Integer:
        output = ca::core::Any(static_cast<int>(spec.intValue));
        return true;
    case ConstantKind::Long: output = ca::core::Any(spec.longValue); return true;
    case ConstantKind::Float: output = ca::core::Any(spec.floatValue); return true;
    case ConstantKind::Double: output = ca::core::Any(spec.doubleValue); return true;
    case ConstantKind::Type:
        if (spec.descriptor.empty()) {
            detail = "type constant descriptor is required";
            return false;
        }
        output = ca::core::Any(Type::fromArena(arena, intern(arena, spec.descriptor)));
        return true;
    case ConstantKind::Handle:
    {
        Handle handle(0, {}, {}, {}, false);
        if (!makeHandle(arena, spec.handle, handle, detail)) return false;
        output = ca::core::Any(std::move(handle));
        return true;
    }
    case ConstantKind::Dynamic:
    {
        if (spec.name.empty() || spec.descriptor.empty()) {
            detail = "constant dynamic name and descriptor are required";
            return false;
        }
        if (spec.bootstrapArguments.size() > 128) {
            detail = "bootstrap argument count exceeds 128";
            return false;
        }
        Handle bootstrap(0, {}, {}, {}, false);
        if (!makeHandle(arena, spec.bootstrapMethod, bootstrap, detail)) return false;
        std::vector<ca::core::Any> arguments;
        arguments.reserve(spec.bootstrapArguments.size());
        for (const auto& argument : spec.bootstrapArguments) {
            ca::core::Any value;
            if (!makeConstant(arena, argument, value, detail, depth + 1)) return false;
            arguments.push_back(std::move(value));
        }
        output = ca::core::Any(ConstantDynamic(intern(arena, spec.name),
                                               intern(arena, spec.descriptor),
                                               std::move(bootstrap),
                                               std::move(arguments)));
        return true;
    }
    }
    detail = "unsupported constant kind";
    return false;
}

jbytecode::core::Label* findLabel(const LabelMap& labels, const std::string& id,
                                  std::string& detail)
{
    const auto found = labels.find(id);
    if (id.empty() || found == labels.end()) {
        detail = "unknown label: " + id;
        return nullptr;
    }
    return found->second;
}

bool isSimpleOpcode(ca::i32 opcode)
{
    using O = jbytecode::core::Opcodes;
    return (opcode >= O::NOP && opcode <= O::DCONST_1) ||
           (opcode >= O::IALOAD && opcode <= O::SALOAD) ||
           (opcode >= O::IASTORE && opcode <= O::SASTORE) ||
           (opcode >= O::POP && opcode <= O::SWAP) || (opcode >= O::IADD && opcode <= O::LXOR) ||
           (opcode >= O::I2L && opcode <= O::I2S) || (opcode >= O::LCMP && opcode <= O::DCMPG) ||
           (opcode >= O::IRETURN && opcode <= O::RETURN) || opcode == O::ARRAYLENGTH ||
           opcode == O::ATHROW || opcode == O::MONITORENTER || opcode == O::MONITOREXIT;
}

bool validInstructionShape(const InstructionSpec& spec, std::string& detail)
{
    using O = jbytecode::core::Opcodes;
    switch (spec.kind) {
    case InstructionKind::Simple:
        if (!isSimpleOpcode(spec.opcode)) detail = "opcode is not a no-operand JVM instruction";
        break;
    case InstructionKind::Int:
        if (spec.opcode != O::BIPUSH && spec.opcode != O::SIPUSH && spec.opcode != O::NEWARRAY)
            detail = "opcode is not an int-operand JVM instruction";
        else if (spec.opcode == O::BIPUSH && (spec.operand < -128 || spec.operand > 127))
            detail = "bipush operand is outside -128..127";
        else if (spec.opcode == O::SIPUSH && (spec.operand < -32768 || spec.operand > 32767))
            detail = "sipush operand is outside -32768..32767";
        else if (spec.opcode == O::NEWARRAY &&
                 (spec.operand < O::T_BOOLEAN || spec.operand > O::T_LONG))
            detail = "newarray operand is outside the JVM primitive type range";
        break;
    case InstructionKind::Var:
        if (!((spec.opcode >= O::ILOAD && spec.opcode <= O::ALOAD) ||
              (spec.opcode >= O::ISTORE && spec.opcode <= O::ASTORE) || spec.opcode == O::RET))
            detail = "opcode is not a local-variable JVM instruction";
        else if (spec.operand < 0 || spec.operand > 65535)
            detail = "local variable slot is outside 0..65535";
        break;
    case InstructionKind::Type:
        if (spec.opcode != O::NEW && spec.opcode != O::ANEWARRAY && spec.opcode != O::CHECKCAST &&
            spec.opcode != O::INSTANCEOF)
            detail = "opcode is not a type JVM instruction";
        else if (spec.descriptor.empty())
            detail = "type instruction descriptor is required";
        break;
    case InstructionKind::Field:
        if (spec.opcode < O::GETSTATIC || spec.opcode > O::PUTFIELD)
            detail = "opcode is not a field JVM instruction";
        else if (spec.owner.empty() || spec.name.empty() || spec.descriptor.empty())
            detail = "field instruction requires owner, name and descriptor";
        break;
    case InstructionKind::Method:
        if (spec.opcode < O::INVOKEVIRTUAL || spec.opcode > O::INVOKEINTERFACE)
            detail = "opcode is not a method JVM instruction";
        else if (spec.owner.empty() || spec.name.empty() || spec.descriptor.empty())
            detail = "method instruction requires owner, name and descriptor";
        break;
    case InstructionKind::Jump:
        if (!((spec.opcode >= O::IFEQ && spec.opcode <= O::JSR) || spec.opcode == O::IFNULL ||
              spec.opcode == O::IFNONNULL))
            detail = "opcode is not a jump JVM instruction";
        break;
    case InstructionKind::Iinc:
        if (spec.operand < 0 || spec.operand > 65535 || spec.increment < -32768 ||
            spec.increment > 32767)
            detail = "iinc slot or increment is outside the JVM wide range";
        break;
    default: break;
    }
    return detail.empty();
}

///物化一条指令节点：在 owner 方法的内容 arena 上分配（寿命 = 方法寿命）。
///owner 由最终把节点挂入方法域的调用方传入；字符串仍经类级 Utf8StringArena intern。
jbytecode::tree::AbstractInsnNode* makeInstruction(ca::str::Utf8StringArena&    arena,
                                                   jbytecode::tree::MethodNode& owner,
                                                   const InstructionSpec&       spec,
                                                   const LabelMap&              labels,
                                                   std::string&                 detail)
{
    using namespace jbytecode::tree;
    if (!validInstructionShape(spec, detail)) return nullptr;
    switch (spec.kind) {
    case InstructionKind::Simple: return owner.create<InsnNode>(spec.opcode);
    case InstructionKind::Int: return owner.create<IntInsnNode>(spec.opcode, spec.operand);
    case InstructionKind::Var: return owner.create<VarInsnNode>(spec.opcode, spec.operand);
    case InstructionKind::Type:
        return owner.create<TypeInsnNode>(spec.opcode, intern(arena, spec.descriptor));
    case InstructionKind::Field:
        return owner.create<FieldInsnNode>(spec.opcode,
                                           intern(arena, spec.owner),
                                           intern(arena, spec.name),
                                           intern(arena, spec.descriptor));
    case InstructionKind::Method:
        return owner.create<MethodInsnNode>(spec.opcode,
                                            intern(arena, spec.owner),
                                            intern(arena, spec.name),
                                            intern(arena, spec.descriptor),
                                            spec.interfaceInvocation);
    case InstructionKind::Jump:
    {
        auto* label = findLabel(labels, spec.label, detail);
        return label ? owner.create<JumpInsnNode>(spec.opcode, label) : nullptr;
    }
    case InstructionKind::Label:
    {
        auto* label = findLabel(labels, spec.label, detail);
        return label ? owner.create<LabelNode>(label) : nullptr;
    }
    case InstructionKind::LdcString:
        return owner.create<LdcInsnNode>(intern(arena, spec.stringValue));
    case InstructionKind::LdcInt: return owner.create<LdcInsnNode>(spec.operand);
    case InstructionKind::LdcLong: return owner.create<LdcInsnNode>(spec.longValue);
    case InstructionKind::LdcFloat: return owner.create<LdcInsnNode>(spec.floatValue);
    case InstructionKind::LdcDouble: return owner.create<LdcInsnNode>(spec.doubleValue);
    case InstructionKind::LdcType:
        if (spec.descriptor.empty()) {
            detail = "LDC type descriptor is required";
            return nullptr;
        }
        return owner.create<LdcInsnNode>(
            jbytecode::core::Type::fromArena(arena, intern(arena, spec.descriptor)));
    case InstructionKind::LdcHandle:
    {
        jbytecode::core::Handle handle(0, {}, {}, {}, false);
        if (!makeHandle(arena, spec.handle, handle, detail)) return nullptr;
        return owner.create<LdcInsnNode>(handle);
    }
    case InstructionKind::LdcDynamic:
    {
        ConstantSpec constant;
        constant.kind               = ConstantKind::Dynamic;
        constant.name               = spec.name;
        constant.descriptor         = spec.descriptor;
        constant.bootstrapMethod    = spec.bootstrapMethod;
        constant.bootstrapArguments = spec.bootstrapArguments;
        ca::core::Any value;
        if (!makeConstant(arena, constant, value, detail)) return nullptr;
        return owner.create<LdcInsnNode>(value);
    }
    case InstructionKind::InvokeDynamic:
    {
        if (spec.name.empty() || spec.descriptor.empty()) {
            detail = "invokedynamic name and descriptor are required";
            return nullptr;
        }
        jbytecode::core::Handle bootstrap(0, {}, {}, {}, false);
        if (!makeHandle(arena, spec.bootstrapMethod, bootstrap, detail)) return nullptr;
        std::vector<ca::core::Any> arguments;
        arguments.reserve(spec.bootstrapArguments.size());
        for (const auto& argument : spec.bootstrapArguments) {
            ca::core::Any value;
            if (!makeConstant(arena, argument, value, detail)) return nullptr;
            arguments.push_back(std::move(value));
        }
        return owner.create<InvokeDynamicInsnNode>(
            intern(arena, spec.name), intern(arena, spec.descriptor), bootstrap, arguments);
    }
    case InstructionKind::Iinc: return owner.create<IincInsnNode>(spec.operand, spec.increment);
    case InstructionKind::TableSwitch:
    {
        if (spec.maximum < spec.minimum ||
            static_cast<ca::usize>(spec.maximum - spec.minimum + 1) != spec.labels.size()) {
            detail = "tableswitch labels must cover every key from minimum through maximum";
            return nullptr;
        }
        auto* defaultLabel = findLabel(labels, spec.defaultLabel, detail);
        if (!defaultLabel) return nullptr;
        std::vector<jbytecode::core::Label*> targets;
        for (const auto& id : spec.labels) {
            auto* label = findLabel(labels, id, detail);
            if (!label) return nullptr;
            targets.push_back(label);
        }
        return owner.create<TableSwitchInsnNode>(spec.minimum, spec.maximum, defaultLabel, targets);
    }
    case InstructionKind::LookupSwitch:
    {
        if (spec.keys.size() != spec.labels.size() ||
            !std::is_sorted(spec.keys.begin(), spec.keys.end()) ||
            std::adjacent_find(spec.keys.begin(), spec.keys.end()) != spec.keys.end()) {
            detail = "lookupswitch keys and labels must have equal size with unique ascending keys";
            return nullptr;
        }
        auto* defaultLabel = findLabel(labels, spec.defaultLabel, detail);
        if (!defaultLabel) return nullptr;
        std::vector<jbytecode::core::Label*> targets;
        for (const auto& id : spec.labels) {
            auto* label = findLabel(labels, id, detail);
            if (!label) return nullptr;
            targets.push_back(label);
        }
        return owner.create<LookupSwitchInsnNode>(
            defaultLabel, std::vector<int>(spec.keys.begin(), spec.keys.end()), targets);
    }
    case InstructionKind::MultiANewArray:
        if (spec.descriptor.empty() || spec.dimensions < 1 || spec.dimensions > 255) {
            detail = "multianewarray requires an array descriptor and dimensions in 1..255";
            return nullptr;
        }
        return owner.create<MultiANewArrayInsnNode>(intern(arena, spec.descriptor),
                                                    spec.dimensions);
    case InstructionKind::Line:
    {
        if (spec.line < 1) {
            detail = "line number must be positive";
            return nullptr;
        }
        auto* label = findLabel(labels, spec.label, detail);
        return label ? owner.create<LineNumberNode>(spec.line, label) : nullptr;
    }
    }
    detail = "unsupported instruction specification";
    return nullptr;
}

ca::Status existingLabels(const jbytecode::tree::MethodNode&   method,
                          const std::vector<LabelBindingSpec>& bindings, LabelMap& labels)
{
    for (const auto& binding : bindings) {
        if (binding.id.empty() || labels.find(binding.id) != labels.end())
            return error(ca::StatusCode::INVALID_ARGUMENT,
                         "bind labels",
                         "label IDs must be non-empty and unique");
        if (binding.instructionIndex < 0 || binding.instructionIndex >= method.instructions.size())
            return error(ca::StatusCode::INVALID_ARGUMENT,
                         "bind labels",
                         "label instruction index is outside the method");
        const auto* node = ca::core::tag_cast::dyn_cast<const jbytecode::tree::LabelNode>(
            method.instructions.get(binding.instructionIndex));
        if (!node)
            return error(ca::StatusCode::INVALID_ARGUMENT,
                         "bind labels",
                         "label binding must identify a LabelNode instruction");
        labels.emplace(binding.id, node->label);
    }
    return ca::OkStatus();
}

ca::Status replaceInstructions(ca::str::Utf8StringArena& arena, jbytecode::tree::MethodNode& method,
                               const ClassMutation& mutation)
{
    using namespace jbytecode::tree;
    const auto index = mutation.instructionIndex;
    if (index < 0 || index >= method.instructions.size())
        return error(ca::StatusCode::INVALID_ARGUMENT,
                     "edit instruction",
                     "instruction index is outside the method");

    auto* target = method.instructions.get(index);
    if (ca::core::tag_cast::isa<LabelNode>(target) && mutation.kind != ClassMutationKind::InsertInstruction)
        return error(ca::StatusCode::INVALID_ARGUMENT,
                     "edit instruction",
                     "existing LabelNode instructions cannot be removed or replaced");

    AbstractInsnNode* instruction = nullptr;
    if (mutation.kind != ClassMutationKind::RemoveInstruction) {
        LabelMap labels;
        auto     labelStatus = existingLabels(method, mutation.existingLabels, labels);
        if (labelStatus.is_err()) return labelStatus;
        if (mutation.instruction.kind == InstructionKind::Label) {
            if (mutation.instruction.label.empty() ||
                labels.find(mutation.instruction.label) != labels.end())
                return error(ca::StatusCode::INVALID_ARGUMENT,
                             "edit instruction",
                             "new label ID must be non-empty and not already bound");
            //新 Label 归方法域（B8 标准建点）：随方法 arena 回收，失败/成功路径均无堆泄漏。
            labels.emplace(mutation.instruction.label, method.createLabel());
        }
        std::string detail;
        instruction = makeInstruction(arena, method, mutation.instruction, labels, detail);
        if (!instruction)
            return error(ca::StatusCode::INVALID_ARGUMENT, "edit instruction", detail);
    }
    switch (mutation.kind) {
    case ClassMutationKind::RemoveInstruction:
        method.instructions.removeAndDelete(target);
        break;
    case ClassMutationKind::ReplaceInstruction: method.instructions.set(target, instruction); break;
    case ClassMutationKind::InsertInstruction:
        if (!mutation.insertAfter || target->getNext())
            method.instructions.insertBefore(mutation.insertAfter ? target->getNext() : target,
                                             instruction);
        else
            method.instructions.add(instruction);
        break;
    default:
        return error(ca::StatusCode::INVALID_ARGUMENT, "edit instruction", "invalid edit kind");
    }
    return ca::OkStatus();
}

ca::Status replaceTryCatchBlocks(ca::str::Utf8StringArena&            arena,
                                 jbytecode::tree::MethodNode&         method,
                                 const std::vector<TryCatchSpec>&     blocks,
                                 const std::vector<LabelBindingSpec>& bindings = {})
{
    std::vector<jbytecode::core::Label*> labels;
    labels.resize(method.instructions.size());
    for (ca::i32 index = 0; index < method.instructions.size(); ++index) {
        auto* node = method.instructions.get(index);
        if (const auto* label = ca::core::tag_cast::dyn_cast<const jbytecode::tree::LabelNode>(node))
            labels[index] = label->label;
    }
    LabelMap symbolicLabels;
    auto     labelStatus = existingLabels(method, bindings, symbolicLabels);
    if (labelStatus.is_err()) return labelStatus;
    const auto resolve = [&](const TryCatchSpec& block,
                             const std::string&  id,
                             ca::i32             index,
                             std::string&        detail) -> jbytecode::core::Label* {
        if (!id.empty()) return findLabel(symbolicLabels, id, detail);
        if (index >= 0 && index < static_cast<ca::i32>(labels.size())) return labels[index];
        return nullptr;
    };
    for (const auto& block : blocks) {
        std::string detail;
        if (!resolve(block, block.startLabel, block.startInstruction, detail) ||
            !resolve(block, block.endLabel, block.endInstruction, detail) ||
            !resolve(block, block.handlerLabel, block.handlerInstruction, detail)) {
            return error(ca::StatusCode::INVALID_ARGUMENT,
                         "replace try catch blocks",
                         detail.empty() ? "all boundaries must identify bound labels" : detail);
        }
    }
    for (auto* block : method.tryCatchBlocks)
        if (!method.arena().owns(block)) delete block;   //混合清除(B7)
    method.tryCatchBlocks.clear();
    for (const auto& block : blocks) {
        std::string detail;
        method.tryCatchBlocks.push_back(method.create<jbytecode::tree::TryCatchBlockNode>(
            resolve(block, block.startLabel, block.startInstruction, detail),
            resolve(block, block.endLabel, block.endInstruction, detail),
            resolve(block, block.handlerLabel, block.handlerInstruction, detail),
            intern(arena, block.type)));
    }
    return ca::OkStatus();
}

ca::Status replaceMethodBody(ca::str::Utf8StringArena& arena, jbytecode::tree::MethodNode& method,
                             const ClassMutation& mutation)
{
    using namespace jbytecode::tree;
    if ((method.access &
         (jbytecode::core::Opcodes::ACC_ABSTRACT | jbytecode::core::Opcodes::ACC_NATIVE)) != 0)
        return error(ca::StatusCode::INVALID_ARGUMENT,
                     "replace method body",
                     "abstract/native methods cannot have a body");
    if (mutation.initialInstructions.empty())
        return error(
            ca::StatusCode::INVALID_ARGUMENT, "replace method body", "instructions are required");

    LabelMap                        labels;
    std::unordered_set<std::string> labelIds;
    for (const auto& instruction : mutation.initialInstructions) {
        if (instruction.kind != InstructionKind::Label) continue;
        if (instruction.label.empty() || !labelIds.insert(instruction.label).second)
            return error(ca::StatusCode::INVALID_ARGUMENT,
                         "replace method body",
                         "label IDs must be non-empty and unique");
    }
    //新 Label 全部归方法域（B8 标准建点）：节点/tryCatch/局部变量按 owner 直返收编，
    //原件随方法 arena 回收，成功/失败路径均无堆泄漏。
    for (const auto& id : labelIds) labels.emplace(id, method.createLabel());

    //暂存表绑定 owner 方法域：错误回滚时 arena 节点只摘链不 delete（disposeNode 混合清除）
    InsnList    instructions{&method.arena()};
    std::string detail;
    for (const auto& instruction : mutation.initialInstructions) {
        auto* node = makeInstruction(arena, method, instruction, labels, detail);
        if (!node) return error(ca::StatusCode::INVALID_ARGUMENT, "replace method body", detail);
        instructions.add(node);
    }

    for (auto* block : method.tryCatchBlocks)
        if (!method.arena().owns(block)) delete block;   //混合清除(B7)
    method.tryCatchBlocks.clear();
    for (auto* local : method.localVariables)
        if (!method.arena().owns(local)) delete local;   //混合清除(B7)
    method.localVariables.clear();
    method.instructions.clear();
    method.instructions.add(instructions);

    for (const auto& block : mutation.tryCatchBlocks) {
        auto* start   = findLabel(labels, block.startLabel, detail);
        auto* end     = findLabel(labels, block.endLabel, detail);
        auto* handler = findLabel(labels, block.handlerLabel, detail);
        if (!start || !end || !handler)
            return error(ca::StatusCode::INVALID_ARGUMENT, "replace method body", detail);
        method.tryCatchBlocks.push_back(
            method.create<TryCatchBlockNode>(start, end, handler, intern(arena, block.type)));
    }
    for (const auto& local : mutation.localVariables) {
        auto* start = findLabel(labels, local.startLabel, detail);
        auto* end   = findLabel(labels, local.endLabel, detail);
        if (!start || !end || local.name.empty() || local.descriptor.empty() || local.index < 0)
            return error(ca::StatusCode::INVALID_ARGUMENT,
                         "replace method body",
                         detail.empty() ? "invalid local variable definition" : detail);
        method.localVariables.push_back(method.create<LocalVariableNode>(intern(arena, local.name),
                                                              intern(arena, local.descriptor),
                                                              intern(arena, local.signature),
                                                              start,
                                                              end,
                                                              local.index));
    }
    return ca::OkStatus();
}

ca::Status validateVisibility(ca::i32 access, const std::string& operation)
{
    const auto visibility =
        access & (jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_PRIVATE |
                  jbytecode::core::Opcodes::ACC_PROTECTED);
    if (visibility != 0 && visibility != jbytecode::core::Opcodes::ACC_PUBLIC &&
        visibility != jbytecode::core::Opcodes::ACC_PRIVATE &&
        visibility != jbytecode::core::Opcodes::ACC_PROTECTED)
        return error(ca::StatusCode::INVALID_ARGUMENT,
                     operation,
                     "public, private and protected are mutually exclusive");
    return ca::OkStatus();
}

ca::Status validateAccess(ca::i32 access, AnnotationTarget target, const std::string& operation)
{
    using O     = jbytecode::core::Opcodes;
    auto status = validateVisibility(access, operation);
    if (status.is_err()) return status;
    ca::i32 allowed = 0;
    switch (target) {
    case AnnotationTarget::Class:
        allowed = O::ACC_PUBLIC | O::ACC_FINAL | O::ACC_SUPER | O::ACC_INTERFACE | O::ACC_ABSTRACT |
                  O::ACC_SYNTHETIC | O::ACC_ANNOTATION | O::ACC_ENUM | O::ACC_MODULE |
                  O::ACC_RECORD | O::ACC_DEPRECATED | O::ACC_SEALED;
        if ((access & O::ACC_FINAL) && (access & O::ACC_ABSTRACT))
            return error(
                ca::StatusCode::INVALID_ARGUMENT, operation, "class cannot be final and abstract");
        if ((access & O::ACC_ANNOTATION) && !(access & O::ACC_INTERFACE))
            return error(ca::StatusCode::INVALID_ARGUMENT,
                         operation,
                         "annotation class must be an interface");
        if ((access & O::ACC_INTERFACE) && !(access & O::ACC_ABSTRACT))
            return error(ca::StatusCode::INVALID_ARGUMENT, operation, "interface must be abstract");
        break;
    case AnnotationTarget::Field:
        allowed = O::ACC_PUBLIC | O::ACC_PRIVATE | O::ACC_PROTECTED | O::ACC_STATIC | O::ACC_FINAL |
                  O::ACC_VOLATILE | O::ACC_TRANSIENT | O::ACC_SYNTHETIC | O::ACC_ENUM |
                  O::ACC_DEPRECATED;
        if ((access & O::ACC_FINAL) && (access & O::ACC_VOLATILE))
            return error(
                ca::StatusCode::INVALID_ARGUMENT, operation, "field cannot be final and volatile");
        break;
    case AnnotationTarget::Method:
        allowed = O::ACC_PUBLIC | O::ACC_PRIVATE | O::ACC_PROTECTED | O::ACC_STATIC | O::ACC_FINAL |
                  O::ACC_SYNCHRONIZED | O::ACC_BRIDGE | O::ACC_VARARGS | O::ACC_NATIVE |
                  O::ACC_ABSTRACT | O::ACC_STRICT | O::ACC_SYNTHETIC | O::ACC_DEPRECATED;
        if ((access & O::ACC_ABSTRACT) &&
            (access & (O::ACC_PRIVATE | O::ACC_STATIC | O::ACC_FINAL | O::ACC_SYNCHRONIZED |
                       O::ACC_NATIVE | O::ACC_STRICT)))
            return error(ca::StatusCode::INVALID_ARGUMENT,
                         operation,
                         "abstract method has an incompatible access flag");
        break;
    }
    if ((access & ~allowed) != 0)
        return error(
            ca::StatusCode::INVALID_ARGUMENT, operation, "access contains invalid flag bits");
    return ca::OkStatus();
}

bool fieldConstant(ca::str::Utf8StringArena& arena, const ClassMutation& mutation,
                   ca::core::Any& output, std::string& detail)
{
    if (!mutation.hasFieldValue) return true;
    const auto& descriptor = mutation.member.descriptor;
    const auto  kind       = mutation.fieldValue.kind;
    const bool  valid      = ((descriptor == "Z" || descriptor == "B" || descriptor == "C" ||
                         descriptor == "S" || descriptor == "I") &&
                        kind == ConstantKind::Integer) ||
                       (descriptor == "J" && kind == ConstantKind::Long) ||
                       (descriptor == "F" && kind == ConstantKind::Float) ||
                       (descriptor == "D" && kind == ConstantKind::Double) ||
                       (descriptor == "Ljava/lang/String;" && kind == ConstantKind::String);
    if (!valid) {
        detail = "field constant kind does not match its descriptor";
        return false;
    }
    return makeConstant(arena, mutation.fieldValue, output, detail);
}

ClassMutationOutcome failure(ca::Status status)
{
    ClassMutationOutcome result;
    result.status = std::move(status);
    return result;
}

}   //namespace

jasm::Result<jasm::JvmJasmClass> ClassMutationService::applyJasm(const std::string&      uri,
                                                                 ca::str::Utf16StringRef sourceName,
                                                                 ca::str::Utf16StringRef source)
{
    jasm::ResourceJasmService service(*container_, *analysisManager_);
    return service.assemble(uri, sourceName, source);
}

ClassMutationOutcome ClassMutationService::apply(const ClassMutation& mutation)
{
    if (mutation.classUri.empty())
        return failure(
            error(ca::StatusCode::INVALID_ARGUMENT, "mutate class", "class URI is empty"));
    IResource* rawResource = container_->getResource(mutation.classUri);
    if (!ca::core::tag_cast::isa<ClassResource>(rawResource))
        return failure(error(ca::StatusCode::NOT_FOUND, "mutate class", mutation.classUri));
    auto* resource = ca::core::tag_cast::cast<ClassResource>(rawResource);
    try {
        ca::str::Utf8StringArena     arena;
        jbytecode::tree::ClassNode   clazz;
        jbytecode::core::ClassReader reader(arena, resource->get());
        reader.accept(&clazz, jbytecode::core::ClassReader::SKIP_FRAMES);

        ca::Status status = ca::OkStatus();
        switch (mutation.kind) {
        case ClassMutationKind::SetClassAccess:
            status = validateAccess(mutation.access, AnnotationTarget::Class, "set class access");
            if (status.is_ok()) clazz.access = mutation.access;
            break;
        case ClassMutationKind::SetFieldAccess:
        {
            auto* field = findField(clazz, mutation.member);
            status =
                field ? validateAccess(mutation.access, AnnotationTarget::Field, "set field access")
                      : error(ca::StatusCode::NOT_FOUND, "set field access", mutation.member.name);
            if (field && status.is_ok()) field->access = mutation.access;
            break;
        }
        case ClassMutationKind::SetMethodAccess:
        {
            auto* method = findMethod(clazz, mutation.member);
            status =
                method
                    ? validateAccess(mutation.access, AnnotationTarget::Method, "set method access")
                    : error(ca::StatusCode::NOT_FOUND, "set method access", mutation.member.name);
            if (method && status.is_ok()) {
                const bool dropsBody =
                    (mutation.access & (jbytecode::core::Opcodes::ACC_ABSTRACT |
                                        jbytecode::core::Opcodes::ACC_NATIVE)) != 0;
                if (dropsBody && !method->instructions.isEmpty())
                    status = error(ca::StatusCode::INVALID_ARGUMENT,
                                   "set method access",
                                   "cannot mark a method with an existing body abstract/native");
                else
                    method->access = mutation.access;
            }
            break;
        }
        case ClassMutationKind::AddField:
            if (findField(clazz, mutation.member)) {
                status = error(ca::StatusCode::ALREADY_EXISTS, "add field", mutation.member.name);
            }
            else if (mutation.member.name.empty() || mutation.member.descriptor.empty()) {
                status = error(ca::StatusCode::INVALID_ARGUMENT,
                               "add field",
                               "name and descriptor are required");
            }
            else {
                status = validateAccess(mutation.access, AnnotationTarget::Field, "add field");
                if (status.is_err()) break;
                ca::core::Any value;
                std::string   detail;
                if (!fieldConstant(arena, mutation, value, detail)) {
                    status = error(ca::StatusCode::INVALID_ARGUMENT, "add field", detail);
                    break;
                }
                clazz.fields.push_back(
                    new jbytecode::tree::FieldNode(mutation.access,
                                                   intern(arena, mutation.member.name),
                                                   intern(arena, mutation.member.descriptor),
                                                   intern(arena, mutation.signature),
                                                   value));
            }
            break;
        case ClassMutationKind::RemoveField:
        {
            bool removed = false;
            for (auto it = clazz.fields.begin(); it != clazz.fields.end(); ++it) {
                if (*it && (*it)->name.to_std_string() == mutation.member.name &&
                    (*it)->desc.to_std_string() == mutation.member.descriptor) {
                    if (!clazz.arena().owns(*it)) delete *it;   //混合清除(B7)
                    clazz.fields.erase(it);
                    removed = true;
                    break;
                }
            }
            if (!removed)
                status = error(ca::StatusCode::NOT_FOUND, "remove field", mutation.member.name);
            break;
        }
        case ClassMutationKind::AddMethod:
        {
            if (findMethod(clazz, mutation.member)) {
                status = error(ca::StatusCode::ALREADY_EXISTS, "add method", mutation.member.name);
                break;
            }
            if (mutation.member.name.empty() || mutation.member.descriptor.empty()) {
                status = error(ca::StatusCode::INVALID_ARGUMENT,
                               "add method",
                               "name and descriptor are required");
                break;
            }
            status = validateAccess(mutation.access, AnnotationTarget::Method, "add method");
            if (status.is_err()) break;
            const bool bodyForbidden =
                (mutation.access & (jbytecode::core::Opcodes::ACC_ABSTRACT |
                                    jbytecode::core::Opcodes::ACC_NATIVE)) != 0;
            if (mutation.initialInstructions.empty() && !bodyForbidden) {
                status = error(ca::StatusCode::INVALID_ARGUMENT,
                               "add method",
                               "non abstract/native methods require initial instructions");
                break;
            }
            if (!mutation.initialInstructions.empty() && bodyForbidden) {
                status = error(ca::StatusCode::INVALID_ARGUMENT,
                               "add method",
                               "abstract/native methods cannot define initial instructions");
                break;
            }
            {
                std::vector<ca::str::Utf8StringRef> exceptions;
                exceptions.reserve(mutation.exceptions.size());
                for (const auto& exception : mutation.exceptions) {
                    if (exception.empty()) {
                        status = error(ca::StatusCode::INVALID_ARGUMENT,
                                       "add method",
                                       "exception internal names must be non-empty");
                        break;
                    }
                    exceptions.push_back(intern(arena, exception));
                }
                if (status.is_err()) break;
                auto* method =
                    new jbytecode::tree::MethodNode(mutation.access,
                                                    intern(arena, mutation.member.name),
                                                    intern(arena, mutation.member.descriptor),
                                                    intern(arena, mutation.signature),
                                                    exceptions);
                if (!bodyForbidden) {
                    status = replaceMethodBody(arena, *method, mutation);
                    if (status.is_err()) {
                        if (!clazz.arena().owns(method)) delete method;   //混合清除(B7)
                        break;
                    }
                }
                clazz.methods.push_back(method);
            }
            break;
        }
        case ClassMutationKind::RemoveMethod:
        {
            bool removed = false;
            for (auto it = clazz.methods.begin(); it != clazz.methods.end(); ++it) {
                if (*it && (*it)->name.to_std_string() == mutation.member.name &&
                    (*it)->desc.to_std_string() == mutation.member.descriptor) {
                    if (!clazz.arena().owns(*it)) delete *it;   //混合清除(B7)
                    clazz.methods.erase(it);
                    removed = true;
                    status  = ca::OkStatus();
                    break;
                }
            }
            if (!removed)
                status = error(ca::StatusCode::NOT_FOUND, "remove method", mutation.member.name);
            break;
        }
        case ClassMutationKind::AddMarkerAnnotation:
        case ClassMutationKind::RemoveAnnotation:
        {
            if (mutation.annotationDescriptor.empty()) {
                status = error(ca::StatusCode::INVALID_ARGUMENT,
                               "edit annotation",
                               "annotation descriptor is required");
                break;
            }
            garena::Arena* annotationArena = nullptr;
            auto* annotations     = annotationList(clazz, mutation, &annotationArena);
            if (!annotations) {
                status =
                    error(ca::StatusCode::NOT_FOUND,
                          "edit annotation",
                          mutation.member.name.empty() ? mutation.classUri : mutation.member.name);
                break;
            }
            if (mutation.kind == ClassMutationKind::AddMarkerAnnotation) {
                const auto duplicate =
                    std::any_of(annotations->begin(), annotations->end(), [&](const auto* item) {
                        return item && item->desc.to_std_string() == mutation.annotationDescriptor;
                    });
                if (duplicate) {
                    status = error(ca::StatusCode::ALREADY_EXISTS,
                                   "add annotation",
                                   mutation.annotationDescriptor);
                    break;
                }
                annotations->push_back(annotationArena->create<jbytecode::tree::AnnotationNode>(
                    *annotationArena, intern(arena, mutation.annotationDescriptor)));
                break;
            }
            bool removed = false;
            for (auto it = annotations->begin(); it != annotations->end(); ++it) {
                if (*it && (*it)->desc.to_std_string() == mutation.annotationDescriptor) {
                    //混合清除：arena 节点由 owner arena 统一析构，堆节点维持立即 delete
                    if (!annotationArena->owns(*it)) delete *it;
                    annotations->erase(it);
                    removed = true;
                    break;
                }
            }
            if (!removed)
                status = error(
                    ca::StatusCode::NOT_FOUND, "remove annotation", mutation.annotationDescriptor);
            break;
        }
        case ClassMutationKind::RenameLocal:
        {
            auto* method = findMethod(clazz, mutation.member);
            if (!method) {
                status = error(ca::StatusCode::NOT_FOUND, "rename local", mutation.member.name);
                break;
            }
            bool changed = false;
            for (auto* local : method->localVariables) {
                if (!local || local->name.to_std_string() != mutation.localName) continue;
                if (mutation.localIndex >= 0 && local->index != mutation.localIndex) continue;
                local->name = intern(arena, mutation.replacementLocalName);
                changed     = true;
            }
            if (!changed)
                status = error(ca::StatusCode::NOT_FOUND, "rename local", mutation.localName);
            break;
        }
        case ClassMutationKind::ReplaceTryCatchBlocks:
        {
            auto* method = findMethod(clazz, mutation.member);
            status       = method ? replaceTryCatchBlocks(
                                  arena, *method, mutation.tryCatchBlocks, mutation.existingLabels)
                                  : error(ca::StatusCode::NOT_FOUND,
                                    "replace try catch blocks",
                                    mutation.member.name);
            break;
        }
        case ClassMutationKind::ReplaceMethodBody:
        {
            auto* method = findMethod(clazz, mutation.member);
            status =
                method
                    ? replaceMethodBody(arena, *method, mutation)
                    : error(ca::StatusCode::NOT_FOUND, "replace method body", mutation.member.name);
            break;
        }
        case ClassMutationKind::InsertInstruction:
        case ClassMutationKind::ReplaceInstruction:
        case ClassMutationKind::RemoveInstruction:
        {
            auto* method = findMethod(clazz, mutation.member);
            status =
                method ? replaceInstructions(arena, *method, mutation)
                       : error(ca::StatusCode::NOT_FOUND, "edit instruction", mutation.member.name);
            break;
        }
        }
        if (status.is_err()) return failure(std::move(status));

        jbytecode::core::ClassWriter writer(arena,
                                            jbytecode::core::ClassWriter::COMPUTE_MAXS |
                                                jbytecode::core::ClassWriter::COMPUTE_FRAMES);
        clazz.accept(&writer);
        auto bytes = writer.toByteArray();

        jasm::JvmJasmService validator;
        auto                 validated = validator.validate(bytes);
        if (!validated.isOk()) {
            ClassMutationOutcome result;
            result.status      = error(ca::StatusCode::INVALID_ARGUMENT,
                                  "mutate class",
                                  "candidate class did not pass validation");
            result.diagnostics = std::move(validated.diagnostics);
            return result;
        }
        resource->set(std::move(bytes));
        ClassMutationOutcome result;
        result.diagnostics = std::move(validated.diagnostics);
        return result;
    }
    catch (const std::exception& exception) {
        return failure(error(ca::StatusCode::INTERNAL, "mutate class", exception.what()));
    }
}

}   //namespace mjt::base::workspace
