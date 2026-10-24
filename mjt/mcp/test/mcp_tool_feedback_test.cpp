//工具面契约打磨回归：键统一裁决
//（class/owner 定位键、jasm/source 文本键）、错误信息风格、搜索语义
//（prefix/strings 分面/instructions 结构化过滤）、分页投影、严格参数校验、
//descriptor 诊断与省略解析。每条用例在注释标注对应反馈条目号。

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <mjbase/model/resource_container.hpp>
#include <mjbase/model/resource_helper.hpp>
#include <mjbase/model/resource_model.hpp>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

///反馈卡专用 fixture：覆盖 node_type 语义分类所需的指令族（invoke/field/
///ldc/cp_ref/var/jump/const）。汇编失败直接抛出，测试失败即 fixture 失败。
std::filesystem::path writeFeedbackFixture()
{
    const auto source =
        ".super java/lang/Object\n"
        ".class public super mcp/feedback/Ops {\n"
        "  .field public static flag Z\n"
        "  .field public counter I\n"
        "  .method public static probe (Ljava/lang/Object;)I {\n"
        "    parameters: { target },\n"
        "    code: {\n"
        "      getstatic mcp/feedback/Ops.flag Z\n"
        "      ifeq skip\n"
        "      aload target\n"
        "      pop\n"
        "      goto join\n"
        "    skip:\n"
        "      aload target\n"
        "      invokevirtual java/lang/Object.hashCode ()I\n"
        "      goto join2\n"
        "    join:\n"
        "      bipush 3\n"
        "      ireturn\n"
        "    join2:\n"
        "      new java/lang/StringBuilder\n"
        "      dup\n"
        "      invokespecial java/lang/StringBuilder.<init> ()V\n"
        "      ldc \"feedback\"\n"
        "      invokevirtual java/lang/StringBuilder.append (Ljava/lang/String;)Ljava/lang/StringBuilder;\n"
        "      pop\n"
        "      bipush 5\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "  .method public static mention ()V {\n"
        "    code: {\n"
        "      aconst_null\n"
        "      pop\n"
        "      getstatic mcp/feedback/Ops.flag Z\n"
        "      pop\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "}\n";
    const auto assembled =
        assembleFixtureClass("mcp/feedback/Ops", source);
    mjbase::model::ResourceContainer container;
    container.addResource("mcp/feedback/Ops.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "mcp/feedback/Ops.class", assembled));
    const auto path = test::getTempOutPath("mjt_mcp_feedback_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

///重载 fixture：mcp/feedback/Dup 携带同名真重载 probe ()V / probe ()I，
///用于断言 descriptor 省略时的歧义报错与候选列表。
std::filesystem::path writeOverloadFixture()
{
    const auto source =
        ".super java/lang/Object\n"
        ".class public super mcp/feedback/Dup {\n"
        "  .field public tag I\n"
        "  .method public static probe ()V {\n"
        "    code: {\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public static probe ()I {\n"
        "    code: {\n"
        "      bipush 7\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
    const auto assembled =
        assembleFixtureClass("mcp/feedback/Dup", source);
    mjbase::model::ResourceContainer container;
    container.addResource("mcp/feedback/Dup.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "mcp/feedback/Dup.class", assembled));
    const auto path = test::getTempOutPath("mjt_mcp_overload_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

//instructions[] 的 node_type 按指令语义分类；invokevirtual/getstatic/
//invokespecial/new 不再标 "ldc"，摘要之外以 jasm 为权威（工具 description 同步声明）。
TEST(McpToolFeedbackTest, NodeTypesFollowInstructionSemantics)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto bytecode = call(server,
                               "class_query",
                               {{"project_id", projectId},
                                {"kind", "bytecode"},
                                {"owner", "mcp/feedback/Ops"},
                                {"name", "probe"},
                                {"descriptor", "(Ljava/lang/Object;)I"}});
    ASSERT_FALSE(bytecode.at("result").at("isError")) << payload(bytecode).dump();
    std::map<std::string, std::string> nodeTypeByMnemonic;
    for (const auto& item : payload(bytecode).at("instructions"))
        nodeTypeByMnemonic[item.at("mnemonic").get<std::string>()] =
            item.at("node_type").get<std::string>();
    const auto expectNodeType = [&](const char* mnemonic, const char* expected) {
        SCOPED_TRACE(mnemonic);
        const auto found = nodeTypeByMnemonic.find(mnemonic);
        ASSERT_TRUE(found != nodeTypeByMnemonic.end()) << payload(bytecode).dump();
        EXPECT_EQ(expected, found->second);
    };
    expectNodeType("getstatic", "field");
    expectNodeType("invokevirtual", "invoke");
    expectNodeType("invokespecial", "invoke");
    expectNodeType("new", "cp_ref");
    expectNodeType("ldc", "ldc");
    expectNodeType("aload_0", "var");
    expectNodeType("ifeq", "jump");
    expectNodeType("goto", "jump");

    const auto mention = call(server,
                              "class_query",
                              {{"project_id", projectId},
                               {"kind", "bytecode"},
                               {"owner", "mcp/feedback/Ops"},
                               {"name", "mention"},
                               {"descriptor", "()V"}});
    ASSERT_FALSE(mention.at("result").at("isError")) << payload(mention).dump();
    for (const auto& item : payload(mention).at("instructions")) {
        if (item.at("mnemonic") == "aconst_null")
            EXPECT_EQ("const", item.at("node_type").get<std::string>());
        if (item.at("mnemonic") == "getstatic")
            EXPECT_EQ("field", item.at("node_type").get<std::string>());
    }

    //tools/list 的 class_query description 声明 jasm 权威口径。
    const auto tools = server.handle(request("tools/list"));
    const auto& listed = tools.at("result").at("tools");
    const auto classQuery = std::find_if(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "class_query";
    });
    ASSERT_NE(classQuery, listed.end());
    EXPECT_NE(std::string::npos,
              classQuery->at("description").get_ref<const std::string&>().find("jasm"));
}

//class_query 分支定位键契约与参数键白名单。类定位键 class、成员
//定位键 owner+name（descriptor 可选，省略时名称解析回填）、资源键 uri；用错
//键报「requires X; got Y」，分支不支持的键报「does not support; supported」，
//完全未声明键在契约层报 unknown parameter。
TEST(McpToolFeedbackTest, ClassQueryBranchSelectorsAreStrict)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto wrongKey = call(server,
                               "class_query",
                               {{"project_id", projectId},
                                {"kind", "class"},
                                {"owner", "mcp/feedback/Ops"}});
    ASSERT_TRUE(wrongKey.at("result").at("isError"));
    EXPECT_EQ("class_query kind=class requires class; got owner",
              payload(wrongKey).at("error").get<std::string>());
    EXPECT_EQ(static_cast<int>(ca::StatusCode::INVALID_ARGUMENT),
              payload(wrongKey).at("status_code").get<int>());

    const auto memberKind = call(server,
                                 "class_query",
                                 {{"project_id", projectId},
                                  {"kind", "bytecode"},
                                  {"class", "mcp/feedback/Ops"}});
    ASSERT_TRUE(memberKind.at("result").at("isError"));
    EXPECT_EQ("class_query kind=bytecode requires owner and name; got class",
              payload(memberKind).at("error").get<std::string>());

    const auto missingSelector = call(server,
                                      "class_query",
                                      {{"project_id", projectId}, {"kind", "jasm"}});
    ASSERT_TRUE(missingSelector.at("result").at("isError"));
    EXPECT_EQ("class_query kind=jasm requires class",
              payload(missingSelector).at("error").get<std::string>());

    const auto listWithSelector = call(server,
                                       "class_query",
                                       {{"project_id", projectId},
                                        {"kind", "list"},
                                        {"owner", "mcp/feedback/Ops"}});
    ASSERT_TRUE(listWithSelector.at("result").at("isError"));
    EXPECT_EQ("class_query kind=list requires no selector keys; got owner",
              payload(listWithSelector).at("error").get<std::string>());

    //kind 完全缺失由 schema required 拦截，非法值由 schema enum 拦截；分发层
    //的 Missing key/Invalid value 文案是内部调用兜底。target 这类「分支条件
    //必填」键不进 required，缺失由分发层报。
    const auto badKind = call(server,
                              "class_query",
                              {{"project_id", projectId}, {"kind", "nonsense"}});
    ASSERT_TRUE(badKind.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(badKind).at("error").get_ref<const std::string&>().find(
                  "not one of the allowed values"));
    const auto missingKind = call(server,
                                  "class_query",
                                  {{"project_id", projectId}});
    ASSERT_TRUE(missingKind.at("result").at("isError"));
    EXPECT_EQ(static_cast<int>(ca::StatusCode::INVALID_ARGUMENT),
              payload(missingKind).at("status_code").get<int>());
    EXPECT_NE(std::string::npos,
              payload(missingKind).at("error").get_ref<const std::string&>().find("kind"));
}

//完全未声明键在 schema additionalProperties 层拒绝（INVALID_ARGUMENT），
//分支已知键在分发层拒绝并列出该分支支持的全部键。
TEST(McpToolFeedbackTest, UnknownAndUnsupportedKeysAreRejected)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //name_glob 从未被任何 kind 声明：schema 层直接拒绝（不再静默丢弃）。
    const auto unknown = call(server,
                              "class_query",
                              {{"project_id", projectId},
                               {"kind", "list"},
                               {"name_glob", "*protect*"}});
    ASSERT_TRUE(unknown.at("result").at("isError"));
    EXPECT_EQ(static_cast<int>(ca::StatusCode::INVALID_ARGUMENT),
              payload(unknown).at("status_code").get<int>());
    EXPECT_NE(std::string::npos,
              payload(unknown).at("error").get_ref<const std::string&>().find("name_glob"));

    //分支已知键用错 kind：分发层拒绝并回显支持键表。
    const auto unsupported = call(server,
                                  "search",
                                  {{"project_id", projectId},
                                   {"kind", "classes"},
                                   {"query", "Ops"},
                                   {"opcode", 18}});
    ASSERT_TRUE(unsupported.at("result").at("isError"));
    const auto& message = payload(unsupported).at("error").get_ref<const std::string&>();
    EXPECT_NE(std::string::npos, message.find("search kind=classes does not support key(s) opcode"));
    EXPECT_NE(std::string::npos, message.find("supported: "));
    EXPECT_NE(std::string::npos, message.find("query"));
}

//分支未声明键（从未在任何分支声明的键）报 unknown parameter 文案，
//expected 列出该 kind 的全部合法键（branchKeysFor 单一事实源），不再回落到
//通用 additionalProperties 文案；声明键的分支错配仍走分发层 does-not-support。
TEST(McpToolFeedbackTest, ClassQueryUnknownKeyListsBranchKeys)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto unknown = call(server,
                              "class_query",
                              {{"project_id", projectId},
                               {"kind", "list"},
                               {"name_glob", "*protect*"}});
    ASSERT_TRUE(unknown.at("result").at("isError"));
    EXPECT_EQ(static_cast<int>(ca::StatusCode::INVALID_ARGUMENT),
              payload(unknown).at("status_code").get<int>());
    const auto& message = payload(unknown).at("error").get_ref<const std::string&>();
    EXPECT_NE(std::string::npos,
              message.find("unknown parameter \"name_glob\" for kind=list; "
                           "expected: cursor, offset, limit, page_size, fields"))
        << message;

    //成员 kind 同口径：expected 列出该 kind 的定位键三元组。
    const auto memberUnknown = call(server,
                                    "class_query",
                                    {{"project_id", projectId},
                                     {"kind", "method"},
                                     {"owner", "mcp/feedback/Ops"},
                                     {"name", "probe"},
                                     {"descriptor", "(Ljava/lang/Object;)I"},
                                     {"match_mode", "contains"}});
    ASSERT_TRUE(memberUnknown.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(memberUnknown).at("error").get_ref<const std::string&>().find(
                  "unknown parameter \"match_mode\" for kind=method; "
                  "expected: owner, name, descriptor"));
}

//xref_query 按 target 分支收敛选择器键；缺 target 报 Missing key
//（与顶层缺 kind 同风格）；分支外键拒绝。
TEST(McpToolFeedbackTest, XrefBranchSelectorsAreStrict)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto missingTarget = call(server,
                                    "xref_query",
                                    {{"project_id", projectId},
                                     {"kind", "references"},
                                     {"owner", "mcp/feedback/Ops"},
                                     {"name", "flag"},
                                     {"descriptor", "Z"}});
    ASSERT_TRUE(missingTarget.at("result").at("isError"));
    EXPECT_EQ("target: Missing key (expected method, field or class)",
              payload(missingTarget).at("error").get<std::string>());

    const auto missingKind =
        call(server, "xref_query", {{"project_id", projectId}});
    ASSERT_TRUE(missingKind.at("result").at("isError"));
    //顶层 kind 是 schema required：完全缺 key 在契约校验层拦截；分发层的
    //Missing key 文案管的是 target 这类「分支条件必填」键。
    EXPECT_EQ(static_cast<int>(ca::StatusCode::INVALID_ARGUMENT),
              payload(missingKind).at("status_code").get<int>());
    EXPECT_NE(std::string::npos,
              payload(missingKind).at("error").get_ref<const std::string&>().find("kind"));

    const auto wrongSelector = call(server,
                                    "xref_query",
                                    {{"project_id", projectId},
                                     {"kind", "references"},
                                     {"target", "class"},
                                     {"owner", "mcp/feedback/Ops"}});
    ASSERT_TRUE(wrongSelector.at("result").at("isError"));
    EXPECT_EQ("xref_query kind=references target=class direction=to requires class; got owner",
              payload(wrongSelector).at("error").get<std::string>());

    const auto memberBranch = call(server,
                                   "xref_query",
                                   {{"project_id", projectId},
                                    {"kind", "references"},
                                    {"target", "method"},
                                    {"class", "mcp/feedback/Ops"}});
    ASSERT_TRUE(memberBranch.at("result").at("isError"));
    EXPECT_EQ("xref_query kind=references target=method direction=to requires owner and "
              "name; got class",
              payload(memberBranch).at("error").get<std::string>());

    //resolved 只在 direction=from 生效；direction=from 只吃 method。
    const auto resolvedOnTo = call(server,
                                   "xref_query",
                                   {{"project_id", projectId},
                                    {"kind", "references"},
                                    {"target", "method"},
                                    {"owner", "mcp/feedback/Ops"},
                                    {"name", "probe"},
                                    {"descriptor", "(Ljava/lang/Object;)I"},
                                    {"resolved", true}});
    ASSERT_TRUE(resolvedOnTo.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(resolvedOnTo).at("error").get_ref<const std::string&>().find(
                  "does not support key(s) resolved"));

    const auto fromField = call(server,
                                "xref_query",
                                {{"project_id", projectId},
                                 {"kind", "references"},
                                 {"target", "field"},
                                 {"direction", "from"},
                                 {"owner", "mcp/feedback/Ops"},
                                 {"name", "counter"},
                                 {"descriptor", "I"}});
    ASSERT_TRUE(fromField.at("result").at("isError"));
    EXPECT_EQ("xref_query direction=from requires target=method; got field",
              payload(fromField).at("error").get<std::string>());
}

