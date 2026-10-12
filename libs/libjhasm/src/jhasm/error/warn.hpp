#pragma once

#include "error.hpp"

namespace jhasm::error {

///@brief 表示一条拥有自身文本和位置的 Jasm 警告诊断。
class Warn final : public Error
{
public:
    using Error::Error;
};

}   //namespace jhasm::error
