#include "instructions.hpp"

#include "jblw/type/primitive_kind.hpp"

#include <algorithm>
#include <stdexcept>

namespace jblw::code {
namespace {
bool labelEquals(const LabelPtr& lhs, const LabelPtr& rhs) noexcept
{
    return lhs == rhs || (lhs && rhs && lhs->equals(*rhs));
}

bool labelsEqual(const std::vector<LabelPtr>& lhs, const std::vector<LabelPtr>& rhs) noexcept
{
    if (lhs.size() != rhs.size()) return false;
    for (ca::usize i = 0; i < lhs.size(); ++i) {
        if (!labelEquals(lhs[i], rhs[i])) return false;
    }
    return true;
}

bool constantsEqual(const std::vector<constant::ConstantPtr>& lhs,
                    const std::vector<constant::ConstantPtr>& rhs) noexcept
{
    if (lhs.size() != rhs.size()) return false;
    for (ca::usize i = 0; i < lhs.size(); ++i) {
        if (lhs[i] != rhs[i] && (!lhs[i] || !rhs[i] || !lhs[i]->equals(*rhs[i]))) return false;
    }
    return true;
}

template<typename T>
const T* instructionAs(const CodeElement& element, InstructionKind kind) noexcept
{
    if (element.getType() == CodeElementKind::LABEL) return nullptr;
    const auto& instruction = static_cast<const Instruction&>(element);
    return instruction.instructionKind() == kind ? static_cast<const T*>(&instruction) : nullptr;
}

void require(bool condition, const char* message)
{
    if (!condition) throw std::invalid_argument(message);
}
}   //namespace

bool SimpleInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value = instructionAs<SimpleInstruction>(other, InstructionKind::SIMPLE);
    return value && opcode_ == value->opcode_;
}

ConstantInstruction::ConstantInstruction(constant::ConstantPtr constant, InstructionKind kind,
                                         ca::i32 opcode)
    : constant_(std::move(constant))
    , kind_(kind)
    , opcode_(opcode)
{
    require(static_cast<bool>(constant_), "Constant instruction value must not be null");
}

bool ConstantInstruction::equals(const CodeElement& other) const noexcept
{
    if (other.getType() == CodeElementKind::LABEL) return false;
    const auto& instruction = static_cast<const Instruction&>(other);
    if (instruction.instructionKind() != kind_) return false;
    const auto& value = static_cast<const ConstantInstruction&>(instruction);
    return constant_ == value.constant_ || constant_->equals(*value.constant_);
}

InstructionPtr ConstantInstruction::wrap(constant::ConstantPtr constant)
{
    require(static_cast<bool>(constant), "Constant instruction value must not be null");
    switch (constant->getType()) {
    case constant::ConstantKind::STRING:
        return std::make_shared<StringConstantInstruction>(std::move(constant));
    case constant::ConstantKind::METHOD_HANDLE:
        return std::make_shared<MethodHandleConstantInstruction>(std::move(constant));
    case constant::ConstantKind::TYPE:
        return std::make_shared<TypeConstantInstruction>(std::move(constant));
    case constant::ConstantKind::DYNAMIC:
        return std::make_shared<DynamicConstantInstruction>(std::move(constant));
    case constant::ConstantKind::LONG:
        return std::make_shared<LongConstantInstruction>(std::move(constant));
    case constant::ConstantKind::DOUBLE:
        return std::make_shared<DoubleConstantInstruction>(std::move(constant));
    case constant::ConstantKind::INT:
        return std::make_shared<IntConstantInstruction>(std::move(constant));
    case constant::ConstantKind::FLOAT:
        return std::make_shared<FloatConstantInstruction>(std::move(constant));
    }
    throw std::logic_error("Unsupported constant kind");
}

AllocateInstruction::AllocateInstruction(type::ObjectTypePtr type)
    : type_(std::move(type))
{
    require(static_cast<bool>(type_), "Allocate type must not be null");
}

bool AllocateInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value = instructionAs<AllocateInstruction>(other, InstructionKind::ALLOCATE);
    return value && type_->equals(*value->type_);
}

