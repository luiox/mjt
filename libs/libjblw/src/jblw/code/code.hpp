#pragma once

#include "jblw/type/instance_type.hpp"

#include <libca/core/datatype.hpp>

#include <memory>
#include <vector>

namespace jblw::code {

class CodeElementList;

namespace attribute {
class Local;
using LocalPtr = std::shared_ptr<const Local>;
}   //namespace attribute

enum class CodeElementKind : ca::u8
{
    LABEL,
    SIMPLE,
    CONSTANT_STRING,
    CONSTANT_METHOD_HANDLE,
    CONSTANT_DYNAMIC,
    CONSTANT_LONG,
    CONSTANT_DOUBLE,
    CONSTANT_INT,
    CONSTANT_FLOAT,
    CONSTANT_TYPE,
    ALLOCATE,
    ALLOCATE_MULTI_ARRAY,
    CHECK_CAST,
    INSTANCE_OF,
    CONDITIONAL_JUMP,
    IMMEDIATE_JUMP,
    LOOKUP_SWITCH,
    TABLE_SWITCH,
    FIELD,
    METHOD,
    INVOKE_DYNAMIC,
    PRIMITIVE_CONVERSION,
    VARIABLE,
    VARIABLE_INCREMENT,
};

enum class InstructionKind : ca::u8
{
    SIMPLE,
    CONSTANT_STRING,
    CONSTANT_METHOD_HANDLE,
    CONSTANT_DYNAMIC,
    CONSTANT_LONG,
    CONSTANT_DOUBLE,
    CONSTANT_INT,
    CONSTANT_FLOAT,
    CONSTANT_TYPE,
    ALLOCATE,
    ALLOCATE_MULTI_ARRAY,
    CHECK_CAST,
    INSTANCE_OF,
    CONDITIONAL_JUMP,
    IMMEDIATE_JUMP,
    LOOKUP_SWITCH,
    TABLE_SWITCH,
    FIELD,
    METHOD,
    INVOKE_DYNAMIC,
    PRIMITIVE_CONVERSION,
    VARIABLE,
    VARIABLE_INCREMENT,
};

///@brief code 元素的 RTTI-free 根接口。
class CodeElement
{
public:
    virtual ~CodeElement()                                                  = default;
    virtual CodeElementKind getType() const noexcept                        = 0;
    virtual bool            equals(const CodeElement& other) const noexcept = 0;

protected:
    CodeElement() = default;
};

using CodeElementPtr = std::shared_ptr<CodeElement>;

///@brief BLW 规范化指令根接口。
class Instruction : public CodeElement
{
public:
    CodeElementKind         getType() const noexcept final;
    virtual InstructionKind instructionKind() const noexcept = 0;
    virtual ca::i32         opcode() const noexcept          = 0;

protected:
    Instruction() = default;
};

using InstructionPtr = std::shared_ptr<Instruction>;

///@brief 可在 code 元素表中共享并被重编号的 label。
class Label : public CodeElement
{
public:
    static constexpr ca::i32 UNSET = -1;

    CodeElementKind getType() const noexcept final { return CodeElementKind::LABEL; }
    virtual ca::i32 getIndex() const noexcept                  = 0;
    virtual void    setIndex(ca::i32 index) noexcept           = 0;
    virtual ca::i32 getLineNumber() const noexcept             = 0;
    virtual void    setLineNumber(ca::i32 lineNumber) noexcept = 0;

    ca::i32 compareTo(const Label& other) const noexcept;

protected:
    Label() = default;
};

using LabelPtr = std::shared_ptr<Label>;

///@brief JVM exception handler 范围；空 type 表示 finally/catch-all。
struct TryCatchBlock final
{
    LabelPtr              start;
    LabelPtr              end;
    LabelPtr              handler;
    type::InstanceTypePtr type;

    bool operator==(const TryCatchBlock& other) const noexcept;
};

///@brief 对 code 元素序列进行可跳转遍历的游标。
class CodeWalker
{
public:
    virtual ~CodeWalker()                         = default;
    virtual ca::i32      index() const noexcept   = 0;
    virtual CodeElement* element() const noexcept = 0;
    virtual void         advance() noexcept       = 0;
    virtual void         set(const Label& label)  = 0;
};

///@brief 方法 Code 属性的可变、拥有型模型接口。
class Code
{
public:
    virtual ~Code()                                                                 = default;
    virtual ca::i32                                 maxStack() const noexcept       = 0;
    virtual ca::i32                                 maxLocals() const noexcept      = 0;
    virtual std::unique_ptr<CodeWalker>             walker()                        = 0;
    virtual CodeElementList&                        elements() noexcept             = 0;
    virtual const CodeElementList&                  elements() const noexcept       = 0;
    virtual const std::vector<TryCatchBlock>&       tryCatchBlocks() const noexcept = 0;
    virtual const std::vector<attribute::LocalPtr>& localVariables() const noexcept = 0;

    LabelPtr start() const;

protected:
    Code() = default;
};

using CodePtr = std::shared_ptr<Code>;

bool operator==(const CodeElement& lhs, const CodeElement& rhs) noexcept;
bool operator!=(const CodeElement& lhs, const CodeElement& rhs) noexcept;

}   //namespace jblw::code
