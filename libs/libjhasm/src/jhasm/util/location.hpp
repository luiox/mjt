#pragma once

#include <libca/core/datatype.hpp>
#include <libca/str/utf16_string.hpp>

namespace jhasm::util {

///@brief 表示 Jasm 诊断或 Token 的源位置。
///
///行号和列号从 1 开始，长度按 UTF-16 code unit 计数。源名称由对象深拷贝持有，
///因此 Location 脱离 tokenizer 后仍然有效。
class Location
{
public:
    ///@brief 构造不带源名称的位置。
    Location(ca::i32 line, ca::i32 column, ca::i32 length);
    ///@brief 深拷贝源名称并构造位置。
    Location(ca::i32 line, ca::i32 column, ca::i32 length, ca::str::Utf16StringRef source);
    ///@brief 深拷贝另一位置及其源名称。
    Location(const Location& other);
    ///@brief 深拷贝另一位置及其源名称。
    Location& operator=(const Location& other);
    Location(Location&&) noexcept            = default;
    Location& operator=(Location&&) noexcept = default;

    ///@brief 返回未知位置哨兵。
    static Location unknown();

    ///@brief 返回 1-based 行号，未知位置为 -1。
    ca::i32 line() const noexcept { return line_; }

    ///@brief 返回 1-based 列号，未知位置为 -1。
    ca::i32 column() const noexcept { return column_; }

    ///@brief 返回 UTF-16 code unit 长度，未知位置为 -1。
    ca::i32 length() const noexcept { return length_; }

    ///@brief 判断是否保存了源名称。
    bool has_source() const noexcept { return has_source_; }

    ///@brief 返回源名称的非拥有视图。
    ///@note 视图在当前 Location 被销毁、移动或赋值后失效。
    ca::str::Utf16StringRef source() const noexcept { return source_.ref(); }

    ///@brief 按 `source:line:column` 或 `line:column` 生成拥有型文本。
    ca::str::Utf16String to_string() const;

    ///@brief 仅按行号、列号排序，与上游 Comparable 语义一致。
    ca::i32 compare_to(const Location& other) const noexcept;

private:
    ca::i32              line_;
    ca::i32              column_;
    ca::i32              length_;
    bool                 has_source_;
    ca::str::Utf16String source_;
};

}   //namespace jhasm::util
