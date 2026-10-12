#pragma once

#include "constant.hpp"

namespace jblw::constant {

class OfInt final : public Constant
{
public:
    explicit OfInt(ca::i32 value) noexcept
        : value_(value)
    {}

    ca::i32      value() const noexcept { return value_; }
    void         accept(ConstantSink& sink) const override;
    ConstantKind getType() const noexcept override { return ConstantKind::INT; }
    bool         equals(const Constant& other) const noexcept override;
    ca::i32      hashCode() const noexcept override { return value_; }

private:
    ca::i32 value_;
};

}   //namespace jblw::constant
