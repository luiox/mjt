#include "code.hpp"

#include "generic/code_model.hpp"

#include <stdexcept>

namespace jblw::code {

CodeElementKind Instruction::getType() const noexcept
{
    switch (instructionKind()) {
    case InstructionKind::SIMPLE: return CodeElementKind::SIMPLE;
    case InstructionKind::CONSTANT_STRING: return CodeElementKind::CONSTANT_STRING;
    case InstructionKind::CONSTANT_METHOD_HANDLE: return CodeElementKind::CONSTANT_METHOD_HANDLE;
    case InstructionKind::CONSTANT_DYNAMIC: return CodeElementKind::CONSTANT_DYNAMIC;
    case InstructionKind::CONSTANT_LONG: return CodeElementKind::CONSTANT_LONG;
    case InstructionKind::CONSTANT_DOUBLE: return CodeElementKind::CONSTANT_DOUBLE;
    case InstructionKind::CONSTANT_INT: return CodeElementKind::CONSTANT_INT;
    case InstructionKind::CONSTANT_FLOAT: return CodeElementKind::CONSTANT_FLOAT;
    case InstructionKind::CONSTANT_TYPE: return CodeElementKind::CONSTANT_TYPE;
    case InstructionKind::ALLOCATE: return CodeElementKind::ALLOCATE;
    case InstructionKind::ALLOCATE_MULTI_ARRAY: return CodeElementKind::ALLOCATE_MULTI_ARRAY;
    case InstructionKind::CHECK_CAST: return CodeElementKind::CHECK_CAST;
    case InstructionKind::INSTANCE_OF: return CodeElementKind::INSTANCE_OF;
    case InstructionKind::CONDITIONAL_JUMP: return CodeElementKind::CONDITIONAL_JUMP;
    case InstructionKind::IMMEDIATE_JUMP: return CodeElementKind::IMMEDIATE_JUMP;
    case InstructionKind::LOOKUP_SWITCH: return CodeElementKind::LOOKUP_SWITCH;
    case InstructionKind::TABLE_SWITCH: return CodeElementKind::TABLE_SWITCH;
    case InstructionKind::FIELD: return CodeElementKind::FIELD;
    case InstructionKind::METHOD: return CodeElementKind::METHOD;
    case InstructionKind::INVOKE_DYNAMIC: return CodeElementKind::INVOKE_DYNAMIC;
    case InstructionKind::PRIMITIVE_CONVERSION: return CodeElementKind::PRIMITIVE_CONVERSION;
    case InstructionKind::VARIABLE: return CodeElementKind::VARIABLE;
    case InstructionKind::VARIABLE_INCREMENT: return CodeElementKind::VARIABLE_INCREMENT;
    }
    return CodeElementKind::SIMPLE;
}

ca::i32 Label::compareTo(const Label& other) const noexcept
{
    const ca::i32 lhs = getIndex();
    const ca::i32 rhs = other.getIndex();
    return lhs < rhs ? -1 : lhs > rhs ? 1 : 0;
}

bool TryCatchBlock::operator==(const TryCatchBlock& other) const noexcept
{
    auto labelEqual = [](const LabelPtr& lhs, const LabelPtr& rhs) {
        return lhs == rhs || (lhs && rhs && lhs->equals(*rhs));
    };
    return labelEqual(start, other.start) && labelEqual(end, other.end) &&
           labelEqual(handler, other.handler) &&
           (type == other.type || (type && other.type && type->equals(*other.type)));
}

LabelPtr Code::start() const
{
    for (const auto& element : elements()) {
        if (element && element->getType() == CodeElementKind::LABEL) {
            return std::static_pointer_cast<Label>(element);
        }
    }
    throw std::logic_error("Code does not contain a label");
}

bool operator==(const CodeElement& lhs, const CodeElement& rhs) noexcept
{
    return lhs.equals(rhs);
}

bool operator!=(const CodeElement& lhs, const CodeElement& rhs) noexcept
{
    return !lhs.equals(rhs);
}

}   //namespace jblw::code
