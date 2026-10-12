#include "constant_dynamic.hpp"

#include "invoke_dynamic.hpp"

#include <stdexcept>

namespace jblw::type {

namespace {
void validateDynamic(const TypePtr& descriptor, const MethodHandlePtr& bootstrapHandle,
                     const std::vector<constant::ConstantPtr>& args)
{
    if (!descriptor) throw std::invalid_argument("Dynamic descriptor cannot be null");
    if (!bootstrapHandle) throw std::invalid_argument("Dynamic bootstrap handle cannot be null");
    for (const auto& arg : args) {
        if (!arg) throw std::invalid_argument("Dynamic bootstrap argument cannot be null");
    }
}
}   //namespace

ConstantDynamic::ConstantDynamic(ca::str::Utf8String name, ClassTypePtr descriptor,
                                 MethodHandlePtr                    bootstrapHandle,
                                 std::vector<constant::ConstantPtr> args)
    : name_(std::move(name))
    , descriptor_(std::move(descriptor))
    , bootstrapHandle_(std::move(bootstrapHandle))
    , args_(std::move(args))
{
    validateDynamic(std::static_pointer_cast<const Type>(descriptor_), bootstrapHandle_, args_);
}

InvokeDynamic::InvokeDynamic(ca::str::Utf8String name, MethodTypePtr descriptor,
                             MethodHandlePtr                    bootstrapHandle,
                             std::vector<constant::ConstantPtr> args)
    : name_(std::move(name))
    , descriptor_(std::move(descriptor))
    , bootstrapHandle_(std::move(bootstrapHandle))
    , args_(std::move(args))
{
    validateDynamic(std::static_pointer_cast<const Type>(descriptor_), bootstrapHandle_, args_);
}

}   //namespace jblw::type
