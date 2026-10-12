#pragma once

#include <exception>
#include "libca/core/datatype.hpp"

namespace mjt::base::mapping::format {

class InvalidMappingException : public std::exception
{
public:
    explicit InvalidMappingException(const char* message)
        : msg_(message)
    {}
    explicit InvalidMappingException(ca::str::Utf8StringRef message)
        : msg_(message.data(), message.data() + message.byte_length())
    {}
    const char* what() const noexcept override { return msg_.c_str(); }

private:
    std::string msg_;
};

}   //namespace mjt::base::mapping::format
