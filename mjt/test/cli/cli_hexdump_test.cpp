/// @file        cli_hexdump_test.cpp
/// @brief       `mjt hexdump` 核心测试：类选择（精确/子串/未命中）、旗标防御、渲染行布局确定性（满行 / 宽度切行 / 短行补齐 / ASCII 列 / 偏移窗口 / 越界提示）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "commands.hpp"
#include "cli_fixture.hpp"

namespace {

using mjt::cli::collectHexdump;
using mjt::cli::HexdumpError;
using mjt::cli::HexdumpOptions;
using mjt::cli::HexdumpReport;

using CapturedSink = cli_fixture::CapturedSink;

/// 渲染便捷入口：report + options → 捕获文本。
std::string renderToText(const HexdumpReport& report, const HexdumpOptions& options)
{
    CapturedSink capture;
    mjt::cli::renderHexdumpText(report, options, capture.sink());
    return capture.text();
}

}   // namespace

TEST(MjtCliHexdumpTest, CollectsClassBytesWithExactAndSubstringSelectors)
{
    const auto jar = cli_fixture::writeInfoJar();

    // 内名精确命中。
    auto byName = collectHexdump(jar.string(), "fixture/Alpha", HexdumpOptions{});
    ASSERT_TRUE(byName.is_ok()) << byName.unwrap_err().message;
    auto nameInfo = std::move(byName).unwrap();
    EXPECT_EQ(nameInfo.classUri, "fixture/Alpha.class");
    EXPECT_EQ(nameInfo.internalName, "fixture/Alpha");
    ASSERT_FALSE(nameInfo.bytes.empty());
    // class 魔数 CAFEBABE。
    EXPECT_EQ(nameInfo.bytes[0], 0xCA);
    EXPECT_EQ(nameInfo.bytes[1], 0xFE);
    EXPECT_EQ(nameInfo.bytes[2], 0xBA);
    EXPECT_EQ(nameInfo.bytes[3], 0xBE);

    // URI（含 .class 后缀）精确命中：同一类。
    auto byUri = collectHexdump(jar.string(), "fixture/Alpha.class", HexdumpOptions{});
    ASSERT_TRUE(byUri.is_ok()) << byUri.unwrap_err().message;
    EXPECT_EQ(std::move(byUri).unwrap().classUri, "fixture/Alpha.class");

    // 子串候选（宽松选择：无精确命中时取首个包含选择器的条目）。
    auto byPart = collectHexdump(jar.string(), "Alpha", HexdumpOptions{});
    ASSERT_TRUE(byPart.is_ok()) << byPart.unwrap_err().message;
    EXPECT_EQ(std::move(byPart).unwrap().internalName, "fixture/Alpha");
}

TEST(MjtCliHexdumpTest, MissingSelectorFailsAsUsageError)
{
    const auto jar = cli_fixture::writeInfoJar();

    auto missing = collectHexdump(jar.string(), "fixture/NoSuch", HexdumpOptions{});
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(std::move(missing).unwrap_err().kind, HexdumpError::Kind::ClassNotFound);

    auto empty = collectHexdump(jar.string(), "", HexdumpOptions{});
    ASSERT_TRUE(empty.is_err());
    EXPECT_EQ(std::move(empty).unwrap_err().kind, HexdumpError::Kind::InvalidArgument);
}

TEST(MjtCliHexdumpTest, InvalidFlagValuesFailAsUsageErrors)
{
    const auto jar = cli_fixture::writeInfoJar();

    HexdumpOptions negativeOffset;
    negativeOffset.offset = -1;
    auto offset = collectHexdump(jar.string(), "fixture/Alpha", negativeOffset);
    ASSERT_TRUE(offset.is_err());
    EXPECT_EQ(std::move(offset).unwrap_err().kind, HexdumpError::Kind::InvalidArgument);

    HexdumpOptions zeroWidth;
    zeroWidth.width = 0;
    auto width = collectHexdump(jar.string(), "fixture/Alpha", zeroWidth);
    ASSERT_TRUE(width.is_err());
    EXPECT_EQ(std::move(width).unwrap_err().kind, HexdumpError::Kind::InvalidArgument);

    HexdumpOptions badLength;
    badLength.length = -2;
    auto length = collectHexdump(jar.string(), "fixture/Alpha", badLength);
    ASSERT_TRUE(length.is_err());
    EXPECT_EQ(std::move(length).unwrap_err().kind, HexdumpError::Kind::InvalidArgument);
}

