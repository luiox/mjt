#pragma once

#include "method_handle.hpp"

#include <vector>

namespace jblw::constant {
class Constant;
using ConstantPtr = std::shared_ptr<const Constant>;
}   //namespace jblw::constant

namespace jblw::type {

enum class DynamicKind : ca::u8
{
    CONSTANT,
    INVOKE,
};

///@brief constant-dynamic 与 invoke-dynamic 的公共只读接口。
class DynamicAlike
{
public:
    virtual ~DynamicAlike() = default;

    virtual ca::str::Utf8StringRef                    name() const noexcept         = 0;
    virtual const Type&                               type() const noexcept         = 0;
    virtual const MethodHandle&                       methodHandle() const noexcept = 0;
    virtual const std::vector<constant::ConstantPtr>& args() const noexcept         = 0;
    virtual DynamicKind                               getType() const noexcept      = 0;

protected:
    DynamicAlike() = default;
};

}   //namespace jblw::type