//search 的 match_mode=prefix 档（缺省 contains 保持现行为）。compute1
//类场景：前缀命中不含同串中段的扩散命中。
TEST(McpToolFeedbackTest, SearchSupportsPrefixMatchMode)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //contains：probe 命中方法名与内部名（Ops.probe 等扩散面）。
    const auto contains = call(server,
                               "search",
                               {{"project_id", projectId},
                                {"kind", "members"},
                                {"query", "probe"}});
    ASSERT_FALSE(contains.at("result").at("isError"));
    EXPECT_FALSE(payload(contains).at("items").empty());

    //prefix：成员行判别文本（name/owner/descriptor/value）前缀匹配。
    const auto prefix = call(server,
                             "search",
                             {{"project_id", projectId},
                              {"kind", "members"},
                              {"query", "probe"},
                              {"match_mode", "prefix"}});
    ASSERT_FALSE(prefix.at("result").at("isError"));
    ASSERT_EQ(payload(contains).at("items").size(), payload(prefix).at("items").size());

    const auto classesByPrefix = call(server,
                                      "search",
                                      {{"project_id", projectId},
                                       {"kind", "classes"},
                                       {"query", "mcp/feedback"},
                                       {"match_mode", "prefix"}});
    ASSERT_FALSE(classesByPrefix.at("result").at("isError"));
    ASSERT_EQ(1U, payload(classesByPrefix).at("items").size());
    EXPECT_EQ("mcp/feedback/Ops",
              payload(classesByPrefix).at("items").at(0).at("class").get<std::string>());
}

