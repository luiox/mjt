#pragma once

#include "jhasm/ast/specific.hpp"
#include "jhasm/error/result.hpp"

namespace jhasm::parser {

///@brief 把 primitive declarations 处理为 JVM specific AST，并校验 JVM 指令。
class ASTProcessor final
{
public:
    error::Result<std::vector<ast::ElementPtr>> process(
        const std::vector<ast::ElementPtr>& input) const;
};

}   //namespace jhasm::parser
