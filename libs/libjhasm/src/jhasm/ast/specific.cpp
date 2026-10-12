#include "specific.hpp"

#include <array>

namespace jhasm::ast {
namespace {
bool equals(ca::str::Utf16StringRef value, const char* ascii) noexcept
{
    ca::usize index = 0;
    for (; ascii[index] != '\0'; ++index) {
        if (index >= value.length() ||
            value.char_at(index).value != static_cast<ca::u16>(ascii[index]))
            return false;
    }
    return index == value.length();
}

std::vector<ElementPtr> annotationChildren(const IdentifierPtr&              type,
                                           const std::vector<Object::Entry>& values,
                                           const std::shared_ptr<Number>&    typeRef,
                                           const IdentifierPtr&              typePath)
{
    std::vector<ElementPtr> result{type};
    for (const auto& entry : values) {
        result.push_back(entry.first);
        result.push_back(entry.second);
    }
    if (typeRef) result.push_back(typeRef);
    if (typePath) result.push_back(typePath);
    return result;
}

std::vector<ElementPtr> methodChildren(const IdentifierPtr& name, const IdentifierPtr& descriptor,
                                       const std::vector<IdentifierPtr>&              parameters,
                                       const ElementPtr&                              defaultValue,
                                       const std::vector<std::shared_ptr<Exception>>& exceptions,
                                       const std::shared_ptr<Code>&                   code)
{
    std::vector<ElementPtr> result{name, descriptor};
    result.insert(result.end(), parameters.begin(), parameters.end());
    if (defaultValue) result.push_back(defaultValue);
    result.insert(result.end(), exceptions.begin(), exceptions.end());
    if (code) result.push_back(code);
    return result;
}
}   //namespace

bool Modifiers::isValid(ca::str::Utf16StringRef value) noexcept
{
    static constexpr std::array<const char*, 23> values = {
        "public",    "private",   "protected", "static",       "final",     "abstract",
        "strictfp",  "transient", "volatile",  "synchronized", "native",    "varargs",
        "bridge",    "synthetic", "enum",      "annotation",   "module",    "super",
        "interface", "record",    "sealed",    "open",         "non-sealed"};
    for (const char* item : values) {
        if (equals(value, item)) return true;
    }
    return false;
}

bool Modifiers::has(ca::str::Utf16StringRef value) const noexcept
{
    for (const auto& item : values_) {
        if (item && item->content().equals(value)) return true;
    }
    return false;
}

Annotation::Annotation(bool visible, IdentifierPtr classType, std::vector<Object::Entry> values,
                       std::shared_ptr<Number> typeRef, IdentifierPtr typePath)
    : Element(annotationChildren(classType, values, typeRef, typePath))
    , visible_(visible)
    , classType_(std::move(classType))
    , values_(std::move(values))
    , typeRef_(std::move(typeRef))
    , typePath_(std::move(typePath))
{}

ElementPtr Annotation::value(ca::str::Utf16StringRef name) const noexcept
{
    for (const auto& entry : values_) {
        if (entry.first && entry.first->content().equals(name)) return entry.second;
    }
    return nullptr;
}

EnumValue::EnumValue(IdentifierPtr enumType, IdentifierPtr enumValue)
    : Element(std::vector<ElementPtr>{enumType, enumValue})
    , enumType_(std::move(enumType))
    , enumValue_(std::move(enumValue))
{}

Exception::Exception(IdentifierPtr start, IdentifierPtr end, IdentifierPtr handler,
                     IdentifierPtr type)
    : Element(std::vector<ElementPtr>{start, end, handler, type})
    , start_(std::move(start))
    , end_(std::move(end))
    , handler_(std::move(handler))
    , type_(std::move(type))
{}

InnerClass::InnerClass(Modifiers modifiers, IdentifierPtr name, IdentifierPtr outer,
                       IdentifierPtr inner)
    : Element(std::vector<ElementPtr>{name, outer, inner})
    , modifiers_(std::move(modifiers))
    , name_(std::move(name))
    , outer_(std::move(outer))
    , inner_(std::move(inner))
{}

RecordComponent::RecordComponent(IdentifierPtr name, IdentifierPtr descriptor)
    : Element(std::vector<ElementPtr>{name, descriptor})
    , name_(std::move(name))
    , descriptor_(std::move(descriptor))
{}

OuterMethod::OuterMethod(IdentifierPtr name, IdentifierPtr descriptor)
    : Element(std::vector<ElementPtr>{name, descriptor})
    , name_(std::move(name))
    , descriptor_(std::move(descriptor))
{}

Member::Member(Modifiers modifiers, IdentifierPtr name, IdentifierPtr descriptor,
               std::vector<ElementPtr> children)
    : Element(std::move(children))
    , modifiers_(std::move(modifiers))
    , name_(std::move(name))
    , descriptor_(std::move(descriptor))
{}

Field::Field(Modifiers modifiers, IdentifierPtr name, IdentifierPtr descriptor, ElementPtr value)
    : Member(std::move(modifiers), name, descriptor,
             value ? std::vector<ElementPtr>{name, descriptor, value}
                   : std::vector<ElementPtr>{name, descriptor})
    , value_(std::move(value))
{}

Method::Method(Modifiers modifiers, IdentifierPtr name, IdentifierPtr descriptor,
               std::vector<IdentifierPtr> parameters, ParameterAnnotations parameterAnnotations,
               ElementPtr defaultValue, std::vector<std::shared_ptr<Exception>> exceptions,
               std::shared_ptr<Code> code)
    : Member(std::move(modifiers), name, descriptor,
             methodChildren(name, descriptor, parameters, defaultValue, exceptions, code))
    , parameters_(std::move(parameters))
    , parameterAnnotations_(std::move(parameterAnnotations))
    , defaultValue_(std::move(defaultValue))
    , exceptions_(std::move(exceptions))
    , code_(std::move(code))
{}

Class::Class(Modifiers modifiers, IdentifierPtr name, std::vector<ElementPtr> contents)
    : Member(std::move(modifiers), name, nullptr, contents)
    , contents_(std::move(contents))
{}

}   //namespace jhasm::ast
