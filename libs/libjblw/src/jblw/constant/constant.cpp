#include "constant.hpp"

#include "constant_sink.hpp"
#include "of_double.hpp"
#include "of_dynamic.hpp"
#include "of_float.hpp"
#include "of_int.hpp"
#include "of_long.hpp"
#include "of_method_handle.hpp"
#include "of_string.hpp"
#include "of_type.hpp"

#include <libca/str/utf16_string.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace jblw::constant {

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

bool operator==(const Constant& lhs, const Constant& rhs) noexcept
{
    return lhs.equals(rhs);
}

bool operator!=(const Constant& lhs, const Constant& rhs) noexcept
{
    return !lhs.equals(rhs);
}

void OfInt::accept(ConstantSink& sink) const
{
    sink.acceptInt(*this);
}

bool OfInt::equals(const Constant& other) const noexcept
{
    return this == &other || (other.getType() == ConstantKind::INT &&
                              value_ == static_cast<const OfInt&>(other).value_);
}

void OfLong::accept(ConstantSink& sink) const
{
    sink.acceptLong(*this);
}

bool OfLong::equals(const Constant& other) const noexcept
{
    return this == &other || (other.getType() == ConstantKind::LONG &&
                              value_ == static_cast<const OfLong&>(other).value_);
}

ca::i32 OfLong::hashCode() const noexcept
{
    const ca::u64 bits = static_cast<ca::u64>(value_);
    return static_cast<ca::i32>(static_cast<ca::u32>(bits ^ (bits >> 32U)));
}

void OfFloat::accept(ConstantSink& sink) const
{
    sink.acceptFloat(*this);
}

bool OfFloat::equals(const Constant& other) const noexcept
{
    return this == &other || (other.getType() == ConstantKind::FLOAT &&
                              value_ == static_cast<const OfFloat&>(other).value_);
}

ca::i32 OfFloat::hashCode() const noexcept
{
    return static_cast<ca::i32>(floatBits(value_));
}

void OfDouble::accept(ConstantSink& sink) const
{
    sink.acceptDouble(*this);
}

bool OfDouble::equals(const Constant& other) const noexcept
{
    return this == &other || (other.getType() == ConstantKind::DOUBLE &&
                              value_ == static_cast<const OfDouble&>(other).value_);
}

ca::i32 OfDouble::hashCode() const noexcept
{
    const ca::u64 bits = doubleBits(value_);
    return static_cast<ca::i32>(static_cast<ca::u32>(bits ^ (bits >> 32U)));
}

OfString::OfString(ca::str::Utf8String value)
    : value_(std::move(value))
    , hashCode_(javaStringHash(value_.ref()))
{}

void OfString::accept(ConstantSink& sink) const
{
    sink.acceptString(*this);
}

bool OfString::equals(const Constant& other) const noexcept
{
    return this == &other || (other.getType() == ConstantKind::STRING &&
                              value_.equals(static_cast<const OfString&>(other).value_));
}

OfType::OfType(type::TypePtr value)
    : value_(std::move(value))
{
    if (!value_) throw std::invalid_argument("Type constant value cannot be null");
}

void OfType::accept(ConstantSink& sink) const
{
    sink.acceptType(*this);
}

bool OfType::equals(const Constant& other) const noexcept
{
    return this == &other || (other.getType() == ConstantKind::TYPE &&
                              *value_ == *static_cast<const OfType&>(other).value_);
}

OfMethodHandle::OfMethodHandle(type::MethodHandlePtr value)
    : value_(std::move(value))
{
    if (!value_) throw std::invalid_argument("Method handle constant value cannot be null");
}

void OfMethodHandle::accept(ConstantSink& sink) const
{
    sink.acceptMethodHandle(*this);
}

bool OfMethodHandle::equals(const Constant& other) const noexcept
{
    return this == &other || (other.getType() == ConstantKind::METHOD_HANDLE &&
                              *value_ == *static_cast<const OfMethodHandle&>(other).value_);
}

OfDynamic::OfDynamic(type::ConstantDynamicPtr value)
    : value_(std::move(value))
{
    if (!value_) throw std::invalid_argument("Dynamic constant value cannot be null");
}

void OfDynamic::accept(ConstantSink& sink) const
{
    sink.acceptDynamic(*this);
}

bool OfDynamic::equals(const Constant& other) const noexcept
{
    return this == &other || (other.getType() == ConstantKind::DYNAMIC &&
                              value_.get() == static_cast<const OfDynamic&>(other).value_.get());
}

ca::i32 OfDynamic::hashCode() const noexcept
{
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(value_.get());
    if (sizeof(address) <= sizeof(ca::u32)) return static_cast<ca::i32>(address);
    return static_cast<ca::i32>(static_cast<ca::u32>(address ^ (address >> 32U)));
}

}   //namespace jblw::constant
