#pragma once

namespace jblw::constant {

class OfString;
class OfMethodHandle;
class OfType;
class OfDynamic;
class OfLong;
class OfDouble;
class OfInt;
class OfFloat;

///@brief 对所有 BLW 常量叶类型进行双分派的 visitor。
class ConstantSink
{
public:
    virtual ~ConstantSink() = default;

    virtual void acceptString(const OfString& value)             = 0;
    virtual void acceptMethodHandle(const OfMethodHandle& value) = 0;
    virtual void acceptType(const OfType& value)                 = 0;
    virtual void acceptDynamic(const OfDynamic& value)           = 0;
    virtual void acceptLong(const OfLong& value)                 = 0;
    virtual void acceptDouble(const OfDouble& value)             = 0;
    virtual void acceptInt(const OfInt& value)                   = 0;
    virtual void acceptFloat(const OfFloat& value)               = 0;
};

}   //namespace jblw::constant
