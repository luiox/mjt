#include "type_path.hpp"

#include <stdexcept>
#include <string>

namespace jblw::annotation {

TypePath::TypePath(std::vector<Step> steps)
    : steps_(std::move(steps))
{
    if (steps_.size() > 255U) throw std::invalid_argument("Type path is longer than u1");
    for (const auto& step : steps_) {
        if (step.kind > TYPE_ARGUMENT) throw std::invalid_argument("Unknown type path step kind");
        if (step.kind != TYPE_ARGUMENT && step.argument != 0)
            throw std::invalid_argument("Non-argument type path step has an argument");
    }
}

std::optional<TypePath> TypePath::fromString(ca::str::Utf8StringRef value)
{
    if (value.is_empty()) return std::nullopt;
    std::vector<Step> steps;
    ca::usize         index = 0;
    while (index < value.byte_length()) {
        const char current = static_cast<char>(value.byte_at(index++));
        switch (current) {
        case '_': return std::nullopt;
        case '[': steps.push_back({ARRAY_ELEMENT, 0}); break;
        case '.': steps.push_back({INNER_TYPE, 0}); break;
        case '*': steps.push_back({WILDCARD_BOUND, 0}); break;
        default:
        {
            if (current < '0' || current > '9')
                throw std::invalid_argument("Invalid type path character");
            ca::u32 argument   = static_cast<ca::u32>(current - '0');
            bool    terminated = false;
            while (index < value.byte_length()) {
                const char digit = static_cast<char>(value.byte_at(index++));
                if (digit == ';') {
                    terminated = true;
                    break;
                }
                if (digit < '0' || digit > '9')
                    throw std::invalid_argument("Invalid type argument path");
                argument = argument * 10U + static_cast<ca::u32>(digit - '0');
                if (argument > 255U) throw std::invalid_argument("Type argument exceeds u1");
            }
            if (!terminated) throw std::invalid_argument("Unterminated type argument path");
            steps.push_back({TYPE_ARGUMENT, static_cast<ca::u8>(argument)});
            break;
        }
        }
    }
    return TypePath(std::move(steps));
}

ca::str::Utf8String TypePath::toString() const
{
    ca::str::Utf8StringBuilder builder;
    for (const auto& step : steps_) {
        switch (step.kind) {
        case ARRAY_ELEMENT: builder.append("["); break;
        case INNER_TYPE: builder.append("."); break;
        case WILDCARD_BOUND: builder.append("*"); break;
        case TYPE_ARGUMENT:
        {
            const std::string argument = std::to_string(step.argument);
            builder.append(argument.c_str()).append(";");
            break;
        }
        default: break;
        }
    }
    return builder.build();
}

ca::i32 TypePath::hashCode() const noexcept
{
    ca::u32 hash = 1;
    for (const auto& step : steps_) {
        hash = hash * 31U + step.kind;
        hash = hash * 31U + step.argument;
    }
    return static_cast<ca::i32>(hash);
}

bool operator==(const TypePath& lhs, const TypePath& rhs) noexcept
{
    return lhs.equals(rhs);
}

bool operator!=(const TypePath& lhs, const TypePath& rhs) noexcept
{
    return !lhs.equals(rhs);
}

}   //namespace jblw::annotation
