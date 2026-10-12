#pragma once

#include "constant.hpp"

namespace jblw::constant {

class OfLong final : public Constant
{
public:
    explicit OfLong(ca::i64 value) noexcept
        : value_(value)
    {}

    ca::i64      value() const noexcept { return value_; }
    void         accept(ConstantSink& sink) const override;
    ConstantKind getType() const noexcept override { return ConstantKind::LONG; }
    bool         equals(const Constant& other) const noexcept override;
    ca::i32      hashCode() const noexcept override;

private:
    ca::i64 value_;
};

}   //namespace jblw::constant
