/// @file        cli_strings_test.cpp
/// @brief       `mjt strings` 核心面测试：抽取计数 / kind 计数 / --min-len 过滤
///              / 单类过滤 / 错误分支 / 文本与 JSON 渲染确定性。
#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "commands.hpp"
#include "shared/json_util.hpp"
#include "shared/output_sink.hpp"
#include "cli_fixture.hpp"

namespace {

using mjt::cli::collectStrings;
using mjt::cli::StringsError;

/// 值查找：命中返回该行，未命中返回 nullptr。
const mjt::cli::StringsValue* findValue(const mjt::cli::StringsReport& report,
                                        const std::string& value)
{
    for (const auto& item : report.values) {
        if (item.value == value) return &item;
    }
    return nullptr;
}

using CapturedSink = cli_fixture::CapturedSink;

}   // namespace

TEST(MjtCliStringsTest, ExtractsDeduplicatedValuesWithKindCounts)
{
    const auto jar    = cli_fixture::writeStringsJar();
    auto       report = collectStrings(jar.string(), "", 1);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    EXPECT_EQ(info.classesScanned, 2);
    EXPECT_EQ(info.classesSkipped, 0);
    EXPECT_EQ(info.uniqueValues, static_cast<ca::i64>(info.values.size()));

    // 计数契约：字面量 = CONSTANT_String + 背书 Utf8 两类条目；同类内池唯一，
    // 跨类累加。"hello world" 在 Words/Echo 各产 2 条目 → count 4。
    const auto* reused = findValue(info, "hello world");
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->count, 4);
    EXPECT_EQ(reused->string, 2);
    EXPECT_EQ(reused->utf8, 2);
    EXPECT_EQ(reused->cls, 0);

    const auto* unique = findValue(info, "zeta_payload_xyz");
    ASSERT_NE(unique, nullptr);
    EXPECT_EQ(unique->count, 2);
    EXPECT_EQ(unique->string, 1);
    EXPECT_EQ(unique->utf8, 1);

    // 对账不变式：occurrences = Σ count。
    ca::i64 sum = 0;
    for (const auto& item : info.values) sum += item.count;
    EXPECT_EQ(sum, info.occurrences);

    // 确定序：count 降序，count 相同按 value 升序。
    for (std::size_t index = 1; index < info.values.size(); ++index) {
        const auto& previous = info.values[index - 1];
        const auto& current  = info.values[index];
        EXPECT_TRUE(previous.count > current.count ||
                    (previous.count == current.count && previous.value < current.value))
            << "values not in (count desc, value asc) order at index " << index;
    }
}

TEST(MjtCliStringsTest, MinLenFiltersShortValuesByByteLength)
{
    const auto jar    = cli_fixture::writeStringsJar();
    auto       report = collectStrings(jar.string(), "", 12);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    EXPECT_EQ(findValue(info, "hello world"), nullptr);          // 11 字节
    EXPECT_NE(findValue(info, "zeta_payload_xyz"), nullptr);     // 16 字节
    EXPECT_NE(findValue(info, "ENCRYPTED_BLOB_0123456789abcdef"), nullptr); // 31 字节
}

TEST(MjtCliStringsTest, CountsDexTableEntriesOncePerUnit)
{
    const auto resources = test::getTopProjectPath() / "test_resource" / "dex";
    const auto dex = resources / "hello_refs.dex";
    auto whole = collectStrings(dex.string(), "", 1);
    ASSERT_TRUE(whole.is_ok()) << whole.unwrap_err().message;
    const auto dexInfo = std::move(whole).unwrap();
    EXPECT_EQ(dexInfo.classesScanned, 3);
    const auto* sharedType = findValue(dexInfo, "Ljava/lang/Object;");
    ASSERT_NE(sharedType, nullptr);
    EXPECT_EQ(sharedType->count, 1);
    EXPECT_EQ(sharedType->utf8, 1);
    ASSERT_NE(findValue(dexInfo, "Subject.java"), nullptr);

    const auto read = [&](const char* filename) {
        std::ifstream input(resources / filename, std::ios::binary);
        return std::vector<ca::u8>(std::istreambuf_iterator<char>(input),
                                   std::istreambuf_iterator<char>());
    };
    const auto apk = test::getTempOutPath("mjt_cli_strings_multidex.apk");
    cli_fixture::writeRawZip(apk, {{"classes.dex", read("hello_refs.dex")},
                                   {"classes2.dex", read("hello_extra.dex")}});
    auto apkReport = collectStrings(apk.string(), "", 1);
    ASSERT_TRUE(apkReport.is_ok()) << apkReport.unwrap_err().message;
    const auto apkInfo = std::move(apkReport).unwrap();
    EXPECT_EQ(apkInfo.classesScanned, 4);
    const auto* acrossUnits = findValue(apkInfo, "Ljava/lang/Object;");
    ASSERT_NE(acrossUnits, nullptr);
    EXPECT_EQ(acrossUnits->count, 2);
    EXPECT_EQ(acrossUnits->utf8, 2);
}

