/// @file        cli_info_test.cpp
/// @brief       `mjt info` 核心面测试：census 普查（条目/类/class_kind/
///              entry_shape）+ 字节码版本 + manifest 有无 + 文本/JSON 渲染。
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "commands.hpp"
#include "shared/output_sink.hpp"
#include "cli_fixture.hpp"

namespace {

using mjt::cli::collectInfo;

/// 名字→计数分布查找；未命中返回 false。
bool findCount(const std::vector<std::pair<std::string, ca::i64>>& distribution,
               const std::string& name, ca::i64 expected)
{
    for (const auto& [key, count] : distribution) {
        if (key == name) return count == expected;
    }
    return false;
}

using CapturedSink = cli_fixture::CapturedSink;

}   // namespace

TEST(MjtCliInfoTest, ReportsCensusCountsKindsShapesVersionsManifest)
{
    const auto jar     = cli_fixture::writeInfoJar();
    auto       report  = collectInfo(jar.string());
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;

    const auto info = std::move(report).unwrap();
    // 5 条目 = 3 class + 2 资源。mjbase#167 后 exportToJar 不再合成目录
    // 占位条目，双斜杠路径写出即折叠（bad//name.txt → bad/name.txt），
    // 产物全常规形态（空段/尾斜杠语料覆盖改由 writeRawZip 直写面承担）。
    EXPECT_EQ(info.entries, 5);
    EXPECT_EQ(info.classes, 3);
    EXPECT_FALSE(info.hasManifest);

    // class_kind：三类形态各 1（class/interface/enum），名字升序。
    ASSERT_EQ(info.classKinds.size(), 3U);
    EXPECT_EQ(info.classKinds[0].first, "class");
    EXPECT_EQ(info.classKinds[0].second, 1);
    EXPECT_EQ(info.classKinds[1].first, "enum");
    EXPECT_EQ(info.classKinds[2].first, "interface");

    // entry_shape：mjbase#167 后产物全常规——单桶 regular==5；投影只含
    // 非零 facet（censusContainer 从真实条目聚合，零计数桶不进分布），
    // 桶序按 facet 枚举声明序。
    ASSERT_EQ(info.entryShapes.size(), 1U);
    EXPECT_EQ(info.entryShapes[0].first, "regular");
    EXPECT_EQ(info.entryShapes[0].second, 5);
    EXPECT_TRUE(findCount(info.entryShapes, "regular", 5));

    // 字节码版本：jasm 缺省 major 61（java 17），单桶 3 类。
    ASSERT_EQ(info.versions.size(), 1U);
    EXPECT_EQ(info.versions[0].first, 61);
    EXPECT_EQ(info.versions[0].second, 3);
}

TEST(MjtCliInfoTest, ReportsMemberCensusNativeAndOrigins)
{
    const auto jar    = cli_fixture::writeInfoJar();
    auto       report = collectInfo(jar.string());
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    // 成员普查（fixture 逐类核对：Alpha private 字段 1 + public static 方法 1，
    // Api public abstract 方法 1，Color public static 方法 1——jasm 只汇编
    // 源内声明的成员，无隐式补齐）。
    EXPECT_EQ(info.methods, 3);
    EXPECT_EQ(info.fields, 1);
    EXPECT_EQ(info.nativeMethods, 0);
    ASSERT_EQ(info.methodVisibility.size(), 1U);
    EXPECT_EQ(info.methodVisibility[0].first, "public");
    EXPECT_EQ(info.methodVisibility[0].second, 3);
    ASSERT_EQ(info.fieldVisibility.size(), 1U);
    EXPECT_EQ(info.fieldVisibility[0].first, "private");
    EXPECT_EQ(info.fieldVisibility[0].second, 1);

    // 来源分布：fixture/* 前缀不命中任何官方/第三方根 → unknown 单桶。
    ASSERT_EQ(info.origins.size(), 1U);
    EXPECT_EQ(info.origins[0].first, "unknown");
    EXPECT_EQ(info.origins[0].second, 3);
}

