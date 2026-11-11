#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"
#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/model/resource_helper.hpp"

#include "workspace/analysis_ops.hpp"
#include "workspace/container_io.hpp"
#include "workspace/edit_command.hpp"
#include "workspace/workspace.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

/// @brief 新格式（dex/apk 前身）注入端到端：注册表加一行 → open/query/edit/
///        export 全链跑通，WorkspaceBase 头签名零变化（W2 核心不变量验收）。
///
/// stub 的导入面复用 jar 读取（物理字节是 jar）——注册表扩展点的被测对象是
/// 「格式可插入」而非 dex 解码本身；导出面走 mjbase exportToJar（保真契约
/// 只约束 jar 行，stub 行自便）。
class McpContainerIoStubTest : public ::testing::Test
{
protected:
    /// stub 格式值：注册表作用域的标签（枚举面不为新格式扩值——加值即改
    /// 公共头，违背扩展点契约）。
    static constexpr auto kStubFormat = static_cast<workspace::ContainerFormat>(0xD1);

    static ca::str::Utf8StringRef refOf(const std::string& value)
    {
        return ca::str::Utf8StringRef::from_data(
            reinterpret_cast<const ca::u8*>(value.data()), value.size());
    }

    static ca::str::Utf8String ownOf(const std::string& value)
    {
        return ca::str::Utf8String::from_data_unchecked(
            reinterpret_cast<const ca::u8*>(value.data()), value.size());
    }

    static workspace::WorkspaceResult<workspace::ImportedArtifact> stubImport(
        const std::string& path, ca::str::Utf8StringRef pathRef,
        const mjbase::jar::IJarReader::EntryNameDecoder& decoder)
    {
        (void)pathRef;
        (void)decoder;
        try {
            mjbase::model::ResourceContainer loaded;
            auto reader = std::make_unique<mjbase::jar::JvmJarReader>(refOf(path));
            auto imported =
                mjbase::model::ResourceHelper::importFromJar(loaded, path, std::move(reader));
            if (imported.is_err()) {
                return ca::core::Err(workspace::modelFailure(
                    "open dexstub", std::move(imported).unwrap_err()));
            }
            auto original = workspace::cloneContainer(loaded);
            if (original.is_err()) return ca::core::Err(std::move(original).unwrap_err());
            auto working = workspace::cloneContainer(loaded);
            if (working.is_err()) return ca::core::Err(std::move(working).unwrap_err());

            workspace::ImportedArtifact artifact;
            artifact.original = std::move(original).unwrap();
            artifact.working  = std::move(working).unwrap();
            return ca::core::Ok(std::move(artifact));
        }
        catch (const std::exception& exception) {
            workspace::WorkspaceErrorInfo info;
            info.code    = workspace::WorkspaceError::DataLoss;
            info.message = "open dexstub: " + std::string(exception.what());
            return ca::core::Err(std::move(info));
        }
    }

    static workspace::WorkspaceErrorInfo stubExport(const workspace::ExportSpec& spec,
                                                    const std::string&           outputPath)
    {
        auto written = mjbase::model::ResourceHelper::exportToJar(*spec.working, outputPath);
        if (written.is_err())
            return workspace::modelFailure("export dexstub", std::move(written).unwrap_err());
        return workspace::WorkspaceErrorInfo{};
    }

    void SetUp() override
    {
        workspace::ContainerIoRegistry::instance().registerEntry(
            {kStubFormat, "dexstub", {false}, &stubImport, &stubExport});
    }

    /// fixture jar 物理拷贝成 .dexstub——格式路由只看扩展名。
    std::filesystem::path writeStubArtifact()
    {
        const auto jarPath  = writeFixture();
        const auto stubPath = test::getTempOutPath("mjt_mcp_container_io_stub.dexstub");
        std::filesystem::copy_file(jarPath, stubPath,
                                   std::filesystem::copy_options::overwrite_existing);
        return stubPath;
    }
};

TEST_F(McpContainerIoStubTest, RegisteredFormatRunsOpenQueryEditExport)
{
    const auto stubPath = writeStubArtifact();

    // open：扩展名路由到注册行（WorkspaceBase 无 stub 专用方法）。
    workspace::WorkspaceBase ws;
    auto opened = ws.open(refOf(stubPath.string()));
    ASSERT_TRUE(opened.is_ok()) << opened.unwrap_err().message;
    EXPECT_EQ(kStubFormat, ws.format());

    // query：classInfo 算子按内名解析（fixture 的 Subject 类）。
    auto info = workspace::classInfo(*ws.workingContainer(), refOf("mcp/Subject"),
                                     ws.lvtJarFallbackFlag());
    ASSERT_TRUE(info.is_ok()) << info.unwrap_err().message;
    EXPECT_EQ("mcp/Subject", std::move(info).unwrap().internalName.to_std_string());

    // edit：ClassBytes 同字节再替换（候选构建 + 门禁 + commit 一条管道）。
    auto bytes = workspace::readClassBytes(*ws.workingContainer(), refOf("mcp/Subject"));
    ASSERT_TRUE(bytes.is_ok());
    const auto subjectBytes = std::move(bytes).unwrap().bytes;
    workspace::MutationRequest request;
    const auto applied = workspace::applyEdit(
        ws,
        workspace::EditCommand{workspace::EditCommand::ClassBytes{ownOf("mcp/Subject.class"),
                                                                  subjectBytes}},
        request);
    EXPECT_TRUE(applied.status.is_ok()) << applied.status.message;

    // export：注册行导出物可重开（.dexstub 扩展名 → 仍路由 stub 行；物化为
    // jar 字节，stub 导入面读回）且内容存活。
    const auto outPath = test::getTempOutPath("mjt_mcp_container_io_stub_out.dexstub");
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
    auto exported = ws.exportArtifact(refOf(outPath.string()));
    ASSERT_TRUE(exported.is_ok()) << exported.unwrap_err().message;
    ASSERT_TRUE(std::filesystem::is_regular_file(outPath));

    workspace::WorkspaceBase reopened;
    auto reopenedStatus = reopened.open(refOf(outPath.string()));
    ASSERT_TRUE(reopenedStatus.is_ok());
    EXPECT_EQ(kStubFormat, reopened.format());
    auto reopenedInfo = workspace::classInfo(*reopened.workingContainer(), refOf("mcp/Subject"),
                                              reopened.lvtJarFallbackFlag());
    ASSERT_TRUE(reopenedInfo.is_ok());
}

}   // namespace
}   // namespace mjt::mcp
