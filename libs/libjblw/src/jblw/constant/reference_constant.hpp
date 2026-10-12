#pragma once

#include "constant.hpp"

namespace jblw::constant {

///@brief 持有拥有型引用值的常量接口。
template<typename T>
class ReferenceConstant : public Constant
{
public:
    virtual const T& value() const noexcept = 0;

protected:
    ReferenceConstant() = default;
};

}   //namespace jblw::constant
