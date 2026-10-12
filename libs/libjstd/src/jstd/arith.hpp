#pragma once

#include "libca/core/datatype.hpp"

namespace jstd {

///@brief Java 32/64 位整数运算语义（C++ 侧唯一权威实现）。
///
///对齐 JVM 行为（`doc/design` 各分析文档）：有符号整数**溢出回绕**（C++ 有符号
///溢出是 UB，这里用无符号位运算规避）、除法**向零截断**、取模**符号跟随被除数**
///（C++11 起与 Java 一致）、移位**掩码取模**（`& 0x1F` / `& 0x3F`）、
///窄化转换（i2b/i2c/i2s/l2i 取低位位模式）。
///
///契约：`div`/`rem` 的除数必须非零（Java 抛 ArithmeticException，C++ 侧由
///调用方保证）。
namespace arith {

///@name int32 语义
///@{

///溢出回绕加法（INT_MAX+1 == INT_MIN）。
inline ca::i32 add(ca::i32 a, ca::i32 b) noexcept
{
    return static_cast<ca::i32>(static_cast<ca::u32>(a) + static_cast<ca::u32>(b));
}

///溢出回绕减法（INT_MIN-1 == INT_MAX）。
inline ca::i32 sub(ca::i32 a, ca::i32 b) noexcept
{
    return static_cast<ca::i32>(static_cast<ca::u32>(a) - static_cast<ca::u32>(b));
}

///溢出回绕乘法。
inline ca::i32 mul(ca::i32 a, ca::i32 b) noexcept
{
    return static_cast<ca::i32>(static_cast<ca::u32>(a) * static_cast<ca::u32>(b));
}

///向零截断除法；INT_MIN / -1 == INT_MIN（回绕）。
inline ca::i32 div(ca::i32 a, ca::i32 b) noexcept
{
    if (a == INT32_MIN && b == -1) return INT32_MIN;
    return a / b;
}

///取模（符号跟随被除数）；INT_MIN % -1 == 0（Java 语义：分子为 0）。
inline ca::i32 rem(ca::i32 a, ca::i32 b) noexcept
{
    if (a == INT32_MIN && b == -1) return 0;
    return a % b;
}

///单目取负（回绕：INT_MIN -> INT_MIN）。
inline ca::i32 neg(ca::i32 a) noexcept
{
    return static_cast<ca::i32>(0U - static_cast<ca::u32>(a));
}

///`Math.abs`：INT_MIN -> INT_MIN（Java 不回绕）。
inline ca::i32 abs(ca::i32 a) noexcept
{
    return a == INT32_MIN ? INT32_MIN : (a < 0 ? -a : a);
}

///`<<`：位移量取模 32。
inline ca::i32 shl(ca::i32 a, int n) noexcept
{
    return static_cast<ca::i32>(static_cast<ca::u32>(a) << (n & 31));
}

///`>>`：算术右移（符号扩展；目标平台一致实现）。
inline ca::i32 shr(ca::i32 a, int n) noexcept
{
    return a >> (n & 31);
}

///`>>>`：逻辑右移（高位补 0）。
inline ca::i32 ushr(ca::i32 a, int n) noexcept
{
    return static_cast<ca::i32>(static_cast<ca::u32>(a) >> (n & 31));
}

///@}

///@name int64 语义
///@{

inline ca::i64 add(ca::i64 a, ca::i64 b) noexcept
{
    return static_cast<ca::i64>(static_cast<ca::u64>(a) + static_cast<ca::u64>(b));
}

inline ca::i64 sub(ca::i64 a, ca::i64 b) noexcept
{
    return static_cast<ca::i64>(static_cast<ca::u64>(a) - static_cast<ca::u64>(b));
}

inline ca::i64 mul(ca::i64 a, ca::i64 b) noexcept
{
    return static_cast<ca::i64>(static_cast<ca::u64>(a) * static_cast<ca::u64>(b));
}

///向零截断除法；INT64_MIN / -1 == INT64_MIN（回绕）。
inline ca::i64 div(ca::i64 a, ca::i64 b) noexcept
{
    if (a == INT64_MIN && b == -1) return INT64_MIN;
    return a / b;
}

///取模（符号跟随被除数）；INT64_MIN % -1 == 0。
inline ca::i64 rem(ca::i64 a, ca::i64 b) noexcept
{
    if (a == INT64_MIN && b == -1) return 0;
    return a % b;
}

inline ca::i64 neg(ca::i64 a) noexcept
{
    return static_cast<ca::i64>(0ULL - static_cast<ca::u64>(a));
}

inline ca::i64 abs(ca::i64 a) noexcept
{
    return a == INT64_MIN ? INT64_MIN : (a < 0 ? -a : a);
}

///`<<`：位移量取模 64。
inline ca::i64 shl(ca::i64 a, int n) noexcept
{
    return static_cast<ca::i64>(static_cast<ca::u64>(a) << (n & 63));
}

///`>>`：算术右移（符号扩展）。
inline ca::i64 shr(ca::i64 a, int n) noexcept
{
    return a >> (n & 63);
}

///`>>>`：逻辑右移。
inline ca::i64 ushr(ca::i64 a, int n) noexcept
{
    return static_cast<ca::i64>(static_cast<ca::u64>(a) >> (n & 63));
}

///@}

///@name 窄化转换（取低位位模式）
///@{

///`i2b`：(byte) a，低 8 位按有符号解释。
inline ca::i8 i2b(ca::i32 a) noexcept
{
    return static_cast<ca::i8>(static_cast<ca::u8>(a));
}

///`i2c`：(char) a，低 16 位无符号（0..65535）。
inline ca::u16 i2c(ca::i32 a) noexcept
{
    return static_cast<ca::u16>(a);
}

///`i2s`：(short) a，低 16 位按有符号解释。
inline ca::i16 i2s(ca::i32 a) noexcept
{
    return static_cast<ca::i16>(static_cast<ca::u16>(a));
}

///`l2i`：(int) a，低 32 位。
inline ca::i32 l2i(ca::i64 a) noexcept
{
    return static_cast<ca::i32>(static_cast<ca::u32>(a));
}

///@}

}   //namespace arith

}   //namespace jstd
