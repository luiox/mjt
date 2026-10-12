#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tool/diff.hpp"

namespace mjt::mcp::detail {
namespace {

TEST(McpJasmDiffTest, SplitAndJoinRoundTripsTrailingNewline)
{
    EXPECT_EQ((std::vector<std::string>{"a", "b"}), splitLines("a\nb\n"));
    EXPECT_EQ((std::vector<std::string>{"a", "b"}), splitLines("a\nb"));
    EXPECT_EQ((std::vector<std::string>{}), splitLines(""));
    EXPECT_EQ((std::vector<std::string>{"", "a"}), splitLines("\na\n"));
    EXPECT_EQ("a\nb\n", joinLines({"a", "b"}));
    EXPECT_EQ("", joinLines({}));
}

TEST(McpJasmDiffTest, ApplySimpleReplacementHunk)
{
    const std::string base = "line1\nline2\nline3\n";
    const std::string diff =
        "--- a\n"
        "+++ b\n"
        "@@ -1,3 +1,3 @@\n"
        " line1\n"
        "-line2\n"
        "+line-two\n"
        " line3\n";
    const auto result = applyUnifiedDiff(base, diff);
    EXPECT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ("line1\nline-two\nline3\n", result.text);
}

TEST(McpJasmDiffTest, ApplyInsertionHunk)
{
    const std::string base = "a\nb\n";
    const std::string diff =
        "@@ -1,2 +1,3 @@\n"
        " a\n"
        "+middle\n"
        " b\n";
    const auto result = applyUnifiedDiff(base, diff);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ("a\nmiddle\nb\n", result.text);
}

TEST(McpJasmDiffTest, ApplyRemovalHunk)
{
    const std::string base = "a\nmiddle\nb\n";
    const std::string diff =
        "@@ -1,3 +1,2 @@\n"
        " a\n"
        "-middle\n"
        " b\n";
    const auto result = applyUnifiedDiff(base, diff);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ("a\nb\n", result.text);
}

TEST(McpJasmDiffTest, ApplyMultipleHunksInOrder)
{
    const std::string base = "h1\nh2\nh3\nh4\nh5\n";
    const std::string diff =
        "@@ -1,1 +1,1 @@\n"
        "-h1\n"
        "+H1\n"
        "@@ -4,1 +4,1 @@\n"
        "-h4\n"
        "+H4\n";
    const auto result = applyUnifiedDiff(base, diff);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ("H1\nh2\nh3\nH4\nh5\n", result.text);
}

TEST(McpJasmDiffTest, InterleavedContextRemovedAddedPreservedInOrder)
{
    //context/-removed/+added 交错：验证解析保留顺序、applier 按序匹配。
    const std::string base = "x\nold1\ny\nold2\nz\n";
    const std::string diff =
        "@@ -1,5 +1,5 @@\n"
        " x\n"
        "-old1\n"
        "+new1\n"
        " y\n"
        "-old2\n"
        "+new2\n"
        " z\n";
    const auto result = applyUnifiedDiff(base, diff);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ("x\nnew1\ny\nnew2\nz\n", result.text);
}

TEST(McpJasmDiffTest, ContextMismatchReportsHunkAndLine)
{
    const std::string base = "line1\nwrong\nline3\n";
    const std::string diff =
        "@@ -1,3 +1,3 @@\n"
        " line1\n"
        "-line2\n"
        "+line-two\n"
        " line3\n";
    const auto result = applyUnifiedDiff(base, diff);
    EXPECT_FALSE(result.error.empty());
    EXPECT_EQ(1u, result.failedHunk);
    EXPECT_NE(std::string::npos, result.error.find("base line 2"));
    EXPECT_NE(std::string::npos, result.error.find("line2"));
}

TEST(McpJasmDiffTest, OverlappingHunksRejected)
{
    const std::string base = "a\nb\nc\n";
    //两个 hunk 都从 base line 1 开始，第二个会回退到已消费位置之前。
    const std::string diff =
        "@@ -1,1 +1,1 @@\n"
        "-a\n"
        "+A\n"
        "@@ -1,1 +1,1 @@\n"
        "-a\n"
        "+B\n";
    const auto result = applyUnifiedDiff(base, diff);
    EXPECT_FALSE(result.error.empty());
    EXPECT_EQ(2u, result.failedHunk);
}

TEST(McpJasmDiffTest, NoNewlineMarkerIsIgnored)
{
    const std::string base = "a\nb\n";
    const std::string diff =
        "@@ -1,1 +1,1 @@\n"
        "-a\n"
        "+A\n"
        "\\ No newline at end of file\n";
    const auto result = applyUnifiedDiff(base, diff);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ("A\nb\n", result.text);
}

TEST(McpJasmDiffTest, HeaderlessDiffAccepted)
{
    //没有 --- / +++ 文件头，直接 @@ 开头也应被接受。
    const std::string base = "a\nb\n";
    const std::string diff =
        "@@ -1,1 +1,1 @@\n"
        "-a\n"
        "+A\n";
    const auto result = applyUnifiedDiff(base, diff);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ("A\nb\n", result.text);
}

TEST(McpJasmDiffTest, MissingCountDefaultsToOne)
{
    //"@@ -1 +1 @@" 缺省 count=1。
    const std::string base = "a\nb\n";
    const std::string diff =
        "@@ -2 +2 @@\n"
        "-b\n"
        "+B\n";
    const auto result = applyUnifiedDiff(base, diff);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ("a\nB\n", result.text);
}

TEST(McpJasmDiffTest, EmptyContextLineHandled)
{
    const std::string base = "a\n\nb\n";
    const std::string diff =
        "@@ -1,3 +1,3 @@\n"
        " a\n"
        " \n"
        "-b\n"
        "+B\n";
    const auto result = applyUnifiedDiff(base, diff);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ("a\n\nB\n", result.text);
}

TEST(McpJasmDiffTest, InvalidPrefixRejected)
{
    const std::string base = "a\n";
    const std::string diff =
        "@@ -1,1 +1,1 @@\n"
        "?a\n";
    std::vector<DiffHunk> hunks;
    std::string error;
    EXPECT_FALSE(parseUnifiedDiff(diff, hunks, error));
    EXPECT_FALSE(error.empty());
}

TEST(McpJasmDiffTest, HunkStartBeyondEndOfBaseRejected)
{
    //hunk 起点超过 base 行数：必须在拷贝循环前报错，而不是越界读 baseLines。
    const std::string base = "a\nb\n";   //2 行
    const std::string diff =
        "@@ -5 +5 @@\n"
        "+x\n";
    const auto result = applyUnifiedDiff(base, diff);
    EXPECT_FALSE(result.error.empty());
    EXPECT_EQ(1u, result.failedHunk);
    EXPECT_NE(std::string::npos, result.error.find("only 2 lines are available"));
}

TEST(McpJasmDiffTest, CrlfDiffMatchesLfBase)
{
    //CRLF diff（Windows 客户端常态）应能匹配 LF base；输出统一为 LF。
    const std::string base = "line1\nline2\nline3\n";
    const std::string diff =
        "@@ -1,3 +1,3 @@\r\n"
        " line1\r\n"
        "-line2\r\n"
        "+line-two\r\n"
        " line3\r\n";
    const auto result = applyUnifiedDiff(base, diff);
    ASSERT_TRUE(result.error.empty()) << result.error;
    EXPECT_EQ("line1\nline-two\nline3\n", result.text);
}

}   //namespace
}   //namespace mjt::mcp::detail
