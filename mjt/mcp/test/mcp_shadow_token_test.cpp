//shadow 引用 token T2：MCP 工具面 name 槽接线 E2E。
//
//覆盖（任务书测试矩阵）：三分表 name 槽全分支 + token 不匹配 + 缺 sidecar +
//$${ 转义 + 常量恒等 + MemberRef 全分支 + ambiguous-member + raw-or-ref 不
//fall-through 专测（同一输入两错误码互斥）+ 评审 N1 处方（daoback）：patch_apply
//全部「寻址既有成员」kind 的 MemberRef 解析（成功向 + unknown-handle 响亮向）。
//法务红线：全部自造字节（writeFixture 的 jasm 装配产物 + 手编 v2 sidecar
//JSON），零反编译样本入仓。
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

constexpr const char* kTokenShape = "${Class_";

///writeFixture 落盘固定路径（test/tmp/mjt_mcp_fixture.jar），本组测试会
///在 jar 旁写 sidecar（含手编歧义表/损坏文件），必须逐用例隔离：拷贝到
///唯一路径后打开副本，杜绝用例间 sidecar 串台。
std::filesystem::path uniqueFixtureJar()
{
    static int counter = 0;
    const auto source  = writeFixture();
    auto       target  = source;
    target.replace_filename("mjt_shadow_t2_" + std::to_string(++counter) + ".jar");
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::overwrite_existing);
    return target;
}

///打开 fixture 工作区并生成 v2 sidecar，返回 workspace_id。
std::string openWithSidecar(McpSessionHarness& server, const std::filesystem::path& jar)
{
    const auto opened = call(server, "workspace_open", {{"path", jar.string()}});
    EXPECT_FALSE(opened.at("result").at("isError")) << opened.dump();
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto generated =
        call(server, "shadow_generate", {{"workspace_id", workspaceId}});
    EXPECT_FALSE(generated.at("result").at("isError")) << generated.dump();
    return workspaceId;
}

///raw_to_shadow 取回首个 match（可带 owner 过滤）。
Json firstMatch(McpSessionHarness& server,
                const std::string& workspaceId,
                const std::string& name,
                const std::string& owner = {},
                const std::string& kind  = {})
{
    Json arguments = {{"workspace_id", workspaceId}, {"name", name}};
    if (!owner.empty()) arguments["owner"] = owner;
    if (!kind.empty()) arguments["kind"] = kind;
    const auto response = call(server, "raw_to_shadow", arguments);
    EXPECT_FALSE(response.at("result").at("isError")) << response.dump();
    const auto& matches = payload(response).at("matches");
    EXPECT_GE(matches.size(), 1U) << response.dump();
    return matches.at(0);
}

///断言 isError 工具错误且消息含 expect（并可选地不含 exclude）。
void expectSlotError(const Json& response,
                     const std::string& expect,
                     const std::string& exclude = {},
                     const std::string& hint    = {})
{
    ASSERT_TRUE(response.at("result").at("isError").get<bool>()) << response.dump();
    const auto message = payload(response).at("error").get<std::string>();
    EXPECT_NE(std::string::npos, message.find(expect)) << hint << " | " << message;
    if (!exclude.empty()) {
        EXPECT_EQ(std::string::npos, message.find(exclude)) << hint << " | " << message;
    }
}

}   //namespace

//───────────────────── handle 发放与 ClassRef 三面往返 ─────────────────────

TEST(McpShadowTokenTest, EchoToolIssuesHandlesAndClassRefResolvesAcrossQueryFace)
{
    const auto jar = uniqueFixtureJar();
    McpSessionHarness server;
    const auto workspaceId = openWithSidecar(server, jar);

    //raw_to_shadow 发放 ref（§2 形态回执）。
    const auto classMatch = firstMatch(server, workspaceId, "mcp/Subject");
    ASSERT_TRUE(classMatch.contains("ref")) << classMatch.dump();
    const auto classRef = classMatch.at("ref").get<std::string>();
    EXPECT_EQ(0, classRef.find(kTokenShape)) << classRef;

    //查询面三工具：ClassRef → 真名。
    const auto detail = call(server, "class_get", {{"workspace_id", workspaceId}, {"class", classRef}});
    ASSERT_FALSE(detail.at("result").at("isError")) << detail.dump();
    EXPECT_EQ("mcp/Subject", payload(detail).at("internal_name"));

    const auto bytes = call(server, "class_bytes", {{"workspace_id", workspaceId}, {"class", classRef}});
    ASSERT_FALSE(bytes.at("result").at("isError")) << bytes.dump();
    EXPECT_EQ("mcp/Subject", payload(bytes).at("internal_name"));

    const auto jasm = call(server, "class_jasm", {{"workspace_id", workspaceId}, {"class", classRef}});
    ASSERT_FALSE(jasm.at("result").at("isError")) << jasm.dump();
    EXPECT_NE(std::string::npos,
              payload(jasm).at("source").get<std::string>().find(".class public super"));

    //relations 选择槽同口径（class_source 走真实反编译后端、依赖环境内
    //CFR jar，不在此钉；其槽解析与 class_get 同一 resolveClassArgument 面）。
    const auto relations =
        call(server, "class_relations", {{"workspace_id", workspaceId}, {"class", classRef}});
    ASSERT_FALSE(relations.at("result").at("isError")) << relations.dump();
}

