#pragma once

#include "jbytecode/jbytecode.hpp"

#include <list>

namespace mjt::base {

class SuperInterpreter : public jbytecode::analysis::BasicInterpreter
{
public:
    ///引用值按 BasicVerifier 语义保留具体类型供 merge/equals 区分;但 BasicValue 只存
    ///const Type*，而调用方(Interpreter::newParameterValue/newExceptionValue 默认实现
    ///转发的 &type)是栈上临时对象，别名会在值进 Frame 后悬垂(Type::operator== 解引用
    ///即越界读)。此处必须拷贝接管所有权;拷贝不损语义，Type::operator== 按 sort+描述符
    ///内容比较(Utf8StringRef::equals 为 memcmp)。同 SimpleVerifier::internType 惯例。
    jbytecode::analysis::BasicValue newValue(const jbytecode::core::Type* type) override
    {
        if (type != nullptr && (type->getSort() == jbytecode::core::Type::OBJECT ||
                                type->getSort() == jbytecode::core::Type::ARRAY)) {
            typeCache_.emplace_back(*type);
            return jbytecode::analysis::BasicValue(&typeCache_.back());
        }
        return jbytecode::analysis::BasicInterpreter::newValue(type);
    }

    jbytecode::analysis::BasicValue merge(const jbytecode::analysis::BasicValue& a,
                                          const jbytecode::analysis::BasicValue& b) override
    {
        if (a.equals(b)) return a;
        if (a.isReference() && b.isReference())
            return jbytecode::analysis::BasicValue::REFERENCE_VALUE;
        return jbytecode::analysis::BasicValue::UNINITIALIZED_VALUE;
    }

private:
    ///接管的 Type 副本，只增不删;list 节点地址稳定，push_back 不搬移既有元素，
    ///已发出的 BasicValue 持有的指针在本解释器生存期内有效。死亡点约束同
    ///SimpleVerifier:分析帧(BasicValue)不得活过本对象。
    std::list<jbytecode::core::Type> typeCache_;
};

}   //namespace mjt::base