//strings 搜索分面。结果项带 string_kind，同名参数过滤；缺省全部
//（constant_pool_string/constant_pool_utf8/resource_string 混排现状不变）。
TEST(McpToolFeedbackTest, StringsSearchCarriesKindFacet)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto all = call(server,
                          "search",
                          {{"project_id", projectId}, {"kind", "strings"}, {"query", "feedback"}});
    ASSERT_FALSE(all.at("result").at("isError"));
    ASSERT_FALSE(payload(all).at("items").empty());
    for (const auto& item : payload(all).at("items")) {
        const auto& kind = item.at("kind").get_ref<const std::string&>();
        if (kind == "constant_pool_string" || kind == "resource_string")
            EXPECT_EQ("string_literal", item.at("string_kind").get<std::string>());
        else if (kind == "constant_pool_utf8")
            EXPECT_EQ("utf8_classref", item.at("string_kind").get<std::string>());
    }

    //ldc 加载的串常量在 string_literal 分面下保留。
    const auto literals = call(server,
                               "search",
                               {{"project_id", projectId},
                                {"kind", "strings"},
                                {"query", "feedback"},
                                {"string_kind", "string_literal"}});
    ASSERT_FALSE(literals.at("result").at("isError"));
    ASSERT_FALSE(payload(literals).at("items").empty());
    for (const auto& item : payload(literals).at("items"))
        EXPECT_NE("constant_pool_utf8", item.at("kind").get<std::string>());

    //utf8_classref 分面：只留常量池 UTF8 条目。
    const auto classRefs = call(server,
                                "search",
                                {{"project_id", projectId},
                                 {"kind", "strings"},
                                 {"query", "Ops"},
                                 {"string_kind", "utf8_classref"}});
    ASSERT_FALSE(classRefs.at("result").at("isError"));
    for (const auto& item : payload(classRefs).at("items")) {
        EXPECT_EQ("constant_pool_utf8", item.at("kind").get<std::string>());
        EXPECT_EQ("utf8_classref", item.at("string_kind").get<std::string>());
    }
}

