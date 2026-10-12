#pragma once

#include "element.hpp"

#include <utility>

namespace jhasm::ast {

class Identifier final : public Element
{
public:
    explicit Identifier(const parser::Token& value)
        : Element(value)
    {}
    ElementKind             getType() const noexcept override { return ElementKind::IDENTIFIER; }
    ca::str::Utf16StringRef literal() const noexcept { return content(); }
};
using IdentifierPtr = std::shared_ptr<Identifier>;

class String final : public Element
{
public:
    explicit String(const parser::Token& value)
        : Element(value)
    {}
    ElementKind getType() const noexcept override { return ElementKind::STRING; }
};

class Character final : public Element
{
public:
    explicit Character(const parser::Token& value)
        : Element(value)
    {}
    ElementKind getType() const noexcept override { return ElementKind::CHARACTER; }
};

class Number final : public Element
{
public:
    explicit Number(const parser::Token& value)
        : Element(value)
    {}
    ElementKind getType() const noexcept override { return ElementKind::NUMBER; }

    bool isFloatingPoint() const noexcept;
    bool isWide() const noexcept;
};

class Bool final : public Element
{
public:
    explicit Bool(const parser::Token& value)
        : Element(value)
    {}
    ElementKind getType() const noexcept override { return ElementKind::BOOL; }
    bool        boolValue() const noexcept;
};

class Empty final : public Element
{
public:
    explicit Empty(const parser::Token& value)
        : Element(value)
    {}
    ElementKind getType() const noexcept override { return ElementKind::EMPTY; }
};

class Comment final : public Element
{
public:
    explicit Comment(const parser::Token& value)
        : Element(value)
    {}
    ElementKind getType() const noexcept override { return ElementKind::COMMENT; }
};

class Array final : public Element
{
public:
    explicit Array(std::vector<ElementPtr> values)
        : Element(values)
        , values_(std::move(values))
    {}

    ElementKind                    getType() const noexcept override { return ElementKind::ARRAY; }
    const std::vector<ElementPtr>& values() const noexcept { return values_; }
    const ElementPtr&              value(ca::usize index) const { return values_.at(index); }

private:
    std::vector<ElementPtr> values_;
};

class Object final : public Element
{
public:
    using Entry = std::pair<IdentifierPtr, ElementPtr>;

    explicit Object(std::vector<Entry> values);
    ElementKind               getType() const noexcept override { return ElementKind::OBJECT; }
    const std::vector<Entry>& values() const noexcept { return values_; }
    ElementPtr                value(ca::str::Utf16StringRef name) const noexcept;

private:
    std::vector<Entry> values_;
};

class Declaration final : public Element
{
public:
    Declaration(IdentifierPtr keyword, std::vector<ElementPtr> elements);
    ElementKind          getType() const noexcept override { return ElementKind::DECLARATION; }
    const IdentifierPtr& keyword() const noexcept { return keyword_; }
    const std::vector<ElementPtr>& elements() const noexcept { return elements_; }
    const ElementPtr&              element(ca::usize index) const { return elements_.at(index); }

private:
    IdentifierPtr           keyword_;
    std::vector<ElementPtr> elements_;
};

class Instruction : public Element
{
public:
    Instruction(IdentifierPtr identifier, std::vector<ElementPtr> arguments);
    ElementKind          getType() const noexcept override { return ElementKind::INSTRUCTION; }
    const IdentifierPtr& identifier() const noexcept { return identifier_; }
    const std::vector<ElementPtr>& arguments() const noexcept { return arguments_; }

private:
    IdentifierPtr           identifier_;
    std::vector<ElementPtr> arguments_;
};
using InstructionPtr = std::shared_ptr<Instruction>;

class Label final : public Instruction
{
public:
    explicit Label(IdentifierPtr identifier);
    ElementKind getType() const noexcept override { return ElementKind::LABEL; }
};

class Code final : public Element
{
public:
    explicit Code(std::vector<InstructionPtr> instructions);
    ElementKind getType() const noexcept override { return ElementKind::CODE; }
    const std::vector<InstructionPtr>& instructions() const noexcept { return instructions_; }

private:
    std::vector<InstructionPtr> instructions_;
};

}   //namespace jhasm::ast
