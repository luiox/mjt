#pragma once

#include "jhasm/util/location.hpp"

#include <libca/str/utf16_string.hpp>

#include <optional>

namespace jhasm::error {

///@brief 表示一条拥有自身文本和位置的 Jasm 错误诊断。
///
///message、Location 及其中的 source 均由 Error 深拷贝持有，诊断离开 tokenizer
///或 parser 后仍然有效。C++ 版本不保存 Java 线程栈信息。
class Error
{
public:
    ///@brief 深拷贝消息和位置并构造诊断。
    Error(ca::str::Utf16StringRef message, const util::Location& location);
    ///@brief 深拷贝消息并构造不带位置的诊断。
    explicit Error(ca::str::Utf16StringRef message);
    ///@brief 深拷贝另一诊断。
    Error(const Error& other);
    ///@brief 深拷贝另一诊断。
    Error& operator=(const Error& other);
    Error(Error&&) noexcept            = default;
    Error& operator=(Error&&) noexcept = default;
    virtual ~Error()                   = default;

    ///@brief 返回错误消息的非拥有视图。
    ///@note 视图在当前 Error 被销毁、移动或赋值后失效。
    ca::str::Utf16StringRef message() const noexcept { return message_.ref(); }

    ///@brief 判断诊断是否带有源位置。
    bool has_location() const noexcept { return location_.has_value(); }

    ///@brief 返回源位置；无位置时返回空指针。
    ///@note 指针由当前 Error 持有，在对象被销毁、移动或赋值后失效。
    const util::Location* location() const noexcept;

    ///@brief 按上游格式生成拥有型诊断文本。
    ca::str::Utf16String to_string() const;

private:
    ca::str::Utf16String          message_;
    std::optional<util::Location> location_;
};

}   //namespace jhasm::error
