#include "elements.hpp"

#include "jblw/type/instance_type.hpp"

#include <libca/str/utf16_string.hpp>

#include <cmath>
#include <cstring>
#include <stdexcept>

namespace jblw::annotation {

namespace {
ca::i32 javaStringHash(ca::str::Utf8StringRef value)
{
    return ca::str::Utf16String::from_utf8_string(value).hash_code();
}

ca::u32 floatBits(ca::f32 value) noexcept
{
    if (std::isnan(value)) return 0x7fc00000U;
    ca::u32 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

ca::u64 doubleBits(ca::f64 value) noexcept
{
    if (std::isnan(value)) return 0x7ff8000000000000ULL;
    ca::u64 bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}
}   //namespace

bool operator==(const Element& lhs, const Element& rhs) noexcept
{
    return lhs.equals(rhs);
}

bool operator!=(const Element& lhs, const Element& rhs) noexcept
{
    return !lhs.equals(rhs);
}

#define JBLW_INTEGER_ELEMENT_IMPL(className, kindName)                                    \
    bool className::equals(const Element& other) const noexcept                           \
    {                                                                                     \
        return this == &other || (other.getType() == ElementKind::kindName &&             \
                                  value_ == static_cast<const className&>(other).value_); \
    }                                                                                     \
    ca::i32 className::hashCode() const noexcept                                          \
    {                                                                                     \
        return static_cast<ca::i32>(value_);                                              \
    }

JBLW_INTEGER_ELEMENT_IMPL(ElementByte, BYTE)
JBLW_INTEGER_ELEMENT_IMPL(ElementChar, CHAR)
JBLW_INTEGER_ELEMENT_IMPL(ElementInt, INT)
JBLW_INTEGER_ELEMENT_IMPL(ElementShort, SHORT)

#undef JBLW_INTEGER_ELEMENT_IMPL

bool ElementBoolean::equals(const Element& other) const noexcept
{
    return this == &other || (other.getType() == ElementKind::BOOLEAN &&
                              value_ == static_cast<const ElementBoolean&>(other).value_);
}

ca::i32 ElementBoolean::hashCode() const noexcept
{
    return value_ ? 1231 : 1237;
}

bool ElementLong::equals(const Element& other) const noexcept
{
    return this == &other || (other.getType() == ElementKind::LONG &&
                              value_ == static_cast<const ElementLong&>(other).value_);
}

ca::i32 ElementLong::hashCode() const noexcept
{
    const ca::u64 bits = static_cast<ca::u64>(value_);
    return static_cast<ca::i32>(static_cast<ca::u32>(bits ^ (bits >> 32U)));
}

bool ElementFloat::equals(const Element& other) const noexcept
{
    return this == &other ||
           (other.getType() == ElementKind::FLOAT &&
            floatBits(value_) == floatBits(static_cast<const ElementFloat&>(other).value_));
}

ca::i32 ElementFloat::hashCode() const noexcept
{
    return static_cast<ca::i32>(floatBits(value_));
}

bool ElementDouble::equals(const Element& other) const noexcept
{
    return this == &other ||
           (other.getType() == ElementKind::DOUBLE &&
            doubleBits(value_) == doubleBits(static_cast<const ElementDouble&>(other).value_));
}

ca::i32 ElementDouble::hashCode() const noexcept
{
    const ca::u64 bits = doubleBits(value_);
    return static_cast<ca::i32>(static_cast<ca::u32>(bits ^ (bits >> 32U)));
}

ElementPtr booleanElement(bool value)
{
    static const ElementPtr trueValue  = std::make_shared<const ElementBoolean>(true);
    static const ElementPtr falseValue = std::make_shared<const ElementBoolean>(false);
    return value ? trueValue : falseValue;
}

ElementString::ElementString(ca::str::Utf8String value)
    : value_(std::move(value))
    , hashCode_(javaStringHash(value_.ref()))
{}

bool ElementString::equals(const Element& other) const noexcept
{
    return this == &other || (other.getType() == ElementKind::STRING &&
                              value_.equals(static_cast<const ElementString&>(other).value_));
}

ElementType::ElementType(type::ObjectTypePtr value)
    : value_(std::move(value))
{
    if (!value_) throw std::invalid_argument("Annotation type element cannot be null");
}

bool ElementType::equals(const Element& other) const noexcept
{
    return this == &other || (other.getType() == ElementKind::TYPE &&
                              *value_ == *static_cast<const ElementType&>(other).value_);
}

ElementEnum::ElementEnum(type::InstanceTypePtr type, ca::str::Utf8String name)
    : type_(std::move(type))
    , name_(std::move(name))
    , hashCode_(0)
{
    if (!type_) throw std::invalid_argument("Annotation enum type cannot be null");
    hashCode_ = static_cast<ca::i32>(static_cast<ca::u32>(type_->hashCode()) * 31U +
                                     static_cast<ca::u32>(javaStringHash(name_.ref())));
}

bool ElementEnum::equals(const Element& other) const noexcept
{
    if (this == &other) return true;
    if (other.getType() != ElementKind::ENUM) return false;
    const auto& value = static_cast<const ElementEnum&>(other);
    return *type_ == *value.type_ && name_.equals(value.name_);
}

ElementArray::ElementArray(std::vector<ElementPtr> elements)
    : elements_(std::move(elements))
    , hashCode_(1)
{
    ca::u32 hash = 1;
    for (const auto& element : elements_) {
        if (!element) throw std::invalid_argument("Annotation array element cannot be null");
        hash = hash * 31U + static_cast<ca::u32>(element->hashCode());
    }
    hashCode_ = static_cast<ca::i32>(hash);
}

bool ElementArray::equals(const Element& other) const noexcept
{
    if (this == &other) return true;
    if (other.getType() != ElementKind::ARRAY) return false;
    const auto& array = static_cast<const ElementArray&>(other);
    if (elements_.size() != array.elements_.size()) return false;
    for (ca::usize i = 0; i < elements_.size(); ++i) {
        if (*elements_[i] != *array.elements_[i]) return false;
    }
    return true;
}

}   //namespace jblw::annotation
