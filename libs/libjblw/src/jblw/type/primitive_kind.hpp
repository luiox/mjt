#pragma once

#include <libca/core/datatype.hpp>

namespace jblw::type {

///@brief JVM primitive array type codes，包含 BLW 扩展的 void code。
struct PrimitiveKind final
{
    static constexpr ca::i32 T_BOOLEAN = 4;
    static constexpr ca::i32 T_CHAR    = 5;
    static constexpr ca::i32 T_FLOAT   = 6;
    static constexpr ca::i32 T_DOUBLE  = 7;
    static constexpr ca::i32 T_BYTE    = 8;
    static constexpr ca::i32 T_SHORT   = 9;
    static constexpr ca::i32 T_INT     = 10;
    static constexpr ca::i32 T_LONG    = 11;
    static constexpr ca::i32 T_VOID    = 12;

    PrimitiveKind() = delete;
};

}   //namespace jblw::type
