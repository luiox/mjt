#include "declaration_parser.hpp"

#include "jhasm/error/error_collector.hpp"

#include <libca/str/utf16_string.hpp>

#include <utility>

namespace jhasm::parser {
namespace {
using namespace ast;

bool equals(ca::str::Utf16StringRef value, const char* ascii) noexcept
{
    ca::usize index = 0;
    for (; ascii[index] != '\0'; ++index) {
        if (index >= value.length() ||
            value.char_at(index).value != static_cast<ca::u16>(ascii[index])) {
            return false;
        }
    }
    return index == value.length();
}

bool startsWithDot(const Token& token) noexcept
{
    return token.type() == TokenType::Identifier && !token.content().empty() &&
           token.content().char_at(0).value == '.';
}

class Context final
{
public:
    explicit Context(const std::vector<Token>& input)
    {
        tokens_.reserve(input.size());
        for (const auto& token : input) {
            if (token.type() == TokenType::Comment) {
                comments_.push_back(std::make_shared<Comment>(token));
            }
            else {
                tokens_.push_back(token);
            }
        }
    }

    ParsingResult parse(bool declarationsOnly)
    {
        std::vector<ElementPtr> result;
        while (!done()) {
            const ca::usize before  = index_;
            ElementPtr      element = declarationsOnly ? parseDeclaration() : parseValue(false);
            if (element) result.push_back(std::move(element));
            if (index_ == before) ++index_;
        }
        return ParsingResult(std::move(result), errors_.take_errors(), std::move(comments_));
    }

private:
    bool done() const noexcept { return index_ >= tokens_.size(); }

    const Token* peek(ca::i32 offset = 0) const noexcept
    {
        const ca::i64 target = static_cast<ca::i64>(index_) + offset;
        if (target < 0 || target >= static_cast<ca::i64>(tokens_.size())) return nullptr;
        return &tokens_[static_cast<ca::usize>(target)];
    }

    Token take()
    {
        if (done()) return tokens_.back();
        return tokens_[index_++];
    }

    bool takeExact(const char* expected)
    {
        const Token* token = peek();
        if (!token) {
            addError("Unexpected end of input", nullptr);
            return false;
        }
        if (!equals(token->content(), expected)) {
            addError("Unexpected token", token);
            ++index_;
            return false;
        }
        ++index_;
        return true;
    }

    void addError(const char* message, const Token* token)
    {
        ca::str::Utf16String text = ca::str::Utf16String::value_of(message);
        if (token) {
            errors_.add_error(text.ref(), token->location());
        }
        else {
            errors_.add_error(text.ref(), util::Location::unknown());
        }
    }

    IdentifierPtr identifier(Token token) { return std::make_shared<Identifier>(token); }

    ElementPtr parseValue(bool codeContext)
    {
        const Token* token = peek();
        if (!token) return nullptr;
        switch (token->type()) {
        case TokenType::Identifier:
            if (startsWithDot(*token)) return parseDeclaration();
            if (equals(token->content(), "true") || equals(token->content(), "false")) {
                return std::make_shared<Bool>(take());
            }
            return std::make_shared<Identifier>(take());
        case TokenType::Number: return std::make_shared<Number>(take());
        case TokenType::String: return std::make_shared<String>(take());
        case TokenType::Character: return std::make_shared<Character>(take());
        case TokenType::Operator:
            if (equals(token->content(), "{")) {
                return codeContext ? parseCode() : parseBraced();
            }
            addError("Unexpected operator", token);
            ++index_;
            return nullptr;
        case TokenType::Comment: break;
        }
        addError("Unexpected token", token);
        ++index_;
        return nullptr;
    }

    ElementPtr parseBraced()
    {
        const Token  begin = take();
        const Token* first = peek();
        if (!first) {
            addError("Unterminated braced element", &begin);
            return nullptr;
        }
        if (equals(first->content(), "}")) {
            ++index_;
            return std::make_shared<Empty>(begin);
        }
        if (startsWithDot(*first)) return parseDeclarationBlock();
        const Token* colon = peek(1);
        if (colon && equals(colon->content(), ":")) return parseObjectBody();
        return parseArrayBody();
    }

