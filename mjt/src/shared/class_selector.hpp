#pragma once

#include <string>

///@brief 用户输入 class 选择器 → JVM internal name 归一化。
///
///从 cmd_xref 的同名局部辅助提升为共享实现（xref / callgraph 共用）：
///必须先剥 .class 后缀（避免把后缀里的点替换成 /），再剥 L...; 描述符外层，
///最后把源码包分隔符 . 统一成 internal name 的 /。
namespace mjt {

inline std::string normalizeClassSelector(std::string value)
{
    if (value.size() > 6 && value.substr(value.size() - 6) == ".class") {
        value = value.substr(0, value.size() - 6);
    }
    if (value.size() >= 2 && value.front() == 'L' && value.back() == ';') {
        value = value.substr(1, value.size() - 2);
    }
    for (char& ch : value) {
        if (ch == '.') ch = '/';
    }
    return value;
}

}   //namespace mjt
