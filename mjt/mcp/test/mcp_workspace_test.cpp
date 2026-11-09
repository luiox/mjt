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
    ASSERT_EQ(3U, payload(info).at("decompilers").size());

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

    //shadow → raw：机械占位名跨类重号按列表返回。表源 = 工程分析库
    //（sidecar 已在 open 期一次性导入；"file" 回显 DB 占位名）。
    const auto toRaw = call(server,
                            "search",
                            {{"project_id", projectId}, {"kind", "shadow"}, {"name", "Class_1"}});
    ASSERT_FALSE(toRaw.at("result").at("isError")) << payload(toRaw).dump();
    ASSERT_EQ(1U, payload(toRaw).at("matches").size());
    EXPECT_EQ("analysis.mjdb (project bundle)",
              payload(toRaw).at("file").get<std::string>());
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
              payload(missing).at("error").get<std::string>().find("shadow table not available"));

    //空 name 拒绝。
    const auto emptyName = call(server,
                                "search",
                                {{"project_id", projectId}, {"kind", "shadow"}, {"name", ""}});
    ASSERT_TRUE(emptyName.at("result").at("isError"));

    std::error_code sidecarRemoveError;
    std::filesystem::remove(sidecarPath, sidecarRemoveError);
}

///kind=classes DB-first 的编辑会话保鲜（查询面换轨 + 编辑增量同步）：编辑
///后不回落引擎扫描——commit 挂 analysis_edit_sync 把受影响类的条目锚/类
///事实/引用切片追平工作副本，DB 路径全程可用。分歧载具 = 真改名（mapping
///漏斗）：条目搬家 mcp/Subject.class → mcp/RenamedSubject.class，库内
///internal_name 随身份迁移更新、条目锚靠增量同步换新——若同步缺席，DB
///路径会答旧 resource_uri（此断言即翻车）。
TEST(McpServerTest, SearchClassesStaysFreshAfterEditsViaIncrementalSync)
{
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //干净会话：exact 查 Subject 一命中（analysis.db 路径，开期导入态）。
    const auto before = call(server,
                             "search",
                             {{"project_id", projectId},
                              {"kind", "classes"},
                              {"query", "mcp/Subject"},
                              {"match_mode", "exact"}});
    ASSERT_FALSE(before.at("result").at("isError")) << before.dump();
    ASSERT_EQ(1U, payload(before).at("total").get<unsigned>());
    EXPECT_EQ("mcp/Subject", payload(before).at("items").at(0).at("class"));
    EXPECT_EQ("mcp/Subject.class", payload(before).at("items").at(0).at("resource_uri"));

    //真改名（mapping）：工作副本条目路径随类名搬家，commit 增量同步换锚。
    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/RenamedSubject\n";
    const auto edited = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "mapping"},
                              {"format", "Tiny-V2"},
                              {"mapping_text", mapping},
                              {"policy", "all"},
                              {"expected_revision", 0}});
    ASSERT_FALSE(edited.at("result").at("isError")) << edited.dump();
    EXPECT_TRUE(payload(edited).at("committed"));
    EXPECT_FALSE(payload(edited).at("analysis_sync").at("failed").get<bool>());
    EXPECT_GE(payload(edited).at("analysis_sync").at("classes").get<unsigned>(), 1U);

    //DB 路径答新态：新名可见且 resource_uri 是搬后的条目路径（增量同步换锚
    //的精确验收点），旧名消失。
    const auto afterNew = call(server,
                               "search",
                               {{"project_id", projectId},
                                {"kind", "classes"},
                                {"query", "mcp/RenamedSubject"},
                                {"match_mode", "exact"}});
    ASSERT_FALSE(afterNew.at("result").at("isError")) << afterNew.dump();
    ASSERT_EQ(1U, payload(afterNew).at("total").get<unsigned>());
    EXPECT_EQ("mcp/RenamedSubject", payload(afterNew).at("items").at(0).at("class"));
    EXPECT_EQ("mcp/RenamedSubject.class",
              payload(afterNew).at("items").at(0).at("resource_uri"));

    const auto afterOld = call(server,
                               "search",
                               {{"project_id", projectId},
                                {"kind", "classes"},
                                {"query", "mcp/Subject"},
                                {"match_mode", "exact"}});
    ASSERT_FALSE(afterOld.at("result").at("isError")) << afterOld.dump();
    EXPECT_EQ(0U, payload(afterOld).at("total").get<unsigned>());
}

