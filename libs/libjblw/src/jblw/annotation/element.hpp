#pragma once

#include <libca/core/datatype.hpp>

#include <memory>

namespace jblw::annotation {

enum class ElementKind : ca::u8
{
    ANNOTATION,
    ARRAY,
    BOOLEAN,
    BYTE,
    CHAR,
    DOUBLE,
    ENUM,
    FLOAT,
    INT,
    LONG,
    SHORT,
    STRING,
    TYPE,
};

///@brief JVM annotation element 的不可变根接口。
class Element
{
public:
    virtual ~Element()                                              = default;
    virtual ElementKind getType() const noexcept                    = 0;
    virtual bool        equals(const Element& other) const noexcept = 0;
    virtual ca::i32     hashCode() const noexcept                   = 0;

protected:
    Element() = default;
};

using ElementPtr = std::shared_ptr<const Element>;

bool operator==(const Element& lhs, const Element& rhs) noexcept;
bool operator!=(const Element& lhs, const Element& rhs) noexcept;

}   //namespace jblw::annotation
