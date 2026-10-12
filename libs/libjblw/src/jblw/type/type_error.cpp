#include "type_error.hpp"

namespace jblw::type {

ca::str::Utf8String TypeError::message() const
{
    const char* text = "Unknown type error";
    switch (code) {
    case TypeErrorCode::EMPTY_DESCRIPTOR: text = "Descriptor cannot be empty"; break;
    case TypeErrorCode::UNEXPECTED_END: text = "Unexpected end of descriptor"; break;
    case TypeErrorCode::UNEXPECTED_CHARACTER: text = "Unexpected descriptor character"; break;
    case TypeErrorCode::UNTERMINATED_INSTANCE: text = "Expected instance type terminator"; break;
    case TypeErrorCode::EXPECTED_CLASS_TYPE: text = "Expected class type"; break;
    case TypeErrorCode::EXPECTED_ARRAY_TYPE: text = "Expected array type"; break;
    case TypeErrorCode::EXPECTED_METHOD_TYPE: text = "Expected method type"; break;
    case TypeErrorCode::TRAILING_CHARACTERS: text = "Trailing descriptor characters"; break;
    case TypeErrorCode::INVALID_VOID_USE: text = "Void is not valid in this position"; break;
    case TypeErrorCode::CANNOT_BOX_VOID: text = "Cannot box void"; break;
    case TypeErrorCode::CANNOT_WIDEN_VOID: text = "Cannot widen void"; break;
    case TypeErrorCode::INVALID_PRIMITIVE_KIND: text = "Unknown primitive kind"; break;
    case TypeErrorCode::INVALID_PRIMITIVE_SIZE: text = "Primitive type has no storage size"; break;
    }
    return ca::str::Utf8String::from_cstr(text);
}

}   //namespace jblw::type
