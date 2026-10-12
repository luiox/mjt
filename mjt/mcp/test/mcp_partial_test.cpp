//P2-3 MCP partial 侧车：截断/超时路径的工具结果文档自带 partial 元数据，
//下游抓取产物可据此区分「空结果」「截断续页」与「抓取失败」。
//实现：mjt/mcp/src/tool/json.cpp —— page/classSummaryPage 截断路径写
//{partial, reason:"truncated", received, total, cursor}；normalizeToolError
//对 DEADLINE_EXCEEDED 写 {partial, reason:"timeout"}。完整页与其余错误零变化。
//sweep-2 扩展（R58 N2/N3）：query.cpp resource_read/class_bytes 字节分页截断
//路径补同口径五键侧车；decompiler.cpp async job fail 超时分支补
//{partial, reason:"timeout"}（job_get 的 result 文档）。

#include <cstddef>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

TEST(McpServerTest, TruncatedPaginationCarriesPartialSidecar)
{
    McpSessionHarness server;
    const auto        fixture = writeFixture();
    const auto        opened  = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

    //limit=1 < total：class_list（classSummaryPage 编码预算路径）必截断。
    const auto listed = call(server, "class_list", {{"workspace_id", workspaceId}, {"limit", 1}});
    ASSERT_FALSE(listed.at("result").at("isError"));
    const auto& body = payload(listed);
    EXPECT_TRUE(body.at("partial").get<bool>());
    EXPECT_EQ("truncated", body.at("reason").get<std::string>());
    ASSERT_EQ(1U, body.at("items").size());
    EXPECT_EQ(body.at("items").size(), body.at("received").get<std::size_t>());
    EXPECT_LT(body.at("received").get<std::size_t>(), body.at("total").get<std::size_t>());
    EXPECT_FALSE(body.at("next_cursor").get<std::string>().empty());
    EXPECT_EQ(body.at("next_cursor").get<std::string>(), body.at("cursor").get<std::string>());

    //page 路径同口径：workspace_resources limit=1 必截断（fixture 含 class 资源）。
    const auto resources =
        call(server, "workspace_resources", {{"workspace_id", workspaceId}, {"limit", 1}});
    ASSERT_FALSE(resources.at("result").at("isError"));
    const auto& resourceBody = payload(resources);
    EXPECT_TRUE(resourceBody.at("partial").get<bool>());
    EXPECT_EQ("truncated", resourceBody.at("reason").get<std::string>());
    EXPECT_EQ(resourceBody.at("items").size(), resourceBody.at("received").get<std::size_t>());
    EXPECT_EQ(resourceBody.at("next_cursor").get<std::string>(),
              resourceBody.at("cursor").get<std::string>());

    //翻到尾页：完整页不得携带 partial 侧车键（正常路径零变化）。
    const auto last = call(server,
                           "class_list",
                           {{"workspace_id", workspaceId},
                            {"limit", 100},
                            {"cursor", body.at("next_cursor").get<std::string>()}});
    ASSERT_FALSE(last.at("result").at("isError"));
    const auto& lastBody = payload(last);
    EXPECT_FALSE(lastBody.contains("partial"));
    EXPECT_FALSE(lastBody.contains("reason"));
    EXPECT_FALSE(lastBody.contains("cursor"));
    EXPECT_FALSE(lastBody.contains("received"));
}