///search kind=members signature 面端到端 + 编辑增量同步保签名：needle 只在
///方法 Signature 属性出现——DB-first 路径（非 regex）经 ingest 落
///members.signature 才能命中；真改名后增量同步重落类事实，签名列随行
///覆写不丢（丢则 DB 行 signature 回空、查询落空）。
TEST(McpServerTest, SearchMembersSignatureFaceStaysFreshAfterEditsViaIncrementalSync)
{
    McpSessionHarness server;
    const auto fixture = writeSignaturedMemberFixture();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //干净会话：泛型签名原文命中（owner/name/descriptor 面都不含该 needle，
    //只有 signature 面答得上）。
    const auto before = call(server,
                             "search",
                             {{"project_id", projectId},
                              {"kind", "members"},
                              {"query", "List<Ljava/lang/String;>"}});
    ASSERT_FALSE(before.at("result").at("isError")) << before.dump();
    ASSERT_EQ(1U, payload(before).at("total").get<unsigned>());
    const auto& beforeItem = payload(before).at("items").at(0);
    EXPECT_EQ("method", beforeItem.at("kind"));
    EXPECT_EQ("mcp/Subject", beforeItem.at("owner"));
    EXPECT_EQ("value", beforeItem.at("name"));
    EXPECT_EQ("()I", beforeItem.at("descriptor"));
    EXPECT_EQ("value()I", beforeItem.at("value"));
    EXPECT_EQ("mcp/Subject.class", beforeItem.at("resource_uri"));

    //真改名（mapping）：commit 增量同步重落类事实——成员行换 owner 锚的
    //同时 signature 列随 DbMemberInput 覆写。
    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/RenamedSubject\n";
    const auto edited = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "mapping"},
                              {"format", "Tiny-V2"},
                              {"mapping_text", mapping},
                              {"policy", "all"},
                              {"expected_revision", 0}});
    ASSERT_FALSE(edited.at("result").at("isError")) << edited.dump();
    EXPECT_TRUE(payload(edited).at("committed"));
    EXPECT_FALSE(payload(edited).at("analysis_sync").at("failed").get<bool>());

    //DB 路径答新态：同一签名 needle 仍一命中，owner/resource_uri 搬家。
    const auto after = call(server,
                            "search",
                            {{"project_id", projectId},
                             {"kind", "members"},
                             {"query", "List<Ljava/lang/String;>"}});
    ASSERT_FALSE(after.at("result").at("isError")) << after.dump();
    ASSERT_EQ(1U, payload(after).at("total").get<unsigned>());
    const auto& afterItem = payload(after).at("items").at(0);
    EXPECT_EQ("method", afterItem.at("kind"));
    EXPECT_EQ("mcp/RenamedSubject", afterItem.at("owner"));
    EXPECT_EQ("value", afterItem.at("name"));
    EXPECT_EQ("()I", afterItem.at("descriptor"));
    EXPECT_EQ("mcp/RenamedSubject.class", afterItem.at("resource_uri"));
}

