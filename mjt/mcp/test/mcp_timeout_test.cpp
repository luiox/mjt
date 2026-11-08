#include <chrono>
#include <filesystem>
#include <fstream>
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

//查询超时诊断（#71）：超时错误不再静默挂 deadline——payload 附 hints 数组，
//按工具与 kind 给指引（kind=list 分页游标切片、search 宽查询收敛、kind=jasm
//方法级粒度 + pysdk 通道）。仅超时路径写入；孤儿收尾后成功路径无 hints 键
//（成功语义零变化）。宽查询夹具用 3000 类的 bulk jar（逐类解码线性成本，
//CI 快机上仍稳定越过 1ms 档——单类 jar 的 kind=list 在 CI 实测 <1ms 不超时）。
TEST(McpToolTimeout, QueryTimeoutCarriesDiagnosticHints)
{
    McpSessionHarness server;
    const auto bulkJar  = writeBulkLongNameFixture(3000, 0);
    const auto bulkOpen = call(server, "project_open", {{"path", bulkJar.string()}});
    ASSERT_FALSE(isError(bulkOpen)) << payload(bulkOpen).dump();
    const auto bulkProject = payload(bulkOpen).at("project_id").get<std::string>();

    const auto wideFixture =
        writeWideClassFixture("mjt_mcp_timeout_hints.jar", "mcp/WideHints", 3000);
    const auto wideOpen = call(server, "project_open", {{"path", wideFixture.string()}});
    ASSERT_FALSE(isError(wideOpen)) << payload(wideOpen).dump();
    const auto wideProject = payload(wideOpen).at("project_id").get<std::string>();

    const auto joinedHints = [](const Json& body) {
        std::string joined;
        for (const auto& hint : body.at("hints")) joined += hint.get<std::string>() + "\n";
        return joined;
    };
    //超时观察窗（既有口径）：1ms 档必然超时，重试兜调度抖动。
    const auto awaitTimeout = [&](const std::string& projectId, const char* tool, Json arguments) {
        Json response;
        bool sawTimeout = false;
        for (int attempt = 0; attempt < 50 && !sawTimeout; ++attempt) {
            arguments["project_id"] = projectId;
            arguments["timeout_ms"] = 1;
            response   = call(server, tool, arguments);
            sawTimeout = isError(response) &&
                         payload(response).value("reason", std::string{}) == "timeout";
        }
        EXPECT_TRUE(sawTimeout) << payload(response).dump();
        return payload(response);
    };

    // class_query kind=list（逐类摘要解码）：分页游标切片指引。
    const auto listBody = awaitTimeout(bulkProject, "class_query", Json{{"kind", "list"}});
    ASSERT_TRUE(listBody.contains("hints"));
    const auto listHints = joinedHints(listBody);
    EXPECT_NE(listHints.find("page_size"), std::string::npos);
    EXPECT_NE(listHints.find("cursor"), std::string::npos);

    // search 宽查询（contains 全类名扫描，query=m 恒命中）：收敛 + 分页指引。
    const auto searchBody =
        awaitTimeout(bulkProject, "search", Json{{"kind", "classes"}, {"query", "m"}});
    ASSERT_TRUE(searchBody.contains("hints"));
    const auto searchHints = joinedHints(searchBody);
    EXPECT_NE(searchHints.find("prefix"), std::string::npos);
    EXPECT_NE(searchHints.find("cursor"), std::string::npos);

    // class_query kind=jasm（3000 方法全类 dump）：方法级粒度 + pysdk 通道指引。
    const auto jasmBody = awaitTimeout(wideProject,
                                      "class_query",
                                      Json{{"kind", "jasm"}, {"class", "mcp/WideHints"}});
    ASSERT_TRUE(jasmBody.contains("hints"));
    const auto jasmHints = joinedHints(jasmBody);
    EXPECT_NE(jasmHints.find("method_jasm"), std::string::npos);
    EXPECT_NE(jasmHints.find("mjt-py SDK"), std::string::npos);

    //孤儿收尾后成功路径恢复：完整页无 hints 键（成功语义零变化）。
    bool recovered = false;
    Json ok;
    for (int attempt = 0; attempt < 600 && !recovered; ++attempt) {
        ok = call(server, "class_query", {{"project_id", wideProject}, {"kind", "list"}});
        recovered = !isError(ok);
        if (!recovered) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ASSERT_TRUE(recovered) << payload(ok).dump();
    EXPECT_FALSE(payload(ok).contains("hints"));

    const auto bulkClosed = call(server, "project_close", {{"project_id", bulkProject}});
    EXPECT_FALSE(isError(bulkClosed));
    const auto wideClosed = call(server, "project_close", {{"project_id", wideProject}});
    EXPECT_FALSE(isError(wideClosed));
}

//decompile 超时诊断（#71）：反编译子进程超时的失败面附通道指引（async 轮询/
//方法级粒度/pysdk 通道），同步与 async job 两个超时出口同源；非超时业务错误
//零变化（无 hints 键）。
TEST(McpToolTimeout, DecompileTimeoutCarriesChannelHints)
{
    McpSessionHarness server;
    const auto        fixture = writeFixture();
    const auto        opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(isError(opened)) << payload(opened).dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //超时注入（mcp_partial_test 同口径）：slow 假反编译器（test 二进制充当
    //java 进程，sleep 1 秒）+ 25ms 超时 → DEADLINE_EXCEEDED。
    const auto slowDecompiler = test::getTempOutPath("fake-mcp-decompiler-hints.jar");
    std::ofstream(slowDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");
    const auto baseArguments = Json{{"decompiler_jar", slowDecompiler.string()},
                                    {"java", fakeJava.string()},
                                    {"timeout_ms", 25},
                                    {"max_memory_mib", 256},
                                    {"max_source_bytes", 1024 * 1024}};

    const auto hintsText = [](const Json& body) {
        std::string joined;
        for (const auto& hint : body.at("hints")) joined += hint.get<std::string>() + "\n";
        return joined;
    };

    //同步超时：hints 指引 async 轮询 + 方法级粒度 + pysdk 通道。
    Json syncArguments = baseArguments;
    syncArguments["project_id"] = projectId;
    syncArguments["class"]      = "mcp/Subject";
    const auto timedOut         = call(server, "decompile", syncArguments);
    ASSERT_TRUE(timedOut.at("result").at("isError")) << payload(timedOut).dump();
    const auto& timedOutBody = payload(timedOut);
    EXPECT_EQ(static_cast<int>(ca::StatusCode::DEADLINE_EXCEEDED),
              timedOutBody.at("status_code").get<int>());
    ASSERT_TRUE(timedOutBody.contains("hints"));
    const auto syncHints = hintsText(timedOutBody);
    EXPECT_NE(syncHints.find("async=true"), std::string::npos);
    EXPECT_NE(syncHints.find("job_get"), std::string::npos);
    EXPECT_NE(syncHints.find("mjt-py SDK"), std::string::npos);

    //async job 超时：job result 侧车同源带 hints。
    Json asyncArguments = baseArguments;
    asyncArguments["project_id"] = projectId;
    asyncArguments["class"]      = "mcp/Subject";
    asyncArguments["async"]      = true;
    const auto started           = call(server, "decompile", asyncArguments);
    ASSERT_FALSE(started.at("result").at("isError")) << payload(started).dump();
    const auto jobId = payload(started).at("job_id").get<std::string>();
    const auto timedOutJob = waitForTerminalJob(server, projectId, jobId);
    ASSERT_FALSE(timedOutJob.at("result").at("isError"));
    ASSERT_EQ("failed", payload(timedOutJob).at("state"));
    const auto& jobResult = payload(timedOutJob).at("result");
    EXPECT_EQ(static_cast<int>(ca::StatusCode::DEADLINE_EXCEEDED),
              jobResult.at("status").at("code").get<int>());
    ASSERT_TRUE(jobResult.contains("hints"));
    EXPECT_NE(hintsText(jobResult).find("job_get"), std::string::npos);
    releaseJob(server, projectId, jobId);

    //非超时业务错误（class 不存在）不带 hints：其余失败零变化。
    const auto missing = call(server,
                              "decompile",
                              {{"project_id", projectId}, {"class", "mcp/Missing"}});
    ASSERT_TRUE(missing.at("result").at("isError"));
    EXPECT_FALSE(payload(missing).contains("hints"));
}

}   //namespace
}   //namespace mjt::mcp
