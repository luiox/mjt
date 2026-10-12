#pragma once

#include "jhasm/parser/token.hpp"

#include <libca/core/datatype.hpp>
#include <libca/str/utf16_string.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace jhasm::ast {

enum class ElementKind : ca::u8
{
    ARRAY,
    OBJECT,
    DECLARATION,
    IDENTIFIER,
    STRING,
    CHARACTER,
    NUMBER,
    BOOL,
    CODE,
    INSTRUCTION,
    LABEL,
    COMMENT,
    EMPTY,
    CLASS,
    METHOD,
    FIELD,
    ANNOTATION,
    ENUM_VALUE,
    INNER_CLASS,
    RECORD_COMPONENT,
    OUTER_METHOD,
    EXCEPTION,
};

class Element;
using ElementPtr = std::shared_ptr<Element>;

///@brief Jasm AST 的 RTTI-free 拥有型根节点。
///
///节点复制 Token，因此 content/location 在输入、Tokenizer 和 parser 销毁后仍有效。
///children 由节点共享持有，parent 不保存反向 owning 引用。
class Element
{
public:
    virtual ~Element() = default;

    virtual ElementKind                    getType() const noexcept = 0;
    virtual const std::vector<ElementPtr>& children() const noexcept { return children_; }

    ca::str::Utf16StringRef content() const noexcept;
    const parser::Token*    value() const noexcept { return value_ ? &*value_ : nullptr; }
    const util::Location*   location() const noexcept;
    util::Range             range() const noexcept;

protected:
    Element() = default;
    explicit Element(const parser::Token& value);
    explicit Element(std::vector<ElementPtr> children);
    Element(const parser::Token& value, std::vector<ElementPtr> children);

    std::optional<parser::Token> value_;
    std::vector<ElementPtr>      children_;
};

template<typename T>
std::shared_ptr<T> elementAs(const ElementPtr& value, ElementKind expected) noexcept
{
    if (!value || value->getType() != expected) return nullptr;
    return std::static_pointer_cast<T>(value);
}

}   //namespace jhasm::ast
