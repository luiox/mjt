#include "instance_type.hpp"

#include "types.hpp"

#include <libca/str/utf16_string.hpp>

namespace jblw::type {

namespace {
ca::i32 javaStringHash(ca::str::Utf8StringRef value)
{
    return ca::str::Utf16String::from_utf8_string(value).hash_code();
}
}   //namespace

InstanceType::InstanceType(ca::str::Utf8String descriptor, ca::str::Utf8String internalName)
    : descriptor_(std::move(descriptor))
    , internalName_(std::move(internalName))
    , externalName_(Types::externalName(internalName_.ref()))
    , hashCode_(javaStringHash(descriptor_.ref()))
{}

bool InstanceType::equals(const Type& other) const noexcept
{
    if (other.getType() != TypeKind::INSTANCE) return false;
    const auto& instance = static_cast<const InstanceType&>(other);
    return descriptor_.equals(instance.descriptor_);
}

}   //namespace jblw::type
