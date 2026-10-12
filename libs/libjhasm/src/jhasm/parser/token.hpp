#pragma once

#include "jhasm/util/location.hpp"
#include "jhasm/util/range.hpp"

#include "token_type.hpp"

#include <libca/str/utf16_string.hpp>

namespace jhasm::parser {

///@brief 表示一个拥有自身文本和源位置的 Jasm Token。
///
///content 和 Location 中的 source 均由 Token 深拷贝持有；tokenizer 返回后，
///即使输入视图或 Tokenizer 已失效，Token 仍可独立使用。range 端点与上游一致，
///表示原始输入中的 UTF-16 code unit 起点和结束游标。
class Token
{
public:
    ///@brief 深拷贝位置和内容并构造 Token。
    Token(util::Range range, const util::Location& location, TokenType type,
          ca::str::Utf16StringRef content);
    ///@brief 深拷贝另一 Token 及其全部文本。
    Token(const Token& other);
    ///@brief 深拷贝另一 Token 及其全部文本。
    Token& operator=(const Token& other);
    Token(Token&&) noexcept            = default;
    Token& operator=(Token&&) noexcept = default;

    ///@brief 返回原始输入中的 UTF-16 code unit 范围。
    const util::Range& range() const noexcept { return range_; }

    ///@brief 返回 1-based 源位置。
    const util::Location& location() const noexcept { return location_; }

    ///@brief 返回 Token 类别。
    TokenType type() const noexcept { return type_; }

    ///@brief 返回 Token 内容的非拥有 UTF-16 视图。
    ///@note 视图在当前 Token 被销毁、移动或赋值后失效。
    ca::str::Utf16StringRef content() const noexcept { return content_.ref(); }

private:
    util::Range          range_;
    util::Location       location_;
    TokenType            type_;
    ca::str::Utf16String content_;
};

}   //namespace jhasm::parser
