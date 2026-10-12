#include "error_collector.hpp"

#include <algorithm>
#include <utility>

namespace jhasm::error {

void ErrorCollector::add_error(Error error)
{
    errors_.push_back(std::move(error));
}

void ErrorCollector::add_warn(Warn warn)
{
    warnings_.push_back(std::move(warn));
}

void ErrorCollector::add_error(ca::str::Utf16StringRef message, const util::Location& location)
{
    errors_.emplace_back(message, location);
}

void ErrorCollector::add_warn(ca::str::Utf16StringRef message, const util::Location& location)
{
    warnings_.emplace_back(message, location);
}

void ErrorCollector::add_errors(const std::vector<Error>& errors)
{
    errors_.insert(errors_.end(), errors.begin(), errors.end());
}

void ErrorCollector::add_warnings(const std::vector<Warn>& warnings)
{
    warnings_.insert(warnings_.end(), warnings.begin(), warnings.end());
}

void ErrorCollector::remove_at(const util::Location& location)
{
    const auto same_line = [&location](const Error& diagnostic) {
        return diagnostic.location() != nullptr && diagnostic.location()->line() == location.line();
    };
    errors_.erase(std::remove_if(errors_.begin(), errors_.end(), same_line), errors_.end());
    warnings_.erase(std::remove_if(warnings_.begin(), warnings_.end(), same_line), warnings_.end());
}

std::vector<Error> ErrorCollector::take_errors() noexcept
{
    return std::exchange(errors_, {});
}

std::vector<Warn> ErrorCollector::take_warnings() noexcept
{
    return std::exchange(warnings_, {});
}

}   //namespace jhasm::error