    ElementPtr parseDeclarationBlock()
    {
        std::vector<ElementPtr> values;
        ElementPtr              first = parseDeclaration();
        if (first) values.push_back(std::move(first));
        if (const Token* separator = peek(); separator && equals(separator->content(), ",")) {
            ++index_;
            while (!done() && !equals(peek()->content(), "}")) {
                ElementPtr value = parseValue(false);
                if (value) values.push_back(std::move(value));
                if (peek() && equals(peek()->content(), ",")) ++index_;
            }
            takeExact("}");
            return std::make_shared<Array>(std::move(values));
        }
        while (!done() && !equals(peek()->content(), "}")) {
            if (!startsWithDot(*peek())) {
                addError("Expected nested declaration", peek());
                ++index_;
                continue;
            }
            ElementPtr value = parseDeclaration();
            if (value) values.push_back(std::move(value));
        }
        takeExact("}");
        return std::make_shared<Declaration>(nullptr, std::move(values));
    }

    ElementPtr parseArrayBody()
    {
        std::vector<ElementPtr> values;
        while (!done() && !equals(peek()->content(), "}")) {
            ElementPtr value = parseValue(false);
            if (value) values.push_back(std::move(value));
            if (peek() && equals(peek()->content(), ",")) {
                ++index_;
            }
            else if (peek() && !equals(peek()->content(), "}")) {
                addError("Expected ',' or '}'", peek());
                ++index_;
            }
        }
        takeExact("}");
        return std::make_shared<Array>(std::move(values));
    }

    ElementPtr parseObjectBody()
    {
        std::vector<Object::Entry> entries;
        while (!done() && !equals(peek()->content(), "}")) {
            const Token* keyToken = peek();
            if (!keyToken || keyToken->type() == TokenType::Operator) {
                addError("Expected object key", keyToken);
                if (!done()) ++index_;
                continue;
            }
            IdentifierPtr key = identifier(take());
            if (!takeExact(":")) continue;
            const bool isCode = equals(key->content(), "code");
            ElementPtr value  = parseValue(isCode);
            if (value) entries.emplace_back(std::move(key), std::move(value));
            if (peek() && equals(peek()->content(), ",")) {
                ++index_;
            }
            else if (peek() && !equals(peek()->content(), "}")) {
                addError("Expected ',' or '}'", peek());
                ++index_;
            }
        }
        takeExact("}");
        return std::make_shared<Object>(std::move(entries));
    }

    std::shared_ptr<Declaration> parseDeclaration()
    {
        const Token* keywordToken = peek();
        if (!keywordToken) return nullptr;
        if (keywordToken->type() != TokenType::Identifier || !startsWithDot(*keywordToken)) {
            addError("Expected declaration", keywordToken);
            ++index_;
            return nullptr;
        }
        IdentifierPtr           keyword = identifier(take());
        std::vector<ElementPtr> elements;
        while (!done()) {
            const Token* token = peek();
            if (equals(token->content(), "}") || equals(token->content(), ",") ||
                startsWithDot(*token)) {
                break;
            }
            ElementPtr value = parseValue(false);
            if (value) elements.push_back(std::move(value));
        }
        return std::make_shared<Declaration>(std::move(keyword), std::move(elements));
    }

    ElementPtr parseCode()
    {
        if (!takeExact("{")) return nullptr;
        std::vector<InstructionPtr> instructions;
        while (!done() && !equals(peek()->content(), "}")) {
            const Token* first = peek();
            if (!first || first->type() != TokenType::Identifier) {
                addError("Expected instruction or label", first);
                if (!done()) ++index_;
                continue;
            }
            IdentifierPtr name = identifier(take());
            if (peek() && equals(peek()->content(), ":")) {
                ++index_;
                instructions.push_back(std::make_shared<Label>(std::move(name)));
                continue;
            }
            std::vector<ElementPtr> arguments;
            const ca::i32           line = first->location().line();
            while (!done() && !equals(peek()->content(), "}") &&
                   peek()->location().line() == line) {
                ElementPtr argument = parseValue(false);
                if (argument) arguments.push_back(std::move(argument));
            }
            instructions.push_back(
                std::make_shared<Instruction>(std::move(name), std::move(arguments)));
        }
        takeExact("}");
        return std::make_shared<Code>(std::move(instructions));
    }

    std::vector<Token>                         tokens_;
    ca::usize                                  index_ = 0;
    error::ErrorCollector                      errors_;
    std::vector<std::shared_ptr<ast::Comment>> comments_;
};
}   //namespace

ParsingResult DeclarationParser::parseAny(const std::vector<Token>& tokens)
{
    return Context(tokens).parse(false);
}

ParsingResult DeclarationParser::parseDeclarations(const std::vector<Token>& tokens)
{
    return Context(tokens).parse(true);
}

}   //namespace jhasm::parser
