#pragma once

#include "object_type.hpp"

namespace jblw::type {

///@brief JVM 数组类型，持有其直接组件类型。
class ArrayType final : public ObjectType
{
public:
    ///@brief 构造数组类型；组件 shared pointer 的生命周期由本对象延长。
    explicit ArrayType(ClassTypePtr componentType);

    ///@brief 返回数组维数。
    ca::i32 dimensions() const noexcept { return dimensions_; }

    ///@brief 返回直接组件类型的共享引用。
    const ClassTypePtr& componentType() const noexcept { return componentType_; }

    ///@brief 返回最内层非数组组件的 observer pointer。
    const ClassType* rootComponentType() const noexcept;

    ///@copydoc Type::descriptor
    ca::str::Utf8StringRef descriptor() const noexcept override { return descriptor_.ref(); }
    ///@copydoc ObjectType::internalName
    ca::str::Utf8StringRef internalName() const noexcept override { return descriptor_.ref(); }
    ///@copydoc ObjectType::externalName
    ca::str::Utf8StringRef externalName() const noexcept override { return externalName_.ref(); }
    ///@copydoc Type::getType
    TypeKind getType() const noexcept override { return TypeKind::ARRAY; }
    ///@copydoc Type::equals
    bool equals(const Type& other) const noexcept override;
    ///@copydoc Type::hashCode
    ca::i32 hashCode() const noexcept override { return hashCode_; }

private:
    ca::i32             dimensions_;
    ClassTypePtr        componentType_;
    ca::str::Utf8String descriptor_;
    ca::str::Utf8String externalName_;
    ca::i32             hashCode_;
};

}   //namespace jblw::type
