#pragma once

#include "class_type.hpp"

namespace jblw::type {

///@brief JVM reference type 接口，包括实例类型和数组类型。
class ObjectType : public ClassType
{
public:
    ///@brief 返回 internal name；数组类型返回其 descriptor。
    virtual ca::str::Utf8StringRef internalName() const noexcept = 0;

    ///@brief 返回点分隔 external name 的非拥有视图。
    virtual ca::str::Utf8StringRef externalName() const noexcept = 0;

protected:
    ObjectType() = default;
};

}   //namespace jblw::type