TEST(MjtCliHexdumpTest, UnopenableJarFailsAsRuntimeError)
{
    auto missing = collectHexdump("mjt_cli_hexdump_no_such.jar", "fixture/Alpha", HexdumpOptions{});
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(std::move(missing).unwrap_err().kind, HexdumpError::Kind::Runtime);
}

TEST(MjtCliHexdumpTest, RendersFullLineWithAsciiColumn)
{
    HexdumpReport report;
    report.bytes = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x01, 0x4A, 0x46};

    // 半行短行（8 字节 × 宽 16）：魔数 hex + 短行补齐 + ASCII 列（不可打印 → '.'）。hex 尾随空格 + 25 补齐空格 + ASCII 列前导空格 = "46" 与 '|' 之间 27 空格。
    const auto text = renderToText(report, HexdumpOptions{0, -1, 16, true});
    const auto expected = "00000000  ca fe ba be 00 01 4a 46 " + std::string(25, ' ') +
                          " |......JF" + std::string(8, ' ') + "|\n";
    EXPECT_EQ(text, expected);
}

TEST(MjtCliHexdumpTest, RendersFullWidthLineWithoutPadding)
{
    HexdumpReport report;
    report.bytes = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x34,
                    0x00, 0x4A, 0x46, 0x61, 0x62, 0x63, 0x64, 0x65};

    // 满行（16 字节 = 行宽）：半行宽分隔（"34" 与 "00" 之间双空格），无补齐；ASCII 列可打印字节原样（0x34='4'、0x4A='J'…）。
    const auto text = renderToText(report, HexdumpOptions{0, -1, 16, true});
    EXPECT_EQ(text,
              "00000000  ca fe ba be 00 00 00 34  00 4a 46 61 62 63 64 65  "
              "|.......4.JFabcde|\n");
}

TEST(MjtCliHexdumpTest, RendersWidthSplitAndShortLinePadding)
{
    HexdumpReport report;
    report.bytes = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0xFF};

    // 宽 4：两行；首行半行宽分隔（fe 与 ba 之间双空格），次行短行补齐。
    const auto text = renderToText(report, HexdumpOptions{0, -1, 4, true});
    const auto expected = std::string("00000000  ca fe  ba be  |....|\n") + "00000004  00 ff " +
                          std::string(7, ' ') + " |..  |\n";
    EXPECT_EQ(text, expected);
}

TEST(MjtCliHexdumpTest, RendersOffsetWindowWithoutAscii)
{
    HexdumpReport report;
    report.bytes = {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00, 0x00, 0x34,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

    // --offset 4 --length 4 --no-ascii：单行窗口；行布局恒补齐 hex 区（ASCII 列缺席也保留对齐位），无 '|'。
    const auto text = renderToText(report, HexdumpOptions{4, 4, 16, false});
    const auto expected = "00000004  00 00 00 34 " + std::string(37, ' ') + "\n";
    EXPECT_EQ(text, expected);
    EXPECT_EQ(text.find('|'), std::string::npos);
}

TEST(MjtCliHexdumpTest, RendersOffsetPastEndNote)
{
    HexdumpReport report;
    report.bytes = {0xCA, 0xFE, 0xBA, 0xBE};

    const auto text = renderToText(report, HexdumpOptions{999, -1, 16, true});
    // en_US 提示行（共享表 mjt.cmd.hexdump.out.offset_past_end）。
    EXPECT_NE(text.find("(empty"), std::string::npos);
    EXPECT_NE(text.find("offset past end"), std::string::npos);
    EXPECT_EQ(text.find("ca fe"), std::string::npos);
}

TEST(MjtCliHexdumpTest, LengthZeroRendersNothing)
{
    HexdumpReport report;
    report.bytes = {0xCA, 0xFE, 0xBA, 0xBE};
    EXPECT_EQ(renderToText(report, HexdumpOptions{0, 0, 16, true}), "");
}

TEST(MjtCliHexdumpTest, RenderIsDeterministic)
{
    const auto jar    = cli_fixture::writeInfoJar();
    auto       report = collectHexdump(jar.string(), "fixture/Alpha", HexdumpOptions{});
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    const auto first  = renderToText(info, HexdumpOptions{});
    const auto second = renderToText(info, HexdumpOptions{});
    EXPECT_EQ(first, second);
    EXPECT_NE(first.find("ca fe ba be"), std::string::npos);
}