//instructions 结构化过滤（mnemonic 精确、operand/cst 子串），与 query
//互斥；响应项带 mnemonic/operand/cst 结构化字段。
TEST(McpToolFeedbackTest, InstructionsSearchHasStructuredFilters)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //mnemonic 精确：只留 invokevirtual。
    const auto byMnemonic = call(server,
                                 "search",
                                 {{"project_id", projectId},
                                  {"kind", "instructions"},
                                  {"mnemonic", "invokevirtual"}});
    ASSERT_FALSE(byMnemonic.at("result").at("isError")) << payload(byMnemonic).dump();
    ASSERT_FALSE(payload(byMnemonic).at("items").empty());
    for (const auto& item : payload(byMnemonic).at("items")) {
        EXPECT_EQ("invokevirtual", item.at("mnemonic").get<std::string>());
        EXPECT_EQ("instruction", item.at("kind").get<std::string>());
    }

    //cst 子串：命中 ldc "feedback" 的常量摘要；响应项有可判别 cst 字段。
    const auto byCst = call(server,
                            "search",
                            {{"project_id", projectId},
                             {"kind", "instructions"},
                             {"cst", "feedback"}});
    ASSERT_FALSE(byCst.at("result").at("isError")) << payload(byCst).dump();
    ASSERT_FALSE(payload(byCst).at("items").empty());
    bool sawLdcFeedback = false;
    for (const auto& item : payload(byCst).at("items")) {
        EXPECT_TRUE(item.contains("mnemonic"));
        EXPECT_TRUE(item.contains("operand"));
        EXPECT_TRUE(item.contains("cst"));
        EXPECT_NE(std::string::npos,
                  item.at("cst").get_ref<const std::string&>().find("feedback"));
        if (item.at("mnemonic") == "ldc") sawLdcFeedback = true;
    }
    EXPECT_TRUE(sawLdcFeedback);

    //operand 与 query 互斥（严格切换）。
    const auto mixed = call(server,
                            "search",
                            {{"project_id", projectId},
                             {"kind", "instructions"},
                             {"query", "ldc"},
                             {"mnemonic", "ldc"}});
    ASSERT_TRUE(mixed.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(mixed).at("error").get_ref<const std::string&>().find(
                  "mutually exclusive"));
}


