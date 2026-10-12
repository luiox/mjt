#pragma once

#include "error.hpp"
#include "warn.hpp"

#include <libca/str/utf16_string.hpp>

#include <vector>

namespace jhasm::error {

///@brief 按插入顺序收集 Jasm errors 与 warnings。
///
///集合拥有全部诊断；通过只读 getter 返回的引用在集合下一次修改、移动或销毁后失效。
class ErrorCollector
{
public:
    ///@brief 追加一条已构造错误。
    void add_error(Error error);

    ///@brief 追加一条已构造警告。
    void add_warn(Warn warn);

    ///@brief 深拷贝消息和位置并追加错误。
    void add_error(ca::str::Utf16StringRef message, const util::Location& location);

    ///@brief 深拷贝消息和位置并追加警告。
    void add_warn(ca::str::Utf16StringRef message, const util::Location& location);

    ///@brief 按原顺序追加一组错误。
    void add_errors(const std::vector<Error>& errors);

    ///@brief 按原顺序追加一组警告。
    void add_warnings(const std::vector<Warn>& warnings);

    ///@brief 删除与给定位置同行的全部 errors 与 warnings。
    void remove_at(const util::Location& location);

    ///@brief 判断是否存在错误。
    bool has_err() const noexcept { return !errors_.empty(); }

    ///@brief 判断是否存在警告。
    bool has_warn() const noexcept { return !warnings_.empty(); }

    ///@brief 返回按插入顺序保存的错误。
    const std::vector<Error>& errors() const noexcept { return errors_; }

    ///@brief 返回按插入顺序保存的警告。
    const std::vector<Warn>& warnings() const noexcept { return warnings_; }

    ///@brief 移出全部错误；调用后错误集合为空。
    std::vector<Error> take_errors() noexcept;

    ///@brief 移出全部警告；调用后警告集合为空。
    std::vector<Warn> take_warnings() noexcept;

private:
    std::vector<Error> errors_;
    std::vector<Warn>  warnings_;
};

}   //namespace jhasm::error
