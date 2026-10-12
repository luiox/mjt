#pragma once

#include "jhasm/error/result.hpp"

#include <libca/core/datatype.hpp>
#include <libca/str/utf8_string.hpp>

#include <vector>

namespace jhasm::printer {

///@brief 将 JVM classfile bytes 打印为可重新汇编的规范化 Jasm。
class JvmPrinter final
{
public:
    error::Result<ca::str::Utf16String> print(const std::vector<ca::u8>& classBytes) const;

    ///@brief 将指定方法打印为可嵌入 class Jasm 的规范化声明。
    ///
    ///@param classBytes 已验证或可由 ClassReader 读取的完整 classfile 字节。
    ///@param name 方法名；构造器使用 `<init>`，类初始化器使用 `<clinit>`。
    ///@param descriptor JVM 方法描述符，用于消除重载歧义。
    ///@return 找到时返回单个 `.method` 声明；方法不存在或 class 无法读取时返回诊断。
    error::Result<ca::str::Utf16String> printMethod(const std::vector<ca::u8>&    classBytes,
                                                     ca::str::Utf8StringRef name,
                                                     ca::str::Utf8StringRef descriptor) const;
};

}   //namespace jhasm::printer
