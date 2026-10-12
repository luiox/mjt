#pragma once

#include "code.hpp"
#include "opcodes.hpp"

#include "jblw/constant/constant.hpp"
#include "jblw/type/array_type.hpp"
#include "jblw/type/invoke_dynamic.hpp"
#include "jblw/type/primitive_type.hpp"

#include <vector>

namespace jblw::code {

class SimpleInstruction final : public Instruction
{
public:
    explicit SimpleInstruction(ca::i32 opcode) noexcept
        : opcode_(opcode)
    {}
    InstructionKind instructionKind() const noexcept override { return InstructionKind::SIMPLE; }
    ca::i32         opcode() const noexcept override { return opcode_; }
    bool            equals(const CodeElement& other) const noexcept override;

private:
    ca::i32 opcode_;
};

///@brief 规范化 LDC 常量指令的公共实现。
class ConstantInstruction : public Instruction
{
public:
    const constant::ConstantPtr& constant() const noexcept { return constant_; }
    InstructionKind              instructionKind() const noexcept final { return kind_; }
    ca::i32                      opcode() const noexcept final { return opcode_; }
    bool                         equals(const CodeElement& other) const noexcept final;

    static InstructionPtr wrap(constant::ConstantPtr constant);

protected:
    ConstantInstruction(constant::ConstantPtr constant, InstructionKind kind, ca::i32 opcode);

private:
    constant::ConstantPtr constant_;
    InstructionKind       kind_;
    ca::i32               opcode_;
};

#define JBLW_CONSTANT_INSTRUCTION(className, kindName, opcodeName)                \
    class className final : public ConstantInstruction                            \
    {                                                                             \
    public:                                                                       \
        explicit className(constant::ConstantPtr constant)                        \
            : ConstantInstruction(std::move(constant), InstructionKind::kindName, \
                                  ExtensionOpcodes::opcodeName)                   \
        {}                                                                        \
    }

JBLW_CONSTANT_INSTRUCTION(StringConstantInstruction, CONSTANT_STRING, STRING_CONSTANT);
JBLW_CONSTANT_INSTRUCTION(MethodHandleConstantInstruction, CONSTANT_METHOD_HANDLE,
                          METHOD_HANDLE_CONSTANT);
JBLW_CONSTANT_INSTRUCTION(DynamicConstantInstruction, CONSTANT_DYNAMIC, DYNAMIC_CONSTANT);
JBLW_CONSTANT_INSTRUCTION(LongConstantInstruction, CONSTANT_LONG, LONG_CONSTANT);
JBLW_CONSTANT_INSTRUCTION(DoubleConstantInstruction, CONSTANT_DOUBLE, DOUBLE_CONSTANT);
JBLW_CONSTANT_INSTRUCTION(IntConstantInstruction, CONSTANT_INT, INT_CONSTANT);
JBLW_CONSTANT_INSTRUCTION(FloatConstantInstruction, CONSTANT_FLOAT, FLOAT_CONSTANT);
JBLW_CONSTANT_INSTRUCTION(TypeConstantInstruction, CONSTANT_TYPE, TYPE_CONSTANT);

#undef JBLW_CONSTANT_INSTRUCTION

class AllocateInstruction final : public Instruction
{
public:
    explicit AllocateInstruction(type::ObjectTypePtr type);
    const type::ObjectTypePtr& type() const noexcept { return type_; }
    InstructionKind instructionKind() const noexcept override { return InstructionKind::ALLOCATE; }
    ca::i32         opcode() const noexcept override { return ExtensionOpcodes::ALLOCATE; }
    bool            equals(const CodeElement& other) const noexcept override;

private:
    type::ObjectTypePtr type_;
};

class AllocateMultiDimArrayInstruction final : public Instruction
{
public:
    AllocateMultiDimArrayInstruction(type::ArrayTypePtr type, ca::i32 dimensions);
    const type::ArrayTypePtr& type() const noexcept { return type_; }
    ca::i32                   dimensions() const noexcept { return dimensions_; }
    InstructionKind           instructionKind() const noexcept override
    {
        return InstructionKind::ALLOCATE_MULTI_ARRAY;
    }
    ca::i32 opcode() const noexcept override { return JavaOpcodes::MULTIANEWARRAY; }
    bool    equals(const CodeElement& other) const noexcept override;

private:
    type::ArrayTypePtr type_;
    ca::i32            dimensions_;
};

class CheckCastInstruction final : public Instruction
{
public:
    explicit CheckCastInstruction(type::ObjectTypePtr type);
    const type::ObjectTypePtr& type() const noexcept { return type_; }
    InstructionKind            instructionKind() const noexcept override
    {
        return InstructionKind::CHECK_CAST;
    }
    ca::i32 opcode() const noexcept override { return JavaOpcodes::CHECKCAST; }
    bool    equals(const CodeElement& other) const noexcept override;

private:
    type::ObjectTypePtr type_;
};

class InstanceofInstruction final : public Instruction
{
public:
    explicit InstanceofInstruction(type::ObjectTypePtr type);
    const type::ObjectTypePtr& type() const noexcept { return type_; }
    InstructionKind            instructionKind() const noexcept override
    {
        return InstructionKind::INSTANCE_OF;
    }
    ca::i32 opcode() const noexcept override { return JavaOpcodes::INSTANCEOF; }
    bool    equals(const CodeElement& other) const noexcept override;

private:
    type::ObjectTypePtr type_;
};

///@brief 跳转指令公共查询接口。
class BranchInstruction : public Instruction
{
public:
    virtual std::vector<LabelPtr> targets() const                 = 0;
    virtual bool                  hasFallthrough() const noexcept = 0;

protected:
    BranchInstruction() = default;
};

class ConditionalJumpInstruction final : public BranchInstruction
{
public:
    ConditionalJumpInstruction(ca::i32 opcode, LabelPtr target);
    const LabelPtr&       target() const noexcept { return target_; }
    std::vector<LabelPtr> targets() const override { return {target_}; }
    bool                  hasFallthrough() const noexcept override { return true; }
    InstructionKind       instructionKind() const noexcept override
    {
        return InstructionKind::CONDITIONAL_JUMP;
    }
    ca::i32 opcode() const noexcept override { return opcode_; }
    bool    equals(const CodeElement& other) const noexcept override;

private:
    ca::i32  opcode_;
    LabelPtr target_;
};

class ImmediateJumpInstruction final : public BranchInstruction
{
public:
    ImmediateJumpInstruction(ca::i32 opcode, LabelPtr target);
    const LabelPtr&       target() const noexcept { return target_; }
    std::vector<LabelPtr> targets() const override { return {target_}; }
    bool                  hasFallthrough() const noexcept override { return false; }
    InstructionKind       instructionKind() const noexcept override
    {
        return InstructionKind::IMMEDIATE_JUMP;
    }
    ca::i32 opcode() const noexcept override { return opcode_; }
    bool    equals(const CodeElement& other) const noexcept override;

private:
    ca::i32  opcode_;
    LabelPtr target_;
};

class SwitchInstruction : public BranchInstruction
{
public:
    const LabelPtr&              defaultTarget() const noexcept { return defaultTarget_; }
    const std::vector<LabelPtr>& caseTargets() const noexcept { return targets_; }
    std::vector<LabelPtr>        targets() const override;
    bool                         hasFallthrough() const noexcept final { return false; }
    virtual LabelPtr             select(ca::i32 key) const noexcept = 0;
    virtual std::vector<ca::i32> keys() const                       = 0;

protected:
    SwitchInstruction(LabelPtr defaultTarget, std::vector<LabelPtr> targets);
    bool switchEquals(const SwitchInstruction& other) const noexcept;

private:
    LabelPtr              defaultTarget_;
    std::vector<LabelPtr> targets_;
};

class LookupSwitchInstruction final : public SwitchInstruction
{
public:
    LookupSwitchInstruction(std::vector<ca::i32> keys, LabelPtr defaultTarget,
                            std::vector<LabelPtr> targets);
    InstructionKind instructionKind() const noexcept override
    {
        return InstructionKind::LOOKUP_SWITCH;
    }
    ca::i32              opcode() const noexcept override { return JavaOpcodes::LOOKUPSWITCH; }
    LabelPtr             select(ca::i32 key) const noexcept override;
    std::vector<ca::i32> keys() const override { return keys_; }
    bool                 equals(const CodeElement& other) const noexcept override;

private:
    std::vector<ca::i32> keys_;
};

class TableSwitchInstruction final : public SwitchInstruction
{
public:
    TableSwitchInstruction(ca::i32 min, LabelPtr defaultTarget, std::vector<LabelPtr> targets);
    ca::i32         min() const noexcept { return min_; }
    InstructionKind instructionKind() const noexcept override
    {
        return InstructionKind::TABLE_SWITCH;
    }
    ca::i32              opcode() const noexcept override { return JavaOpcodes::TABLESWITCH; }
    LabelPtr             select(ca::i32 key) const noexcept override;
    std::vector<ca::i32> keys() const override;
    bool                 equals(const CodeElement& other) const noexcept override;

private:
    ca::i32 min_;
};

///@brief 字段或方法成员指令的公共接口。
class MemberInstruction : public Instruction
{
public:
    virtual const type::ObjectTypePtr& owner() const noexcept = 0;
    virtual ca::str::Utf8StringRef     name() const noexcept  = 0;
    virtual const type::TypePtr&       type() const noexcept  = 0;

protected:
    MemberInstruction() = default;
};

class FieldInstruction final : public MemberInstruction
{
public:
    FieldInstruction(ca::i32 opcode, type::InstanceTypePtr owner, ca::str::Utf8String name,
                     type::ClassTypePtr descriptor);
    const type::ObjectTypePtr&   owner() const noexcept override { return owner_; }
    ca::str::Utf8StringRef       name() const noexcept override { return name_.ref(); }
    const type::TypePtr&         type() const noexcept override { return descriptor_; }
    const type::InstanceTypePtr& instanceOwner() const noexcept { return instanceOwner_; }
    const type::ClassTypePtr&    fieldType() const noexcept { return fieldType_; }
    InstructionKind instructionKind() const noexcept override { return InstructionKind::FIELD; }
    ca::i32         opcode() const noexcept override { return opcode_; }
    bool            equals(const CodeElement& other) const noexcept override;

private:
    ca::i32               opcode_;
    type::InstanceTypePtr instanceOwner_;
    type::ObjectTypePtr   owner_;
    ca::str::Utf8String   name_;
    type::ClassTypePtr    fieldType_;
    type::TypePtr         descriptor_;
};

class MethodInstruction final : public MemberInstruction
{
public:
    MethodInstruction(ca::i32 opcode, type::ObjectTypePtr owner, ca::str::Utf8String name,
                      type::MethodTypePtr descriptor, bool isInterface);
    const type::ObjectTypePtr& owner() const noexcept override { return owner_; }
    ca::str::Utf8StringRef     name() const noexcept override { return name_.ref(); }
    const type::TypePtr&       type() const noexcept override { return descriptor_; }
    const type::MethodTypePtr& methodType() const noexcept { return methodType_; }
    bool                       isInterface() const noexcept { return isInterface_; }
    InstructionKind instructionKind() const noexcept override { return InstructionKind::METHOD; }
    ca::i32         opcode() const noexcept override { return opcode_; }
    bool            equals(const CodeElement& other) const noexcept override;

private:
    ca::i32             opcode_;
    type::ObjectTypePtr owner_;
    ca::str::Utf8String name_;
    type::MethodTypePtr methodType_;
    type::TypePtr       descriptor_;
    bool                isInterface_;
};

class InvokeDynamicInstruction final : public Instruction
{
public:
    InvokeDynamicInstruction(ca::str::Utf8String name, type::TypePtr descriptor,
                             type::MethodHandlePtr              bootstrapHandle,
                             std::vector<constant::ConstantPtr> args);
    ca::str::Utf8StringRef       name() const noexcept { return name_.ref(); }
    const type::TypePtr&         type() const noexcept { return descriptor_; }
    const type::MethodHandlePtr& bootstrapHandle() const noexcept { return bootstrapHandle_; }
    const std::vector<constant::ConstantPtr>& args() const noexcept { return args_; }
    InstructionKind                           instructionKind() const noexcept override
    {
        return InstructionKind::INVOKE_DYNAMIC;
    }
    ca::i32 opcode() const noexcept override { return JavaOpcodes::INVOKEDYNAMIC; }
    bool    equals(const CodeElement& other) const noexcept override;

private:
    ca::str::Utf8String                name_;
    type::TypePtr                      descriptor_;
    type::MethodHandlePtr              bootstrapHandle_;
    std::vector<constant::ConstantPtr> args_;
};

///@brief primitive conversion 展开目标。
class PrimitiveConversion
{
public:
    virtual ~PrimitiveConversion() = default;
    virtual void i2l()             = 0;
    virtual void i2f()             = 0;
    virtual void i2d()             = 0;
    virtual void l2i()             = 0;
    virtual void l2f()             = 0;
    virtual void l2d()             = 0;
    virtual void f2i()             = 0;
    virtual void f2l()             = 0;
    virtual void f2d()             = 0;
    virtual void d2i()             = 0;
    virtual void d2l()             = 0;
    virtual void d2f()             = 0;
    virtual void i2b()             = 0;
    virtual void i2c()             = 0;
    virtual void i2s()             = 0;
};

class PrimitiveConversionInstruction final : public Instruction
{
public:
    PrimitiveConversionInstruction(type::PrimitiveTypePtr from, type::PrimitiveTypePtr to);
    const type::PrimitiveTypePtr& from() const noexcept { return from_; }
    const type::PrimitiveTypePtr& to() const noexcept { return to_; }
    void                          accept(PrimitiveConversion& conversion) const;
    InstructionKind               instructionKind() const noexcept override
    {
        return InstructionKind::PRIMITIVE_CONVERSION;
    }
    ca::i32 opcode() const noexcept override { return ExtensionOpcodes::PRIMITIVE_CONVERSION; }
    bool    equals(const CodeElement& other) const noexcept override;

private:
    type::PrimitiveTypePtr from_;
    type::PrimitiveTypePtr to_;
};

class VarInstruction final : public Instruction
{
public:
    VarInstruction(ca::i32 opcode, ca::i32 variableIndex) noexcept
        : opcode_(opcode)
        , variableIndex_(variableIndex)
    {}
    ca::i32         variableIndex() const noexcept { return variableIndex_; }
    InstructionKind instructionKind() const noexcept override { return InstructionKind::VARIABLE; }
    ca::i32         opcode() const noexcept override { return opcode_; }
    bool            equals(const CodeElement& other) const noexcept override;

private:
    ca::i32 opcode_;
    ca::i32 variableIndex_;
};

class VariableIncrementInstruction final : public Instruction
{
public:
    VariableIncrementInstruction(ca::i32 variableIndex, ca::i32 incrementBy) noexcept
        : variableIndex_(variableIndex)
        , incrementBy_(incrementBy)
    {}
    ca::i32         variableIndex() const noexcept { return variableIndex_; }
    ca::i32         incrementBy() const noexcept { return incrementBy_; }
    InstructionKind instructionKind() const noexcept override
    {
        return InstructionKind::VARIABLE_INCREMENT;
    }
    ca::i32 opcode() const noexcept override { return JavaOpcodes::IINC; }
    bool    equals(const CodeElement& other) const noexcept override;

private:
    ca::i32 variableIndex_;
    ca::i32 incrementBy_;
};

}   //namespace jblw::code

