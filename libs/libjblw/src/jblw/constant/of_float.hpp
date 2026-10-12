#pragma once

#include "constant.hpp"

namespace jblw::constant {

class OfFloat final : public Constant
{
public:
    explicit OfFloat(ca::f32 value) noexcept
        : value_(value)
    {}

    ca::f32      value() const noexcept { return value_; }
    void         accept(ConstantSink& sink) const override;
    ConstantKind getType() const noexcept override { return ConstantKind::FLOAT; }
    bool         equals(const Constant& other) const noexcept override;
    ca::i32      hashCode() const noexcept override;

private:
    ca::f32 value_;
};

}   //namespace jblw::constant
