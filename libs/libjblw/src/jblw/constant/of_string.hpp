#pragma once

#include "reference_constant.hpp"

#include <libca/str/utf8_string.hpp>

namespace jblw::constant {

class OfString final : public ReferenceConstant<ca::str::Utf8String>
{
public:
    explicit OfString(ca::str::Utf8String value);

    const ca::str::Utf8String& value() const noexcept override { return value_; }
    void                       accept(ConstantSink& sink) const override;
    ConstantKind               getType() const noexcept override { return ConstantKind::STRING; }
    bool                       equals(const Constant& other) const noexcept override;
    ca::i32                    hashCode() const noexcept override { return hashCode_; }

private:
    ca::str::Utf8String value_;
    ca::i32             hashCode_;
};

}   //namespace jblw::constant