TEST(McpShadowTokenTest, MemberRefResolvesWithDescriptorBackfill)
{
    const auto jar = uniqueFixtureJar();
    McpSessionHarness server;
    const auto workspaceId = openWithSidecar(server, jar);

    const auto classRef = firstMatch(server, workspaceId, "mcp/Subject").at("ref").get<std::string>();
    //owner 过滤槽自身接受 ClassRef；kind=method 消歧（两 fixture 类同有 value）。
    const auto methodMatch =
        firstMatch(server, workspaceId, "value", classRef, "method");
    ASSERT_TRUE(methodMatch.contains("ref")) << methodMatch.dump();
    const auto memberRef = methodMatch.at("ref").get<std::string>();
    EXPECT_NE(std::string::npos, memberRef.find(".Method_")) << memberRef;

    //desc 不进 handle：descriptor 省略 → 自表回填。
    const auto detail = call(server,
                             "method_get",
                             {{"workspace_id", workspaceId},
                              {"owner", classRef},
                              {"name", memberRef},
                              {"descriptor", ""}});
    ASSERT_FALSE(detail.at("result").at("isError")) << detail.dump();
    EXPECT_EQ("value", payload(detail).at("name"));
    EXPECT_EQ("()I", payload(detail).at("descriptor"));

    //显式一致 desc 照常；显式矛盾 desc → 参数级错误（非四分类）。
    const auto consistent = call(server,
                                 "method_get",
                                 {{"workspace_id", workspaceId},
                                  {"owner", classRef},
                                  {"name", memberRef},
                                  {"descriptor", "()I"}});
    EXPECT_FALSE(consistent.at("result").at("isError")) << consistent.dump();
    const auto contradictory = call(server,
                                    "method_get",
                                    {{"workspace_id", workspaceId},
                                     {"owner", classRef},
                                     {"name", memberRef},
                                     {"descriptor", "(J)V"}});
    expectSlotError(contradictory, "does not match the descriptor");

    //bytecode / jasm / overrides / 反编译槽同口径。
    const auto bytecode = call(server,
                               "method_bytecode",
                               {{"workspace_id", workspaceId},
                                {"owner", classRef},
                                {"name", memberRef},
                                {"descriptor", ""}});
    EXPECT_FALSE(bytecode.at("result").at("isError")) << bytecode.dump();
    const auto methodJasm = call(server,
                                 "method_jasm",
                                 {{"workspace_id", workspaceId},
                                  {"owner", classRef},
                                  {"name", memberRef},
                                  {"descriptor", ""}});
    EXPECT_FALSE(methodJasm.at("result").at("isError")) << methodJasm.dump();
    const auto overrides = call(server,
                                "method_overrides",
                                {{"workspace_id", workspaceId},
                                 {"owner", classRef},
                                 {"name", memberRef},
                                 {"descriptor", ""}});
    EXPECT_FALSE(overrides.at("result").at("isError")) << overrides.dump();
}

//───────────────────── 四分类错误面（工具边界） ─────────────────────

