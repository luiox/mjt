#pragma once

#include "element.hpp"

#include "jblw/type/object_type.hpp"

#include <libca/str/utf8_string.hpp>

#include <vector>

namespace jblw::annotation {

#define JBLW_SCALAR_ELEMENT(className, valueType, kindName)           \
    class className final : public Element                            \
    {                                                                 \
    public:                                                           \
        explicit className(valueType value) noexcept                  \
            : value_(value)                                           \
        {}                                                            \
        valueType value() const noexcept                              \
        {                                                             \
            return value_;                                            \
        }                                                             \
        ElementKind getType() const noexcept override                 \
        {                                                             \
            return ElementKind::kindName;                             \
        }                                                             \
        bool    equals(const Element& other) const noexcept override; \
        ca::i32 hashCode() const noexcept override;                   \
                                                                      \
    private:                                                          \
        valueType value_;                                             \
    }

JBLW_SCALAR_ELEMENT(ElementBoolean, bool, BOOLEAN);
JBLW_SCALAR_ELEMENT(ElementByte, ca::i8, BYTE);
JBLW_SCALAR_ELEMENT(ElementChar, ca::u16, CHAR);
JBLW_SCALAR_ELEMENT(ElementDouble, ca::f64, DOUBLE);
JBLW_SCALAR_ELEMENT(ElementFloat, ca::f32, FLOAT);
JBLW_SCALAR_ELEMENT(ElementInt, ca::i32, INT);
JBLW_SCALAR_ELEMENT(ElementLong, ca::i64, LONG);
JBLW_SCALAR_ELEMENT(ElementShort, ca::i16, SHORT);

#undef JBLW_SCALAR_ELEMENT

///@brief 返回共享的布尔 annotation element singleton。
ElementPtr booleanElement(bool value);

class ElementString final : public Element
{
public:
    explicit ElementString(ca::str::Utf8String value);
    const ca::str::Utf8String& value() const noexcept { return value_; }
    ElementKind                getType() const noexcept override { return ElementKind::STRING; }
    bool                       equals(const Element& other) const noexcept override;
    ca::i32                    hashCode() const noexcept override { return hashCode_; }

private:
    ca::str::Utf8String value_;
    ca::i32             hashCode_;
};

class ElementType final : public Element
{
public:
    explicit ElementType(type::ObjectTypePtr value);
    const type::ObjectTypePtr& value() const noexcept { return value_; }
    ElementKind                getType() const noexcept override { return ElementKind::TYPE; }
    bool                       equals(const Element& other) const noexcept override;
    ca::i32                    hashCode() const noexcept override { return value_->hashCode(); }

private:
    type::ObjectTypePtr value_;
};

class ElementEnum final : public Element
{
public:
    ElementEnum(type::InstanceTypePtr type, ca::str::Utf8String name);
    const type::InstanceTypePtr& type() const noexcept { return type_; }
    ca::str::Utf8StringRef       name() const noexcept { return name_.ref(); }
    ElementKind                  getType() const noexcept override { return ElementKind::ENUM; }
    bool                         equals(const Element& other) const noexcept override;
    ca::i32                      hashCode() const noexcept override { return hashCode_; }

private:
    type::InstanceTypePtr type_;
    ca::str::Utf8String   name_;
    ca::i32               hashCode_;
};

class ElementArray final : public Element
{
public:
    explicit ElementArray(std::vector<ElementPtr> elements);
    const ElementPtr&              get(ca::usize index) const { return elements_.at(index); }
    ca::usize                      count() const noexcept { return elements_.size(); }
    const std::vector<ElementPtr>& elements() const noexcept { return elements_; }
    ElementKind                    getType() const noexcept override { return ElementKind::ARRAY; }
    bool                           equals(const Element& other) const noexcept override;
    ca::i32                        hashCode() const noexcept override { return hashCode_; }

private:
    std::vector<ElementPtr> elements_;
    ca::i32                 hashCode_;
};

}   //namespace jblw::annotation