AllocateMultiDimArrayInstruction::AllocateMultiDimArrayInstruction(type::ArrayTypePtr type,
                                                                   ca::i32            dimensions)
    : type_(std::move(type))
    , dimensions_(dimensions)
{
    require(static_cast<bool>(type_), "Array allocation type must not be null");
}

bool AllocateMultiDimArrayInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value = instructionAs<AllocateMultiDimArrayInstruction>(
        other, InstructionKind::ALLOCATE_MULTI_ARRAY);
    return value && dimensions_ == value->dimensions_ && type_->equals(*value->type_);
}

CheckCastInstruction::CheckCastInstruction(type::ObjectTypePtr type)
    : type_(std::move(type))
{
    require(static_cast<bool>(type_), "Check-cast type must not be null");
}

bool CheckCastInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value = instructionAs<CheckCastInstruction>(other, InstructionKind::CHECK_CAST);
    return value && type_->equals(*value->type_);
}

InstanceofInstruction::InstanceofInstruction(type::ObjectTypePtr type)
    : type_(std::move(type))
{
    require(static_cast<bool>(type_), "Instanceof type must not be null");
}

bool InstanceofInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value = instructionAs<InstanceofInstruction>(other, InstructionKind::INSTANCE_OF);
    return value && type_->equals(*value->type_);
}

ConditionalJumpInstruction::ConditionalJumpInstruction(ca::i32 opcode, LabelPtr target)
    : opcode_(opcode)
    , target_(std::move(target))
{
    require(static_cast<bool>(target_), "Jump target must not be null");
}

bool ConditionalJumpInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value =
        instructionAs<ConditionalJumpInstruction>(other, InstructionKind::CONDITIONAL_JUMP);
    return value && opcode_ == value->opcode_ && labelEquals(target_, value->target_);
}

ImmediateJumpInstruction::ImmediateJumpInstruction(ca::i32 opcode, LabelPtr target)
    : opcode_(opcode)
    , target_(std::move(target))
{
    require(static_cast<bool>(target_), "Jump target must not be null");
}

bool ImmediateJumpInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value =
        instructionAs<ImmediateJumpInstruction>(other, InstructionKind::IMMEDIATE_JUMP);
    return value && opcode_ == value->opcode_ && labelEquals(target_, value->target_);
}

SwitchInstruction::SwitchInstruction(LabelPtr defaultTarget, std::vector<LabelPtr> targets)
    : defaultTarget_(std::move(defaultTarget))
    , targets_(std::move(targets))
{
    require(static_cast<bool>(defaultTarget_), "Switch default target must not be null");
    for (const auto& target : targets_) {
        require(static_cast<bool>(target), "Switch case target must not be null");
    }
}

std::vector<LabelPtr> SwitchInstruction::targets() const
{
    std::vector<LabelPtr> result;
    result.reserve(caseTargets().size() + 1);
    result.push_back(defaultTarget());
    result.insert(result.end(), caseTargets().begin(), caseTargets().end());
    return result;
}

bool SwitchInstruction::switchEquals(const SwitchInstruction& other) const noexcept
{
    return labelEquals(defaultTarget(), other.defaultTarget()) &&
           labelsEqual(caseTargets(), other.caseTargets());
}

LookupSwitchInstruction::LookupSwitchInstruction(std::vector<ca::i32> keys, LabelPtr defaultTarget,
                                                 std::vector<LabelPtr> targets)
    : SwitchInstruction(std::move(defaultTarget), std::move(targets))
    , keys_(std::move(keys))
{
    require(keys_.size() == caseTargets().size(), "Lookup switch key/target counts differ");
    require(std::is_sorted(keys_.begin(), keys_.end()), "Lookup switch keys must be sorted");
}

LabelPtr LookupSwitchInstruction::select(ca::i32 key) const noexcept
{
    auto it = std::lower_bound(keys_.begin(), keys_.end(), key);
    if (it == keys_.end() || *it != key) return defaultTarget();
    return caseTargets()[static_cast<ca::usize>(it - keys_.begin())];
}

