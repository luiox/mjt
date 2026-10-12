#pragma once

namespace jhasm::parser {

///@brief Jasm tokenizer 产生的 Token 类别。
enum class TokenType
{
    Number,       ///< 整数或浮点数字面量。
    String,       ///< 不含双引号定界符的字符串内容。
    Character,    ///< 不含单引号定界符的字符内容。
    Identifier,   ///< 非空白且不属于其他类别的文本。
    Operator,     ///< `{`、`}`、`:` 或 `,`。
    Comment,      ///< 不含注释定界符的注释内容。
};

}   //namespace jhasm::parser
