/// @file        cli_jar_fix_test.cpp
/// @brief       `mjt jar fix` 核心面测试：坏 class 条目 / 损坏输出标记的
///              诊断与修复（workspace analyzeJarRepair/applyJarRepair 接线）、
///              输出 jar 内容核验、同路径防护。
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "commands.hpp"
#include "shared/output_sink.hpp"
#include "cli_fixture.hpp"

namespace {

/// 报告内查找问题；命中返回该问题，否则返回 nullptr。
const mjt::cli::FixIssue* findIssue(const mjt::cli::FixReport& report, const std::string& uri,
                                    const std::string& kind)
{
    for (const auto& issue : report.issues) {
        if (issue.uri == uri && issue.kind == kind) return &issue;
    }
    return nullptr;
}

}   // namespace

TEST(MjtCliJarFixTest, RepairsBrokenEntriesAndWritesReport)
{
    const auto inJar  = cli_fixture::writeBrokenJar();
    const auto outJar = test::getTempOutPath("mjt_cli_fix_out.jar");
    std::error_code ec;
    std::filesystem::remove(outJar, ec);

    auto result = mjt::cli::fixJar(inJar.string(), outJar.string());
    ASSERT_TRUE(result.is_ok()) << (result.is_err() ? result.unwrap_err().message : "");
    const auto report = std::move(result).unwrap();

    // 扫描面：4 文件条目（mjbase#167 后 exportToJar 不再合成目录占位，
    // 导入容器无占位可数）；1 个合法 class（坏字节条目按形态归类披露
    // 在 issues）。
    EXPECT_EQ(report.scannedEntries, 4U);

    // 坏字节 .class 条目被移除（kind 为 invalid_class 或 invalid_class_entry，
    // 取决于容器把坏字节归成损坏 ClassResource 还是未知条目）。
    const auto* invalid =
        findIssue(report, "fixture/Broken.class", "invalid_class");
    const auto* invalidEntry =
        findIssue(report, "fixture/Broken.class", "invalid_class_entry");
    ASSERT_TRUE(invalid != nullptr || invalidEntry != nullptr);
    EXPECT_EQ((invalid != nullptr ? invalid->removed : invalidEntry->removed), true);

    // 损坏输出标记被移除。
    const auto* marker = findIssue(report, "corrupt_output_stream.dat", "corrupt_output_marker");
    ASSERT_NE(marker, nullptr);
    EXPECT_TRUE(marker->removed);

    // 合法 class 不在问题清单；常规资源不在问题清单。
    EXPECT_EQ(findIssue(report, "fixture/Alpha.class", "invalid_class"), nullptr);
    EXPECT_EQ(findIssue(report, "data/notes.txt", "corrupt_output_marker"), nullptr);

    // 输出 jar：坏条目消失，合法 class 与常规资源保留。
    ASSERT_TRUE(std::filesystem::is_regular_file(outJar));
    const auto uris = cli_fixture::jarEntryUris(outJar);
    EXPECT_NE(std::find(uris.begin(), uris.end(), "fixture/Alpha.class"), uris.end());
    EXPECT_NE(std::find(uris.begin(), uris.end(), "data/notes.txt"), uris.end());
    EXPECT_EQ(std::find(uris.begin(), uris.end(), "fixture/Broken.class"), uris.end());
    EXPECT_EQ(std::find(uris.begin(), uris.end(), "corrupt_output_stream.dat"), uris.end());
}

TEST(MjtCliJarFixTest, CleanJarProducesEmptyReportAndCopy)
{
    const auto inJar  = cli_fixture::writeInfoJar();
    const auto outJar = test::getTempOutPath("mjt_cli_fix_clean.jar");
    std::error_code ec;
    std::filesystem::remove(outJar, ec);

    auto result = mjt::cli::fixJar(inJar.string(), outJar.string());
    ASSERT_TRUE(result.is_ok()) << (result.is_err() ? result.unwrap_err().message : "");
    const auto report = std::move(result).unwrap();
    EXPECT_TRUE(report.issues.empty());
    ASSERT_TRUE(std::filesystem::is_regular_file(outJar));
    // 干净 jar 的修复副本与输入逐条目同构（3 class + 2 资源；mjbase#167
    // 后无目录占位，双斜杠资源写出即折叠）。
    const auto uris = cli_fixture::jarEntryUris(outJar);
    EXPECT_EQ(uris.size(), 5U);
}

TEST(MjtCliJarFixTest, RejectsSameInputOutputPath)
{
    const auto inJar = cli_fixture::writeBrokenJar();
    auto       result = mjt::cli::fixJar(inJar.string(), inJar.string());
    ASSERT_TRUE(result.is_err());
    // 防自毁：输入原文件仍在。
    EXPECT_TRUE(std::filesystem::is_regular_file(inJar));
}

TEST(MjtCliJarFixTest, KeepsExistingOutputWhenInputFailsToOpen)
{
    // 非 jar 输入（文本冒充 .jar）：openJar 失败，且上一份有效输出必须
    // 原样保留（删旧输出已推迟到导出前一刻）。
    const auto inJar  = cli_fixture::writeTextFile("mjt_cli_fix_bad_input.jar", "not a jar");
    const auto outJar = test::getTempOutPath("mjt_cli_fix_previous.jar");
    {
        std::ofstream out(outJar, std::ios::binary);
        out << "previous good output";
    }
    auto result = mjt::cli::fixJar(inJar.string(), outJar.string());
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(cli_fixture::readFileText(outJar), "previous good output");
}

TEST(MjtCliJarFixTest, RendersReportText)
{
    mjt::cli::FixReport report;
    report.scannedEntries = 4;
    report.scannedClasses = 1;
    report.issues.push_back({"fixture/Broken.class", "invalid_class",
                             "class bytes did not pass Jasm validation", true});
    report.issues.push_back({"data/notes.txt", "corrupt_output_marker", "kept marker", false});

    // tmpfile 捕获渲染输出（同 cli_info_test 的 CapturedSink 思路，就地展开）。
    std::FILE* file = std::tmpfile();
    ASSERT_NE(file, nullptr);
    mjt::OutputSink sink(file);
    mjt::cli::renderFixReport(report, "fixed.jar", sink);
    std::fflush(file);
    std::rewind(file);
    std::string text;
    char        buffer[4096];
    std::size_t read = 0;
    while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
    std::fclose(file);

    EXPECT_NE(text.find("scanned 4 entries (1 classes)"), std::string::npos);
    EXPECT_NE(text.find("issues found: 2"), std::string::npos);
    EXPECT_NE(text.find("removed: fixture/Broken.class [invalid_class]"), std::string::npos);
    EXPECT_NE(text.find("kept: data/notes.txt [corrupt_output_marker]"), std::string::npos);
    EXPECT_NE(text.find("repaired jar written to: fixed.jar"), std::string::npos);
}
