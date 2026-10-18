#include <gtest/gtest.h>

#include <string>

#include "tool/json.hpp"
#include "tool/parser.hpp"

//A4 类型取值防护：schema 校验（contract.cpp mutationOptions 已带 minimum: 0）之外的
//运行时最后防线。mutationRequest 抛 Json::exception → service.call 统一映射为
//INVALID_ARGUMENT；parseCursor 返回 false → page/classSummaryPage 退化到空尾页。
namespace mjt::mcp::detail {
namespace {

TEST(McpParserGuardTest, MutationRequestRejectsNegativeRevision)
{
    EXPECT_THROW((void)mutationRequest(Json{{"expected_revision", -1}}), Json::exception);
    EXPECT_THROW((void)mutationRequest(Json::parse(R"({"expected_revision": -1})")),
                 Json::exception);
}

TEST(McpParserGuardTest, MutationRequestRejectsFloatAndStringRevision)
{
    EXPECT_THROW((void)mutationRequest(Json{{"expected_revision", 1.5}}), Json::exception);
    EXPECT_THROW((void)mutationRequest(Json{{"expected_revision", "3"}}), Json::exception);
    EXPECT_THROW((void)mutationRequest(Json::parse(R"({"expected_revision": 3.0})")),
                 Json::exception);
}

TEST(McpParserGuardTest, MutationRequestAcceptsNonNegativeIntegerRevision)
{
    //wire 路径：JSON 文本解析出的非负整数为 number_unsigned。
    auto parsed = mutationRequest(Json::parse(R"({"expected_revision": 7})"));
    EXPECT_EQ(7U, parsed.expectedRevision);

    //u64 满值透传。
    auto full = mutationRequest(Json::parse(R"({"expected_revision": 18446744073709551615})"));
    EXPECT_EQ(18446744073709551615ULL, full.expectedRevision);

    //C++ 内部构造的非负 int 是 number_integer（signed），不能被误伤。
    auto constructed = mutationRequest(Json{{"expected_revision", 7}});
    EXPECT_EQ(7U, constructed.expectedRevision);

    //缺省：保留 kAnyRevision 语义。
    auto absent = mutationRequest(Json::object());
    EXPECT_EQ(mjbase::workspace::kAnyRevision, absent.expectedRevision);
}

TEST(McpParserGuardTest, ParseCursorRejectsNonStringCursor)
{
    ca::usize offset = 3;
    EXPECT_FALSE(parseCursor(Json{{"cursor", 5}}, 10, offset));
    EXPECT_EQ(3U, offset);
    EXPECT_FALSE(parseCursor(Json{{"cursor", {{"nested", true}}}}, 10, offset));
    EXPECT_EQ(3U, offset);
}

TEST(McpParserGuardTest, ParseCursorRejectsMalformedStringCursor)
{
    ca::usize offset = 0;
    for (const auto* cursor : {"", "-1", "+1", "1x", "184467440737095516160"}) {
        EXPECT_FALSE(parseCursor(Json{{"cursor", cursor}}, 10, offset));
    }
}

TEST(McpParserGuardTest, ParseCursorAcceptsDecimalStringAndClampsToTotal)
{
    ca::usize offset = 0;
    EXPECT_TRUE(parseCursor(Json{{"cursor", "4"}}, 10, offset));
    EXPECT_EQ(4U, offset);

    offset = 0;
    EXPECT_TRUE(parseCursor(Json{{"cursor", "40"}}, 10, offset));
    EXPECT_EQ(10U, offset);

    //无 cursor：保留传入 offset。
    offset = 0;
    EXPECT_TRUE(parseCursor(Json::object(), 10, offset));
    EXPECT_EQ(0U, offset);
}

}   //namespace
}   //namespace mjt::mcp::detail
