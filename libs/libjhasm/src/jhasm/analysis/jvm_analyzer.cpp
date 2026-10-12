#include "jvm_analyzer.hpp"

#include <jbytecode/analysis/analyzer.hpp>
#include <jbytecode/analysis/simple_verifier.hpp>
#include <jbytecode/core/readers.hpp>
#include <jbytecode/tree/nodes.hpp>

#include <list>
#include <stdexcept>

namespace jhasm::analysis {
namespace {

ca::str::Utf16String diagnosticText(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

ca::str::Utf8String owning(ca::str::Utf8StringRef value)
{
    return value.substr(0, value.length());
}

Value snapshot(const jbytecode::analysis::BasicValue& value, ca::str::Utf8StringArena& scratch)
{
    const auto* type = value.getType();
    if (!type) return Value{};
    //staticArena 退役：OBJECT 排序描述符 intern 落点=调用方给的函数级局部
    //arena（帧快照结束即回收），字节立即深拷贝进 Value。
    return Value(owning(type->getDescriptor(scratch)));
}

Frame snapshot(const jbytecode::analysis::Frame<jbytecode::analysis::BasicValue>& source)
{
    ca::str::Utf8StringArena scratch;   //staticArena 退役：帧级局部，零驻留。
    Frame                    result;
    result.locals.reserve(static_cast<ca::usize>(source.getLocals()));
    result.stack.reserve(static_cast<ca::usize>(source.getStackSize()));
    for (ca::i32 index = 0; index < source.getLocals(); ++index)
        result.locals.push_back(snapshot(source.getLocal(index), scratch));
    for (ca::i32 index = 0; index < source.getStackSize(); ++index)
        result.stack.push_back(snapshot(source.getStack(index), scratch));
    return result;
}

class HierarchyVerifier final : public jbytecode::analysis::SimpleVerifier
{
public:
    HierarchyVerifier(const jbytecode::core::Type&              currentClass,
                      const jbytecode::core::Type&              currentSuperClass,
                      const std::vector<jbytecode::core::Type>& interfaces, bool isInterface,
                      const InheritanceChecker& inheritance)
        : SimpleVerifier(currentClass, currentSuperClass, interfaces, isInterface)
        , inheritance_(&inheritance)
        , currentClass_(currentClass)
        , currentSuperClass_(currentSuperClass)
        , interfaces_(interfaces)
    {}

    jbytecode::analysis::BasicValue unaryOperation(
        const jbytecode::tree::AbstractInsnNode* insn,
        const jbytecode::analysis::BasicValue&   value) override
    {
        const auto opcode = insn->getOpcode();
        if (isNull(value) && (opcode == jbytecode::core::Opcodes::ARRAYLENGTH ||
                              opcode == jbytecode::core::Opcodes::GETFIELD))
            throw jbytecode::analysis::AnalyzerException(insn,
                                                         "Cannot dereference known null value");
        return SimpleVerifier::unaryOperation(insn, value);
    }

    jbytecode::analysis::BasicValue binaryOperation(
        const jbytecode::tree::AbstractInsnNode* insn, const jbytecode::analysis::BasicValue& first,
        const jbytecode::analysis::BasicValue& second) override
    {
        const auto opcode = insn->getOpcode();
        if (isNull(first) && ((opcode >= jbytecode::core::Opcodes::IALOAD &&
                               opcode <= jbytecode::core::Opcodes::SALOAD) ||
                              opcode == jbytecode::core::Opcodes::PUTFIELD))
            throw jbytecode::analysis::AnalyzerException(insn,
                                                         "Cannot dereference known null value");
        return SimpleVerifier::binaryOperation(insn, first, second);
    }

    jbytecode::analysis::BasicValue ternaryOperation(
        const jbytecode::tree::AbstractInsnNode* insn, const jbytecode::analysis::BasicValue& first,
        const jbytecode::analysis::BasicValue& second,
        const jbytecode::analysis::BasicValue& third) override
    {
        if (isNull(first))
            throw jbytecode::analysis::AnalyzerException(insn,
                                                         "Cannot dereference known null value");
        return SimpleVerifier::ternaryOperation(insn, first, second, third);
    }

    jbytecode::analysis::BasicValue naryOperation(
        const jbytecode::tree::AbstractInsnNode*            insn,
        const std::vector<jbytecode::analysis::BasicValue>& values) override
    {
        const auto opcode = insn->getOpcode();
        if (opcode != jbytecode::core::Opcodes::INVOKESTATIC &&
            opcode != jbytecode::core::Opcodes::INVOKEDYNAMIC && !values.empty() &&
            isNull(values.front()))
            throw jbytecode::analysis::AnalyzerException(
                insn, "Cannot invoke method on known null value");
        return SimpleVerifier::naryOperation(insn, values);
    }

protected:
    bool isSubTypeOf(const jbytecode::analysis::BasicValue& value,
                     const jbytecode::analysis::BasicValue& expected) override
    {
        const auto* actualType   = value.getType();
        const auto* expectedType = expected.getType();
        if (!actualType || !expectedType) return !actualType && !expectedType;
        if (*actualType == *expectedType) return true;
        if (*actualType == jbytecode::analysis::BasicInterpreter::NULL_TYPE)
            return expectedType->getSort() == jbytecode::core::Type::OBJECT ||
                   expectedType->getSort() == jbytecode::core::Type::ARRAY;
        if (actualType->getSort() == jbytecode::core::Type::ARRAY ||
            expectedType->getSort() == jbytecode::core::Type::ARRAY)
            return SimpleVerifier::isSubTypeOf(value, expected);
        if (actualType->getSort() != jbytecode::core::Type::OBJECT ||
            expectedType->getSort() != jbytecode::core::Type::OBJECT)
            return false;
        return assignable(expectedType->getInternalName(typeArena_),
                          actualType->getInternalName(typeArena_));
    }

    jbytecode::analysis::BasicValue merge(const jbytecode::analysis::BasicValue& first,
                                          const jbytecode::analysis::BasicValue& second) override
    {
        const auto* firstType  = first.getType();
        const auto* secondType = second.getType();
        if (!firstType || !secondType || *firstType == *secondType)
            return SimpleVerifier::merge(first, second);
        const bool firstReference = firstType->getSort() == jbytecode::core::Type::OBJECT ||
                                    firstType->getSort() == jbytecode::core::Type::ARRAY;
        const bool secondReference = secondType->getSort() == jbytecode::core::Type::OBJECT ||
                                     secondType->getSort() == jbytecode::core::Type::ARRAY;
        if (firstReference != secondReference)
            throw jbytecode::analysis::AnalyzerException(
                "Cannot merge object and non-object values");
        if (!firstReference || firstType->getSort() != jbytecode::core::Type::OBJECT ||
            secondType->getSort() != jbytecode::core::Type::OBJECT)
            return SimpleVerifier::merge(first, second);
        if (isNull(first)) return second;
        if (isNull(second)) return first;
        auto common = inheritance_->commonSuperclass(firstType->getInternalName(typeArena_),
                                                     secondType->getInternalName(typeArena_));
        types_.push_back(
            jbytecode::core::Type::getObjectType(typeArena_.intern(common.ref())));
        return newValue(&types_.back());
    }

private:
    static bool isNull(const jbytecode::analysis::BasicValue& value)
    {
        return value.getType() &&
               *value.getType() == jbytecode::analysis::BasicInterpreter::NULL_TYPE;
    }

    //非 const：intern 落点 typeArena_ 会被写入（const 成员函数内无法绑定非 const 引用）。
    bool assignable(ca::str::Utf8StringRef expected, ca::str::Utf8StringRef actual)
    {
        if (expected == actual) return true;
        if (expected == ca::str::ZUtf8StringRef::from_static("java/lang/Object")) return true;
        //staticArena 退役：OBJECT 排序 getInternalName 返回 value_ 不 intern，
        //落点统一走本 verifier 私有 typeArena_。
        if (actual == currentClass_.getInternalName(typeArena_)) {
            if (currentSuperClass_.getSort() != jbytecode::core::Type::VOID &&
                assignable(expected, currentSuperClass_.getInternalName(typeArena_)))
                return true;
            for (const auto& type : interfaces_)
                if (assignable(expected, type.getInternalName(typeArena_))) return true;
        }
        return inheritance_->isSubclassOf(actual, expected);
    }

    const InheritanceChecker*          inheritance_;
    jbytecode::core::Type              currentClass_;
    jbytecode::core::Type              currentSuperClass_;
    std::vector<jbytecode::core::Type> interfaces_;
    std::list<jbytecode::core::Type>   types_;
    ca::str::Utf8StringArena           typeArena_;
};

}   //namespace

error::Result<JvmAnalysisResult> JvmAnalyzer::analyze(const std::vector<ca::u8>& classBytes) const
{
    JvmAnalysisResult         result;
    std::vector<error::Error> errors;
    try {
        ca::str::Utf8StringArena     arena;
        jbytecode::core::ClassReader reader(arena, classBytes);
        jbytecode::tree::ClassNode   clazz;
        reader.accept(&clazz, jbytecode::core::ClassReader::SKIP_FRAMES);

        const auto current = jbytecode::core::Type::getObjectType(clazz.name);
        const auto super   = clazz.superName.is_empty()
                                 ? jbytecode::core::Type{}
                                 : jbytecode::core::Type::getObjectType(clazz.superName);
        std::vector<jbytecode::core::Type> interfaces;
        interfaces.reserve(clazz.interfaces.size());
        for (auto name : clazz.interfaces)
            interfaces.push_back(jbytecode::core::Type::getObjectType(name));

        for (auto* method : clazz.methods) {
            MethodAnalysis methodResult{owning(method->name), owning(method->desc), {}};
            methodResult.frames.resize(method->instructions.size());
            try {
                HierarchyVerifier verifier(
                    current,
                    super,
                    interfaces,
                    (clazz.access & jbytecode::core::Opcodes::ACC_INTERFACE) != 0,
                    *inheritance_);
                jbytecode::analysis::Analyzer<jbytecode::analysis::BasicValue> analyzer(&verifier);
                const auto frames = analyzer.analyze(clazz.name, method);
                for (ca::usize index = 0; index < frames.size(); ++index)
                    if (frames[index]) methodResult.frames[index] = snapshot(*frames[index]);
            }
            catch (const std::exception& exception) {
                std::string message = "Method " + method->name.to_std_string() +
                                      method->desc.to_std_string() + ": " + exception.what();
                errors.emplace_back(diagnosticText(message).ref());
            }
            result.methods.push_back(std::move(methodResult));
        }
    }
    catch (const std::exception& exception) {
        errors.emplace_back(diagnosticText(exception.what()).ref());
    }
    return error::Result<JvmAnalysisResult>(std::move(result), std::move(errors), {});
}

}   //namespace jhasm::analysis
