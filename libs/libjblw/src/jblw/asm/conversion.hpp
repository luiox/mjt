#pragma once

#include "jblw/annotation/elements.hpp"
#include "jblw/constant/constant.hpp"
#include "jblw/type/constant_dynamic.hpp"

#include <jbytecode/core/primitives.hpp>
#include <jbytecode/core/type.hpp>

#include <libca/core/any.hpp>

namespace jblw::adapter {

type::MethodHandlePtr   wrapMethodHandle(const jbytecode::core::Handle& value);
jbytecode::core::Handle unwrapMethodHandle(const type::MethodHandle& value);

type::ConstantDynamicPtr         wrapConstantDynamic(const jbytecode::core::ConstantDynamic& value);
jbytecode::core::ConstantDynamic unwrapConstantDynamic(const type::ConstantDynamic& value);

constant::ConstantPtr wrapConstant(const ca::core::Any& value);
ca::core::Any         unwrapConstant(const constant::Constant& value);

}   //namespace jblw::adapter
