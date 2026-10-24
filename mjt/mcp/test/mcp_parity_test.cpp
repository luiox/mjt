#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

TEST(McpServerTest, PreservesParityFixtureEdgesAcrossQueriesTransactionsAndExport)
{
    McpSessionHarness server;

    const auto opened = call(server, "project_open", {{"path", writeParityFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto resources =
        call(server, "search", {{"project_id", projectId}, {"kind", "resources"}});
    ASSERT_FALSE(resources.at("result").at("isError"));
    const auto& resourceItems = payload(resources).at("items");
    const auto hasResource = [&resourceItems](const std::string& uri) {
        return std::any_of(resourceItems.begin(), resourceItems.end(), [&uri](const Json& item) {
            return item.at("uri") == uri;
        });
    };
    EXPECT_TRUE(hasResource("META-INF/versions/9/mcp/parity/Versioned.class"));
    EXPECT_TRUE(hasResource("assets/payload.bin"));
    EXPECT_TRUE(hasResource("mcp/parity/Broken.class"));

    const auto brokenBytes = call(server,
                                  "class_query",
                                  {{"project_id", projectId},
                                   {"kind", "bytes"},
                                   {"class", "mcp/parity/Broken.class"}});
    ASSERT_FALSE(brokenBytes.at("result").at("isError"));
    EXPECT_EQ("mcp/parity/Broken.class", payload(brokenBytes).at("resource_uri"));
    EXPECT_TRUE(payload(brokenBytes).at("internal_name").get<std::string>().empty());
    EXPECT_EQ(3, payload(brokenBytes).at("size"));
    EXPECT_EQ("yv66", payload(brokenBytes).at("content"));

    const auto manifest =
        call(server, "class_query", {{"project_id", projectId}, {"kind", "manifest"}});
    ASSERT_FALSE(manifest.at("result").at("isError"));
    EXPECT_NE(std::find_if(payload(manifest).at("attributes").begin(),
                           payload(manifest).at("attributes").end(),
                           [](const Json& attribute) {
                               return attribute.at("key") == "Multi-Release" &&
                                      attribute.at("value") == "true";
                           }),
              payload(manifest).at("attributes").end());

    const auto binaryChunk = call(server,
                                  "class_query",
                                  {{"project_id", projectId},
                                   {"kind", "resource_bytes"},
                                   {"uri", "assets/payload.bin"},
                                   {"offset", 0},
                                   {"limit", 2}});
    ASSERT_FALSE(binaryChunk.at("result").at("isError"));
    EXPECT_EQ(5, payload(binaryChunk).at("size"));
    EXPECT_EQ("AP4=", payload(binaryChunk).at("content"));
    EXPECT_EQ("2", payload(binaryChunk).at("next_cursor"));

    const auto annotation = call(server,
                                 "class_query",
                                 {{"project_id", projectId},
                                  {"kind", "class"},
                                  {"class", "mcp/parity/Annotated"}});
    ASSERT_FALSE(annotation.at("result").at("isError"));
    const auto& annotations = payload(annotation).at("annotations");
    ASSERT_EQ(2U, annotations.size());
    EXPECT_EQ("Lmcp/parity/TestAnnotation;", annotations.at(0).at("descriptor"));
    EXPECT_TRUE(annotations.at(0).at("visible"));
    ASSERT_EQ(2U, annotations.at(0).at("elements").size());
    EXPECT_EQ("Lmcp/parity/NestedAnnotation;",
              annotations.at(0).at("elements").at(1).at("annotation_descriptor"));
    EXPECT_FALSE(annotations.at(1).at("visible"));

    const auto indy = call(server,
                           "class_query",
                           {{"project_id", projectId},
                            {"kind", "bytecode"},
                            {"owner", "mcp/parity/Indy"},
                            {"name", "run"},
                            {"descriptor", "()V"}});
    ASSERT_FALSE(indy.at("result").at("isError"));
    EXPECT_NE(std::find_if(payload(indy).at("instructions").begin(),
                           payload(indy).at("instructions").end(),
                           [](const Json& instruction) {
                               return instruction.at("mnemonic") == "invokedynamic";
                           }),
              payload(indy).at("instructions").end());

    const auto condy = call(server,
                            "class_query",
                            {{"project_id", projectId},
                             {"kind", "jasm"},
                             {"class", "mcp/parity/Condy"}});
    ASSERT_FALSE(condy.at("result").at("isError"));
    EXPECT_EQ("mcp/parity/Condy", payload(condy).at("internal_name"));
    EXPECT_NE(std::string::npos,
              payload(condy).at("jasm").get<std::string>().find("fixtureConstant"));

    const auto frames = call(server,
                             "class_query",
                             {{"project_id", projectId},
                              {"kind", "bytecode"},
                              {"owner", "mcp/parity/FrameSubject"},
                              {"name", "choose"},
                              {"descriptor", "(Z)I"}});
    ASSERT_FALSE(frames.at("result").at("isError"));
    //libjbc DOM 指令序不含 ASM 帧伪节点（StackMapTable 是校验元数据而非指令，
    //帧证据由 jasm 全文承载），指令摘要只列真实指令：iload/ifeq/bipush/goto/
    //bipush/ireturn，分支形态在此足证复杂方法体解析无损。
    EXPECT_EQ(6U, payload(frames).at("instructions").size());
    EXPECT_NE(std::find_if(payload(frames).at("instructions").begin(),
                           payload(frames).at("instructions").end(),
                           [](const Json& instruction) {
                               return instruction.at("mnemonic") == "ifeq";
                           }),
              payload(frames).at("instructions").end());
    EXPECT_NE(std::find_if(payload(frames).at("instructions").begin(),
                           payload(frames).at("instructions").end(),
                           [](const Json& instruction) {
                               return instruction.at("mnemonic") == "goto";
                           }),
              payload(frames).at("instructions").end());

    const auto reflected = call(server,
                                "xref_query",
                                {{"project_id", projectId},
                                 {"kind", "references"},
                                 {"target", "class"},
                                 {"direction", "to"},
                                 {"class", "mcp/parity/ReflectionTarget"}});
    ASSERT_FALSE(reflected.at("result").at("isError"));
    EXPECT_NE(std::find_if(payload(reflected).at("items").begin(),
                           payload(reflected).at("items").end(),
                           [](const Json& reference) {
                               return reference.at("source").at("class") == "mcp/parity/ReflectionUser" &&
                                      reference.at("kind") == "type";
                           }),
              payload(reflected).at("items").end());

    const auto searchable = call(server,
                                 "search",
                                 {{"project_id", projectId},
                                  {"kind", "classes"},
                                  {"query", "FrameSubject"}});
    ASSERT_FALSE(searchable.at("result").at("isError"));
    ASSERT_EQ(1U, payload(searchable).at("items").size());
    EXPECT_EQ("mcp/parity/FrameSubject", payload(searchable).at("items").at(0).at("class"));
    const auto brokenJasm =
        call(server,
             "class_query",
             {{"project_id", projectId}, {"kind", "jasm"}, {"class", "mcp/parity/Broken.class"}});
    EXPECT_TRUE(brokenJasm.at("result").at("isError"));

    //事务 + 导出：mapping 提交为台账事务（edit_apply kind=mapping），导出把
    //编辑态物化为派生容器（project_export）；导出目标被占用按 ALREADY_EXISTS
    //失败且不影响工程状态。jar 修复面（旧 jar_fix_*）随收敛 SDK 化，Broken
    //条目因此随容器原样进入导出产物。
    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/ExportedSubject\n";
    const auto mapped = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "mapping"},
                              {"format", "Tiny-V2"},
                              {"mapping_text", mapping},
                              {"policy", "all"},
                              {"expected_revision", 0}});
    ASSERT_FALSE(mapped.at("result").at("isError")) << mapped;
    EXPECT_TRUE(payload(mapped).at("committed"));
    EXPECT_EQ(1, payload(mapped).at("revision"));

    const auto existingOutput = test::getTempOutPath("mjt_mcp_parity_existing.jar");
    std::ofstream(existingOutput, std::ios::binary | std::ios::trunc) << "existing";
    const auto failedExport = call(server,
                                   "project_export",
                                   {{"project_id", projectId},
                                    {"path", existingOutput.string()}});
    ASSERT_TRUE(failedExport.at("result").at("isError"));
    const auto mappedSubject = call(server,
                                    "class_query",
                                    {{"project_id", projectId},
                                     {"kind", "class"},
                                     {"class", "mcp/ExportedSubject"}});
    ASSERT_FALSE(mappedSubject.at("result").at("isError"));
    EXPECT_EQ("mcp/ExportedSubject", payload(mappedSubject).at("internal_name"));

    const auto exportedFixture = test::getTempOutPath("mjt_mcp_parity_exported.jar");
    std::error_code exportError;
    std::filesystem::remove(exportedFixture, exportError);
    const auto exported = call(server,
                               "project_export",
                               {{"project_id", projectId},
                                {"path", exportedFixture.string()}});
    ASSERT_FALSE(exported.at("result").at("isError"));
    EXPECT_TRUE(payload(exported).at("exported"));
    EXPECT_TRUE(std::filesystem::exists(exportedFixture));

    const auto reopened = call(server, "project_open", {{"path", exportedFixture.string()}});
    ASSERT_FALSE(reopened.at("result").at("isError"));
    const auto exportedProjectId = payload(reopened).at("project_id").get<std::string>();
    const auto exportedResources = call(server,
                                        "search",
                                        {{"project_id", exportedProjectId}, {"kind", "resources"}});
    ASSERT_FALSE(exportedResources.at("result").at("isError"));
    const auto& exportedResourceItems = payload(exportedResources).at("items");
    const auto hasExportedResource = [&exportedResourceItems](const std::string& uri) {
        return std::any_of(exportedResourceItems.begin(),
                           exportedResourceItems.end(),
                           [&uri](const Json& item) { return item.at("uri") == uri; });
    };
    EXPECT_TRUE(hasExportedResource("META-INF/MANIFEST.MF"));
    EXPECT_TRUE(hasExportedResource("META-INF/versions/9/mcp/parity/Versioned.class"));
    EXPECT_TRUE(hasExportedResource("mcp/parity/Annotated.class"));
    EXPECT_TRUE(hasExportedResource("mcp/parity/Condy.class"));
    EXPECT_TRUE(hasExportedResource("mcp/parity/Broken.class"));
    const auto renamed = call(server,
                              "class_query",
                              {{"project_id", exportedProjectId},
                               {"kind", "class"},
                               {"class", "mcp/ExportedSubject"}});
    ASSERT_FALSE(renamed.at("result").at("isError"));
    EXPECT_EQ("mcp/ExportedSubject", payload(renamed).at("internal_name"));
    const auto exportedBinary = call(server,
                                     "class_query",
                                     {{"project_id", exportedProjectId},
                                      {"kind", "resource_bytes"},
                                      {"uri", "assets/payload.bin"},
                                      {"offset", 0},
                                      {"limit", 2}});
    ASSERT_FALSE(exportedBinary.at("result").at("isError"));
    EXPECT_EQ("AP4=", payload(exportedBinary).at("content"));
    const auto exportedManifest = call(server,
                                       "class_query",
                                       {{"project_id", exportedProjectId}, {"kind", "manifest"}});
    ASSERT_FALSE(exportedManifest.at("result").at("isError"));
    EXPECT_NE(std::find_if(payload(exportedManifest).at("attributes").begin(),
                           payload(exportedManifest).at("attributes").end(),
                           [](const Json& attribute) {
                               return attribute.at("key") == "Multi-Release" &&
                                      attribute.at("value") == "true";
                           }),
              payload(exportedManifest).at("attributes").end());
}

}   //namespace
}   //namespace mjt::mcp