///kind=references DB-first 的干净挂载态命中（#121 查询面换轨的 xref 步）：
///无支撑容器挂载 + 会话分析库在线时 xref_query 直查 analysis.db——开期整库
///导入的 refs 表与引擎扫描同源。分歧载具 = target=class java/lang/Object
///（目标类从未入库登记，钉 referencesToClass 的文本分支兜底）：两类的
/// .super 头声明是 Type 行（sourceMethod/descriptor 空、insnIndex -1），库行
///映射 + finalizeReferenceRows 收口后与引擎输出同形（kind "type"、target 内
///部名、sourceClass 字节序全序）。
TEST(McpServerTest, XrefReferencesQueryFromAnalysisDbInCleanMountState)
{
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto supers = call(server,
                             "xref_query",
                             {{"project_id", projectId},
                              {"kind", "references"},
                              {"target", "class"},
                              {"direction", "to"},
                              {"class", "java/lang/Object"}});
    ASSERT_FALSE(supers.at("result").at("isError")) << supers.dump();
    ASSERT_EQ(2U, payload(supers).at("items").size());
    //sortReferences 全序（sourceClass 字节序）：mcp/Second 先于 mcp/Subject。
    EXPECT_EQ("mcp/Second", payload(supers).at("items").at(0).at("source").at("class"));
    EXPECT_EQ("mcp/Subject", payload(supers).at("items").at(1).at("source").at("class"));
    for (const auto& item : payload(supers).at("items")) {
        EXPECT_EQ("java/lang/Object", item.at("target"));
        EXPECT_EQ("type", item.at("kind"));
        EXPECT_EQ("", item.at("source").at("method").get<std::string>());
        EXPECT_EQ("", item.at("source").at("descriptor").get<std::string>());
        EXPECT_EQ(-1, item.at("source").at("instruction_index"));
        EXPECT_FALSE(item.at("source").at("supporting").get<bool>());
        EXPECT_EQ("", item.at("source").at("support_id").get<std::string>());
    }

    //method 分支干净态空集（fixture 的 value()I 无调用点）：DB 路径照常答
    //空页，行键/分页形状与引擎一致。
    const auto noCallers = call(server,
                                "xref_query",
                                {{"project_id", projectId},
                                 {"kind", "references"},
                                 {"target", "method"},
                                 {"direction", "to"},
                                 {"owner", "mcp/Second"},
                                 {"name", "value"},
                                 {"descriptor", "()I"}});
    ASSERT_FALSE(noCallers.at("result").at("isError")) << noCallers.dump();
    EXPECT_EQ(0U, payload(noCallers).at("items").size());
}

