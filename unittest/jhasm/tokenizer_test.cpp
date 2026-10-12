#include "jhasm/parser/tokenizer.hpp"
#include "jhasm/util/escape_util.hpp"

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

jhasm::error::Result<std::vector<jhasm::parser::Token>> tokenize(const char* text)
{
    auto source = utf16("<stdin>");
    auto input  = utf16(text);
    return jhasm::parser::Tokenizer().tokenize(source.ref(), input.ref());
}

const std::vector<jhasm::parser::Token>& tokens(
    const jhasm::error::Result<std::vector<jhasm::parser::Token>>& result)
{
    EXPECT_NE(result.get(), nullptr);
    return *result.get();
}

}   //namespace

TEST(JhasmTokenizerTest, TokenizesUpstreamStringVector)
{
    auto result = tokenize("{ \"Hello World\", type: \"java/lang/HelloWorld\" }");
    ASSERT_TRUE(result.is_ok());
    const auto& value = tokens(result);
    ASSERT_EQ(value.size(), 7u);
    EXPECT_EQ(utf8(value[0].content()), "{");
    EXPECT_EQ(utf8(value[1].content()), "Hello World");
    EXPECT_EQ(utf8(value[2].content()), ",");
    EXPECT_EQ(utf8(value[3].content()), "type");
    EXPECT_EQ(utf8(value[4].content()), ":");
    EXPECT_EQ(utf8(value[5].content()), "java/lang/HelloWorld");
    EXPECT_EQ(utf8(value[6].content()), "}");
    EXPECT_EQ(value[1].type(), jhasm::parser::TokenType::String);
}

TEST(JhasmTokenizerTest, TokenizesUpstreamProgramSamples)
{
    const char* samples[] = {
        ".class public java/lang/HelloWorld",
        ".visible-annotation Annotate {\n\tvalue: \"java/lang/HelloWorld\", \n\ttype: "
        ".enum java/lang/TargetType, METHOD\n}",
        ".method add (II)I { \n\t.parameters: {a, b}\n\t.code: {\n\t\tiload a\n"
        "\t\tiload b\n\t\tiadd\n\t\tireturn\t\n\t}\n}",
    };
    for (const char* sample : samples) {
        auto result = tokenize(sample);
        ASSERT_TRUE(result.is_ok());
        EXPECT_FALSE(tokens(result).empty());
    }
}

TEST(JhasmTokenizerTest, DecodesStringCharacterAndUnicodeEscapes)
{
    auto result = tokenize("\"Hello \\u0020World\\\"\\\\\" '\\u0041'");
    ASSERT_TRUE(result.is_ok());
    const auto& value = tokens(result);
    ASSERT_EQ(value.size(), 2u);
    EXPECT_EQ(utf8(value[0].content()), "Hello  World\"\\");
    EXPECT_EQ(value[0].type(), jhasm::parser::TokenType::String);
    EXPECT_EQ(utf8(value[1].content()), "A");
    EXPECT_EQ(value[1].type(), jhasm::parser::TokenType::Character);
}

TEST(JhasmTokenizerTest, ClassifiesUpstreamNumberLanguage)
{
    auto result = tokenize("0 -10 10f 10.16F 10.161616D 10L 0xDEADBEEF 0E10 0.3e10f "
                           "6.02214076e23 0b101 0x1.fp3");
    ASSERT_TRUE(result.is_ok());
    const auto& value = tokens(result);
    ASSERT_EQ(value.size(), 12u);
    for (const auto& token : value) {
        EXPECT_EQ(token.type(), jhasm::parser::TokenType::Number) << utf8(token.content());
    }

    auto identifiers = tokenize("+1 1e+2 0x 0b102");
    ASSERT_TRUE(identifiers.is_ok());
    for (const auto& token : tokens(identifiers)) {
        EXPECT_EQ(token.type(), jhasm::parser::TokenType::Identifier) << utf8(token.content());
    }
}