TEST(MjtCliInfoTest, MemberCensusTotalsMatchVisibilityBuckets)
{
    // 对账不变式：可见性桶计数之和 = 各自成员总数（投影两个独立查询的交叉
    // 验证；censusMembers 行数 = Σ method_count + field_count 同款纪律）。
    const auto jar    = cli_fixture::writeDecompileJar();
    auto       report = collectInfo(jar.string());
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    ca::i64 methodSum = 0;
    for (const auto& [bucket, count] : info.methodVisibility) methodSum += count;
    EXPECT_EQ(methodSum, info.methods);
    ca::i64 fieldSum = 0;
    for (const auto& [bucket, count] : info.fieldVisibility) fieldSum += count;
    EXPECT_EQ(fieldSum, info.fields);
}

TEST(MjtCliInfoTest, ReportsManifestPresence)
{
    const auto jar    = cli_fixture::writeInfoJarWithManifest();
    auto       report = collectInfo(jar.string());
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();
    EXPECT_TRUE(info.hasManifest);
    EXPECT_EQ(info.classes, 1);
}

TEST(MjtCliInfoTest, FailsLoudOnMissingJar)
{
    const auto report = collectInfo("mjt_cli_info_no_such.jar");
    ASSERT_TRUE(report.is_err());
}

TEST(MjtCliInfoTest, RendersDeterministicTextAndJson)
{
    const auto jar    = cli_fixture::writeInfoJar();
    auto       report = collectInfo(jar.string());
    ASSERT_TRUE(report.is_ok());
    const auto info = std::move(report).unwrap();

    CapturedSink textCapture;
    mjt::cli::renderInfoText(info, textCapture.sink());
    const auto text = textCapture.text();
    EXPECT_NE(text.find("jar overview: " + jar.string()), std::string::npos);
    EXPECT_NE(text.find("entries: 5"), std::string::npos);
    EXPECT_NE(text.find("classes: 3"), std::string::npos);
    EXPECT_NE(text.find("members: methods 3, fields 1, native 0"), std::string::npos);
    EXPECT_NE(text.find("class kinds:"), std::string::npos);
    EXPECT_NE(text.find("  interface: 1"), std::string::npos);
    EXPECT_NE(text.find("method visibility:"), std::string::npos);
    EXPECT_NE(text.find("  public: 3"), std::string::npos);
    EXPECT_NE(text.find("field visibility:"), std::string::npos);
    EXPECT_NE(text.find("  private: 1"), std::string::npos);
    EXPECT_NE(text.find("class origins:"), std::string::npos);
    EXPECT_NE(text.find("  unknown: 3"), std::string::npos);
    EXPECT_NE(text.find("entry path shapes:"), std::string::npos);
    // 渲染面只打非零桶：分布仅剩 regular，空段/尾斜杠行整体消失。
    EXPECT_NE(text.find("  regular: 5"), std::string::npos);
    EXPECT_NE(text.find("bytecode versions:"), std::string::npos);
    EXPECT_NE(text.find("61 (java 17): 3"), std::string::npos);
    EXPECT_NE(text.find("manifest: absent"), std::string::npos);

    CapturedSink jsonCapture;
    mjt::cli::renderInfoJson(info, jsonCapture.sink());
    const auto json = jsonCapture.text();
    EXPECT_NE(json.find("\"entries\": 5"), std::string::npos);
    EXPECT_NE(json.find("\"classes\": 3"), std::string::npos);
    EXPECT_NE(json.find("\"methods\": 3"), std::string::npos);
    EXPECT_NE(json.find("\"fields\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"native_methods\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"interface\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"method_visibility\": {\"public\": 3}"), std::string::npos);
    EXPECT_NE(json.find("\"field_visibility\": {\"private\": 1}"), std::string::npos);
    EXPECT_NE(json.find("\"origins\": {\"unknown\": 3}"), std::string::npos);
    EXPECT_NE(json.find("\"entry_shapes\": {\"regular\": 5}"), std::string::npos);
    EXPECT_NE(json.find("\"61\": 3"), std::string::npos);
    EXPECT_NE(json.find("\"has_manifest\": false"), std::string::npos);

    // 渲染纯函数：同数据两次渲染逐字节一致。
    CapturedSink again;
    mjt::cli::renderInfoJson(info, again.sink());
    EXPECT_EQ(json, again.text());
}
