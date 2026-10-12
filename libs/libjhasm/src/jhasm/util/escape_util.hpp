#pragma once

#include <libca/core/datatype.hpp>
#include <libca/core/result.hpp>
#include <libca/str/utf16_string.hpp>

namespace jhasm::util {

///@brief 描述反转义失败的位置和类别。
struct UnescapeError
{
    ///@brief 错误类别。
    enum class Code
    {
        TrailingBackslash,
        TruncatedUnicodeEscape,
        InvalidUnicodeEscape,
    };

    Code      code;     ///< 错误类别。
    ca::usize offset;   ///< 错误转义在输入中的 UTF-16 code unit 偏移。
};

///@brief 提供与 Jasm EscapeUtil 对齐的 UTF-16 转义操作。
///
///所有输入均为调用期有效的非拥有视图，返回值或输出 builder 拥有生成的 code unit。
class EscapeUtil
{
public:
    ///@brief 按 Jasm 字符串规则转义并返回拥有型 UTF-16 文本。
    static ca::str::Utf16String escape_string(ca::str::Utf16StringRef string);

    ///@brief 按 Jasm identifier literal 规则转义并返回拥有型 UTF-16 文本。
    static ca::str::Utf16String escape_literal(ca::str::Utf16StringRef string);

    ///@brief 将 Jasm 转义文本追加到调用方提供的 builder。
    ///@param escaped 待反转义的 UTF-16 文本。
    ///@param output 接收结果的 builder；失败时保留错误位置之前的部分结果。
    ///@return 成功返回 Ok；尾随反斜杠或非法 `\\uXXXX` 返回错误及 code unit 偏移。
    static ca::Result<void, UnescapeError> unescape(ca::str::Utf16StringRef      escaped,
                                                    ca::str::Utf16StringBuilder& output);

private:
    static ca::str::Utf16String escape(ca::str::Utf16StringRef string, bool literal);
};

}   //namespace jhasm::util