///kind=references DB-first 的编辑增量新鲜度：jasm 编辑给 mcp/Subject.value
///注入对 mcp/Second.value 的调用后，commit 挂 analysis_edit_sync 把受影响
///方法体的引用切片追平工作副本——DB 路径不回落引擎，direction=from
///（referencesFromMember 的 Method 行切片，type 行不进结果）与
/// direction=to（编辑前空集 → 编辑后一命中）都答新态；若增量同步缺席，
///两查仍答旧态空集（此断言即翻车）。挂载态守卫回落引擎的对照面由
/// ExposesProjectQueriesAndTransactionalEdits 的支撑挂载断言覆盖。
TEST(McpServerTest, XrefReferencesStayFreshAfterEditsViaIncrementalSync)
{
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //编辑前基线：mcp/Second.value 无调用方（DB 开期导入态）。
    const auto before = call(server,
                             "xref_query",
                             {{"project_id", projectId},
                              {"kind", "references"},
                              {"target", "method"},
                              {"direction", "to"},
                              {"owner", "mcp/Second"},
                              {"name", "value"},
                              {"descriptor", "()I"}});
    ASSERT_FALSE(before.at("result").at("isError")) << before.dump();
    EXPECT_EQ(0U, payload(before).at("items").size());

    //整类 jasm 重汇编：value 方法体换成 invokestatic mcp/Second.value + ireturn
    //（类名/字段保持不动，满足 jasm 编辑的类名匹配资源门禁）。
    const std::string source = R"JASM(.super java/lang/Object
.class public super mcp/Subject {
  .field public counter I
  .method public static value ()I {
    code: {
      invokestatic mcp/Second.value ()I
      ireturn
    }
  }
}
)JASM";
    const auto edited = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "jasm"},
                              {"uri", "mcp/Subject.class"},
                              {"source_name", "Subject.jasm"},
                              {"source", source},
                              {"expected_revision", 0}});
    ASSERT_FALSE(edited.at("result").at("isError")) << edited.dump();
    EXPECT_TRUE(payload(edited).at("committed").get<bool>());
    EXPECT_FALSE(payload(edited).at("analysis_sync").at("failed").get<bool>());

    //from（method_callees resolved=false → referencesFromMember）：一命中，
    //kind "direct"、target = owner+"."+name+descriptor 拼接（引擎口径）。
    const auto callees = call(server,
                              "xref_query",
                              {{"project_id", projectId},
                               {"kind", "references"},
                               {"target", "method"},
                               {"direction", "from"},
                               {"resolved", false},
                               {"owner", "mcp/Subject"},
                               {"name", "value"},
                               {"descriptor", "()I"}});
    ASSERT_FALSE(callees.at("result").at("isError")) << callees.dump();
    ASSERT_EQ(1U, payload(callees).at("items").size());
    const auto& callee = payload(callees).at("items").at(0);
    EXPECT_EQ("mcp/Subject", callee.at("source").at("class"));
    EXPECT_EQ("value", callee.at("source").at("method"));
    EXPECT_EQ("()I", callee.at("source").at("descriptor"));
    EXPECT_EQ("mcp/Second.value()I", callee.at("target"));
    EXPECT_EQ("direct", callee.at("kind"));
    EXPECT_EQ(0, callee.at("source").at("instruction_index"));

    //to（referencesToMethod）：编辑注入的调用点反查一命中，来源是新方法体。
    const auto callers = call(server,
                              "xref_query",
                              {{"project_id", projectId},
                               {"kind", "references"},
                               {"target", "method"},
                               {"direction", "to"},
                               {"owner", "mcp/Second"},
                               {"name", "value"},
                               {"descriptor", "()I"}});
    ASSERT_FALSE(callers.at("result").at("isError")) << callers.dump();
    ASSERT_EQ(1U, payload(callers).at("items").size());
    const auto& caller = payload(callers).at("items").at(0);
    EXPECT_EQ("mcp/Subject", caller.at("source").at("class"));
    EXPECT_EQ("value", caller.at("source").at("method"));
    EXPECT_EQ("()I", caller.at("source").at("descriptor"));
    EXPECT_EQ("mcp/Second.value()I", caller.at("target"));
    EXPECT_EQ("direct", caller.at("kind"));
}

