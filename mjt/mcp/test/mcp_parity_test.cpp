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

    const auto opened = call(server, "workspace_open", {{"path", writeParityFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

    const auto resources = call(server, "workspace_resources", {{"workspace_id", workspaceId}});
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
                                  "class_bytes",
                                  {{"workspace_id", workspaceId},
                                   {"class", "mcp/parity/Broken.class"}});
    ASSERT_FALSE(brokenBytes.at("result").at("isError"));
    EXPECT_EQ("mcp/parity/Broken.class", payload(brokenBytes).at("resource_uri"));
    EXPECT_TRUE(payload(brokenBytes).at("internal_name").get<std::string>().empty());
    EXPECT_EQ(3, payload(brokenBytes).at("size"));
    EXPECT_EQ("yv66", payload(brokenBytes).at("content"));

    const auto manifest = call(server, "resource_manifest", {{"workspace_id", workspaceId}});
    ASSERT_FALSE(manifest.at("result").at("isError"));
    EXPECT_NE(std::find_if(payload(manifest).at("attributes").begin(),
                           payload(manifest).at("attributes").end(),
                           [](const Json& attribute) {
                               return attribute.at("key") == "Multi-Release" &&
                                      attribute.at("value") == "true";
                           }),
              payload(manifest).at("attributes").end());

    const auto binaryChunk = call(server,
                                  "resource_read",
                                  {{"workspace_id", workspaceId},
                                   {"uri", "assets/payload.bin"},
                                   {"offset", 0},
                                   {"limit", 2}});
    ASSERT_FALSE(binaryChunk.at("result").at("isError"));
    EXPECT_EQ(5, payload(binaryChunk).at("size"));
    EXPECT_EQ("AP4=", payload(binaryChunk).at("content"));
    EXPECT_EQ("2", payload(binaryChunk).at("next_cursor"));

    const auto annotation = call(server,
                                 "class_get",
                                 {{"workspace_id", workspaceId}, {"class", "mcp/parity/Annotated"}});
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
                           "method_bytecode",
                           {{"workspace_id", workspaceId},
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

    const auto condy =
        call(server, "class_jasm", {{"workspace_id", workspaceId}, {"class", "mcp/parity/Condy"}});
    ASSERT_FALSE(condy.at("result").at("isError"));
    EXPECT_EQ("mcp/parity/Condy", payload(condy).at("internal_name"));
    EXPECT_NE(std::string::npos,
              payload(condy).at("source").get<std::string>().find("fixtureConstant"));

    const auto frames = call(server,
                             "method_bytecode",
                             {{"workspace_id", workspaceId},
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
                                "xref_class",
                                {{"workspace_id", workspaceId}, {"class", "mcp/parity/ReflectionTarget"}});
    ASSERT_FALSE(reflected.at("result").at("isError"));
    EXPECT_NE(std::find_if(payload(reflected).at("items").begin(),
                           payload(reflected).at("items").end(),
                           [](const Json& reference) {
                               return reference.at("source").at("class") == "mcp/parity/ReflectionUser" &&
                                      reference.at("kind") == "type";
                           }),
              payload(reflected).at("items").end());

    const auto searchable = call(server,
                                 "search_classes",
                                 {{"workspace_id", workspaceId}, {"query", "FrameSubject"}});
    ASSERT_FALSE(searchable.at("result").at("isError"));
    ASSERT_EQ(1U, payload(searchable).at("items").size());
    EXPECT_EQ("mcp/parity/FrameSubject", payload(searchable).at("items").at(0).at("owner"));
    const auto brokenJasm =
        call(server,
             "class_jasm",
             {{"workspace_id", workspaceId}, {"class", "mcp/parity/Broken.class"}});
    EXPECT_TRUE(brokenJasm.at("result").at("isError"));

    const auto repairPreview = call(server,
                                    "jar_fix_analyze",
                                    {{"workspace_id", workspaceId},
                                     {"remove_invalid_class_entries", true},
                                     {"expected_revision", 0}});
    ASSERT_FALSE(repairPreview.at("result").at("isError"));
    EXPECT_FALSE(payload(repairPreview).at("committed"));
    EXPECT_EQ(0, payload(repairPreview).at("revision"));
    EXPECT_NE(std::find_if(payload(repairPreview).at("issues").begin(),
                           payload(repairPreview).at("issues").end(),
                           [](const Json& issue) {
                               return issue.at("uri") == "mcp/parity/Broken.class" &&
                                      issue.at("selected_for_removal");
                           }),
              payload(repairPreview).at("issues").end());
    //ReflectionUser 是合法类（字符串反射引用 ReflectionTarget），不受损坏类移除影响；
    //jar_fix 只移除显式选择的损坏条目（不承诺级联移除）。
    EXPECT_EQ(std::find_if(payload(repairPreview).at("issues").begin(),
                           payload(repairPreview).at("issues").end(),
                           [](const Json& issue) {
                               return issue.at("uri") == "mcp/parity/ReflectionUser.class" &&
                                      issue.at("selected_for_removal");
                           }),
              payload(repairPreview).at("issues").end());

    const auto repaired = call(server,
                               "jar_fix_apply",
                               {{"workspace_id", workspaceId},
                                {"remove_invalid_class_entries", true},
                                {"expected_revision", 0}});
    ASSERT_FALSE(repaired.at("result").at("isError"));
    EXPECT_TRUE(payload(repaired).at("committed"));
    EXPECT_EQ(1, payload(repaired).at("revision"));
    const auto removed = call(server,
                              "resource_read",
                              {{"workspace_id", workspaceId}, {"uri", "mcp/parity/Broken.class"}});
    EXPECT_TRUE(removed.at("result").at("isError"));
    const auto preservedReflection = call(
        server,
        "resource_read",
        {{"workspace_id", workspaceId}, {"uri", "mcp/parity/ReflectionUser.class"}});
    EXPECT_FALSE(preservedReflection.at("result").at("isError"));

    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/ExportedSubject\n";
    const auto existingOutput = test::getTempOutPath("mjt_mcp_parity_existing.jar");
    std::ofstream(existingOutput, std::ios::binary | std::ios::trunc) << "existing";
    const auto failedWorkflow = call(server,
                                     "analysis_workflow",
                                     {{"workspace_id", workspaceId},
                                      {"mapping_format", "Tiny-V2"},
                                      {"mapping_text", mapping},
                                      {"mapping_policy", "all"},
                                      {"export_path", existingOutput.string()},
                                      {"expected_revision", 1}});
    ASSERT_TRUE(failedWorkflow.at("result").at("isError"));
    EXPECT_EQ(1, payload(failedWorkflow).at("mutation").at("revision"));
    EXPECT_NE(std::find_if(payload(failedWorkflow).at("stages").begin(),
                           payload(failedWorkflow).at("stages").end(),
                           [](const Json& stage) { return stage.at("name") == "export"; }),
              payload(failedWorkflow).at("stages").end());
    const auto original = call(server,
                               "class_get",
                               {{"workspace_id", workspaceId}, {"class", "mcp/Subject"}});
    ASSERT_FALSE(original.at("result").at("isError"));
    EXPECT_EQ("mcp/Subject", payload(original).at("internal_name"));

    const auto exportedFixture = test::getTempOutPath("mjt_mcp_parity_exported.jar");
    std::error_code exportError;
    std::filesystem::remove(exportedFixture, exportError);
    const auto exported = call(server,
                               "analysis_workflow",
                               {{"workspace_id", workspaceId},
                                {"mapping_format", "Tiny-V2"},
                                {"mapping_text", mapping},
                                {"mapping_policy", "all"},
                                {"export_path", exportedFixture.string()},
                                {"expected_revision", 1}});
    ASSERT_FALSE(exported.at("result").at("isError"));
    EXPECT_TRUE(payload(exported).at("mutation").at("committed"));
    EXPECT_EQ(2, payload(exported).at("mutation").at("revision"));
    EXPECT_TRUE(payload(exported).at("exported"));
    EXPECT_TRUE(std::filesystem::exists(exportedFixture));

    const auto reopened = call(server, "workspace_open", {{"path", exportedFixture.string()}});
    ASSERT_FALSE(reopened.at("result").at("isError"));
    const auto exportedWorkspaceId = payload(reopened).at("workspace_id").get<std::string>();
    const auto exportedResources = call(server,
                                        "workspace_resources",
                                        {{"workspace_id", exportedWorkspaceId}});
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
    EXPECT_FALSE(hasExportedResource("mcp/parity/Broken.class"));
    const auto renamed = call(server,
                              "class_get",
                              {{"workspace_id", exportedWorkspaceId}, {"class", "mcp/ExportedSubject"}});
    ASSERT_FALSE(renamed.at("result").at("isError"));
    EXPECT_EQ("mcp/ExportedSubject", payload(renamed).at("internal_name"));
    const auto exportedBinary = call(server,
                                     "resource_read",
                                     {{"workspace_id", exportedWorkspaceId},
                                      {"uri", "assets/payload.bin"},
                                      {"offset", 0},
                                      {"limit", 2}});
    ASSERT_FALSE(exportedBinary.at("result").at("isError"));
    EXPECT_EQ("AP4=", payload(exportedBinary).at("content"));
    const auto exportedManifest = call(server,
                                       "resource_manifest",
                                       {{"workspace_id", exportedWorkspaceId}});
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
