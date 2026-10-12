#pragma once

#include <libca/core/datatype.hpp>
#include <libca/str/utf8_string.hpp>

#include <memory>

namespace jblw::type {

///@brief 无 RTTI 类型层次的运行时标签。
enum class TypeKind : ca::u8
{
    PRIMITIVE,
    INSTANCE,
    ARRAY,
    METHOD,
};

class Type;
class ClassType;
class ObjectType;
class PrimitiveType;
class InstanceType;
class ArrayType;
class MethodType;

using TypePtr          = std::shared_ptr<const Type>;
using ClassTypePtr     = std::shared_ptr<const ClassType>;
using ObjectTypePtr    = std::shared_ptr<const ObjectType>;
using PrimitiveTypePtr = std::shared_ptr<const PrimitiveType>;
using InstanceTypePtr  = std::shared_ptr<const InstanceType>;
using ArrayTypePtr     = std::shared_ptr<const ArrayType>;
using MethodTypePtr    = std::shared_ptr<const MethodType>;

///@brief BLW 类型根接口。
///
///类型实例不可变并由 shared pointer 共享；descriptor 返回的视图在类型实例销毁后
///失效。
class Type
{
public:
    virtual ~Type() = default;

    ///@brief 返回 JVM descriptor 的非拥有视图。
    virtual ca::str::Utf8StringRef descriptor() const noexcept = 0;

    ///@brief 返回用于 `ca::core::tag_cast::cast` 的运行时标签。
    virtual TypeKind getType() const noexcept = 0;

    ///@brief 按 BLW 值语义比较类型。
    virtual bool equals(const Type& other) const noexcept = 0;

    ///@brief 返回与 equals 一致的 Java 风格 32-bit hash。
    virtual ca::i32 hashCode() const noexcept = 0;

protected:
    Type() = default;
};

///@brief 比较两个类型的结构值。
bool operator==(const Type& lhs, const Type& rhs) noexcept;

///@brief 比较两个类型的结构值是否不同。
bool operator!=(const Type& lhs, const Type& rhs) noexcept;

}   //namespace jblw::type
