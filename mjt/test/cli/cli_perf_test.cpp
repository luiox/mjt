/// @file        cli_perf_test.cpp
/// @brief       CLI 全命令性能回归（软门）：运行时生成 2100 类 × 有内容方法体
///              的大 jar（生成法对照 mcp_long_identifier_perf_test：模板一次
///              jasm 汇编 + libjbc 会话面逐类改名），量 info / mapping 互转 /
///              decompile 单类的耗时上界。上界取宽松值只防数量级劣化（CI 抖
///              动防护），真实数字落 PR 正文。decompile 档按后端发现序门控，
///              CI 无反编译器 JAR 时整档跳过。
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "cli_fixture.hpp"
#include "commands.hpp"
#include "test_helper.hpp"

namespace {

using namespace mjt::cli;

/// 单次测量的墙钟毫秒。
template <typename Fn>
double measureOnce(Fn&& fn)
{
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

/// 软门断言：上限按数量级取宽松值（防劣化，不防抖动），实测值打进输出。
void expectWithinSoftBound(const char* tag, double actualMs, double boundMs)
{
    std::printf("perf %s: %.1f ms (soft bound %.0f ms)\n", tag, actualMs, boundMs);
    std::cout.flush();
    EXPECT_LT(actualMs, boundMs) << tag << " regressed beyond the soft bound";
}

constexpr ca::usize kPerfClassCount = 2100;

TEST(MjtCliPerfTest, InfoOnBigJarStaysWithinSoftBound)
{
    const auto jar = cli_fixture::writePerfClassListJar(kPerfClassCount);

    //类数正确性先行（语料自检）：2100 类 + 1 资源。
    auto counted = countJarClassEntries(jar.string());
    ASSERT_TRUE(counted.is_ok());
    EXPECT_EQ(kPerfClassCount, counted.unwrap());

    const double infoMs = measureOnce([&] {
        auto report = collectInfo(jar.string());
        ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
        const auto info = std::move(report).unwrap();
        EXPECT_EQ(static_cast<ca::i64>(kPerfClassCount), info.classes);
        // 批三新增维度自检：成员普查总量与可见性桶对账（分布不空转）。
        ca::i64 methodSum = 0;
        for (const auto& [bucket, count] : info.methodVisibility) methodSum += count;
        EXPECT_EQ(methodSum, info.methods);
        EXPECT_GT(info.methods, 0);
    });
    // 批三收紧：info 新增成员普查（censusMembers）/来源分布维度后，语料实测
    // 仍在低秒档，上界自 60s 收至 20s（仍只防数量级劣化）。
    expectWithinSoftBound("info 2100 classes (member census + origins)", infoMs, 20000.0);
}

TEST(MjtCliPerfTest, StringsOnBigJarStaysWithinSoftBound)
{
    const auto jar = cli_fixture::writePerfClassListJar(kPerfClassCount);

    const double stringsMs = measureOnce([&] {
        auto report = collectStrings(jar.string(), "", 1);
        ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
        const auto info = std::move(report).unwrap();
        EXPECT_EQ(info.classesScanned, static_cast<ca::i64>(kPerfClassCount));
        EXPECT_GT(info.uniqueValues, 0);
    });
    expectWithinSoftBound("strings 2100 classes", stringsMs, 30000.0);
}

TEST(MjtCliPerfTest, XrefOnBigJarStaysWithinSoftBound)
{
    const auto jar = cli_fixture::writePerfClassListJar(kPerfClassCount);

    const double xrefMs = measureOnce([&] {
        // from 方向（语料类 super 头声明必有类型边）：全量 ReferenceScan 建索引
        // 是查询主体，方向只影响线性过滤。
        auto report = collectXref(jar.string(), "perf/Subject0", "", "from", 200);
        ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
        EXPECT_GT(std::move(report).unwrap().total, 0);
    });
    expectWithinSoftBound("xref from perf/Subject0 (2100 classes)", xrefMs, 30000.0);
}

TEST(MjtCliPerfTest, ClassesOnBigJarStaysWithinSoftBound)
{
    const auto jar = cli_fixture::writePerfClassListJar(kPerfClassCount);

    const double classesMs = measureOnce([&] {
        auto report = collectClasses(jar.string(), "", "", 10000);
        ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
        const auto info = std::move(report).unwrap();
        // 对账自检：全量无过滤 = censusClasses 行数，无截断。
        EXPECT_EQ(info.classesInJar, static_cast<ca::i64>(kPerfClassCount));
        EXPECT_EQ(info.total, static_cast<ca::i64>(kPerfClassCount));
        EXPECT_FALSE(info.truncated);
        EXPECT_GT(info.classes[0].internalName.size(), 0);
    });
    // classes 与 info 同走整 jar census 导入（语料 2100 类），上界同 info 档。
    expectWithinSoftBound("classes 2100 classes (census projection)", classesMs, 20000.0);
}

TEST(MjtCliPerfTest, DisasmSingleClassOnBigJarStaysWithinSoftBound)
{
    const auto jar = cli_fixture::writePerfClassListJar(kPerfClassCount);

    const double disasmMs = measureOnce([&] {
        auto report = collectDisasm(jar.string(), "perf/Subject0");
        ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
        // 指令形状自检：语料方法体含 iadd/ireturn（模板方法体零触碰）。
        const auto info = std::move(report).unwrap();
        EXPECT_NE(info.jasm.find("ireturn"), std::string::npos);
    });
    // 单类直读 + 反汇编，应亚秒级；上界只防数量级劣化。
    expectWithinSoftBound("disasm single class (2100-class jar)", disasmMs, 5000.0);
}

TEST(MjtCliPerfTest, SubsSingleClassOnBigJarStaysWithinSoftBound)
{
    const auto jar = cli_fixture::writePerfClassListJar(kPerfClassCount);

    const double subsMs = measureOnce([&] {
        auto report = collectSubs(jar.string(), "perf/Subject2099");
        ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
        // 对账自检：语料类 1 字段 1 方法（模板结构零触碰）。
        const auto info = std::move(report).unwrap();
        EXPECT_EQ(info.fields, 1);
        EXPECT_EQ(info.methods, 1);
        EXPECT_FALSE(info.members.empty());
    });
    // 单类 classInfo 直读，应亚秒级；上界只防数量级劣化。
    expectWithinSoftBound("subs single class (2100-class jar)", subsMs, 5000.0);
}

TEST(MjtCliPerfTest, MappingRoundTripOnBigMappingStaysWithinSoftBound)
{
    const auto srg = cli_fixture::writePerfSrgMapping(kPerfClassCount);
    //往返：SRG → Tiny-V2 → SRG，语义自检（首行类映射存活）。
    double tinyMs = 0;
    double srgMs  = 0;
    std::string tiny;
    tinyMs = measureOnce([&] {
        auto converted = convertMappingText(srg, "srg", "tiny-v2");
        ASSERT_TRUE(converted.is_ok()) << converted.unwrap_err().message;
        tiny = std::move(converted).unwrap();
    });
    ASSERT_NE(tiny.find("perf/Renamed2099"), std::string::npos);
    srgMs = measureOnce([&] {
        auto converted = convertMappingText(tiny, "tiny-v2", "srg");
        ASSERT_TRUE(converted.is_ok()) << converted.unwrap_err().message;
        EXPECT_NE(std::move(converted).unwrap().find("perf/Renamed2099"), std::string::npos);
    });
    expectWithinSoftBound("mapping srg->tiny-v2 2100 classes", tinyMs, 30000.0);
    expectWithinSoftBound("mapping tiny-v2->srg 2100 classes", srgMs, 30000.0);
}

TEST(MjtCliPerfTest, SingleClassDecompileOnBigJarStaysWithinSoftBound)
{
    const auto probe = cli_fixture::probeBackend(mjt::DecompilerKind::Cfr);
    if (!probe.available) GTEST_SKIP() << probe.reason;
    const auto jar = cli_fixture::writePerfClassListJar(kPerfClassCount);

    DecompileCliOptions options;
    options.kind          = mjt::DecompilerKind::Cfr;
    options.backendName   = "cfr";
    options.decompilerJar = probe.decompilerJar;

    //选最末位类：classpath 扫描/读回的最坏观察点。
    const std::string lastName = "perf/Subject" + std::to_string(kPerfClassCount - 1);
    const double decompileMs = measureOnce([&] {
        auto report = decompileSingleClass(jar.string(), lastName, "", options);
        ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
        //CFR 产物按包拆分（package perf; + class Subject2099），钉简单名形态。
        EXPECT_NE(std::move(report).unwrap().source.find("class Subject2099"),
                  std::string::npos);
    });
    expectWithinSoftBound("decompile single class (2100-class classpath)", decompileMs,
                          60000.0);
}

}   // namespace
