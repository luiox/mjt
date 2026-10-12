#pragma once

#include "reference_constant.hpp"

#include "jblw/type/constant_dynamic.hpp"

namespace jblw::constant {

class OfDynamic final : public ReferenceConstant<type::ConstantDynamicPtr>
{
public:
    explicit OfDynamic(type::ConstantDynamicPtr value);

    const type::ConstantDynamicPtr& value() const noexcept override { return value_; }
    void                            accept(ConstantSink& sink) const override;
    ConstantKind getType() const noexcept override { return ConstantKind::DYNAMIC; }
    bool         equals(const Constant& other) const noexcept override;
    ca::i32      hashCode() const noexcept override;

private:
    type::ConstantDynamicPtr value_;
};

}   //namespace jblw::constant