TEST(JhasmTokenizerTest, TokenizesSingleAndMultiLineComments)
{
    auto single = tokenize("// This is a comment\n");
    ASSERT_TRUE(single.is_ok());
    ASSERT_EQ(tokens(single).size(), 1u);
    EXPECT_EQ(utf8(tokens(single)[0].content()), " This is a comment");
    EXPECT_EQ(tokens(single)[0].type(), jhasm::parser::TokenType::Comment);

    auto multi = tokenize("/* This is a comment\n * with multiple lines\n */");
    ASSERT_TRUE(multi.is_ok());
    ASSERT_EQ(tokens(multi).size(), 1u);
    EXPECT_EQ(utf8(tokens(multi)[0].content()), " This is a comment\n * with multiple lines\n ");
    EXPECT_EQ(tokens(multi)[0].type(), jhasm::parser::TokenType::Comment);
}

TEST(JhasmTokenizerTest, EscapeUtilVectorsRoundTripThroughTokenizer)
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
        "\xE2\x80\xA8",
        "\"{ \\\"Hello World\\\", type: \\\"java/lang/HelloWorld\\\" }\",",
        "\n\n\n\n\n\n\n",
    };

    auto source = utf16("<stdin>");
    for (const char* vector : vectors) {
        auto                        input   = utf16(vector);
        auto                        escaped = jhasm::util::EscapeUtil::escape_string(input.ref());
        ca::str::Utf16StringBuilder quoted;
        quoted.append(ca::str::Char16('"'));
        quoted.append(escaped);
        quoted.append(ca::str::Char16('"'));
        auto tokenized = jhasm::parser::Tokenizer().tokenize(source.ref(), quoted.build().ref());
        ASSERT_TRUE(tokenized.is_ok());
        ASSERT_EQ(tokens(tokenized).size(), 1u);
        EXPECT_TRUE(tokens(tokenized)[0].content() == input);
    }
}

TEST(JhasmTokenizerTest, LiteralEscapeKeepsWholeInputInOneIdentifier)
{
    const char* vectors[] = {
        "epic obfuscated name",
        "{ \"Hello World\", type: \"java/lang/HelloWorld\" }",
        "0 -10 10f 10.16F 10L",
        ".class public java/lang/HelloWorld",
        "@ValueSource(strings = {\n  \"epic\\u0020name\",\n  "
        "\"{ \\\"Hello\\\" }\"\n})",
    };
    auto source = utf16("<stdin>");
    for (const char* vector : vectors) {
        auto input   = utf16(vector);
        auto escaped = jhasm::util::EscapeUtil::escape_literal(input.ref());
        auto result  = jhasm::parser::Tokenizer().tokenize(source.ref(), escaped.ref());
        ASSERT_TRUE(result.is_ok());
        ASSERT_EQ(tokens(result).size(), 1u);

        ca::str::Utf16StringBuilder unescaped;
        auto                        unescape_result =
            jhasm::util::EscapeUtil::unescape(tokens(result)[0].content(), unescaped);
        ASSERT_TRUE(unescape_result.is_ok());
        EXPECT_TRUE(unescaped.build() == input);
    }
}

TEST(JhasmTokenizerTest, UsesUtf16CodeUnitsForRangesLocationsAndNul)
{
    const ca::u16        raw[]  = {'a', 0xD83D, 0xDE00, ' ', 'b', 0, 'c'};
    auto                 source = utf16("source.jasm");
    ca::str::Utf16String input(raw, 7);
    auto                 result = jhasm::parser::Tokenizer().tokenize(source.ref(), input.ref());
    ASSERT_TRUE(result.is_ok());
    const auto& value = tokens(result);
    ASSERT_EQ(value.size(), 2u);
    EXPECT_EQ(value[0].content().length(), 3u);
    EXPECT_EQ(value[0].range().start(), 0);
    EXPECT_EQ(value[0].range().end(), 3);
    EXPECT_EQ(value[0].location().line(), 1);
    EXPECT_EQ(value[0].location().column(), 1);
    EXPECT_EQ(value[0].location().length(), 3);
    EXPECT_EQ(value[1].content().length(), 3u);
    EXPECT_EQ(value[1].content().char_at(1).unit(), 0u);
    EXPECT_EQ(value[1].location().column(), 5);
}

