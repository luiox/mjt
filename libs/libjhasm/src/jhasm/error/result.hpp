#pragma once

#include "error.hpp"
#include "warn.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace jhasm::error {

///@brief 保存部分值、errors 与 warnings 的 Jasm 操作结果。
///
///该类型刻意不使用二选一的 `ca::Result<T, E>`：即使存在 errors，value 也可以
///继续存在，供调用方检查部分 tokenizer/parser 输出。Result 拥有值和全部诊断。
///通过 getter 返回的指针或引用在 Result 被销毁、移动或赋值后失效。
template<typename T>
class Result
{
public:
    ///@brief 接管可选值与诊断集合的所有权。
    Result(std::optional<T> value, std::vector<Error> errors, std::vector<Warn> warnings)
        : value_(std::move(value))
        , errors_(std::move(errors))
        , warnings_(std::move(warnings))
    {}

    ///@brief 接管部分值与诊断集合的所有权。
    Result(T value, std::vector<Error> errors, std::vector<Warn> warnings)
        : value_(std::move(value))
        , errors_(std::move(errors))
        , warnings_(std::move(warnings))
    {}

    ///@brief 构造不带诊断的有值结果。
    static Result ok(T value) { return Result(std::move(value), {}, {}); }

    ///@brief 构造无值错误结果。
    static Result err(std::vector<Error> errors)
    {
        return Result(std::nullopt, std::move(errors), {});
    }

    ///@brief 值存在且没有 errors 时返回 true；warnings 不影响结果可用性。
    bool is_ok() const noexcept { return has_value() && !has_err(); }

    ///@brief 判断是否存在至少一条错误。
    bool has_err() const noexcept { return !errors_.empty(); }

    ///@brief 判断是否存在至少一条警告。
    bool has_warn() const noexcept { return !warnings_.empty(); }

    ///@brief 判断是否保存了完整或部分值。
    bool has_value() const noexcept { return value_.has_value(); }

    ///@brief 返回值指针；无值时返回空指针。
    const T* get() const noexcept { return value_ ? &*value_ : nullptr; }

    ///@brief 返回可修改值指针；无值时返回空指针。
    T* get() noexcept { return value_ ? &*value_ : nullptr; }

    ///@brief 返回按产生顺序保存的 errors。
    const std::vector<Error>& errors() const noexcept { return errors_; }

    ///@brief 返回按产生顺序保存的 warnings。
    const std::vector<Warn>& warnings() const noexcept { return warnings_; }

    ///@brief 移出并清空保存的值；无值时返回空 optional。
    std::optional<T> take_value()
    {
        auto value = std::move(value_);
        value_.reset();
        return value;
    }

private:
    std::optional<T>   value_;
    std::vector<Error> errors_;
    std::vector<Warn>  warnings_;
};

}   //namespace jhasm::error
