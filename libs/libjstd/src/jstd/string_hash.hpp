#pragma once

#include <string_view>

#include "libca/core/datatype.hpp"

namespace jstd {

///@brief Java 标准库字符串行为（C++ 侧对齐实现）。
namespace string_hash {

///@brief Java `String.hashCode`：`s[0]*31^(n-1) + ... + s[n-1]`，i32 回绕。
///
///哈希按 UTF-16 码元计（非 UTF-8 字节）：utf8 先解码为码点，超出 BMP 的码点
///拆为代理对（两个码元）后逐码元累加。输入必须是合法 UTF-8；非法字节按 Latin-1
///单字节处理（与 JVM 对 modified-UTF-8 的容错行为一致，调用方先自行校验）。
inline ca::i32 java_string_hash(const char* utf8, ca::usize len) noexcept
{
    ca::i32 h = 0;
    ca::usize   i = 0;
    while (i < len) {
        const ca::u32 b0 = static_cast<ca::u8>(utf8[i]);
        ca::u32 cp    = b0;
        ca::usize   adv   = 1;
        if (b0 >= 0xC0 && b0 < 0xE0 && i + 1 < len) {
            cp  = ((b0 & 0x1Fu) << 6) | (static_cast<ca::u8>(utf8[i + 1]) & 0x3Fu);
            adv = 2;
        }
        else if (b0 >= 0xE0 && b0 < 0xF0 && i + 2 < len) {
            cp  = ((b0 & 0x0Fu) << 12) | ((static_cast<ca::u8>(utf8[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<ca::u8>(utf8[i + 2]) & 0x3Fu);
            adv = 3;
        }
        else if (b0 >= 0xF0 && i + 3 < len) {
            cp = ((b0 & 0x07u) << 18) | ((static_cast<ca::u8>(utf8[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<ca::u8>(utf8[i + 2]) & 0x3Fu) << 6) |
                 (static_cast<ca::u8>(utf8[i + 3]) & 0x3Fu);
            adv = 4;
        }
        if (cp >= 0x10000) {
            //代理对：两个 UTF-16 码元。
            const ca::u32 v = cp - 0x10000;
            h               = h * 31 + static_cast<ca::i32>(0xD800 + (v >> 10));
            h               = h * 31 + static_cast<ca::i32>(0xDC00 + (v & 0x3FFu));
        }
        else {
            h = h * 31 + static_cast<ca::i32>(cp);
        }
        i += adv;
    }
    return h;
}

///@brief std::string_view 重载。
inline ca::i32 java_string_hash(std::string_view text) noexcept
{
    return java_string_hash(text.data(), text.size());
}

}   //namespace string_hash
}   //namespace jstd
