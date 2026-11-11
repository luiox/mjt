/// @file        mcp_dex_test.cpp
/// @brief       apk/dex 输入的 MCP 全链测试：project_open 分派 → project_info
///              容器形态 → search 四档（DB-first）→ xref references/relations →
///              class_query jasm（smali 视图）→ 变更族与 decompile 守卫。
///
/// fixture：test_resource/dex 的 smalict 产物（与 workspace 层 dex 测试同源）
/// 经 test::writeRawZip 包 apk（两 dex + 资源条目）；裸 dex 形态单独开工程。

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"
#include "raw_zip_fixture.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

std::vector<ca::u8> readFileBytes(const std::filesystem::path& path)
{
    std::vector<ca::u8> bytes;
    std::ifstream       in(path, std::ios::binary);
    char                buf[4096];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
        bytes.insert(bytes.end(), buf, buf + in.gcount());
    }
    return bytes;
}

std::filesystem::path dexResource(const std::string& name)
{
    // 基线资源在仓顶层 test_resource/（与 workspace 层 dex 测试同款定位）。
    return test::getTopProjectPath() / "test_resource" / "dex" / name;
}

/// @brief apk fixture：两 dex + 资源条目（workspace 层 writeFixtureApk 同构）。
std::filesystem::path writeFixtureApk()
{
    const auto path = test::getTempOutPath("mjt_mcp_dex_fixture.apk");
    test::writeRawZip(path,
                      {{"classes.dex", readFileBytes(dexResource("hello_refs.dex"))},
                       {"classes2.dex", readFileBytes(dexResource("hello_extra.dex"))},
                       {"res/values.txt",
                        std::vector<ca::u8>{'m', 'j', 't', '-', 'd', 'e', 'x', '-'}}});
    return path;
}

