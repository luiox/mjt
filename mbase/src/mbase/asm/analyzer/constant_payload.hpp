#pragma once

///@file constant_payload.hpp
///
///@brief 跨 value 域共享的常量载荷（`ca::core::Any`）工具。
///
///mbase 的多个 value 域（`ConstantValue`、`OriginalSourceConstantValue`、
///`CodeReferenceValue` 等）都用 `ca::core::Any` 持有 JVM 常量载荷，并各自
///重新实现了"两个 Any 是否承载同一个 JVM 常量"的相等比较。本头集中该
///逻辑，避免散落复制。
///
///注意：这些 value 域的 lattice 故意分开（见 `original_source_value.hpp`
///头注释说明）—— 本头只共享底层 Any 比较，不合并上层 value 类型。

#include <string>

#include "libca/core/any.hpp"

namespace mjt::base {

///@brief 判断两个 `Any` 是否承载相同的 JVM 常量值。
///
///两个载荷都为空时视为相等；任一为空、或持有的不兼容类型时返回 false。
///`std::int64_t` 与 `long long` 统一按 JVM long 数值比较；其余类型要求具体类型一致。
///覆盖 mbase 各 value 域实际使用的全部载荷类型：`int`、
///`std::int64_t`、`float`、`double`、`std::string`、`bool`、`char`、
///`short`（以及 `long long` —— 在某些平台上与 `std::int64_t` 不同，
///单独分支仅为可移植性）。
inline bool constantAnyEqual(const ca::core::Any& a, const ca::core::Any& b)
{
    if (a.has_value() != b.has_value()) return false;
    if (!a.has_value()) return true;

    if (auto* p = a.as<int>()) {
        auto* q = b.as<int>();
        return q && *p == *q;
    }
    auto getInt64 = [](const ca::core::Any& value, std::int64_t& out) {
        if (auto* p = value.as<std::int64_t>()) {
            out = *p;
            return true;
        }
        if (auto* p = value.as<long long>()) {
            out = static_cast<std::int64_t>(*p);
            return true;
        }
        return false;
    };
    std::int64_t int64A = 0;
    if (getInt64(a, int64A)) {
        std::int64_t int64B = 0;
        return getInt64(b, int64B) && int64A == int64B;
    }
    if (auto* p = a.as<float>()) {
        auto* q = b.as<float>();
        return q && *p == *q;
    }
    if (auto* p = a.as<double>()) {
        auto* q = b.as<double>();
        return q && *p == *q;
    }
    if (auto* p = a.as<std::string>()) {
        auto* q = b.as<std::string>();
        return q && *p == *q;
    }
    if (auto* p = a.as<bool>()) {
        auto* q = b.as<bool>();
        return q && *p == *q;
    }
    if (auto* p = a.as<char>()) {
        auto* q = b.as<char>();
        return q && *p == *q;
    }
    if (auto* p = a.as<short>()) {
        auto* q = b.as<short>();
        return q && *p == *q;
    }
    return false;
}

}   //namespace mjt::base
