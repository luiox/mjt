/// @file        cli_smoke_test.cpp
/// @brief       CLI 冒烟：运行时生成语料 → 运行 `mjt` exe → 断言退出码与
///              输出（0 成功 / 1 用法错误 / 2 运行失败约定）。
///
/// exe 定位：test_helper 以本测试 exe 所在目录为活动 builddir 目标层，`mjt`
/// 为同层伴生产物（unittest target 经 add_deps 保证构建序）。
///
/// 平台注记：本文件经 std::system 拼 cmd /c 重定向语法（首尾引号剥离防护），
/// 是 Windows 命令面专有形态；Linux 无 cmd，整文件限 _WIN32 编译（CLI 命令
/// 核心面由其余 *_test.cpp 全平台覆盖）。
#ifdef _WIN32
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
    EXPECT_NE(text.output.find("entries: 5"), std::string::npos);   ///< mjbase#167 零目录占位
    EXPECT_NE(text.output.find("classes: 3"), std::string::npos);
    EXPECT_NE(text.output.find("manifest: absent"), std::string::npos);

    const auto json = runCli("info -i \"" + jar.string() + "\" --json");
    EXPECT_EQ(json.exitCode, 0);
    EXPECT_NE(json.output.find("\"entries\": 5"), std::string::npos);
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

TEST(MjtCliSmokeTest, MappingSamePathIsUsageError)
{
    const auto srgPath = cli_fixture::writeTextFile("mjt_cli_smoke_same.srg",
                                                    cli_fixture::srgMappingText());
    const auto run =
        runCli("mapping -i \"" + srgPath.string() + "\" -o \"" + srgPath.string() +
               "\" --from srg --to tiny-v2");
    EXPECT_EQ(run.exitCode, 1);
    // 防自毁：输入 mapping 原文仍在。
    EXPECT_NE(cli_fixture::readFileText(srgPath).find("CL: fixture/Alpha"), std::string::npos);
}

TEST(MjtCliSmokeTest, DecompileHelpListsCommand)
{
    const auto run = runCli("--help");
    EXPECT_EQ(run.exitCode, 0);
    EXPECT_NE(run.output.find("decompile"), std::string::npos);
}

TEST(MjtCliSmokeTest, DecompileWithoutModeIsUsageError)
{
    const auto jar = cli_fixture::writeInfoJar();
    const auto run = runCli("decompile -i \"" + jar.string() + "\"");
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.output.find("-c"), std::string::npos);
}

TEST(MjtCliSmokeTest, DecompileBatchWithoutAllIsUsageErrorWithClassCount)
{
    const auto jar = cli_fixture::writeInfoJar();
    const auto run = runCli("decompile -i \"" + jar.string() + "\" -o \"decompile_out\"");
    EXPECT_EQ(run.exitCode, 1);
    //提示带类数（info 语料 3 个 class）并点名 --all。
    EXPECT_NE(run.output.find("--all"), std::string::npos);
    EXPECT_NE(run.output.find("3"), std::string::npos);
}

TEST(MjtCliSmokeTest, DecompileSingleClassOutputMustBeJavaFile)
{
    const auto jar = cli_fixture::writeInfoJar();
    const auto run =
        runCli("decompile -i \"" + jar.string() + "\" -c fixture/Alpha -o alpha.txt");
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.output.find(".java"), std::string::npos);
}

TEST(MjtCliSmokeTest, DecompileUnknownBackendIsUsageError)
{
    const auto jar = cli_fixture::writeInfoJar();
    const auto run =
        runCli("decompile -i \"" + jar.string() + "\" -c fixture/Alpha --decompiler bogus");
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.output.find("bogus"), std::string::npos);
}

TEST(MjtCliSmokeTest, DecompileNonPositiveLimitsAreUsageErrors)
{
    const auto jar = cli_fixture::writeInfoJar();
    EXPECT_EQ(runCli("decompile -i \"" + jar.string() + "\" -c fixture/Alpha --timeout-ms 0")
                  .exitCode,
              1);
    EXPECT_EQ(runCli("decompile -i \"" + jar.string() + "\" -c fixture/Alpha --max-memory-mib abc")
                  .exitCode,
              1);
}

TEST(MjtCliSmokeTest, DecompileOverflowMemoryValueIsUsageError)
{
    const auto jar = cli_fixture::writeInfoJar();
    // i64 解析成功但超共享层 i32 字段上界：按用法错误拒绝（而非溢出回绕后
    // 落运行失败 2）。
    const auto run = runCli("decompile -i \"" + jar.string() +
                            "\" -c fixture/Alpha --max-memory-mib 99999999999");
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.output.find("99999999999"), std::string::npos);
}

TEST(MjtCliSmokeTest, DecompileZipOutputWithCfrIsUsageError)
{
    const auto jar = cli_fixture::writeInfoJar();
    const auto outZip = test::getTempOutPath("mjt_cli_smoke_zipout.zip");
    std::error_code ec;
    std::filesystem::remove(outZip, ec);
    const auto run = runCli("decompile -i \"" + jar.string() + "\" -o \"" + outZip.string() +
                            "\" --all --decompiler cfr");
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.output.find("zip"), std::string::npos);
    EXPECT_FALSE(std::filesystem::is_regular_file(outZip));
}

