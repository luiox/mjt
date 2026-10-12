#include "types.hpp"

#include "primitive_kind.hpp"
#include "type_reader.hpp"

#include <libca/str/utf8_string.hpp>

namespace jblw::type {

namespace {
PrimitiveTypePtr primitive(const char* descriptor, ca::i32 kind, const char* name)
{
    return std::make_shared<const PrimitiveType>(ca::str::Utf8StringRef::from_cstr(descriptor),
                                                 kind,
                                                 ca::str::Utf8StringRef::from_cstr(name));
}
}   //namespace

const PrimitiveTypePtr Types::VOID    = primitive("V", PrimitiveKind::T_VOID, "void");
const PrimitiveTypePtr Types::LONG    = primitive("J", PrimitiveKind::T_LONG, "long");
const PrimitiveTypePtr Types::DOUBLE  = primitive("D", PrimitiveKind::T_DOUBLE, "double");
const PrimitiveTypePtr Types::INT     = primitive("I", PrimitiveKind::T_INT, "int");
const PrimitiveTypePtr Types::FLOAT   = primitive("F", PrimitiveKind::T_FLOAT, "float");
const PrimitiveTypePtr Types::CHAR    = primitive("C", PrimitiveKind::T_CHAR, "char");
const PrimitiveTypePtr Types::SHORT   = primitive("S", PrimitiveKind::T_SHORT, "short");
const PrimitiveTypePtr Types::BYTE    = primitive("B", PrimitiveKind::T_BYTE, "byte");
const PrimitiveTypePtr Types::BOOLEAN = primitive("Z", PrimitiveKind::T_BOOLEAN, "boolean");

const InstanceTypePtr Types::OBJECT =
    Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/Object"));
const InstanceTypePtr Types::STRING =
    Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/String"));
const InstanceTypePtr Types::BOX_VOID =
    Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/Void"));
const InstanceTypePtr Types::BOX_LONG =
    Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/Long"));
const InstanceTypePtr Types::BOX_DOUBLE =
    Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/Double"));
const InstanceTypePtr Types::BOX_INT =
    Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"));
const InstanceTypePtr Types::BOX_FLOAT =
    Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/Float"));
const InstanceTypePtr Types::BOX_CHAR =
    Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/Character"));
const InstanceTypePtr Types::BOX_SHORT =
    Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/Short"));
const InstanceTypePtr Types::BOX_BYTE =
    Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/Byte"));
const InstanceTypePtr Types::BOX_BOOLEAN =
    Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/Boolean"));

ca::Result<TypePtr, TypeError> Types::typeFromDescriptor(ca::str::Utf8StringRef descriptor)
{
    if (descriptor.is_empty()) {
        return ca::Err(TypeError{TypeErrorCode::EMPTY_DESCRIPTOR, 0, 0});
    }
    TypeReader reader(descriptor);
    auto       result = reader.required();
    if (result.is_err()) return ca::Err(result.unwrap_err());
    if (reader.position() != descriptor.byte_length()) {
        return ca::Err(TypeError{TypeErrorCode::TRAILING_CHARACTERS, reader.position(), 0});
    }
    return ca::Ok(result.unwrap());
}

ca::Result<InstanceTypePtr, TypeError> Types::instanceTypeFromDescriptor(
    ca::str::Utf8StringRef descriptor)
{
    auto result = typeFromDescriptor(descriptor);
    if (result.is_err()) return ca::Err(result.unwrap_err());
    TypePtr type = result.unwrap();
    if (type->getType() != TypeKind::INSTANCE) {
        return ca::Err(
            TypeError{TypeErrorCode::UNEXPECTED_CHARACTER,
                      0,
                      descriptor.is_empty() ? 0U : static_cast<ca::u32>(descriptor.byte_at(0))});
    }
    return ca::Ok(std::static_pointer_cast<const InstanceType>(type));
}

InstanceTypePtr Types::instanceTypeFromInternalName(ca::str::Utf8StringRef internalName)
{
    ca::str::Utf8StringBuilder descriptor;
    descriptor.append("L").append(internalName).append(";");
    auto name = ca::str::Utf8String::from_data(internalName.data(), internalName.byte_length());
    return std::make_shared<const InstanceType>(descriptor.build(), std::move(name));
}

ca::Result<ObjectTypePtr, TypeError> Types::objectTypeFromInternalName(
    ca::str::Utf8StringRef internalName)
{
    if (internalName.is_empty()) {
        return ca::Err(TypeError{TypeErrorCode::EMPTY_DESCRIPTOR, 0, 0});
    }
    if (internalName.byte_at(0) == '[') {
        auto array = arrayTypeFromDescriptor(internalName);
        if (array.is_err()) return ca::Err(array.unwrap_err());
        return ca::Ok(std::static_pointer_cast<const ObjectType>(array.unwrap()));
    }
    return ca::Ok(
        std::static_pointer_cast<const ObjectType>(instanceTypeFromInternalName(internalName)));
}

ca::Result<ArrayTypePtr, TypeError> Types::arrayTypeFromDescriptor(
    ca::str::Utf8StringRef descriptor)
{
    auto result = typeFromDescriptor(descriptor);
    if (result.is_err()) return ca::Err(result.unwrap_err());
    TypePtr type = result.unwrap();
    if (type->getType() != TypeKind::ARRAY) {
        return ca::Err(TypeError{TypeErrorCode::EXPECTED_ARRAY_TYPE, 0, 0});
    }
    return ca::Ok(std::static_pointer_cast<const ArrayType>(type));
}

ArrayTypePtr Types::arrayType(ClassTypePtr component)
{
    return std::make_shared<const ArrayType>(std::move(component));
}

ca::Result<MethodTypePtr, TypeError> Types::methodType(ca::str::Utf8StringRef descriptor)
{
    auto result = typeFromDescriptor(descriptor);
    if (result.is_err()) return ca::Err(result.unwrap_err());
    TypePtr type = result.unwrap();
    if (type->getType() != TypeKind::METHOD) {
        return ca::Err(TypeError{TypeErrorCode::EXPECTED_METHOD_TYPE, 0, 0});
    }
    return ca::Ok(std::static_pointer_cast<const MethodType>(type));
}

MethodTypePtr Types::methodType(ClassTypePtr returnType, std::vector<ClassTypePtr> parameterTypes)
{
    return std::make_shared<const MethodType>(std::move(returnType), std::move(parameterTypes));
}

InstanceTypePtr Types::instanceTypeFromExternalName(ca::str::Utf8StringRef externalNameValue)
{
    auto name = internalName(externalNameValue);
    return instanceTypeFromInternalName(name.ref());
}

ca::Result<ArrayTypePtr, TypeError> Types::arrayTypeFromExternalName(
    ca::str::Utf8StringRef externalNameValue)
{
    auto name = internalName(externalNameValue);
    return arrayTypeFromDescriptor(name.ref());
}

ca::Result<PrimitiveTypePtr, TypeError> Types::primitiveFromDesc(ca::str::Utf8StringRef descriptor)
{
    if (descriptor.byte_length() != 1) {
        return ca::Err(TypeError{descriptor.is_empty() ? TypeErrorCode::EMPTY_DESCRIPTOR
                                                       : TypeErrorCode::TRAILING_CHARACTERS,
                                 descriptor.byte_length(),
                                 0});
    }
    return primitiveFromChar(static_cast<char>(descriptor.byte_at(0)));
}

ca::Result<PrimitiveTypePtr, TypeError> Types::primitiveFromChar(char tag, ca::usize offset)
{
    switch (tag) {
    case 'V': return ca::Ok(VOID);
    case 'J': return ca::Ok(LONG);
    case 'D': return ca::Ok(DOUBLE);
    case 'I': return ca::Ok(INT);
    case 'F': return ca::Ok(FLOAT);
    case 'C': return ca::Ok(CHAR);
    case 'S': return ca::Ok(SHORT);
    case 'B': return ca::Ok(BYTE);
    case 'Z': return ca::Ok(BOOLEAN);
    default:
        return ca::Err(TypeError{TypeErrorCode::UNEXPECTED_CHARACTER,
                                 offset,
                                 static_cast<ca::u32>(static_cast<ca::u8>(tag))});
    }
}

ca::Result<PrimitiveTypePtr, TypeError> Types::primitiveOfKind(ca::i32 kind)
{
    switch (kind) {
    case PrimitiveKind::T_BOOLEAN: return ca::Ok(BOOLEAN);
    case PrimitiveKind::T_CHAR: return ca::Ok(CHAR);
    case PrimitiveKind::T_FLOAT: return ca::Ok(FLOAT);
    case PrimitiveKind::T_DOUBLE: return ca::Ok(DOUBLE);
    case PrimitiveKind::T_BYTE: return ca::Ok(BYTE);
    case PrimitiveKind::T_SHORT: return ca::Ok(SHORT);
    case PrimitiveKind::T_INT: return ca::Ok(INT);
    case PrimitiveKind::T_LONG: return ca::Ok(LONG);
    case PrimitiveKind::T_VOID: return ca::Ok(VOID);
    default:
        return ca::Err(
            TypeError{TypeErrorCode::INVALID_PRIMITIVE_KIND, 0, static_cast<ca::u32>(kind)});
    }
}

bool Types::isPrimitive(ca::str::Utf8StringRef descriptor) noexcept
{
    if (descriptor.byte_length() != 1) return false;
    switch (descriptor.byte_at(0)) {
    case 'V':
    case 'J':
    case 'D':
    case 'I':
    case 'F':
    case 'C':
    case 'S':
    case 'B':
    case 'Z': return true;
    default: return false;
    }
}

ca::i32 Types::category(const ClassType& type) noexcept
{
    if (type.getType() == TypeKind::PRIMITIVE) {
        const ca::i32 kind = static_cast<const PrimitiveType&>(type).kind();
        if (kind == PrimitiveKind::T_LONG || kind == PrimitiveKind::T_DOUBLE) return CATEGORY2;
    }
    return CATEGORY1;
}

ca::i32 Types::parametersSize(const MethodType& type) noexcept
{
    ca::i32 size = 0;
    for (const auto& parameter : type.parameterTypes()) size += category(*parameter);
    return size;
}

ca::Result<ca::i32, TypeError> Types::sizeOf(const PrimitiveType& type)
{
    switch (type.kind()) {
    case PrimitiveKind::T_BOOLEAN:
    case PrimitiveKind::T_BYTE: return ca::Ok(1);
    case PrimitiveKind::T_CHAR:
    case PrimitiveKind::T_SHORT: return ca::Ok(2);
    case PrimitiveKind::T_FLOAT:
    case PrimitiveKind::T_INT: return ca::Ok(4);
    case PrimitiveKind::T_DOUBLE:
    case PrimitiveKind::T_LONG: return ca::Ok(8);
    default:
        return ca::Err(
            TypeError{TypeErrorCode::INVALID_PRIMITIVE_SIZE, 0, static_cast<ca::u32>(type.kind())});
    }
}

ca::str::Utf8String Types::internalName(ca::str::Utf8StringRef externalNameValue)
{
    return externalNameValue.replace_all(ca::str::Utf8StringRef::from_cstr("."),
                                         ca::str::Utf8StringRef::from_cstr("/"));
}

ca::str::Utf8String Types::externalName(ca::str::Utf8StringRef internalNameValue)
{
    return internalNameValue.replace_all(ca::str::Utf8StringRef::from_cstr("/"),
                                         ca::str::Utf8StringRef::from_cstr("."));
}

}   //namespace jblw::type
