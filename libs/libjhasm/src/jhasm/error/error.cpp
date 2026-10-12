#include "error.hpp"

namespace jhasm::error {

Error::Error(ca::str::Utf16StringRef message, const util::Location& location)
    : message_(message.to_string())
    , location_(location)
{}

Error::Error(ca::str::Utf16StringRef message)
    : message_(message.to_string())
{}

Error::Error(const Error& other)
    : message_(other.message_.clone())
    , location_(other.location_)
{}

Error& Error::operator=(const Error& other)
{
    if (this == &other) return *this;
    message_  = other.message_.clone();
    location_ = other.location_;
    return *this;
}

const util::Location* Error::location() const noexcept
{
    return location_ ? &*location_ : nullptr;
}

ca::str::Utf16String Error::to_string() const
{
    if (!location_) return message_.clone();
    ca::str::Utf16StringBuilder builder;
    builder.append(location_->to_string());
    builder.append(": ");
    builder.append(message_);
    return builder.build();
}

}   //namespace jhasm::error