TEST(McpShadowTokenTest, FourErrorClassesSurfaceAtToolBoundary)
{
    const auto jar = uniqueFixtureJar();
    McpSessionHarness server;
    const auto workspaceId = openWithSidecar(server, jar);
    const auto classRef =
        firstMatch(server, workspaceId, "mcp/Subject").at("ref").get<std::string>();

    //unknown-handle：形状对、表里没有（错序号）。
    const auto unknown =
        call(server, "class_get", {{"workspace_id", workspaceId}, {"class", "${Class_99.00000000}"}});
    expectSlotError(unknown, "unknown-handle", "raw-not-found", "wrong index");

    //unknown-handle：token 手滑（末位翻转）。
    auto mutated = std::string(classRef);
    mutated[mutated.size() - 2] = mutated[mutated.size() - 2] == '0' ? '1' : '0';
    const auto stale = call(server, "class_get", {{"workspace_id", workspaceId}, {"class", mutated}});
    expectSlotError(stale, "unknown-handle", "raw-not-found", "flipped token");

    //malformed-handle：缺后缀 / 坏 hex / 裸 ${。
    for (const auto* broken : {"${Class_1}", "${Class_1.zzzzzzzz}", "${", "${CLASS_1.11111111}"}) {
        const auto response =
            call(server, "class_get", {{"workspace_id", workspaceId}, {"class", broken}});
        expectSlotError(response, "malformed-handle", "raw-not-found", broken);
    }

    //raw-not-found：sidecar 活跃后裸名缺表（不回落、不串码）。
    const auto rawMiss =
        call(server, "class_get", {{"workspace_id", workspaceId}, {"class", "no/such/Thing"}});
    expectSlotError(rawMiss, "raw-not-found", "unknown-handle", "raw missing");

    //MemberRef 成员 token 不匹配 → unknown-handle。
    const auto memberRef =
        firstMatch(server, workspaceId, "value", classRef, "method").at("ref").get<std::string>();
    auto memberMutated = std::string(memberRef);
    memberMutated[memberMutated.size() - 2] =
        memberMutated[memberMutated.size() - 2] == '0' ? '1' : '0';
    const auto memberStale = call(server,
                                  "method_get",
                                  {{"workspace_id", workspaceId},
                                   {"owner", classRef},
                                   {"name", memberMutated},
                                   {"descriptor", ""}});
    expectSlotError(memberStale, "unknown-handle", "raw-not-found", "member token");
}

TEST(McpShadowTokenTest, RawOrRefNeverFallThroughAtToolBoundary)
{
    //同一输入两码互斥（E2E 面）：ref 形状缺失绝不报 raw-not-found；
    //raw 形状缺失绝不报 unknown-handle/malformed-handle。
    const auto jar = uniqueFixtureJar();
    McpSessionHarness server;
    const auto workspaceId = openWithSidecar(server, jar);

    const auto refMiss =
        call(server, "class_get", {{"workspace_id", workspaceId}, {"class", "${Class_42.c0ffee00}"}});
    expectSlotError(refMiss, "unknown-handle", "raw-not-found", "ref miss");

    const auto rawMiss =
        call(server, "class_get", {{"workspace_id", workspaceId}, {"class", "ghost/Gone"}});
    expectSlotError(rawMiss, "raw-not-found", "unknown-handle", "raw miss");
    expectSlotError(rawMiss, "raw-not-found", "malformed-handle", "raw miss");
}

TEST(McpShadowTokenTest, MissingSidecarHardErrorsReferencesButRawStaysZeroDrift)
{
    //全新 fixture、无 sidecar：引用形状 → missing-sidecar 硬报错；raw → 原样。
    const auto jar = uniqueFixtureJar();
    //复制路径可能残留上一轮运行的 sidecar，先清场保住"缺 sidecar"前提。
    std::error_code cleanupError;
    std::filesystem::remove(jar.string() + ".mjt-shadow", cleanupError);
    McpSessionHarness server;
    const auto opened = call(server, "workspace_open", {{"path", jar.string()}});
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

    const auto reference =
        call(server, "class_get", {{"workspace_id", workspaceId}, {"class", "${Class_1.11111111}"}});
    expectSlotError(reference, "missing-sidecar", "raw-not-found", "no sidecar");
    expectSlotError(reference, "missing-sidecar", "unknown-handle", "no sidecar");

    const auto raw =
        call(server, "class_get", {{"workspace_id", workspaceId}, {"class", "mcp/Subject"}});
    EXPECT_FALSE(raw.at("result").at("isError")) << raw.dump();
    EXPECT_EQ("mcp/Subject", payload(raw).at("internal_name"));

    //损坏 sidecar：同缺表口径（note 带原因），raw 仍零漂移。
    std::ofstream garbage(jar.string() + ".mjt-shadow", std::ios::binary);
    garbage << "{not-json";
    garbage.close();
    const auto invalidRef =
        call(server, "class_get", {{"workspace_id", workspaceId}, {"class", "${Class_1.11111111}"}});
    expectSlotError(invalidRef, "missing-sidecar", "", "corrupt sidecar");
    const auto rawAfter = call(server,
                               "class_get",
                               {{"workspace_id", workspaceId}, {"class", "mcp/Subject"}});
    EXPECT_FALSE(rawAfter.at("result").at("isError")) << rawAfter.dump();
}

