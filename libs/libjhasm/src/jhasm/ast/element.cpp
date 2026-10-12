#include "element.hpp"

#include <algorithm>

namespace jhasm::ast {

Element::Element(const parser::Token& value)
    : value_(value)
{}

Element::Element(std::vector<ElementPtr> children)
    : children_(std::move(children))
{}

Element::Element(const parser::Token& value, std::vector<ElementPtr> children)
    : value_(value)
    , children_(std::move(children))
{}

ca::str::Utf16StringRef Element::content() const noexcept
{
    return value_ ? value_->content() : ca::str::Utf16StringRef{};
}

const util::Location* Element::location() const noexcept
{
    if (value_) return &value_->location();
    for (const auto& child : children_) {
        if (child) {
            if (const auto* result = child->location()) return result;
        }
    }
    return nullptr;
}

util::Range Element::range() const noexcept
{
    if (value_) return value_->range();
    ca::i32 start = -1;
    ca::i32 end   = -1;
    for (const auto& child : children_) {
        if (!child) continue;
        const util::Range childRange = child->range();
        if (childRange.start() < 0) continue;
        start = start < 0 ? childRange.start() : std::min(start, childRange.start());
        end   = std::max(end, childRange.end());
    }
    return util::Range(start, end);
}

}   //namespace jhasm::ast