///kind=strings DB-first 的常量面端到端：类常量池字符串（String 行 + 裸 utf8
///行）直查 analysis.db；非 class 资源串由引擎并集补答（并集规则：owner 与
///package_prefix 均空才参与——DB 不收资源串面，DB-only 会漏答 (a)）；string_kind
///沿用 MCP 层 post-filter，对 DB 行照常生效；真改名后 commit 挂的常量切片增量
///同步换新（utf8 行跟随新类名与新条目路径——若同步缺席，(d) 的新名断言翻车）。
TEST(McpServerTest, SearchStringsConstantFacesFromAnalysisDb)
{
    McpSessionHarness server;
    const auto fixture = writeConstantFacesFixture();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    // (a) 资源串并集：notice.txt 的 "mcp-fixture" 命中 resource_string 行，
    // 同时 Subject 的 ldc String 字面量命中 constant_pool_string 行（DB 行）。
    const auto notice = call(server,
                             "search",
                             {{"project_id", projectId},
                              {"kind", "strings"},
                              {"query", "mcp-fixture"}});
    ASSERT_FALSE(notice.at("result").at("isError")) << notice.dump();
    const auto& noticeItems = payload(notice).at("items");
    ASSERT_EQ(2U, noticeItems.size());
    const auto resourceRow = std::find_if(noticeItems.begin(),
                                          noticeItems.end(),
                                          [](const Json& item) {
                                              return item.at("kind") == "resource_string";
                                          });
    ASSERT_NE(resourceRow, noticeItems.end());
    EXPECT_EQ("notice.txt", resourceRow->at("resource_uri"));
    EXPECT_EQ("mcp-fixture", resourceRow->at("value"));
    EXPECT_EQ("string_literal", resourceRow->at("string_kind"));
    const auto literalRow = std::find_if(noticeItems.begin(),
                                         noticeItems.end(),
                                         [](const Json& item) {
                                             return item.at("kind") == "constant_pool_string";
                                         });
    ASSERT_NE(literalRow, noticeItems.end());
    EXPECT_EQ("mcp/Subject", literalRow->at("owner"));
    EXPECT_EQ("hello mcp-fixture world", literalRow->at("value"));
    EXPECT_EQ("string_literal", literalRow->at("string_kind"));

    // (b) 裸 utf8 行：字段名 counter 双类命中（constant_pool_utf8 / utf8_name）。
    const auto counter = call(server,
                              "search",
                              {{"project_id", projectId},
                               {"kind", "strings"},
                               {"query", "counter"},
                               {"string_kind", "utf8_name"}});
    ASSERT_FALSE(counter.at("result").at("isError")) << counter.dump();
    const auto& counterItems = payload(counter).at("items");
    ASSERT_EQ(2U, counterItems.size());
    for (const auto& item : counterItems) {
        EXPECT_EQ("constant_pool_utf8", item.at("kind"));
        EXPECT_EQ("counter", item.at("value"));
        EXPECT_EQ("utf8_name", item.at("string_kind"));
        const auto& owner = item.at("owner").get_ref<const std::string&>();
        EXPECT_TRUE(owner == "mcp/Subject" || owner == "mcp/Second");
    }

    // (c) string_literal 分面：只剩串常量行（utf8 行全部出局）。
    const auto literals = call(server,
                               "search",
                               {{"project_id", projectId},
                                {"kind", "strings"},
                                {"query", "hello"},
                                {"string_kind", "string_literal"}});
    ASSERT_FALSE(literals.at("result").at("isError")) << literals.dump();
    const auto& literalItems = payload(literals).at("items");
    ASSERT_EQ(1U, literalItems.size());
    EXPECT_EQ("constant_pool_string", literalItems.at(0).at("kind"));
    EXPECT_EQ("hello mcp-fixture world", literalItems.at(0).at("value"));

    // (d) 编辑新鲜度：mapping 真改名 Subject→RenamedSubject 后，类名 utf8 行
    // 跟随（owner=新名、resource_uri=搬后的条目路径），analysis_sync 披露常量
    // 切片计数且非 failed。
    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/RenamedSubject\n";
    const auto edited = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "mapping"},
                              {"format", "Tiny-V2"},
                              {"mapping_text", mapping},
                              {"policy", "all"},
                              {"expected_revision", 0}});
    ASSERT_FALSE(edited.at("result").at("isError")) << edited.dump();
    EXPECT_TRUE(payload(edited).at("committed").get<bool>());
    EXPECT_FALSE(payload(edited).at("analysis_sync").at("failed").get<bool>());
    EXPECT_GE(payload(edited).at("analysis_sync").at("constant_strings").get<unsigned>(), 1U);

    const auto renamed = call(server,
                              "search",
                              {{"project_id", projectId},
                               {"kind", "strings"},
                               {"query", "RenamedSubject"},
                               {"string_kind", "utf8_classref"}});
    ASSERT_FALSE(renamed.at("result").at("isError")) << renamed.dump();
    const auto& renamedItems = payload(renamed).at("items");
    ASSERT_FALSE(renamedItems.empty());
    for (const auto& item : renamedItems) {
        EXPECT_EQ("constant_pool_utf8", item.at("kind"));
        EXPECT_EQ("utf8_classref", item.at("string_kind"));
        EXPECT_EQ("mcp/RenamedSubject", item.at("owner"));
        EXPECT_EQ("mcp/RenamedSubject.class", item.at("resource_uri"));
    }
}