TEST(McpServerTest, DecompileTimeoutCarriesPartialSidecar)
{
    McpSessionHarness server;
    const auto        fixture = writeFixture();
    const auto        opened  = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

    //超时注入（既有口径，同 mcp_workflow_test）：slow 假反编译器（test 二进制
    //充当 java 进程，sleep 1 秒）+ 25ms 超时 → DEADLINE_EXCEEDED。
    const auto slowDecompiler = test::getTempOutPath("fake-mcp-decompiler-slow.jar");
    std::ofstream(slowDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");

    const auto timedOut = call(server,
                               "class_source",
                               {{"workspace_id", workspaceId},
                                {"class", "mcp/Subject"},
                                {"decompiler_jar", slowDecompiler.string()},
                                {"java", fakeJava.string()},
                                {"timeout_ms", 25},
                                {"max_memory_mib", 256},
                                {"max_source_bytes", 1024 * 1024}});
    ASSERT_TRUE(timedOut.at("result").at("isError"));
    const auto& body = payload(timedOut);
    EXPECT_TRUE(body.at("partial").get<bool>());
    EXPECT_EQ("timeout", body.at("reason").get<std::string>());
    EXPECT_EQ(static_cast<int>(ca::StatusCode::DEADLINE_EXCEEDED),
              body.at("status_code").get<int>());

    //即时业务错误（class 不存在）不得携带 partial 侧车键（正常错误路径零变化）。
    const auto missing = call(server,
                              "class_source",
                              {{"workspace_id", workspaceId}, {"class", "mcp/Missing"}});
    ASSERT_TRUE(missing.at("result").at("isError"));
    EXPECT_FALSE(payload(missing).contains("partial"));
    EXPECT_FALSE(payload(missing).contains("reason"));
}

//N3 字节分页侧车：resource_read / class_bytes 截断路径此前只写 next_cursor，
//与列表分页侧车（P2-3）口径不一；现补齐五键 {partial, reason:"truncated",
//received, total, cursor}。received=本页实际字节数，total=资源总字节数，
//cursor=下一页字节偏移。完整页零变化。
TEST(McpServerTest, TruncatedBytePaginationCarriesPartialSidecar)
{
    McpSessionHarness server;
    const auto        fixture = writeFixture();
    const auto        opened  = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

    //resource_read：notice.txt 共 11 字节，limit=4 必截断。
    const auto resource = call(server,
                               "resource_read",
                               {{"workspace_id", workspaceId}, {"uri", "notice.txt"}, {"limit", 4}});
    ASSERT_FALSE(resource.at("result").at("isError"));
    const auto& resourceBody = payload(resource);
    EXPECT_TRUE(resourceBody.at("partial").get<bool>());
    EXPECT_EQ("truncated", resourceBody.at("reason").get<std::string>());
    EXPECT_EQ(4U, resourceBody.at("received").get<std::size_t>());
    EXPECT_EQ(11U, resourceBody.at("total").get<std::size_t>());
    EXPECT_EQ("4", resourceBody.at("next_cursor").get<std::string>());
    EXPECT_EQ(resourceBody.at("next_cursor").get<std::string>(),
              resourceBody.at("cursor").get<std::string>());

    //翻到尾页：完整页不得携带侧车键（正常路径零变化）。
    const auto resourceTail =
        call(server,
             "resource_read",
             {{"workspace_id", workspaceId},
              {"uri", "notice.txt"},
              {"cursor", resourceBody.at("next_cursor").get<std::string>()}});
    ASSERT_FALSE(resourceTail.at("result").at("isError"));
    const auto& resourceTailBody = payload(resourceTail);
    EXPECT_FALSE(resourceTailBody.contains("partial"));
    EXPECT_FALSE(resourceTailBody.contains("reason"));
    EXPECT_FALSE(resourceTailBody.contains("received"));
    EXPECT_FALSE(resourceTailBody.contains("total"));
    EXPECT_FALSE(resourceTailBody.contains("cursor"));
    EXPECT_FALSE(resourceTailBody.contains("next_cursor"));

    //class_bytes：limit=1 必截断；total 与既有 size 同源（资源总字节数）。
    const auto classBytes = call(server,
                                 "class_bytes",
                                 {{"workspace_id", workspaceId},
                                  {"class", "mcp/Subject"},
                                  {"limit", 1}});
    ASSERT_FALSE(classBytes.at("result").at("isError"));
    const auto& classBytesBody = payload(classBytes);
    EXPECT_TRUE(classBytesBody.at("partial").get<bool>());
    EXPECT_EQ("truncated", classBytesBody.at("reason").get<std::string>());
    EXPECT_EQ(1U, classBytesBody.at("received").get<std::size_t>());
    EXPECT_EQ(classBytesBody.at("size").get<std::size_t>(),
              classBytesBody.at("total").get<std::size_t>());
    EXPECT_LT(classBytesBody.at("received").get<std::size_t>(),
              classBytesBody.at("total").get<std::size_t>());
    EXPECT_EQ(classBytesBody.at("next_cursor").get<std::string>(),
              classBytesBody.at("cursor").get<std::string>());

    //翻到尾页（大 limit 一页读完）：完整页零变化。
    const auto classTail =
        call(server,
             "class_bytes",
             {{"workspace_id", workspaceId},
              {"class", "mcp/Subject"},
              {"limit", 1024 * 1024},
              {"cursor", classBytesBody.at("next_cursor").get<std::string>()}});
    ASSERT_FALSE(classTail.at("result").at("isError"));
    const auto& classTailBody = payload(classTail);
    EXPECT_FALSE(classTailBody.contains("partial"));
    EXPECT_FALSE(classTailBody.contains("reason"));
    EXPECT_FALSE(classTailBody.contains("received"));
    EXPECT_FALSE(classTailBody.contains("total"));
    EXPECT_FALSE(classTailBody.contains("cursor"));
    EXPECT_FALSE(classTailBody.contains("next_cursor"));
}

//N2 async 超时侧车：async 反编译 job 超时（DEADLINE_EXCEEDED）此前与同步路径
//口径不一（job result 无 partial 元数据）；现 fail 超时分支与
//normalizeToolError 同口径补 {partial, reason:"timeout"}。非超时失败与取消
//零变化。
TEST(McpServerTest, AsyncJobTimeoutCarriesPartialSidecar)
{
    McpSessionHarness server;
    const auto        fixture = writeFixture();
    const auto        opened  = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

    //超时注入与同步路径同口径：slow 假反编译器（sleep 1 秒）+ 25ms 超时。
    const auto slowDecompiler = test::getTempOutPath("fake-mcp-decompiler-slow.jar");
    std::ofstream(slowDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");

    const auto started = call(server,
                              "class_source",
                              {{"workspace_id", workspaceId},
                               {"class", "mcp/Subject"},
                               {"decompiler_jar", slowDecompiler.string()},
                               {"java", fakeJava.string()},
                               {"timeout_ms", 25},
                               {"max_memory_mib", 256},
                               {"max_source_bytes", 1024 * 1024},
                               {"async", true}});
    ASSERT_FALSE(started.at("result").at("isError"));
    const auto timedOutJob = waitForTerminalJob(server, payload(started).at("job_id").get<std::string>());
    ASSERT_FALSE(timedOutJob.at("result").at("isError"));
    ASSERT_EQ("failed", payload(timedOutJob).at("state"));
    EXPECT_TRUE(payload(timedOutJob).at("result_is_error"));
    const auto& timedOutBody = payload(timedOutJob).at("result");
    EXPECT_TRUE(timedOutBody.at("partial").get<bool>());
    EXPECT_EQ("timeout", timedOutBody.at("reason").get<std::string>());
    EXPECT_EQ(static_cast<int>(ca::StatusCode::DEADLINE_EXCEEDED),
              timedOutBody.at("status").at("code").get<int>());
    releaseJob(server, payload(started).at("job_id").get<std::string>());

    //非超时失败（假反编译器立即 exit 9 → INTERNAL）不带侧车键：仅超时路径写入。
    const auto failDecompiler = test::getTempOutPath("fake-mcp-decompiler-fail.jar");
    std::ofstream(failDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto failedStart = call(server,
                                  "class_source",
                                  {{"workspace_id", workspaceId},
                                   {"class", "mcp/Subject"},
                                   {"decompiler_jar", failDecompiler.string()},
                                   {"java", fakeJava.string()},
                                   {"async", true}});
    ASSERT_FALSE(failedStart.at("result").at("isError"));
    const auto failedJob =
        waitForTerminalJob(server, payload(failedStart).at("job_id").get<std::string>());
    ASSERT_FALSE(failedJob.at("result").at("isError"));
    ASSERT_EQ("failed", payload(failedJob).at("state"));
    const auto& failedBody = payload(failedJob).at("result");
    EXPECT_EQ(static_cast<int>(ca::StatusCode::INTERNAL),
              failedBody.at("status").at("code").get<int>());
    EXPECT_FALSE(failedBody.contains("partial"));
    EXPECT_FALSE(failedBody.contains("reason"));
    releaseJob(server, payload(failedStart).at("job_id").get<std::string>());
}

}   //namespace
}   //namespace mjt::mcp
