/// @file        cli_smoke_test.cpp
/// @brief       CLI 冒烟：运行时生成语料 → 运行 `mjt` exe → 断言退出码与
///              输出（0 成功 / 1 用法错误 / 2 运行失败约定）。
///
/// exe 定位：test_helper 以本测试 exe 所在目录为活动 builddir 目标层，`mjt`
/// 为同层伴生产物（unittest target 经 add_deps 保证构建序）。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "cli_fixture.hpp"
#include "test_helper.hpp"

namespace {

struct SmokeRun
{
    int         exitCode = -1;
    std::string output;
};

/// 运行 `mjt` exe（参数原样拼接），stdout/stderr 重定向捕获。
SmokeRun runCli(const std::string& args)
{
    const auto exe = test::getBuildTargetPath("mjt");
    if (exe.empty() || !std::filesystem::is_regular_file(exe)) {
        ADD_FAILURE() << "mjt exe not found next to the unittest binary";
        return {};
    }
    const auto outFile = test::getTempOutPath("mjt_cli_smoke_capture.txt");
    std::error_code ec;
    std::filesystem::remove(outFile, ec);
    // cmd /c 对首尾引号有剥离规则：整条命令再包一层外引号（经典防护），
    // 剥外层后剩下的就是带引号 exe + 参数 + 重定向的原形。
    const std::string command = "\"\"" + exe.string() + "\" " + args + " > \"" +
                                outFile.string() + "\" 2>&1\"";
    const int rc = std::system(command.c_str());
    SmokeRun run;
    run.exitCode = rc;
    run.output   = cli_fixture::readFileText(outFile);
    return run;
}

}   // namespace

TEST(MjtCliSmokeTest, VersionFlagPrintsVersionAndExitsZero)
{
    const auto run = runCli("--version");
    EXPECT_EQ(run.exitCode, 0);
    EXPECT_NE(run.output.find("mjt " MJT_VERSION), std::string::npos);
}

TEST(MjtCliSmokeTest, HelpExitsZeroAndListsCommands)
{
    const auto run = runCli("--help");
    EXPECT_EQ(run.exitCode, 0);
    EXPECT_NE(run.output.find("info"), std::string::npos);
    EXPECT_NE(run.output.find("mapping"), std::string::npos);
    EXPECT_NE(run.output.find("fix"), std::string::npos);
}

TEST(MjtCliSmokeTest, NoCommandIsUsageError)
{
    const auto run = runCli("");
    EXPECT_EQ(run.exitCode, 1);
}

TEST(MjtCliSmokeTest, MissingInputIsUsageError)
{
    const auto run = runCli("info -i mjt_cli_smoke_missing.jar");
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.output.find("mjt_cli_smoke_missing.jar"), std::string::npos);
}

TEST(MjtCliSmokeTest, UnknownFormatIsUsageError)
{
    const auto jar = cli_fixture::writeInfoJar();
    const auto run = runCli("mapping -i \"" + jar.string() + "\" -o out.txt --from srg --to bogus");
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.output.find("bogus"), std::string::npos);
}

TEST(MjtCliSmokeTest, InfoCommandReportsOverviewAndJson)
{
    const auto jar = cli_fixture::writeInfoJar();

    const auto text = runCli("info -i \"" + jar.string() + "\"");
    EXPECT_EQ(text.exitCode, 0);
    EXPECT_NE(text.output.find("entries: 9"), std::string::npos);
    EXPECT_NE(text.output.find("classes: 3"), std::string::npos);
    EXPECT_NE(text.output.find("manifest: absent"), std::string::npos);

    const auto json = runCli("info -i \"" + jar.string() + "\" --json");
    EXPECT_EQ(json.exitCode, 0);
    EXPECT_NE(json.output.find("\"entries\": 9"), std::string::npos);
    EXPECT_NE(json.output.find("\"has_manifest\": false"), std::string::npos);
}

TEST(MjtCliSmokeTest, MappingCommandConvertsThreeFamilies)
{
    const auto srgPath  = cli_fixture::writeTextFile("mjt_cli_smoke.srg", cli_fixture::srgMappingText());
    const auto tinyPath = test::getTempOutPath("mjt_cli_smoke_out.tiny");

    const auto run =
        runCli("mapping -i \"" + srgPath.string() + "\" -o \"" + tinyPath.string() +
               "\" --from srg --to tiny-v2");
    EXPECT_EQ(run.exitCode, 0);
    ASSERT_TRUE(std::filesystem::is_regular_file(tinyPath));
    EXPECT_NE(cli_fixture::readFileText(tinyPath).find("tiny\t2\t0"), std::string::npos);
}

TEST(MjtCliSmokeTest, JarFixCommandRepairsAndReports)
{
    const auto inJar  = cli_fixture::writeBrokenJar();
    const auto outJar = test::getTempOutPath("mjt_cli_smoke_fixed.jar");
    std::error_code ec;
    std::filesystem::remove(outJar, ec);

    const auto run = runCli("jar fix -i \"" + inJar.string() + "\" -o \"" + outJar.string() + "\"");
    EXPECT_EQ(run.exitCode, 0);
    EXPECT_NE(run.output.find("removed: fixture/Broken.class"), std::string::npos);
    EXPECT_NE(run.output.find("corrupt_output_stream.dat"), std::string::npos);
    EXPECT_NE(run.output.find("repaired jar written to"), std::string::npos);
    ASSERT_TRUE(std::filesystem::is_regular_file(outJar));
    const auto uris = cli_fixture::jarEntryUris(outJar);
    EXPECT_EQ(std::find(uris.begin(), uris.end(), "fixture/Broken.class"), uris.end());
}
