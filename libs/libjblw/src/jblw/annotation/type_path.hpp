#pragma once

#include <libca/core/datatype.hpp>
#include <libca/str/utf8_string.hpp>

#include <optional>
#include <vector>

namespace jblw::annotation {

///@brief JVMS 4.7.20.2 type_path 的拥有型表示。
class TypePath final
{
public:
    static constexpr ca::u8 ARRAY_ELEMENT  = 0;
    static constexpr ca::u8 INNER_TYPE     = 1;
    static constexpr ca::u8 WILDCARD_BOUND = 2;
    static constexpr ca::u8 TYPE_ARGUMENT  = 3;

    struct Step
    {
        ca::u8 kind;
        ca::u8 argument;

        bool operator==(const Step& other) const noexcept
        {
            return kind == other.kind && argument == other.argument;
        }
    };

    explicit TypePath(std::vector<Step> steps);

    ca::usize length() const noexcept { return steps_.size(); }
    ca::u8    step(ca::usize index) const { return steps_.at(index).kind; }
    ca::u8    stepArgument(ca::usize index) const { return steps_.at(index).argument; }
    const std::vector<Step>& steps() const noexcept { return steps_; }

    ///@brief 解析 ASM 格式；空输入或 `_` 返回 nullopt，非法输入抛出 invalid_argument。
    static std::optional<TypePath> fromString(ca::str::Utf8StringRef value);
    ca::str::Utf8String            toString() const;
    bool    equals(const TypePath& other) const noexcept { return steps_ == other.steps_; }
    ca::i32 hashCode() const noexcept;

private:
    std::vector<Step> steps_;
};

bool operator==(const TypePath& lhs, const TypePath& rhs) noexcept;
bool operator!=(const TypePath& lhs, const TypePath& rhs) noexcept;

}   //namespace jblw::annotation
