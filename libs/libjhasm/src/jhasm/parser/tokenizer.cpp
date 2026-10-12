#include "tokenizer.hpp"

#include "jhasm/error/error_collector.hpp"

#include <libca/core/datatype.hpp>

#include <regex>
#include <string>
#include <utility>

namespace jhasm::parser {
namespace {

using ca::str::Char16;
using ca::str::Utf16StringRef;

enum class State
{
    Normal,
    String,
    Character,
    LineComment,
    BlockComment,
};

bool is_java_whitespace(ca::u16 unit) noexcept
{
    if ((unit >= 0x0009 && unit <= 0x000D) || (unit >= 0x001C && unit <= 0x0020)) {
        return true;
    }
    if (unit == 0x1680 || unit == 0x2028 || unit == 0x2029 || unit == 0x205F || unit == 0x3000) {
        return true;
    }
    return unit >= 0x2000 && unit <= 0x200A && unit != 0x2007;
}

ca::i32 hex_value(ca::u16 unit) noexcept
{
    if (unit >= '0' && unit <= '9') return static_cast<ca::i32>(unit - '0');
    if (unit >= 'a' && unit <= 'f') return static_cast<ca::i32>(unit - 'a' + 10);
    if (unit >= 'A' && unit <= 'F') return static_cast<ca::i32>(unit - 'A' + 10);
    return -1;
}

bool is_number_text(Utf16StringRef content)
{
    std::string ascii;
    ascii.reserve(content.length());
    for (ca::usize index = 0; index < content.length(); ++index) {
        const ca::u16 unit = content.char_at(index).unit();
        if (unit > 0x7F) return false;
        ascii.push_back(static_cast<char>(unit));
    }

    static const std::regex number_pattern(
        R"(-?(?:(?:(?:[0-9][0-9_]*\.(?:[0-9][0-9_]*)?(?:[eE]-?[0-9][0-9_]*)?|\.[0-9][0-9_]*(?:[eE]-?[0-9][0-9_]*)?|[0-9][0-9_]*[eE]-?[0-9][0-9_]*|0[xX][0-9A-Fa-f_]*(?:\.[0-9A-Fa-f_]*)?[pP]-?[0-9][0-9_]*|0[bB][01]+)[fFdD]?)|(?:(?:0[xX][0-9A-Fa-f_]+|[0-9][0-9_]*)[LlFfDd]?)))");
    return std::regex_match(ascii, number_pattern);
}

TokenType classify(Utf16StringRef content)
{
    if (content.length() == 1 && Tokenizer::is_operator(content.char_at(0))) {
        return TokenType::Operator;
    }
    return is_number_text(content) ? TokenType::Number : TokenType::Identifier;
}

class TokenizerContext
{
public:
    TokenizerContext(Utf16StringRef source, Utf16StringRef input)
        : source_(source)
        , input_(input)
    {}

    error::Result<std::vector<Token>> run()
    {
        while (!at_end()) {
            switch (state_) {
            case State::Normal: handle_normal(); break;
            case State::String: handle_quoted('"', TokenType::String, "Unterminated string"); break;
            case State::Character:
                handle_quoted('\'', TokenType::Character, "Unterminated character");
                break;
            case State::LineComment: handle_line_comment(); break;
            case State::BlockComment: handle_block_comment(); break;
            }
        }
        finish_at_end();
        return error::Result<std::vector<Token>>(
            std::move(tokens_), errors_.take_errors(), errors_.take_warnings());
    }

private:
    bool at_end() const noexcept { return index_ >= input_.length(); }

    ca::u16 current() const noexcept { return input_.char_at(index_).unit(); }

    bool has_next() const noexcept { return index_ + 1 < input_.length(); }

    ca::u16 next_unit() const noexcept { return input_.char_at(index_ + 1).unit(); }

    void advance()
    {
        const ca::u16 unit = current();
        ++index_;
        if (unit == '\n') {
            ++line_;
            column_ = 1;
        }
        else {
            ++column_;
        }
    }

    void begin_token()
    {
        buffer_.clear();
        token_start_        = index_;
        token_start_line_   = line_;
        token_start_column_ = column_;
        token_active_       = true;
    }