///@brief 与上游 `dev.xdark.blw.code.instruction` 对齐的命名空间视图。
namespace jblw::code::instruction {
using ::jblw::code::AllocateInstruction;
using ::jblw::code::AllocateMultiDimArrayInstruction;
using ::jblw::code::BranchInstruction;
using ::jblw::code::CheckCastInstruction;
using ::jblw::code::ConditionalJumpInstruction;
using ::jblw::code::ConstantInstruction;
using ::jblw::code::DoubleConstantInstruction;
using ::jblw::code::DynamicConstantInstruction;
using ::jblw::code::FieldInstruction;
using ::jblw::code::FloatConstantInstruction;
using ::jblw::code::ImmediateJumpInstruction;
using ::jblw::code::InstanceofInstruction;
using ::jblw::code::IntConstantInstruction;
using ::jblw::code::InvokeDynamicInstruction;
using ::jblw::code::LongConstantInstruction;
using ::jblw::code::LookupSwitchInstruction;
using ::jblw::code::MemberInstruction;
using ::jblw::code::MethodHandleConstantInstruction;
using ::jblw::code::MethodInstruction;
using ::jblw::code::PrimitiveConversion;
using ::jblw::code::PrimitiveConversionInstruction;
using ::jblw::code::SimpleInstruction;
using ::jblw::code::StringConstantInstruction;
using ::jblw::code::SwitchInstruction;
using ::jblw::code::TableSwitchInstruction;
using ::jblw::code::TypeConstantInstruction;
using ::jblw::code::VariableIncrementInstruction;
using ::jblw::code::VarInstruction;
}   //namespace jblw::code::instruction
