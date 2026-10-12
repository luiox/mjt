#pragma once

#include "constant.hpp"

namespace jblw::constant {

class OfDouble final : public Constant
{
public:
    explicit OfDouble(ca::f64 value) noexcept
        : value_(value)
    {}

    ca::f64      value() const noexcept { return value_; }
    void         accept(ConstantSink& sink) const override;
    ConstantKind getType() const noexcept override { return ConstantKind::DOUBLE; }
    bool         equals(const Constant& other) const noexcept override;
    ca::i32      hashCode() const noexcept override;

private:
    ca::f64 value_;
};

}   //namespace jblw::constant
