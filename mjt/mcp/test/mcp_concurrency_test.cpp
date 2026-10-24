#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

///@brief M1 并发回归：长同步工具执行期间，session 工具（decompile 的
///poll / cancel 作业面、project_close）必须即时返回，不得被 executionMutex_
///阻塞；project_close 后执行中的工具仍能安全完成（工程由 shared_ptr 保活）。
///
///场景：线程 A 用 slow 假反编译器（sleep 1 秒后成功）跑同步 decompile，主线程
///同时调用 decompile(action=poll/cancel) / project_close 并测量耗时。
///修复前 call 全程持 mutex_，这些调用会被阻塞约 1 秒；修复后即时返回。
TEST(McpConcurrencyTest, SessionToolsAreNotBlockedByLongSynchronousDecompile)
{
    McpToolService service;
    const auto     fixture = writeFixture();
    auto           opened  = service.call("project_open",
                              {{"path", fixture.string()}, {"project_id", "conc"}});
    ASSERT_FALSE(opened.at("isError").get<bool>());

    //slow 假反编译器：test 二进制充当 java 进程，`fake-mcp-decompiler-slow.jar`
    //触发 sleep 1 秒后写出假源码并成功返回（见 mcp_test_fixture.cpp runFakeDecompiler）。
    const auto slowDecompiler = test::getTempOutPath("fake-mcp-decompiler-slow.jar");
    std::ofstream(slowDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");

    std::atomic<bool> started{false};
    std::atomic<bool> finished{false};
    Json              decompileResult;
    std::thread       decompilerThread([&] {
        started.store(true);
        decompileResult = service.call(
            "decompile",
            {{"project_id", "conc"},
             {"class", "mcp/Subject"},
             {"decompiler_jar", slowDecompiler.string()},
             {"java", fakeJava.string()}});
        finished.store(true);
    });
    while (!started.load()) std::this_thread::yield();
    //等线程 A 进入执行体（假反编译器 sleep 期间仍持有 executionMutex_）。
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (finished.load()) {
        decompilerThread.join();
        FAIL() << "slow decompile finished too early; fake java not engaged";
        return;
    }

    //decompile(action=poll)：长工具执行期间必须即时返回 "job not found"，
    //而非等待反编译结束。
    const auto pollBegin     = std::chrono::steady_clock::now();
    auto       pollMissing   = service.call("decompile",
                                   {{"project_id", "conc"}, {"action", "poll"}, {"job_id", "conc-missing"}});
    const auto pollElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - pollBegin)
                                   .count();
    EXPECT_TRUE(pollMissing.at("isError").get<bool>());
    EXPECT_LT(pollElapsedMs, 500)
        << "decompile poll was blocked behind the long synchronous decompile (" << pollElapsedMs
        << " ms)";

    //decompile(action=cancel)：同样即时返回。
    const auto cancelBegin     = std::chrono::steady_clock::now();
    auto       cancelMissing   = service.call("decompile",
                                     {{"project_id", "conc"}, {"action", "cancel"}, {"job_id", "conc-missing"}});
    const auto cancelElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - cancelBegin)
                                     .count();
    EXPECT_TRUE(cancelMissing.at("isError").get<bool>());
    EXPECT_LT(cancelElapsedMs, 500)
        << "decompile cancel was blocked behind the long synchronous decompile ("
        << cancelElapsedMs << " ms)";

    //project_close：即时返回（仅状态锁），且执行中的工具持有 shared_ptr，
    //关闭后仍能安全完成。
    const auto closeBegin = std::chrono::steady_clock::now();
    auto       closed     = service.call("project_close", {{"project_id", "conc"}});
    const auto closeElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - closeBegin)
                                    .count();
    EXPECT_FALSE(closed.at("isError").get<bool>());
    EXPECT_LT(closeElapsedMs, 500)
        << "project_close was blocked behind the long synchronous decompile ("
        << closeElapsedMs << " ms)";

    decompilerThread.join();
    EXPECT_TRUE(finished.load());
    //慢反编译器 sleep 后成功写出假源码，同步反编译最终成功（工程已 close，
    //shared_ptr 保活使工具正常完成）。
    EXPECT_FALSE(decompileResult.at("isError").get<bool>());
}

}   //namespace
}   //namespace mjt::mcp
