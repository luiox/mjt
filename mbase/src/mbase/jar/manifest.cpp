#include "manifest.hpp"

#include <sstream>

namespace mjt::base {

static constexpr ca::usize kMaxLineLen = 72;

//解析 MANIFEST.MF：主属性段，支持续行（以空格开头）和 key: value 格式
Manifest Manifest::fromBytes(const std::vector<ca::u8>& bytes)
{
    Manifest           manifest;
    std::string        content(bytes.begin(), bytes.end());
    std::istringstream stream(content);
    std::string        line, pendingKey, pendingValue;

    //将当前累积的 key:value 刷入 manifest（setMainAttribute：既有 key 原位
    //覆写保插入序，语义对齐旧 unordered_map 的 operator[] 赋值）。
    auto flush = [&]() {
        if (!pendingKey.empty()) {
            manifest.setMainAttribute(pendingKey, pendingValue);
            pendingKey.clear();
            pendingValue.clear();
        }
    };

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            flush();
            break;
        }   //空行 = 主属性段结束
        if (line[0] == ' ') {   //续行：追加到上一个 value
            pendingValue += line.substr(1);
            continue;
        }
        auto colon = line.find(':');   //key: value 分割
        if (colon != std::string::npos) {
            flush();   //先刷出上一个条目
            pendingKey   = line.substr(0, colon);
            pendingValue = (colon + 1 < line.size() && line[colon + 1] == ' ')
                               ? line.substr(colon + 2)
                               : line.substr(colon + 1);
        }
    }
    flush();
    return manifest;
}

std::vector<ca::u8> Manifest::toBytes() const
{
    //JAR-4：折行截断点必须落在 UTF-8 字符起始字节上，否则会把多字节字符切成两半
    //（截断后无法原样解析回）。从给定字节位置回退：连续字节（0b10xxxxxx）前移
    //到所属字符的起始字节；ASCII 与字符起始字节均不会被回退。
    const auto charStart = [](const std::string& s, ca::usize pos) -> ca::usize {
        while (pos > 0 && pos < s.size() && (static_cast<ca::u8>(s[pos]) & 0xC0) == 0x80) --pos;
        return pos;
    };

    std::ostringstream oss;
    for (const auto& [key, value] : mainAttributes_) {
        std::string line = key + ": " + value;
        if (line.size() <= kMaxLineLen) {
            oss << line << "\r\n";
        }
        else {
            //Wrap long lines: first line at most 72 bytes, continuation lines prefixed with
            //space（含前缀后不超过 72 字节）。截断点回退到 UTF-8 字符起始字节（JAR-4）。
            ca::usize cut = charStart(line, kMaxLineLen);
            oss << line.substr(0, cut) << "\r\n";
            ca::usize pos = cut;
            while (pos < line.size()) {
                ca::usize end = charStart(line, std::min(line.size(), pos + (kMaxLineLen - 1)));
                //前进保障：整窗都是续字节时 charStart 会回退到 pos 甚至越过 pos
                //（长串连续 0x80-0xBF 的敌意输入），end==pos 将使循环永不前进。
                //此时放弃回退按字节硬切，至少推进 1 字节。
                if (end <= pos) end = std::min(line.size(), pos + 1);
                oss << " " << line.substr(pos, end - pos) << "\r\n";
                pos = end;
            }
        }
    }
    oss << "\r\n";
    auto str = oss.str();
    return std::vector<ca::u8>(str.begin(), str.end());
}

}   //namespace mjt::base
