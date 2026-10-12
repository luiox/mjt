#pragma once

#include <libca/core/datatype.hpp>
#include <libca/str/utf8_string.hpp>

namespace jblw::type {

///@brief 类型描述符解析或类型操作失败的稳定错误类别。
enum class TypeErrorCode : ca::u8
{
    EMPTY_DESCRIPTOR,
    UNEXPECTED_END,
    UNEXPECTED_CHARACTER,
    UNTERMINATED_INSTANCE,
    EXPECTED_CLASS_TYPE,
    EXPECTED_ARRAY_TYPE,
    EXPECTED_METHOD_TYPE,
    TRAILING_CHARACTERS,
    INVALID_VOID_USE,
    CANNOT_BOX_VOID,
    CANNOT_WIDEN_VOID,
    INVALID_PRIMITIVE_KIND,
    INVALID_PRIMITIVE_SIZE,
};

///@brief 可复制的类型错误值。
///
///@p offset 是 UTF-8 descriptor 的字节偏移；@p actual 保存导致错误的 byte/code。
struct TypeError final
{
    TypeErrorCode code;
    ca::usize     offset = 0;
    ca::u32       actual = 0;

    ///@brief 生成人类可读的 UTF-8 错误消息。
    ca::str::Utf8String message() const;

    ///@brief 比较错误类别、位置和实际输入是否完全相同。
    friend bool operator==(const TypeError& lhs, const TypeError& rhs) noexcept
    {
        return lhs.code == rhs.code && lhs.offset == rhs.offset && lhs.actual == rhs.actual;
    }
};

}   //namespace jblw::type
