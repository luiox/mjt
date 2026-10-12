#pragma once

#include "annotation.hpp"
#include "elements.hpp"

#include "libca/core/tag_cast.hpp"

namespace ca::core::tag_cast {

#define JBLW_ELEMENT_TYPE_OF(typeName, typeTag)                               \
    template<>                                                                \
    struct TypeOf<jblw::annotation::typeName>                                 \
    {                                                                         \
        static constexpr auto value = jblw::annotation::ElementKind::typeTag; \
    }

JBLW_ELEMENT_TYPE_OF(Annotation, ANNOTATION);
JBLW_ELEMENT_TYPE_OF(ElementArray, ARRAY);
JBLW_ELEMENT_TYPE_OF(ElementBoolean, BOOLEAN);
JBLW_ELEMENT_TYPE_OF(ElementByte, BYTE);
JBLW_ELEMENT_TYPE_OF(ElementChar, CHAR);
JBLW_ELEMENT_TYPE_OF(ElementDouble, DOUBLE);
JBLW_ELEMENT_TYPE_OF(ElementEnum, ENUM);
JBLW_ELEMENT_TYPE_OF(ElementFloat, FLOAT);
JBLW_ELEMENT_TYPE_OF(ElementInt, INT);
JBLW_ELEMENT_TYPE_OF(ElementLong, LONG);
JBLW_ELEMENT_TYPE_OF(ElementShort, SHORT);
JBLW_ELEMENT_TYPE_OF(ElementString, STRING);
JBLW_ELEMENT_TYPE_OF(ElementType, TYPE);

#undef JBLW_ELEMENT_TYPE_OF

}   //namespace ca::core::tag_cast
