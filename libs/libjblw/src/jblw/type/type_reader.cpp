#include "type_reader.hpp"

#include "primitive_kind.hpp"
#include "primitive_type.hpp"
#include "types.hpp"

namespace jblw::type {

namespace {
bool isVoid(const ClassType& type) noexcept
{
    return type.getType() == TypeKind::PRIMITIVE &&
           static_cast<const PrimitiveType&>(type).kind() == PrimitiveKind::T_VOID;
}

ClassTypePtr asClassType(const TypePtr& type)
{
    return std::static_pointer_cast<const ClassType>(type);
}
}   //namespace

TypeReader::ReadResult TypeReader::read()
{
    if (index_ >= descriptor_.byte_length()) return ca::Ok(std::optional<TypePtr>());
    auto result = readRequiredAtCurrent();
    if (result.is_err()) return ca::Err(result.unwrap_err());
    return ca::Ok(std::optional<TypePtr>(result.unwrap()));
}

ca::Result<TypePtr, TypeError> TypeReader::required()
{
    if (index_ >= descriptor_.byte_length()) {
        return ca::Err(TypeError{TypeErrorCode::UNEXPECTED_END, index_, 0});
    }
    return readRequiredAtCurrent();
}

ca::Result<ClassTypePtr, TypeError> TypeReader::requireClassType()
{
    auto result = required();
    if (result.is_err()) return ca::Err(result.unwrap_err());
    TypePtr type = result.unwrap();
    if (type->getType() == TypeKind::METHOD) {
        return ca::Err(TypeError{TypeErrorCode::EXPECTED_CLASS_TYPE, index_, 0});
    }
    return ca::Ok(asClassType(type));
}

ca::Result<TypePtr, TypeError> TypeReader::readRequiredAtCurrent()
{
    const ca::usize start = index_;
    const char      tag   = static_cast<char>(descriptor_.byte_at(index_++));

    if (tag == '[') {
        auto componentResult = requireClassType();
        if (componentResult.is_err()) return ca::Err(componentResult.unwrap_err());
        ClassTypePtr component = componentResult.unwrap();
        if (isVoid(*component)) {
            return ca::Err(TypeError{TypeErrorCode::INVALID_VOID_USE, start, 'V'});
        }
        return ca::Ok(std::static_pointer_cast<const Type>(Types::arrayType(std::move(component))));
    }

    if (tag == 'L') {
        const ca::usize nameStart = index_;
        while (index_ < descriptor_.byte_length() && descriptor_.byte_at(index_) != ';') ++index_;
        if (index_ >= descriptor_.byte_length()) {
            index_ = start;
            return ca::Err(TypeError{TypeErrorCode::UNTERMINATED_INSTANCE, start, 'L'});
        }
        if (index_ == nameStart) {
            index_ = start;
            return ca::Err(TypeError{TypeErrorCode::UNEXPECTED_CHARACTER, nameStart, ';'});
        }
        const auto name = descriptor_.slice(nameStart, index_);
        ++index_;
        return ca::Ok(
            std::static_pointer_cast<const Type>(Types::instanceTypeFromInternalName(name)));
    }

    auto primitive = Types::primitiveFromChar(tag, start);
    if (primitive.is_ok()) {
        return ca::Ok(std::static_pointer_cast<const Type>(primitive.unwrap()));
    }
    if (tag == '(') return readMethod(start);

    index_ = start;
    return ca::Err(TypeError{TypeErrorCode::UNEXPECTED_CHARACTER,
                             start,
                             static_cast<ca::u32>(static_cast<ca::u8>(tag))});
}

ca::Result<TypePtr, TypeError> TypeReader::readMethod(ca::usize openingOffset)
{
    std::vector<ClassTypePtr> parameters;
    while (index_ < descriptor_.byte_length() && descriptor_.byte_at(index_) != ')') {
        auto parameterResult = requireClassType();
        if (parameterResult.is_err()) {
            index_ = openingOffset;
            return ca::Err(parameterResult.unwrap_err());
        }
        ClassTypePtr parameter = parameterResult.unwrap();
        if (isVoid(*parameter)) {
            index_ = openingOffset;
            return ca::Err(TypeError{TypeErrorCode::INVALID_VOID_USE, openingOffset, 'V'});
        }
        parameters.push_back(std::move(parameter));
    }
    if (index_ >= descriptor_.byte_length()) {
        index_ = openingOffset;
        return ca::Err(TypeError{TypeErrorCode::UNEXPECTED_END, openingOffset, 0});
    }
    ++index_;

    auto returnResult = requireClassType();
    if (returnResult.is_err()) {
        index_ = openingOffset;
        return ca::Err(returnResult.unwrap_err());
    }
    return ca::Ok(std::static_pointer_cast<const Type>(
        Types::methodType(returnResult.unwrap(), std::move(parameters))));
}

}   //namespace jblw::type
