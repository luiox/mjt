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

std::vector<ca::u8> readFileBytes(const std::string& path)
{
    std::vector<ca::u8> bytes;
    std::ifstream       in(path, std::ios::binary);
    char                buf[4096];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
        bytes.insert(bytes.end(), buf, buf + in.gcount());
    }
    return bytes;
}

TEST(McpServerTest, ExposesWorkspaceQueriesAndTransactionalJasm)
{
    McpSessionHarness server;

    const auto& initialize = server.initializeResponse();
    EXPECT_EQ("mjt-mcp", initialize.at("result").at("serverInfo").at("name"));

    const auto tools = server.handle(request("tools/list"));
    const auto listed = tools.at("result").at("tools");
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "patch_apply_jasm";
    }));
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "patch_replace_class_bytes";
    }));
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "workspace_modified_classes";
    }));
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "workspace_revert_class";
    }));
    const auto methodJasm = std::find_if(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "method_jasm";
    });
    ASSERT_NE(methodJasm, listed.end());
    EXPECT_EQ(Json::array({"workspace_id", "owner", "name", "descriptor"}),
              methodJasm->at("inputSchema").at("required"));
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "mapping_apply";
    }));
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "decompiler_compare";
    }));
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "opt_apply";
    }));
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "analysis_evaluate";
    }));
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "analysis_workflow";
    }));
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "job_cancel";
    }));
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "jar_diff";
    }));
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "jar_fix_apply";
    }));
    const auto searchInstructions = std::find_if(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "search_instructions";
    });
    ASSERT_NE(searchInstructions, listed.end());
    EXPECT_EQ(Json::array({"workspace_id", "query"}),
              searchInstructions->at("inputSchema").at("required"));

    const auto backends = call(server, "decompiler_list", Json::object());
    ASSERT_FALSE(backends.at("result").at("isError"));
    EXPECT_EQ(2U, payload(backends).at("backends").size());

    const auto passes = call(server, "pass_describe", {{"name", "NopRemover"}, {"kind", "pass"}});
    ASSERT_FALSE(passes.at("result").at("isError"));
    ASSERT_EQ(1U, payload(passes).at("passes").size());
    EXPECT_EQ("method", payload(passes).at("passes").at(0).at("scope"));

    const auto evaluation = call(server,
                                 "analysis_evaluate",
                                 {{"expression", "(x ^ y) + 2 * (x & y)"},
                                  {"bindings", {{"x", 4}, {"y", 3}}},
                                  {"simplify", true}});
    ASSERT_FALSE(evaluation.at("result").at("isError"));
    EXPECT_TRUE(payload(evaluation).at("evaluated"));
    EXPECT_EQ(7, payload(evaluation).at("value"));

    const auto unbound = call(server, "analysis_evaluate", {{"expression", "x + 1"}});
    ASSERT_FALSE(unbound.at("result").at("isError"));
    EXPECT_FALSE(payload(unbound).at("evaluated"));
    EXPECT_EQ("(+ (var x:i32) (const 1:i32))", payload(unbound).at("normalized"));
    EXPECT_NE(std::string::npos, payload(unbound).at("error").get<std::string>().find("unbound variable"));

    const Json oversizedArguments = {{"expression", "x"},
                                     {"bindings", {{"x", std::numeric_limits<ca::u64>::max()}}}};
    const auto oversizedBinding = call(server, "analysis_evaluate", oversizedArguments);
    ASSERT_TRUE(oversizedBinding.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(oversizedBinding).at("error").get<std::string>().find("must be an integer"))
        << payload(oversizedBinding).dump();

    const auto fixture = writeFixture();
    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto fakeDecompiler = test::getTempOutPath("fake-mcp-decompiler.jar");
    std::ofstream(fakeDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");

    const auto settingsSet = call(server,
                                  "decompiler_settings_set",
                                  {{"backend", "vineflower"},
                                   {"cfr_jar", fakeDecompiler.string()},
                                   {"vineflower_jar", fakeDecompiler.string()},
                                   {"java", fakeJava.string()},
                                   {"timeout_ms", 5000},
                                   {"max_memory_mib", 256},
                                   {"max_source_bytes", 1024 * 1024}});
    ASSERT_FALSE(settingsSet.at("result").at("isError"));
    EXPECT_EQ("vineflower", payload(settingsSet).at("settings").at("backend"));
    ASSERT_EQ(2U, payload(settingsSet).at("backends").size());
    EXPECT_TRUE(payload(settingsSet).at("backends").at(0).at("available"));
    EXPECT_TRUE(payload(settingsSet).at("backends").at(1).at("available"));
    const auto settingsGet = call(server, "decompiler_settings_get", Json::object());
    ASSERT_FALSE(settingsGet.at("result").at("isError"));
    EXPECT_EQ(256, payload(settingsGet).at("settings").at("max_memory_mib"));
    const auto invalidSettings = call(server,
                                      "decompiler_settings_set",
                                      {{"max_source_bytes", 0}});
    EXPECT_TRUE(invalidSettings.at("result").at("isError"));
    const auto settingsAfterInvalid = call(server, "decompiler_settings_get", Json::object());
    ASSERT_FALSE(settingsAfterInvalid.at("result").at("isError"));
    EXPECT_EQ(1024 * 1024,
              payload(settingsAfterInvalid).at("settings").at("max_source_bytes"));

    const std::string workflowMapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                        "c\tmcp/Subject\tmcp/WorkflowSubject\n";
    const auto workflowOutput = test::getTempOutPath("mjt_mcp_workflow_output.jar");
    std::error_code workflowOutputError;
    std::filesystem::remove(workflowOutput, workflowOutputError);
    const auto conflictingWorkflowDecompile = call(
        server,
        "analysis_workflow",
        {{"workspace_id", workspaceId},
         {"decompile_class", "mcp/Subject"},
         {"decompile_classes", Json::array({"mcp/Second"})}});
    ASSERT_TRUE(conflictingWorkflowDecompile.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(conflictingWorkflowDecompile).at("error").get<std::string>().find("mutually exclusive"));

    const auto workflowPreview = call(
        server,
        "analysis_workflow",
        {{"workspace_id", workspaceId},
         {"mapping_format", "Tiny-V2"},
         {"mapping_text", workflowMapping},
         {"mapping_policy", "all"},
         {"decompile_classes", Json::array({"mcp/WorkflowSubject", "mcp/Second"})},
         {"decompiler_jar", fakeDecompiler.string()},
         {"java", fakeJava.string()},
         {"export_path", workflowOutput.string()},
         {"references", Json::array({{{"kind", "class"}, {"owner", "mcp/Subject"}}})},
         {"expected_revision", 0},
         {"dry_run", true}});
    ASSERT_FALSE(workflowPreview.at("result").at("isError"));
    EXPECT_FALSE(payload(workflowPreview).at("mutation").at("committed"));
    EXPECT_EQ(0, payload(workflowPreview).at("mutation").at("revision"));
    EXPECT_FALSE(payload(workflowPreview).at("input_fingerprint").get<std::string>().empty());
    EXPECT_NE(payload(workflowPreview).at("input_fingerprint"),
              payload(workflowPreview).at("candidate_fingerprint"));
    EXPECT_EQ(std::filesystem::absolute(workflowOutput).string(),
              payload(workflowPreview).at("export_path"));
    EXPECT_FALSE(payload(workflowPreview).at("exported"));
    EXPECT_FALSE(std::filesystem::exists(workflowOutput));
    ASSERT_EQ(2U, payload(workflowPreview).at("decompiled_sources").size());
    EXPECT_EQ("mcp/WorkflowSubject",
              payload(workflowPreview).at("decompiled_sources").at(0).at("internal_name"));
    EXPECT_NE(std::string::npos,
              payload(workflowPreview).at("decompiled_sources").at(0).at("source").get<std::string>().find("WorkflowSubject"));
    EXPECT_EQ("mcp/Second",
              payload(workflowPreview).at("decompiled_sources").at(1).at("internal_name"));
    EXPECT_FALSE(payload(workflowPreview).at("stages").empty());
    ASSERT_EQ(1U, payload(workflowPreview).at("references").size());

    const auto classSearch = call(server,
                                  "search_classes",
                                  {{"workspace_id", workspaceId}, {"query", "subject"}});
    ASSERT_FALSE(classSearch.at("result").at("isError"));
    ASSERT_EQ(1U, payload(classSearch).at("items").size());
    EXPECT_EQ("class", payload(classSearch).at("items").at(0).at("kind"));
    EXPECT_EQ("mcp/Subject", payload(classSearch).at("items").at(0).at("owner"));

    const auto memberSearch = call(server,
                                   "search_members",
                                   {{"workspace_id", workspaceId}, {"query", "counter"}});
    ASSERT_FALSE(memberSearch.at("result").at("isError"));
    ASSERT_EQ(2U, payload(memberSearch).at("items").size());
    EXPECT_EQ("field", payload(memberSearch).at("items").at(0).at("kind"));

    const auto stringSearch = call(server,
                                   "search_strings",
                                   {{"workspace_id", workspaceId}, {"query", "mcp/Subject"}});
    ASSERT_FALSE(stringSearch.at("result").at("isError"));
    EXPECT_FALSE(payload(stringSearch).at("items").empty());

    const auto numberSearch = call(server,
                                   "search_numbers",
                                   {{"workspace_id", workspaceId}, {"query", "1.0"}});
    ASSERT_FALSE(numberSearch.at("result").at("isError"));
    EXPECT_FALSE(payload(numberSearch).at("items").empty());

    const auto instructionSearch = call(server,
                                        "search_instructions",
                                        {{"workspace_id", workspaceId}, {"query", "ldc"}});
    ASSERT_FALSE(instructionSearch.at("result").at("isError"));
    EXPECT_FALSE(payload(instructionSearch).at("items").empty());

    const auto workspaceDiff = call(server,
                                    "jar_diff",
                                    {{"workspace_id", workspaceId}, {"path", fixture.string()}});
    ASSERT_FALSE(workspaceDiff.at("result").at("isError"));
    EXPECT_EQ(0, payload(workspaceDiff).at("workspace_only"));
    EXPECT_EQ(0, payload(workspaceDiff).at("reference_only"));
    EXPECT_EQ(0, payload(workspaceDiff).at("modified"));

    const auto classes = call(server, "class_list", {{"workspace_id", workspaceId}});
    ASSERT_FALSE(classes.at("result").at("isError"));
    ASSERT_EQ(2U, payload(classes).at("items").size());
    const auto& firstClass = payload(classes).at("items").at(0);
    EXPECT_TRUE(firstClass.contains("resource_uri"));
    EXPECT_TRUE(firstClass.contains("internal_name"));
    EXPECT_TRUE(firstClass.contains("super_name"));
    EXPECT_TRUE(firstClass.contains("field_count"));
    EXPECT_TRUE(firstClass.contains("method_count"));
    EXPECT_FALSE(firstClass.contains("signature"));
    EXPECT_FALSE(firstClass.contains("fields"));
    EXPECT_FALSE(firstClass.contains("methods"));
    EXPECT_FALSE(firstClass.contains("annotations"));
    EXPECT_NE(std::find_if(payload(classes).at("items").begin(),
                           payload(classes).at("items").end(),
                           [](const Json& item) { return item.at("internal_name") == "mcp/Subject"; }),
              payload(classes).at("items").end());

    const auto classBytes = call(server,
                                 "class_bytes",
                                 {{"workspace_id", workspaceId},
                                  {"class", "mcp/Subject"},
                                  {"limit", 4}});
    ASSERT_FALSE(classBytes.at("result").at("isError"));
    EXPECT_EQ("mcp/Subject.class", payload(classBytes).at("resource_uri"));
    EXPECT_EQ("mcp/Subject", payload(classBytes).at("internal_name"));
    EXPECT_EQ(0U,
              payload(classBytes).at("fingerprint").get<std::string>().find("fnv1a64:"));
    EXPECT_EQ(0, payload(classBytes).at("offset"));
    EXPECT_EQ("4", payload(classBytes).at("next_cursor"));
    EXPECT_EQ("yv66vg==", payload(classBytes).at("content"));

    const auto supportPath = writeQuerySupportingFixture();
    const auto supportAdded = call(server,
                                   "workspace_add_support",
                                   {{"workspace_id", workspaceId},
                                    {"path", supportPath.string()}});
    ASSERT_FALSE(supportAdded.at("result").at("isError"));
    const auto supportId = payload(supportAdded)
                               .at("supporting_resource")
                               .at("id")
                               .get<std::string>();
    EXPECT_EQ("support-1", supportId);
    EXPECT_EQ(0U,
              payload(supportAdded)
                  .at("supporting_resource")
                  .at("fingerprint")
                  .get<std::string>()
                  .find("fnv1a64:"));
    const auto supportList = call(server,
                                  "workspace_list_support",
                                  {{"workspace_id", workspaceId}});
    ASSERT_FALSE(supportList.at("result").at("isError"));
    ASSERT_EQ(1U, payload(supportList).at("items").size());
    EXPECT_EQ(supportId, payload(supportList).at("items").at(0).at("id"));

    const auto callers = call(server,
                              "method_callers",
                              {{"workspace_id", workspaceId},
                               {"owner", "mcp/Subject"},
                               {"name", "value"},
                               {"descriptor", "()I"}});
    ASSERT_FALSE(callers.at("result").at("isError"));
    ASSERT_EQ(1U, payload(callers).at("items").size());
    EXPECT_EQ("support/McpLibrary",
              payload(callers).at("items").at(0).at("source").at("class"));
    EXPECT_TRUE(payload(callers).at("items").at(0).at("source").at("supporting"));
    EXPECT_EQ(supportId,
              payload(callers).at("items").at(0).at("source").at("support_id"));
    const auto callees = call(server,
                              "method_callees",
                              {{"workspace_id", workspaceId},
                               {"owner", "support/McpLibrary"},
                               {"name", "call"},
                               {"descriptor", "()I"}});
    ASSERT_FALSE(callees.at("result").at("isError"));
    ASSERT_EQ(1U, payload(callees).at("items").size());
    EXPECT_TRUE(payload(callees).at("items").at(0).at("source").at("supporting"));
    const auto relations = call(server,
                                "class_relations",
                                {{"workspace_id", workspaceId},
                                 {"class", "mcp/Subject"}});
    ASSERT_FALSE(relations.at("result").at("isError"));
    EXPECT_NE(payload(relations).at("all_subclasses").end(),
              std::find(payload(relations).at("all_subclasses").begin(),
                        payload(relations).at("all_subclasses").end(),
                        Json("support/McpLibrary")));
    const auto supportSource = std::find_if(
        payload(relations).at("sources").begin(),
        payload(relations).at("sources").end(),
        [](const Json& source) { return source.at("internal_name") == "support/McpLibrary"; });
    ASSERT_NE(supportSource, payload(relations).at("sources").end());
    EXPECT_TRUE(supportSource->at("supporting"));
    EXPECT_EQ(supportId, supportSource->at("support_id"));

    const auto exactSearch = call(server,
                                  "search_classes",
                                  {{"workspace_id", workspaceId},
                                   {"query", "mcp/Subject"},
                                   {"match_mode", "exact"}});
    ASSERT_FALSE(exactSearch.at("result").at("isError"));
    ASSERT_EQ(1U, payload(exactSearch).at("items").size());
    const auto regexSearch = call(server,
                                  "search_classes",
                                  {{"workspace_id", workspaceId},
                                   {"query", "^mcp/(Subject|Second)$"},
                                   {"match_mode", "regex"},
                                   {"case_sensitive", true},
                                   {"package_prefix", "mcp/"}});
    ASSERT_FALSE(regexSearch.at("result").at("isError"));
    EXPECT_EQ(2U, payload(regexSearch).at("items").size());
    const auto memberFilter = call(server,
                                   "search_members",
                                   {{"workspace_id", workspaceId},
                                    {"query", "counter"},
                                    {"owner", "mcp/Subject"},
                                    {"member_kind", "field"}});
    ASSERT_FALSE(memberFilter.at("result").at("isError"));
    ASSERT_EQ(1U, payload(memberFilter).at("items").size());
    const auto opcodeFilter = call(server,
                                   "search_instructions",
                                   {{"workspace_id", workspaceId},
                                    {"query", "ldc"},
                                    {"owner", "mcp/Subject"},
                                    {"opcode", 18}});
    ASSERT_FALSE(opcodeFilter.at("result").at("isError"));
    ASSERT_EQ(1U, payload(opcodeFilter).at("items").size());
    const auto invalidRegex = call(server,
                                   "search_classes",
                                   {{"workspace_id", workspaceId},
                                    {"query", "["},
                                    {"match_mode", "regex"}});
    EXPECT_TRUE(invalidRegex.at("result").at("isError"));
    const auto duplicateSupport = call(server,
                                       "workspace_add_support",
                                       {{"workspace_id", workspaceId},
                                        {"path", supportPath.string()}});
    EXPECT_TRUE(duplicateSupport.at("result").at("isError"));
    const auto supportRemoved = call(server,
                                     "workspace_remove_support",
                                     {{"workspace_id", workspaceId},
                                      {"support_id", supportId}});
    ASSERT_FALSE(supportRemoved.at("result").at("isError"));
    EXPECT_EQ(supportId, payload(supportRemoved).at("removed").at("id"));
    const auto emptySupportList = call(server,
                                       "workspace_list_support",
                                       {{"workspace_id", workspaceId}});
    ASSERT_FALSE(emptySupportList.at("result").at("isError"));
    EXPECT_TRUE(payload(emptySupportList).at("items").empty());
    const auto callersAfterRemove = call(server,
                                         "method_callers",
                                         {{"workspace_id", workspaceId},
                                          {"owner", "mcp/Subject"},
                                          {"name", "value"},
                                          {"descriptor", "()I"}});
    ASSERT_FALSE(callersAfterRemove.at("result").at("isError"));
    EXPECT_TRUE(payload(callersAfterRemove).at("items").empty());

    const auto resource = call(server,
                               "resource_read",
                               {{"workspace_id", workspaceId}, {"uri", "notice.txt"}, {"limit", 4}});
    ASSERT_FALSE(resource.at("result").at("isError"));
    EXPECT_EQ(0, payload(resource).at("offset"));
    EXPECT_EQ("4", payload(resource).at("next_cursor"));

    const auto method = call(server,
                             "method_jasm",
                             {{"workspace_id", workspaceId},
                              {"owner", "mcp/Subject"},
                              {"name", "value"},
                              {"descriptor", "()I"}});
    ASSERT_FALSE(method.at("result").at("isError"));
    EXPECT_NE(std::string::npos, payload(method).at("source").get<std::string>().find("value ()I"));

    const auto source = call(server,
                             "class_source",
                             {{"workspace_id", workspaceId},
                              {"class", "mcp/Subject"}});
    ASSERT_FALSE(source.at("result").at("isError"));
    EXPECT_EQ("class", payload(source).at("source_scope"));
    EXPECT_EQ("vineflower", payload(source).at("backend"));
    EXPECT_NE(std::string::npos,
              payload(source).at("source").get<std::string>().find("public class Subject"));
    const auto limitedSource = call(server,
                                    "class_source",
                                    {{"workspace_id", workspaceId},
                                     {"class", "mcp/Subject"},
                                     {"max_source_bytes", 16}});
    EXPECT_TRUE(limitedSource.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(limitedSource).at("status").at("message").get<std::string>().find("16 bytes"));

    const auto methodSource = call(server,
                                   "method_source",
                                   {{"workspace_id", workspaceId},
                                    {"owner", "mcp/Subject"},
                                    {"name", "value"},
                                    {"descriptor", "()I"}});
    ASSERT_FALSE(methodSource.at("result").at("isError"));
    EXPECT_EQ("class", payload(methodSource).at("source_scope"));

    const auto compared = call(server,
                               "decompiler_compare",
                               {{"workspace_id", workspaceId},
                                {"class", "mcp/Subject"}});
    ASSERT_FALSE(compared.at("result").at("isError"));
    ASSERT_EQ(2U, payload(compared).at("sources").size());
    EXPECT_EQ("cfr", payload(compared).at("sources").at(0).at("backend"));
    EXPECT_EQ("vineflower", payload(compared).at("sources").at(1).at("backend"));

    const auto asyncSource = call(server,
                                  "class_source",
                                  {{"workspace_id", workspaceId},
                                   {"class", "mcp/Subject"},
                                   {"async", true}});
    ASSERT_FALSE(asyncSource.at("result").at("isError"));
    const auto asyncJobId = payload(asyncSource).at("job_id").get<std::string>();
    Json       completedJob;
    for (int attempt = 0; attempt < 100; ++attempt) {
        completedJob = call(server, "job_get", {{"job_id", asyncJobId}});
        ASSERT_FALSE(completedJob.at("result").at("isError"));
        if (payload(completedJob).at("state") == "succeeded") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ("succeeded", payload(completedJob).at("state"));
    EXPECT_NE(std::string::npos,
              payload(completedJob).at("result").at("source").get<std::string>().find("public class Subject"));
    const auto releasedJob = call(server, "job_release", {{"job_id", asyncJobId}});
    ASSERT_FALSE(releasedJob.at("result").at("isError"));
    EXPECT_TRUE(payload(releasedJob).at("released"));

    const auto slowDecompiler = test::getTempOutPath("fake-mcp-decompiler-slow.jar");
    std::ofstream(slowDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto slowSource = call(server,
                                 "class_source",
                                 {{"workspace_id", workspaceId},
                                  {"class", "mcp/Subject"},
                                  {"decompiler_jar", slowDecompiler.string()},
                                  {"java", fakeJava.string()},
                                  {"async", true}});
    ASSERT_FALSE(slowSource.at("result").at("isError"));
    const auto slowJobId = payload(slowSource).at("job_id").get<std::string>();
    const auto cancelled = call(server, "job_cancel", {{"job_id", slowJobId}});
    ASSERT_FALSE(cancelled.at("result").at("isError"));
    EXPECT_TRUE(payload(cancelled).at("cancel_requested"));
    Json cancelledJob;
    for (int attempt = 0; attempt < 100; ++attempt) {
        cancelledJob = call(server, "job_get", {{"job_id", slowJobId}});
        ASSERT_FALSE(cancelledJob.at("result").at("isError"));
        if (payload(cancelledJob).at("state") == "cancelled") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ("cancelled", payload(cancelledJob).at("state"));
    EXPECT_TRUE(payload(cancelledJob).at("result_is_error"));
    EXPECT_EQ(static_cast<int>(ca::StatusCode::CANCELLED),
              payload(cancelledJob).at("result").at("status").at("code"));
    const auto releasedCancelledJob = call(server, "job_release", {{"job_id", slowJobId}});
    ASSERT_FALSE(releasedCancelledJob.at("result").at("isError"));
    EXPECT_TRUE(payload(releasedCancelledJob).at("released"));

    const auto slowSettings = call(server,
                                   "decompiler_settings_set",
                                   {{"backend", "cfr"},
                                    {"cfr_jar", slowDecompiler.string()},
                                    {"java", fakeJava.string()},
                                    {"timeout_ms", 5000}});
    ASSERT_FALSE(slowSettings.at("result").at("isError"));
    const auto immutableJob = call(server,
                                   "class_source",
                                   {{"workspace_id", workspaceId},
                                    {"class", "mcp/Subject"},
                                    {"async", true}});
    ASSERT_FALSE(immutableJob.at("result").at("isError"));
    const auto immutableJobId = payload(immutableJob).at("job_id").get<std::string>();
    const auto failedDecompiler = test::getTempOutPath("fake-mcp-decompiler-fail.jar");
    std::ofstream(failedDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto changedSettings = call(server,
                                      "decompiler_settings_set",
                                      {{"cfr_jar", failedDecompiler.string()}});
    ASSERT_FALSE(changedSettings.at("result").at("isError"));
    const auto immutableCompleted = waitForTerminalJob(server, immutableJobId);
    ASSERT_FALSE(immutableCompleted.at("result").at("isError"));
    EXPECT_EQ("succeeded", payload(immutableCompleted).at("state"));
    EXPECT_EQ(slowDecompiler.string(),
              payload(immutableCompleted).at("result").at("decompiler_jar"));
    ASSERT_FALSE(call(server, "job_release", {{"job_id", immutableJobId}})
                     .at("result")
                     .at("isError"));
    ASSERT_FALSE(call(server,
                      "decompiler_settings_set",
                      {{"backend", "vineflower"},
                       {"cfr_jar", fakeDecompiler.string()},
                       {"vineflower_jar", fakeDecompiler.string()}})
                     .at("result")
                     .at("isError"));

    const auto preview = call(server,
                              "patch_validate_jasm",
                              {{"workspace_id", workspaceId},
                               {"uri", "mcp/Subject.class"},
                               {"source_name", "Subject.jasm"},
                               {"source", classSource("42")},
                               {"expected_revision", 0},
                               {"dry_run", true}});
    ASSERT_FALSE(preview.at("result").at("isError"));
    EXPECT_FALSE(payload(preview).at("committed"));
    EXPECT_EQ(0, payload(preview).at("revision"));
    EXPECT_FALSE(payload(preview).at("byte_diffs").empty());

    const auto applied = call(server,
                              "patch_apply_jasm",
                              {{"workspace_id", workspaceId},
                               {"uri", "mcp/Subject.class"},
                               {"source_name", "Subject.jasm"},
                               {"source", classSource("42")},
                               {"expected_revision", 0}});
    ASSERT_FALSE(applied.at("result").at("isError"));
    EXPECT_TRUE(payload(applied).at("committed"));
    EXPECT_EQ(1, payload(applied).at("revision"));

    const auto invalidPass = call(server,
                                  "pass_run",
                                  {{"workspace_id", workspaceId},
                                   {"passes", Json::array({"DefinitelyNotAPass"})},
                                   {"expected_revision", 1}});
    EXPECT_TRUE(invalidPass.at("result").at("isError"));
    EXPECT_EQ(1, payload(invalidPass).at("revision"));

    const auto optimizationPreview = call(server,
                                          "opt_preview",
                                          {{"workspace_id", workspaceId},
                                           {"expected_revision", 1},
                                           {"label", "preview default optimization"}});
    ASSERT_FALSE(optimizationPreview.at("result").at("isError"));
    EXPECT_FALSE(payload(optimizationPreview).at("committed"));
    EXPECT_EQ(1, payload(optimizationPreview).at("revision"));

    const auto rename = call(server,
                             "rename_analyze",
                             {{"workspace_id", workspaceId},
                              {"kind", "field"},
                              {"owner", "mcp/Subject"},
                              {"name", "counter"},
                              {"descriptor", "I"},
                              {"new_name", "count"}});
    ASSERT_FALSE(rename.at("result").at("isError"));
    EXPECT_TRUE(payload(rename).at("ready_to_apply"));
    EXPECT_FALSE(payload(rename).at("mappings").empty());

    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/RenamedSubject\n";
    const auto mappingPreview = call(server,
                                     "mapping_analyze",
                                     {{"workspace_id", workspaceId},
                                      {"format", "Tiny-V2"},
                                      {"mapping_text", mapping},
                                      {"policy", "all"}});
    ASSERT_FALSE(mappingPreview.at("result").at("isError"));
    EXPECT_TRUE(payload(mappingPreview).at("exportable"));
    EXPECT_FALSE(payload(mappingPreview).at("byte_diffs").empty());

    const auto mappingApplied = call(server,
                                     "mapping_apply",
                                     {{"workspace_id", workspaceId},
                                      {"format", "Tiny-V2"},
                                      {"mapping_text", mapping},
                                      {"policy", "all"},
                                      {"expected_revision", 1},
                                      {"label", "rename subject with mapping"}});
    ASSERT_FALSE(mappingApplied.at("result").at("isError"));
    EXPECT_TRUE(payload(mappingApplied).at("committed"));
    EXPECT_EQ(2, payload(mappingApplied).at("revision"));

    const auto repairPreview = call(server,
                                    "jar_fix_analyze",
                                    {{"workspace_id", workspaceId},
                                     {"remove_invalid_class_entries", true},
                                     {"expected_revision", 2}});
    ASSERT_FALSE(repairPreview.at("result").at("isError"));
    EXPECT_FALSE(payload(repairPreview).at("committed"));
    EXPECT_FALSE(payload(repairPreview).at("byte_diffs").empty());
    ASSERT_EQ(1U, payload(repairPreview).at("issues").size());
    EXPECT_TRUE(payload(repairPreview).at("issues").at(0).at("selected_for_removal"));

    const auto repairApplied = call(server,
                                    "jar_fix_apply",
                                    {{"workspace_id", workspaceId},
                                     {"remove_invalid_class_entries", true},
                                     {"expected_revision", 2}});
    ASSERT_FALSE(repairApplied.at("result").at("isError"));
    EXPECT_TRUE(payload(repairApplied).at("committed"));
    EXPECT_EQ(3, payload(repairApplied).at("revision"));
    const auto removed = call(server,
                              "resource_read",
                              {{"workspace_id", workspaceId}, {"uri", "broken/Invalid.class"}});
    EXPECT_TRUE(removed.at("result").at("isError"));

    const auto renamed = call(server,
                              "class_get",
                              {{"workspace_id", workspaceId}, {"class", "mcp/RenamedSubject"}});
    ASSERT_FALSE(renamed.at("result").at("isError"));
    EXPECT_EQ("mcp/RenamedSubject", payload(renamed).at("internal_name"));
}

TEST(McpServerTest, KeepsLargeClassListResponsesBounded)
{
    const std::string signature =
        "<T" + std::string(65000, 'A') + ":Ljava/lang/Object;>Ljava/lang/Object;";
    McpSessionHarness server;
    const auto        opened = call(
        server,
        "workspace_open",
        {{"path", writeLargeClassListFixture(signature).string()}, {"workspace_id", "large-list"}});
    ASSERT_FALSE(opened.at("result").at("isError"));

    const auto classes =
        call(server, "class_list", {{"workspace_id", "large-list"}, {"limit", 200}});
    ASSERT_FALSE(classes.at("result").at("isError"));
    ASSERT_EQ(200U, payload(classes).at("items").size());
    EXPECT_EQ(210U, payload(classes).at("total"));
    EXPECT_EQ("200", payload(classes).at("next_cursor"));
    EXPECT_LT(classes.dump().size(), 1024U * 1024U);
    for (const auto& item : payload(classes).at("items")) {
        EXPECT_FALSE(item.contains("signature"));
        EXPECT_FALSE(item.contains("fields"));
        EXPECT_FALSE(item.contains("methods"));
        EXPECT_FALSE(item.contains("annotations"));
    }

    const auto remaining = call(
        server, "class_list", {{"workspace_id", "large-list"}, {"cursor", "200"}, {"limit", 200}});
    ASSERT_FALSE(remaining.at("result").at("isError"));
    EXPECT_EQ(10U, payload(remaining).at("items").size());
    EXPECT_FALSE(payload(remaining).contains("next_cursor"));

    const auto detail = call(
        server, "class_get", {{"workspace_id", "large-list"}, {"class", "mcp/large/Subject0"}});
    ASSERT_FALSE(detail.at("result").at("isError"));
    EXPECT_EQ(signature, payload(detail).at("signature"));
}

//验证 parseCursor 对越界 cursor 的退化（S3）：合法但远超 total 的 cursor
//经 parseCursor clamp 到 total，返回空尾页而非抛异常。该行为覆盖 page 与
//classSummaryPage 共用的 parseCursor 路径。
TEST(McpServerTest, ReturnsEmptyPageWhenCursorExceedsTotal)
{
    const std::string signature =
        "<T" + std::string(65000, 'A') + ":Ljava/lang/Object;>Ljava/lang/Object;";
    McpSessionHarness server;
    const auto        opened = call(
        server,
        "workspace_open",
        {{"path", writeLargeClassListFixture(signature).string()}, {"workspace_id", "cursor-bound"}});
    ASSERT_FALSE(opened.at("result").at("isError"));

    //cursor 远超 total(210)：parseCursor clamp 到 210，items 为空、无 next_cursor。
    const auto pastEnd =
        call(server, "class_list", {{"workspace_id", "cursor-bound"}, {"cursor", "999999"}});
    ASSERT_FALSE(pastEnd.at("result").at("isError"));
    EXPECT_EQ(0U, payload(pastEnd).at("items").size());
    EXPECT_EQ(210U, payload(pastEnd).at("total"));
    EXPECT_EQ(210U, payload(pastEnd).at("offset"));
    EXPECT_FALSE(payload(pastEnd).contains("next_cursor"));
}

TEST(McpServerTest, ReplacesRevertsAndListsModifiedClassesThroughMcp)
{
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto original = assembleFixtureClass("mcp/Subject", classSource("1"));
    const auto originalBase64 = encodeBase64(original);

    const auto changed = call(server,
                              "patch_apply_jasm",
                              {{"workspace_id", workspaceId},
                               {"uri", "mcp/Subject.class"},
                               {"source", classSource("42")},
                               {"expected_revision", 0}});
    ASSERT_FALSE(changed.at("result").at("isError"));
    EXPECT_EQ(1, payload(changed).at("revision"));

    const auto modified = call(server,
                               "workspace_modified_classes",
                               {{"workspace_id", workspaceId}});
    ASSERT_FALSE(modified.at("result").at("isError"));
    ASSERT_EQ(1U, payload(modified).at("items").size());
    EXPECT_EQ("mcp/Subject.class",
              payload(modified).at("items").at(0).at("resource_uri"));
    EXPECT_NE(payload(modified).at("items").at(0).at("original_fingerprint"),
              payload(modified).at("items").at(0).at("current_fingerprint"));

    const auto revertPreview = call(server,
                                    "workspace_revert_class",
                                    {{"workspace_id", workspaceId},
                                     {"class_uri", "mcp/Subject.class"},
                                     {"expected_revision", 1},
                                     {"dry_run", true}});
    ASSERT_FALSE(revertPreview.at("result").at("isError"));
    EXPECT_FALSE(payload(revertPreview).at("committed"));
    EXPECT_EQ(1, payload(revertPreview).at("revision"));
    EXPECT_FALSE(payload(revertPreview).at("byte_diffs").empty());

    const auto invalidBase64 = call(server,
                                    "patch_replace_class_bytes",
                                    {{"workspace_id", workspaceId},
                                     {"class_uri", "mcp/Subject.class"},
                                     {"content_base64", "AB=="},
                                     {"expected_revision", 1}});
    EXPECT_TRUE(invalidBase64.at("result").at("isError"));
    const auto truncatedClass = call(server,
                                     "patch_replace_class_bytes",
                                     {{"workspace_id", workspaceId},
                                      {"class_uri", "mcp/Subject.class"},
                                      {"content_base64", "yv66vg=="},
                                      {"expected_revision", 1}});
    EXPECT_TRUE(truncatedClass.at("result").at("isError"));

    const auto replaced = call(server,
                               "patch_replace_class_bytes",
                               {{"workspace_id", workspaceId},
                                {"class_uri", "mcp/Subject.class"},
                                {"content_base64", originalBase64},
                                {"expected_revision", 1},
                                {"label", "restore original class bytes"}});
    ASSERT_FALSE(replaced.at("result").at("isError"));
    EXPECT_TRUE(payload(replaced).at("committed"));
    EXPECT_EQ(2, payload(replaced).at("revision"));
    const auto clean = call(server,
                            "workspace_modified_classes",
                            {{"workspace_id", workspaceId}});
    ASSERT_FALSE(clean.at("result").at("isError"));
    EXPECT_TRUE(payload(clean).at("items").empty());

    const auto structured = call(
        server,
        "patch_apply",
        {{"workspace_id", workspaceId},
         {"class_uri", "mcp/Subject.class"},
         {"kind", "add_method"},
         {"member_name", "choose"},
         {"member_descriptor", "(I)I"},
         {"signature", "(I)I"},
         {"exceptions", Json::array({"java/lang/RuntimeException"})},
         {"access_flags", Json::array({"public", "static"})},
         {"initial_instructions",
          Json::array({{{"kind", "var"}, {"opcode", 21}, {"operand", 0}},
                       {{"kind", "jump"}, {"opcode", 153}, {"label", "zero"}},
                       {{"kind", "ldc_int"}, {"operand", 1}},
                       {{"kind", "simple"}, {"opcode", 172}},
                       {{"kind", "label"}, {"label", "zero"}},
                       {{"kind", "ldc_int"}, {"operand", 0}},
                       {{"kind", "simple"}, {"opcode", 172}}})},
         {"expected_revision", 2}});
    ASSERT_FALSE(structured.at("result").at("isError"));
    EXPECT_EQ(3, payload(structured).at("revision"));
    const auto method = call(server,
                             "method_get",
                             {{"workspace_id", workspaceId},
                              {"owner", "mcp/Subject"},
                              {"name", "choose"},
                              {"descriptor", "(I)I"}});
    ASSERT_FALSE(method.at("result").at("isError"));
    EXPECT_EQ("(I)I", payload(method).at("signature"));
    EXPECT_EQ(Json::array({"java/lang/RuntimeException"}),
              payload(method).at("exceptions"));

    const auto reverted = call(server,
                               "workspace_revert_class",
                               {{"workspace_id", workspaceId},
                                {"class_uri", "mcp/Subject.class"},
                                {"expected_revision", 3}});
    ASSERT_FALSE(reverted.at("result").at("isError"));
    EXPECT_TRUE(payload(reverted).at("committed"));
    EXPECT_EQ(4, payload(reverted).at("revision"));
    const auto removedMethod = call(server,
                                    "method_get",
                                    {{"workspace_id", workspaceId},
                                     {"owner", "mcp/Subject"},
                                     {"name", "choose"},
                                     {"descriptor", "(I)I"}});
    EXPECT_TRUE(removedMethod.at("result").at("isError"));
}

TEST(McpServerTest, ShadowAliasQueriesResolveBothDirections)
{
    McpSessionHarness server;

    const auto fixture    = writeFixture();
    const auto sidecarPath = fixture.string() + ".mjt-shadow";
    {
        std::ofstream sidecar(sidecarPath, std::ios::binary | std::ios::trunc);
        sidecar << "{\n"
                << "  \"version\": 1,\n"
                << "  \"aliases\": [\n"
                << "    {\"kind\": \"class\", \"name\": \"mcp/Subject\", \"shadow\": \"Class_1\"},\n"
                << "    {\"kind\": \"method\", \"owner\": \"mcp/Subject\", \"desc\": \"()V\","
                << " \"name\": \"run\", \"shadow\": \"Method_1\"},\n"
                << "    {\"kind\": \"field\", \"owner\": \"mcp/Second\", \"desc\": \"I\","
                << " \"name\": \"counter\", \"shadow\": \"Field_1\"}\n"
                << "  ]\n"
                << "}\n";
    }

    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

    //shadow → raw：机械占位名跨类重号按列表返回。
    const auto toRaw = call(server,
                            "shadow_to_raw",
                            {{"workspace_id", workspaceId}, {"name", "Class_1"}});
    ASSERT_FALSE(toRaw.at("result").at("isError")) << payload(toRaw).dump();
    ASSERT_EQ(1U, payload(toRaw).at("matches").size());
    EXPECT_EQ(sidecarPath, payload(toRaw).at("file").get<std::string>());
    EXPECT_EQ("class", payload(toRaw).at("matches").at(0).at("kind"));
    EXPECT_EQ("mcp/Subject", payload(toRaw).at("matches").at(0).at("name"));
    EXPECT_EQ("", payload(toRaw).at("matches").at(0).at("desc").get<std::string>());
    EXPECT_EQ("Class_1", payload(toRaw).at("matches").at(0).at("shadow"));

    //raw → shadow + kind 过滤。
    const auto toShadow = call(server,
                               "raw_to_shadow",
                               {{"workspace_id", workspaceId},
                                {"name", "run"},
                                {"kind", "method"}});
    ASSERT_FALSE(toShadow.at("result").at("isError"));
    ASSERT_EQ(1U, payload(toShadow).at("matches").size());
    EXPECT_EQ("Method_1", payload(toShadow).at("matches").at(0).at("shadow"));

    //owner 过滤命中与不命中。
    const auto byOwner = call(server,
                              "raw_to_shadow",
                              {{"workspace_id", workspaceId},
                               {"name", "counter"},
                               {"owner", "mcp/Second"}});
    ASSERT_FALSE(byOwner.at("result").at("isError"));
    ASSERT_EQ(1U, payload(byOwner).at("matches").size());
    const auto none = call(server,
                           "raw_to_shadow",
                           {{"workspace_id", workspaceId},
                            {"name", "counter"},
                            {"owner", "elsewhere/None"}});
    ASSERT_FALSE(none.at("result").at("isError"));
    EXPECT_EQ(0U, payload(none).at("matches").size());

    //file 参数显式指定 sidecar（旁路 <jar>.mjt-shadow 约定）。
    const auto explicitFile = call(server,
                                   "shadow_to_raw",
                                   {{"workspace_id", workspaceId},
                                    {"name", "Field_1"},
                                    {"file", sidecarPath}});
    ASSERT_FALSE(explicitFile.at("result").at("isError"));
    ASSERT_EQ(1U, payload(explicitFile).at("matches").size());
    EXPECT_EQ("field", payload(explicitFile).at("matches").at(0).at("kind"));

    //sidecar 缺失 → 工具错误带生成提示。
    const auto missing = call(server,
                              "shadow_to_raw",
                              {{"workspace_id", workspaceId},
                               {"name", "X"},
                               {"file", sidecarPath + ".no-such"}});
    ASSERT_TRUE(missing.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(missing).at("error").get<std::string>().find("shadow sidecar not found"));

    //空 name 拒绝。
    const auto emptyName = call(server,
                                "shadow_to_raw",
                                {{"workspace_id", workspaceId}, {"name", ""}});
    ASSERT_TRUE(emptyName.at("result").at("isError"));

    std::error_code sidecarRemoveError;
    std::filesystem::remove(sidecarPath, sidecarRemoveError);
}

TEST(McpServerTest, ShadowGenerationAndPruneManageSidecar)
{
    McpSessionHarness server;

    const auto fixture     = writeFixture();
    const auto sidecarPath = fixture.string() + ".mjt-shadow";
    std::error_code removeError;
    std::filesystem::remove(sidecarPath, removeError);
    const auto jarBefore = readFileBytes(fixture.string());

    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

    //生成：fixture 两个真 class（Subject/Second）+ jar 字节零写入。
    const auto generated = call(server, "shadow_generate", {{"workspace_id", workspaceId}});
    ASSERT_FALSE(generated.at("result").at("isError")) << payload(generated).dump();
    EXPECT_EQ(2U, payload(generated).at("assignedClasses").get<unsigned>());
    EXPECT_EQ(sidecarPath, payload(generated).at("file").get<std::string>());
    ASSERT_TRUE(std::filesystem::exists(sidecarPath));
    const auto totalAfterGenerate = payload(generated).at("totalEntries");
    EXPECT_EQ(jarBefore, readFileBytes(fixture.string()));

    //生成即可查：Class_1 = mcp/Second（字典序在 Subject 前）。
    const auto lookup = call(server,
                             "shadow_to_raw",
                             {{"workspace_id", workspaceId}, {"name", "Class_1"}});
    ASSERT_FALSE(lookup.at("result").at("isError"));
    ASSERT_EQ(1U, payload(lookup).at("matches").size());
    EXPECT_EQ("mcp/Second", payload(lookup).at("matches").at(0).at("name").get<std::string>());

    //幂等重跑：零新分配、全量保留。
    const auto regen = call(server, "shadow_generate", {{"workspace_id", workspaceId}});
    ASSERT_FALSE(regen.at("result").at("isError"));
    EXPECT_EQ(0U, payload(regen).at("assignedClasses").get<unsigned>());
    EXPECT_EQ(totalAfterGenerate, payload(regen).at("totalEntries"));

    //prune：显式 file 指向含过期条目的 sidecar，只清 Ghost。
    const auto staleSidecar = sidecarPath + ".stale";
    {
        std::ofstream stale(staleSidecar, std::ios::binary | std::ios::trunc);
        stale << "{\n"
              << "  \"version\": 1,\n"
              << "  \"aliases\": [\n"
              << "    {\"kind\": \"class\", \"name\": \"mcp/Subject\", \"shadow\": \"Class_1\"},\n"
              << "    {\"kind\": \"class\", \"name\": \"mcp/Ghost\", \"shadow\": \"Class_9\"}\n"
              << "  ]\n"
              << "}\n";
    }
    const auto pruned = call(server,
                             "shadow_prune",
                             {{"workspace_id", workspaceId}, {"file", staleSidecar}});
    ASSERT_FALSE(pruned.at("result").at("isError")) << payload(pruned).dump();
    EXPECT_EQ(1U, payload(pruned).at("removed").get<unsigned>());
    EXPECT_EQ(1U, payload(pruned).at("totalEntries").get<unsigned>());
    const auto ghost = call(server,
                            "raw_to_shadow",
                            {{"workspace_id", workspaceId},
                             {"name", "Ghost"},
                             {"file", staleSidecar}});
    ASSERT_FALSE(ghost.at("result").at("isError"));
    EXPECT_EQ(0U, payload(ghost).at("matches").size());
    std::filesystem::remove(staleSidecar, removeError);

    //file 指向 jar 本身 → 拒绝；prune 缺 sidecar → 报错带生成提示。
    const auto selfTarget = call(server,
                                 "shadow_generate",
                                 {{"workspace_id", workspaceId}, {"file", fixture.string()}});
    ASSERT_TRUE(selfTarget.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(selfTarget).at("error").get<std::string>().find("must not be the workspace JAR"));
    const auto pruneMissing = call(server,
                                   "shadow_prune",
                                   {{"workspace_id", workspaceId},
                                    {"file", sidecarPath + ".no-such"}});
    ASSERT_TRUE(pruneMissing.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(pruneMissing).at("error").get<std::string>().find("shadow sidecar not found"));

    std::filesystem::remove(sidecarPath, removeError);
}

}   //namespace
}   //namespace mjt::mcp
