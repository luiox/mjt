#pragma once

#include "entries.hpp"

#include "libca/core/tag_cast.hpp"

namespace ca::core::tag_cast {

#define JBLW_ENTRY_TYPE_OF(typeName, typeTag)                                 \
    template<>                                                                \
    struct TypeOf<jblw::constantpool::typeName>                               \
    {                                                                         \
        static constexpr auto value = jblw::constantpool::EntryKind::typeTag; \
    }

JBLW_ENTRY_TYPE_OF(EntryUtf8, UTF8);
JBLW_ENTRY_TYPE_OF(EntryInteger, INTEGER);
JBLW_ENTRY_TYPE_OF(EntryFloat, FLOAT);
JBLW_ENTRY_TYPE_OF(EntryLong, LONG);
JBLW_ENTRY_TYPE_OF(EntryDouble, DOUBLE);
JBLW_ENTRY_TYPE_OF(EntryClass, CLASS);
JBLW_ENTRY_TYPE_OF(EntryString, STRING);
JBLW_ENTRY_TYPE_OF(EntryFieldRef, FIELD_REF);
JBLW_ENTRY_TYPE_OF(EntryMethodRef, METHOD_REF);
JBLW_ENTRY_TYPE_OF(EntryInterfaceMethodRef, INTERFACE_METHOD_REF);
JBLW_ENTRY_TYPE_OF(EntryNameAndType, NAME_AND_TYPE);
JBLW_ENTRY_TYPE_OF(EntryMethodHandle, METHOD_HANDLE);
JBLW_ENTRY_TYPE_OF(EntryMethodType, METHOD_TYPE);
JBLW_ENTRY_TYPE_OF(EntryDynamic, DYNAMIC);
JBLW_ENTRY_TYPE_OF(EntryInvokeDynamic, INVOKE_DYNAMIC);
JBLW_ENTRY_TYPE_OF(EntryModule, MODULE);
JBLW_ENTRY_TYPE_OF(EntryPackage, PACKAGE);

#undef JBLW_ENTRY_TYPE_OF

}   //namespace ca::core::tag_cast
