#pragma once

#include "type.hpp"

#include <memory>

namespace jblw::type {

///@brief JVM 字段或方法句柄，按字段结构进行值比较。
class MethodHandle final
{
public:
    ///@brief 构造不可变方法句柄。
    ///@throws std::invalid_argument owner 或 descriptor 为空时抛出。
    MethodHandle(ca::i32 kind, ObjectTypePtr owner, ca::str::Utf8String name, TypePtr descriptor,
                 bool isInterface);

    ca::i32                kind() const noexcept { return kind_; }
    const ObjectTypePtr&   owner() const noexcept { return owner_; }
    ca::str::Utf8StringRef name() const noexcept { return name_.ref(); }
    const TypePtr&         type() const noexcept { return descriptor_; }
    bool                   isInterface() const noexcept { return isInterface_; }

    bool                equals(const MethodHandle& other) const noexcept;
    ca::i32             hashCode() const noexcept { return hashCode_; }
    ca::str::Utf8String toString() const;

private:
    ca::i32             kind_;
    ObjectTypePtr       owner_;
    ca::str::Utf8String name_;
    TypePtr             descriptor_;
    bool                isInterface_;
    ca::i32             hashCode_;
};

using MethodHandlePtr = std::shared_ptr<const MethodHandle>;

bool operator==(const MethodHandle& lhs, const MethodHandle& rhs) noexcept;
bool operator!=(const MethodHandle& lhs, const MethodHandle& rhs) noexcept;

}   //namespace jblw::type