TEST(McpShadowTokenTest, EscapedLiteralTakesRawPathNotHandlePath)
{
    const auto jar = uniqueFixtureJar();
    McpSessionHarness server;
    const auto workspaceId = openWithSidecar(server, jar);

    //$${...} = 字面 ${...}：按 raw 语义核表 → raw-not-found（绝不串 unknown/malformed）。
    const auto escaped = call(server,
                              "class_get",
                              {{"workspace_id", workspaceId}, {"class", "$${Class_1.11111111}"}});
    expectSlotError(escaped, "raw-not-found", "unknown-handle", "escape");
    expectSlotError(escaped, "raw-not-found", "malformed-handle", "escape");
}

TEST(McpShadowTokenTest, AmbiguousMemberSurfacesWithDescriptorGuidance)
{
    //手编 v2 sidecar：同 owner 同影子名两条 desc（生成器不产、表 API 可达）。
    const auto jar = uniqueFixtureJar();
    McpSessionHarness server;
    const auto workspaceId = openWithSidecar(server, jar);
    const auto sidecarPath = jar.string() + ".mjt-shadow";

    //覆写为歧义表：mcp/Subject 名下 Method_1 两枚重载、token 各异。
    const char* ambiguous =
        "{\"version\":2,\"aliases\":["
        "{\"kind\":\"class\",\"name\":\"mcp/Subject\",\"shadow\":\"Class_1\",\"token\":\"11111111\"},"
        "{\"kind\":\"method\",\"owner\":\"mcp/Subject\",\"desc\":\"()I\",\"name\":\"value\","
        "\"shadow\":\"Method_1\",\"token\":\"22222222\"},"
        "{\"kind\":\"method\",\"owner\":\"mcp/Subject\",\"desc\":\"(J)V\",\"name\":\"value\","
        "\"shadow\":\"Method_1\",\"token\":\"55555555\"}]}";
    {
        std::ofstream sidecar(sidecarPath, std::ios::binary | std::ios::trunc);
        sidecar << ambiguous;
    }
    const auto response = call(server,
                               "method_get",
                               {{"workspace_id", workspaceId},
                                {"owner", "${Class_1.11111111}"},
                                {"name", "${Class_1.11111111.Method_1.22222222}"},
                                {"descriptor", ""}});
    expectSlotError(response, "ambiguous-member", "unknown-handle", "overloads");
    expectSlotError(response, "ambiguous-member", "malformed-handle", "overloads");
    const auto message = payload(response).at("error").get<std::string>();
    EXPECT_NE(std::string::npos, message.find("raw descriptor")) << message;
}

//───────────────────── 变更面接线（rename / patch / revert） ─────────────────────

TEST(McpShadowTokenTest, MutationFaceResolvesHandlesEndToEnd)
{
    const auto jar = uniqueFixtureJar();
    McpSessionHarness server;
    const auto workspaceId = openWithSidecar(server, jar);
    const auto classRef =
        firstMatch(server, workspaceId, "mcp/Subject").at("ref").get<std::string>();
    const auto memberRef =
        firstMatch(server, workspaceId, "value", classRef, "method").at("ref").get<std::string>();

    //rename_analyze：owner=ClassRef + name=MemberRef + desc 省略（自表回填）。
    const auto rename = call(server,
                             "rename_analyze",
                             {{"workspace_id", workspaceId},
                              {"kind", "method"},
                              {"owner", classRef},
                              {"name", memberRef},
                              {"descriptor", ""},
                              {"new_name", "renamed_value"}});
    ASSERT_FALSE(rename.at("result").at("isError")) << rename.dump();
    const auto& mappings = payload(rename).at("mappings");
    ASSERT_GE(mappings.size(), 1U);
    EXPECT_NE(std::string::npos,
              mappings.at(0).at("before").get<std::string>().find("mcp/Subject.value"));

    //patch_apply：class_uri=ClassRef（内部换算精确资源 URI）。
    const auto patch = call(server,
                            "patch_apply",
                            {{"workspace_id", workspaceId},
                             {"class_uri", classRef},
                             {"kind", "set_class_access"},
                             {"access_flags", Json::array({"public", "final"})}});
    EXPECT_FALSE(patch.at("result").at("isError")) << patch.dump();

    //workspace_revert_class：class_uri=ClassRef。
    const auto revert = call(server,
                             "workspace_revert_class",
                             {{"workspace_id", workspaceId}, {"class_uri", classRef}});
    EXPECT_FALSE(revert.at("result").at("isError")) << revert.dump();

    //patch_replace_class_bytes：class_uri=ClassRef（bytes 取原类，恒等替换）。
    const auto bytes = call(server, "class_bytes", {{"workspace_id", workspaceId}, {"class", "mcp/Subject"}});
    const auto content = payload(bytes).at("content").get<std::string>();
    const auto replace = call(server,
                              "patch_replace_class_bytes",
                              {{"workspace_id", workspaceId},
                               {"class_uri", classRef},
                               {"content_base64", content}});
    EXPECT_FALSE(replace.at("result").at("isError")) << replace.dump();
}

