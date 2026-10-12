#pragma once

#include "class_type.hpp"

#include <vector>

namespace jblw::type {

///@brief JVM method descriptor 类型。
class MethodType final : public Type
{
public:
    ///@brief 构造 method 类型并持有 return/parameter 类型共享引用。
    ///@throws std::invalid_argument 任一类型为空时抛出。
    MethodType(ClassTypePtr returnType, std::vector<ClassTypePtr> parameterTypes);

    ///@brief 返回 return type 的共享引用。
    const ClassTypePtr& returnType() const noexcept { return returnType_; }

    ///@brief 返回按 descriptor 顺序排列的 parameter types。
    const std::vector<ClassTypePtr>& parameterTypes() const noexcept { return parameterTypes_; }

    ///@copydoc Type::descriptor
    ca::str::Utf8StringRef descriptor() const noexcept override { return descriptor_.ref(); }
    ///@copydoc Type::getType
    TypeKind getType() const noexcept override { return TypeKind::METHOD; }
    ///@copydoc Type::equals
    bool equals(const Type& other) const noexcept override;
    ///@copydoc Type::hashCode
    ca::i32 hashCode() const noexcept override { return hashCode_; }

private:
    ClassTypePtr              returnType_;
    std::vector<ClassTypePtr> parameterTypes_;
    ca::str::Utf8String       descriptor_;
    ca::i32                   hashCode_;
};

}   //namespace jblw::type
