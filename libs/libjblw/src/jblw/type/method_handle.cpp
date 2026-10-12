#include "method_handle.hpp"

#include "object_type.hpp"

#include <libca/str/utf16_string.hpp>

#include <stdexcept>

namespace jblw::type {

namespace {
ca::i32 javaStringHash(ca::str::Utf8StringRef value)
{
    return ca::str::Utf16String::from_utf8_string(value).hash_code();
}
}   //namespace

MethodHandle::MethodHandle(ca::i32 kind, ObjectTypePtr owner, ca::str::Utf8String name,
                           TypePtr descriptor, bool isInterface)
    : kind_(kind)
    , owner_(std::move(owner))
    , name_(std::move(name))
    , descriptor_(std::move(descriptor))
    , isInterface_(isInterface)
    , hashCode_(0)
{
    if (!owner_) throw std::invalid_argument("Method handle owner cannot be null");
    if (!descriptor_) throw std::invalid_argument("Method handle descriptor cannot be null");

    ca::u32 hash = static_cast<ca::u32>(kind_);
    hash         = hash * 31U + static_cast<ca::u32>(owner_->hashCode());
    hash         = hash * 31U + static_cast<ca::u32>(javaStringHash(name_.ref()));
    hash         = hash * 31U + static_cast<ca::u32>(descriptor_->hashCode());
    hash         = hash * 31U + (isInterface_ ? 1U : 0U);
    hashCode_    = static_cast<ca::i32>(hash);
}

bool MethodHandle::equals(const MethodHandle& other) const noexcept
{
    return this == &other || (kind_ == other.kind_ && isInterface_ == other.isInterface_ &&
                              *owner_ == *other.owner_ && name_.equals(other.name_) &&
                              *descriptor_ == *other.descriptor_);
}

ca::str::Utf8String MethodHandle::toString() const
{
    ca::str::Utf8StringBuilder builder;
    builder.append(owner_->internalName())
        .append(".")
        .append(name_)
        .append(descriptor_->descriptor())
        .append(",")
        .append(isInterface_ ? "true" : "false");
    return builder.build();
}

bool operator==(const MethodHandle& lhs, const MethodHandle& rhs) noexcept
{
    return lhs.equals(rhs);
}

bool operator!=(const MethodHandle& lhs, const MethodHandle& rhs) noexcept
{
    return !lhs.equals(rhs);
}

}   //namespace jblw::type
