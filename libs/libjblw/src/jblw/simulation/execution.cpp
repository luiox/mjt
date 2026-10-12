#include "execution.hpp"

namespace jblw::simulation {

void ExecutionEngines::execute(ExecutionEngine& engine, const code::Instruction& instruction)
{
    switch (instruction.instructionKind()) {
    case code::InstructionKind::SIMPLE:
        engine.execute(static_cast<const code::SimpleInstruction&>(instruction));
        break;
    case code::InstructionKind::CONSTANT_STRING:
    case code::InstructionKind::CONSTANT_METHOD_HANDLE:
    case code::InstructionKind::CONSTANT_DYNAMIC:
    case code::InstructionKind::CONSTANT_LONG:
    case code::InstructionKind::CONSTANT_DOUBLE:
    case code::InstructionKind::CONSTANT_INT:
    case code::InstructionKind::CONSTANT_FLOAT:
    case code::InstructionKind::CONSTANT_TYPE:
        engine.execute(static_cast<const code::ConstantInstruction&>(instruction));
        break;
    case code::InstructionKind::ALLOCATE:
        engine.execute(static_cast<const code::AllocateInstruction&>(instruction));
        break;
    case code::InstructionKind::ALLOCATE_MULTI_ARRAY:
        engine.execute(static_cast<const code::AllocateMultiDimArrayInstruction&>(instruction));
        break;
    case code::InstructionKind::CHECK_CAST:
        engine.execute(static_cast<const code::CheckCastInstruction&>(instruction));
        break;
    case code::InstructionKind::INSTANCE_OF:
        engine.execute(static_cast<const code::InstanceofInstruction&>(instruction));
        break;
    case code::InstructionKind::CONDITIONAL_JUMP:
        engine.execute(static_cast<const code::ConditionalJumpInstruction&>(instruction));
        break;
    case code::InstructionKind::IMMEDIATE_JUMP:
        engine.execute(static_cast<const code::ImmediateJumpInstruction&>(instruction));
        break;
    case code::InstructionKind::LOOKUP_SWITCH:
        engine.execute(static_cast<const code::LookupSwitchInstruction&>(instruction));
        break;
    case code::InstructionKind::TABLE_SWITCH:
        engine.execute(static_cast<const code::TableSwitchInstruction&>(instruction));
        break;
    case code::InstructionKind::FIELD:
        engine.execute(static_cast<const code::FieldInstruction&>(instruction));
        break;
    case code::InstructionKind::METHOD:
        engine.execute(static_cast<const code::MethodInstruction&>(instruction));
        break;
    case code::InstructionKind::INVOKE_DYNAMIC:
        engine.execute(static_cast<const code::InvokeDynamicInstruction&>(instruction));
        break;
    case code::InstructionKind::PRIMITIVE_CONVERSION:
        engine.execute(static_cast<const code::PrimitiveConversionInstruction&>(instruction));
        break;
    case code::InstructionKind::VARIABLE:
        engine.execute(static_cast<const code::VarInstruction&>(instruction));
        break;
    case code::InstructionKind::VARIABLE_INCREMENT:
        engine.execute(static_cast<const code::VariableIncrementInstruction&>(instruction));
        break;
    }
}

void StraightforwardSimulation::execute(ExecutionEngine&         engine,
                                        const classfile::Method& method) const
{
    const code::CodePtr& code = method.code();
    if (!code) throw SimulationException("Method code must not be null");
    std::unique_ptr<code::CodeWalker> walker = code->walker();
    while (true) {
        walker->advance();
        code::CodeElement* element = walker->element();
        if (!element) return;
        if (element->getType() == code::CodeElementKind::LABEL) {
            engine.label(static_cast<code::Label&>(*element));
        }
        else {
            ExecutionEngines::execute(engine, static_cast<const code::Instruction&>(*element));
        }
    }
}

}   //namespace jblw::simulation