//分页与投影——page_size 缺省 50、与 limit 互斥；fields 投影只保留指定
//键，未知键报错并列出可用键；分支白名单拒绝非 list kind 的分页投影键。
TEST(McpToolFeedbackTest, PaginationDefaultsAndFieldProjection)
{
    McpSessionHarness server;
    const auto opened =
        call(server, "project_open", {{"path", writeLargeClassListFixture("").string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //缺省页大小 50：不带 page_size/limit 的 class_query kind=list 返回前 50 行
    //并给出 next_cursor。
    const auto firstPage =
        call(server, "class_query", {{"project_id", projectId}, {"kind", "list"}});
    ASSERT_FALSE(firstPage.at("result").at("isError")) << firstPage.dump();
    EXPECT_EQ(50U, payload(firstPage).at("items").size());
    EXPECT_EQ(210U, payload(firstPage).at("total"));
    EXPECT_EQ("50", payload(firstPage).at("next_cursor"));

    //page_size 显式生效；与 limit 互斥。
    const auto sized = call(server,
                            "class_query",
                            {{"project_id", projectId}, {"kind", "list"}, {"page_size", 7}});
    ASSERT_FALSE(sized.at("result").at("isError")) << sized.dump();
    EXPECT_EQ(7U, payload(sized).at("items").size());

    const auto both = call(server,
                           "class_query",
                           {{"project_id", projectId},
                            {"kind", "list"},
                            {"page_size", 7},
                            {"limit", 9}});
    ASSERT_TRUE(both.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(both).at("error").get_ref<const std::string&>().find(
                  "page_size and limit are mutually exclusive"));

    //fields 投影：行只保留指定键；classSummaryPage 的校验面对全量行集（本页
    //字节预算截断不影响可用键）。
    const auto projected =
        call(server,
             "class_query",
             {{"project_id", projectId}, {"kind", "list"}, {"page_size", 3}, {"fields", {"class"}}});
    ASSERT_FALSE(projected.at("result").at("isError")) << projected.dump();
    ASSERT_EQ(3U, payload(projected).at("items").size());
    for (const auto& item : payload(projected).at("items")) {
        ASSERT_EQ(1U, item.size());
        EXPECT_TRUE(item.contains("class"));
    }

    //search 的 Json 分页路径同样吃 page_size + fields。
    const auto searched = call(server,
                               "search",
                               {{"project_id", projectId},
                                {"kind", "classes"},
                                {"query", "mcp/large/Subject1"},
                                {"page_size", 4},
                                {"fields", {"class"}}});
    ASSERT_FALSE(searched.at("result").at("isError")) << searched.dump();
    ASSERT_EQ(4U, payload(searched).at("items").size());
    for (const auto& item : payload(searched).at("items")) {
        ASSERT_EQ(1U, item.size());
        EXPECT_TRUE(item.contains("class"));
    }

    //未知 fields 键：报错列出全结果集可用键。
    const auto unknownField = call(server,
                                   "class_query",
                                   {{"project_id", projectId},
                                    {"kind", "list"},
                                    {"page_size", 2},
                                    {"fields", {"bogus"}}});
    ASSERT_TRUE(unknownField.at("result").at("isError"));
    const auto fieldError = payload(unknownField).at("error").get_ref<const std::string&>();
    ASSERT_TRUE(fieldError.find("available:") != std::string::npos) << fieldError;
    EXPECT_NE(std::string::npos, fieldError.find("fields: unknown key \"bogus\""));
    //Json 键按字母序展示。
    EXPECT_NE(std::string::npos, fieldError.find("available: access, class"));

    //分支白名单：page_size/fields 只属于 kind=list，其他 kind 用了即拒绝。
    const auto wrongKind = call(server,
                                "class_query",
                                {{"project_id", projectId},
                                 {"kind", "class"},
                                 {"class", "mcp/large/Subject0"},
                                 {"page_size", 7}});
    ASSERT_TRUE(wrongKind.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(wrongKind).at("error").get_ref<const std::string&>().find(
                  "does not support key(s) page_size"));
}


//project_open 响应自述持久性——temporary 工程带 persistence_hint 指明
//bundle 随 project_close 删除、重开需带同一 project_dir；持久工程无提示。
TEST(McpToolFeedbackTest, ProjectOpenCarriesPersistenceHint)
{
    McpSessionHarness server;
    const auto fixture = writeFixture();

    const auto temporary = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(temporary.at("result").at("isError")) << temporary.dump();
    EXPECT_EQ("temporary", payload(temporary).at("persistence").get<std::string>());
    const auto& hint = payload(temporary).at("persistence_hint")
                           .get_ref<const std::string&>();
    EXPECT_NE(std::string::npos, hint.find("project_dir"));
    EXPECT_NE(std::string::npos, hint.find("project_close"));
    const auto closed =
        call(server, "project_close", {{"project_id", payload(temporary).at("project_id")}});
    ASSERT_FALSE(closed.at("result").at("isError"));

    const auto bundle =
        std::filesystem::temp_directory_path() / "mjt_mcp_p8_persistent_bundle";
    std::error_code ec;
    std::filesystem::remove_all(bundle, ec);
    const auto persistent = call(server,
                                 "project_open",
                                 {{"path", fixture.string()}, {"project_dir", bundle.string()}});
    ASSERT_FALSE(persistent.at("result").at("isError")) << persistent.dump();
    EXPECT_EQ("project", payload(persistent).at("persistence").get<std::string>());
    EXPECT_FALSE(payload(persistent).contains("persistence_hint"));
    std::filesystem::remove_all(bundle, ec);
}


//owner+name 命中但 descriptor 不匹配时响应附 diagnostics，列出宿主
//类里该名字的全部真实描述符；描述符正确或名字不存在时不诊断。
TEST(McpToolFeedbackTest, XrefDescriptorMismatchCarriesDiagnostics)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //probe 的真实描述符是 (Ljava/lang/Object;)I：传入 ()V 得到诊断候选。
    const auto methodMismatch = call(server,
                                     "xref_query",
                                     {{"project_id", projectId},
                                      {"kind", "references"},
                                      {"target", "method"},
                                      {"owner", "mcp/feedback/Ops"},
                                      {"name", "probe"},
                                      {"descriptor", "()V"}});
    ASSERT_FALSE(methodMismatch.at("result").at("isError")) << methodMismatch.dump();
    const auto& methodDiag = payload(methodMismatch).at("diagnostics");
    ASSERT_EQ(1U, methodDiag.size());
    const auto& methodMessage = methodDiag.at(0).get_ref<const std::string&>();
    EXPECT_NE(std::string::npos,
              methodMessage.find("descriptor not found for mcp/feedback/Ops.probe"));
    EXPECT_NE(std::string::npos, methodMessage.find("(Ljava/lang/Object;)I"));

    //字段同款：flag 真实描述符是 Z。
    const auto fieldMismatch = call(server,
                                    "xref_query",
                                    {{"project_id", projectId},
                                     {"kind", "references"},
                                     {"target", "field"},
                                     {"owner", "mcp/feedback/Ops"},
                                     {"name", "flag"},
                                     {"descriptor", "I"}});
    ASSERT_FALSE(fieldMismatch.at("result").at("isError")) << fieldMismatch.dump();
    const auto& fieldDiag = payload(fieldMismatch).at("diagnostics");
    ASSERT_EQ(1U, fieldDiag.size());
    EXPECT_NE(std::string::npos,
              fieldDiag.at(0).get_ref<const std::string&>().find("; candidates: Z"));

    //描述符正确：响应不带 diagnostics。
    const auto exact = call(server,
                            "xref_query",
                            {{"project_id", projectId},
                             {"kind", "references"},
                             {"target", "method"},
                             {"owner", "mcp/feedback/Ops"},
                             {"name", "probe"},
                             {"descriptor", "(Ljava/lang/Object;)I"}});
    ASSERT_FALSE(exact.at("result").at("isError")) << exact.dump();
    EXPECT_FALSE(payload(exact).contains("diagnostics"));

    //名字不存在：错误面（无诊断注入）。
    const auto absent = call(server,
                             "xref_query",
                             {{"project_id", projectId},
                              {"kind", "references"},
                              {"target", "method"},
                              {"owner", "mcp/feedback/Ops"},
                              {"name", "nope"},
                              {"descriptor", "()V"}});
    ASSERT_FALSE(absent.at("result").at("isError")) << absent.dump();
    EXPECT_FALSE(payload(absent).contains("diagnostics"));
}

//descriptor 省略时的名称解析回填——唯一命中即用，class_query 成员 kind
//与 xref target=method/field 同口径；显式 descriptor 的行为不受影响。
TEST(McpToolFeedbackTest, MethodSelectorWithoutDescriptorResolvesByName)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //kind=method 省略 descriptor：probe 唯一命中，回填 (Ljava/lang/Object;)I。
    const auto method = call(server,
                             "class_query",
                             {{"project_id", projectId},
                              {"kind", "method"},
                              {"owner", "mcp/feedback/Ops"},
                              {"name", "probe"}});
    ASSERT_FALSE(method.at("result").at("isError")) << payload(method).dump();
    EXPECT_EQ("(Ljava/lang/Object;)I", payload(method).at("descriptor").get<std::string>());

    //kind=bytecode 同口径（省略 descriptor 直接取字节码）。
    const auto bytecode = call(server,
                               "class_query",
                               {{"project_id", projectId},
                                {"kind", "bytecode"},
                                {"owner", "mcp/feedback/Ops"},
                                {"name", "probe"}});
    ASSERT_FALSE(bytecode.at("result").at("isError")) << payload(bytecode).dump();
    EXPECT_EQ("(Ljava/lang/Object;)I",
              payload(bytecode).at("method").at("descriptor").get<std::string>());

    //xref target=method 省略 descriptor 正常执行（probe 有 getstatic/…引用面
    //之外的查询零漂移，空集也合法）。
    const auto methodRefs = call(server,
                                 "xref_query",
                                 {{"project_id", projectId},
                                  {"kind", "references"},
                                  {"target", "method"},
                                  {"owner", "mcp/feedback/Ops"},
                                  {"name", "probe"}});
    ASSERT_FALSE(methodRefs.at("result").at("isError")) << payload(methodRefs).dump();

    //字段选择器同规则：flag 唯一 → 回填 Z。
    const auto fieldRefs = call(server,
                                "xref_query",
                                {{"project_id", projectId},
                                 {"kind", "references"},
                                 {"target", "field"},
                                 {"owner", "mcp/feedback/Ops"},
                                 {"name", "flag"}});
    ASSERT_FALSE(fieldRefs.at("result").at("isError")) << payload(fieldRefs).dump();
}

//真重载（同多名异描述符）报歧义并列出全部候选，响应附同一格式的
//diagnostics 数组；xref 面同口径。
TEST(McpToolFeedbackTest, AmbiguousOverloadsRejectMissingDescriptor)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeOverloadFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto bytecode = call(server,
                               "class_query",
                               {{"project_id", projectId},
                                {"kind", "bytecode"},
                                {"owner", "mcp/feedback/Dup"},
                                {"name", "probe"}});
    ASSERT_TRUE(bytecode.at("result").at("isError"));
    EXPECT_EQ(static_cast<int>(ca::StatusCode::INVALID_ARGUMENT),
              payload(bytecode).at("status_code").get<int>());
    const auto& message = payload(bytecode).at("error").get_ref<const std::string&>();
    EXPECT_NE(std::string::npos,
              message.find("descriptor is ambiguous for mcp/feedback/Dup.probe")) << message;
    EXPECT_NE(std::string::npos, message.find("candidates: ()V, ()I"));
    const auto& diagnostics = payload(bytecode).at("diagnostics");
    ASSERT_EQ(1U, diagnostics.size());
    EXPECT_NE(std::string::npos,
              diagnostics.at(0).get_ref<const std::string&>().find("candidates: ()V, ()I"));

    //xref target=method 同口径。
    const auto refs = call(server,
                           "xref_query",
                           {{"project_id", projectId},
                            {"kind", "references"},
                            {"target", "method"},
                            {"owner", "mcp/feedback/Dup"},
                            {"name", "probe"}});
    ASSERT_TRUE(refs.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(refs).at("error").get_ref<const std::string&>().find(
                  "descriptor is ambiguous for mcp/feedback/Dup.probe"));

    //显式 descriptor 不受影响：任一重载正常执行。
    const auto exact = call(server,
                            "class_query",
                            {{"project_id", projectId},
                             {"kind", "bytecode"},
                             {"owner", "mcp/feedback/Dup"},
                             {"name", "probe"},
                             {"descriptor", "()I"}});
    ASSERT_FALSE(exact.at("result").at("isError")) << payload(exact).dump();
}

