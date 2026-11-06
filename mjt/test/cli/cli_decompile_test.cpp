/// @file        cli_decompile_test.cpp
/// @brief       `mjt decompile` 命令核心面测试：敌意名预检（纯逻辑）、单类
///              短安全名方案（#63 A/B 根治门）、批量跳过披露、zip 契约、
///              超时与用法分支。真后端用例按发现序门控（无 JAR/Java 跳过）。
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "libca/zip/file.hpp"

#include "cli_fixture.hpp"
#include "commands.hpp"
#include "shared/decompiler.hpp"
#include "test_helper.hpp"

namespace {

using namespace mjt::cli;

/// 无后端依赖的最小选项（默认 cfr；仅走预检/解析/暂存路径的用例用）。
DecompileCliOptions minimalOptions() { return {}; }

/// 有后端依赖的选项（probeBackend 命中后调用；JAR 走发现序结果）。
DecompileCliOptions backendOptions(mjt::DecompilerKind kind, const std::string& jar)
{
    DecompileCliOptions options;
    options.kind          = kind;
    options.backendName   = mjt::decompilerKindId(kind);
    options.decompilerJar = jar;
    return options;
}

/// 读文件全部文本（binary 模式）。
std::string readAll(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

// ── 敌意名预检（纯逻辑，无后端）──────────────────────────────────────

TEST(MjtCliDecompilePrecheckTest, NormalNamesAreSafe)
{
    EXPECT_TRUE(classifyEntryNameForDisk("fixture/Alpha.class").safe);
    EXPECT_TRUE(classifyEntryNameForDisk("META-INF/MANIFEST.MF").safe);
    EXPECT_TRUE(classifyEntryNameForDisk("a$b/C$d$1.class").safe);
    //段长按 UTF-16 单元计（含 ".class" 后缀）：249×a + ".class" = 255 单元，
    //恰在上限内。
    EXPECT_TRUE(classifyEntryNameForDisk(std::string(249, 'a') + ".class").safe);
    //中文名：UTF-8 3 字节/字，249 字 + ".class" = 255 单元，按单元数判定。
    {
        std::string cjk;
        for (int index = 0; index < 249; ++index) cjk += "\xe4\xb8\xad";
        EXPECT_TRUE(classifyEntryNameForDisk(cjk + ".class").safe);
    }
}

TEST(MjtCliDecompilePrecheckTest, OverlongSegmentIsRejected)
{
    const auto verdict = classifyEntryNameForDisk(std::string(256, 'a') + ".class");
    EXPECT_FALSE(verdict.safe);
    EXPECT_EQ("segment_too_long", verdict.reason);

    //#63 样本形态：21845 个空格 + ".class"。
    const auto hostile = classifyEntryNameForDisk(cli_fixture::hostileInternalName() + ".class");
    EXPECT_FALSE(hostile.safe);
    EXPECT_EQ("segment_too_long", hostile.reason);
}

TEST(MjtCliDecompilePrecheckTest, IllegalCharactersAreRejected)
{
    EXPECT_EQ("illegal_character", classifyEntryNameForDisk("a<b.class").reason);
    EXPECT_EQ("illegal_character", classifyEntryNameForDisk("a:b.class").reason);
    EXPECT_EQ("illegal_character", classifyEntryNameForDisk("a|b.class").reason);
    EXPECT_EQ("illegal_character", classifyEntryNameForDisk("a?b.class").reason);
    EXPECT_EQ("illegal_character",
              classifyEntryNameForDisk(std::string("a\x01b.class")).reason);
}

TEST(MjtCliDecompilePrecheckTest, TrailingDotOrSpaceAndRelativeSegmentsAreRejected)
{
    //资源条目以点/空格结尾：Win32 落盘剥离尾字符，文件无法以原名存在。
    EXPECT_EQ("trailing_dot_or_space", classifyEntryNameForDisk("weird/notes.").reason);
    EXPECT_EQ("trailing_dot_or_space", classifyEntryNameForDisk("weird/notes ").reason);
    EXPECT_EQ("parent_or_self", classifyEntryNameForDisk("fixture/../Alpha.class").reason);
    EXPECT_EQ("parent_or_self", classifyEntryNameForDisk("./Alpha.class").reason);
    EXPECT_EQ("empty_segment", classifyEntryNameForDisk("fixture//Alpha.class").reason);
}

TEST(MjtCliDecompilePrecheckTest, DirectoryEntriesAreExemptFromPrecheck)
{
    //jar 写出器的目录补全条目（'data/' 形）不是文件落盘对象，不判空段。
    EXPECT_TRUE(classifyEntryNameForDisk("data/").safe);
    EXPECT_TRUE(classifyEntryNameForDisk("fixture/").safe);
}

// ── 计数与选择器（无后端）────────────────────────────────────────────

TEST(MjtCliDecompilePrecheckTest, CountsClassEntries)
{
    const auto jar = cli_fixture::writeDecompileJar();
    auto counted = countJarClassEntries(jar.string());
    ASSERT_TRUE(counted.is_ok()) << counted.unwrap_err().message;
    EXPECT_EQ(2U, counted.unwrap());   // fixture/Alpha.class + fixture/Api.class
}

TEST(MjtCliDecompilePrecheckTest, MissingClassSelectorIsUsageError)
{
    const auto jar = cli_fixture::writeDecompileJar();
    auto report = decompileSingleClass(jar.string(), "fixture/Nope", "", minimalOptions());
    ASSERT_TRUE(report.is_err());
    EXPECT_EQ(DecompileError::Kind::ClassNotFound, report.unwrap_err().kind);
}

TEST(MjtCliDecompilePrecheckTest, ZipOutputWithCfrIsUsageError)
{
    const auto jar = cli_fixture::writeDecompileJar();
    auto report =
        decompileJarBatch(jar.string(), "out.zip", minimalOptions());
    ASSERT_TRUE(report.is_err());
    EXPECT_EQ(DecompileError::Kind::InvalidArgument, report.unwrap_err().kind);
}

TEST(MjtCliDecompilePrecheckTest, UnknownBackendNameIsUsageError)
{
    auto kind = parseDecompilerBackendName("jd-cli");
    ASSERT_TRUE(kind.is_err());
    EXPECT_EQ(DecompileError::Kind::UnknownBackend, kind.unwrap_err().kind);
    //大小写归一：CFR / Vineflower / jcdc（实验后端）均命中。
    EXPECT_TRUE(parseDecompilerBackendName("CFR").is_ok());
    EXPECT_TRUE(parseDecompilerBackendName("Vineflower").is_ok());
    EXPECT_TRUE(parseDecompilerBackendName("JCDC").is_ok());
    const auto jcdc = parseDecompilerBackendName("jcdc");
    ASSERT_TRUE(jcdc.is_ok());
    EXPECT_EQ(mjt::DecompilerKind::Jcdc, jcdc.unwrap());
}

// ── 单类模式（真后端，按发现序门控）──────────────────────────────────

/// 门控 helper：后端可用时填充选项并返回 true；不可用返回 false（调用方在
/// TEST 体里 GTEST_SKIP——gtest 的 skip 在子函数里只会记录结果，不会截断
/// TEST 体，后续断言仍会跑，windows CI 实证）。
bool requireBackend(mjt::DecompilerKind kind, DecompileCliOptions& options)
{
    const auto probe = cli_fixture::probeBackend(kind);
    if (!probe.available) return false;
    options = backendOptions(kind, probe.decompilerJar);
    return true;
}

TEST(MjtCliDecompileBackendTest, SingleClassWritesSourceAndHonorsOutputPath)
{
    DecompileCliOptions options;
    if (!requireBackend(mjt::DecompilerKind::Cfr, options)) GTEST_SKIP() << "decompiler backend not available";
    const auto jar    = cli_fixture::writeDecompileJar();
    const auto outPath = test::getTempOutPath("mjt_cli_decompile_alpha.java");
    std::error_code ec;
    std::filesystem::remove(outPath, ec);

    auto report = decompileSingleClass(jar.string(), "fixture/Alpha", outPath.string(), options);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    EXPECT_EQ("cfr", report.unwrap().backend);
    ASSERT_TRUE(std::filesystem::is_regular_file(outPath));
    EXPECT_NE(readAll(outPath).find("class Alpha"), std::string::npos);
}

TEST(MjtCliDecompileBackendTest, SingleClassToStdoutModeReturnsSource)
{
    DecompileCliOptions options;
    if (!requireBackend(mjt::DecompilerKind::Cfr, options)) GTEST_SKIP() << "decompiler backend not available";
    const auto jar = cli_fixture::writeDecompileJar();

    auto report = decompileSingleClass(jar.string(), "fixture/Api", "", options);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto value = std::move(report).unwrap();
    EXPECT_NE(value.source.find("interface Api"), std::string::npos);
    EXPECT_TRUE(value.decompilerJar.size() > 0);
}

/// #63 核心门：敌意名不毒化正常类（级联修复门，双后端）。敌意类本身：
/// CFR 输出名自截断后可落盘 → 可达；vineflower 以类名物化产物文件，超限
/// 名在其内部异常被吞 → rc=0 无产物 → 共享层 fail-loud（NOT_FOUND + 根因
/// 摘要），错误通道依旧指向 zip / jasm 替代路径。
TEST(MjtCliDecompileBackendTest, HostileNameDoesNotPoisonNormalClassesOnBothBackends)
{
    const auto jar = cli_fixture::writeHostileNameJar();
    const std::string hostileName = cli_fixture::hostileInternalName();
    for (const auto kind : {mjt::DecompilerKind::Cfr, mjt::DecompilerKind::Vineflower}) {
        DecompileCliOptions options;
        if (!requireBackend(kind, options)) GTEST_SKIP() << "decompiler backend not available";

        //同 jar 正常类不受敌意类级联毒化（#63 A 修复门）。
        auto normal = decompileSingleClass(jar.string(), "fixture/Alpha", "", options);
        ASSERT_TRUE(normal.is_ok())
            << mjt::decompilerKindId(kind) << ": " << normal.unwrap_err().message;
        EXPECT_NE(std::move(normal).unwrap().source.find("class Alpha"), std::string::npos);
    }
}

TEST(MjtCliDecompileBackendTest, HostileNameClassReachableViaCfrTruncation)
{
    DecompileCliOptions options;
    if (!requireBackend(mjt::DecompilerKind::Cfr, options)) GTEST_SKIP() << "decompiler backend not available";
    const auto jar = cli_fixture::writeHostileNameJar();
    auto hostile =
        decompileSingleClass(jar.string(), cli_fixture::hostileInternalName(), "", options);
    ASSERT_TRUE(hostile.is_ok()) << hostile.unwrap_err().message;
    EXPECT_FALSE(std::move(hostile).unwrap().source.empty());
}

TEST(MjtCliDecompileBackendTest, HostileNameClassOnVineflowerFailsLoud)
{
    DecompileCliOptions options;
    if (!requireBackend(mjt::DecompilerKind::Vineflower, options)) GTEST_SKIP() << "decompiler backend not available";
    const auto jar = cli_fixture::writeHostileNameJar();
    auto hostile =
        decompileSingleClass(jar.string(), cli_fixture::hostileInternalName(), "", options);
    ASSERT_TRUE(hostile.is_err());
    EXPECT_EQ(DecompileError::Kind::Runtime, hostile.unwrap_err().kind);
    //fail-loud：错误消息带后端根因摘要（分诊「输入触发后端内部异常」）。
    EXPECT_FALSE(hostile.unwrap_err().message.empty());
}

// ── 批量模式（真后端，按发现序门控）──────────────────────────────────

TEST(MjtCliDecompileBackendTest, BatchWritesSourcesAndSkipsHostileEntries)
{
    DecompileCliOptions options;
    if (!requireBackend(mjt::DecompilerKind::Vineflower, options)) GTEST_SKIP() << "decompiler backend not available";
    const auto jar = cli_fixture::writeHostileNameJar();
    const auto outDir = test::getTempOutPath("mjt_cli_decompile_batch_out.keep");
    std::error_code ec;
    std::filesystem::remove_all(outDir, ec);

    auto report = decompileJarBatch(jar.string(), outDir.string(), options);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto value = std::move(report).unwrap();
    EXPECT_EQ("vineflower", value.backend);
    //class 条目 = fixture/Alpha + 敌意名类；跳过 = 敌意名一条（目录条目不预检）。
    EXPECT_EQ(2U, value.classEntries);
    ASSERT_EQ(1U, value.skipped.size());
    EXPECT_EQ("segment_too_long", value.skipped[0].reason);
    //敌意类不落盘，正常类照常产出（#63 批量修复门）。
    EXPECT_TRUE(std::filesystem::is_regular_file(outDir / "fixture" / "Alpha.java"));
    EXPECT_FALSE(std::filesystem::exists(outDir / cli_fixture::hostileInternalName()));

    std::filesystem::remove_all(outDir, ec);
}

TEST(MjtCliDecompileBackendTest, BatchZipOutputWritesVineflowerArchive)
{
    DecompileCliOptions options;
    if (!requireBackend(mjt::DecompilerKind::Vineflower, options)) GTEST_SKIP() << "decompiler backend not available";
    const auto jar = cli_fixture::writeDecompileJar();
    const auto outZip = test::getTempOutPath("mjt_cli_decompile_batch_out.zip");
    std::error_code ec;
    std::filesystem::remove(outZip, ec);

    auto report = decompileJarBatch(jar.string(), outZip.string(), options);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto value = std::move(report).unwrap();
    EXPECT_TRUE(value.zipOutput);
    ASSERT_TRUE(std::filesystem::is_regular_file(outZip));
    //归档是合法 zip 且含 Alpha.java 条目（共享层 zip 输出契约）。
    ca::zip::ZipFile archive;
    ASSERT_FALSE(archive.open(outZip.u8string()).is_err());
    bool found = false;
    for (const auto& name : archive.entries()) {
        if (name.find("Alpha.java") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
    archive.close();
}

TEST(MjtCliDecompileBackendTest, MissingBackendJarFailsLoudWithProbedList)
{
    const auto jar = cli_fixture::writeDecompileJar();
    DecompileCliOptions options = backendOptions(mjt::DecompilerKind::Cfr,
                                                 test::getTempOutPath("no_such_decompiler.jar")
                                                     .string());
    auto report = decompileSingleClass(jar.string(), "fixture/Alpha", "", options);
    ASSERT_TRUE(report.is_err());
    EXPECT_EQ(DecompileError::Kind::Runtime, report.unwrap_err().kind);
    //fail-loud：错误文案带已探测候选清单（「没配」与「配了但文件不在」可分诊）。
    EXPECT_NE(report.unwrap_err().message.find("no_such_decompiler.jar"), std::string::npos);
}

TEST(MjtCliDecompileBackendTest, TimeoutKillsBackendAndFailsLoud)
{
    DecompileCliOptions options;
    if (!requireBackend(mjt::DecompilerKind::Cfr, options)) GTEST_SKIP() << "decompiler backend not available";
    options.timeoutMs = 1;   //毫秒级超时：子进程必然被击杀
    const auto jar = cli_fixture::writeDecompileJar();
    auto report = decompileSingleClass(jar.string(), "fixture/Alpha", "", options);
    ASSERT_TRUE(report.is_err());
    EXPECT_EQ(DecompileError::Kind::Runtime, report.unwrap_err().kind);
}

}   // namespace
