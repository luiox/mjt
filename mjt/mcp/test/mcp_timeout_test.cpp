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
    std::string source =
        R"JASM(.super java/lang/Object
.class public super )JASM" + internalName +
        R"JASM( {
)JASM";
    for (int index = 0; index < methodCount; ++index) {
        source += R"JASM(  .method public static m)JASM" + std::to_string(index) +
                  R"JASM( ()I {
    code: {
      bipush )JASM" + std::to_string(index % 128) +
                  R"JASM(
      ireturn
    }
  }
)JASM";
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

//胖 bulk 夹具：count 个类、每类 methodCount × pairsPerMethod 对指令的码量
//（近似真实容器的指令总量），外加瘦对照类 mcp/Normal（bipush 2，diff 补丁
//目标）。胖类经装配一次 + ClassWriter 重写内部名铺开（装配器成本不随类数
//增长）。#80 耗时回归门用：off 档尾部的 O(容器) 扫描在该体积上确定性放大。
std::filesystem::path writeFatBulkFixture(const std::string& fileName,
                                          std::size_t        count,
                                          int                methodCount,
                                          int                pairsPerMethod)
{
    std::string source =
        R"JASM(.super java/lang/Object
.class public super mcp/FatTemplate {
)JASM";
    for (int index = 0; index < methodCount; ++index) {
        source += R"JASM(  .method public static m)JASM" + std::to_string(index) +
                  R"JASM( ()I {
    code: {
      bipush )JASM" + std::to_string(index % 128) +
                  R"JASM(
)JASM";
        for (int pair = 0; pair < pairsPerMethod; ++pair) {
            source += R"JASM(      bipush )JASM" + std::to_string(pair % 128) +
                      R"JASM(
      pop
)JASM";
        }
        source += R"JASM(      ireturn
    }
  }
)JASM";
    }
    source += "}\n";
    const auto fatBytes = assembleFixtureClass("mcp/FatTemplate.jasm", source);

    const auto normalSource = std::string{R"JASM(.super java/lang/Object
.class public super mcp/Normal {
  .field public counter I
  .method public static value ()I {
    code: {
      bipush 2
      ireturn
    }
  }
}
)JASM"};
    const auto normalBytes  = assembleFixtureClass("mcp/Normal.jasm", normalSource);

    mjbase::model::ResourceContainer container;
    container.addResource("mcp/Normal.class",
                          std::make_unique<mjbase::model::ClassResource>("mcp/Normal.class",
                                                                         normalBytes));
    for (std::size_t index = 0; index < count; ++index) {
        const auto name = "mcp/Fat" + std::to_string(index);
        const auto bytes = withClassNameAndSignature(fatBytes, name, "");
        const auto uri   = name + ".class";
        container.addResource(uri, std::make_unique<mjbase::model::ClassResource>(uri, bytes));
    }
    const auto path = test::getTempOutPath(fileName);
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

//单行替换 unified diff（mcp_jasm_patch_test 同款；行号由运行时定位）。
std::string singleLineDiff(const std::string& base,
                           const std::string& oldValue,
                           const std::string& newValue)
{
    const auto marker = base.find(oldValue);
    EXPECT_NE(std::string::npos, marker);
    const auto lineStart = base.rfind('\n', marker) + 1;
    ca::usize lineNumber = 1;
    for (ca::usize i = 0; i < lineStart; ++i)
        if (base[i] == '\n') ++lineNumber;
    const auto oldValueLine = base.substr(lineStart, base.find('\n', lineStart) - lineStart);
    const std::string newValueLine =
        std::string(oldValueLine).replace(oldValueLine.find(oldValue), oldValue.size(), newValue);
    return "@@ -" + std::to_string(lineNumber) + ",1 +" + std::to_string(lineNumber) + ",1 @@\n" +
           "-" + oldValueLine + "\n+" + newValueLine + "\n";
}

