#pragma once

#include "of_double.hpp"
#include "of_dynamic.hpp"
#include "of_float.hpp"
#include "of_int.hpp"
#include "of_long.hpp"
#include "of_method_handle.hpp"
#include "of_string.hpp"
#include "of_type.hpp"

#include "libca/core/tag_cast.hpp"

namespace ca::core::tag_cast {

#define JBLW_CONSTANT_TYPE_OF(typeName, typeTag)                             \
    template<>                                                               \
    struct TypeOf<jblw::constant::typeName>                                  \
    {                                                                        \
        static constexpr auto value = jblw::constant::ConstantKind::typeTag; \
    }

JBLW_CONSTANT_TYPE_OF(OfString, STRING);
JBLW_CONSTANT_TYPE_OF(OfMethodHandle, METHOD_HANDLE);
JBLW_CONSTANT_TYPE_OF(OfType, TYPE);
JBLW_CONSTANT_TYPE_OF(OfDynamic, DYNAMIC);
JBLW_CONSTANT_TYPE_OF(OfLong, LONG);
JBLW_CONSTANT_TYPE_OF(OfDouble, DOUBLE);
JBLW_CONSTANT_TYPE_OF(OfInt, INT);
JBLW_CONSTANT_TYPE_OF(OfFloat, FLOAT);

#undef JBLW_CONSTANT_TYPE_OF

}   //namespace ca::core::tag_cast