TEST(McpDexSessionTest, OpensApkAndAnswersDbFirstSurfaces)
{
    McpSessionHarness server;
    const auto fixture = writeFixtureApk();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //project_info：容器形态 apk、dex 单元清单与 dex 版本（dex 会话新增投影）。
    const auto info = call(server, "project_info", {{"project_id", projectId}});
    ASSERT_FALSE(info.at("result").at("isError")) << info.dump();
    EXPECT_EQ("apk", payload(info).at("artifact_kind"));
    ASSERT_EQ(2U, payload(info).at("dex_units").size());
    EXPECT_EQ(35, payload(info).at("dex_version"));

    //search classes/strings/numbers：开期 ingest 后 DB-first 回答（dex 与 jar
    //同构灌库，查询面零改动）。
    const auto classSearch = call(server,
                                  "search",
                                  {{"project_id", projectId},
                                   {"kind", "classes"},
                                   {"query", "subject"}});
    ASSERT_FALSE(classSearch.at("result").at("isError")) << classSearch.dump();
    ASSERT_EQ(1U, payload(classSearch).at("items").size());
    EXPECT_EQ("class", payload(classSearch).at("items").at(0).at("kind"));
    EXPECT_EQ("com/mjt/dexfix/Subject", payload(classSearch).at("items").at(0).at("class"));

    const auto stringSearch = call(server,
                                   "search",
                                   {{"project_id", projectId},
                                    {"kind", "strings"},
                                    {"query", "hook-fired"}});
    ASSERT_FALSE(stringSearch.at("result").at("isError")) << stringSearch.dump();
    EXPECT_FALSE(payload(stringSearch).at("items").empty());

    const auto numberSearch = call(server,
                                   "search",
                                   {{"project_id", projectId},
                                    {"kind", "numbers"},
                                    {"query", "1234605616436508552"}});
    ASSERT_FALSE(numberSearch.at("result").at("isError")) << numberSearch.dump();
    EXPECT_FALSE(payload(numberSearch).at("items").empty());

    //search instructions：引擎活扫描路径的 dex 分域实现（const-string 行含值
    //文本可命中）。
    const auto instructionSearch = call(server,
                                        "search",
                                        {{"project_id", projectId},
                                         {"kind", "instructions"},
                                         {"query", "hook-fired"}});
    ASSERT_FALSE(instructionSearch.at("result").at("isError")) << instructionSearch.dump();
    EXPECT_FALSE(payload(instructionSearch).at("items").empty());

    //xref references（to-method）：Subject.onEvent → Base.tag 的 invoke-static
    //站点经 refs 表回答（DB-first，dex 引用扫描入库）。
    const auto callers = call(server,
                              "xref_query",
                              {{"project_id", projectId},
                               {"kind", "references"},
                               {"target", "method"},
                               {"direction", "to"},
                               {"owner", "com/mjt/dexfix/Base"},
                               {"name", "tag"},
                               {"descriptor", "()Ljava/lang/String;"}});
    ASSERT_FALSE(callers.at("result").at("isError")) << callers.dump();
    ASSERT_FALSE(payload(callers).at("items").empty());
    bool onEventCallsTag = false;
    for (const auto& item : payload(callers).at("items")) {
        EXPECT_EQ("direct", item.at("kind"));
        if (item.at("source").at("class") == "com/mjt/dexfix/Subject" &&
            item.at("source").at("method") == "onEvent") {
            onEventCallsTag = true;
            EXPECT_EQ("com/mjt/dexfix/Base.tag()Ljava/lang/String;", item.at("target"));
        }
    }
    EXPECT_TRUE(onEventCallsTag);

    //xref relations：Subject 层次闭包（dex 域 super/interfaces 面经虚面回答）。
    //Subject 是叶类：direct_subclasses/all_subclasses/implementors 全空。
    const auto relations = call(server,
                                "xref_query",
                                {{"project_id", projectId},
                                 {"kind", "relations"},
                                 {"class", "com/mjt/dexfix/Subject"}});
    ASSERT_FALSE(relations.at("result").at("isError")) << relations.dump();
    //super 链去自身（DB 直查口径）：Base + 未登记父类链尾 java/lang/Object。
    ASSERT_EQ(2U, payload(relations).at("superclasses").size());
    EXPECT_EQ("com/mjt/dexfix/Base", payload(relations).at("superclasses").at(0));
    EXPECT_EQ("java/lang/Object", payload(relations).at("superclasses").at(1));
    ASSERT_EQ(1U, payload(relations).at("interfaces").size());
    EXPECT_EQ("com/mjt/dexfix/Hook", payload(relations).at("interfaces").at(0));
    EXPECT_TRUE(payload(relations).at("direct_subclasses").empty());
    EXPECT_TRUE(payload(relations).at("all_subclasses").empty());
    EXPECT_TRUE(payload(relations).at("implementors").empty());

    //Base 的直接子类含 Subject；Hook 的实现者含 Subject（叶类事实在祖先侧）。
    const auto baseRelations = call(server,
                                    "xref_query",
                                    {{"project_id", projectId},
                                     {"kind", "relations"},
                                     {"class", "com/mjt/dexfix/Base"}});
    ASSERT_FALSE(baseRelations.at("result").at("isError")) << baseRelations.dump();
    ASSERT_EQ(1U, payload(baseRelations).at("direct_subclasses").size());
    EXPECT_EQ("com/mjt/dexfix/Subject",
              payload(baseRelations).at("direct_subclasses").at(0));
    const auto hookRelations = call(server,
                                    "xref_query",
                                    {{"project_id", projectId},
                                     {"kind", "relations"},
                                     {"class", "com/mjt/dexfix/Hook"}});
    ASSERT_FALSE(hookRelations.at("result").at("isError")) << hookRelations.dump();
    ASSERT_EQ(1U, payload(hookRelations).at("implementors").size());
    EXPECT_EQ("com/mjt/dexfix/Subject", payload(hookRelations).at("implementors").at(0));

    //class_query list：dex 类事实（resource_uri = classesN.dex 条目 uri）。
    const auto classes = call(server,
                              "class_query",
                              {{"project_id", projectId}, {"kind", "list"}});
    ASSERT_FALSE(classes.at("result").at("isError")) << classes.dump();
    ASSERT_EQ(4U, payload(classes).at("items").size());

    //class_query jasm：dex 会话文本视图走 smali（format 投影标注），内容为
    //libsmali 反汇编（.class 头 + 方法体）。
    const auto jasm = call(server,
                           "class_query",
                           {{"project_id", projectId},
                            {"kind", "jasm"},
                            {"class", "com/mjt/dexfix/Subject"}});
    ASSERT_FALSE(jasm.at("result").at("isError")) << jasm.dump();
    EXPECT_EQ("com/mjt/dexfix/Subject", payload(jasm).at("class"));
    EXPECT_EQ("smali", payload(jasm).at("format"));
    const auto& smaliText = payload(jasm).at("jasm").get_ref<const std::string&>();
    EXPECT_NE(std::string::npos, smaliText.find(".class public Lcom/mjt/dexfix/Subject;"));
    EXPECT_NE(std::string::npos, smaliText.find(".end method"));
}

