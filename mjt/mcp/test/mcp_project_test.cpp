#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

//角色化挂载 jar（library/runtime 测试载荷）：逐类 jasm 装配导出。
std::filesystem::path writeMountedJar(
    const std::string&                                             fileName,
    const std::vector<std::pair<std::string, std::string>>& classes)
{
    mjbase::model::ResourceContainer container;
    for (const auto& [uri, source] : classes) {
        container.addResource(uri,
                              std::make_unique<mjbase::model::ClassResource>(uri,
                                                                    assembleFixtureClass(
                                                                        uri + ".jasm", source)));
    }
    const auto path = test::getTempOutPath(fileName);
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

//工程开合与临时工程路径（P1 接线语义）：不带 project_dir 时自动落临时工程，
//close 后随会话整体删除；带 project_dir 时 bundle 留盘、指纹与 revision 可查。
TEST(McpProjectTest, OpensTemporaryProjectAndRemovesItOnClose)
{
    McpSessionHarness server;
    const auto        fixture = writeFixture();

    const auto opened = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << payload(opened).dump();
    const auto& session = payload(opened);
    EXPECT_EQ("temporary", session.at("persistence").get<std::string>());
    EXPECT_TRUE(session.contains("persistence_hint"));
    EXPECT_TRUE(session.at("created").get<bool>());
    EXPECT_EQ(0, session.at("revision").get<int>());
    EXPECT_FALSE(session.at("project_dir").get<std::string>().empty());
    EXPECT_EQ("primary", session.at("container").at("role").get<std::string>());
    EXPECT_EQ(0U,
              session.at("container").at("fingerprint").get<std::string>().find("sha256:"));
    const auto projectDir = session.at("project_dir").get<std::string>();
    const auto projectId  = session.at("project_id").get<std::string>();
    EXPECT_TRUE(std::filesystem::exists(projectDir));

    const auto info = call(server, "project_info", {{"project_id", projectId}});
    ASSERT_FALSE(info.at("result").at("isError")) << payload(info).dump();
    const auto& overview = payload(info);
    EXPECT_EQ(projectDir, overview.at("project_dir").get<std::string>());
    EXPECT_EQ("temporary", overview.at("persistence").get<std::string>());
    EXPECT_EQ(1U, overview.at("containers").size());
    EXPECT_EQ("primary", overview.at("containers").at(0).at("role").get<std::string>());
    EXPECT_EQ(overview.at("containers").at(0).at("fingerprint"),
              session.at("container").at("fingerprint"));
    EXPECT_TRUE(overview.at("ledger").at("bound").get<bool>());
    EXPECT_TRUE(overview.at("ledger").at("compression").at("auto_compact").get<bool>());
    EXPECT_TRUE(overview.at("store_lock").is_object());
    EXPECT_GT(overview.at("store_lock").at("pid").get<int>(), 0);
    EXPECT_EQ(2U, overview.at("decompilers").size());

    const auto closed = call(server, "project_close", {{"project_id", projectId}});
    ASSERT_FALSE(closed.at("result").at("isError")) << payload(closed).dump();
    EXPECT_TRUE(payload(closed).at("flushed").get<bool>());
    std::error_code ec;
    EXPECT_FALSE(std::filesystem::exists(projectDir, ec));

    expectDirectToolError(call(server, "project_info", {{"project_id", projectId}}).at("result"),
                          "project_info", ca::StatusCode::NOT_FOUND, "project not found");
}

//台账与书签跨会话存活：显式工程目录下提交编辑、打书签、restore 回书签位，
//close 落盘；关服务重开同工程后事务行与书签行可列览，undo 沿台账回退且
//revision 归零重计（会话内乐观控制语义不变）。
TEST(McpProjectTest, PersistsLedgerAndBookmarksAcrossSessions)
{
    const auto fixture = writeFixture();
    const auto bundle  = test::getTempOutPath("mjt_mcp_project_bundle");
    std::error_code ec;
    std::filesystem::remove_all(bundle, ec);

    const std::string projectId = "persist";
    {
        McpSessionHarness server;
        const auto opened = call(server,
                                 "project_open",
                                 {{"path", fixture.string()},
                                  {"project_dir", bundle.string()},
                                  {"project_id", projectId}});
        ASSERT_FALSE(opened.at("result").at("isError")) << payload(opened).dump();
        EXPECT_TRUE(payload(opened).at("created").get<bool>());
        const auto closed = call(server, "project_close", {{"project_id", projectId}});
        ASSERT_FALSE(closed.at("result").at("isError"));
    }
    {
        McpSessionHarness server;
        const auto opened = call(server,
                                 "project_open",
                                 {{"path", fixture.string()},
                                  {"project_dir", bundle.string()},
                                  {"project_id", projectId}});
        ASSERT_FALSE(opened.at("result").at("isError")) << payload(opened).dump();
        EXPECT_FALSE(payload(opened).at("created").get<bool>());

        //编辑 → 书签 → 再编辑 → restore 回书签位（会话内）→ close 落盘：
        //flush 持有 pending 事务时书签行随活动链一并落盘。
        const auto edited = call(server,
                                 "edit_apply",
                                 {{"project_id", projectId},
                                  {"kind", "jasm"},
                                  {"uri", "mcp/Subject.class"},
                                  {"source", classSource("42")},
                                  {"expected_revision", 0}});
        ASSERT_FALSE(edited.at("result").at("isError")) << payload(edited).dump();
        EXPECT_EQ(1, payload(edited).at("revision").get<int>());

        const auto saved = call(server,
                                "edit_history",
                                {{"project_id", projectId},
                                 {"action", "savepoint"},
                                 {"name", "sp-one"}});
        ASSERT_FALSE(saved.at("result").at("isError")) << payload(saved).dump();
        EXPECT_EQ(1, payload(saved).at("revision").get<int>());

        const auto editedAgain = call(server,
                                      "edit_apply",
                                      {{"project_id", projectId},
                                       {"kind", "jasm"},
                                       {"uri", "mcp/Subject.class"},
                                       {"source", classSource("7")},
                                       {"expected_revision", 1}});
        ASSERT_FALSE(editedAgain.at("result").at("isError"));
        EXPECT_EQ(2, payload(editedAgain).at("revision").get<int>());

        const auto restored = call(server,
                                   "edit_history",
                                   {{"project_id", projectId},
                                    {"action", "restore"},
                                    {"name", "sp-one"},
                                    {"expected_revision", 2}});
        ASSERT_FALSE(restored.at("result").at("isError")) << payload(restored).dump();
        EXPECT_EQ(3, payload(restored).at("revision").get<int>());

        const auto closed = call(server, "project_close", {{"project_id", projectId}});
        ASSERT_FALSE(closed.at("result").at("isError"));
    }
    {
        //新会话重开：revision 归零、工作副本物化到头事务位；list 触发 flush
        //后活动链三事务 + 书签同页可读；undo 沿台账单步回退。
        McpSessionHarness server;
        const auto opened = call(server,
                                 "project_open",
                                 {{"path", fixture.string()},
                                  {"project_dir", bundle.string()},
                                  {"project_id", projectId}});
        ASSERT_FALSE(opened.at("result").at("isError"));
        EXPECT_EQ(0, payload(opened).at("revision").get<int>());

        const auto listed = call(server,
                                 "edit_history",
                                 {{"project_id", projectId},
                                  {"action", "list"},
                                  {"limit", 10}});
        ASSERT_FALSE(listed.at("result").at("isError")) << payload(listed).dump();
        ASSERT_EQ(4U, payload(listed).at("total").get<unsigned>());
        EXPECT_EQ("txn", payload(listed).at("items").at(0).at("row").get<std::string>());
        EXPECT_EQ("mcp/Subject.class",
                  payload(listed).at("items").at(0).at("entries").at(0).at("uri")
                      .get<std::string>());
        EXPECT_EQ("replace",
                  payload(listed).at("items").at(0).at("entries").at(0).at("op")
                      .get<std::string>());
        EXPECT_EQ(0U,
                  payload(listed).at("items").at(0).at("entries").at(0).at("before")
                      .get<std::string>()
                      .find("sha256:"));
        const auto& bookmarkRow = payload(listed).at("items").at(3);
        EXPECT_EQ("bookmark", bookmarkRow.at("row").get<std::string>());
        EXPECT_EQ("sp-one", bookmarkRow.at("name").get<std::string>());
        EXPECT_EQ(1, bookmarkRow.at("txn").get<int>());

        const auto undone = call(server,
                                 "edit_history",
                                 {{"project_id", projectId},
                                  {"action", "undo"},
                                  {"expected_revision", 0}});
        ASSERT_FALSE(undone.at("result").at("isError")) << payload(undone).dump();
        EXPECT_EQ(1, payload(undone).at("revision").get<int>());

        const auto closed = call(server, "project_close", {{"project_id", projectId}});
        ASSERT_FALSE(closed.at("result").at("isError"));
    }
    std::filesystem::remove_all(bundle, ec);
}

//手动压缩经 MCP 触发：报告列明保留点与被压缩区间，基线 section 落盘，
//书签与 undo 窗口按保留参数存活。
TEST(McpProjectTest, CompactsLedgerThroughMcp)
{
    McpSessionHarness server;
    const auto        fixture   = writeFixture();
    const auto        opened    = call(server, "project_open", {{"path", fixture.string()}});
    const auto        projectId = payload(opened).at("project_id").get<std::string>();

    for (int round = 0; round < 2; ++round) {
        const auto applied = call(server,
                                  "edit_apply",
                                  {{"project_id", projectId},
                                   {"kind", "jasm"},
                                   {"uri", "mcp/Subject.class"},
                                   {"source", round == 0 ? classSource("42") : classSource("1")},
                                   {"expected_revision", static_cast<ca::u64>(round)}});
        ASSERT_FALSE(applied.at("result").at("isError")) << payload(applied).dump();
    }

    const auto saved = call(server,
                            "edit_history",
                            {{"project_id", projectId},
                             {"action", "savepoint"},
                             {"name", "after-edits"}});
    ASSERT_FALSE(saved.at("result").at("isError"));

    const auto compacted = call(server,
                                "edit_history",
                                {{"project_id", projectId},
                                 {"action", "compact"},
                                 {"keep_recent_bookmarks", 1},
                                 {"keep_undo_depth", 1}});
    ASSERT_FALSE(compacted.at("result").at("isError")) << payload(compacted).dump();
    EXPECT_TRUE(payload(compacted).at("executed").get<bool>());
    EXPECT_EQ(0U,
              payload(compacted).at("baseline_section").get<std::string>().find("edit-baseline-"));
    EXPECT_LT(0, payload(compacted).at("dropped_transactions").get<int>());

    const auto info = call(server, "project_info", {{"project_id", projectId}});
    ASSERT_FALSE(info.at("result").at("isError"));
    const auto& ledger = payload(info).at("ledger");
    EXPECT_TRUE(ledger.at("has_baseline").get<bool>());
    EXPECT_LT(0, ledger.at("generation").get<int>());
    ASSERT_EQ(1U, ledger.at("bookmarks").size());
    EXPECT_EQ("after-edits", ledger.at("bookmarks").at(0).at("name").get<std::string>());

    //压缩后 undo 窗口仍可用（keep_undo_depth=1 承诺）。
    const auto undone = call(server,
                             "edit_history",
                             {{"project_id", projectId},
                              {"action", "undo"},
                              {"expected_revision", payload(compacted).at("revision")}});
    ASSERT_FALSE(undone.at("result").at("isError")) << payload(undone).dump();
    const auto closed = call(server, "project_close", {{"project_id", projectId}});
    ASSERT_FALSE(closed.at("result").at("isError"));
}

//store.lock 单写者：活进程持锁时第二个服务开同工程被拒，锁释放后可开。
TEST(McpProjectTest, EnforcesSingleWriterStoreLock)
{
    const auto fixture = writeFixture();
    const auto bundle  = test::getTempOutPath("mjt_mcp_project_lock");
    std::error_code ec;
    std::filesystem::remove_all(bundle, ec);

    McpSessionHarness first;
    const auto opened = call(first,
                             "project_open",
                             {{"path", fixture.string()}, {"project_dir", bundle.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));

    McpSessionHarness second;
    const auto rejected = call(second,
                               "project_open",
                               {{"path", fixture.string()}, {"project_dir", bundle.string()}});
    ASSERT_TRUE(rejected.at("result").at("isError"));
    const auto& details = rejected.at("result").at("structuredContent");
    EXPECT_NE(std::string::npos,
              details.at("error").get_ref<const std::string&>().find("open project failed"))
        << details.dump();

    const auto closed = call(first,
                             "project_close",
                             {{"project_id", payload(opened).at("project_id")}});
    ASSERT_FALSE(closed.at("result").at("isError"));
    const auto reopened = call(second,
                               "project_open",
                               {{"path", fixture.string()}, {"project_dir", bundle.string()}});
    ASSERT_FALSE(reopened.at("result").at("isError")) << payload(reopened).dump();
    EXPECT_FALSE(payload(reopened).at("created").get<bool>());
    const auto cleanup = call(second,
                              "project_close",
                              {{"project_id", payload(reopened).at("project_id")}});
    ASSERT_FALSE(cleanup.at("result").at("isError"));
    std::filesystem::remove_all(bundle, ec);
}

//条目级反编译缓存契约（RFC 0001 P3 接线）：同参数二次 decompile 第二次命中且
//输出一致；编辑类字节后失效（entryDigest 随工作副本字节变化）；换引擎后失效
//（produced_by 自述串变化）；project_close 的合并 flush 使缓存跨会话存活；
//project_info 报告缓存健康（槽清单 + 会话命中计数）。
TEST(McpProjectTest, CachesDecompileByKeyAndFlushesOnClose)
{
    const auto fixture = writeFixture();
    const auto bundle  = test::getTempOutPath("mjt_mcp_cache_bundle");
    std::error_code ec;
    std::filesystem::remove_all(bundle, ec);

    const auto fakeDecompiler = test::getTempOutPath("fake-mcp-decompiler.jar");
    std::ofstream(fakeDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");

    const std::string projectId = "cache";
    {
        McpSessionHarness server;
        const auto opened = call(server,
                                 "project_open",
                                 {{"path", fixture.string()},
                                  {"project_dir", bundle.string()},
                                  {"project_id", projectId}});
        ASSERT_FALSE(opened.at("result").at("isError")) << payload(opened).dump();

        const auto decompileOnce = [&](const Json& extra) {
            Json args = {{"project_id", projectId},
                         {"class", "mcp/Subject"},
                         {"decompiler_jar", fakeDecompiler.string()},
                         {"java", fakeJava.string()}};
            for (const auto& [key, value] : extra.items()) args[key] = value;
            return call(server, "decompile", args);
        };

        const auto first = decompileOnce(Json::object());
        ASSERT_FALSE(first.at("result").at("isError")) << payload(first).dump();
        EXPECT_FALSE(payload(first).contains("cached"));
        const auto source = payload(first).at("source").get<std::string>();
        EXPECT_NE(std::string::npos, source.find("public class Subject"));

        const auto second = decompileOnce(Json::object());
        ASSERT_FALSE(second.at("result").at("isError"));
        EXPECT_TRUE(payload(second).at("cached").get<bool>());
        EXPECT_EQ(source, payload(second).at("source").get<std::string>());

        //换引擎失效：不同 produced_by 自述串落不同槽，未命中重算。
        const auto vineflower = decompileOnce(Json{{"decompiler", "vineflower"}});
        ASSERT_FALSE(vineflower.at("result").at("isError")) << payload(vineflower).dump();
        EXPECT_FALSE(payload(vineflower).contains("cached"));

        //改类字节失效：替换类字节后 entryDigest 变化，未命中重算。
        const auto replaced = assembleFixtureClass("Subject.jasm", classSource("9"));
        const auto edited   = call(server,
                                 "edit_apply",
                                 {{"project_id", projectId},
                                  {"kind", "class_bytes"},
                                  {"class_uri", "mcp/Subject"},
                                  {"content_base64", encodeBase64(replaced)}});
        ASSERT_FALSE(edited.at("result").at("isError")) << payload(edited).dump();
        const auto afterEdit = decompileOnce(Json::object());
        ASSERT_FALSE(afterEdit.at("result").at("isError"));
        EXPECT_FALSE(payload(afterEdit).contains("cached"));

        //缓存健康：flush 后两槽在册（cfr/vineflower 各一），命中 1、未命中 3。
        const auto info = call(server, "project_info", {{"project_id", projectId}});
        ASSERT_FALSE(info.at("result").at("isError"));
        const auto& cache = payload(info).at("cache");
        EXPECT_TRUE(cache.at("bound").get<bool>());
        EXPECT_EQ(2U, cache.at("decompile_slots").get<ca::usize>());
        EXPECT_EQ(1U, cache.at("session_hits").get<ca::usize>());
        EXPECT_EQ(3U, cache.at("session_misses").get<ca::usize>());
        EXPECT_EQ(0U, cache.at("pending_records").get<ca::usize>());

        //async 命中短路：作业契约面不变（poll 收敛），结果带缓存源。
        const auto async = decompileOnce(Json{{"async", true}});
        ASSERT_FALSE(async.at("result").at("isError"));
        const auto asyncJobId = payload(async).at("job_id").get<std::string>();
        Json completedJob;
        for (int attempt = 0; attempt < 100; ++attempt) {
            completedJob = call(server,
                                "decompile",
                                {{"project_id", projectId}, {"action", "poll"}, {"job_id", asyncJobId}});
            ASSERT_FALSE(completedJob.at("result").at("isError"));
            if (payload(completedJob).at("state") == "succeeded") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ASSERT_EQ("succeeded", payload(completedJob).at("state"));
        EXPECT_TRUE(payload(completedJob).at("result").at("cached").get<bool>());

        const auto closed = call(server, "project_close", {{"project_id", projectId}});
        ASSERT_FALSE(closed.at("result").at("isError"));
    }

    //close 的合并 flush 已落盘：重开同 bundle 首次 decompile 即命中（跨会话）。
    {
        McpSessionHarness server;
        const auto opened = call(server,
                                 "project_open",
                                 {{"path", fixture.string()},
                                  {"project_dir", bundle.string()},
                                  {"project_id", projectId}});
        ASSERT_FALSE(opened.at("result").at("isError")) << payload(opened).dump();
        const auto decompile = call(server,
                                    "decompile",
                                    {{"project_id", projectId},
                                     {"class", "mcp/Subject"},
                                     {"decompiler_jar", fakeDecompiler.string()},
                                     {"java", fakeJava.string()}});
        ASSERT_FALSE(decompile.at("result").at("isError")) << payload(decompile).dump();
        EXPECT_TRUE(payload(decompile).at("cached").get<bool>());
        const auto info = call(server, "project_info", {{"project_id", projectId}});
        ASSERT_FALSE(info.at("result").at("isError"));
        EXPECT_EQ(1U, payload(info).at("cache").at("session_hits").get<ca::usize>());
        EXPECT_EQ(0U, payload(info).at("cache").at("session_misses").get<ca::usize>());
        const auto closed = call(server, "project_close", {{"project_id", projectId}});
        ASSERT_FALSE(closed.at("result").at("isError"));
    }
    std::filesystem::remove_all(bundle, ec);
}

//decompile 后端 rc=0 但目标类缺席（类级异常只上 stderr、后端整体照常退出）：
//错误消息附 backend stderr 根因摘要（最后一个 Caused by 行），分诊
//「输入触发后端内部异常」与「工具侧问题」不必再手工跑 java -jar 复现。
TEST(McpProjectTest, DecompileMissingTargetCarriesBackendSummary)
{
    const auto fixture = writeFixture();
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //partial 假反编译器：产出 mcp/Other.java、stderr 打 CFR 风格异常栈、rc=0。
    const auto partialDecompiler = test::getTempOutPath("fake-mcp-decompiler-partial.jar");
    std::ofstream(partialDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");

    const auto failed = call(server,
                             "decompile",
                             {{"project_id", projectId},
                              {"class", "mcp/Subject"},
                              {"decompiler_jar", partialDecompiler.string()},
                              {"java", fakeJava.string()}});
    ASSERT_TRUE(failed.at("result").at("isError")) << payload(failed).dump();
    const auto& message =
        payload(failed).at("status").at("message").get_ref<const std::string&>();
    EXPECT_NE(std::string::npos,
              message.find("反编译器未生成目标 class 的源码: mcp/Subject"));
    EXPECT_NE(std::string::npos, message.find("[cfr]"));
    EXPECT_NE(std::string::npos,
              message.find("Caused by: java.lang.IllegalStateException: deep cause for mcp/Subject"));
    //根因行取最后一个 Caused by，WARNING 行不入选。
    EXPECT_EQ(std::string::npos, message.find("WARNING: skipping class"));
}

//角色化多容器接线（RFC 0001 P5）：library/runtime 进 MountedContainerSet 并
//登记 project.json（schema v2 role/parent）；卸载重建解析序、被遮蔽副本浮现
//并透传；search/xref_query 的 role 过滤按挂载栈归位；primary 拒绝语义保持。
TEST(McpProjectTest, MountsRoleContainersWithShadowTrackingAndRoleFilters)
{
    const auto fixture = writeFixture();
    const auto libHelper =
        ".super java/lang/Object\n"
        ".class public super mcp/LibHelper {\n"
        "  .method public static help ()I {\n"
        "    code: {\n"
        "      bipush 7\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "  .method public static wrap ()I {\n"
        "    code: {\n"
        "      invokestatic mcp/LibHelper.help ()I\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
    const auto sharedOne = classSourceFor("mcp/Shared", "1");
    const auto library   = writeMountedJar("mjt_mcp_role_library.jar",
                                         {{"mcp/LibHelper.class", libHelper},
                                          {"mcp/Shared.class", sharedOne}});
    const auto runtime =
        writeMountedJar("mjt_mcp_role_runtime.jar", {{"mcp/Shared.class", classSourceFor("mcp/Shared", "2")}});

    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << payload(opened).dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //primary 拒绝语义保持。
    const auto rejected =
        call(server, "container_mount", {{"project_id", projectId}, {"action", "mount"}, {"role", "primary"},
                                          {"path", library.string()}});
    EXPECT_TRUE(rejected.at("result").at("isError"));

    //library 挂载：栈内无重复 URI，无遮蔽记录。
    const auto mountLibrary = call(server,
                                   "container_mount",
                                   {{"project_id", projectId},
                                    {"action", "mount"},
                                    {"role", "library"},
                                    {"path", library.string()}});
    ASSERT_FALSE(mountLibrary.at("result").at("isError")) << payload(mountLibrary).dump();
    const auto libraryId = payload(mountLibrary).at("container").at("id").get<std::string>();
    EXPECT_EQ("library", payload(mountLibrary).at("container").at("role").get<std::string>());
    EXPECT_FALSE(payload(mountLibrary).contains("shadowed"));

    //runtime 挂载：Shared 同 URI 双容器，library（解析序在前）胜出，runtime
    //副本被遮蔽——新增遮蔽记录透传。
    const auto mountRuntime = call(server,
                                   "container_mount",
                                   {{"project_id", projectId},
                                    {"action", "mount"},
                                    {"role", "runtime"},
                                    {"path", runtime.string()}});
    ASSERT_FALSE(mountRuntime.at("result").at("isError")) << payload(mountRuntime).dump();
    const auto runtimeId = payload(mountRuntime).at("container").at("id").get<std::string>();
    ASSERT_TRUE(payload(mountRuntime).contains("shadowed"));
    //shadowed 记录按 uri 排序（重建序不稳定）；jar 写出会合成父目录条目
    //（mcp/），同样参与遮蔽，故按 uri 定位断言而非下标。
    const auto& runtimeShadowed = payload(mountRuntime).at("shadowed");
    const auto shadowedShared   = std::find_if(runtimeShadowed.begin(),
                                             runtimeShadowed.end(),
                                             [](const Json& item) {
                                                 return item.at("uri") == "mcp/Shared.class";
                                             });
    ASSERT_TRUE(shadowedShared != runtimeShadowed.end()) << runtimeShadowed.dump();
    EXPECT_EQ(libraryId, shadowedShared->at("winner").get<std::string>());
    EXPECT_EQ(runtimeId, shadowedShared->at("shadowed").get<std::string>());

    //project_info 报告三角色容器面。
    const auto info = call(server, "project_info", {{"project_id", projectId}});
    ASSERT_FALSE(info.at("result").at("isError"));
    const auto hasRole = [](const Json& doc, const std::string& role) {
        for (const auto& item : payload(doc).at("containers"))
            if (item.at("role") == role) return true;
        return false;
    };
    EXPECT_TRUE(hasRole(info, "primary"));
    EXPECT_TRUE(hasRole(info, "library"));
    EXPECT_TRUE(hasRole(info, "runtime"));

    //角色过滤搜索：library 类清单与 runtime 资源清单各归其位（复合挂载链
    //地址 `<containerId>!/entry`）。
    const auto librarySearch = call(server,
                                    "search",
                                    {{"project_id", projectId},
                                     {"kind", "classes"},
                                     {"query", "LibHelper"},
                                     {"role", "library"}});
    ASSERT_FALSE(librarySearch.at("result").at("isError")) << payload(librarySearch).dump();
    ASSERT_EQ(1U, payload(librarySearch).at("items").size());
    EXPECT_EQ("mcp/LibHelper", payload(librarySearch).at("items").at(0).at("class").get<std::string>());
    EXPECT_EQ(libraryId + "!/mcp/LibHelper.class",
              payload(librarySearch).at("items").at(0).at("uri").get<std::string>());
    const auto runtimeSearch = call(server,
                                    "search",
                                    {{"project_id", projectId},
                                     {"kind", "resources"},
                                     {"query", "Shared"},
                                     {"role", "runtime"}});
    ASSERT_FALSE(runtimeSearch.at("result").at("isError"));
    ASSERT_EQ(1U, payload(runtimeSearch).at("items").size());
    EXPECT_EQ(runtimeId + "!/mcp/Shared.class",
              payload(runtimeSearch).at("items").at(0).at("uri").get<std::string>());

    //xref 跨容器：主容器字节改为调用 library 的 LibHelper.help 后，精确引用
    //查询覆盖 primary（Subject.value）与 library（wrap）两侧站点，role 过滤
    //按容器归位。
    const auto caller =
        ".super java/lang/Object\n"
        ".class public super mcp/Subject {\n"
        "  .field public counter I\n"
        "  .method public static value ()I {\n"
        "    code: {\n"
        "      invokestatic mcp/LibHelper.help ()I\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
    const auto edited = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "class_bytes"},
                              {"class_uri", "mcp/Subject"},
                              {"content_base64", encodeBase64(assembleFixtureClass("Subject.jasm", caller))}});
    ASSERT_FALSE(edited.at("result").at("isError")) << payload(edited).dump();
    const auto xrefArgs = Json{{"project_id", projectId},
                               {"kind", "references"},
                               {"target", "method"},
                               {"owner", "mcp/LibHelper"},
                               {"name", "help"},
                               {"descriptor", "()I"}};
    const auto xrefAll = call(server, "xref_query", xrefArgs);
    ASSERT_FALSE(xrefAll.at("result").at("isError")) << payload(xrefAll).dump();
    ASSERT_EQ(2U, payload(xrefAll).at("items").size());
    Json xrefLibraryArgs = xrefArgs;
    xrefLibraryArgs["role"] = "library";
    const auto xrefLibrary = call(server, "xref_query", xrefLibraryArgs);
    ASSERT_FALSE(xrefLibrary.at("result").at("isError"));
    ASSERT_EQ(1U, payload(xrefLibrary).at("items").size());
    EXPECT_EQ("mcp/LibHelper", payload(xrefLibrary).at("items").at(0).at("source").at("class").get<std::string>());
    Json xrefPrimaryArgs = xrefArgs;
    xrefPrimaryArgs["role"] = "primary";
    const auto xrefPrimary = call(server, "xref_query", xrefPrimaryArgs);
    ASSERT_FALSE(xrefPrimary.at("result").at("isError"));
    ASSERT_EQ(1U, payload(xrefPrimary).at("items").size());
    EXPECT_EQ("mcp/Subject", payload(xrefPrimary).at("items").at(0).at("source").at("class").get<std::string>());

    //library 卸载：解析序重建，此前被 library 遮蔽的 runtime Shared 副本浮现
    //并透传；role 过滤与 project_info 同步收窄。
    const auto detach = call(server,
                             "container_mount",
                             {{"project_id", projectId},
                              {"action", "detach"},
                              {"role", "library"},
                              {"support_id", libraryId}});
    ASSERT_FALSE(detach.at("result").at("isError")) << payload(detach).dump();
    ASSERT_TRUE(payload(detach).contains("surfaced")) << payload(detach).dump();
    const auto& surfaced = payload(detach).at("surfaced");
    const auto surfacedShared =
        std::find_if(surfaced.begin(),
                     surfaced.end(),
                     [](const Json& item) {
                         return item.at("uri") == "mcp/Shared.class";
                     });
    ASSERT_TRUE(surfacedShared != surfaced.end()) << surfaced.dump();
    EXPECT_EQ(runtimeId, surfacedShared->at("container").get<std::string>());

    const auto emptySearch = call(server,
                                  "search",
                                  {{"project_id", projectId},
                                   {"kind", "classes"},
                                   {"query", "LibHelper"},
                                   {"role", "library"}});
    ASSERT_FALSE(emptySearch.at("result").at("isError"));
    EXPECT_EQ(0U, payload(emptySearch).at("items").size());
    const auto infoAfter = call(server, "project_info", {{"project_id", projectId}});
    ASSERT_FALSE(infoAfter.at("result").at("isError"));
    EXPECT_FALSE(hasRole(infoAfter, "library"));
    EXPECT_TRUE(hasRole(infoAfter, "runtime"));

    const auto detachRuntime = call(server,
                                    "container_mount",
                                    {{"project_id", projectId},
                                     {"action", "detach"},
                                     {"role", "runtime"},
                                     {"support_id", runtimeId}});
    ASSERT_FALSE(detachRuntime.at("result").at("isError"));

    const auto closed = call(server, "project_close", {{"project_id", projectId}});
    ASSERT_FALSE(closed.at("result").at("isError"));
}

}   //namespace
}   //namespace mjt::mcp
