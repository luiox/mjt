#include "search_match.hpp"

#include <algorithm>
#include <cctype>

namespace mjt::workspace {
namespace {

/// @brief WorkspaceErrorInfo 便捷构造（workspace.cpp 引擎面同款消息形状：
///        `operation + ": " + detail`）。
WorkspaceErrorInfo matchError(WorkspaceError code, const std::string& operation,
                              const std::string& detail)
{
    return WorkspaceErrorInfo{code, operation + ": " + detail};
}

}   // namespace

std::string lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

bool matchesText(const std::string& value, const std::string& query,
                 const SearchOptions& options, const std::regex* compiledRegex)
{
    if (options.matchMode == SearchMatchMode::Regex)
        return compiledRegex != nullptr && std::regex_search(value, *compiledRegex);
    const auto comparableValue = options.caseSensitive ? value : lowercase(value);
    const auto comparableQuery = options.caseSensitive ? query : lowercase(query);
    if (options.matchMode == SearchMatchMode::Exact) return comparableValue == comparableQuery;
    return comparableValue.find(comparableQuery) != std::string::npos;
}

WorkspaceErrorInfo validateSearchOptions(const std::string&   query, const SearchOptions& options,
                                         const std::string& operation)
{
    if (query.empty())
        return matchError(WorkspaceError::InvalidArgument, operation, "query must not be empty");
    if (query.size() > 1024)
        return matchError(WorkspaceError::InvalidArgument, operation, "query exceeds 1024 bytes");
    if (!options.memberKind.is_empty() && options.memberKind != "field" &&
        options.memberKind != "method")
        return matchError(WorkspaceError::InvalidArgument, operation,
                          "member kind must be field or method");
    if (options.opcode < -1 || options.opcode >= 200)
        return matchError(WorkspaceError::InvalidArgument, operation,
                          "opcode must be between 0 and 199");
    return WorkspaceErrorInfo{};
}

WorkspaceResult<std::optional<std::regex>> compileSearchRegex(const std::string&   query,
                                                              const SearchOptions& options,
                                                              const std::string& operation)
{
    WorkspaceErrorInfo status = validateSearchOptions(query, options, operation);
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (options.matchMode != SearchMatchMode::Regex)
        return ca::core::Ok(std::optional<std::regex>{});
    try {
        auto flags = std::regex_constants::ECMAScript;
        if (!options.caseSensitive) flags |= std::regex_constants::icase;
        return ca::core::Ok(std::optional<std::regex>(std::in_place, query, flags));
    }
    catch (const std::regex_error& exception) {
        return ca::core::Err(
            matchError(WorkspaceError::InvalidArgument, operation, exception.what()));
    }
}

bool matchesSearchScope(std::string_view owner, const SearchOptions& options)
{
    if (!options.owner.is_empty() && owner != std::string_view(options.owner)) return false;
    return options.packagePrefix.is_empty() ||
           owner.rfind(std::string_view(options.packagePrefix), 0) == 0;
}

bool matchesAnyText(std::initializer_list<ca::str::Utf8StringRef> values,
                    const std::string& query, const SearchOptions& options,
                    const std::regex* compiledRegex)
{
    return std::any_of(values.begin(), values.end(), [&](const ca::str::Utf8StringRef& value) {
        return matchesText(value.to_std_string(), query, options, compiledRegex);
    });
}

WorkspaceErrorInfo validateNumberQuery(const std::string& query)
{
    try {
        std::size_t offset = 0;
        const auto  parsed = std::stold(query, &offset);
        (void)parsed;
        if (offset == query.size()) return WorkspaceErrorInfo{};
    }
    catch (const std::exception&) {
    }
    return matchError(WorkspaceError::InvalidArgument, "search numbers",
                      "query must be a complete numeric value");
}

}   // namespace mjt::workspace