TEST(McpDexSessionTest, GuardsJvmOnlyOperationsButKeepsReads)
{
    McpSessionHarness server;
    const auto opened =
        call(server, "project_open", {{"path", writeFixtureApk().string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    // JVM-only edits fail for DEX input.
    const auto edited = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "jasm"},
                              {"uri", "com/mjt/dexfix/Subject"},
                              {"source", ""},
                              {"dry_run", true}});
    ASSERT_TRUE(edited.at("result").at("isError")) << edited.dump();
    EXPECT_NE(std::string::npos,
              payload(edited)
                  .at("status")
                  .at("message")
                  .get<std::string>()
                  .find("not supported for apk/dex input"))
        << payload(edited).dump();

    // A JVM backend cannot handle DEX classes.
    const auto decompiled = call(server,
                                 "decompile",
                                 {{"project_id", projectId},
                                  {"class", "com/mjt/dexfix/Subject"},
                                  {"decompiler", "cfr"}});
    ASSERT_TRUE(decompiled.at("result").at("isError")) << decompiled.dump();
    EXPECT_NE(std::string::npos,
              payload(decompiled)
                  .at("status")
                  .at("message")
                  .get<std::string>()
                  .find("require the jadx backend"));

    //守卫不误伤读面：xref_query references（DB 分支）照常回答。
    const auto callers = call(server,
                              "xref_query",
                              {{"project_id", projectId},
                               {"kind", "references"},
                               {"target", "class"},
                               {"direction", "to"},
                               {"class", "com/mjt/dexfix/Hook"}});
    ASSERT_FALSE(callers.at("result").at("isError")) << callers.dump();
    EXPECT_FALSE(payload(callers).at("items").empty());
}

