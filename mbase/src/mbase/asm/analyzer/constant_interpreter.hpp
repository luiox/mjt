#pragma once

#include "libca/core/any.hpp"
#include <vector>


#include "jbytecode/jbytecode.hpp"

#include "constant_value.hpp"
#include "super_interpreter.hpp"

namespace mjt::base {

//JVM 名称（owner/name/desc）统一以 ca::str::Utf8StringRef 传递（按值，非拥有 view）。
//来源通常是 tree node 字段（fin->owner 等），本就是 Utf8StringRef，无需经 std::string 转换。
class IConstantReferenceHandler
{
public:
    virtual ~IConstantReferenceHandler()                                                  = default;
    virtual ca::core::Any getFieldValueOrNull(const jbytecode::analysis::BasicValue& v,
                                              ca::str::Utf8StringRef                 owner,
                                              ca::str::Utf8StringRef                 name,
                                              ca::str::Utf8StringRef                 desc)                = 0;
    virtual ca::core::Any getMethodReturnOrNull(const jbytecode::analysis::BasicValue& v,
                                                ca::str::Utf8StringRef                 owner,
                                                ca::str::Utf8StringRef                 name,
                                                ca::str::Utf8StringRef                 desc,
                                                const std::vector<ConstantValue>&      values) = 0;
};

class BasicReferenceHandler : public IConstantReferenceHandler
{
public:
    ca::core::Any getFieldValueOrNull(const jbytecode::analysis::BasicValue& v,
                                      ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                      ca::str::Utf8StringRef desc) override
    {
        (void)v;
        (void)owner;
        (void)name;
        (void)desc;
        return ca::core::Any{};
    }

    ca::core::Any getMethodReturnOrNull(const jbytecode::analysis::BasicValue& v,
                                        ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                        ca::str::Utf8StringRef            desc,
                                        const std::vector<ConstantValue>& values) override
    {
        (void)v;
        (void)owner;
        (void)name;
        (void)desc;
        (void)values;
        return ca::core::Any{};
    }
};

namespace detail {

template<typename Vec, typename T>
inline int arrayLength(const ca::core::Any& arr)
{
    auto* p = arr.as<const Vec>();
    return p ? static_cast<int>(p->size()) : -1;
}

template<typename Vec, typename T>
inline ca::core::Any arrayGet(const ca::core::Any& arr, int index)
{
    auto* p = arr.as<const Vec>();
    return p ? static_cast<T>((*p)[index]) : ca::core::Any{};
}

template<typename Vec, typename T>
inline void arraySet(ca::core::Any& arr, int index, T value)
{
    auto* p = arr.as<Vec>();
    if (p) (*p)[index] = value;
}

template<typename Vec>
inline bool arrayTypeCheck(const ca::core::Any& arr)
{
    return arr.as<const Vec>() != nullptr;
}

}   //namespace detail

///@brief 在 JVM tree 指令上执行保守常量解释，供常量折叠和数据流 pass 复用。
///
///只在操作数类型和常量值都可证明时产生已知结果；除零、类型冲突、未知引用或不支持的
///opcode 返回 unknown，不猜测结果。整数和长整数加减乘遵循 JVM 补码环绕，`MIN / -1`
///与余数遵循 JVM 特例，移位距离分别按低 5/6 位屏蔽。分析结果借用调用方提供的引用
///handler；解释器不拥有 IR 节点和 handler，非线程安全。
class ConstantInterpreter : public jbytecode::analysis::Interpreter<ConstantValue>
{
public:
    explicit ConstantInterpreter(IConstantReferenceHandler* referenceHandler);

    ConstantInterpreter(IConstantReferenceHandler* referenceHandler, bool isStatic,
                        int localVariables, ca::str::Utf8StringRef descr,
                        const std::vector<ca::core::Any>& args);

    ConstantValue newOperation(const jbytecode::tree::AbstractInsnNode* insn) override;

    ConstantValue copyOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                const ConstantValue&                     value) override;

    ConstantValue newValue(const jbytecode::core::Type* type) override;

    ConstantValue unaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                 const ConstantValue&                     value) override;

    ConstantValue binaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                  const ConstantValue& a, const ConstantValue& b) override;

    ConstantValue ternaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                   const ConstantValue& a, const ConstantValue& b,
                                   const ConstantValue& c) override;

    ConstantValue naryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                const std::vector<ConstantValue>&        values) override;

    void returnOperation(const jbytecode::tree::AbstractInsnNode* insn, const ConstantValue& value,
                         const ConstantValue& expected) override;

    ConstantValue merge(const ConstantValue& a, const ConstantValue& b) override;

private:
    SuperInterpreter                   basic_;
    IConstantReferenceHandler*         referenceHandler_ = nullptr;
    std::vector<ca::core::Any>         args_;
    std::vector<jbytecode::core::Type> desc_;

    static ca::core::Any newArray(int arrayType, int size);
    static int           getArrayLength(const ca::core::Any& arr);
    static ca::core::Any getArrayElement(const ca::core::Any& arr, int index);
    static ca::core::Any getArrayElementAsNumber(const ca::core::Any& arr, int index);
    static void          setArrayElement(const ca::core::Any& arr, int opcode, int index,
                                         const ca::core::Any& value);
    static ca::core::Any getUnaryValue(int opcode, const ca::core::Any& value);
    static ca::core::Any getBinaryValue(int opcode, const ca::core::Any& a, const ca::core::Any& b);
    static int           toInt(const ca::core::Any& v);
    static double        toDouble(const ca::core::Any& v);
};

}   //namespace mjt::base
