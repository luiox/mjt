#pragma once

#include "jblw/code/code.hpp"
#include "jblw/type/class_type.hpp"

#include <libca/str/utf8_string.hpp>

#include <optional>

namespace jblw::code::attribute {

///@brief LocalVariableTable/LocalVariableTypeTable 的公共只读接口。
class Local
{
public:
    virtual ~Local()                                                             = default;
    virtual const LabelPtr&                           start() const noexcept     = 0;
    virtual const LabelPtr&                           end() const noexcept       = 0;
    virtual ca::i32                                   index() const noexcept     = 0;
    virtual ca::str::Utf8StringRef                    name() const noexcept      = 0;
    virtual const type::ClassTypePtr&                 type() const noexcept      = 0;
    virtual const std::optional<ca::str::Utf8String>& signature() const noexcept = 0;

protected:
    Local() = default;
};

namespace generic {

///@brief BLW 默认 local 值实现。
class GenericLocal final : public Local
{
public:
    GenericLocal(LabelPtr start, LabelPtr end, ca::i32 index, ca::str::Utf8String name,
                 type::ClassTypePtr                 type,
                 std::optional<ca::str::Utf8String> signature = std::nullopt);

    const LabelPtr&                           start() const noexcept override { return start_; }
    const LabelPtr&                           end() const noexcept override { return end_; }
    ca::i32                                   index() const noexcept override { return index_; }
    ca::str::Utf8StringRef                    name() const noexcept override { return name_.ref(); }
    const type::ClassTypePtr&                 type() const noexcept override { return type_; }
    const std::optional<ca::str::Utf8String>& signature() const noexcept override
    {
        return signature_;
    }

    bool operator==(const GenericLocal& other) const noexcept;

private:
    LabelPtr                           start_;
    LabelPtr                           end_;
    ca::i32                            index_;
    ca::str::Utf8String                name_;
    type::ClassTypePtr                 type_;
    std::optional<ca::str::Utf8String> signature_;
};

}   //namespace generic

}   //namespace jblw::code::attribute
