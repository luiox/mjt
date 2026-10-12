#pragma once

#include "class_type.hpp"
#include "type_error.hpp"

#include <libca/core/result.hpp>

namespace jblw::type {

///@brief JVM primitive 类型值。实例通常取自 `Types` 的 singleton 常量。
class PrimitiveType final : public ClassType
{
public:
    ///@brief 构造 primitive 类型；字符串参数被复制为 owning string。
    PrimitiveType(ca::str::Utf8StringRef descriptor, ca::i32 kind, ca::str::Utf8StringRef name);

    ///@brief 返回 boxed instance 类型；void 返回错误。
    ca::Result<InstanceTypePtr, TypeError> box() const;

    ///@brief 按 BLW widening 表返回合并类型；void 返回错误。
    ca::Result<PrimitiveTypePtr, TypeError> widen(const PrimitiveType& other) const;

    ///@brief 返回 PrimitiveKind code。
    ca::i32 kind() const noexcept { return kind_; }

    ///@brief 返回 Java primitive 名称的非拥有视图。
    ca::str::Utf8StringRef name() const noexcept { return name_.ref(); }

    ///@copydoc Type::descriptor
    ca::str::Utf8StringRef descriptor() const noexcept override { return descriptor_.ref(); }
    ///@copydoc Type::getType
    TypeKind getType() const noexcept override { return TypeKind::PRIMITIVE; }
    ///@copydoc Type::equals
    bool equals(const Type& other) const noexcept override;
    ///@copydoc Type::hashCode
    ca::i32 hashCode() const noexcept override { return kind_; }

private:
    ca::str::Utf8String descriptor_;
    ca::i32             kind_;
    ca::str::Utf8String name_;
};

}   //namespace jblw::type
