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

///@brief M1 并发回归：长同步工具执行期间，session 工具（job_get / job_cancel /
///workspace_close）必须即时返回，不得被 executionMutex_ 阻塞；workspace_close
///后执行中的工具仍能安全完成（workspace 由 shared_ptr 保活）。
///
///场景：线程 A 用 slow 假反编译器（sleep 1 秒后成功）跑同步 decompiler_decompile，
///主线程同时调用 job_get / job_cancel / workspace_close 并测量耗时。
///修复前 call 全程持 mutex_，这些调用会被阻塞约 1 秒；修复后即时返回。
TEST(McpConcurrencyTest, SessionToolsAreNotBlockedByLongSynchronousDecompile)
{
    McpToolService service;
    const auto     fixture = writeFixture();
    auto           opened  = service.call("workspace_open",
                              {{"path", fixture.string()}, {"workspace_id", "conc"}});
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
            "decompiler_decompile",
            {{"workspace_id", "conc"},
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

    //job_get：长工具执行期间必须即时返回 "job not found"，而非等待反编译结束。
    const auto jobGetBegin = std::chrono::steady_clock::now();
    auto       jobGet      = service.call("job_get", {{"job_id", "conc-missing"}});
    const auto jobGetElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - jobGetBegin)
                                     .count();
    EXPECT_TRUE(jobGet.at("isError").get<bool>());
    EXPECT_LT(jobGetElapsedMs, 500)
        << "job_get was blocked behind the long synchronous decompile (" << jobGetElapsedMs
        << " ms)";

    //job_cancel：同样即时返回。
    const auto jobCancelBegin = std::chrono::steady_clock::now();
    auto       jobCancel      = service.call("job_cancel", {{"job_id", "conc-missing"}});
    const auto jobCancelElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - jobCancelBegin)
                                        .count();
    EXPECT_TRUE(jobCancel.at("isError").get<bool>());
    EXPECT_LT(jobCancelElapsedMs, 500)
        << "job_cancel was blocked behind the long synchronous decompile ("
        << jobCancelElapsedMs << " ms)";

    //workspace_close：即时返回（仅状态锁），且执行中的工具持有 shared_ptr，
    //关闭后仍能安全完成。
    const auto closeBegin = std::chrono::steady_clock::now();
    auto       closed     = service.call("workspace_close", {{"workspace_id", "conc"}});
    const auto closeElapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - closeBegin)
                                    .count();
    EXPECT_FALSE(closed.at("isError").get<bool>());
    EXPECT_LT(closeElapsedMs, 500)
        << "workspace_close was blocked behind the long synchronous decompile ("
        << closeElapsedMs << " ms)";

    decompilerThread.join();
    EXPECT_TRUE(finished.load());
    //慢反编译器 sleep 后成功写出假源码，同步反编译最终成功（workspace 已 close，
    //shared_ptr 保活使工具正常完成）。
    EXPECT_FALSE(decompileResult.at("isError").get<bool>());
}

}   //namespace
}   //namespace mjt::mcp
