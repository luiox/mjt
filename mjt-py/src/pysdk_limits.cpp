#include "pysdk_limits.hpp"

// pysdk_limits.cpp — deadline 状态与 watchdog 实现（纯 C++，无 Python 依赖）。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace mjt::pysdk {

namespace {

std::atomic<std::int64_t> g_deadlineMs{0};   // steady clock 毫秒时点；0=无上限

std::mutex                   g_watchdogMutex;
std::thread                  g_watchdog;
std::function<void(std::int64_t)> g_hook;    // g_watchdogMutex 保护
// 提前收线程旗标：stop 时置位，watchdog 醒来即退（不 join 等到点）。
// 竞态窗口：旗标置位瞬间线程恰已过旗标检查且到点——仍会触发一次，
// 仅发生在「deadline 与 stop 同瞬间」的边界，接受。
std::atomic<bool>            g_watchdogStop{false};

std::int64_t steadyNowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}   // namespace

void setDeadlineMs(std::int64_t ms)
{
    if (ms == 0) {
        g_deadlineMs.store(0, std::memory_order_relaxed);
        return;
    }
    const std::int64_t deadline = ms > 0 ? steadyNowMs() + ms : 1;
    g_deadlineMs.store(deadline, std::memory_order_relaxed);
}

std::int64_t deadlineRemainingMs()
{
    const std::int64_t deadline = g_deadlineMs.load(std::memory_order_relaxed);
    if (deadline == 0) return -1;
    const std::int64_t remaining = deadline - steadyNowMs();
    return remaining > 0 ? remaining : 0;
}

void deadlineCheck()
{
    const std::int64_t deadline = g_deadlineMs.load(std::memory_order_relaxed);
    if (deadline == 0) return;
    if (steadyNowMs() >= deadline) {
        throw DeadlineExceeded("script deadline exceeded");
    }
}

void startDeadlineWatchdog(std::int64_t ms, DeadlineExitHook hook)
{
    stopDeadlineWatchdog();
    if (ms <= 0) return;

    // 时点在 start 线程取好，watchdog 只睡到点——setDeadlineMs 之后再改
    // deadline 不影响已起的 watchdog（每次 --script 只装一轮，够用且简单）。
    const std::int64_t deadline = steadyNowMs() + ms;
    {
        std::lock_guard<std::mutex> lock(g_watchdogMutex);
        g_hook = std::move(hook);
    }
    g_watchdogStop.store(false, std::memory_order_relaxed);
    g_watchdog = std::thread([deadline, ms] {
        for (;;) {
            if (g_watchdogStop.load(std::memory_order_relaxed)) return;
            const std::int64_t remaining = deadline - steadyNowMs();
            if (remaining <= 0) break;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(remaining < 50 ? remaining : 50));
        }
        DeadlineExitHook fire;
        {
            std::lock_guard<std::mutex> lock(g_watchdogMutex);
            fire = g_hook;
        }
        if (fire) fire(ms);
    });
}

void stopDeadlineWatchdog()
{
    std::thread dying;
    {
        std::lock_guard<std::mutex> lock(g_watchdogMutex);
        dying.swap(g_watchdog);
        g_hook = nullptr;
    }
    // 先置旗标再 join：协作层提前退出时（常态），不等 watchdog 睡满武装时点。
    g_watchdogStop.store(true, std::memory_order_relaxed);
    if (dying.joinable()) dying.join();
}

void hardExitHook(std::int64_t ms)
{
    std::fprintf(stderr,
                 "mjt: script deadline exceeded and hard-exit grace elapsed"
                 " (armed at %lld ms), terminating\n",
                 static_cast<long long>(ms));
    std::fflush(stderr);
    std::_Exit(3);
}

}   // namespace mjt::pysdk
