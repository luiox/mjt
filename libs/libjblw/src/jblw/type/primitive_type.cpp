#include "primitive_type.hpp"

#include "primitive_kind.hpp"
#include "types.hpp"

namespace jblw::type {

PrimitiveType::PrimitiveType(ca::str::Utf8StringRef descriptor, ca::i32 kind,
                             ca::str::Utf8StringRef name)
    : descriptor_(ca::str::Utf8String::from_data(descriptor.data(), descriptor.byte_length()))
    , kind_(kind)
    , name_(ca::str::Utf8String::from_data(name.data(), name.byte_length()))
{}

ca::Result<InstanceTypePtr, TypeError> PrimitiveType::box() const
{
    switch (kind_) {
    case PrimitiveKind::T_BOOLEAN: return ca::Ok(Types::BOX_BOOLEAN);
    case PrimitiveKind::T_BYTE: return ca::Ok(Types::BOX_BYTE);
    case PrimitiveKind::T_CHAR: return ca::Ok(Types::BOX_CHAR);
    case PrimitiveKind::T_SHORT: return ca::Ok(Types::BOX_SHORT);
    case PrimitiveKind::T_INT: return ca::Ok(Types::BOX_INT);
    case PrimitiveKind::T_FLOAT: return ca::Ok(Types::BOX_FLOAT);
    case PrimitiveKind::T_LONG: return ca::Ok(Types::BOX_LONG);
    case PrimitiveKind::T_DOUBLE: return ca::Ok(Types::BOX_DOUBLE);
    case PrimitiveKind::T_VOID: return ca::Err(TypeError{TypeErrorCode::CANNOT_BOX_VOID, 0, 0});
    default:
        return ca::Err(
            TypeError{TypeErrorCode::INVALID_PRIMITIVE_KIND, 0, static_cast<ca::u32>(kind_)});
    }
}

ca::Result<PrimitiveTypePtr, TypeError> PrimitiveType::widen(const PrimitiveType& other) const
{
    if (kind_ == PrimitiveKind::T_VOID) {
        return ca::Err(TypeError{TypeErrorCode::CANNOT_WIDEN_VOID, 0, 0});
    }
    if (kind_ == other.kind_) return Types::primitiveOfKind(kind_);

    ca::i32 result = kind_;
    switch (kind_) {
    case PrimitiveKind::T_BOOLEAN:
        if (other.kind_ == PrimitiveKind::T_BYTE || other.kind_ == PrimitiveKind::T_SHORT ||
            other.kind_ == PrimitiveKind::T_CHAR || other.kind_ == PrimitiveKind::T_INT ||
            other.kind_ == PrimitiveKind::T_LONG || other.kind_ == PrimitiveKind::T_DOUBLE ||
            other.kind_ == PrimitiveKind::T_FLOAT) {
            result = other.kind_;
        }
        break;
    case PrimitiveKind::T_BYTE:
        if (other.kind_ == PrimitiveKind::T_SHORT || other.kind_ == PrimitiveKind::T_CHAR ||
            other.kind_ == PrimitiveKind::T_INT || other.kind_ == PrimitiveKind::T_LONG ||
            other.kind_ == PrimitiveKind::T_DOUBLE || other.kind_ == PrimitiveKind::T_FLOAT) {
            result = other.kind_;
        }
        break;
    case PrimitiveKind::T_CHAR:
    case PrimitiveKind::T_SHORT:
        if (other.kind_ == PrimitiveKind::T_INT || other.kind_ == PrimitiveKind::T_LONG ||
            other.kind_ == PrimitiveKind::T_DOUBLE || other.kind_ == PrimitiveKind::T_FLOAT) {
            result = other.kind_;
        }
        break;
    case PrimitiveKind::T_INT:
        if (other.kind_ == PrimitiveKind::T_LONG || other.kind_ == PrimitiveKind::T_DOUBLE ||
            other.kind_ == PrimitiveKind::T_FLOAT) {
            result = other.kind_;
        }
        break;
    case PrimitiveKind::T_FLOAT:
        if (other.kind_ == PrimitiveKind::T_DOUBLE) result = other.kind_;
        break;
    case PrimitiveKind::T_LONG: result = PrimitiveKind::T_LONG; break;
    case PrimitiveKind::T_DOUBLE: result = PrimitiveKind::T_DOUBLE; break;
    default:
        return ca::Err(
            TypeError{TypeErrorCode::INVALID_PRIMITIVE_KIND, 0, static_cast<ca::u32>(kind_)});
    }
    return Types::primitiveOfKind(result);
}

bool PrimitiveType::equals(const Type& other) const noexcept
{
    return other.getType() == TypeKind::PRIMITIVE &&
           kind_ == static_cast<const PrimitiveType&>(other).kind_;
}

}   //namespace jblw::type
