#pragma once

/// @file cesu8.hpp
/// @brief CESU-8/WTF-8 → 标准 UTF-8 消毒（MCP 工具参数解码前的宽容层）。

#include <string>

namespace mjt::mcp {

/// @brief 重组 CESU-8 代理对、替换孤立/截断代理，其余字节原样通过。
///
/// JS/UTF-16 宿主按码元序列化，增补平面字符以高低代理各 3 字节送达
/// （高代理 ED A0..AF、低代理 ED B0..BF），nlohmann 严格校验整体拒收该
/// 参数对象。成对高低代理重组为 4 字节标准 UTF-8；孤立/截断代理替换为
/// U+FFFD；ED 80..9F 开头的是合法 3 字节 UTF-8（U+0800..U+0FFF），原样
/// 通过——本函数只处理代理区间，不越权改写其他非法字节。
inline std::string sanitizeCesu8Text(const std::string& raw)
{
    auto byteAt = [&raw](std::size_t index) { return static_cast<unsigned char>(raw[index]); };
    auto isCont = [&raw, &byteAt](std::size_t index) {
        return index < raw.size() && byteAt(index) >= 0x80 && byteAt(index) <= 0xBF;
    };
    std::string out;
    out.reserve(raw.size());
    std::size_t i = 0;
    while (i < raw.size()) {
        if (byteAt(i) == 0xED && isCont(i + 1)) {
            const auto b1    = byteAt(i + 1);
            const bool high  = b1 >= 0xA0 && b1 <= 0xAF;
            const bool low   = b1 >= 0xB0 && b1 <= 0xBF;
            const bool whole = isCont(i + 2);
            if (high && whole && i + 5 < raw.size() && byteAt(i + 3) == 0xED &&
                byteAt(i + 4) >= 0xB0 && byteAt(i + 4) <= 0xBF && isCont(i + 5)) {
                const unsigned short hi = static_cast<unsigned short>(
                    0xD000u + (static_cast<unsigned short>(b1 & 0x3F) << 6) +
                    static_cast<unsigned short>(byteAt(i + 2) & 0x3F));
                const unsigned short lo = static_cast<unsigned short>(
                    0xD000u + (static_cast<unsigned short>(byteAt(i + 4) & 0x3F) << 6) +
                    static_cast<unsigned short>(byteAt(i + 5) & 0x3F));
                const unsigned int codepoint =
                    0x10000u + ((static_cast<unsigned int>(hi - 0xD800u) << 10) |
                                static_cast<unsigned int>(lo - 0xDC00u));
                out += static_cast<char>(0xF0 | (codepoint >> 18));
                out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
                i += 6;
                continue;
            }
            if (high || low) {
                //孤立/截断代理 → U+FFFD；截断时只消耗已到手的字节。
                out += "\xEF\xBF\xBD";
                i += whole ? 3 : 2;
                continue;
            }
        }
        out += raw[i];
        ++i;
    }
    return out;
}

}   //namespace mjt::mcp
