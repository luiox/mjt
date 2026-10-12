#pragma once

#include <string>
#include <vector>

#include "jbytecode/analysis/basic_interpreter.hpp"

namespace mjt::base {

class AliasAnalysis;

struct ReflectionExecutionTarget
{
    enum class Kind
    {
        Method,
        Constructor,
        Field
    };

    Kind        kind = Kind::Method;
    std::string owner;
    std::string name;
    std::string desc;
    std::string reflectionArgDesc;
};

std::string reflectionArgumentDescriptor(const jbytecode::tree::MethodInsnNode& lookupInsn);

struct ReflectionValue
{
    enum class Kind
    {
        Unknown,
        String,
        Class,
        Method,
        Constructor,
        Field
    };

    jbytecode::analysis::BasicValue basic;
    Kind                            kind = Kind::Unknown;
    std::string                     owner;
    std::string                     name;
    std::string                     desc;
    std::string                     reflectionArgDesc;

    int  getSize() const { return basic.getSize(); }
    bool operator==(const ReflectionValue& other) const
    {
        return basic == other.basic && kind == other.kind && owner == other.owner &&
               name == other.name && desc == other.desc &&
               reflectionArgDesc == other.reflectionArgDesc;
    }
};

bool resolveReflectionExecution(const jbytecode::tree::MethodInsnNode& method,
                                const ReflectionValue& receiver, ReflectionExecutionTarget& target);

///@brief Method-local abstract interpreter for reflection object aliases.
///
///The value domain intentionally tracks only constants needed by reflection:
///class literals, member names, and resolved Method/Constructor/Field objects.
///All other values remain unknown; merge is fail-closed when two paths disagree.
class ReflectionInterpreter final : public jbytecode::analysis::Interpreter<ReflectionValue>
{
public:
    explicit ReflectionInterpreter(const AliasAnalysis* aliases = nullptr)
        : aliases_(aliases)
    {}

    ReflectionValue newValue(const jbytecode::core::Type* type) override;
    ReflectionValue newOperation(const jbytecode::tree::AbstractInsnNode* insn) override;
    ReflectionValue copyOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                  const ReflectionValue&                   value) override;
    ReflectionValue unaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                   const ReflectionValue&                   value) override;
    ReflectionValue binaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                    const ReflectionValue&                   value1,
                                    const ReflectionValue&                   value2) override;
    ReflectionValue ternaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                     const ReflectionValue& value1, const ReflectionValue& value2,
                                     const ReflectionValue& value3) override;
    ReflectionValue naryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                  const std::vector<ReflectionValue>&      values) override;
    void            returnOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                    const ReflectionValue& value, const ReflectionValue& expected) override;
    ReflectionValue merge(const ReflectionValue& value1, const ReflectionValue& value2) override;

private:
    jbytecode::analysis::BasicInterpreter basic_;
    const AliasAnalysis*                  aliases_ = nullptr;
};

}   //namespace mjt::base
