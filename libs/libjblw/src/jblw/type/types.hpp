#pragma once

#include "array_type.hpp"
#include "instance_type.hpp"
#include "method_type.hpp"
#include "primitive_type.hpp"
#include "type_error.hpp"

#include <libca/core/result.hpp>

#include <vector>

namespace jblw::type {

///@brief BLW 类型 singleton 与 descriptor 工厂。
class Types final
{
public:
    ///@name Primitive singleton types。
    ///@{
    static const PrimitiveTypePtr VOID;
    static const PrimitiveTypePtr LONG;
    static const PrimitiveTypePtr DOUBLE;
    static const PrimitiveTypePtr INT;
    static const PrimitiveTypePtr FLOAT;
    static const PrimitiveTypePtr CHAR;
    static const PrimitiveTypePtr SHORT;
    static const PrimitiveTypePtr BYTE;
    static const PrimitiveTypePtr BOOLEAN;
    ///@}

    ///@name 常用 java.lang instance types。
    ///@{
    static const InstanceTypePtr OBJECT;
    static const InstanceTypePtr STRING;
    static const InstanceTypePtr BOX_VOID;
    static const InstanceTypePtr BOX_LONG;
    static const InstanceTypePtr BOX_DOUBLE;
    static const InstanceTypePtr BOX_INT;
    static const InstanceTypePtr BOX_FLOAT;
    static const InstanceTypePtr BOX_CHAR;
    static const InstanceTypePtr BOX_SHORT;
    static const InstanceTypePtr BOX_BYTE;
    static const InstanceTypePtr BOX_BOOLEAN;
    ///@}

    ///@name JVM local/stack slot categories。
    ///@{
    static constexpr ca::i32 CATEGORY1 = 1;
    static constexpr ca::i32 CATEGORY2 = 2;
    ///@}

    ///@brief 解析一个完整 JVM descriptor。
    static ca::Result<TypePtr, TypeError> typeFromDescriptor(ca::str::Utf8StringRef descriptor);

    ///@brief 从 `L...;` descriptor 创建实例类型。
    static ca::Result<InstanceTypePtr, TypeError> instanceTypeFromDescriptor(
        ca::str::Utf8StringRef descriptor);

    ///@brief 从 internal name 创建实例类型。
    static InstanceTypePtr instanceTypeFromInternalName(ca::str::Utf8StringRef internalName);

    ///@brief internal name 以 `[` 开头时解析数组，否则创建实例类型。
    static ca::Result<ObjectTypePtr, TypeError> objectTypeFromInternalName(
        ca::str::Utf8StringRef internalName);

    ///@brief 解析完整数组 descriptor。
    static ca::Result<ArrayTypePtr, TypeError> arrayTypeFromDescriptor(
        ca::str::Utf8StringRef descriptor);

    ///@brief 从直接组件类型创建数组类型。
    static ArrayTypePtr arrayType(ClassTypePtr component);

    ///@brief 解析完整 method descriptor。
    static ca::Result<MethodTypePtr, TypeError> methodType(ca::str::Utf8StringRef descriptor);

    ///@brief 从 return type 和 parameter types 创建 method 类型。
    static MethodTypePtr methodType(ClassTypePtr              returnType,
                                    std::vector<ClassTypePtr> parameterTypes = {});

    ///@brief 从 external name 创建实例类型。
    static InstanceTypePtr instanceTypeFromExternalName(ca::str::Utf8StringRef externalName);

    ///@brief 从 external array name 创建数组类型。
    static ca::Result<ArrayTypePtr, TypeError> arrayTypeFromExternalName(
        ca::str::Utf8StringRef externalName);

    ///@brief 从单字符 descriptor 查找 primitive singleton。
    static ca::Result<PrimitiveTypePtr, TypeError> primitiveFromDesc(
        ca::str::Utf8StringRef descriptor);

    ///@brief 从 descriptor tag 查找 primitive singleton。
    static ca::Result<PrimitiveTypePtr, TypeError> primitiveFromChar(char      tag,
                                                                     ca::usize offset = 0);

    ///@brief 从 PrimitiveKind code 查找 primitive singleton。
    static ca::Result<PrimitiveTypePtr, TypeError> primitiveOfKind(ca::i32 kind);

    ///@brief 判断输入是否为单字符 primitive descriptor。
    static bool isPrimitive(ca::str::Utf8StringRef descriptor) noexcept;

    ///@brief 返回 JVM local/stack category。
    static ca::i32 category(const ClassType& type) noexcept;

    ///@brief 返回 method parameters 占用的 JVM slots。
    static ca::i32 parametersSize(const MethodType& type) noexcept;

    ///@brief 返回 primitive 的字节宽度；void 返回错误。
    static ca::Result<ca::i32, TypeError> sizeOf(const PrimitiveType& type);

    ///@brief 把点分隔 external name 转为 slash 分隔 internal name。
    static ca::str::Utf8String internalName(ca::str::Utf8StringRef externalName);

    ///@brief 把 slash 分隔 internal name 转为点分隔 external name。
    static ca::str::Utf8String externalName(ca::str::Utf8StringRef internalName);

    Types() = delete;
};

}   //namespace jblw::type