///kind=numbers DB-first 的常量面端到端：常量池数值行 + 方法体立即数行两源
///并集，收窄键（valueInt/valueBits）取候选、共享层数值等值函数终审——"1.0"
///与 "1" 命中集逐字节相同（parity）；opcode 门透传 SQL（常量池行出局）；
///jasm 编辑改方法体立即数后 commit 挂的数值切片换新（"1" 掉出 / "9" 进入）。
TEST(McpServerTest, SearchNumbersConstantFacesFromAnalysisDb)
{
    McpSessionHarness server;
    const auto fixture = writeConstantFacesFixture();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    // (a) "1" → Subject.value 方法行（instruction_number，insn=装载指令下标 4）
    //（汇编器整型装载糖若落 ldc CP 行则另有常量池行并集，两形态断言均成立）。
    const auto ones = call(server,
                           "search",
                           {{"project_id", projectId}, {"kind", "numbers"}, {"query", "1"}});
    ASSERT_FALSE(ones.at("result").at("isError")) << ones.dump();
    const auto& oneItems = payload(ones).at("items");
    const auto  oneInsn  = std::find_if(oneItems.begin(),
                                       oneItems.end(),
                                       [](const Json& item) {
                                           return item.at("kind") == "instruction_number" &&
                                                  item.at("owner") == "mcp/Subject";
                                       });
    ASSERT_NE(oneInsn, oneItems.end());
    EXPECT_EQ("value", oneInsn->at("name"));
    EXPECT_EQ("()I", oneInsn->at("descriptor"));
    EXPECT_EQ(4, oneInsn->at("instruction_index"));
    EXPECT_EQ("1", oneInsn->at("value"));
    for (const auto& item : oneItems)
        EXPECT_NE("mcp/Second", item.at("owner"));

    // (b) "2" → Second 的行（Subject 不参与）。装载形态两可（bipush 真立即数
    //或汇编器整型装载糖折 ldc CP 行），按行类别分别断言：指令行锚定方法名与
    //装载下标；常量池行 name 空、insn=-1。
    const auto twos = call(server,
                           "search",
                           {{"project_id", projectId}, {"kind", "numbers"}, {"query", "2"}});
    ASSERT_FALSE(twos.at("result").at("isError")) << twos.dump();
    const auto& twoItems = payload(twos).at("items");
    ASSERT_FALSE(twoItems.empty());
    const auto twoInsn = std::find_if(twoItems.begin(),
                                      twoItems.end(),
                                      [](const Json& item) {
                                          return item.at("kind") == "instruction_number";
                                      });
    ASSERT_NE(twoInsn, twoItems.end());
    EXPECT_EQ("value", twoInsn->at("name"));
    EXPECT_EQ("()I", twoInsn->at("descriptor"));
    EXPECT_EQ(0, twoInsn->at("instruction_index"));
    for (const auto& item : twoItems) {
        EXPECT_EQ("mcp/Second", item.at("owner"));
        EXPECT_EQ("2", item.at("value"));
        if (item.at("kind") == "constant_pool_number") {
            EXPECT_EQ("", item.at("name").get<std::string>());
            EXPECT_EQ(-1, item.at("instruction_index"));
        } else {
            EXPECT_EQ("instruction_number", item.at("kind"));
        }
    }

    // (c) 数值等值 parity："1.0" 与 "1" 命中集相同（客户端终审，非 needle 等值）。
    const auto oneDot = call(server,
                             "search",
                             {{"project_id", projectId}, {"kind", "numbers"}, {"query", "1.0"}});
    ASSERT_FALSE(oneDot.at("result").at("isError")) << oneDot.dump();
    EXPECT_EQ(payload(ones).at("items"), payload(oneDot).at("items"));

    // (d) 常量池源：ldc 100000 的 CP Integer 行（constant_pool_number，name 空、
    // insn=-1）与 ldc 指令行（instruction_number）并集。
    const auto bigs = call(server,
                           "search",
                           {{"project_id", projectId}, {"kind", "numbers"}, {"query", "100000"}});
    ASSERT_FALSE(bigs.at("result").at("isError")) << bigs.dump();
    const auto& bigItems = payload(bigs).at("items");
    const auto  bigCp    = std::find_if(bigItems.begin(),
                                   bigItems.end(),
                                   [](const Json& item) {
                                       return item.at("kind") == "constant_pool_number";
                                   });
    ASSERT_NE(bigCp, bigItems.end());
    EXPECT_EQ("mcp/Subject", bigCp->at("owner"));
    EXPECT_EQ("", bigCp->at("name").get<std::string>());
    EXPECT_EQ(-1, bigCp->at("instruction_index"));
    EXPECT_EQ("100000", bigCp->at("value"));
    const auto bigInsn = std::find_if(bigItems.begin(),
                                      bigItems.end(),
                                      [](const Json& item) {
                                          return item.at("kind") == "instruction_number" &&
                                                 item.at("value") == "100000";
                                      });
    ASSERT_NE(bigInsn, bigItems.end());
    EXPECT_EQ("value", bigInsn->at("name"));
    EXPECT_EQ(2, bigInsn->at("instruction_index"));

    // (e) opcode 门（ldc=18）透传 SQL：常量池行出局，只剩 ldc 指令行。
    const auto ldcs = call(server,
                           "search",
                           {{"project_id", projectId},
                            {"kind", "numbers"},
                            {"query", "100000"},
                            {"opcode", 18}});
    ASSERT_FALSE(ldcs.at("result").at("isError")) << ldcs.dump();
    const auto& ldcItems = payload(ldcs).at("items");
    ASSERT_EQ(1U, ldcItems.size());
    EXPECT_EQ("instruction_number", ldcItems.at(0).at("kind"));
    EXPECT_EQ("mcp/Subject", ldcItems.at(0).at("owner"));

    // (f) 编辑新鲜度：jasm 重汇编 Subject.value（装载 1 → 9），commit 挂数值
    // 切片换新——"1" 全部掉出、"9" 进入，analysis_sync 披露数值切片计数。
    const auto edited = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "jasm"},
                              {"uri", "mcp/Subject.class"},
                              {"source_name", "Subject.jasm"},
                              {"source", classSource("9")},
                              {"expected_revision", 0}});
    ASSERT_FALSE(edited.at("result").at("isError")) << edited.dump();
    EXPECT_TRUE(payload(edited).at("committed").get<bool>());
    EXPECT_FALSE(payload(edited).at("analysis_sync").at("failed").get<bool>());
    EXPECT_GE(payload(edited).at("analysis_sync").at("constant_numbers").get<unsigned>(), 1U);

    const auto afterOne = call(server,
                               "search",
                               {{"project_id", projectId}, {"kind", "numbers"}, {"query", "1"}});
    ASSERT_FALSE(afterOne.at("result").at("isError")) << afterOne.dump();
    EXPECT_EQ(0U, payload(afterOne).at("items").size());

    const auto afterNine = call(server,
                                "search",
                                {{"project_id", projectId},
                                 {"kind", "numbers"},
                                 {"query", "9"},
                                 {"owner", "mcp/Subject"}});
    ASSERT_FALSE(afterNine.at("result").at("isError")) << afterNine.dump();
    const auto& nineItems = payload(afterNine).at("items");
    ASSERT_FALSE(nineItems.empty());
    for (const auto& item : nineItems) {
        EXPECT_EQ("mcp/Subject", item.at("owner"));
        EXPECT_EQ("9", item.at("value"));
    }
}

}   //namespace
}   //namespace mjt::mcp
