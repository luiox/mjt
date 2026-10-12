#include "location.hpp"

#include <utility>

namespace jhasm::util {

Location::Location(ca::i32 line, ca::i32 column, ca::i32 length)
    : line_(line)
    , column_(column)
    , length_(length)
    , has_source_(false)
{}

Location::Location(ca::i32 line, ca::i32 column, ca::i32 length, ca::str::Utf16StringRef source)
    : line_(line)
    , column_(column)
    , length_(length)
    , has_source_(true)
    , source_(source.to_string())
{}

Location::Location(const Location& other)
    : line_(other.line_)
    , column_(other.column_)
    , length_(other.length_)
    , has_source_(other.has_source_)
    , source_(other.source_.clone())
{}

Location& Location::operator=(const Location& other)
{
    if (this == &other) return *this;
    line_       = other.line_;
    column_     = other.column_;
    length_     = other.length_;
    has_source_ = other.has_source_;
    source_     = other.source_.clone();
    return *this;
}

Location Location::unknown()
{
    return Location(-1, -1, -1);
}

ca::str::Utf16String Location::to_string() const
{
    ca::str::Utf16StringBuilder builder;
    if (has_source_) {
        builder.append(source_);
        builder.append(ca::str::Char16(':'));
    }
    builder.append(line_);
    builder.append(ca::str::Char16(':'));
    builder.append(column_);
    return builder.build();
}

ca::i32 Location::compare_to(const Location& other) const noexcept
{
    if (line_ < other.line_) return -1;
    if (line_ > other.line_) return 1;
    if (column_ < other.column_) return -1;
    if (column_ > other.column_) return 1;
    return 0;
}

}   //namespace jhasm::util
