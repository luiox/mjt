/// @file        cli_classes_test.cpp
/// @brief       `mjt classes` 核心面测试：kind / origin 过滤组合对账 / --limit
///              截断 / 错误分支 / 文本与 JSON 渲染确定性。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "cli_fixture.hpp"
#include "commands.hpp"
#include "shared/output_sink.hpp"

namespace {

using mjt::cli::ClassesRow;
using mjt::cli::collectClasses;

/// 行查找：命中返回该行，未命中返回 nullptr。
const mjt::cli::ClassesRow* findRow(const mjt::cli::ClassesReport& report,
                                    const std::string& internalName)
{
    for (const auto& row : report.classes) {
        if (row.internalName == internalName) return &row;
    }
    return nullptr;
}

/// OutputSink 捕获（FILE* 往返；strings 测试同款构造）。
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
    std::FILE*       file_ = nullptr;
    mjt::OutputSink sink_;
};

}   // namespace

TEST(MjtCliClassesTest, ListsAllClassesWithKindAndOriginColumns)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectClasses(jar.string(), "", "", 10000);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    // 语料自检：9 类 + 1 资源；无过滤 = 全量无截断。
    EXPECT_EQ(info.classesInJar, 9);
    EXPECT_EQ(info.total, 9);
    EXPECT_EQ(static_cast<ca::i64>(info.classes.size()), 9);
    EXPECT_FALSE(info.truncated);
    EXPECT_EQ(info.limit, 10000);
    EXPECT_TRUE(info.kindFilter.empty());
    EXPECT_TRUE(info.originFilter.empty());

    // 行序 = 库序（internal_name BINARY 升序）。
    ASSERT_EQ(info.classes.size(), static_cast<std::size_t>(9));
    EXPECT_EQ(info.classes[0].internalName, "com/google/gson/Stub");
    EXPECT_EQ(info.classes[1].internalName, "fixture/Alpha");
    EXPECT_EQ(info.classes[7].internalName, "module-info");
    EXPECT_EQ(info.classes[8].internalName, "sun/misc/Stub");

    // kind 列 = 投影层派生形态（class_kind 稳定名）。
    EXPECT_EQ(findRow(info, "fixture/Alpha")->kind, "class");
    EXPECT_EQ(findRow(info, "fixture/Api")->kind, "interface");
    EXPECT_EQ(findRow(info, "fixture/Color")->kind, "enum");
    EXPECT_EQ(findRow(info, "fixture/Anno")->kind, "annotation");
    EXPECT_EQ(findRow(info, "fixture/Pair")->kind, "record");
    EXPECT_EQ(findRow(info, "module-info")->kind, "module");

    // origin 列 = 投影层派生来源（library_origin 稳定名，前缀分类）。
    EXPECT_EQ(findRow(info, "sun/misc/Stub")->origin, "jdk");
    EXPECT_EQ(findRow(info, "com/google/gson/Stub")->origin, "known");
    EXPECT_EQ(findRow(info, "fixture/Alpha")->origin, "unknown");
}

TEST(MjtCliClassesTest, KindFilterMatchesProjectionDerivedColumn)
{
    const auto jar = cli_fixture::writeClassesJar();

    auto interfaces = collectClasses(jar.string(), "interface", "", 10000);
    ASSERT_TRUE(interfaces.is_ok()) << interfaces.unwrap_err().message;
    {
        const auto info = std::move(interfaces).unwrap();
        EXPECT_EQ(info.kindFilter, "interface");
        EXPECT_EQ(info.total, 1);
        ASSERT_EQ(info.classes.size(), static_cast<std::size_t>(1));
        EXPECT_EQ(info.classes[0].internalName, "fixture/Api");
        EXPECT_EQ(info.classes[0].kind, "interface");
    }

    auto enums = collectClasses(jar.string(), "enum", "", 10000);
    ASSERT_TRUE(enums.is_ok()) << enums.unwrap_err().message;
    {
        const auto info = std::move(enums).unwrap();
        EXPECT_EQ(info.total, 1);
        EXPECT_EQ(info.classes[0].internalName, "fixture/Color");
    }

    auto classes = collectClasses(jar.string(), "class", "", 10000);
    ASSERT_TRUE(classes.is_ok()) << classes.unwrap_err().message;
    {
        const auto info = std::move(classes).unwrap();
        // Alpha / Native / 两个 Stub 常规类（Anno/Api/Color/Pair/module-info 出列）。
        EXPECT_EQ(info.total, 4);
        EXPECT_EQ(info.classesInJar, 9);
        EXPECT_NE(findRow(info, "fixture/Alpha"), nullptr);
        EXPECT_NE(findRow(info, "fixture/Native"), nullptr);
        EXPECT_EQ(findRow(info, "fixture/Api"), nullptr);
    }
}

TEST(MjtCliClassesTest, OriginFilterMatchesProjectionDerivedColumn)
{
    const auto jar = cli_fixture::writeClassesJar();

    auto jdk = collectClasses(jar.string(), "", "jdk", 10000);
    ASSERT_TRUE(jdk.is_ok()) << jdk.unwrap_err().message;
    {
        const auto info = std::move(jdk).unwrap();
        EXPECT_EQ(info.originFilter, "jdk");
        EXPECT_EQ(info.total, 1);
        EXPECT_EQ(info.classes[0].internalName, "sun/misc/Stub");
    }

    auto known = collectClasses(jar.string(), "", "known", 10000);
    ASSERT_TRUE(known.is_ok()) << known.unwrap_err().message;
    {
        const auto info = std::move(known).unwrap();
        EXPECT_EQ(info.total, 1);
        EXPECT_EQ(info.classes[0].internalName, "com/google/gson/Stub");
    }

    auto unknown = collectClasses(jar.string(), "", "unknown", 10000);
    ASSERT_TRUE(unknown.is_ok()) << unknown.unwrap_err().message;
    {
        const auto info = std::move(unknown).unwrap();
        EXPECT_EQ(info.total, 7);
        EXPECT_NE(findRow(info, "fixture/Alpha"), nullptr);
        EXPECT_NE(findRow(info, "module-info"), nullptr);
        EXPECT_EQ(findRow(info, "sun/misc/Stub"), nullptr);
    }
}