TEST(JhasmTokenizerTest, UpdatesOneBasedLineAndColumn)
{
    auto result = tokenize("first\n  second\nthird");
    ASSERT_TRUE(result.is_ok());
    const auto& value = tokens(result);
    ASSERT_EQ(value.size(), 3u);
    EXPECT_EQ(value[0].location().line(), 1);
    EXPECT_EQ(value[0].location().column(), 1);
    EXPECT_EQ(value[1].location().line(), 2);
    EXPECT_EQ(value[1].location().column(), 3);
    EXPECT_EQ(value[2].location().line(), 3);
    EXPECT_EQ(value[2].location().column(), 1);
}

TEST(JhasmTokenizerTest, ReportsUnterminatedQuotedTokensWithPartialValues)
{
    auto string_result = tokenize("prefix \"unterminated");
    ASSERT_TRUE(string_result.has_value());
    ASSERT_TRUE(string_result.has_err());
    ASSERT_EQ(tokens(string_result).size(), 2u);
    EXPECT_EQ(tokens(string_result)[1].type(), jhasm::parser::TokenType::String);
    ASSERT_EQ(string_result.errors().size(), 1u);
    EXPECT_EQ(utf8(string_result.errors()[0].message()), "Unterminated string");
    EXPECT_EQ(string_result.errors()[0].location()->column(), 9);

    auto character_result = tokenize("'x\nnext");
    ASSERT_TRUE(character_result.has_value());
    ASSERT_TRUE(character_result.has_err());
    ASSERT_EQ(tokens(character_result).size(), 2u);
    EXPECT_EQ(tokens(character_result)[0].type(), jhasm::parser::TokenType::Character);
    EXPECT_EQ(utf8(character_result.errors()[0].message()), "Unterminated character");
}

TEST(JhasmTokenizerTest, HandlesCommentAndEscapeBoundariesWithoutOutOfBoundsAccess)
{
    auto slash = tokenize("/");
    ASSERT_TRUE(slash.is_ok());
    ASSERT_EQ(tokens(slash).size(), 1u);
    EXPECT_EQ(utf8(tokens(slash)[0].content()), "/");

    auto line_comment = tokenize("// eof");
    ASSERT_TRUE(line_comment.is_ok());
    ASSERT_EQ(tokens(line_comment).size(), 1u);
    EXPECT_EQ(utf8(tokens(line_comment)[0].content()), " eof");

    auto block_comment = tokenize("/* eof");
    ASSERT_TRUE(block_comment.has_err());
    ASSERT_EQ(tokens(block_comment).size(), 1u);
    EXPECT_EQ(utf8(block_comment.errors()[0].message()), "Unterminated multiline comment");

    auto escape = tokenize("\"trailing\\");
    ASSERT_TRUE(escape.has_err());
    ASSERT_EQ(tokens(escape).size(), 1u);
    ASSERT_EQ(escape.errors().size(), 2u);
    EXPECT_EQ(utf8(escape.errors()[0].message()), "Incomplete escape sequence");
    EXPECT_EQ(utf8(escape.errors()[1].message()), "Unterminated string");

    auto invalid_unicode = tokenize("\"bad \\u12XZ\"");
    ASSERT_TRUE(invalid_unicode.has_err());
    ASSERT_EQ(tokens(invalid_unicode).size(), 1u);
    EXPECT_EQ(utf8(invalid_unicode.errors()[0].message()), "Invalid unicode escape");

    auto truncated_unicode = tokenize("\"bad \\u12\"");
    ASSERT_TRUE(truncated_unicode.has_err());
    ASSERT_EQ(tokens(truncated_unicode).size(), 1u);
    EXPECT_EQ(utf8(truncated_unicode.errors()[0].message()), "Truncated unicode escape");
}

TEST(JhasmTokenizerTest, ReturnedTokensAndDiagnosticsOwnInputText)
{
    auto source = utf16("owned.jasm");
    auto input  = utf16("\"value");
    auto result = jhasm::parser::Tokenizer().tokenize(source.ref(), input.ref());
    source      = utf16("changed");
    input       = utf16("changed");

    ASSERT_TRUE(result.has_err());
    ASSERT_EQ(tokens(result).size(), 1u);
    EXPECT_EQ(utf8(tokens(result)[0].content()), "value");
    EXPECT_EQ(utf8(tokens(result)[0].location().source()), "owned.jasm");
    EXPECT_EQ(utf8(result.errors()[0].location()->source()), "owned.jasm");
}
