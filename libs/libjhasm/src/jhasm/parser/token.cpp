#include "token.hpp"

namespace jhasm::parser {

Token::Token(util::Range range, const util::Location& location, TokenType type,
             ca::str::Utf16StringRef content)
    : range_(range)
    , location_(location)
    , type_(type)
    , content_(content.to_string())
{}

Token::Token(const Token& other)
    : range_(other.range_)
    , location_(other.location_)
    , type_(other.type_)
    , content_(other.content_.clone())
{}

Token& Token::operator=(const Token& other)
{
    if (this == &other) return *this;
    range_    = other.range_;
    location_ = other.location_;
    type_     = other.type_;
    content_  = other.content_.clone();
    return *this;
}

}   //namespace jhasm::parser
