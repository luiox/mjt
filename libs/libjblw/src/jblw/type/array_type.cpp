#include "array_type.hpp"

#include "types.hpp"

#include <libca/str/utf8_string.hpp>

#include <stdexcept>

namespace jblw::type {

namespace {
ca::str::Utf8String arrayDescriptor(const ClassType& component)
{
    ca::str::Utf8StringBuilder builder;
    builder.append("[").append(component.descriptor());
    return builder.build();
}

ca::i32 arrayHash(ca::i32 dimensions, ca::i32 componentHash) noexcept
{
    const ca::u32 value =
        static_cast<ca::u32>(dimensions) * 31U + static_cast<ca::u32>(componentHash);
    return static_cast<ca::i32>(value);
}
}   //namespace

ArrayType::ArrayType(ClassTypePtr componentType)
    : dimensions_(1)
    , componentType_(std::move(componentType))
    , descriptor_()
    , externalName_()
    , hashCode_(0)
{
    if (!componentType_) throw std::invalid_argument("Array component type cannot be null");
    if (componentType_->getType() == TypeKind::ARRAY) {
        dimensions_ = static_cast<const ArrayType&>(*componentType_).dimensions() + 1;
    }
    descriptor_   = arrayDescriptor(*componentType_);
    externalName_ = Types::externalName(descriptor_.ref());
    hashCode_     = arrayHash(dimensions_, componentType_->hashCode());
}

const ClassType* ArrayType::rootComponentType() const noexcept
{
    const ClassType* component = componentType_.get();
    while (component->getType() == TypeKind::ARRAY) {
        component = static_cast<const ArrayType*>(component)->componentType().get();
    }
    return component;
}

bool ArrayType::equals(const Type& other) const noexcept
{
    if (other.getType() != TypeKind::ARRAY) return false;
    const auto& array = static_cast<const ArrayType&>(other);
    return dimensions_ == array.dimensions_ && *componentType_ == *array.componentType_;
}

}   //namespace jblw::type
