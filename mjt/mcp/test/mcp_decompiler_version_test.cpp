#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "shared/decompiler.hpp"
#include "test_helper.hpp"

namespace mjt {
namespace {

using namespace mjt::mcp::test_support;

///@brief 写一个只含 META-INF/MANIFEST.MF 的 fixture JAR。
std::filesystem::path writeManifestJar(const std::string& fileName,
                                       const std::string& manifestText)
{
    mjbase::model::ResourceContainer container;
    container.addResource(
        "META-INF/MANIFEST.MF",
        std::make_unique<mjbase::model::UnknownResource>(
            "META-INF/MANIFEST.MF",
            std::vector<ca::u8>(manifestText.begin(), manifestText.end())));
    const auto path = test::getTempOutPath(fileName);
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
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
    mjbase::model::ResourceContainer container;
    container.addResource("notice.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "notice.txt", std::vector<ca::u8>{'o', 'k'}));
    const auto path = test::getTempOutPath("cfr-0.152.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
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

TEST(McpDecompilerVersionTest, ProjectInfoSurfacesBackendVersions)
{
    //decompiler_settings_* 已 SDK 化出 MCP 面：project_info 直接报告会话默认
    //配置下的后端可用性；version 恒为字符串（稳定契约），未配置 JAR 时值为空。
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto info =
        call(server, "project_info", {{"project_id", payload(opened).at("project_id")}});
    ASSERT_FALSE(info.at("result").at("isError"));
    const auto& backends = payload(info).at("decompilers");
    ASSERT_EQ(2U, backends.size());
    EXPECT_TRUE(backends.at(0).at("version").is_string());
    EXPECT_TRUE(backends.at(1).at("version").is_string());
}


//decompilers[] 报 JAR 来源层（explicit/env/package_dir）与可覆盖环境
//变量 override_env；旧 environment 键删除（严格切换，无双轨）。
TEST(McpDecompilerVersionTest, DiscoverySourceAndOverrideEnv)
{
    const auto jar = writeManifestJar(
        "mjt_source_layer.jar",
        "Manifest-Version: 1.0\n"
        "\n");
    const auto explicitHit = inspectDecompiler(DecompilerKind::Cfr, jar.u8string(), "java");
    EXPECT_EQ("explicit", explicitHit.discoverySource);
    EXPECT_EQ("MJT_CFR_JAR", explicitHit.environmentVariable);

    const auto vineflower = inspectDecompiler(DecompilerKind::Vineflower, {}, "java");
    EXPECT_EQ("MJT_VINEFLOWER_JAR", vineflower.environmentVariable);
    EXPECT_TRUE(vineflower.discoverySource == "env" ||
                vineflower.discoverySource == "package_dir")
        << vineflower.discoverySource;

    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto info =
        call(server, "project_info", {{"project_id", payload(opened).at("project_id")}});
    ASSERT_FALSE(info.at("result").at("isError"));
    const auto& backends = payload(info).at("decompilers");
    ASSERT_EQ(2U, backends.size());
    EXPECT_EQ("MJT_CFR_JAR", backends.at(0).at("override_env").get<std::string>());
    EXPECT_EQ("MJT_VINEFLOWER_JAR", backends.at(1).at("override_env").get<std::string>());
    for (const auto& backend : backends) {
        EXPECT_FALSE(backend.contains("environment"));
        const auto& source = backend.at("source").get_ref<const std::string&>();
        EXPECT_TRUE(source == "explicit" || source == "env" || source == "package_dir")
            << source;
    }
}

}   //namespace
}   //namespace mjt
