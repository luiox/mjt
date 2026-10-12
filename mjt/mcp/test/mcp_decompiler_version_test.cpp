#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/model/resource_helper.hpp"
#include "mbase/model/resource_model.hpp"
#include "shared/decompiler.hpp"
#include "test_helper.hpp"

namespace mjt {
namespace {

using namespace mjt::mcp::test_support;

///@brief 写一个只含 META-INF/MANIFEST.MF 的 fixture JAR。
std::filesystem::path writeManifestJar(const std::string& fileName,
                                       const std::string& manifestText)
{
    base::ResourceContainer container;
    container.addResource(
        "META-INF/MANIFEST.MF",
        std::make_unique<base::UnknownResource>(
            "META-INF/MANIFEST.MF",
            std::vector<ca::u8>(manifestText.begin(), manifestText.end())));
    const auto path = test::getTempOutPath(fileName);
    base::ResourceHelper::exportToJar(container, path.string());
    return path;
}

TEST(McpDecompilerVersionTest, ReadsImplementationVersionFromManifest)
{
    const auto jar = writeManifestJar(
        "mjt_version_impl.jar",
        "Manifest-Version: 1.0\n"
        "Implementation-Version: 0.152\n"
        "\n");
    EXPECT_EQ("0.152", decompilerJarVersion(jar.u8string()));
}

TEST(McpDecompilerVersionTest, FallsBackToSpecificationVersion)
{
    const auto jar = writeManifestJar(
        "mjt_version_spec.jar",
        "Manifest-Version: 1.0\n"
        "Specification-Version: 9.9\n"
        "\n");
    EXPECT_EQ("9.9", decompilerJarVersion(jar.u8string()));
}

TEST(McpDecompilerVersionTest, ImplementationVersionWinsOverSpecificationVersion)
{
    const auto jar = writeManifestJar(
        "mjt_version_both.jar",
        "Manifest-Version: 1.0\n"
        "Specification-Version: 9.9\n"
        "Implementation-Version: 1.2.3\n"
        "\n");
    EXPECT_EQ("1.2.3", decompilerJarVersion(jar.u8string()));
}

TEST(McpDecompilerVersionTest, UnfoldsManifestContinuationLines)
{
    const auto jar = writeManifestJar(
        "mjt_version_folded.jar",
        "Manifest-Version: 1.0\n"
        "Implementation-Title: vineflower with a rather long display name that needs folding\n"
        "Implementation-Version: 1.12.0\n"
        "\n");
    EXPECT_EQ("1.12.0", decompilerJarVersion(jar.u8string()));
}

TEST(McpDecompilerVersionTest, ManifestWithoutVersionFallsBackToFileName)
{
    const auto jar = writeManifestJar("vineflower-1.12.0.jar", "Manifest-Version: 1.0\n\n");
    EXPECT_EQ("1.12.0", decompilerJarVersion(jar.u8string()));
}

TEST(McpDecompilerVersionTest, MissingManifestFallsBackToFileName)
{
    base::ResourceContainer container;
    container.addResource("notice.txt",
                          std::make_unique<base::UnknownResource>(
                              "notice.txt", std::vector<ca::u8>{'o', 'k'}));
    const auto path = test::getTempOutPath("cfr-0.152.jar");
    base::ResourceHelper::exportToJar(container, path.string());
    EXPECT_EQ("0.152", decompilerJarVersion(path.u8string()));
}

TEST(McpDecompilerVersionTest, NonNumericFileNameSegmentIsNotAVersion)
{
    //词尾（decompiler）不得冒充版本；文件名回退要求段首是数字。
    const auto missing = test::getTempOutPath("fake-mcp-decompiler.jar");
    EXPECT_EQ("", decompilerJarVersion(missing.u8string()));

    //非 ZIP 文件（既有 fake 产物写法）走文件名回退，同样不产生伪版本。
    const auto plain = test::getTempOutPath("fake-mcp-decompiler.jar");
    std::ofstream(plain, std::ios::binary | std::ios::trunc) << "fake";
    EXPECT_EQ("", decompilerJarVersion(plain.u8string()));
}

TEST(McpDecompilerVersionTest, EmptyJarPathYieldsEmptyVersion)
{
    EXPECT_EQ("", decompilerJarVersion(""));
}

TEST(McpDecompilerVersionTest, InspectDecompilerFillsVersionWhenArtifactAvailable)
{
    const auto jar = writeManifestJar(
        "mjt_version_inspect.jar",
        "Manifest-Version: 1.0\n"
        "Implementation-Version: 7.7.7\n"
        "\n");
    const auto availability = inspectDecompiler(DecompilerKind::Cfr, jar.u8string(), "java");
    EXPECT_TRUE(availability.artifactAvailable);
    EXPECT_EQ("7.7.7", availability.version);

    const auto absent = inspectDecompiler(DecompilerKind::Cfr, {}, "java");
    EXPECT_EQ("", absent.version);
}

TEST(McpDecompilerVersionTest, DecompilerListSurfacesBackendVersion)
{
    McpSessionHarness server;
    const auto        jar = writeManifestJar(
        "mjt_version_list.jar",
        "Manifest-Version: 1.0\n"
        "Implementation-Version: 0.152\n"
        "\n");
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");

    const auto settingsSet = call(server,
                                  "decompiler_settings_set",
                                  {{"cfr_jar", jar.string()},
                                   {"java", fakeJava.string()}});
    ASSERT_FALSE(settingsSet.at("result").at("isError"));
    const auto& backends = payload(settingsSet).at("backends");
    ASSERT_EQ(2U, backends.size());
    EXPECT_EQ("0.152", backends.at(0).at("version").get<std::string>());
    //vineflower 后端未配置 JAR：version 必须是字符串（稳定契约），值为空。
    EXPECT_TRUE(backends.at(1).at("version").is_string());

    const auto listed = call(server, "decompiler_list", nlohmann::json::object());
    ASSERT_FALSE(listed.at("result").at("isError"));
    EXPECT_EQ("0.152", payload(listed).at("backends").at(0).at("version").get<std::string>());
}

}   //namespace
}   //namespace mjt