TEST(MjtCliSmokeTest, HelpListsReadFamilyCommands)
{
    const auto run = runCli("--help");
    EXPECT_EQ(run.exitCode, 0);
    EXPECT_NE(run.output.find("classes"), std::string::npos);
    EXPECT_NE(run.output.find("disasm"), std::string::npos);
    EXPECT_NE(run.output.find("subs"), std::string::npos);
}

TEST(MjtCliSmokeTest, ClassesCommandListsKindsAndJson)
{
    const auto jar = cli_fixture::writeClassesJar();

    const auto text = runCli("classes -i \"" + jar.string() + "\"");
    EXPECT_EQ(text.exitCode, 0);
    EXPECT_NE(text.output.find("matched: 9 of 9 jar classes"), std::string::npos);
    EXPECT_NE(text.output.find("fixture/Alpha class unknown"), std::string::npos);
    EXPECT_NE(text.output.find("sun/misc/Stub class jdk"), std::string::npos);

    const auto json = runCli("classes -i \"" + jar.string() + "\" --origin known --json");
    EXPECT_EQ(json.exitCode, 0);
    EXPECT_NE(json.output.find("\"origin\": \"known\""), std::string::npos);
    EXPECT_NE(json.output.find("\"total\": 1"), std::string::npos);
}

TEST(MjtCliSmokeTest, ClassesInvalidKindIsUsageError)
{
    const auto jar = cli_fixture::writeClassesJar();
    const auto run = runCli("classes -i \"" + jar.string() + "\" --kind struct");
    EXPECT_EQ(run.exitCode, 1);
    EXPECT_NE(run.output.find("struct"), std::string::npos);
}

TEST(MjtCliSmokeTest, ClassesOutputFileBypassesLimitSanityCap)
{
    const auto jar = cli_fixture::writeClassesJar();

    // stdout 路径：--limit 2 截断并提示放大。
    const auto stdoutRun = runCli("classes -i \"" + jar.string() + "\" --limit 2");
    EXPECT_EQ(stdoutRun.exitCode, 0);
    EXPECT_NE(stdoutRun.output.find("matched: 9 of 9 jar classes; showing 2"), std::string::npos);
    EXPECT_NE(stdoutRun.output.find("truncated"), std::string::npos);

    // -o 路径：防呆上限不作用，完整清单落盘（无截断提示）。
    const auto outFile = test::getTempOutPath("mjt_cli_smoke_classes_full.txt");
    std::error_code ec;
    std::filesystem::remove(outFile, ec);
    const auto run =
        runCli("classes -i \"" + jar.string() + "\" --limit 2 -o \"" + outFile.string() + "\"");
    EXPECT_EQ(run.exitCode, 0);
    EXPECT_NE(run.output.find("list written to"), std::string::npos);
    const auto text = cli_fixture::readFileText(outFile);
    EXPECT_NE(text.find("matched: 9 of 9 jar classes; showing 9"), std::string::npos);
    EXPECT_EQ(text.find("truncated"), std::string::npos);
    // 库序末行在列（internal_name BINARY 升序：sun/misc/Stub 收尾）。
    EXPECT_NE(text.find("sun/misc/Stub class jdk"), std::string::npos);

    // JSON 同形落盘：limit 归一为清单行数（无截断机读口径）。
    const auto jsonOut = test::getTempOutPath("mjt_cli_smoke_classes_full.json");
    std::filesystem::remove(jsonOut, ec);
    const auto jsonRun = runCli("classes -i \"" + jar.string() + "\" --limit 2 --json -o \"" +
                                jsonOut.string() + "\"");
    EXPECT_EQ(jsonRun.exitCode, 0);
    const auto json = cli_fixture::readFileText(jsonOut);
    EXPECT_NE(json.find("\"limit\": 9"), std::string::npos);
    EXPECT_NE(json.find("\"truncated\": false"), std::string::npos);
}

TEST(MjtCliSmokeTest, DisasmCommandPrintsJasmAndReportsMissingClass)
{
    const auto jar = cli_fixture::writeClassesJar();

    const auto text = runCli("disasm -i \"" + jar.string() + "\" -c fixture/Alpha");
    EXPECT_EQ(text.exitCode, 0);
    EXPECT_NE(text.output.find("fixture/Alpha"), std::string::npos);
    EXPECT_NE(text.output.find("ireturn"), std::string::npos);

    const auto missing = runCli("disasm -i \"" + jar.string() + "\" -c fixture/NoSuch");
    EXPECT_EQ(missing.exitCode, 1);
    EXPECT_NE(missing.output.find("fixture/NoSuch"), std::string::npos);
}

TEST(MjtCliSmokeTest, SubsCommandListsMembersAndReportsMissingClass)
{
    const auto jar = cli_fixture::writeClassesJar();

    const auto text = runCli("subs -i \"" + jar.string() + "\" -c fixture/Native");
    EXPECT_EQ(text.exitCode, 0);
    EXPECT_NE(text.output.find("fields: 1, methods: 2"), std::string::npos);
    EXPECT_NE(text.output.find("method probe (I)V access=257 native"), std::string::npos);

    const auto json = runCli("subs -i \"" + jar.string() + "\" -c fixture/Native --json");
    EXPECT_EQ(json.exitCode, 0);
    EXPECT_NE(json.output.find("\"native\": true"), std::string::npos);

    const auto missing = runCli("subs -i \"" + jar.string() + "\" -c fixture/NoSuch");
    EXPECT_EQ(missing.exitCode, 1);
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

#endif   // _WIN32