//───────────────────── xref 族 raw 直通（零漂移） ─────────────────────

TEST(McpShadowTokenTest, XrefFamilyRawPassThroughKeepsExternalTargets)
{
    const auto jar = uniqueFixtureJar();
    McpSessionHarness server;
    const auto workspaceId = openWithSidecar(server, jar);

    //表外合法目标（JDK 类）：raw 直通，绝不 raw-not-found。
    const auto xref = call(server,
                           "xref_class",
                           {{"workspace_id", workspaceId}, {"class", "java/lang/Object"}});
    ASSERT_FALSE(xref.at("result").at("isError")) << xref.dump();
    EXPECT_GE(payload(xref).at("items").size(), 1U);   //.super java/lang/Object 被引用

    //Ref 形状照常解析（xref 面 Ref → 内部名）。
    const auto classRef =
        firstMatch(server, workspaceId, "mcp/Subject").at("ref").get<std::string>();
    const auto xrefByRef =
        call(server, "xref_class", {{"workspace_id", workspaceId}, {"class", classRef}});
    EXPECT_FALSE(xrefByRef.at("result").at("isError")) << xrefByRef.dump();
}

//───────────────────── 字符串常量恒等（E2E 零毒化） ─────────────────────

TEST(McpShadowTokenTest, StringConstantsWithHandleShapedTextStayByteIdentical)
{
    const auto jar = uniqueFixtureJar();
    McpSessionHarness server;
    const auto workspaceId = openWithSidecar(server, jar);
    const auto classRef =
        firstMatch(server, workspaceId, "mcp/Subject").at("ref").get<std::string>();

    //经 handle 选择槽写一枚字符串常量，内容含 ${var} 模板与 handle 形态文本。
    const std::string literal = "${var} and ${Class_1.deadbeef} stay literal";
    const auto add = call(server,
                          "patch_apply",
                          {{"workspace_id", workspaceId},
                           {"class_uri", classRef},
                           {"kind", "add_field"},
                           {"member_name", "flagText"},
                           {"member_descriptor", "Ljava/lang/String;"},
                           {"access", 10},
                           {"field_value",
                            {{"kind", "string"}, {"string_value", literal}}}});
    ASSERT_FALSE(add.at("result").at("isError")) << add.dump();

    auto readBytes = [&]() {
        const auto bytes =
            call(server, "class_bytes", {{"workspace_id", workspaceId}, {"class", "mcp/Subject"}});
        EXPECT_FALSE(bytes.at("result").at("isError")) << bytes.dump();
        const auto& encoded = payload(bytes).at("content").get_ref<const std::string&>();
        //等价 base64 解码（工具面外自足，不依赖 detail 面）。
        static const std::string alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<ca::u8> decoded;
        int                 buffer = 0;
        int                 bits   = 0;
        for (const char c : encoded) {
            if (c == '=' || alphabet.find(c) == std::string::npos) continue;
            buffer = (buffer << 6) | static_cast<int>(alphabet.find(c));
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                decoded.push_back(static_cast<ca::u8>((buffer >> bits) & 0xFF));
            }
        }
        return decoded;
    };
    const auto afterAdd = readBytes();
    const std::string needle(literal);
    //常量以裸字节落在常量池：零转义、零解析、零改写。
    ASSERT_NE(std::string::npos,
              std::string(reinterpret_cast<const char*>(afterAdd.data()), afterAdd.size())
                  .find(needle))
        << "constant text missing after add";

    //随后再经 handle 打一发变更：常量字节恒等不动。
    const auto memberRef =
        firstMatch(server, workspaceId, "value", classRef, "method").at("ref").get<std::string>();
    const auto second = call(server,
                             "patch_apply",
                             {{"workspace_id", workspaceId},
                              {"class_uri", classRef},
                              {"kind", "set_method_access"},
                              {"member_name", memberRef},
                              {"member_descriptor", "()I"},
                              {"access_flags", Json::array({"public", "final"})}});
    EXPECT_FALSE(second.at("result").at("isError")) << second.dump();
    const auto afterSecond = readBytes();
    ASSERT_EQ(afterAdd.size(), afterSecond.size());
    //恒等断言：除访问标志外逐字节一致；常量文本必须仍在原样出现。
    EXPECT_NE(std::string::npos,
              std::string(reinterpret_cast<const char*>(afterSecond.data()), afterSecond.size())
                  .find(needle))
        << "constant text mutated by later handle mutation";
}

