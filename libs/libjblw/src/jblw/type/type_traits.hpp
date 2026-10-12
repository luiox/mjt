#pragma once

#include "array_type.hpp"
#include "instance_type.hpp"
#include "method_type.hpp"
#include "primitive_type.hpp"

#include "libca/core/tag_cast.hpp"

namespace ca::core::tag_cast {

template<>
struct TypeOf<jblw::type::PrimitiveType>
{
    static constexpr auto value = jblw::type::TypeKind::PRIMITIVE;
};

template<>
struct TypeOf<jblw::type::InstanceType>
{
    static constexpr auto value = jblw::type::TypeKind::INSTANCE;
};

template<>
struct TypeOf<jblw::type::ArrayType>
{
    static constexpr auto value = jblw::type::TypeKind::ARRAY;
};

template<>
struct TypeOf<jblw::type::MethodType>
{
    static constexpr auto value = jblw::type::TypeKind::METHOD;
};

}   //namespace ca::core::tag_cast