//名字不存在报错并说明 owner 内无此名；名字以字段形态存在时附提示
//（复用既有的候选枚举机）。
TEST(McpToolFeedbackTest, AbsentMemberNameWithoutDescriptorIsRejected)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto absent = call(server,
                             "class_query",
                             {{"project_id", projectId},
                              {"kind", "method"},
                              {"owner", "mcp/feedback/Ops"},
                              {"name", "nope"}});
    ASSERT_TRUE(absent.at("result").at("isError"));
    EXPECT_EQ(static_cast<int>(ca::StatusCode::INVALID_ARGUMENT),
              payload(absent).at("status_code").get<int>());
    EXPECT_NE(std::string::npos,
              payload(absent).at("error").get_ref<const std::string&>().find(
                  "no method named \"nope\" in mcp/feedback/Ops"));

    //名字是字段名：报错附字段形态提示（flag 的真实描述符 Z）。
    const auto fieldNamed = call(server,
                                 "class_query",
                                 {{"project_id", projectId},
                                  {"kind", "method"},
                                  {"owner", "mcp/feedback/Ops"},
                                  {"name", "flag"}});
    ASSERT_TRUE(fieldNamed.at("result").at("isError"));
    const auto& message = payload(fieldNamed).at("error").get_ref<const std::string&>();
    EXPECT_NE(std::string::npos,
              message.find("no method named \"flag\" in mcp/feedback/Ops"));
    EXPECT_NE(std::string::npos,
              message.find("a field with this name exists (descriptor Z)"));
}