TEST(MjtCliClassesTest, KindAndOriginFiltersCombineAsIntersection)
{
    const auto jar = cli_fixture::writeClassesJar();

    auto both = collectClasses(jar.string(), "class", "known", 10000);
    ASSERT_TRUE(both.is_ok()) << both.unwrap_err().message;
    {
        const auto info = std::move(both).unwrap();
        EXPECT_EQ(info.kindFilter, "class");
        EXPECT_EQ(info.originFilter, "known");
        EXPECT_EQ(info.total, 1);
        ASSERT_EQ(info.classes.size(), static_cast<std::size_t>(1));
        EXPECT_EQ(info.classes[0].internalName, "com/google/gson/Stub");
    }

    // 交集无命中：known 来源里没有 interface。
    auto empty = collectClasses(jar.string(), "interface", "known", 10000);
    ASSERT_TRUE(empty.is_ok()) << empty.unwrap_err().message;
    {
        const auto info = std::move(empty).unwrap();
        EXPECT_EQ(info.total, 0);
        EXPECT_TRUE(info.classes.empty());
        EXPECT_FALSE(info.truncated);
    }
}

TEST(MjtCliClassesTest, LimitTruncatesWithHintAndTotal)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectClasses(jar.string(), "", "", 2);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    EXPECT_EQ(info.limit, 2);
    EXPECT_EQ(info.total, 9);
    EXPECT_TRUE(info.truncated);
    ASSERT_EQ(info.classes.size(), static_cast<std::size_t>(2));
    // 截断取库序前缀（internal_name 升序前两行）。
    EXPECT_EQ(info.classes[0].internalName, "com/google/gson/Stub");
    EXPECT_EQ(info.classes[1].internalName, "fixture/Alpha");

    // limit = total 恰好不截断。
    auto exact = collectClasses(jar.string(), "", "", 9);
    ASSERT_TRUE(exact.is_ok()) << exact.unwrap_err().message;
    EXPECT_FALSE(std::move(exact).unwrap().truncated);
}

TEST(MjtCliClassesTest, InvalidArgumentsAreUsageErrors)
{
    const auto jar = cli_fixture::writeClassesJar();

    auto badKind = collectClasses(jar.string(), "struct", "", 10000);
    ASSERT_TRUE(badKind.is_err());
    EXPECT_EQ(badKind.unwrap_err().kind, mjt::cli::ClassesError::Kind::InvalidArgument);

    auto badOrigin = collectClasses(jar.string(), "", "business", 10000);
    ASSERT_TRUE(badOrigin.is_err());
    EXPECT_EQ(badOrigin.unwrap_err().kind, mjt::cli::ClassesError::Kind::InvalidArgument);

    auto badLimit = collectClasses(jar.string(), "", "", 0);
    ASSERT_TRUE(badLimit.is_err());
    EXPECT_EQ(badLimit.unwrap_err().kind, mjt::cli::ClassesError::Kind::InvalidArgument);

    // kind 值大小写不敏感（xref 方向名同款口径）。
    auto upperKind = collectClasses(jar.string(), "INTERFACE", "", 10000);
    ASSERT_TRUE(upperKind.is_ok()) << upperKind.unwrap_err().message;
    EXPECT_EQ(std::move(upperKind).unwrap().total, 1);
}

TEST(MjtCliClassesTest, TextAndJsonRenderingsAreDeterministic)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectClasses(jar.string(), "enum", "unknown", 2);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    CapturedSink textCapture;
    mjt::cli::renderClassesText(info, textCapture.sink());
    const auto text = textCapture.text();
    // 文本含 kind / origin 列与命中数对账行。
    EXPECT_NE(text.find("fixture/Color enum unknown"), std::string::npos);
    EXPECT_NE(text.find("matched: 1 of 9 jar classes; showing 1"), std::string::npos);

    CapturedSink jsonCapture;
    mjt::cli::renderClassesJson(info, jsonCapture.sink());
    const auto json = jsonCapture.text();
    // JSON 形状：过滤键在场 + 对账三键 + 行对象三键。
    EXPECT_NE(json.find("\"kind\": \"enum\""), std::string::npos);
    EXPECT_NE(json.find("\"origin\": \"unknown\""), std::string::npos);
    EXPECT_NE(json.find("\"classes_in_jar\": 9"), std::string::npos);
    EXPECT_NE(json.find("\"total\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"truncated\": false"), std::string::npos);
    EXPECT_NE(json.find("{\"name\": \"fixture/Color\", \"kind\": \"enum\", "
                        "\"origin\": \"unknown\"}"),
              std::string::npos);

    // 渲染纯函数：同数据两次渲染逐字节一致。
    CapturedSink textAgain;
    mjt::cli::renderClassesText(info, textAgain.sink());
    EXPECT_EQ(text, textAgain.text());
    CapturedSink jsonAgain;
    mjt::cli::renderClassesJson(info, jsonAgain.sink());
    EXPECT_EQ(json, jsonAgain.text());
}

TEST(MjtCliClassesTest, UnopenableJarIsRuntimeError)
{
    auto report = collectClasses("mjt_cli_classes_missing.jar", "", "", 10000);
    ASSERT_TRUE(report.is_err());
    EXPECT_EQ(report.unwrap_err().kind, mjt::cli::ClassesError::Kind::Runtime);
}
