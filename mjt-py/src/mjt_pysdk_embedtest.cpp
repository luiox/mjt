// mjt_pysdk_embedtest.cpp — 嵌入宿主进程级测试（--script 退出码契约）。
//
// 为什么不是 gtest：被测契约里有 watchdog 硬退出（_Exit(3)，测试进程自己
// 活不下来）和「每个场景一轮完整 init/finalize」的进程级行为——单进程单测
// 进不来。结构：无参运行 = 逐场景 spawn 自身并断言子进程退出码；
// <scenario> 单参 = 在本进程跑该场景（父进程专用入口，勿手敲）。
//
// 场景与契约（pysdk_embed.hpp 退出码口径）：
//   clean            干净脚本                       → 0
//   syntax_error     编译期语法错误                 → 1（traceback）
//   deadline_coop    纯 Python 死循环 + 400ms 期限  → 1（协作层 trace 中断，
//                                                     而非 3——证明不是
//                                                     watchdog 兜的底）
//   deadline_watchdog time.sleep 阻塞 + 400ms 期限  → 3（硬层；trace/检查点
//                                                     在原生调用里够不着）
//   output_cap       大量打印 + 1000 字节上限       → 1

#include <Python.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "pysdk_embed.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;

/// 场景脚本写到临时目录（零样本入库），返回路径；临时目录不可用退 cwd。
std::string writeScenario(const char* name, const char* body)
{
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec) dir = ".";
    const fs::path path = dir / ("mjt_embedtest_" + std::string(name) + ".py");
    std::ofstream out(path, std::ios::binary);
    if (!out) return {};
    out.write(body, static_cast<std::streamsize>(std::strlen(body)));
    return path.u8string();
}

std::string selfPath()
{
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return buf;
#else
    return "/proc/self/exe";
#endif
}

int runScenario(const std::string& scenario)
{
    mjt::pysdk::EmbedOptions embed;
    if (scenario == "clean") {
        embed.scriptPath = writeScenario("clean", "print('clean-ok')\n");
    }
    else if (scenario == "syntax_error") {
        embed.scriptPath = writeScenario("syntax_error", "def = 1\n");
    }
    else if (scenario == "deadline_coop") {
        // 期限要给足余量：脚本起步前要付 init + import mjt 的成本，预算太紧
        // 会变成 watchdog 先到（rc 3，协作层根本没有中断窗口）。
        embed.scriptPath = writeScenario("deadline_coop", "while True:\n    pass\n");
        embed.deadlineMs = 2000;
    }
    else if (scenario == "deadline_watchdog") {
        embed.scriptPath =
            writeScenario("deadline_watchdog", "import time\ntime.sleep(60)\n");
        embed.deadlineMs = 400;
    }
    else if (scenario == "output_cap") {
        embed.scriptPath =
            writeScenario("output_cap", "for _ in range(100000):\n    print('x' * 64)\n");
        embed.maxOutputBytes = 1000;
    }
    else {
        std::fprintf(stderr, "unknown scenario: %s\n", scenario.c_str());
        return 2;
    }
    if (embed.scriptPath.empty()) {
        std::fprintf(stderr, "cannot write scenario script: %s\n", scenario.c_str());
        return 2;
    }
    return mjt::pysdk::runEmbeddedScript(embed);
}

}   // namespace

int main(int argc, char* argv[])
{
    // 单场景模式：父进程 spawn 进来的实际执行体。
    if (argc == 2) return runScenario(argv[1]);

    struct Expectation
    {
        const char*      scenario;
        int              exitCode;
        double           maxSeconds;   // 超时即挂（watchdog 场景兜底验证）
    };
    const Expectation expectations[] = {
        {"clean", 0, 60},
        {"syntax_error", 1, 60},
        {"deadline_coop", 1, 30},
        {"deadline_watchdog", 3, 30},
        {"output_cap", 1, 60},
    };

    int failures = 0;
    for (const auto& expect : expectations) {
        const std::string command =
            "\"" + selfPath() + "\" " + expect.scenario;
        const auto started = std::chrono::steady_clock::now();
        const int  rc      = std::system(command.c_str());
        const auto elapsed = std::chrono::duration<double>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
        const bool codeOk  = rc == expect.exitCode;
        const bool timeOk  = elapsed < expect.maxSeconds;
        std::printf("%-18s rc=%-3d (%s) %.1fs (%s)\n",
                    expect.scenario, rc,
                    codeOk ? "ok" : "FAIL", elapsed,
                    timeOk ? "ok" : "SLOW");
        if (!codeOk || !timeOk) ++failures;
    }
    std::printf(failures == 0 ? "embedtest: all green\n" : "embedtest: %d failures\n",
                failures);
    return failures == 0 ? 0 : 1;
}
