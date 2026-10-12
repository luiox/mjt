#include "mbase/util/class_name_util.hpp"

#include <cstdio>

namespace mjt::base {

namespace {

//转义后最长显示长度，超出则右截断为 198 + ".."（见设计文档 §4.3）。
//正常 class 名远短于此，只有恶意混淆的超长名才会触发。
constexpr ca::usize kMaxClassNameDisplay = 200;

constexpr ca::u32 kBmpMax = 0xFFFF;

inline bool isAllowedClassNameChar(ca::u32 cp) noexcept
{
    //白名单：A-Za-z0-9_ $ / 及 .（entry 名里的 .class 后缀、META-INF/MANIFEST.MF 等）。
    return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') ||
           cp == '_' || cp == '$' || cp == '/' || cp == '.';
}

}   //namespace

std::string escapeClassName(ca::str::Utf8StringRef name)
{
    std::string out;
    out.reserve(name.byte_length());

    //用 Utf8Iterator range-for：O(1) 步进，整体 O(n)。
    //禁止 code_point_at(i) 下标遍历（单次 O(n)，循环 O(n²)）。
    for (ca::u32 cp : name) {
        if (isAllowedClassNameChar(cp)) {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp <= kBmpMax) {
            char buf[7];
            std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(cp));
            out += buf;
        }
        else {
            //补充平面码点（class 名里正常不出现，兜底）。
            char buf[11];
            std::snprintf(buf, sizeof(buf), "\\U%08x", static_cast<unsigned>(cp));
            out += buf;
        }
    }

    //仅超长才截断，正常 class 名不受影响。
    if (out.size() > kMaxClassNameDisplay) {
        out.resize(kMaxClassNameDisplay - 2);
        out += "..";
    }
    return out;
}

}   //namespace mjt::base
