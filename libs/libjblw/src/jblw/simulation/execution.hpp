#pragma once

#include "jblw/classfile/model.hpp"
#include "jblw/code/instructions.hpp"

#include <stdexcept>

namespace jblw::simulation {

class SimulationException : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

class ExecutionEngine
{
public:
    virtual ~ExecutionEngine()             = default;
    virtual void label(code::Label& label) = 0;

    virtual void execute(const code::SimpleInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::ConstantInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::VarInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::LookupSwitchInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::TableSwitchInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::InstanceofInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::CheckCastInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::AllocateInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::AllocateMultiDimArrayInstruction& value)
    {
        executeInstruction(value);
    }
    virtual void execute(const code::MethodInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::FieldInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::InvokeDynamicInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::ImmediateJumpInstruction& value) { executeInstruction(value); }
    virtual void execute(const code::ConditionalJumpInstruction& value)
    {
        executeInstruction(value);
    }
    virtual void execute(const code::VariableIncrementInstruction& value)
    {
        executeInstruction(value);
    }
    virtual void execute(const code::PrimitiveConversionInstruction& value)
    {
        executeInstruction(value);
    }
    virtual void executeInstruction(const code::Instruction& instruction) = 0;
};

class ExecutionEngines final
{
public:
    static void execute(ExecutionEngine& engine, const code::Instruction& instruction);

    ExecutionEngines() = delete;
};

class StraightforwardSimulation final
{
public:
    void execute(ExecutionEngine& engine, const classfile::Method& method) const;
};

}   //namespace jblw::simulation
