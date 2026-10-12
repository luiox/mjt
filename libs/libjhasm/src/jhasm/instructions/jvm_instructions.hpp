#pragma once

#include "jhasm/ast/primitive.hpp"
#include "jhasm/error/error_collector.hpp"

#include <vector>

namespace jhasm::instructions {

enum class OperandKind : ca::u8
{
    INTEGER,
    LITERAL,
    LABEL,
    TYPE,
    NEW_ARRAY_TYPE,
    CONSTANT,
    FIELD_DESCRIPTOR,
    METHOD_DESCRIPTOR,
    HANDLE,
    ARGS,
    TABLE_SWITCH,
    LOOKUP_SWITCH,
};

struct InstructionSpec final
{
    const char*              name;
    std::vector<OperandKind> operands;
};

///@brief HotSpot/JVM Jasm 指令签名与 operand 校验表。
class JvmInstructions final
{
public:
    static const JvmInstructions& instance();
    const InstructionSpec*        find(ca::str::Utf16StringRef name) const noexcept;
    bool verify(const ast::Instruction& instruction, error::ErrorCollector& errors) const;
    const std::vector<InstructionSpec>& all() const noexcept { return instructions_; }

private:
    JvmInstructions();
    std::vector<InstructionSpec> instructions_;
};

bool isFieldDescriptor(ca::str::Utf16StringRef value) noexcept;
bool isMethodDescriptor(ca::str::Utf16StringRef value) noexcept;

}   //namespace jhasm::instructions
