#pragma once

#include "object_type.hpp"

namespace jblw::type {

///@brief 普通 JVM class/interface 类型。
class InstanceType final : public ObjectType
{
public:
    ///@brief 从 descriptor 和 internal name 构造不可变实例类型。
    ///@note 两个参数都会被移动到对象内部，返回视图由本对象拥有。
    InstanceType(ca::str::Utf8String descriptor, ca::str::Utf8String internalName);

    ///@copydoc Type::descriptor
    ca::str::Utf8StringRef descriptor() const noexcept override { return descriptor_.ref(); }
    ///@copydoc ObjectType::internalName
    ca::str::Utf8StringRef internalName() const noexcept override { return internalName_.ref(); }
    ///@copydoc ObjectType::externalName
    ca::str::Utf8StringRef externalName() const noexcept override { return externalName_.ref(); }
    ///@copydoc Type::getType
    TypeKind getType() const noexcept override { return TypeKind::INSTANCE; }
    ///@copydoc Type::equals
    bool equals(const Type& other) const noexcept override;
    ///@copydoc Type::hashCode
    ca::i32 hashCode() const noexcept override { return hashCode_; }

private:
    ca::str::Utf8String descriptor_;
    ca::str::Utf8String internalName_;
    ca::str::Utf8String externalName_;
    ca::i32             hashCode_;
};

}   //namespace jblw::type
