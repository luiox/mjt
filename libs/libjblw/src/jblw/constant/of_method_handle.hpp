#pragma once

#include "reference_constant.hpp"

#include "jblw/type/method_handle.hpp"

namespace jblw::constant {

class OfMethodHandle final : public ReferenceConstant<type::MethodHandlePtr>
{
public:
    explicit OfMethodHandle(type::MethodHandlePtr value);

    const type::MethodHandlePtr& value() const noexcept override { return value_; }
    void                         accept(ConstantSink& sink) const override;
    ConstantKind getType() const noexcept override { return ConstantKind::METHOD_HANDLE; }
    bool         equals(const Constant& other) const noexcept override;
    ca::i32      hashCode() const noexcept override { return value_->hashCode(); }

private:
    type::MethodHandlePtr value_;
};

}   //namespace jblw::constant
