#include "method_type.hpp"

#include <libca/str/utf8_string.hpp>

#include <stdexcept>

namespace jblw::type {

namespace {
ca::str::Utf8String methodDescriptor(const ClassType&                 returnType,
                                     const std::vector<ClassTypePtr>& parameterTypes)
{
    ca::str::Utf8StringBuilder builder;
    builder.append("(");
    for (const auto& parameter : parameterTypes) builder.append(parameter->descriptor());
    builder.append(")").append(returnType.descriptor());
    return builder.build();
}

ca::i32 listHash(const std::vector<ClassTypePtr>& values) noexcept
{
    ca::u32 hash = 1;
    for (const auto& value : values) hash = hash * 31U + static_cast<ca::u32>(value->hashCode());
    return static_cast<ca::i32>(hash);
}

ca::i32 methodHash(const ClassType&                 returnType,
                   const std::vector<ClassTypePtr>& parameters) noexcept
{
    const ca::u32 hash = static_cast<ca::u32>(returnType.hashCode()) * 31U +
                         static_cast<ca::u32>(listHash(parameters));
    return static_cast<ca::i32>(hash);
}
}   //namespace

MethodType::MethodType(ClassTypePtr returnType, std::vector<ClassTypePtr> parameterTypes)
    : returnType_(std::move(returnType))
    , parameterTypes_(std::move(parameterTypes))
    , descriptor_()
    , hashCode_(0)
{
    if (!returnType_) throw std::invalid_argument("Method return type cannot be null");
    for (const auto& parameter : parameterTypes_) {
        if (!parameter) throw std::invalid_argument("Method parameter type cannot be null");
    }
    descriptor_ = methodDescriptor(*returnType_, parameterTypes_);
    hashCode_   = methodHash(*returnType_, parameterTypes_);
}

bool MethodType::equals(const Type& other) const noexcept
{
    if (other.getType() != TypeKind::METHOD) return false;
    const auto& method = static_cast<const MethodType&>(other);
    if (*returnType_ != *method.returnType_ ||
        parameterTypes_.size() != method.parameterTypes_.size()) {
        return false;
    }
    for (ca::usize i = 0; i < parameterTypes_.size(); ++i) {
        if (*parameterTypes_[i] != *method.parameterTypes_[i]) return false;
    }
    return true;
}

}   //namespace jblw::type
