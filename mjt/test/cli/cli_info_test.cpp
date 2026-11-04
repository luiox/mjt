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

/// OutputSink 捕获：临时文件 FILE* 读写往返。
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

    //返回引用而非值：OutputSink 非 const 左值引用形参不能绑右值（MSVC 扩展
    //放行、g++ 拒绝——linux CI 实证）。
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

TEST(MjtCliInfoTest, ReportsCensusCountsKindsShapesVersionsManifest)
{
    const auto jar     = cli_fixture::writeInfoJar();
    auto       report  = collectInfo(jar.string());
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;

    const auto info = std::move(report).unwrap();
    // 9 条目 = 3 class + 2 资源 + 4 目录占位（exportToJar 为父目录补
    // fixture/、notes/、bad/、bad// 占位条目）。
    EXPECT_EQ(info.entries, 9);
    EXPECT_EQ(info.classes, 3);
    EXPECT_FALSE(info.hasManifest);

    // class_kind：三类形态各 1（class/interface/enum），名字升序。
    ASSERT_EQ(info.classKinds.size(), 3U);
    EXPECT_EQ(info.classKinds[0].first, "class");
    EXPECT_EQ(info.classKinds[0].second, 1);
    EXPECT_EQ(info.classKinds[1].first, "enum");
    EXPECT_EQ(info.classKinds[2].first, "interface");

    // entry_shape：4 条目常规；bad// 族 empty_segment 两例（"bad//" 与
    // "bad//name.txt"；facet 优先序 EmptySegment 先于 TrailingSlash）；3 个
    // 目录占位 trailing_slash；投影按 facet 枚举声明序（regular 在前）。
    ASSERT_EQ(info.entryShapes.size(), 3U);
    EXPECT_EQ(info.entryShapes[0].first, "regular");
    EXPECT_EQ(info.entryShapes[0].second, 4);
    EXPECT_EQ(info.entryShapes[1].first, "empty_segment");
    EXPECT_EQ(info.entryShapes[1].second, 2);
    EXPECT_EQ(info.entryShapes[2].first, "trailing_slash");
    EXPECT_EQ(info.entryShapes[2].second, 3);
    EXPECT_TRUE(findCount(info.entryShapes, "regular", 4));
    EXPECT_TRUE(findCount(info.entryShapes, "empty_segment", 2));
    EXPECT_TRUE(findCount(info.entryShapes, "trailing_slash", 3));

    // 字节码版本：jasm 缺省 major 61（java 17），单桶 3 类。
    ASSERT_EQ(info.versions.size(), 1U);
    EXPECT_EQ(info.versions[0].first, 61);
    EXPECT_EQ(info.versions[0].second, 3);
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
    EXPECT_NE(text.find("entries: 9"), std::string::npos);
    EXPECT_NE(text.find("classes: 3"), std::string::npos);
    EXPECT_NE(text.find("class kinds:"), std::string::npos);
    EXPECT_NE(text.find("  interface: 1"), std::string::npos);
    EXPECT_NE(text.find("entry path shapes:"), std::string::npos);
    EXPECT_NE(text.find("  trailing_slash: 3"), std::string::npos);
    EXPECT_NE(text.find("  empty_segment: 2"), std::string::npos);
    EXPECT_NE(text.find("bytecode versions:"), std::string::npos);
    EXPECT_NE(text.find("61 (java 17): 3"), std::string::npos);
    EXPECT_NE(text.find("manifest: absent"), std::string::npos);

    CapturedSink jsonCapture;
    mjt::cli::renderInfoJson(info, jsonCapture.sink());
    const auto json = jsonCapture.text();
    EXPECT_NE(json.find("\"entries\": 9"), std::string::npos);
    EXPECT_NE(json.find("\"classes\": 3"), std::string::npos);
    EXPECT_NE(json.find("\"interface\": 1"), std::string::npos);
    EXPECT_NE(json.find("\"empty_segment\": 2"), std::string::npos);
    EXPECT_NE(json.find("\"61\": 3"), std::string::npos);
    EXPECT_NE(json.find("\"has_manifest\": false"), std::string::npos);

    // 渲染纯函数：同数据两次渲染逐字节一致。
    CapturedSink again;
    mjt::cli::renderInfoJson(info, again.sink());
    EXPECT_EQ(json, again.text());
}
