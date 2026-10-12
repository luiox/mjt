#include "conversion.hpp"

#include "jblw/constant/constant_traits.hpp"
#include "jblw/constant/of_double.hpp"
#include "jblw/constant/of_dynamic.hpp"
#include "jblw/constant/of_float.hpp"
#include "jblw/constant/of_int.hpp"
#include "jblw/constant/of_long.hpp"
#include "jblw/constant/of_method_handle.hpp"
#include "jblw/constant/of_string.hpp"
#include "jblw/constant/of_type.hpp"
#include "jblw/type/types.hpp"

#include "libca/core/tag_cast.hpp"

#include <stdexcept>

namespace jblw::adapter {
namespace {
ca::str::Utf8String copy(ca::str::Utf8StringRef value)
{
    return ca::str::Utf8String::from_data(value.data(), value.byte_length());
}
}   //namespace

type::MethodHandlePtr wrapMethodHandle(const jbytecode::core::Handle& value)
{
    return std::make_shared<const type::MethodHandle>(
        value.getTag(),
        type::Types::objectTypeFromInternalName(value.getOwner()).unwrap(),
        copy(value.getName()),
        type::Types::typeFromDescriptor(value.getDesc()).unwrap(),
        value.isInterface());
}

jbytecode::core::Handle unwrapMethodHandle(const type::MethodHandle& value)
{
    return jbytecode::core::Handle(value.kind(),
                                   value.owner()->internalName(),
                                   value.name(),
                                   value.type()->descriptor(),
                                   value.isInterface());
}

type::ConstantDynamicPtr wrapConstantDynamic(const jbytecode::core::ConstantDynamic& value)
{
    std::vector<constant::ConstantPtr> arguments;
    arguments.reserve(value.getBootstrapMethodArguments().size());
    for (const auto& argument : value.getBootstrapMethodArguments()) {
        arguments.push_back(wrapConstant(argument));
    }
    type::TypePtr descriptor = type::Types::typeFromDescriptor(value.getDescriptor()).unwrap();
    if (descriptor->getType() == type::TypeKind::METHOD) {
        throw std::invalid_argument("ConstantDynamic descriptor must be a class type");
    }
    return std::make_shared<const type::ConstantDynamic>(
        copy(value.getName()),
        std::static_pointer_cast<const type::ClassType>(descriptor),
        wrapMethodHandle(value.getBootstrapMethod()),
        std::move(arguments));
}

jbytecode::core::ConstantDynamic unwrapConstantDynamic(const type::ConstantDynamic& value)
{
    std::vector<ca::core::Any> arguments;
    arguments.reserve(value.args().size());
    for (const auto& argument : value.args()) arguments.push_back(unwrapConstant(*argument));
    return jbytecode::core::ConstantDynamic(value.name(),
                                            value.type().descriptor(),
                                            unwrapMethodHandle(value.methodHandle()),
                                            std::move(arguments));
}

constant::ConstantPtr wrapConstant(const ca::core::Any& value)
{
    if (const auto* string = value.as<ca::str::Utf8StringRef>()) {
        return std::make_shared<const constant::OfString>(copy(*string));
    }
    if (const auto* string = value.as<ca::str::ZUtf8StringRef>()) {
        return std::make_shared<const constant::OfString>(copy(string->ref()));
    }
    if (const auto* string = value.as<std::string>()) {
        return std::make_shared<const constant::OfString>(
            ca::str::Utf8String(reinterpret_cast<const ca::u8*>(string->data()), string->size()));
    }
    if (const auto* number = value.as<ca::i32>()) {
        return std::make_shared<const constant::OfInt>(*number);
    }
    if (const auto* number = value.as<ca::i64>()) {
        return std::make_shared<const constant::OfLong>(*number);
    }
    if (const auto* number = value.as<ca::f32>()) {
        return std::make_shared<const constant::OfFloat>(*number);
    }
    if (const auto* number = value.as<ca::f64>()) {
        return std::make_shared<const constant::OfDouble>(*number);
    }
    if (const auto* handle = value.as<jbytecode::core::Handle>()) {
        return std::make_shared<const constant::OfMethodHandle>(wrapMethodHandle(*handle));
    }
    if (const auto* bytecodeType = value.as<jbytecode::core::Type>()) {
        //staticArena 退役：描述符仅瞬时喂给 typeFromDescriptor（产出自有类型对象），
        //函数级局部 arena 零驻留。
        ca::str::Utf8StringArena scratch;
        return std::make_shared<const constant::OfType>(
            type::Types::typeFromDescriptor(bytecodeType->getDescriptor(scratch)).unwrap());
    }
    if (const auto* dynamic = value.as<jbytecode::core::ConstantDynamic>()) {
        return std::make_shared<const constant::OfDynamic>(wrapConstantDynamic(*dynamic));
    }
    throw std::invalid_argument("Unsupported libjbytecode constant");
}

ca::core::Any unwrapConstant(const constant::Constant& value)
{
    switch (value.getType()) {
    case constant::ConstantKind::STRING:
        return ca::core::tag_cast::cast<constant::OfString>(&value)->value().ref();
    case constant::ConstantKind::METHOD_HANDLE:
        return unwrapMethodHandle(*ca::core::tag_cast::cast<constant::OfMethodHandle>(&value)->value());
    case constant::ConstantKind::TYPE:
        return jbytecode::core::Type::getType(
            ca::core::tag_cast::cast<constant::OfType>(&value)->value()->descriptor());
    case constant::ConstantKind::DYNAMIC:
        return unwrapConstantDynamic(*ca::core::tag_cast::cast<constant::OfDynamic>(&value)->value());
    case constant::ConstantKind::LONG: return ca::core::tag_cast::cast<constant::OfLong>(&value)->value();
    case constant::ConstantKind::DOUBLE: return ca::core::tag_cast::cast<constant::OfDouble>(&value)->value();
    case constant::ConstantKind::INT: return ca::core::tag_cast::cast<constant::OfInt>(&value)->value();
    case constant::ConstantKind::FLOAT: return ca::core::tag_cast::cast<constant::OfFloat>(&value)->value();
    }
    throw std::invalid_argument("Unsupported BLW constant");
}

}   //namespace jblw::adapter