//fields 键校验按每个 kind 的静态行键 schema 判定，不依赖结果集样本
//——空结果 + 合法键正常返回空 rows（xref 引用面同口径）。
TEST(McpToolFeedbackTest, FieldsProjectionAllowsLegalKeysOnEmptyResults)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //search members 空结果 + 合法键（owner）：空 rows，不报「no items」。
    const auto emptySearch = call(server,
                                  "search",
                                  {{"project_id", projectId},
                                   {"kind", "members"},
                                   {"query", "zzz_absent_member"},
                                   {"fields", {"owner"}}});
    ASSERT_FALSE(emptySearch.at("result").at("isError")) << payload(emptySearch).dump();
    EXPECT_EQ(0U, payload(emptySearch).at("items").size());
    EXPECT_EQ(0U, payload(emptySearch).at("total"));

    //xref 空引用集 + 合法键（target）：同样放行；行只保留投影键。
    const auto emptyRefs = call(server,
                                "xref_query",
                                {{"project_id", projectId},
                                 {"kind", "references"},
                                 {"target", "method"},
                                 {"owner", "mcp/feedback/Ops"},
                                 {"name", "mention"},
                                 {"descriptor", "()V"},
                                 {"fields", {"target"}}});
    ASSERT_FALSE(emptyRefs.at("result").at("isError")) << payload(emptyRefs).dump();
    for (const auto& item : payload(emptyRefs).at("items")) {
        ASSERT_EQ(1U, item.size());
        EXPECT_TRUE(item.contains("target"));
    }
}

//非法 fields 键报 unknown key 并列出该 kind 的静态合法键全集——空结果
//集照常列出全集，不再报「no items in this result set」；类分面与成员分面的
//键集按行形状区分（class 键 vs owner 键）。
TEST(McpToolFeedbackTest, FieldsProjectionRejectsUnknownKeysWithStaticSchema)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //空结果 + 非法键：报错列出 members 行静态键全集（排序展示）。
    const auto unknown = call(server,
                              "search",
                              {{"project_id", projectId},
                               {"kind", "members"},
                               {"query", "zzz_absent_member"},
                               {"fields", {"bogus"}}});
    ASSERT_TRUE(unknown.at("result").at("isError"));
    const auto& message = payload(unknown).at("error").get_ref<const std::string&>();
    EXPECT_NE(std::string::npos, message.find("fields: unknown key \"bogus\"")) << message;
    EXPECT_NE(std::string::npos,
              message.find("available: descriptor, instruction_index, kind, name, owner, "
                           "resource_uri, value"));

    //类分面的合法键集是 class 键（非 owner）：空结果下传 owner 同样拒绝并列出
    //类行全集。
    const auto unknownClass = call(server,
                                   "search",
                                   {{"project_id", projectId},
                                    {"kind", "classes"},
                                    {"query", "zzz_absent_class"},
                                    {"fields", {"owner"}}});
    ASSERT_TRUE(unknownClass.at("result").at("isError"));
    const auto& classMessage = payload(unknownClass).at("error").get_ref<const std::string&>();
    EXPECT_NE(std::string::npos, classMessage.find("fields: unknown key \"owner\""));
    EXPECT_NE(std::string::npos,
              classMessage.find("available: class, descriptor, instruction_index, kind, "
                                "name, resource_uri, value"));
}

//change_class_access 增量语义——未提及位保留（ACC_SUPER 不丢），
//deprecated 映射属性而非静默丢弃；旧词 set_class_access 严格切换报迁移指引；
//位掩码 access/access_flags 对新词显式拒绝。
TEST(McpToolFeedbackTest, ChangeClassAccessKeepsUnmentionedBits)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();
    const auto classArgs = [projectId] {
        return Json{{"project_id", projectId}, {"kind", "class"}, {"class", "mcp/feedback/Ops"}};
    };

    const auto before = call(server, "class_query", classArgs());
    ASSERT_FALSE(before.at("result").at("isError")) << payload(before).dump();
    const auto accessBefore = payload(before).at("access").get<ca::u32>();
    EXPECT_EQ(0x0021U, accessBefore & 0x0021U)
        << "fixture should be public super";   //public + super

    //旧词严格切换：schema enum 层即拒（mutation_kind 白名单不含旧词）。
    const auto legacy = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "structured"},
                              {"class_uri", "mcp/feedback/Ops"},
                              {"mutation_kind", "set_class_access"},
                              {"access", 0x0031}});
    ASSERT_TRUE(legacy.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(legacy).at("error").get_ref<const std::string&>().find(
                  "not one of the allowed values"));

    //位掩码通道对新词拒绝（增量语义只吃 set/clear 名单）。
    const auto bitmask = call(server,
                              "edit_apply",
                              {{"project_id", projectId},
                               {"kind", "structured"},
                               {"class_uri", "mcp/feedback/Ops"},
                               {"mutation_kind", "change_class_access"},
                               {"access", 0x0031}});
    ASSERT_TRUE(bitmask.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(bitmask).at("error").get_ref<const std::string&>().find(
                  "takes set/clear flag lists"));

    //增量置位：只提 final，public/super 保留（0x0021 → 0x0031）；deprecated
    //随名单接受（引擎映射为 Deprecated 属性而非访问位，不再静默丢弃）。
    const auto edited = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "structured"},
                              {"class_uri", "mcp/feedback/Ops"},
                              {"mutation_kind", "change_class_access"},
                              {"set", Json::array({"final", "deprecated"})}});
    ASSERT_FALSE(edited.at("result").at("isError")) << payload(edited).dump();
    const auto after = call(server, "class_query", classArgs());
    ASSERT_FALSE(after.at("result").at("isError"));
    const auto accessAfter = payload(after).at("access").get<ca::u32>();
    EXPECT_EQ(0x0031U, accessAfter) << "final set; public and ACC_SUPER preserved";

    //clear 回清 final，回到 0x0021。
    const auto cleared = call(server,
                              "edit_apply",
                              {{"project_id", projectId},
                               {"kind", "structured"},
                               {"class_uri", "mcp/feedback/Ops"},
                               {"mutation_kind", "change_class_access"},
                               {"clear", Json::array({"final"})}});
    ASSERT_FALSE(cleared.at("result").at("isError")) << payload(cleared).dump();
    const auto reverted = call(server, "class_query", classArgs());
    ASSERT_FALSE(reverted.at("result").at("isError"));
    EXPECT_EQ(0x0021U, payload(reverted).at("access").get<ca::u32>());
}