TEST(McpDexSessionTest, OpensRawDexProject)
{
    McpSessionHarness server;
    const auto opened =
        call(server, "project_open", {{"path", dexResource("hello_refs.dex").string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //裸 dex：artifact_kind=dex，单元清单一项（容器 uri = 文件路径本身）。
    const auto info = call(server, "project_info", {{"project_id", projectId}});
    ASSERT_FALSE(info.at("result").at("isError")) << info.dump();
    EXPECT_EQ("dex", payload(info).at("artifact_kind"));
    ASSERT_EQ(1U, payload(info).at("dex_units").size());

    //DB-first 查询面照常（单 dex 3 类）。
    const auto classes = call(server,
                              "class_query",
                              {{"project_id", projectId}, {"kind", "list"}});
    ASSERT_FALSE(classes.at("result").at("isError")) << classes.dump();
    ASSERT_EQ(3U, payload(classes).at("items").size());

    const auto method = call(server, "class_query",
                             {{"project_id", projectId}, {"kind", "method"},
                              {"owner", "com/mjt/dexfix/Subject"}, {"name", "onEvent"}});
    ASSERT_FALSE(method.at("result").at("isError")) << method.dump();
    EXPECT_EQ("(I)V", payload(method).at("descriptor"));
    const auto disassembled = call(server, "class_query",
                                   {{"project_id", projectId}, {"kind", "bytecode"},
                                    {"owner", "com/mjt/dexfix/Subject"}, {"name", "onEvent"},
                                    {"descriptor", "(I)V"}, {"resolved", true}});
    ASSERT_FALSE(disassembled.at("result").at("isError")) << disassembled.dump();
    EXPECT_EQ("smali", payload(disassembled).at("format"));
    const auto& instructions = payload(disassembled).at("instructions");
    ASSERT_FALSE(instructions.empty());
    EXPECT_EQ(0, instructions.front().at("index"));
    EXPECT_EQ(0, instructions.front().at("pc"));
    const auto branch = std::find_if(instructions.begin(), instructions.end(), [](const Json& row) {
        return row.at("mnemonic") == "if-eqz";
    });
    ASSERT_NE(instructions.end(), branch);
    EXPECT_EQ(instructions.back().at("pc"), branch->at("branch_target_pc"));
    EXPECT_EQ("return-void", instructions.back().at("mnemonic"));
    EXPECT_NE(std::string::npos, branch->at("operand_text").get<std::string>().find("v1"));
}

TEST(McpDexSessionTest, ReportsSwitchCasesAndFallthroughAsByteOffsets)
{
    McpSessionHarness server;
    const auto opened =
        call(server, "project_open", {{"path", dexResource("hello_refs.dex").u8string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();
    const auto derived = test::getTempOutPath("mjt_switch_fixture.dex");
    std::error_code cleanupError;
    std::filesystem::remove(derived, cleanupError);
    const auto assembled = call(server, "edit_apply",
                                {{"project_id", projectId}, {"kind", "smali"},
                                 {"class", "com/mjt/dexfix/Subject"}, {"path", derived.u8string()},
                                 {"source", R"(.class public Lcom/mjt/dexfix/Subject;
.super Lcom/mjt/dexfix/Base;
.method public static choose(I)I
    .registers 2
    packed-switch v1, :cases
    const/4 v0, -1
    return v0
    :one
    const/4 v0, 1
    return v0
    :cases
    .packed-switch 42
        :one
    .end packed-switch
.end method
)"}});
    ASSERT_FALSE(assembled.at("result").at("isError")) << assembled.dump();
    const auto reopened = call(server, "project_open", {{"path", derived.u8string()}});
    ASSERT_FALSE(reopened.at("result").at("isError")) << reopened.dump();
    const auto result = call(server, "class_query",
                             {{"project_id", payload(reopened).at("project_id")},
                              {"kind", "bytecode"}, {"owner", "com/mjt/dexfix/Subject"},
                              {"name", "choose"}});
    ASSERT_FALSE(result.at("result").at("isError")) << result.dump();
    const auto& rows = payload(result).at("instructions");
    ASSERT_GE(rows.size(), 6U);
    const auto& branch = rows.at(0);
    ASSERT_EQ("packed-switch", branch.at("mnemonic"));
    EXPECT_EQ(rows.at(1).at("pc"), branch.at("switch_default_pc"));
    EXPECT_EQ(rows.back().at("pc"), branch.at("payload_pc"));
    EXPECT_EQ(true, rows.back().at("payload"));
    const auto& cases = branch.at("switch_cases");
    ASSERT_EQ(1U, cases.size());
    EXPECT_EQ(42, cases.at(0).at("key"));
    EXPECT_EQ(rows.at(3).at("pc"), cases.at(0).at("target_pc"));
}

TEST(McpDexSessionTest, MountsApkAlongsideJarAndQueriesItsClassesAndReferences)
{
    McpSessionHarness server;
    const auto jar = test::getTempOutPath("mjt_mixed_primary.jar");
    test::writeRawZip(jar, {{"data.txt", {'j', 'a', 'r'}}});
    const auto opened = call(server, "project_open", {{"path", jar.u8string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();
    const auto mounted = call(server, "container_mount",
                              {{"project_id", projectId}, {"role", "library"},
                               {"path", writeFixtureApk().u8string()}});
    ASSERT_FALSE(mounted.at("result").at("isError")) << mounted.dump();
    const auto supportId = payload(mounted).at("container").at("support_id");

    const Json search = {{"project_id", projectId}, {"kind", "classes"},
                         {"query", "subject"}, {"role", "library"}};
    const auto classes = call(server, "search", search);
    ASSERT_FALSE(classes.at("result").at("isError")) << classes.dump();
    ASSERT_EQ(1u, payload(classes).at("items").size());
    EXPECT_EQ("com/mjt/dexfix/Subject", payload(classes).at("items").at(0).at("class"));
    const auto xrefs = call(server, "xref_query",
                            {{"project_id", projectId}, {"kind", "references"},
                             {"direction", "to"}, {"target", "class"},
                             {"class", "com/mjt/dexfix/Hook"}, {"role", "library"}});
    ASSERT_FALSE(xrefs.at("result").at("isError")) << xrefs.dump();
    EXPECT_FALSE(payload(xrefs).at("items").empty());

    const auto callees = call(server, "xref_query",
                              {{"project_id", projectId}, {"kind", "references"},
                               {"direction", "from"}, {"target", "method"},
                               {"owner", "com/mjt/dexfix/Subject"}, {"name", "onEvent"},
                               {"role", "library"}});
    ASSERT_FALSE(callees.at("result").at("isError")) << callees.dump();
    EXPECT_FALSE(payload(callees).at("items").empty());

    const auto detached = call(server, "container_mount",
                               {{"project_id", projectId}, {"role", "library"},
                                {"action", "detach"}, {"support_id", supportId}});
    ASSERT_FALSE(detached.at("result").at("isError")) << detached.dump();
    const auto after = call(server, "search", search);
    ASSERT_FALSE(after.at("result").at("isError")) << after.dump();
    EXPECT_TRUE(payload(after).at("items").empty());
}

}   // namespace
}   // namespace mjt::mcp
