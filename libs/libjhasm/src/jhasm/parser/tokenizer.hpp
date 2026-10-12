#pragma once

#include "jhasm/error/result.hpp"

#include "token.hpp"

#include <libca/str/utf16_string.hpp>

#include <vector>

namespace jhasm::parser {

///@brief 将 Jasm UTF-16 源文本切分为拥有型 Token 和诊断。
///
///Tokenizer 本身不保存输入。`tokenize` 只在调用期间借用 source 与 input，返回的
///Token、错误和警告均拥有自己的文本，可安全跨越输入及 Tokenizer 生命周期。
class Tokenizer
{
public:
    ///@brief 判断 code unit 是否为 Jasm 结构操作符。
    static bool is_operator(ca::str::Char16 character) noexcept;

    ///@brief 判断 code unit 是否为 ASCII 十进制数字。
    static bool is_number(ca::str::Char16 character) noexcept;

    ///@brief 判断 code unit 是否为 ASCII 十六进制数字。
    static bool is_hex(ca::str::Char16 character) noexcept;

    ///@brief 判断 code unit 是否为上游支持的数字后缀。
    static bool is_number_suffix(ca::str::Char16 character) noexcept;

    ///@brief 判断 code unit 是否为十进制指数标记。
    static bool is_exponent(ca::str::Char16 character) noexcept;

    ///@brief 判断 code unit 是否可继续普通十进制数字。
    static bool is_number_continuation(ca::str::Char16 character) noexcept;

    ///@brief 切分 Jasm 源文本。
    ///@param source 诊断中使用的源名称，按 UTF-16 深拷贝到每个返回对象。
    ///@param input 待切分文本；只在本次调用期间借用。
    ///@return 始终带有 Token vector；遇到非法或未终止输入时同时携带 errors。
    error::Result<std::vector<Token>> tokenize(ca::str::Utf16StringRef source,
                                               ca::str::Utf16StringRef input) const;
};

}   //namespace jhasm::parser
