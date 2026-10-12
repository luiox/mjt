#pragma once

#include "class_type.hpp"
#include "dynamic_alike.hpp"

namespace jblw::type {

///@brief JVM CONSTANT_Dynamic；对象本身保留上游身份语义。
class ConstantDynamic final : public DynamicAlike
{
public:
    ConstantDynamic(ca::str::Utf8String name, ClassTypePtr descriptor,
                    MethodHandlePtr bootstrapHandle, std::vector<constant::ConstantPtr> args);

    ca::str::Utf8StringRef name() const noexcept override { return name_.ref(); }
    const ClassType&       type() const noexcept override { return *descriptor_; }
    const ClassTypePtr&    typePtr() const noexcept { return descriptor_; }
    const MethodHandle&    methodHandle() const noexcept override { return *bootstrapHandle_; }
    const MethodHandlePtr& methodHandlePtr() const noexcept { return bootstrapHandle_; }
    const std::vector<constant::ConstantPtr>& args() const noexcept override { return args_; }
    DynamicKind getType() const noexcept override { return DynamicKind::CONSTANT; }

private:
    ca::str::Utf8String                name_;
    ClassTypePtr                       descriptor_;
    MethodHandlePtr                    bootstrapHandle_;
    std::vector<constant::ConstantPtr> args_;
};

using ConstantDynamicPtr = std::shared_ptr<const ConstantDynamic>;

}   //namespace jblw::type
