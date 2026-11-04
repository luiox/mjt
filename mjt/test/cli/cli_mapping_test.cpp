/// @file        cli_mapping_test.cpp
/// @brief       `mjt mapping` 核心面测试：格式名解析、SRG ↔ Tiny-V2 ↔
///              proguard 三族互转往返恒等、未知格式 fail-loud、jar 重映射
///              接线（shared 层 remapJar）。
#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "commands.hpp"
#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/mapping/mapping_format_registry.hpp"
#include "cli_fixture.hpp"

namespace {

/// SRG → Tiny-V2 → Proguard → SRG 的三级转换链（同一命令核心函数）。
std::string chainConvert(const std::string& srgText)
{
    auto toTiny = mjt::cli::convertMappingText(srgText, "srg", "tiny-v2");
    EXPECT_TRUE(toTiny.is_ok()) << (toTiny.is_err() ? toTiny.unwrap_err().message : "");
    if (toTiny.is_err()) return {};
    auto toProguard = mjt::cli::convertMappingText(toTiny.unwrap(), "Tiny-V2", "proguard");
    EXPECT_TRUE(toProguard.is_ok()) << (toProguard.is_err() ? toProguard.unwrap_err().message : "");
    if (toProguard.is_err()) return {};
    auto backToSrg = mjt::cli::convertMappingText(toProguard.unwrap(), "PROGUARD", "SRG");
    EXPECT_TRUE(backToSrg.is_ok()) << (backToSrg.is_err() ? backToSrg.unwrap_err().message : "");
    if (backToSrg.is_err()) return {};
    return backToSrg.unwrap();
}

/// 解析 SRG 文本，返回类映射条目数 / 字段数 / 方法数（语义对账用）。
struct MappingCounts
{
    std::size_t classes = 0;
    std::size_t fields  = 0;
    std::size_t methods = 0;
    bool        ok      = false;
};

MappingCounts parseSrgCounts(const std::string& text)
{
    MappingCounts counts;
    auto          registry = mjbase::mapping::MappingFormatRegistry::withStandardFormats();
    auto parsed = registry.parse(ca::str::Utf8StringRef::from_cstr("SRG"),
                                 ca::str::Utf8StringRef::from_data(
                                     reinterpret_cast<const ca::u8*>(text.data()), text.size()));
    if (parsed.is_err()) return counts;
    auto mappings = std::move(parsed).unwrap();
    counts.classes  = mappings.getClasses().size();
    counts.fields   = mappings.getClassFieldMappings(
                          ca::str::Utf8StringRef::from_cstr("fixture/Alpha")) != nullptr
                          ? mappings.getClassFieldMappings(
                                ca::str::Utf8StringRef::from_cstr("fixture/Alpha"))
                                ->size()
                          : 0;
    counts.methods  = mappings.getClassMethodMappings(
                          ca::str::Utf8StringRef::from_cstr("fixture/Alpha")) != nullptr
                          ? mappings.getClassMethodMappings(
                                ca::str::Utf8StringRef::from_cstr("fixture/Alpha"))
                                ->size()
                          : 0;
    counts.ok       = true;
    return counts;
}

}   // namespace

TEST(MjtCliMappingTest, ResolvesFormatNamesCaseInsensitively)
{
    auto srg = mjt::cli::resolveMappingFormatName("srg");
    ASSERT_TRUE(srg.is_ok());
    EXPECT_EQ(srg.unwrap(), "SRG");

    auto tiny = mjt::cli::resolveMappingFormatName("Tiny-V2");
    ASSERT_TRUE(tiny.is_ok());
    EXPECT_EQ(tiny.unwrap(), "Tiny-V2");

    // 注册表多别名（"Proguard"/"ProGuard"）折叠后命中同一实现，取
    // formatNames() 确定序（字节升序）首个。
    auto proguard = mjt::cli::resolveMappingFormatName("proguard");
    ASSERT_TRUE(proguard.is_ok());
    EXPECT_EQ(proguard.unwrap(), "ProGuard");

    auto unknown = mjt::cli::resolveMappingFormatName("definitely-not-a-format");
    ASSERT_TRUE(unknown.is_err());
    EXPECT_EQ(unknown.unwrap_err().kind, mjt::cli::MappingError::Kind::UnknownFormat);
    EXPECT_NE(unknown.unwrap_err().message.find("definitely-not-a-format"), std::string::npos);
}