    void append_current()
    {
        buffer_.append(Char16(current()));
        advance();
    }

    util::Location location(ca::i32 line, ca::i32 column, ca::i32 length) const
    {
        return util::Location(line, column, length, source_);
    }

    void add_error(const char* message, ca::i32 line, ca::i32 column, ca::i32 length)
    {
        auto text = ca::str::Utf16String::value_of(message);
        errors_.add_error(text.ref(), location(line, column, length));
    }

    void collect(TokenType type, ca::usize end)
    {
        if (!token_active_) return;
        auto content = buffer_.build();
        tokens_.emplace_back(
            util::Range(static_cast<ca::i32>(token_start_), static_cast<ca::i32>(end)),
            location(
                token_start_line_, token_start_column_, static_cast<ca::i32>(content.length())),
            type,
            content.ref());
        buffer_.clear();
        token_active_ = false;
    }

    void collect_normal()
    {
        if (!token_active_) return;
        auto       content = buffer_.build();
        const auto type    = classify(content.ref());
        tokens_.emplace_back(
            util::Range(static_cast<ca::i32>(token_start_), static_cast<ca::i32>(index_)),
            location(
                token_start_line_, token_start_column_, static_cast<ca::i32>(content.length())),
            type,
            content.ref());
        buffer_.clear();
        token_active_ = false;
    }

    void handle_normal()
    {
        const ca::u16 unit = current();
        if (is_java_whitespace(unit)) {
            collect_normal();
            advance();
            return;
        }
        if (Tokenizer::is_operator(Char16(unit))) {
            collect_normal();
            begin_token();
            append_current();
            collect(TokenType::Operator, index_);
            return;
        }
        if (unit == '/' && has_next() && (next_unit() == '/' || next_unit() == '*')) {
            collect_normal();
            const ca::u16 delimiter = next_unit();
            advance();
            advance();
            begin_token();
            state_ = delimiter == '/' ? State::LineComment : State::BlockComment;
            return;
        }
        if (unit == '"' || unit == '\'') {
            collect_normal();
            const State quoted_state = unit == '"' ? State::String : State::Character;
            advance();
            begin_token();
            state_ = quoted_state;
            return;
        }
        if (!token_active_) begin_token();
        append_current();
    }

    void handle_line_comment()
    {
        while (!at_end() && current() != '\n') append_current();
        collect(TokenType::Comment, index_);
        state_ = State::Normal;
        if (!at_end()) advance();
    }

    void handle_block_comment()
    {
        while (!at_end()) {
            if (current() == '*' && has_next() && next_unit() == '/') {
                collect(TokenType::Comment, index_);
                advance();
                advance();
                state_ = State::Normal;
                return;
            }
            if (current() == '\\') {
                process_escape();
            }
            else {
                append_current();
            }
        }
    }

    void handle_quoted(ca::u16 delimiter, TokenType type, const char* unterminated_message)
    {
        const ca::u16 unit = current();
        if (unit == delimiter) {
            collect(type, index_);
            advance();
            state_ = State::Normal;
            return;
        }
        if (unit == '\n') {
            collect(type, index_);
            add_error(unterminated_message,
                      token_start_line_,
                      token_start_column_,
                      static_cast<ca::i32>(index_ - token_start_));
            state_ = State::Normal;
            advance();
            return;
        }
        if (unit == '\\') {
            process_escape();
            return;
        }
        append_current();
    }

    void process_escape()
    {
        const ca::i32 escape_line   = line_;
        const ca::i32 escape_column = column_;
        advance();
        if (at_end()) {
            buffer_.append(Char16('\\'));
            add_error("Incomplete escape sequence", escape_line, escape_column, 1);
            return;
        }

        const ca::u16 escaped = current();
        switch (escaped) {
        case 'n':
            buffer_.append(Char16('\n'));
            advance();
            break;
        case 'r':
            buffer_.append(Char16('\r'));
            advance();
            break;
        case 't':
            buffer_.append(Char16('\t'));
            advance();
            break;
        case 'b':
            buffer_.append(Char16('\b'));
            advance();
            break;
        case 'f':
            buffer_.append(Char16('\f'));
            advance();
            break;
        case '"':
            buffer_.append(Char16('"'));
            advance();
            break;
        case '\'':
            buffer_.append(Char16('\''));
            advance();
            break;
        case '\\':
            buffer_.append(Char16('\\'));
            advance();
            break;
        case 'u': process_unicode_escape(escape_line, escape_column); break;
        default:
            buffer_.append(Char16('\\'));
            advance();
            break;
        }
    }

