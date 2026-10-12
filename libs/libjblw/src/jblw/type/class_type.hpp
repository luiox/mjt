#pragma once

#include "type.hpp"

namespace jblw::type {

///@brief 字段、参数、返回值或数组组件可使用的类型接口。
class ClassType : public Type
{
protected:
    ClassType() = default;
};

}   //namespace jblw::type
