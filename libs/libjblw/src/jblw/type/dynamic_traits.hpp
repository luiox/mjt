#pragma once

#include "constant_dynamic.hpp"
#include "invoke_dynamic.hpp"

#include "libca/core/tag_cast.hpp"

namespace ca::core::tag_cast {

template<>
struct TypeOf<jblw::type::ConstantDynamic>
{
    static constexpr auto value = jblw::type::DynamicKind::CONSTANT;
};

template<>
struct TypeOf<jblw::type::InvokeDynamic>
{
    static constexpr auto value = jblw::type::DynamicKind::INVOKE;
};

}   //namespace ca::core::tag_cast
