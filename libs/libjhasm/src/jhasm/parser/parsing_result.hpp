#pragma once

#include "jhasm/ast/primitive.hpp"
#include "jhasm/error/error.hpp"

#include <vector>

namespace jhasm::parser {

///@brief Declaration parser 的拥有型部分结果与注释集合。
class ParsingResult
{
public:
    ParsingResult(std::vector<ast::ElementPtr> value, std::vector<error::Error> errors,
                  std::vector<std::shared_ptr<ast::Comment>> comments)
        : value_(std::move(value))
        , errors_(std::move(errors))
        , comments_(std::move(comments))
    {}

    bool                                has_err() const noexcept { return !errors_.empty(); }
    const std::vector<ast::ElementPtr>& get() const noexcept { return value_; }
    std::vector<ast::ElementPtr>&       get() noexcept { return value_; }
    const std::vector<error::Error>&    errors() const noexcept { return errors_; }
    const std::vector<std::shared_ptr<ast::Comment>>& comments() const noexcept
    {
        return comments_;
    }

private:
    std::vector<ast::ElementPtr>               value_;
    std::vector<error::Error>                  errors_;
    std::vector<std::shared_ptr<ast::Comment>> comments_;
};

}   //namespace jhasm::parser