TEST(MjtCliStringsTest, ExtractsStandaloneClassWithoutJarAccounting)
{
    const auto path = test::getTempOutPath("mjt_cli_strings_standalone.class");
    {
        const auto bytes = cli_fixture::assembleWordsClass();
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output);
    }
    auto report = collectStrings(path.string(), "", 1);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();
    EXPECT_EQ(info.classesScanned, 1);
    EXPECT_EQ(info.classesSkipped, 0);
    EXPECT_NE(findValue(info, "hello world"), nullptr);
}

TEST(MjtCliStringsTest, FiltersDexStringsByOwningClass)
{
    const auto dex = test::getTopProjectPath() / "test_resource" / "dex" / "hello_refs.dex";
    auto selected = collectStrings(dex.string(), "com/mjt/dexfix/Subject", 10);
    ASSERT_TRUE(selected.is_ok()) << selected.unwrap_err().message;
    const auto info = std::move(selected).unwrap();
    EXPECT_EQ(info.classesScanned, 1);
    const auto* literal = findValue(info, "hook-fired");
    ASSERT_NE(literal, nullptr);
    EXPECT_EQ(literal->count, 1);
    EXPECT_EQ(literal->string, 1);
    EXPECT_EQ(findValue(info, "Subject.java"), nullptr);
    EXPECT_EQ(findValue(info, "sCache"), nullptr);

    auto missing = collectStrings(dex.string(), "fixture/Missing", 1);
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(std::move(missing).unwrap_err().kind, StringsError::Kind::ClassNotFound);
}

TEST(MjtCliStringsTest, ClassFilterRestrictsScan)
{
    const auto jar    = cli_fixture::writeStringsJar();
    auto       report = collectStrings(jar.string(), "fixture/Words", 1);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    EXPECT_EQ(info.classesScanned, 1);
    EXPECT_EQ(info.classFilter, "fixture/Words");
    EXPECT_NE(findValue(info, "zeta_payload_xyz"), nullptr);
    EXPECT_EQ(findValue(info, "ENCRYPTED_BLOB_0123456789abcdef"), nullptr);
    const auto* reused = findValue(info, "hello world");
    ASSERT_NE(reused, nullptr);
    EXPECT_EQ(reused->count, 2);   // 仅 Words 一类：String + Utf8
}

TEST(MjtCliStringsTest, SkipsMalformedClassEntries)
{
    // 坏字节 .class 条目不进骨架（listClassSummaries 跳过）→ skipped 计数 1。
    mjbase::model::ResourceContainer container;
    container.addResource("fixture/Words.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Words.class", cli_fixture::assembleWordsClass()));
    container.addResource("fixture/Broken.class",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "fixture/Broken.class",
                              std::vector<ca::u8>{'n', 'o', 't', '-', 'a', '-', 'c', 'l', 'a', 's', 's'}));
    const auto path = test::getTempOutPath("mjt_cli_strings_broken_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());

    auto report = collectStrings(path.string(), "", 1);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();
    EXPECT_EQ(info.classesScanned, 1);
    EXPECT_EQ(info.classesSkipped, 1);
}

