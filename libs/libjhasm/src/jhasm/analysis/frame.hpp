#pragma once

#include "value.hpp"

#include <vector>

namespace jhasm::analysis {

///@brief 某条 JVM 指令执行前的局部变量与操作数栈快照。
struct Frame final
{
    std::vector<Value> locals;
    std::vector<Value> stack;
};

}   //namespace jhasm::analysis
