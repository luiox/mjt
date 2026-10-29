#pragma once

#include <cstddef>
#include <string>

#include "output_sink.hpp"

namespace mjt::json {

///@brief JSON 字符串转义（含非法 UTF-8 字节的兜底）。
///
///损坏/恶意 JAR 的常量池可含任意字节序列：无效 UTF-8 字节若原样透传会产出
///整篇非法 JSON。这里对有效的多字节序列原样保留，无效字节转义为 `\uFFFD`。
inline std::string escape(const std::string& value)
{
    static constexpr char hex[] = "0123456789abcdef";

    auto appendReplacement = [](std::string& out) {
        out += "\\ufffd";
    };

    std::string out;
    out.reserve(value.size() + 8);
    for (std::size_t i = 0; i < value.size();) {
        const unsigned char ch = static_cast<unsigned char>(value[i]);
        switch (ch) {
        case '\\': out += "\\\\"; ++i; continue;
        case '"': out += "\\\""; ++i; continue;
        case '\b': out += "\\b"; ++i; continue;
        case '\f': out += "\\f"; ++i; continue;
        case '\n': out += "\\n"; ++i; continue;
        case '\r': out += "\\r"; ++i; continue;
        case '\t': out += "\\t"; ++i; continue;
        default: break;
        }
        if (ch < 0x20) {
            out += "\\u00";
            out += hex[ch >> 4];
            out += hex[ch & 0x0f];
            ++i;
            continue;
        }
        if (ch < 0x80) {
            out += static_cast<char>(ch);
            ++i;
            continue;
        }
        //多字节序列：校验 UTF-8 lead/continuation 的组合与长度，有效则原样保留。
        std::size_t len   = 0;
        unsigned    cp    = 0;   //仅用于 overlong/代理区/越界校验，不用于输出
        bool        valid = false;
        if ((ch & 0xE0) == 0xC0) { len = 2; cp = ch & 0x1F; }
        else if ((ch & 0xF0) == 0xE0) { len = 3; cp = ch & 0x0F; }
        else if ((ch & 0xF8) == 0xF0) { len = 4; cp = ch & 0x07; }
        if (len != 0 && i + len <= value.size()) {
            valid = true;
            for (std::size_t k = 1; k < len; ++k) {
                const auto cc = static_cast<unsigned char>(value[i + k]);
                if ((cc & 0xC0) != 0x80) { valid = false; break; }
                cp = (cp << 6) | (cc & 0x3F);
            }
        }
        //overlong 编码 / 代理区 / 超 U+10FFFF 视为无效。
        if (valid) {
            if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) ||
                (len == 4 && cp < 0x10000) || cp > 0x10FFFF ||
                (cp >= 0xD800 && cp <= 0xDFFF)) {
                valid = false;
            }
        }
        if (valid) {
            out.append(value, i, len);
            i += len;
        }
        else {
            appendReplacement(out);
            ++i;
        }
    }
    return out;
}

///@brief 流式 JSON 数组 RAII 写出器。
///
///消除各 CLI 命令手工拼 `[\n` / `,\n` / `]\n` 的样板（xref 出现 5 处、jar list /
///classes / strings 各 1 处）。构造时输出 `[\n`，每次 `next` 在非首元素前输出
///`,\n` 分隔符，析构时输出 `\n]` 闭合。支持嵌套（外层未闭合时构造内层 writer，
///内层析构后外层继续 next 即可）。
///
///用法：
///@code
///{
///json::JsonArrayWriter arr(cli::out);
///for (auto& item : items) {
///arr.next;
///cli::out.printf(" {\"name\": \"%s\"}", item.c_str);
///}
///} //自动输出 '\n]'
///@endcode
class JsonArrayWriter
{
public:
    ///@param sink 输出目标（通常是 cli::out）。
    ///不标 noexcept：构造/析构中的 printf 走输出通道，抛异常时 noexcept 只会
    ///把可恢复错误升级成 terminate。
    explicit JsonArrayWriter(mjt::OutputSink& sink)
        : sink_(sink)
    {
        sink_.printf("[\n");
    }

    ~JsonArrayWriter()
    {
        sink_.printf("\n]");
    }

    JsonArrayWriter(const JsonArrayWriter&)            = delete;
    JsonArrayWriter& operator=(const JsonArrayWriter&) = delete;

    ///@brief 标记开始一个新元素，在非首元素前输出 `,\n` 分隔符。
    ///@return 输出 sink 的引用，便于调用方在分隔符后直接 printf 元素内容。
    mjt::OutputSink& next()
    {
        if (!first_) sink_.printf(",\n");
        first_ = false;
        return sink_;
    }

private:
    mjt::OutputSink& sink_;
    bool                            first_ = true;
};

}   //namespace mjt::json

namespace mjt::csv {

///@brief 按 RFC 4180 转义单个 CSV 字段。
///
///若字段含逗号、双引号、回车或换行，则整体用双引号包裹，内部双引号翻倍。
///否则原样返回（不额外加引号）。
inline std::string escape(const std::string& value)
{
    bool needQuote = false;
    for (char ch : value) {
        if (ch == ',' || ch == '"' || ch == '\r' || ch == '\n') {
            needQuote = true;
            break;
        }
    }
    if (!needQuote) return value;

    std::string out;
    out.reserve(value.size() + 8);
    out += '"';
    for (char ch : value) {
        if (ch == '"') out += "\"\"";
        else out += ch;
    }
    out += '"';
    return out;
}

}   //namespace mjt::csv
