#pragma once

#include "jblw/classfile/model.hpp"

#include <libca/core/datatype.hpp>

#include <vector>

namespace jbytecode::core {
class ICommonSuperClassProvider;
}

namespace jblw::adapter {

///@brief 使用 libjbytecode 读取和写入 BLW classfile 模型。
///
///读取结果拥有全部字符串存储。写出时重新构建常量池并计算 stack-map frames，
///不保证原始 class 字节或常量池排列不变。当前 libjbytecode tree/BLW code 模型
///未保留的字段 type annotation 与指令、局部变量和异常处理器 type annotation
///不参与往返。
class BytecodeLibrary final
{
public:
    static classfile::ClassFileViewPtr read(const std::vector<ca::u8>& bytes);
    static std::vector<ca::u8>         write(const classfile::ClassFileView& classFile);
    ///@brief 使用调用方提供的类型层级计算 StackMap common superclass。
    static std::vector<ca::u8> write(const classfile::ClassFileView&             classFile,
                                     jbytecode::core::ICommonSuperClassProvider& provider);

    BytecodeLibrary() = delete;
};

}   //namespace jblw::adapter
