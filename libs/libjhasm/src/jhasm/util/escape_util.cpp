#include "escape_util.hpp"

#include <array>

namespace jhasm::util {
namespace {

using ca::str::Char16;

constexpr std::array<std::array<ca::u16, 2>, 13> kUnicodeRanges = {{
    {{0x0000, 0x001F}},
    {{0x007F, 0x009F}},
    {{0x06E5, 0x06E5}},
    {{0x17B4, 0x17B4}},
    {{0x180B, 0x180E}},
    {{0x2000, 0x200E}},
    {{0x2028, 0x202F}},
    {{0x205F, 0x206E}},
    {{0x2400, 0x243E}},
    {{0xE000, 0xF8FF}},
    {{0xFE00, 0xFE0F}},
    {{0xFE1A, 0xFE20}},
    {{0xFFF0, 0xFFFF}},
}};

bool uses_unicode_escape(ca::u16 unit) noexcept
{
    if (unit == 0x3000 || unit == 0x00A0) return true;
    for (const auto& range : kUnicodeRanges) {
        if (unit >= range[0] && unit <= range[1]) return true;
    }
    return false;
}

bool uses_literal_unicode_escape(ca::u16 unit) noexcept
{
    switch (unit) {
    case ' ':
    case ',':
    case '.':
    case ':':
    case '{':
    case '}':
    case '"':
    case '\'': return true;
    default: return uses_unicode_escape(unit);
    }
}

Char16 hex_digit(ca::u16 value) noexcept
{
    return Char16(static_cast<ca::u16>(value < 10 ? '0' + value : 'A' + value - 10));
}

void append_unicode_escape(ca::str::Utf16StringBuilder& builder, ca::u16 unit)
{
    builder.append(Char16('\\'));
    builder.append(Char16('u'));
    builder.append(hex_digit(static_cast<ca::u16>((unit >> 12) & 0xF)));
    builder.append(hex_digit(static_cast<ca::u16>((unit >> 8) & 0xF)));
    builder.append(hex_digit(static_cast<ca::u16>((unit >> 4) & 0xF)));
    builder.append(hex_digit(static_cast<ca::u16>(unit & 0xF)));
}

ca::i32 from_hex(ca::u16 unit) noexcept
{
    if (unit >= '0' && unit <= '9') return static_cast<ca::i32>(unit - '0');
    if (unit >= 'a' && unit <= 'f') return static_cast<ca::i32>(unit - 'a' + 10);
    if (unit >= 'A' && unit <= 'F') return static_cast<ca::i32>(unit - 'A' + 10);
    return -1;
}

}   //namespace

ca::str::Utf16String EscapeUtil::escape_string(ca::str::Utf16StringRef string)
{
    return escape(string, false);
}

ca::str::Utf16String EscapeUtil::escape_literal(ca::str::Utf16StringRef string)
{
    return escape(string, true);
}

ca::str::Utf16String EscapeUtil::escape(ca::str::Utf16StringRef string, bool literal)
{
    ca::str::Utf16StringBuilder builder;
    builder.reserve(string.length());
    for (ca::usize index = 0; index < string.length(); ++index) {
        const ca::u16 unit = string.char_at(index).unit();
        if ((literal && uses_literal_unicode_escape(unit)) || uses_unicode_escape(unit)) {
            append_unicode_escape(builder, unit);
            continue;
        }
        switch (unit) {
        case '"': builder.append("\\\""); break;
        case '\'': builder.append("\\'"); break;
        case '\\': builder.append("\\\\"); break;
        default: builder.append(Char16(unit)); break;
        }
    }
    return builder.build();
}

ca::Result<void, UnescapeError> EscapeUtil::unescape(ca::str::Utf16StringRef      escaped,
                                                     ca::str::Utf16StringBuilder& output)
{
    for (ca::usize index = 0; index < escaped.length(); ++index) {
        const ca::u16 current = escaped.char_at(index).unit();
        if (current != '\\') {
            output.append(Char16(current));
            continue;
        }
        if (index + 1 >= escaped.length()) {
            return ca::Err(UnescapeError{UnescapeError::Code::TrailingBackslash, index});
        }

        const ca::usize escape_offset = index;
        const ca::u16   next          = escaped.char_at(++index).unit();
        switch (next) {
        case 'n': output.append(Char16('\n')); break;
        case 'r': output.append(Char16('\r')); break;
        case 't': output.append(Char16('\t')); break;
        case 'b': output.append(Char16('\b')); break;
        case 'f': output.append(Char16('\f')); break;
        case '"': output.append(Char16('"')); break;
        case '\'': output.append(Char16('\'')); break;
        case '\\': output.append(Char16('\\')); break;
        case 'u':
        {
            if (index + 4 >= escaped.length()) {
                return ca::Err(
                    UnescapeError{UnescapeError::Code::TruncatedUnicodeEscape, escape_offset});
            }
            ca::u16 value = 0;
            for (ca::usize digit = 1; digit <= 4; ++digit) {
                const ca::i32 decoded = from_hex(escaped.char_at(index + digit).unit());
                if (decoded < 0) {
                    return ca::Err(
                        UnescapeError{UnescapeError::Code::InvalidUnicodeEscape, escape_offset});
                }
                value = static_cast<ca::u16>((value << 4) | static_cast<ca::u16>(decoded));
            }
            output.append(Char16(value));
            index += 4;
            break;
        }
        default:
            output.append(Char16('\\'));
            output.append(Char16(next));
            break;
        }
    }
    return ca::Ok();
}

}   //namespace jhasm::util