TEST(MjtCliStringsTest, SkippedAccountingMatchesContainerClassification)
{
    // 对账口径与容器分类（mjbase JarUtil::isClassFile）双侧同源：`.class/`
    // 尾斜杠条目容器剥尾斜杠登记 ClassResource（计数侧须按类计），大小写
    // 变体 `.CLASS` 双侧都不计，坏字节 .class 单侧计（不进骨架）——三者
    // 合计 skipped 恰为 1，任何一侧口径漂移都会翻成 2 / 0 / 负数。
    // JarWriter 对显式尾斜杠条目忽略内容（目录语义零字节）且写侧路径规范化
    // 会折叠/拒写非常规名（mjbase#167），语料要求 `.class/` 携类字节原样
    // 落盘——走原始 zip 直写（writeRawZip）。
    const std::vector<std::pair<std::string, std::vector<ca::u8>>> entries = {
        {"fixture/Words.class", cli_fixture::assembleWordsClass()},
        {"fixture/Trail.class/", cli_fixture::assembleWordsClass()},
        {"fixture/Upper.CLASS", cli_fixture::assembleWordsClass()},
        {"fixture/Broken.class",
         std::vector<ca::u8>{'n', 'o', 't', '-', 'a', '-', 'c', 'l', 'a', 's', 's'}},
    };
    const auto path = test::getTempOutPath("mjt_cli_strings_accounting_fixture.jar");
    cli_fixture::writeRawZip(path, entries);

    auto report = collectStrings(path.string(), "", 1);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();
    EXPECT_EQ(info.classesScanned, 2);   // Words + Trail（剥尾斜杠登记）
    EXPECT_EQ(info.classesSkipped, 1);   // 仅 Broken；Upper.CLASS 双侧不计
}

TEST(MjtCliStringsTest, FailsOnUnknownClassAndBadMinLen)
{
    const auto jar = cli_fixture::writeStringsJar();

    auto missing = collectStrings(jar.string(), "fixture/NoSuch", 1);
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(std::move(missing).unwrap_err().kind, StringsError::Kind::ClassNotFound);

    auto zero = collectStrings(jar.string(), "", 0);
    ASSERT_TRUE(zero.is_err());
    EXPECT_EQ(std::move(zero).unwrap_err().kind, StringsError::Kind::InvalidArgument);

    auto negative = collectStrings(jar.string(), "", -5);
    ASSERT_TRUE(negative.is_err());
    EXPECT_EQ(std::move(negative).unwrap_err().kind, StringsError::Kind::InvalidArgument);
}

TEST(MjtCliStringsTest, RendersDeterministicTextAndJson)
{
    const auto jar    = cli_fixture::writeStringsJar();
    auto       report = collectStrings(jar.string(), "", 1);
    ASSERT_TRUE(report.is_ok());
    const auto info = std::move(report).unwrap();

    CapturedSink textCapture;
    mjt::cli::renderStringsText(info, textCapture.sink());
    const auto text = textCapture.text();
    EXPECT_NE(text.find("strings: " + jar.string()), std::string::npos);
    EXPECT_NE(text.find("scanned 2 classes (skipped: 0)"), std::string::npos);
    EXPECT_NE(text.find("  4 hello world"), std::string::npos);
    EXPECT_NE(text.find("  2 zeta_payload_xyz"), std::string::npos);

    CapturedSink jsonCapture;
    mjt::cli::renderStringsJson(info, jsonCapture.sink());
    const auto json = jsonCapture.text();
    // Windows 路径的反斜杠经 json::escape 翻倍，期望值同口径转义。
    EXPECT_NE(json.find("\"jar\": \"" + mjt::json::escape(jar.string()) + "\""), std::string::npos);
    EXPECT_NE(json.find("\"classes_scanned\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"classes_skipped\": 0"), std::string::npos);
    EXPECT_NE(json.find("\"value\": \"hello world\", \"count\": 4, \"kinds\": {\"utf8\": 2, \"string\": 2}"),
              std::string::npos);

    // 渲染纯函数：同数据两次渲染逐字节一致。
    CapturedSink again;
    mjt::cli::renderStringsJson(info, again.sink());
    EXPECT_EQ(json, again.text());
}
