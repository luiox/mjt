#pragma once

#include <libca/str/utf8_string.hpp>

namespace jhasm::analysis {

///@brief 数据流 frame 中一个 JVM 值的 owning 表示。
class Value final
{
public:
    Value() = default;
    explicit Value(ca::str::Utf8String descriptor)
        : descriptor_(std::move(descriptor))
    {}

    ca::str::Utf8StringRef descriptor() const noexcept { return descriptor_.ref(); }
    bool                   isUninitialized() const noexcept { return descriptor_.is_empty(); }

private:
    ca::str::Utf8String descriptor_;
};

}   //namespace jhasm::analysis
