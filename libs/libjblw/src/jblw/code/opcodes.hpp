#pragma once

#include <jbytecode/core/opcodes.hpp>

#include <libca/core/datatype.hpp>

namespace jblw::code {

///@brief BLW 使用的标准 JVM opcode 表，直接复用 libjbytecode 的权威定义。
using JavaOpcodes = jbytecode::core::Opcodes;

///@brief BLW 为规范化指令模型保留的扩展 opcode。
struct ExtensionOpcodes final
{
    static constexpr ca::i32 STRING_CONSTANT        = 203;
    static constexpr ca::i32 METHOD_HANDLE_CONSTANT = 204;
    static constexpr ca::i32 DYNAMIC_CONSTANT       = 205;
    static constexpr ca::i32 LONG_CONSTANT          = 206;
    static constexpr ca::i32 DOUBLE_CONSTANT        = 207;
    static constexpr ca::i32 INT_CONSTANT           = 208;
    static constexpr ca::i32 FLOAT_CONSTANT         = 209;
    static constexpr ca::i32 TYPE_CONSTANT          = 210;
    static constexpr ca::i32 ALLOCATE               = 211;
    static constexpr ca::i32 PRIMITIVE_CONVERSION   = 212;

    ExtensionOpcodes() = delete;
};

}   //namespace jblw::code