//edit_apply 耗时测量：返回 {端到端毫秒, 响应}（MCP 信封往返全包）。
std::pair<ca::i64, Json> editApplyTimed(McpSessionHarness& server, Json arguments)
{
    const auto start = std::chrono::steady_clock::now();
    auto       resp  = call(server, "edit_apply", std::move(arguments));
    const auto ms    = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
    return {static_cast<ca::i64>(ms), std::move(resp)};
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

//edit_apply validation=off 耗时回归门（#80）：off 档跳过校验门但曾经把
//受影响引用扫描押在全容器 IR 重建上——大 jar 上 off dry_run 比 relaxed
//（门禁早退）慢一个数量级，挂死客户端 deadline 且无诊断。修复后两者的
//共 Tail（clone + 双 pass 装配 + 层次图 + 字节 diff）同为 O(容器)，引用
//扫描收敛为 O(受影响条目)，off 与 relaxed 同量级。断言形态：以 relaxed
//门禁早退调用为同机同夹具基线，off 耗时不得超过基线 3 倍 + 1.5s 余量
//（CI 快慢机两侧同缩放；夹具体积保证 pre-fix 形态超界 10 倍以上）。
//kind=class_bytes + validation=off 走同一尾部，同门覆盖（issue 点名的
//未测面）。relaxed 坏 diff 的 gate=rebuild 错误同时是功能锚点：证明两
//次调用确实走了「门禁早退 vs 全管线」两条路。
TEST(McpToolTimeout, EditApplyValidationOffLatencyTracksRelaxed)
{
    McpSessionHarness server;
    const auto fixture = writeFatBulkFixture("mjt_mcp_timeout_off_latency.jar", 2000, 20, 100);
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(isError(opened)) << payload(opened).dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto jasmQuery = call(server,
                                "class_query",
                                {{"project_id", projectId}, {"kind", "jasm"}, {"class", "mcp/Normal"}});
    ASSERT_FALSE(isError(jasmQuery)) << payload(jasmQuery).dump();
    const auto base = payload(jasmQuery).at("jasm").get<std::string>();
    //坏 diff：ireturn 前栈顶换 null 引用——assemblable、relaxed 门禁必拒
    //（复现 issue 非对称面的确定性触发器，不依赖语义错误是否被装配器拦截）。
    const auto badDiff = singleLineDiff(base, "ldc 2", "aconst_null");

    //基线：relaxed 门禁早退（gate=rebuild）。
    auto [relaxedMs, relaxedResp] = editApplyTimed(server,
                                                   Json{{"kind", "jasm_diff"},
                                                        {"uri", "mcp/Normal.class"},
                                                        {"diff", badDiff},
                                                        {"project_id", projectId},
                                                        {"dry_run", true}});
    ASSERT_TRUE(isError(relaxedResp)) << payload(relaxedResp).dump();
    const auto& relaxedBody = payload(relaxedResp);
    ASSERT_TRUE(relaxedBody.at("diagnostics").is_array());
    ASSERT_GT(relaxedBody.at("diagnostics").size(), 0);
    EXPECT_EQ("rebuild", relaxedBody.at("diagnostics").at(0).value("gate", std::string{}));

    //被测面：off 全管线 dry_run（成功，引用扫描不拖尾）。
    auto [offMs, offResp] = editApplyTimed(server,
                                           Json{{"kind", "jasm_diff"},
                                                {"uri", "mcp/Normal.class"},
                                                {"diff", badDiff},
                                                {"project_id", projectId},
                                                {"validation", "off"},
                                                {"dry_run", true}});
    ASSERT_FALSE(isError(offResp)) << payload(offResp).dump();
    const auto& offBody = payload(offResp);
    EXPECT_EQ(1, offBody.at("byte_diffs").size());
    //作用域收敛的引用扫描仍如实产出受影响引用（瘦类的 super 声明）。
    EXPECT_FALSE(offBody.at("affected_references").empty());

    //kind=class_bytes + validation=off（issue 点名的未测面）：换体 bipush 9。
    const auto normalSource = std::string{R"JASM(.super java/lang/Object
.class public super mcp/Normal {
  .field public counter I
  .method public static value ()I {
    code: {
      bipush 9
      ireturn
    }
  }
}
)JASM"};
    const auto replacedBytes = assembleFixtureClass("mcp/Normal.jasm", normalSource);
    auto [bytesMs, bytesResp] = editApplyTimed(server,
                                               Json{{"kind", "class_bytes"},
                                                    {"class_uri", "mcp/Normal.class"},
                                                    {"content_base64", encodeBase64(replacedBytes)},
                                                    {"project_id", projectId},
                                                    {"validation", "off"},
                                                    {"dry_run", true}});
    ASSERT_FALSE(isError(bytesResp)) << payload(bytesResp).dump();
    EXPECT_EQ(1, payload(bytesResp).at("byte_diffs").size());

    //同机同夹具基线下的比例门（3x + 1.5s 余量）。
    EXPECT_LE(offMs, relaxedMs * 3 + 1500) << "off=" << offMs << " relaxed=" << relaxedMs;
    EXPECT_LE(bytesMs, relaxedMs * 3 + 1500) << "bytes=" << bytesMs << " relaxed=" << relaxedMs;

    const auto closed = call(server, "project_close", {{"project_id", projectId}});
    EXPECT_FALSE(isError(closed));
}

}   //namespace
}   //namespace mjt::mcp
