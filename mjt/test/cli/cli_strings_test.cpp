/// @file        cli_strings_test.cpp
/// @brief       `mjt strings` 核心面测试：抽取计数 / kind 计数 / --min-len 过滤
///              / 单类过滤 / 错误分支 / 文本与 JSON 渲染确定性。
#include <gtest/gtest.h>

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

/// OutputSink 捕获（FILE* 往返；cli_info_test 同款构造）。
class CapturedSink
{
public:
    CapturedSink()
        : file_(std::tmpfile())
        , sink_(file_)
    {}
    ~CapturedSink()
    {
        if (file_ != nullptr) std::fclose(file_);
    }
    CapturedSink(const CapturedSink&)            = delete;
    CapturedSink& operator=(const CapturedSink&) = delete;

    mjt::OutputSink& sink() { return sink_; }

    std::string text()
    {
        std::fflush(file_);
        std::rewind(file_);
        std::string out;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file_)) > 0) out.append(buffer, read);
        return out;
    }

private:
    std::FILE*     file_ = nullptr;
    mjt::OutputSink sink_;
};

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
