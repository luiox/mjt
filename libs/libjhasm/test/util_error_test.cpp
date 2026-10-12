#include "jhasm/error/error_collector.hpp"
#include "jhasm/error/result.hpp"
#include "jhasm/util/escape_util.hpp"
#include "jhasm/util/location.hpp"
#include "jhasm/util/range.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

ca::str::Utf16String utf16(const char* text)
{
    return ca::str::Utf16String::value_of(text);
}

std::string utf8(ca::str::Utf16StringRef text)
{
    return text.to_utf8_string().to_std_string();
}

}   //namespace

TEST(JhasmRangeTest, UsesUtf16CodeUnitEndpoints)
{
    const jhasm::util::Range range(2, 5);
    EXPECT_TRUE(range.within(2));
    EXPECT_TRUE(range.within(5));
    EXPECT_FALSE(range.within_exclusive(2));
    EXPECT_TRUE(range.within_exclusive(3));
    EXPECT_EQ(jhasm::util::Range::empty().start(), -1);
}

TEST(JhasmLocationTest, OwnsSourceAndFormatsOneBasedPosition)
{
    auto                  source = utf16("sample.jasm");
    jhasm::util::Location location(3, 7, 2, source.ref());
    source = utf16("changed");

    EXPECT_EQ(location.line(), 3);
    EXPECT_EQ(location.column(), 7);
    EXPECT_EQ(utf8(location.source()), "sample.jasm");
    EXPECT_EQ(utf8(location.to_string().ref()), "sample.jasm:3:7");
}

TEST(JhasmEscapeUtilTest, EscapesAndUnescapesUpstreamVectors)
{
    const std::vector<const char*> vectors = {
        "\\n",
        "\\r",
        "\\t",
        "\\b",
        "\\f",
        "\\\"",
        "\t",
        " ",
        ",",
        "\xE2\x80\x80",
        "\"{ \\\"Hello World\\\", type: \\\"java/lang/HelloWorld\\\" }\",",
        "\n\n\n\n\n\n\n",
    };

    for (const char* vector : vectors) {
        auto                        input   = utf16(vector);
        auto                        escaped = jhasm::util::EscapeUtil::escape_string(input.ref());
        ca::str::Utf16StringBuilder output;
        auto result = jhasm::util::EscapeUtil::unescape(escaped.ref(), output);
        ASSERT_TRUE(result.is_ok());
        EXPECT_TRUE(output.build() == input);
    }
}

TEST(JhasmEscapeUtilTest, LiteralEscapeCoversSeparatorsNulAndSupplementaryCharacters)
{
    const ca::u16              raw[] = {'a', 0, ' ', ',', 0xD83D, 0xDE00};
    const ca::str::Utf16String input(raw, 6);
    const auto                 escaped = jhasm::util::EscapeUtil::escape_literal(input.ref());
    EXPECT_EQ(utf8(escaped.ref()), "a\\u0000\\u0020\\u002C\xF0\x9F\x98\x80");

    ca::str::Utf16StringBuilder output;
    auto                        result = jhasm::util::EscapeUtil::unescape(escaped.ref(), output);
    ASSERT_TRUE(result.is_ok());
    EXPECT_TRUE(output.build() == input);
}

TEST(JhasmEscapeUtilTest, ReportsMalformedUnicodeAndTrailingBackslash)
{
    auto                        invalid = utf16("before\\u12XZ");
    ca::str::Utf16StringBuilder output;
    auto invalid_result = jhasm::util::EscapeUtil::unescape(invalid.ref(), output);
    ASSERT_TRUE(invalid_result.is_err());
    EXPECT_EQ(invalid_result.unwrap_err().code,
              jhasm::util::UnescapeError::Code::InvalidUnicodeEscape);

    auto                        trailing = utf16("after\\");
    ca::str::Utf16StringBuilder trailing_output;
    auto trailing_result = jhasm::util::EscapeUtil::unescape(trailing.ref(), trailing_output);
    ASSERT_TRUE(trailing_result.is_err());
    EXPECT_EQ(trailing_result.unwrap_err().code,
              jhasm::util::UnescapeError::Code::TrailingBackslash);
}

TEST(JhasmErrorTest, CollectorAndResultOwnPartialValueAndDiagnostics)
{
    auto                         source  = utf16("<stdin>");
    auto                         message = utf16("Unterminated string");
    jhasm::util::Location        location(1, 2, 4, source.ref());
    jhasm::error::ErrorCollector collector;
    collector.add_error(message.ref(), location);

    using PartialResult = jhasm::error::Result<std::vector<ca::i32>>;
    PartialResult result(
        std::vector<ca::i32>{1, 2}, collector.take_errors(), collector.take_warnings());
    source  = utf16("changed");
    message = utf16("changed");

    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(result.has_err());
    EXPECT_FALSE(result.is_ok());
    ASSERT_NE(result.get(), nullptr);
    EXPECT_EQ(result.get()->size(), 2u);
    ASSERT_EQ(result.errors().size(), 1u);
    EXPECT_EQ(utf8(result.errors()[0].message()), "Unterminated string");
    ASSERT_NE(result.errors()[0].location(), nullptr);
    EXPECT_EQ(utf8(result.errors()[0].location()->source()), "<stdin>");
}

TEST(JhasmErrorTest, TakingValueClearsResultState)
{
    using ValueResult = jhasm::error::Result<std::vector<ca::i32>>;
    auto result       = ValueResult::ok(std::vector<ca::i32>{1, 2});

    auto value = result.take_value();

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->size(), 2U);
    EXPECT_FALSE(result.has_value());
    EXPECT_FALSE(result.is_ok());
    EXPECT_EQ(result.get(), nullptr);
}