//───────────────── patch_apply 既有成员 kind 全量 MemberRef 解析（N1） ─────────────────

TEST(McpShadowTokenTest, PatchApplyResolvesMemberRefForAllExistingMemberKinds)
{
    //T2 评审 N1 处方（daoback）：patch_apply 12 个「寻址既有成员」kind 全量
    //接入 MemberRef 解析。每 kind 两向：
    //合法 handle → 槽层解析成功、成员寻址命中（变更落地 / 既有 canonical
    //后续错误），绝不带 handle 直落下游 NOT_FOUND；
    //成员 token 手滑 → unknown-handle 响亮（工具边界拦截，不入下游）。
    //annotation 两 kind 另钉：annotation_target=class 时 member 不寻址、
    //不解析（表外 handle 字面量零核表，防越界解析）。
    const auto jar = uniqueFixtureJar();
    McpSessionHarness server;
    const auto workspaceId = openWithSidecar(server, jar);
    const auto classRef =
        firstMatch(server, workspaceId, "mcp/Subject").at("ref").get<std::string>();
    const auto methodRef =
        firstMatch(server, workspaceId, "value", classRef, "method").at("ref").get<std::string>();
    const auto fieldRef =
        firstMatch(server, workspaceId, "counter", classRef, "field").at("ref").get<std::string>();
    ASSERT_NE(std::string::npos, methodRef.find(".Method_")) << methodRef;
    ASSERT_NE(std::string::npos, fieldRef.find(".Field_")) << fieldRef;

    auto flipToken = [](std::string ref) {
        ref[ref.size() - 2] = ref[ref.size() - 2] == '0' ? '1' : '0';
        return ref;
    };
    long revision = -1;   //首个成功调用只采样基线，其后逐次断言 +1。
    auto applyPatch = [&](const Json& extra) {
        Json arguments = {{"workspace_id", workspaceId}, {"class_uri", classRef}};
        for (const auto& item : extra.items()) arguments[item.key()] = item.value();
        return call(server, "patch_apply", arguments);
    };
    //成功向公共断言：非 error 且 revision 严格递增（证明变更真实提交）。
    auto expectCommitted = [&](const Json& response, const std::string& hint) {
        ASSERT_FALSE(response.at("result").at("isError")) << hint << " | " << response.dump();
        const auto now = payload(response).at("revision").get<long>();
        if (revision >= 0) EXPECT_EQ(revision + 1, now) << hint << " | " << response.dump();
        revision = now;
    };
    //unknown-handle 向公共断言：槽层工具错误（error 键 + 码字前缀），
    //绝不串 raw-not-found，也绝不落下游 canonical NOT_FOUND（那不带 error 键）。
    auto expectUnknownHandle = [&](const Json& response, const std::string& hint) {
        ASSERT_TRUE(response.at("result").at("isError").get<bool>()) << hint << " | " << response.dump();
        const auto& content = payload(response);
        ASSERT_TRUE(content.contains("error")) << hint << " | " << response.dump();
        const auto message = content.at("error").get<std::string>();
        EXPECT_NE(std::string::npos, message.find("unknown-handle")) << hint << " | " << message;
        EXPECT_EQ(std::string::npos, message.find("raw-not-found")) << hint << " | " << message;
    };
    auto classBytes = [&](const std::string& uri) {
        const auto bytes = call(server, "class_bytes", {{"workspace_id", workspaceId}, {"class", uri}});
        EXPECT_FALSE(bytes.at("result").at("isError")) << bytes.dump();
        const auto& encoded = payload(bytes).at("content").get_ref<const std::string&>();
        static const std::string alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<ca::u8> decoded;
        int buffer = 0;
        int bits   = 0;
        for (const char c : encoded) {
            if (c == '=' || alphabet.find(c) == std::string::npos) continue;
            buffer = (buffer << 6) | static_cast<int>(alphabet.find(c));
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                decoded.push_back(static_cast<ca::u8>((buffer >> bits) & 0xFF));
            }
        }
        return decoded;
    };

    //── 前置（既有接线面 replace_method_body，MemberRef + 显式一致 desc）：
    //给 valueI 装一个带 LVT 条目 "held" + 一枚 try-catch 的方法体（Lh+athrow
    //是 catch 专用 handler 入口；标号随 LVT/try-catch 属性跨写读往返存活），
    //让 rename_local 与后续 replace_try_catch_blocks 端到端落地。
    const auto body = applyPatch(
        {{"kind", "replace_method_body"},
         {"member_name", methodRef},
         {"member_descriptor", "()I"},
         {"initial_instructions",
          Json::array({{{"kind", "label"}, {"label", "Ls"}},
                       {{"kind", "int"}, {"opcode", 16}, {"operand", 7}},
                       {{"kind", "var"}, {"opcode", 54}, {"operand", 0}},
                       {{"kind", "var"}, {"opcode", 21}, {"operand", 0}},
                       {{"kind", "label"}, {"label", "Le"}},
                       {{"kind", "simple"}, {"opcode", 172}},
                       {{"kind", "label"}, {"label", "Lh"}},
                       {{"kind", "simple"}, {"opcode", 191}}})},
         {"local_variables",
          Json::array({Json{{"name", "held"},
                            {"descriptor", "I"},
                            {"start_label", "Ls"},
                            {"end_label", "Le"},
                            {"index", 0}}})},
         {"try_catch_blocks",
          Json::array({Json{{"start_label", "Ls"},
                            {"end_label", "Le"},
                            {"handler_label", "Lh"},
                            {"type", "java/lang/Exception"}}})}});
    expectCommitted(body, "replace_method_body setup");
    const auto beforeRename = classBytes("mcp/Subject");
    ASSERT_NE(std::string::npos,
              std::string(reinterpret_cast<const char*>(beforeRename.data()), beforeRename.size())
                  .find("held"))
        << "LVT entry missing after body replacement";

    //── 1. rename_local：MemberRef 寻址既有方法 → 局部变量改名落地。
    const auto renameLocal =
        applyPatch({{"kind", "rename_local"},
                    {"member_name", methodRef},
                    {"member_descriptor", "()I"},
                    {"local_name", "held"},
                    {"replacement_local_name", "kept"}});
    expectCommitted(renameLocal, "rename_local");
    const auto afterRename = classBytes("mcp/Subject");
    const std::string afterRenameText(reinterpret_cast<const char*>(afterRename.data()),
                                      afterRename.size());
    EXPECT_NE(std::string::npos, afterRenameText.find("kept")) << "local rename did not land";
    EXPECT_EQ(std::string::npos, afterRenameText.find("held")) << "stale local name survived";

    //── 2. replace_try_catch_blocks：MemberRef 寻址既有方法，把 setup 装入的
    //catch 类型 Exception → Throwable（标号随属性跨往返存活，真实变更必提交）。
    expectCommitted(applyPatch({{"kind", "replace_try_catch_blocks"},
                                {"member_name", methodRef},
                                {"member_descriptor", "()I"},
                                {"try_catch_blocks",
                                 Json::array({Json{{"start_instruction", 0},
                                                   {"end_instruction", 4},
                                                   {"handler_instruction", 6},
                                                   {"type", "java/lang/Throwable"}}})}}),
                    "replace_try_catch_blocks");

    //── 3. insert_instruction：MemberRef 寻址既有方法，bipush 前插 nop。
    expectCommitted(applyPatch({{"kind", "insert_instruction"},
                                {"member_name", methodRef},
                                {"member_descriptor", "()I"},
                                {"instruction_index", 1},
                                {"instruction", Json{{"kind", "simple"}, {"opcode", 0}}}}),
                    "insert_instruction");

    //── 4. replace_instruction：MemberRef 寻址既有方法，bipush 7 → iconst_1。
    expectCommitted(applyPatch({{"kind", "replace_instruction"},
                                {"member_name", methodRef},
                                {"member_descriptor", "()I"},
                                {"instruction_index", 2},
                                {"instruction", Json{{"kind", "simple"}, {"opcode", 4}}}}),
                    "replace_instruction");

    //── 5. remove_instruction：MemberRef 寻址既有方法，移除步骤 3 插入的 nop。
    expectCommitted(applyPatch({{"kind", "remove_instruction"},
                                {"member_name", methodRef},
                                {"member_descriptor", "()I"},
                                {"instruction_index", 1}}),
                    "remove_instruction");

    //── 6. add_marker_annotation：field|method 两目标都经 findField/findMethod
    //寻址既有成员（MemberRef 解析后落地）。
    expectCommitted(applyPatch({{"kind", "add_marker_annotation"},
                                {"member_name", fieldRef},
                                {"member_descriptor", "I"},
                                {"annotation_target", "field"},
                                {"annotation_descriptor", "Lmcp/T2Ann;"}}),
                    "add_marker_annotation field");
    expectCommitted(applyPatch({{"kind", "add_marker_annotation"},
                                {"member_name", methodRef},
                                {"member_descriptor", "()I"},
                                {"annotation_target", "method"},
                                {"annotation_descriptor", "Lmcp/T2Ann;"}}),
                    "add_marker_annotation method");

    //── 7. remove_annotation：field|method 两目标同口径。
    expectCommitted(applyPatch({{"kind", "remove_annotation"},
                                {"member_name", fieldRef},
                                {"member_descriptor", "I"},
                                {"annotation_target", "field"},
                                {"annotation_descriptor", "Lmcp/T2Ann;"}}),
                    "remove_annotation field");
    expectCommitted(applyPatch({{"kind", "remove_annotation"},
                                {"member_name", methodRef},
                                {"member_descriptor", "()I"},
                                {"annotation_target", "method"},
                                {"annotation_descriptor", "Lmcp/T2Ann;"}}),
                    "remove_annotation method");

    //── 负控（不越界解析）：annotation_target=class 时 member 不寻址——表外
    //handle 形态字面量原样透传，绝不 unknown-handle。
    const auto classTarget = applyPatch({{"kind", "add_marker_annotation"},
                                         {"member_name", "${Class_1.deadbeef}"},
                                         {"annotation_target", "class"},
                                         {"annotation_descriptor", "Lmcp/T2Ann;"}});
    EXPECT_FALSE(classTarget.at("result").at("isError")) << classTarget.dump();

    //── unknown-handle 响亮向 ×7：成员 token 手滑 → 工具边界四分类拦截。
    const auto badMethod = flipToken(methodRef);
    const auto badField  = flipToken(fieldRef);
    expectUnknownHandle(applyPatch({{"kind", "rename_local"},
                                    {"member_name", badMethod},
                                    {"member_descriptor", "()I"},
                                    {"local_name", "kept"},
                                    {"replacement_local_name", "moved"}}),
                        "rename_local");
    expectUnknownHandle(applyPatch({{"kind", "replace_try_catch_blocks"},
                                    {"member_name", badMethod},
                                    {"member_descriptor", "()I"}}),
                        "replace_try_catch_blocks");
    expectUnknownHandle(applyPatch({{"kind", "insert_instruction"},
                                    {"member_name", badMethod},
                                    {"member_descriptor", "()I"},
                                    {"instruction_index", 0},
                                    {"instruction", Json{{"kind", "simple"}, {"opcode", 0}}}}),
                        "insert_instruction");
    expectUnknownHandle(applyPatch({{"kind", "replace_instruction"},
                                    {"member_name", badMethod},
                                    {"member_descriptor", "()I"},
                                    {"instruction_index", 0},
                                    {"instruction", Json{{"kind", "simple"}, {"opcode", 4}}}}),
                        "replace_instruction");
    expectUnknownHandle(applyPatch({{"kind", "remove_instruction"},
                                    {"member_name", badMethod},
                                    {"member_descriptor", "()I"},
                                    {"instruction_index", 0}}),
                        "remove_instruction");
    expectUnknownHandle(applyPatch({{"kind", "add_marker_annotation"},
                                    {"member_name", badField},
                                    {"member_descriptor", "I"},
                                    {"annotation_target", "field"},
                                    {"annotation_descriptor", "Lmcp/T2Ann;"}}),
                        "add_marker_annotation");
    expectUnknownHandle(applyPatch({{"kind", "remove_annotation"},
                                    {"member_name", badMethod},
                                    {"member_descriptor", "()I"},
                                    {"annotation_target", "method"},
                                    {"annotation_descriptor", "Lmcp/T2Ann;"}}),
                        "remove_annotation");
}

//───────────────────── shadow_to_raw 接受 Ref 输入 ─────────────────────

TEST(McpShadowTokenTest, ShadowToRawAcceptsHandleInput)
{
    const auto jar = uniqueFixtureJar();
    McpSessionHarness server;
    const auto workspaceId = openWithSidecar(server, jar);
    const auto classRef =
        firstMatch(server, workspaceId, "mcp/Subject").at("ref").get<std::string>();

    const auto response = call(server, "shadow_to_raw", {{"workspace_id", workspaceId}, {"name", classRef}});
    ASSERT_FALSE(response.at("result").at("isError")) << response.dump();
    const auto& matches = payload(response).at("matches");
    ASSERT_GE(matches.size(), 1U);
    EXPECT_EQ("mcp/Subject", matches.at(0).at("name"));
    EXPECT_EQ(classRef, matches.at(0).at("ref"));

    //raw_to_shadow 拒收 Ref（语义面分立，指路 shadow_to_raw）。
    const auto wrongWay = call(server, "raw_to_shadow", {{"workspace_id", workspaceId}, {"name", classRef}});
    expectSlotError(wrongWay, "shadow_to_raw", "", "reverse direction");
}

}   //namespace mjt::mcp