TEST(MjtCliMappingTest, ConvertsSrgThroughTinyV2AndProguardRoundTrip)
{
    const std::string srgText = cli_fixture::srgMappingText();

    // 单跳恒等：SRG parse→format 产出规范形 == 原文（语料已按规范序书写）。
    auto canonical = mjt::cli::convertMappingText(srgText, "srg", "SRG");
    ASSERT_TRUE(canonical.is_ok());
    EXPECT_EQ(canonical.unwrap(), srgText);

    // 三级链：SRG → Tiny-V2 → Proguard → SRG 逐字节恒等。
    const auto roundTrip = chainConvert(srgText);
    ASSERT_FALSE(roundTrip.empty());
    EXPECT_EQ(roundTrip, srgText);

    // 中间形态可辨识：Tiny-V2 头行 + Proguard 箭头行。
    auto toTiny = mjt::cli::convertMappingText(srgText, "srg", "tiny-v2");
    ASSERT_TRUE(toTiny.is_ok());
    EXPECT_NE(toTiny.unwrap().find("tiny\t2\t0\tintermediary\tnamed"), std::string::npos);
    auto toProguard = mjt::cli::convertMappingText(toTiny.unwrap(), "Tiny-V2", "proguard");
    ASSERT_TRUE(toProguard.is_ok());
    EXPECT_NE(toProguard.unwrap().find("fixture.RenamedAlpha -> fixture.Alpha:"), std::string::npos);

    // 语义对账：往返前后 SRG 解析产物同构（类/字段/方法计数一致）。
    const auto before = parseSrgCounts(srgText);
    const auto after  = parseSrgCounts(roundTrip);
    ASSERT_TRUE(before.ok);
    ASSERT_TRUE(after.ok);
    EXPECT_EQ(before.classes, after.classes);
    EXPECT_EQ(before.fields, after.fields);
    EXPECT_EQ(before.methods, after.methods);
}

TEST(MjtCliMappingTest, UnknownFormatFailsLoud)
{
    const std::string srgText = cli_fixture::srgMappingText();
    auto              badFrom = mjt::cli::convertMappingText(srgText, "nope", "srg");
    ASSERT_TRUE(badFrom.is_err());
    EXPECT_EQ(badFrom.unwrap_err().kind, mjt::cli::MappingError::Kind::UnknownFormat);
    auto badTo = mjt::cli::convertMappingText(srgText, "srg", "");
    ASSERT_TRUE(badTo.is_err());
    EXPECT_EQ(badTo.unwrap_err().kind, mjt::cli::MappingError::Kind::UnknownFormat);
}

TEST(MjtCliMappingTest, RemapsJarWithConvertedMapping)
{
    // 语料 jar 内含 fixture/Alpha；SRG 映射 fixture/Alpha → fixture/RenamedAlpha。
    const auto mappingPath = cli_fixture::writeTextFile("mjt_cli_remap.srg", cli_fixture::srgMappingText());
    const auto remapIn    = cli_fixture::writeInfoJar();
    const auto remapOut   = test::getTempOutPath("mjt_cli_remap_out.jar");
    std::error_code ec;
    std::filesystem::remove(remapOut, ec);

    mjt::cli::RemapJarRequest request;
    request.jarPath     = remapIn.string();
    request.outputPath  = remapOut.string();
    request.mappingPath = mappingPath.string();
    request.format      = "SRG";
    request.policy      = "bytecode-only";
    auto remapped = mjt::cli::remapJarWithMapping(request);
    ASSERT_TRUE(remapped.is_ok()) << (remapped.is_err() ? remapped.unwrap_err().message : "");
    EXPECT_TRUE(remapped.unwrap());

    // 重映射产物：Alpha 条目消失，RenamedAlpha 出现（class 引用面被重写）。
    const auto uris = cli_fixture::jarEntryUris(remapOut);
    EXPECT_NE(std::find(uris.begin(), uris.end(), "fixture/RenamedAlpha.class"), uris.end());
    EXPECT_EQ(std::find(uris.begin(), uris.end(), "fixture/Alpha.class"), uris.end());
}
