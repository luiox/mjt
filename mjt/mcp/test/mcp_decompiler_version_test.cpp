#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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

///@brief 写一个假的常规文件（可执行件发现/产物在场性用例的轻量 fixture）。
std::filesystem::path writeFakeArtifact(const std::string& fileName)
{
    const auto path = test::getTempOutPath(fileName);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "fake";
    return path;
}

///@brief 进程级环境变量保存 → 清空 → 析构恢复（RAII）。
///
///jcdc 发现层用例要让发现序落到第 4 层 package 目录扫描，必须先让
///MJT_JCDC_EXE / MJT_DECOMPILER_HOME 两层落空；作用域结束恢复原值，不污染
///同进程后续用例（_putenv_s 与产品侧 _dupenv_s 同一 CRT 环境块，清空即删除）。
class ScopedEnvironment final
{
public:
    explicit ScopedEnvironment(const char* name) : name_(name)
    {
#if defined(_WIN32)
        //_dupenv_s 与产品侧 environment() 同一取值面（且避开 C4996 的 getenv 弃用告警）。
        char* value = nullptr;
        if (_dupenv_s(&value, nullptr, name) == 0 && value != nullptr) {
            saved_ = value;
            std::free(value);
        }
#else
        if (const char* value = std::getenv(name)) saved_ = value;
#endif
        set(nullptr);
    }
    ~ScopedEnvironment() { set(saved_ ? saved_->c_str() : nullptr); }

    ScopedEnvironment(const ScopedEnvironment&)            = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    void set(const char* value)
    {
#if defined(_WIN32)
        _putenv_s(name_, value == nullptr ? "" : value);
#else
        if (value == nullptr)
            unsetenv(name_);
        else
            setenv(name_, value, 1);
#endif
    }

    const char*                name_;
    std::optional<std::string> saved_;
};

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
    ASSERT_EQ(3U, backends.size());
    EXPECT_TRUE(backends.at(0).at("version").is_string());
    EXPECT_TRUE(backends.at(1).at("version").is_string());
    EXPECT_TRUE(backends.at(2).at("version").is_string());
    EXPECT_EQ("jcdc", backends.at(2).at("id").get<std::string>());
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
    ASSERT_EQ(3U, backends.size());
    EXPECT_EQ("MJT_CFR_JAR", backends.at(0).at("override_env").get<std::string>());
    EXPECT_EQ("MJT_VINEFLOWER_JAR", backends.at(1).at("override_env").get<std::string>());
    EXPECT_EQ("MJT_JCDC_EXE", backends.at(2).at("override_env").get<std::string>());
    for (const auto& backend : backends) {
        EXPECT_FALSE(backend.contains("environment"));
        const auto& source = backend.at("source").get_ref<const std::string&>();
        EXPECT_TRUE(source == "explicit" || source == "env" || source == "package_dir")
            << source;
    }
}

//── jcdc（第三实验后端：原生可执行文件、非 JAR、不经 JVM；luiox/mjt#62）────

//后端 ID 稳定且第三后端可解析——缓存 produced_by、报告 backend 字段、CLI 旗标
//三处共用同一条 ID（decompilerKindId / parseDecompilerKind）。
TEST(McpDecompilerVersionTest, JcdcAndJadxBackendIdsParseAndStayStable)
{
    const auto kind = parseDecompilerKind("jcdc");
    ASSERT_TRUE(kind.is_ok());
    EXPECT_EQ(DecompilerKind::Jcdc, kind.unwrap());
    EXPECT_EQ("jcdc", decompilerKindId(kind.unwrap()));
    const auto jadx = parseDecompilerKind("jadx");
    ASSERT_TRUE(jadx.is_ok());
    EXPECT_EQ(DecompilerKind::Jadx, jadx.unwrap());
    EXPECT_EQ("jadx", decompilerKindId(jadx.unwrap()));
    //未知名仍报错（错误文案含期望列表，见 mjt.shared.err.unknown_decompiler）。
    EXPECT_TRUE(parseDecompilerKind("unknown-backend").is_err());
}

//jcdc 无 MANIFEST 可读：版本走可执行文件名回退——`jcdc-<version>.exe` 取数字段；
//发布件 `jcdc-v<version>-<os>-<arch>.exe` 剥 `v` 取版本且平台段随版本串自述；
//固定名 `jcdc.exe` 无数字段不冒充版本（空串，展示层以产物路径兜底）。
//requiresJava=false：Java 缺席不拦 available、不进 unavailableReason。
TEST(McpDecompilerVersionTest, JcdcVersionFallsBackToExecutableFileName)
{
    const auto versioned = writeFakeArtifact("jcdc-1.0.0.exe");
    const auto availability =
        inspectDecompiler(DecompilerKind::Jcdc, versioned.u8string(), "java");
    EXPECT_TRUE(availability.artifactAvailable);
    EXPECT_EQ("MJT_JCDC_EXE", availability.environmentVariable);
    EXPECT_EQ("explicit", availability.discoverySource);
    EXPECT_EQ("1.0.0", availability.version);
    EXPECT_FALSE(availability.requiresJava);
    EXPECT_TRUE(availability.unavailableReason.empty()) << availability.unavailableReason;

    const auto release = writeFakeArtifact("jcdc-v2.3.4-windows-amd64.exe");
    EXPECT_EQ("2.3.4-windows-amd64",
              inspectDecompiler(DecompilerKind::Jcdc, release.u8string(), "java").version);

    const auto bare = writeFakeArtifact("jcdc.exe");
    EXPECT_EQ("", inspectDecompiler(DecompilerKind::Jcdc, bare.u8string(), "java").version);
}

//package 目录扫描（发现序第 4 层）对 jcdc 走可执行件规则：前缀 `jcdc` +
//平台后缀（Windows 必须 .exe），文件名字典序取首个命中；同前缀旁文件
//（.sha256 等，字典序更靠前）必须出局。前两层 env（MJT_JCDC_EXE /
//MJT_DECOMPILER_HOME）在本用例里清空以落到扫描层，析构恢复。
TEST(McpDecompilerVersionTest, JcdcPackageDirectoryDiscoverySkipsSidecarFiles)
{
    const ScopedEnvironment jcdcExeEnv("MJT_JCDC_EXE");
    const ScopedEnvironment homeEnv("MJT_DECOMPILER_HOME");

    const auto       root = test::getTempOutPath("mjt_jcdc_probe");
    std::error_code  error;
    std::filesystem::remove_all(root, error);
    const auto packageDir = root / "package";
    ASSERT_TRUE(std::filesystem::create_directories(packageDir, error)) << error.message();

    const auto exePath     = writeFakeArtifact("mjt_jcdc_probe/mjt_jcdc_probe.exe");
    const auto versioned   = packageDir / "jcdc-v9.9.9-windows-amd64.exe";
    std::ofstream(versioned, std::ios::binary | std::ios::trunc) << "fake";
    //同前缀旁文件：'a' < 'v' 字典序靠前，若规则失效会先命中它。
    const auto sidecar = packageDir / "jcdc-a.sha256";
    std::ofstream(sidecar, std::ios::binary | std::ios::trunc) << "x";

    DecompileRequest request;
    request.kind = DecompilerKind::Jcdc;
    const auto discovery = discoverDecompilerJarDetailed(request, exePath.u8string());
    EXPECT_EQ(versioned.lexically_normal().string(), discovery.jar);
}

}   //namespace
}   //namespace mjt
