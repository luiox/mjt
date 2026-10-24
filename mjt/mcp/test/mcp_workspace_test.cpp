#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
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

TEST(McpServerTest, ExposesProjectQueriesAndTransactionalEdits)
{
    McpSessionHarness server;

    const auto& initialize = server.initializeResponse();
    EXPECT_EQ("mjt-mcp", initialize.at("result").at("serverInfo").at("name"));

    //工具面收敛（RFC §8.1）：11 个粗粒度工具，旧细粒度名不再注册。
    const auto tools = server.handle(request("tools/list"));
    const auto listed = tools.at("result").at("tools");
    for (const char* name :
         {"project_open", "project_close", "project_info", "container_mount", "class_query",
          "search", "xref_query", "decompile", "edit_apply", "edit_history", "project_export"}) {
        SCOPED_TRACE(name);
        ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [&](const Json& item) {
            return item.at("name") == name;
        }));
    }
    const auto classQuery = std::find_if(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "class_query";
    });
    ASSERT_NE(classQuery, listed.end());
    EXPECT_EQ(Json::array({"project_id", "kind"}),
              classQuery->at("inputSchema").at("required"));
    const auto editApply = std::find_if(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "edit_apply";
    });
    ASSERT_NE(editApply, listed.end());
    EXPECT_EQ(Json::array({"project_id", "kind"}),
              editApply->at("inputSchema").at("required"));

    const auto fixture = writeFixture();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();
    const auto fakeDecompiler = test::getTempOutPath("fake-mcp-decompiler.jar");
    std::ofstream(fakeDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");

    //project_info 报告反编译后端可用性（decompiler_list 的折叠去向）。
    const auto info = call(server, "project_info", {{"project_id", projectId}});
    ASSERT_FALSE(info.at("result").at("isError"));
    ASSERT_EQ(2U, payload(info).at("decompilers").size());

    const auto classSearch = call(server,
                                  "search",
                                  {{"project_id", projectId},
                                   {"kind", "classes"},
                                   {"query", "subject"}});
    ASSERT_FALSE(classSearch.at("result").at("isError"));
    ASSERT_EQ(1U, payload(classSearch).at("items").size());
    EXPECT_EQ("class", payload(classSearch).at("items").at(0).at("kind"));
    EXPECT_EQ("mcp/Subject", payload(classSearch).at("items").at(0).at("class"));

    const auto memberSearch = call(server,
                                   "search",
                                   {{"project_id", projectId},
                                    {"kind", "members"},
                                    {"query", "counter"}});
    ASSERT_FALSE(memberSearch.at("result").at("isError"));
    ASSERT_EQ(2U, payload(memberSearch).at("items").size());
    EXPECT_EQ("field", payload(memberSearch).at("items").at(0).at("kind"));

    const auto stringSearch = call(server,
                                   "search",
                                   {{"project_id", projectId},
                                    {"kind", "strings"},
                                    {"query", "mcp/Subject"}});
    ASSERT_FALSE(stringSearch.at("result").at("isError"));
    EXPECT_FALSE(payload(stringSearch).at("items").empty());

    const auto numberSearch = call(server,
                                   "search",
                                   {{"project_id", projectId},
                                    {"kind", "numbers"},
                                    {"query", "1.0"}});
    ASSERT_FALSE(numberSearch.at("result").at("isError"));
    EXPECT_FALSE(payload(numberSearch).at("items").empty());

    const auto instructionSearch = call(server,
                                        "search",
                                        {{"project_id", projectId},
                                         {"kind", "instructions"},
                                         {"query", "ldc"}});
    ASSERT_FALSE(instructionSearch.at("result").at("isError"));
    EXPECT_FALSE(payload(instructionSearch).at("items").empty());

    const auto classes = call(server,
                              "class_query",
                              {{"project_id", projectId}, {"kind", "list"}});
    ASSERT_FALSE(classes.at("result").at("isError"));
    ASSERT_EQ(2U, payload(classes).at("items").size());
    const auto& firstClass = payload(classes).at("items").at(0);
    EXPECT_TRUE(firstClass.contains("resource_uri"));
    EXPECT_TRUE(firstClass.contains("class"));
    EXPECT_TRUE(firstClass.contains("super_name"));
    EXPECT_TRUE(firstClass.contains("field_count"));
    EXPECT_TRUE(firstClass.contains("method_count"));
    EXPECT_FALSE(firstClass.contains("signature"));
    EXPECT_FALSE(firstClass.contains("fields"));
    EXPECT_FALSE(firstClass.contains("methods"));
    EXPECT_FALSE(firstClass.contains("annotations"));
    EXPECT_NE(std::find_if(payload(classes).at("items").begin(),
                           payload(classes).at("items").end(),
                           [](const Json& item) { return item.at("class") == "mcp/Subject"; }),
              payload(classes).at("items").end());

    const auto classBytes = call(server,
                                 "class_query",
                                 {{"project_id", projectId},
                                  {"kind", "bytes"},
                                  {"class", "mcp/Subject"},
                                  {"limit", 4}});
    ASSERT_FALSE(classBytes.at("result").at("isError"));
    EXPECT_EQ("mcp/Subject.class", payload(classBytes).at("resource_uri"));
    EXPECT_EQ("mcp/Subject", payload(classBytes).at("class"));
    EXPECT_EQ(0U,
              payload(classBytes).at("fingerprint").get<std::string>().find("fnv1a64:"));
    EXPECT_EQ(0, payload(classBytes).at("offset"));
    EXPECT_EQ("4", payload(classBytes).at("next_cursor"));
    EXPECT_EQ("yv66vg==", payload(classBytes).at("content"));

    //container_mount：support 容器挂载（登记 project.json），project_info 容器
    //列表可查（workspace_list_support 的折叠去向）。
    const auto supportPath = writeQuerySupportingFixture();
    const auto supportMounted = call(server,
                                     "container_mount",
                                     {{"project_id", projectId},
                                      {"action", "mount"},
                                      {"path", supportPath.string()}});
    ASSERT_FALSE(supportMounted.at("result").at("isError")) << supportMounted.dump();
    const auto supportId = payload(supportMounted)
                               .at("container")
                               .at("id")
                               .get<std::string>();
    EXPECT_EQ("support-1", supportId);
    EXPECT_EQ("support", payload(supportMounted).at("container").at("role"));
    const auto supportInfo = call(server, "project_info", {{"project_id", projectId}});
    ASSERT_FALSE(supportInfo.at("result").at("isError"));
    const auto& containers = payload(supportInfo).at("containers");
    ASSERT_EQ(2U, containers.size());
    EXPECT_EQ("support", containers.at(1).at("role"));
    EXPECT_EQ(supportId, containers.at(1).at("id"));

    const auto callers = call(server,
                              "xref_query",
                              {{"project_id", projectId},
                               {"kind", "references"},
                               {"target", "method"},
                               {"direction", "to"},
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
                              "xref_query",
                              {{"project_id", projectId},
                               {"kind", "references"},
                               {"target", "method"},
                               {"direction", "from"},
                               {"owner", "support/McpLibrary"},
                               {"name", "call"},
                               {"descriptor", "()I"}});
    ASSERT_FALSE(callees.at("result").at("isError"));
    ASSERT_EQ(1U, payload(callees).at("items").size());
    EXPECT_TRUE(payload(callees).at("items").at(0).at("source").at("supporting"));
    const auto relations = call(server,
                                "xref_query",
                                {{"project_id", projectId},
                                 {"kind", "relations"},
                                 {"class", "mcp/Subject"}});
    ASSERT_FALSE(relations.at("result").at("isError"));
    EXPECT_NE(payload(relations).at("all_subclasses").end(),
              std::find(payload(relations).at("all_subclasses").begin(),
                        payload(relations).at("all_subclasses").end(),
                        Json("support/McpLibrary")));
    const auto supportSource = std::find_if(
        payload(relations).at("sources").begin(),
        payload(relations).at("sources").end(),
        [](const Json& source) { return source.at("class") == "support/McpLibrary"; });
    ASSERT_NE(supportSource, payload(relations).at("sources").end());
    EXPECT_TRUE(supportSource->at("supporting"));
    EXPECT_EQ(supportId, supportSource->at("support_id"));

    const auto exactSearch = call(server,
                                  "search",
                                  {{"project_id", projectId},
                                   {"kind", "classes"},
                                   {"query", "mcp/Subject"},
                                   {"match_mode", "exact"}});
    ASSERT_FALSE(exactSearch.at("result").at("isError"));
    ASSERT_EQ(1U, payload(exactSearch).at("items").size());
    const auto regexSearch = call(server,
                                  "search",
                                  {{"project_id", projectId},
                                   {"kind", "classes"},
                                   {"query", "^mcp/(Subject|Second)$"},
                                   {"match_mode", "regex"},
                                   {"case_sensitive", true},
                                   {"package_prefix", "mcp/"}});
    ASSERT_FALSE(regexSearch.at("result").at("isError"));
    EXPECT_EQ(2U, payload(regexSearch).at("items").size());
    const auto memberFilter = call(server,
                                   "search",
                                   {{"project_id", projectId},
                                    {"kind", "members"},
                                    {"query", "counter"},
                                    {"owner", "mcp/Subject"},
                                    {"member_kind", "field"}});
    ASSERT_FALSE(memberFilter.at("result").at("isError"));
    ASSERT_EQ(1U, payload(memberFilter).at("items").size());
    const auto opcodeFilter = call(server,
                                   "search",
                                   {{"project_id", projectId},
                                    {"kind", "instructions"},
                                    {"query", "ldc"},
                                    {"owner", "mcp/Subject"},
                                    {"opcode", 18}});
    ASSERT_FALSE(opcodeFilter.at("result").at("isError"));
    ASSERT_EQ(1U, payload(opcodeFilter).at("items").size());
    const auto invalidRegex = call(server,
                                   "search",
                                   {{"project_id", projectId},
                                    {"kind", "classes"},
                                    {"query", "["},
                                    {"match_mode", "regex"}});
    EXPECT_TRUE(invalidRegex.at("result").at("isError"));
    const auto duplicateSupport = call(server,
                                       "container_mount",
                                       {{"project_id", projectId},
                                        {"action", "mount"},
                                        {"path", supportPath.string()}});
    EXPECT_TRUE(duplicateSupport.at("result").at("isError"));
    const auto supportDetached = call(server,
                                      "container_mount",
                                      {{"project_id", projectId},
                                       {"action", "detach"},
                                       {"support_id", supportId}});
    ASSERT_FALSE(supportDetached.at("result").at("isError"));
    EXPECT_EQ(supportId, payload(supportDetached).at("removed").at("id"));
    const auto emptyInfo = call(server, "project_info", {{"project_id", projectId}});
    ASSERT_FALSE(emptyInfo.at("result").at("isError"));
    EXPECT_EQ(1U, payload(emptyInfo).at("containers").size());
    const auto callersAfterRemove = call(server,
                                         "xref_query",
                                         {{"project_id", projectId},
                                          {"kind", "references"},
                                          {"target", "method"},
                                          {"direction", "to"},
                                          {"owner", "mcp/Subject"},
                                          {"name", "value"},
                                          {"descriptor", "()I"}});
    ASSERT_FALSE(callersAfterRemove.at("result").at("isError"));
    EXPECT_TRUE(payload(callersAfterRemove).at("items").empty());

    const auto resource = call(server,
                               "class_query",
                               {{"project_id", projectId},
                                {"kind", "resource_bytes"},
                                {"uri", "notice.txt"},
                                {"limit", 4}});
    ASSERT_FALSE(resource.at("result").at("isError"));
    EXPECT_EQ(0, payload(resource).at("offset"));
    EXPECT_EQ("4", payload(resource).at("next_cursor"));

    const auto method = call(server,
                             "class_query",
                             {{"project_id", projectId},
                              {"kind", "method_jasm"},
                              {"owner", "mcp/Subject"},
                              {"name", "value"},
                              {"descriptor", "()I"}});
    ASSERT_FALSE(method.at("result").at("isError"));
    EXPECT_NE(std::string::npos, payload(method).at("jasm").get<std::string>().find("value ()I"));

    const auto source = call(server,
                             "decompile",
                             {{"project_id", projectId},
                              {"class", "mcp/Subject"},
                              {"decompiler_jar", fakeDecompiler.string()},
                              {"java", fakeJava.string()}});
    ASSERT_FALSE(source.at("result").at("isError"));
    EXPECT_EQ("class", payload(source).at("source_scope"));
    EXPECT_NE(std::string::npos,
              payload(source).at("source").get<std::string>().find("public class Subject"));
    const auto limitedSource = call(server,
                                    "decompile",
                                    {{"project_id", projectId},
                                     {"class", "mcp/Subject"},
                                     {"max_source_bytes", 16},
                                     {"decompiler_jar", fakeDecompiler.string()},
                                     {"java", fakeJava.string()}});
    EXPECT_TRUE(limitedSource.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(limitedSource).at("status").at("message").get<std::string>().find("16 bytes"));

    const auto methodSource = call(server,
                                   "decompile",
                                   {{"project_id", projectId},
                                    {"owner", "mcp/Subject"},
                                    {"name", "value"},
                                    {"descriptor", "()I"},
                                    {"decompiler_jar", fakeDecompiler.string()},
                                    {"java", fakeJava.string()}});
    ASSERT_FALSE(methodSource.at("result").at("isError"));
    EXPECT_EQ("class", payload(methodSource).at("source_scope"));

    //async 作业面（job_get/job_cancel/job_release 的折叠去向）。
    const auto slowDecompiler = test::getTempOutPath("fake-mcp-decompiler-slow.jar");
    std::ofstream(slowDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto asyncSource = call(server,
                                  "decompile",
                                  {{"project_id", projectId},
                                   {"class", "mcp/Subject"},
                                   {"decompiler_jar", fakeDecompiler.string()},
                                   {"java", fakeJava.string()},
                                   {"async", true}});
    ASSERT_FALSE(asyncSource.at("result").at("isError"));
    const auto asyncJobId = payload(asyncSource).at("job_id").get<std::string>();
    Json       completedJob;
    for (int attempt = 0; attempt < 100; ++attempt) {
        completedJob =
            call(server, "decompile", {{"project_id", projectId}, {"action", "poll"}, {"job_id", asyncJobId}});
        ASSERT_FALSE(completedJob.at("result").at("isError"));
        if (payload(completedJob).at("state") == "succeeded") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ("succeeded", payload(completedJob).at("state"));
    EXPECT_NE(std::string::npos,
              payload(completedJob).at("result").at("source").get<std::string>().find("public class Subject"));
    const auto releasedJob =
        call(server, "decompile", {{"project_id", projectId}, {"action", "release"}, {"job_id", asyncJobId}});
    ASSERT_FALSE(releasedJob.at("result").at("isError"));
    EXPECT_TRUE(payload(releasedJob).at("released"));

    const auto slowSource = call(server,
                                 "decompile",
                                 {{"project_id", projectId},
                                  {"class", "mcp/Subject"},
                                  {"decompiler_jar", slowDecompiler.string()},
                                  {"java", fakeJava.string()},
                                  {"async", true}});
    ASSERT_FALSE(slowSource.at("result").at("isError"));
    const auto slowJobId = payload(slowSource).at("job_id").get<std::string>();
    const auto cancelled =
        call(server, "decompile", {{"project_id", projectId}, {"action", "cancel"}, {"job_id", slowJobId}});
    ASSERT_FALSE(cancelled.at("result").at("isError"));
    EXPECT_TRUE(payload(cancelled).at("cancel_requested"));
    Json cancelledJob;
    for (int attempt = 0; attempt < 100; ++attempt) {
        cancelledJob =
            call(server, "decompile", {{"project_id", projectId}, {"action", "poll"}, {"job_id", slowJobId}});
        ASSERT_FALSE(cancelledJob.at("result").at("isError"));
        if (payload(cancelledJob).at("state") == "cancelled") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ("cancelled", payload(cancelledJob).at("state"));
    EXPECT_TRUE(payload(cancelledJob).at("result_is_error"));
    EXPECT_EQ(static_cast<int>(ca::StatusCode::CANCELLED),
              payload(cancelledJob).at("result").at("status").at("code"));
    const auto releasedCancelledJob =
        call(server, "decompile", {{"project_id", projectId}, {"action", "release"}, {"job_id", slowJobId}});
    ASSERT_FALSE(releasedCancelledJob.at("result").at("isError"));
    EXPECT_TRUE(payload(releasedCancelledJob).at("released"));

    const auto preview = call(server,
                              "edit_apply",
                              {{"project_id", projectId},
                               {"kind", "jasm"},
                               {"dry_run", true},
                               {"uri", "mcp/Subject.class"},
                               {"source_name", "Subject.jasm"},
                               {"source", classSource("42")},
                               {"expected_revision", 0}});
    ASSERT_FALSE(preview.at("result").at("isError"));
    EXPECT_FALSE(payload(preview).at("committed"));
    EXPECT_EQ(0, payload(preview).at("revision"));
    EXPECT_FALSE(payload(preview).at("byte_diffs").empty());

    const auto applied = call(server,
                              "edit_apply",
                              {{"project_id", projectId},
                               {"kind", "jasm"},
                               {"uri", "mcp/Subject.class"},
                               {"source_name", "Subject.jasm"},
                               {"source", classSource("42")},
                               {"expected_revision", 0}});
    ASSERT_FALSE(applied.at("result").at("isError"));
    EXPECT_TRUE(payload(applied).at("committed"));
    EXPECT_EQ(1, payload(applied).at("revision"));

    const auto invalidPass = call(server,
                                  "edit_apply",
                                  {{"project_id", projectId},
                                   {"kind", "pipeline"},
                                   {"passes", Json::array({"DefinitelyNotAPass"})},
                                   {"expected_revision", 1}});
    EXPECT_TRUE(invalidPass.at("result").at("isError"));
    EXPECT_EQ(1, payload(invalidPass).at("revision"));

    const auto optimizationPreview = call(server,
                                          "edit_apply",
                                          {{"project_id", projectId},
                                           {"kind", "pipeline"},
                                           {"dry_run", true},
                                           {"expected_revision", 1},
                                           {"label", "preview default optimization"}});
    ASSERT_FALSE(optimizationPreview.at("result").at("isError"));
    EXPECT_FALSE(payload(optimizationPreview).at("committed"));
    EXPECT_EQ(1, payload(optimizationPreview).at("revision"));

    const auto rename = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "rename"},
                              {"dry_run", true},
                              {"rename_kind", "field"},
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
                                     "edit_apply",
                                     {{"project_id", projectId},
                                      {"kind", "mapping"},
                                      {"dry_run", true},
                                      {"format", "Tiny-V2"},
                                      {"mapping_text", mapping},
                                      {"policy", "all"}});
    ASSERT_FALSE(mappingPreview.at("result").at("isError"));
    EXPECT_TRUE(payload(mappingPreview).at("exportable"));
    EXPECT_FALSE(payload(mappingPreview).at("byte_diffs").empty());

    const auto mappingApplied = call(server,
                                     "edit_apply",
                                     {{"project_id", projectId},
                                      {"kind", "mapping"},
                                      {"format", "Tiny-V2"},
                                      {"mapping_text", mapping},
                                      {"policy", "all"},
                                      {"expected_revision", 1},
                                      {"label", "rename subject with mapping"}});
    ASSERT_FALSE(mappingApplied.at("result").at("isError"));
    EXPECT_TRUE(payload(mappingApplied).at("committed"));
    EXPECT_EQ(2, payload(mappingApplied).at("revision"));

    const auto renamed = call(server,
                              "class_query",
                              {{"project_id", projectId},
                               {"kind", "class"},
                               {"class", "mcp/RenamedSubject"}});
    ASSERT_FALSE(renamed.at("result").at("isError"));
    EXPECT_EQ("mcp/RenamedSubject", payload(renamed).at("class"));
}

TEST(McpServerTest, KeepsLargeClassListResponsesBounded)
{
    const std::string signature =
        "<T" + std::string(65000, 'A') + ":Ljava/lang/Object;>Ljava/lang/Object;";
    McpSessionHarness server;
    const auto        opened = call(
        server,
        "project_open",
        {{"path", writeLargeClassListFixture(signature).string()}, {"project_id", "large-list"}});
    ASSERT_FALSE(opened.at("result").at("isError"));

    const auto classes = call(server,
                              "class_query",
                              {{"project_id", "large-list"}, {"kind", "list"}, {"limit", 200}});
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

    const auto remaining = call(server,
                                "class_query",
                                {{"project_id", "large-list"},
                                 {"kind", "list"},
                                 {"cursor", "200"},
                                 {"limit", 200}});
    ASSERT_FALSE(remaining.at("result").at("isError"));
    EXPECT_EQ(10U, payload(remaining).at("items").size());
    EXPECT_FALSE(payload(remaining).contains("next_cursor"));

    const auto detail = call(server,
                             "class_query",
                             {{"project_id", "large-list"},
                              {"kind", "class"},
                              {"class", "mcp/large/Subject0"}});
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
        "project_open",
        {{"path", writeLargeClassListFixture(signature).string()}, {"project_id", "cursor-bound"}});
    ASSERT_FALSE(opened.at("result").at("isError"));

    //cursor 远超 total(210)：parseCursor clamp 到 210，items 为空、无 next_cursor。
    const auto pastEnd = call(server,
                              "class_query",
                              {{"project_id", "cursor-bound"}, {"kind", "list"}, {"cursor", "999999"}});
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
    const auto opened = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();
    const auto original = assembleFixtureClass("mcp/Subject", classSource("1"));
    const auto originalBase64 = encodeBase64(original);

    const auto changed = call(server,
                              "edit_apply",
                              {{"project_id", projectId},
                               {"kind", "jasm"},
                               {"uri", "mcp/Subject.class"},
                               {"source", classSource("42")},
                               {"expected_revision", 0}});
    ASSERT_FALSE(changed.at("result").at("isError"));
    EXPECT_EQ(1, payload(changed).at("revision"));

    const auto modified = call(server,
                               "edit_history",
                               {{"project_id", projectId}, {"action", "modified_classes"}});
    ASSERT_FALSE(modified.at("result").at("isError"));
    ASSERT_EQ(1U, payload(modified).at("items").size());
    EXPECT_EQ("mcp/Subject.class",
              payload(modified).at("items").at(0).at("resource_uri"));
    EXPECT_NE(payload(modified).at("items").at(0).at("original_fingerprint"),
              payload(modified).at("items").at(0).at("current_fingerprint"));

    const auto revertPreview = call(server,
                                    "edit_history",
                                    {{"project_id", projectId},
                                     {"action", "revert_class"},
                                     {"class_uri", "mcp/Subject.class"},
                                     {"expected_revision", 1},
                                     {"dry_run", true}});
    ASSERT_FALSE(revertPreview.at("result").at("isError"));
    EXPECT_FALSE(payload(revertPreview).at("committed"));
    EXPECT_EQ(1, payload(revertPreview).at("revision"));
    EXPECT_FALSE(payload(revertPreview).at("byte_diffs").empty());

    const auto invalidBase64 = call(server,
                                    "edit_apply",
                                    {{"project_id", projectId},
                                     {"kind", "class_bytes"},
                                     {"class_uri", "mcp/Subject.class"},
                                     {"content_base64", "AB=="},
                                     {"expected_revision", 1}});
    EXPECT_TRUE(invalidBase64.at("result").at("isError"));
    const auto truncatedClass = call(server,
                                     "edit_apply",
                                     {{"project_id", projectId},
                                      {"kind", "class_bytes"},
                                      {"class_uri", "mcp/Subject.class"},
                                      {"content_base64", "yv66vg=="},
                                      {"expected_revision", 1}});
    EXPECT_TRUE(truncatedClass.at("result").at("isError"));

    const auto replaced = call(server,
                               "edit_apply",
                               {{"project_id", projectId},
                                {"kind", "class_bytes"},
                                {"class_uri", "mcp/Subject.class"},
                                {"content_base64", originalBase64},
                                {"expected_revision", 1},
                                {"label", "restore original class bytes"}});
    ASSERT_FALSE(replaced.at("result").at("isError"));
    EXPECT_TRUE(payload(replaced).at("committed"));
    EXPECT_EQ(2, payload(replaced).at("revision"));
    const auto clean = call(server,
                            "edit_history",
                            {{"project_id", projectId}, {"action", "modified_classes"}});
    ASSERT_FALSE(clean.at("result").at("isError"));
    EXPECT_TRUE(payload(clean).at("items").empty());

    const auto structured = call(
        server,
        "edit_apply",
        {{"project_id", projectId},
         {"kind", "structured"},
         {"class_uri", "mcp/Subject.class"},
         {"mutation_kind", "add_method"},
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
                             "class_query",
                             {{"project_id", projectId},
                              {"kind", "method"},
                              {"owner", "mcp/Subject"},
                              {"name", "choose"},
                              {"descriptor", "(I)I"}});
    ASSERT_FALSE(method.at("result").at("isError"));
    EXPECT_EQ("(I)I", payload(method).at("signature"));
    EXPECT_EQ(Json::array({"java/lang/RuntimeException"}),
              payload(method).at("exceptions"));

    const auto reverted = call(server,
                               "edit_history",
                               {{"project_id", projectId},
                                {"action", "revert_class"},
                                {"class_uri", "mcp/Subject.class"},
                                {"expected_revision", 3}});
    ASSERT_FALSE(reverted.at("result").at("isError"));
    EXPECT_TRUE(payload(reverted).at("committed"));
    EXPECT_EQ(4, payload(reverted).at("revision"));
    const auto removedMethod = call(server,
                                    "class_query",
                                    {{"project_id", projectId},
                                     {"kind", "method"},
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

    const auto opened = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //shadow → raw：机械占位名跨类重号按列表返回。
    const auto toRaw = call(server,
                            "search",
                            {{"project_id", projectId}, {"kind", "shadow"}, {"name", "Class_1"}});
    ASSERT_FALSE(toRaw.at("result").at("isError")) << payload(toRaw).dump();
    ASSERT_EQ(1U, payload(toRaw).at("matches").size());
    EXPECT_EQ(sidecarPath, payload(toRaw).at("file").get<std::string>());
    EXPECT_EQ("class", payload(toRaw).at("matches").at(0).at("kind"));
    EXPECT_EQ("mcp/Subject", payload(toRaw).at("matches").at(0).at("name"));
    EXPECT_EQ("", payload(toRaw).at("matches").at(0).at("desc").get<std::string>());
    EXPECT_EQ("Class_1", payload(toRaw).at("matches").at(0).at("shadow"));

    //raw → shadow + symbol_kind 过滤。
    const auto toShadow = call(server,
                               "search",
                               {{"project_id", projectId},
                                {"kind", "shadow"},
                                {"direction", "raw_to_shadow"},
                                {"name", "run"},
                                {"symbol_kind", "method"}});
    ASSERT_FALSE(toShadow.at("result").at("isError"));
    ASSERT_EQ(1U, payload(toShadow).at("matches").size());
    EXPECT_EQ("Method_1", payload(toShadow).at("matches").at(0).at("shadow"));

    //owner 过滤命中与不命中。
    const auto byOwner = call(server,
                              "search",
                              {{"project_id", projectId},
                               {"kind", "shadow"},
                               {"direction", "raw_to_shadow"},
                               {"name", "counter"},
                               {"owner", "mcp/Second"}});
    ASSERT_FALSE(byOwner.at("result").at("isError"));
    ASSERT_EQ(1U, payload(byOwner).at("matches").size());
    const auto none = call(server,
                           "search",
                           {{"project_id", projectId},
                            {"kind", "shadow"},
                            {"direction", "raw_to_shadow"},
                            {"name", "counter"},
                            {"owner", "elsewhere/None"}});
    ASSERT_FALSE(none.at("result").at("isError"));
    EXPECT_EQ(0U, payload(none).at("matches").size());

    //file 参数显式指定 sidecar（旁路 <jar>.mjt-shadow 约定）。
    const auto explicitFile = call(server,
                                   "search",
                                   {{"project_id", projectId},
                                    {"kind", "shadow"},
                                    {"name", "Field_1"},
                                    {"file", sidecarPath}});
    ASSERT_FALSE(explicitFile.at("result").at("isError"));
    ASSERT_EQ(1U, payload(explicitFile).at("matches").size());
    EXPECT_EQ("field", payload(explicitFile).at("matches").at(0).at("kind"));

    //sidecar 缺失 → 工具错误带生成提示。
    const auto missing = call(server,
                              "search",
                              {{"project_id", projectId},
                               {"kind", "shadow"},
                               {"name", "X"},
                               {"file", sidecarPath + ".no-such"}});
    ASSERT_TRUE(missing.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(missing).at("error").get<std::string>().find("shadow sidecar not found"));

    //空 name 拒绝。
    const auto emptyName = call(server,
                                "search",
                                {{"project_id", projectId}, {"kind", "shadow"}, {"name", ""}});
    ASSERT_TRUE(emptyName.at("result").at("isError"));

    std::error_code sidecarRemoveError;
    std::filesystem::remove(sidecarPath, sidecarRemoveError);
}

}   //namespace
}   //namespace mjt::mcp
