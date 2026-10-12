#pragma once

#include "reference_constant.hpp"

#include "jblw/type/type.hpp"

namespace jblw::constant {

class OfType final : public ReferenceConstant<type::TypePtr>
{
public:
    explicit OfType(type::TypePtr value);

    const type::TypePtr& value() const noexcept override { return value_; }
    void                 accept(ConstantSink& sink) const override;
    ConstantKind         getType() const noexcept override { return ConstantKind::TYPE; }
    bool                 equals(const Constant& other) const noexcept override;
    ca::i32              hashCode() const noexcept override { return value_->hashCode(); }

private:
    type::TypePtr value_;
};

}   //namespace jblw::constant
