#include <algorithm>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

//行为钉住：合法超长类名（JVM 常量池 UTF8 上限 65535 字节内）在
//mjt-mcp 读路径正常工作；超过 classfile 规格的选择器在工具边界快速失败，
//返回含实际长度与规格上限的稳定结构化错误文案，杜绝静默挂起/静默截断。
//常量池 UTF8 语义键（descriptor/signature/source_name 等）与
//exceptions[]、try_catch_blocks[].type、local_variables[].name/descriptor/
//signature 数组白名单一并纳入守卫。

namespace mjt::mcp {
namespace {

using namespace test_support;

TEST(McpLongIdentifierTest, LegalLongNamesWorkEndToEnd)
{
    //819 是 报告的问题档位；65000 贴近 65535 规格上限但仍然合法，
    //两者都必须走正常路径而不是守卫拒绝。
    for (const std::size_t length : {std::size_t{819}, std::size_t{65000}}) {
        const auto jar      = writeLongNameFixture(length);
        const auto longName = longInternalName(length);

        McpSessionHarness server;
        const auto opened = call(server, "project_open", {{"path", jar.string()}});
        ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
        const auto projectId = payload(opened).at("project_id").get<std::string>();

        const auto detail = call(server,
                                 "class_query",
                                 {{"project_id", projectId},
                                  {"kind", "class"},
                                  {"class", longName}});
        ASSERT_FALSE(detail.at("result").at("isError")) << detail.dump();
        EXPECT_EQ(longName, payload(detail).at("class"));

        const auto jasm = call(server,
                               "class_query",
                               {{"project_id", projectId},
                                {"kind", "jasm"},
                                {"class", longName}});
        ASSERT_FALSE(jasm.at("result").at("isError")) << jasm.dump();
        const auto& source = payload(jasm).at("jasm").get_ref<const std::string&>();
        EXPECT_NE(std::string::npos, source.find(".class public super"));
        EXPECT_NE(std::string::npos, source.find(longName));

        const auto search = call(server,
                                 "search",
                                 {{"project_id", projectId},
                                  {"kind", "classes"},
                                  {"query", "_" + std::to_string(length)}});
        ASSERT_FALSE(search.at("result").at("isError")) << search.dump();
        ASSERT_EQ(1U, payload(search).at("items").size());
        EXPECT_EQ(longName, payload(search).at("items").at(0).at("class"));

        const auto bytes = call(server,
                                "class_query",
                                {{"project_id", projectId},
                                 {"kind", "bytes"},
                                 {"class", longName}});
        ASSERT_FALSE(bytes.at("result").at("isError")) << bytes.dump();
        EXPECT_EQ(longName + ".class", payload(bytes).at("resource_uri"));
        EXPECT_EQ(longName, payload(bytes).at("class"));
    }
}

TEST(McpLongIdentifierTest, OverlongSelectorFailsFastWithStructuredError)
{
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //65536 字节 = classfile 常量池 UTF8 u2 上限 + 1，必然非法。
    const std::string oversized(65536, 'a');
    const auto classGet =
        call(server, "class_query", {{"project_id", projectId}, {"kind", "class"}, {"class", oversized}});
    expectDirectToolError(classGet.at("result"),
                          "class_query",
                          ca::StatusCode::INVALID_ARGUMENT,
                          "$.class identifier length 65536 exceeds classfile limit 65535");

    const auto ownerGet = call(server,
                               "class_query",
                               {{"project_id", projectId},
                                {"kind", "method"},
                                {"owner", oversized},
                                {"name", "value"},
                                {"descriptor", "()I"}});
    expectDirectToolError(ownerGet.at("result"),
                          "class_query",
                          ca::StatusCode::INVALID_ARGUMENT,
                          "$.owner identifier length 65536 exceeds classfile limit 65535");

    const auto resourceRead = call(server,
                                   "class_query",
                                   {{"project_id", projectId},
                                    {"kind", "resource_bytes"},
                                    {"uri", oversized}});
    expectDirectToolError(resourceRead.at("result"),
                          "class_query",
                          ca::StatusCode::INVALID_ARGUMENT,
                          "$.uri identifier length 65536 exceeds classfile limit 65535");

    const auto revert = call(server,
                             "edit_history",
                             {{"project_id", projectId},
                              {"action", "revert_class"},
                              {"class_uri", oversized}});
    expectDirectToolError(revert.at("result"),
                          "edit_history",
                          ca::StatusCode::INVALID_ARGUMENT,
                          "$.class_uri identifier length 65536 exceeds classfile limit 65535");
}

TEST(McpLongIdentifierTest, OverlongConstantPoolUtf8KeysFailFast)
{
    //常量池 UTF8 语义但非「选择器」的键超限时同样在工具边界
    //快速失败，文案含参数路径（$.<key>[i].<member>）与实际长度，不再穿透
    //到下游报不指因的 NOT_FOUND/装配错误。
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened  = call(server, "project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    //65536 字节 = classfile 常量池 UTF8 u2 上限 + 1，必然非法。
    const std::string oversized(65536, 'a');

    //顶层 descriptor：方法选择器三件套的第三键。
    const auto overrides = call(server,
                                "xref_query",
                                {{"project_id", projectId},
                                 {"kind", "overrides"},
                                 {"owner", "mcp/Subject"},
                                 {"name", "value"},
                                 {"descriptor", oversized}});
    expectDirectToolError(overrides.at("result"),
                          "xref_query",
                          ca::StatusCode::INVALID_ARGUMENT,
                          "$.descriptor identifier length 65536 exceeds classfile limit 65535");

    //source_name：Jasm patch 的 SourceFile 名。
    const auto jasmPreview =
        call(server,
             "edit_apply",
             {{"project_id", projectId},
              {"kind", "jasm"},
              {"dry_run", true},
              {"uri", "mcp/Subject.class"},
              {"source", ".class public mcp/Subject {\n}\n"},
              {"source_name", oversized}});
    expectDirectToolError(jasmPreview.at("result"),
                          "edit_apply",
                          ca::StatusCode::INVALID_ARGUMENT,
                          "$.source_name identifier length 65536 exceeds classfile limit 65535");

    //edit_apply(kind=structured) 顶层键逐一覆盖：member_descriptor /
    //annotation_descriptor / signature / exceptions[] / try_catch_blocks[].type /
    //local_variables[].{name,descriptor,signature}。
    const auto patch = [&](Json extra) {
        //调用方经 "kind" 携带 mutation kind 时搬到 mutation_kind 槽；工具级
        //kind 固定为 structured。调用方已用 mutation_kind 时原样保留。
        if (extra.contains("kind")) {
            extra["mutation_kind"] = extra["kind"];
            extra.erase("kind");
        }
        extra["project_id"] = projectId;
        extra["kind"]       = "structured";
        extra["class_uri"]  = "mcp/Subject.class";
        return call(server, "edit_apply", std::move(extra));
    };

    expectDirectToolError(patch({{"mutation_kind", "set_method_access"},
                                 {"member_name", "value"},
                                 {"member_descriptor", oversized}})
                              .at("result"),
                          "edit_apply",
                          ca::StatusCode::INVALID_ARGUMENT,
                          "$.member_descriptor identifier length 65536 exceeds classfile "
                          "limit 65535");

    expectDirectToolError(patch({{"mutation_kind", "add_marker_annotation"},
                                 {"annotation_target", "class"},
                                 {"annotation_descriptor", oversized}})
                              .at("result"),
                          "edit_apply",
                          ca::StatusCode::INVALID_ARGUMENT,
                          "$.annotation_descriptor identifier length 65536 exceeds classfile "
                          "limit 65535");

    expectDirectToolError(patch({{"mutation_kind", "set_field_access"},
                                 {"member_name", "counter"},
                                 {"signature", oversized}})
                              .at("result"),
                          "edit_apply",
                          ca::StatusCode::INVALID_ARGUMENT,
                          "$.signature identifier length 65536 exceeds classfile limit 65535");

    expectDirectToolError(
        patch({{"mutation_kind", "add_method"},
               {"member_name", "extra"},
               {"exceptions", Json::array({"java/io/IOException", oversized})}})
            .at("result"),
        "edit_apply",
        ca::StatusCode::INVALID_ARGUMENT,
        "$.exceptions[1] identifier length 65536 exceeds classfile limit 65535");

    expectDirectToolError(
        patch({{"mutation_kind", "replace_try_catch_blocks"},
               {"try_catch_blocks",
                Json::array({Json{{"start_instruction", 0},
                                  {"end_instruction", 1},
                                  {"handler_instruction", 1},
                                  {"type", oversized}}})}})
            .at("result"),
        "edit_apply",
        ca::StatusCode::INVALID_ARGUMENT,
        "$.try_catch_blocks[0].type identifier length 65536 exceeds classfile limit 65535");

    //同一调用三个元素分别超长 name/descriptor/signature：按元素序报首个，
    //错误路径钉死到具体成员。
    const Json localVariables = Json::array(
        {Json{{"name", "v"},
               {"descriptor", oversized},
               {"start_label", "a"},
               {"end_label", "b"},
               {"index", 0}},
         Json{{"name", oversized},
               {"descriptor", "I"},
               {"start_label", "a"},
               {"end_label", "b"},
               {"index", 1}},
         Json{{"name", "w"},
               {"descriptor", "I"},
               {"signature", oversized},
               {"start_label", "a"},
               {"end_label", "b"},
               {"index", 2}}});
    expectDirectToolError(patch({{"mutation_kind", "replace_method_body"}, {"local_variables", localVariables}})
                              .at("result"),
                          "edit_apply",
                          ca::StatusCode::INVALID_ARGUMENT,
                          "$.local_variables[0].descriptor identifier length 65536 exceeds "
                          "classfile limit 65535");
}

TEST(McpLongIdentifierTest, LegalConstantPoolUtf8KeysPassGuard)
{
    //各新键一例 65000 字节合法值：守卫放行，工具走正常下游路径
    //（未知类/成员的常规错误），而非 INVALID_ARGUMENT 长度拒绝。
    std::string legalName = "java/lang/L";
    legalName.append(65000U - legalName.size(), 'x');
    ASSERT_EQ(65000U, legalName.size());
    //合法方法描述符：(III…I)V，总长恰 65000 字节。
    const std::string legalDescriptor = "(" + std::string(64997, 'I') + ")V";
    ASSERT_EQ(65000U, legalDescriptor.size());
    //合法字段/类型签名式样：L…; 总长恰 65000 字节。
    const std::string legalSignature = "L" + std::string(64998, 'x') + ";";
    ASSERT_EQ(65000U, legalSignature.size());

    //守卫拒绝必为 INVALID_ARGUMENT + 「exceeds classfile limit」文案；两处
    //信号均不出现即证明请求穿过守卫走到了正常工具路径（成功或常规失败）。
    const auto expectGuardPassed = [](const Json& response, const std::string& context) {
        ASSERT_TRUE(response.at("result").is_object()) << context << ": " << response.dump();
        const auto& body    = payload(response);
        const auto  codeOf  = [](const Json& object) {
            return object.contains("status_code") ? object.at("status_code").get<int>() : -1;
        };
        const int topCode       = codeOf(body);
        const int statusCode    = body.contains("status") && body.at("status").is_object()
                                      ? codeOf(body.at("status"))
                                      : -1;
        EXPECT_NE(static_cast<int>(ca::StatusCode::INVALID_ARGUMENT), topCode)
            << context << ": " << response.dump();
        EXPECT_NE(static_cast<int>(ca::StatusCode::INVALID_ARGUMENT), statusCode)
            << context << ": " << response.dump();
        EXPECT_EQ(std::string::npos, response.dump().find("exceeds classfile limit"))
            << context << ": " << response.dump();
    };

    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto overrides = call(server,
                                "xref_query",
                                {{"project_id", projectId},
                                 {"kind", "overrides"},
                                 {"owner", "mcp/Subject"},
                                 {"name", "missing"},
                                 {"descriptor", legalDescriptor}});
    expectGuardPassed(overrides, "$.descriptor");

    const auto patch = [&](Json extra) {
        //调用方经 "kind" 携带 mutation kind 时搬到 mutation_kind 槽；工具级
        //kind 固定为 structured。调用方已用 mutation_kind 时原样保留。
        if (extra.contains("kind")) {
            extra["mutation_kind"] = extra["kind"];
            extra.erase("kind");
        }
        extra["project_id"] = projectId;
        extra["kind"]       = "structured";
        extra["class_uri"]  = "mcp/No.class";
        extra["dry_run"]    = true;
        return call(server, "edit_apply", std::move(extra));
    };

    expectGuardPassed(patch({{"mutation_kind", "set_method_access"},
                             {"member_name", "value"},
                             {"member_descriptor", legalDescriptor}}),
                      "$.member_descriptor");

    expectGuardPassed(patch({{"mutation_kind", "add_marker_annotation"},
                             {"annotation_target", "class"},
                             {"annotation_descriptor", legalSignature}}),
                      "$.annotation_descriptor");

    expectGuardPassed(patch({{"mutation_kind", "set_field_access"},
                             {"member_name", "counter"},
                             {"signature", legalSignature}}),
                      "$.signature");

    expectGuardPassed(patch({{"mutation_kind", "add_method"},
                             {"member_name", "extra"},
                             {"exceptions", Json::array({legalName})}}),
                      "$.exceptions[0]");

    expectGuardPassed(patch({{"mutation_kind", "replace_try_catch_blocks"},
                             {"try_catch_blocks",
                              Json::array({Json{{"start_instruction", 0},
                                                {"end_instruction", 1},
                                                {"handler_instruction", 1},
                                                {"type", legalName}}})}}),
                      "$.try_catch_blocks[0].type");

    expectGuardPassed(patch(
                          {{"mutation_kind", "replace_method_body"},
                           {"local_variables",
                            Json::array({Json{{"name", legalName},
                                              {"descriptor", legalDescriptor},
                                              {"signature", legalSignature},
                                              {"start_label", "a"},
                                              {"end_label", "b"},
                                              {"index", 0}}})}}),
                      "$.local_variables[0]");

    //source_name：对既有类做完整 Jasm 校验，合法长 SourceFile 名应整链成功。
    const auto jasmPreview = call(server,
                                  "edit_apply",
                                  {{"project_id", projectId},
                                   {"kind", "jasm"},
                                   {"dry_run", true},
                                   {"uri", "mcp/Subject.class"},
                                   {"source", classSource("1")},
                                   {"source_name", legalName}});
    ASSERT_FALSE(jasmPreview.at("result").at("isError"))
        << "$.source_name: " << jasmPreview.dump();
}

TEST(McpLongIdentifierTest, SelectorAtClassfileLimitPassesGuard)
{
    //恰好 65535 字节的选择器合法，守卫必须放行；无对应类时工具继续走
    //正常 NOT_FOUND 路径，而不是长度守卫拒绝。
    std::string atLimit = "mcp/";
    atLimit.append(65535 - atLimit.size(), 'a');
    ASSERT_EQ(65535U, atLimit.size());

    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto missing = call(server,
                              "class_query",
                              {{"project_id", projectId}, {"kind", "class"}, {"class", atLimit}});
    ASSERT_TRUE(missing.at("result").at("isError")) << missing.dump();
    const auto& error = payload(missing).at("error").get_ref<const std::string&>();
    EXPECT_EQ(std::string::npos, error.find("exceeds classfile limit")) << error;
    //走到正常类不存在路径（NOT_FOUND）而非长度守卫（INVALID_ARGUMENT）。
    EXPECT_EQ(static_cast<int>(ca::StatusCode::NOT_FOUND),
              payload(missing).at("status_code").get<int>());
}

TEST(McpLongIdentifierTest, FreeTextArgumentsAreNotIdentifierGuarded)
{
    //守卫只针对 class/owner/URI 类选择器；书签名这类自由文本的 "name" 键有
    //工具级豁免（edit_history），不受 65535 限制。
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", writeFixture().string()}});
    ASSERT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto savepoint = call(server,
                                "edit_history",
                                {{"project_id", projectId},
                                 {"action", "savepoint"},
                                 {"name", std::string(70000, 'z')}});
    ASSERT_FALSE(savepoint.at("result").at("isError")) << savepoint.dump();
    //未提交变更时 savepoint 不推进 revision，但必须正常受理而非守卫拒绝。
    EXPECT_TRUE(payload(savepoint).at("revision").is_number_unsigned());

    //对照：同名键在 class/owner 语义工具上仍受守卫约束（见
    //OverlongSelectorFailsFastWithStructuredError）。
}

}   //namespace
}   //namespace mjt::mcp
