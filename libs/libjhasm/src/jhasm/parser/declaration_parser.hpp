#pragma once

#include "parsing_result.hpp"

#include <vector>

namespace jhasm::parser {

///@brief 把 Jasm Token 转为 primitive AST。
///
///parser 深拷贝所需 Token；返回节点不依赖输入 vector 生命周期。语法错误会保存
///在 ParsingResult 中，并尽量返回已完成的顶层节点。
class DeclarationParser final
{
public:
    ParsingResult parseAny(const std::vector<Token>& tokens);
    ParsingResult parseDeclarations(const std::vector<Token>& tokens);
};

}   //namespace jhasm::parser
