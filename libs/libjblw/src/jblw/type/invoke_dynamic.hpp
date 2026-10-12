#pragma once

#include "dynamic_alike.hpp"
#include "method_type.hpp"

namespace jblw::type {

///@brief JVM invokedynamic 调用点；对象本身保留上游身份语义。
class InvokeDynamic final : public DynamicAlike
{
public:
    InvokeDynamic(ca::str::Utf8String name, MethodTypePtr descriptor,
                  MethodHandlePtr bootstrapHandle, std::vector<constant::ConstantPtr> args);

    ca::str::Utf8StringRef name() const noexcept override { return name_.ref(); }
    const MethodType&      type() const noexcept override { return *descriptor_; }
    const MethodTypePtr&   typePtr() const noexcept { return descriptor_; }
    const MethodHandle&    methodHandle() const noexcept override { return *bootstrapHandle_; }
    const MethodHandlePtr& methodHandlePtr() const noexcept { return bootstrapHandle_; }
    const std::vector<constant::ConstantPtr>& args() const noexcept override { return args_; }
    DynamicKind getType() const noexcept override { return DynamicKind::INVOKE; }

private:
    ca::str::Utf8String                name_;
    MethodTypePtr                      descriptor_;
    MethodHandlePtr                    bootstrapHandle_;
    std::vector<constant::ConstantPtr> args_;
};

using InvokeDynamicPtr = std::shared_ptr<const InvokeDynamic>;

}   //namespace jblw::type