bool LookupSwitchInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value =
        instructionAs<LookupSwitchInstruction>(other, InstructionKind::LOOKUP_SWITCH);
    return value && keys_ == value->keys_ && switchEquals(*value);
}

TableSwitchInstruction::TableSwitchInstruction(ca::i32 min, LabelPtr defaultTarget,
                                               std::vector<LabelPtr> targets)
    : SwitchInstruction(std::move(defaultTarget), std::move(targets))
    , min_(min)
{}

LabelPtr TableSwitchInstruction::select(ca::i32 key) const noexcept
{
    const ca::i64 index = static_cast<ca::i64>(key) - min_;
    if (index < 0 || static_cast<ca::usize>(index) >= caseTargets().size()) return defaultTarget();
    return caseTargets()[static_cast<ca::usize>(index)];
}

std::vector<ca::i32> TableSwitchInstruction::keys() const
{
    std::vector<ca::i32> result;
    result.reserve(caseTargets().size());
    for (ca::usize i = 0; i < caseTargets().size(); ++i) {
        result.push_back(min_ + static_cast<ca::i32>(i));
    }
    return result;
}

bool TableSwitchInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value = instructionAs<TableSwitchInstruction>(other, InstructionKind::TABLE_SWITCH);
    return value && min_ == value->min_ && switchEquals(*value);
}

FieldInstruction::FieldInstruction(ca::i32 opcode, type::InstanceTypePtr owner,
                                   ca::str::Utf8String name, type::ClassTypePtr descriptor)
    : opcode_(opcode)
    , instanceOwner_(std::move(owner))
    , owner_(instanceOwner_)
    , name_(std::move(name))
    , fieldType_(std::move(descriptor))
    , descriptor_(fieldType_)
{
    require(static_cast<bool>(owner_) && static_cast<bool>(descriptor_),
            "Field owner and descriptor must not be null");
}

bool FieldInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value = instructionAs<FieldInstruction>(other, InstructionKind::FIELD);
    return value && opcode_ == value->opcode_ && owner_->equals(*value->owner_) &&
           name_.ref() == value->name_.ref() && descriptor_->equals(*value->descriptor_);
}

MethodInstruction::MethodInstruction(ca::i32 opcode, type::ObjectTypePtr owner,
                                     ca::str::Utf8String name, type::MethodTypePtr descriptor,
                                     bool isInterface)
    : opcode_(opcode)
    , owner_(std::move(owner))
    , name_(std::move(name))
    , methodType_(std::move(descriptor))
    , descriptor_(methodType_)
    , isInterface_(isInterface)
{
    require(static_cast<bool>(owner_) && static_cast<bool>(descriptor_),
            "Method owner and descriptor must not be null");
}

bool MethodInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value = instructionAs<MethodInstruction>(other, InstructionKind::METHOD);
    return value && opcode_ == value->opcode_ && isInterface_ == value->isInterface_ &&
           owner_->equals(*value->owner_) && name_.ref() == value->name_.ref() &&
           descriptor_->equals(*value->descriptor_);
}

InvokeDynamicInstruction::InvokeDynamicInstruction(ca::str::Utf8String   name,
                                                   type::TypePtr         descriptor,
                                                   type::MethodHandlePtr bootstrapHandle,
                                                   std::vector<constant::ConstantPtr> args)
    : name_(std::move(name))
    , descriptor_(std::move(descriptor))
    , bootstrapHandle_(std::move(bootstrapHandle))
    , args_(std::move(args))
{
    require(static_cast<bool>(descriptor_) && static_cast<bool>(bootstrapHandle_),
            "Invokedynamic descriptor and bootstrap handle must not be null");
    for (const auto& arg : args_) {
        require(static_cast<bool>(arg), "Invokedynamic argument must not be null");
    }
}