    void process_unicode_escape(ca::i32 escape_line, ca::i32 escape_column)
    {
        advance();
        if (input_.length() - index_ < 4) {
            buffer_.append(Char16('\\'));
            buffer_.append(Char16('u'));
            add_error("Truncated unicode escape", escape_line, escape_column, 2);
            return;
        }

        ca::u16 value = 0;
        for (ca::usize digit = 0; digit < 4; ++digit) {
            const ca::i32 decoded = hex_value(input_.char_at(index_ + digit).unit());
            if (decoded < 0) {
                buffer_.append(Char16('\\'));
                buffer_.append(Char16('u'));
                add_error("Invalid unicode escape", escape_line, escape_column, 6);
                return;
            }
            value = static_cast<ca::u16>((value << 4) | static_cast<ca::u16>(decoded));
        }
        buffer_.append(Char16(value));
        for (ca::usize digit = 0; digit < 4; ++digit) advance();
    }

    void finish_at_end()
    {
        switch (state_) {
        case State::Normal: collect_normal(); break;
        case State::String:
            collect(TokenType::String, index_);
            add_error("Unterminated string",
                      token_start_line_,
                      token_start_column_,
                      static_cast<ca::i32>(index_ - token_start_));
            break;
        case State::Character:
            collect(TokenType::Character, index_);
            add_error("Unterminated character",
                      token_start_line_,
                      token_start_column_,
                      static_cast<ca::i32>(index_ - token_start_));
            break;
        case State::LineComment: collect(TokenType::Comment, index_); break;
        case State::BlockComment:
            collect(TokenType::Comment, index_);
            add_error("Unterminated multiline comment",
                      token_start_line_,
                      token_start_column_,
                      static_cast<ca::i32>(index_ - token_start_));
            break;
        }
    }

    Utf16StringRef              source_;
    Utf16StringRef              input_;
    ca::usize                   index_              = 0;
    ca::i32                     line_               = 1;
    ca::i32                     column_             = 1;
    State                       state_              = State::Normal;
    bool                        token_active_       = false;
    ca::usize                   token_start_        = 0;
    ca::i32                     token_start_line_   = 1;
    ca::i32                     token_start_column_ = 1;
    ca::str::Utf16StringBuilder buffer_;
    error::ErrorCollector       errors_;
    std::vector<Token>          tokens_;
};

}   //namespace

bool Tokenizer::is_operator(Char16 character) noexcept
{
    const ca::u16 unit = character.unit();
    return unit == '{' || unit == '}' || unit == ':' || unit == ',';
}

bool Tokenizer::is_number(Char16 character) noexcept
{
    const ca::u16 unit = character.unit();
    return unit >= '0' && unit <= '9';
}

bool Tokenizer::is_hex(Char16 character) noexcept
{
    const ca::u16 unit = character.unit();
    return is_number(character) || (unit >= 'a' && unit <= 'f') || (unit >= 'A' && unit <= 'F');
}

bool Tokenizer::is_number_suffix(Char16 character) noexcept
{
    const ca::u16 unit = character.unit();
    return unit == 'f' || unit == 'F' || unit == 'l' || unit == 'L' || unit == 'd' || unit == 'D';
}

bool Tokenizer::is_exponent(Char16 character) noexcept
{
    const ca::u16 unit = character.unit();
    return unit == 'E' || unit == 'e';
}

bool Tokenizer::is_number_continuation(Char16 character) noexcept
{
    return character.unit() == '.' || is_exponent(character);
}

error::Result<std::vector<Token>> Tokenizer::tokenize(Utf16StringRef source,
                                                      Utf16StringRef input) const
{
    return TokenizerContext(source, input).run();
}

}   //namespace jhasm::parser