//validation 三档走 MutationRequest 单一收口——非法值报合法枚举；
//off 跳过门禁照常提交；非账本动作（savepoint）显式拒绝该键。
TEST(McpToolFeedbackTest, ValidationGateParameterIsRouted)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //非法值在 schema enum 层先拦（枚举文案），分发层文案是内部兜底。
    const auto badValue = call(server,
                               "edit_apply",
                               {{"project_id", projectId},
                                {"kind", "structured"},
                                {"class_uri", "mcp/feedback/Ops"},
                                {"mutation_kind", "change_class_access"},
                                {"set", Json::array({"final"})},
                                {"validation", "lenient"}});
    ASSERT_TRUE(badValue.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(badValue).at("error").get_ref<const std::string&>().find(
                  "not one of the allowed values"));

    const auto edited = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "structured"},
                              {"class_uri", "mcp/feedback/Ops"},
                              {"mutation_kind", "change_class_access"},
                              {"set", Json::array({"final"})},
                              {"validation", "off"}});
    ASSERT_FALSE(edited.at("result").at("isError")) << payload(edited).dump();

    const auto savepoint = call(server,
                                "edit_history",
                                {{"project_id", projectId},
                                 {"action", "savepoint"},
                                 {"name", "gate"},
                                 {"validation", "off"}});
    ASSERT_TRUE(savepoint.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              payload(savepoint).at("error").get_ref<const std::string&>().find(
                  "only valid for undo/redo/restore/revert_class"));

    //undo 吃 validation（回滚上面的 final 置位），动作本身成功。
    const auto undone = call(server,
                             "edit_history",
                             {{"project_id", projectId},
                              {"action", "undo"},
                              {"validation", "relaxed"}});
    ASSERT_FALSE(undone.at("result").at("isError")) << payload(undone).dump();
}


//诊断归因：每条 diagnostics[] 携带 stage_name + gate——编辑门（workspace
//候选聚合校验）与重建管线（jasm 汇编/validate）分属不同 gate，客户端可分源。
TEST(McpToolFeedbackTest, DiagnosticsCarryGateAttribution)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //重建门：非法 Jasm 文本在汇编阶段报错，gate=rebuild / stage_name=assemble。
    const auto broken = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "jasm"},
                              {"dry_run", true},
                              {"uri", "mcp/feedback/Ops"},
                              {"source", "this is not jasm"}});
    ASSERT_TRUE(broken.at("result").at("isError"));
    const auto diagnostics = payload(broken).at("diagnostics");
    ASSERT_FALSE(diagnostics.empty());
    bool sawAssemble = false;
    for (const auto& item : diagnostics) {
        if (item.at("stage_name").get<std::string>() == "assemble") sawAssemble = true;
        EXPECT_FALSE(item.at("gate").get<std::string>().empty());
    }
    EXPECT_TRUE(sawAssemble);
    EXPECT_EQ(0, diagnostics.at(0).at("stage").get<int>());
}


//library 挂载只在 workspace 在册表产生一条记录（role 是标签），
//project_info 不再出现同容器双行；detach 单句柄一次清干净。
TEST(McpToolFeedbackTest, LibraryMountRegistersSingleRecord)
{
    const auto source =
        ".super java/lang/Object\n"
        ".class public super mcp/feedback/LibPoint {\n"
        "  .method public static point ()I {\n"
        "    code: {\n"
        "      bipush 1\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
    const auto assembled = assembleFixtureClass("mcp/feedback/LibPoint", source);
    mjbase::model::ResourceContainer container;
    container.addResource("mcp/feedback/LibPoint.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "mcp/feedback/LibPoint.class", assembled));
    const auto library = test::getTempOutPath("mjt_mcp_p20_library.jar");
    mjbase::model::ResourceHelper::exportToJar(container, library.string());

    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFeedbackFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto mounted = call(server,
                              "container_mount",
                              {{"project_id", projectId},
                               {"action", "mount"},
                               {"role", "library"},
                               {"path", library.string()}});
    ASSERT_FALSE(mounted.at("result").at("isError")) << payload(mounted).dump();
    //detach 句柄 = 容器登记 id（响应 container.id）；support_id 字段是
    //workspace 支撑编号，仅回显用途。
    const auto containerId = payload(mounted).at("container").at("id").get<std::string>();

    const auto countRecords = [&](const std::string& role) {
        const auto info = call(server, "project_info", {{"project_id", projectId}});
        EXPECT_FALSE(info.at("result").at("isError"));
        ca::usize hits = 0;
        for (const auto& item : payload(info).at("containers"))
            if (item.at("role") == role &&
                item.at("path") == library.string())
                ++hits;
        return hits;
    };
    EXPECT_EQ(1U, countRecords("library")) << "exactly one record per mounted library";

    const auto detached = call(server,
                               "container_mount",
                               {{"project_id", projectId},
                                {"action", "detach"},
                                {"role", "library"},
                                {"support_id", containerId}});
    ASSERT_FALSE(detached.at("result").at("isError")) << payload(detached).dump();
    EXPECT_EQ(0U, countRecords("library")) << "detach clears the whole registration";
}

}   //namespace
}   //namespace mjt::mcp
