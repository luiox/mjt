#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

//仓内断言口径：isError 在响应信封的 result 层，structuredContent 里没有。
bool isError(const Json& response) { return response.at("result").at("isError").get<bool>(); }

//大元数据夹具：单类挂 methodCount 个两指令方法。kind=class 的全量元数据解码
//（method_info 顺序扫描 + Json 组装）耗时稳定超过 1ms 超时档——超时路径的
//确定性触发器，不依赖调度抖动。
std::filesystem::path writeWideClassFixture(const std::string& fileName,
                                            const std::string& internalName,
                                            int                methodCount)
{
    std::string source = ".super java/lang/Object\n.class public super " + internalName + " {\n";
    for (int index = 0; index < methodCount; ++index) {
        source += "  .method public static m" + std::to_string(index) + " ()I {\n"
                  "    code: {\n      bipush " + std::to_string(index % 128) +
                  "\n      ireturn\n    }\n  }\n";
    }
    source += "}\n";
    mjbase::model::ResourceContainer container;
    const auto                       uri = internalName + ".class";
    container.addResource(uri,
                          std::make_unique<mjbase::model::ClassResource>(
                              uri, assembleFixtureClass(internalName + ".jasm", source)));
    const auto path = test::getTempOutPath(fileName);
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

//schema 校验面：越界 timeout_ms（0 / 超上限）在执行前被契约拒绝。
TEST(McpToolTimeout, SchemaRejectsOutOfRangeTimeout)
{
    McpSessionHarness server;
    const auto        fixture = writeFixture();
    const auto opened = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(isError(opened)) << payload(opened).dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    for (const auto bad : Json::array({0, 300001})) {
        const auto resp = call(server,
                               "class_query",
                               {{"project_id", projectId}, {"kind", "list"}, {"timeout_ms", bad}});
        EXPECT_TRUE(isError(resp)) << payload(resp).dump();
    }
    const auto closed = call(server, "project_close", {{"project_id", projectId}});
    EXPECT_FALSE(isError(closed));
}

//查询超时面（#25 验收核心）：1ms 档在正常负载下必然超时（线程启动 + workspace
//解码远超 1ms），重试若干次直到观察到超时形态；随后验证——结构化字段齐备、
//session 面即时返回（通道不被孤儿阻塞）、查询面在孤儿收尾后照常可用。
TEST(McpToolTimeout, QueryTimeoutReturnsStructuredErrorAndSessionSurvives)
{
    McpSessionHarness server;
    const auto fixture = writeWideClassFixture("mjt_mcp_timeout_wide.jar", "mcp/Wide", 3000);
    const auto opened = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(isError(opened)) << payload(opened).dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    bool sawTimeout  = false;
    Json timeoutResp;
    for (int attempt = 0; attempt < 50 && !sawTimeout; ++attempt) {
        timeoutResp = call(server,
                           "class_query",
                           {{"project_id", projectId},
                            {"kind", "class"},
                            {"class", "mcp/Wide"},
                            {"timeout_ms", 1}});
        sawTimeout = isError(timeoutResp) &&
                     payload(timeoutResp).value("reason", std::string{}) == "timeout";
    }
    ASSERT_TRUE(sawTimeout) << payload(timeoutResp).dump();
    const auto& body = payload(timeoutResp);
    EXPECT_EQ(body.at("operation").get<std::string>(), "class_query");
    EXPECT_EQ(body.at("project_id").get<std::string>(), projectId);
    EXPECT_GT(body.at("elapsed_ms").get<ca::i64>(), 0);
    EXPECT_TRUE(body.contains("status"));

    //session 面（状态锁，短临界区）不被孤儿执行体阻塞。
    const auto info = call(server, "project_info", {{"project_id", projectId}});
    EXPECT_FALSE(isError(info)) << payload(info).dump();

    //孤儿收尾后查询面恢复：串行执行锁保证无并发访问，缺省窗口内必然完成。
    bool recovered = false;
    for (int attempt = 0; attempt < 600 && !recovered; ++attempt) {
        const auto resp = call(server,
                               "class_query",
                               {{"project_id", projectId}, {"kind", "list"}});
        recovered = !isError(resp);
        if (!recovered) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(recovered);

    const auto closed = call(server, "project_close", {{"project_id", projectId}});
    EXPECT_FALSE(isError(closed));
}

//project_open 超时面：预订 id 随超时错误外透，孤儿完成后会话照常可用（可
//project_info / project_close）。小 jar 的打开可能整体快于 1ms 档——重试窗口内
//一次都未观察到超时则跳过（机器过快，非缺陷）。
TEST(McpToolTimeout, ProjectOpenTimeoutCarriesReservedIdAndSessionSurvives)
{
    McpSessionHarness server;
    const auto fixture = writeWideClassFixture("mjt_mcp_timeout_open.jar", "mcp/WideOpen", 3000);

    bool sawTimeout = false;
    std::string reservedId;
    for (int attempt = 0; attempt < 50 && !sawTimeout; ++attempt) {
        const auto resp = call(server, "project_open", {{"path", fixture.string()}, {"timeout_ms", 1}});
        const auto& body = payload(resp);
        if (isError(resp) && body.value("reason", std::string{}) == "timeout") {
            sawTimeout = true;
            reservedId = body.value("project_id", std::string{});
        }
        else if (!isError(resp)) {
            //本轮正常打开：收掉再试，避免会话表涨满。
            call(server, "project_close", {{"project_id", body.at("project_id").get<std::string>()}});
        }
    }
    if (!sawTimeout) {
        GTEST_SKIP() << "project_open completed within 1ms on every attempt (too-fast host)";
    }
    EXPECT_FALSE(reservedId.empty());

    //孤儿 open 完成后会话登记生效：轮询直到可寻址（上限 30s）。
    bool registered = false;
    for (int attempt = 0; attempt < 600 && !registered; ++attempt) {
        const auto info = call(server, "project_info", {{"project_id", reservedId}});
        registered = !isError(info);
        if (!registered) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(registered) << "timed-out project_open never completed in background";
    const auto closed = call(server, "project_close", {{"project_id", reservedId}});
    EXPECT_FALSE(isError(closed));
}

}   //namespace
}   //namespace mjt::mcp