bool InvokeDynamicInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value =
        instructionAs<InvokeDynamicInstruction>(other, InstructionKind::INVOKE_DYNAMIC);
    return value && name_.ref() == value->name_.ref() && descriptor_->equals(*value->descriptor_) &&
           bootstrapHandle_->equals(*value->bootstrapHandle_) &&
           constantsEqual(args_, value->args_);
}

PrimitiveConversionInstruction::PrimitiveConversionInstruction(type::PrimitiveTypePtr from,
                                                               type::PrimitiveTypePtr to)
    : from_(std::move(from))
    , to_(std::move(to))
{
    require(static_cast<bool>(from_) && static_cast<bool>(to_),
            "Primitive conversion types must not be null");
}

void PrimitiveConversionInstruction::accept(PrimitiveConversion& conversion) const
{
    using type::PrimitiveKind;
    const ca::i32 from = from_->kind();
    const ca::i32 to   = to_->kind();
    if (from == PrimitiveKind::T_VOID || to == PrimitiveKind::T_VOID) {
        throw std::logic_error("Cannot convert void primitive");
    }
    if (from == to) return;

    const bool intLike = from == PrimitiveKind::T_INT || from == PrimitiveKind::T_BYTE ||
                         from == PrimitiveKind::T_CHAR || from == PrimitiveKind::T_SHORT ||
                         from == PrimitiveKind::T_BOOLEAN;
    auto narrowInt = [&]() -> bool {
        switch (to) {
        case PrimitiveKind::T_BYTE:
        case PrimitiveKind::T_BOOLEAN: conversion.i2b(); return true;
        case PrimitiveKind::T_CHAR: conversion.i2c(); return true;
        case PrimitiveKind::T_SHORT: conversion.i2s(); return true;
        default: return false;
        }
    };
    if (intLike) {
        switch (to) {
        case PrimitiveKind::T_LONG: conversion.i2l(); return;
        case PrimitiveKind::T_FLOAT: conversion.i2f(); return;
        case PrimitiveKind::T_DOUBLE: conversion.i2d(); return;
        default:
            if (narrowInt()) return;
        }
    }
    else if (from == PrimitiveKind::T_LONG) {
        switch (to) {
        case PrimitiveKind::T_INT: conversion.l2i(); return;
        case PrimitiveKind::T_FLOAT: conversion.l2f(); return;
        case PrimitiveKind::T_DOUBLE: conversion.l2d(); return;
        default:
            conversion.l2i();
            if (narrowInt()) return;
            break;
        }
    }
    else if (from == PrimitiveKind::T_FLOAT) {
        switch (to) {
        case PrimitiveKind::T_INT: conversion.f2i(); return;
        case PrimitiveKind::T_LONG: conversion.f2l(); return;
        case PrimitiveKind::T_DOUBLE: conversion.f2d(); return;
        default:
            conversion.f2i();
            if (narrowInt()) return;
            break;
        }
    }
    else if (from == PrimitiveKind::T_DOUBLE) {
        switch (to) {
        case PrimitiveKind::T_INT: conversion.d2i(); return;
        case PrimitiveKind::T_LONG: conversion.d2l(); return;
        case PrimitiveKind::T_FLOAT: conversion.d2f(); return;
        default:
            conversion.d2i();
            if (narrowInt()) return;
            break;
        }
    }
    throw std::logic_error("Unsupported primitive conversion");
}

bool PrimitiveConversionInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value =
        instructionAs<PrimitiveConversionInstruction>(other, InstructionKind::PRIMITIVE_CONVERSION);
    return value && from_->equals(*value->from_) && to_->equals(*value->to_);
}

bool VarInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value = instructionAs<VarInstruction>(other, InstructionKind::VARIABLE);
    return value && opcode_ == value->opcode_ && variableIndex_ == value->variableIndex_;
}

bool VariableIncrementInstruction::equals(const CodeElement& other) const noexcept
{
    const auto* value =
        instructionAs<VariableIncrementInstruction>(other, InstructionKind::VARIABLE_INCREMENT);
    return value && variableIndex_ == value->variableIndex_ && incrementBy_ == value->incrementBy_;
}

}   //namespace jblw::code
