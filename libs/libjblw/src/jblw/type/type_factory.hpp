#pragma once

#include "constant_dynamic.hpp"
#include "invoke_dynamic.hpp"

namespace jblw::type {

///@brief 动态类型对象工厂契约。
///
///实现方可通过 arena、interning 或普通 shared ownership 构造对象；调用方只依赖
///不可变共享指针结果。
class TypeFactory
{
public:
    virtual ~TypeFactory() = default;

    virtual MethodHandlePtr methodHandle(ca::i32 kind, ObjectTypePtr owner,
                                         ca::str::Utf8String name, TypePtr descriptor,
                                         bool isInterface) = 0;

    virtual ConstantDynamicPtr constantDynamic(ca::str::Utf8String name, ClassTypePtr descriptor,
                                               MethodHandlePtr                    bootstrapHandle,
                                               std::vector<constant::ConstantPtr> args) = 0;

    virtual InvokeDynamicPtr invokeDynamic(ca::str::Utf8String name, MethodTypePtr descriptor,
                                           MethodHandlePtr                    bootstrapHandle,
                                           std::vector<constant::ConstantPtr> args) = 0;

protected:
    TypeFactory() = default;
};

}   //namespace jblw::type
