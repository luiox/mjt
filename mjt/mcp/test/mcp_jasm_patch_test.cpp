#include <algorithm>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

///@brief 打开默认 fixture（mcp/Subject 含 valueI 返回 ldc 1; counter I）的
///临时工程，返回 project_id。
std::string openSubjectProject(McpSessionHarness& server)
{
    const auto fixture    = writeFixture();
    const auto opened     = call(server, "project_open", {{"path", fixture.string()}});
    EXPECT_FALSE(opened.at("result").at("isError"));
    return payload(opened).at("project_id").get<std::string>();
}

///@brief 取 class Jasm 的完整 source 文本。
std::string fetchClassJasm(McpSessionHarness& server,
                           const std::string& projectId,
                           const std::string& clazz)
{
    const auto result = call(server,
                             "class_query",
                             {{"project_id", projectId}, {"kind", "jasm"}, {"class", clazz}});
    EXPECT_FALSE(result.at("result").at("isError"));
    return payload(result).at("jasm").get<std::string>();
}

///@brief 取单方法 Jasm 的完整 source 文本。
std::string fetchMethodJasm(McpSessionHarness& server,
                            const std::string& projectId,
                            const std::string& owner,
                            const std::string& name,
                            const std::string& descriptor)
{
    const auto result = call(server,
                             "class_query",
                             {{"project_id", projectId},
                              {"kind", "method_jasm"},
                              {"owner", owner},
                              {"name", name},
                              {"descriptor", descriptor}});
    EXPECT_FALSE(result.at("result").at("isError"));
    return payload(result).at("jasm").get<std::string>();
}

///@brief 构造单行替换 unified diff（基于 base 中 oldValue 的真实行）。
std::string singleLineDiff(const std::string& base,
                           const std::string& oldValue,
                           const std::string& newValue)
{
    const auto marker = base.find(oldValue);
    EXPECT_NE(std::string::npos, marker);
    const auto lineStart = base.rfind('\n', marker) + 1;
    ca::usize lineNumber = 1;
    for (ca::usize i = 0; i < lineStart; ++i)
        if (base[i] == '\n') ++lineNumber;
    const auto oldValueLine = base.substr(lineStart, base.find('\n', lineStart) - lineStart);
    const std::string newValueLine =
        std::string(oldValueLine).replace(oldValueLine.find(oldValue), oldValue.size(), newValue);
    return "@@ -" + std::to_string(lineNumber) + ",1 +" + std::to_string(lineNumber) + ",1 @@\n" +
           "-" + oldValueLine + "\n+" + newValueLine + "\n";
}

Json applyEdit(McpSessionHarness& server,
               const std::string& projectId,
               const char*        kind,
               Json               extra)
{
    extra["project_id"] = projectId;
    extra["kind"]       = kind;
    return call(server, "edit_apply", std::move(extra));
}

//─────────────────────────────────────────────────────────────────────────────
//工具注册：edit_apply 的 kind 选择器覆盖 Jasm/diff/方法编辑四族。
//─────────────────────────────────────────────────────────────────────────────

TEST(McpJasmPatchToolsTest, EditApplyKindSelectorIsRegistered)
{
    McpSessionHarness server;
    const auto tools  = server.handle(request("tools/list"));
    const auto listed = tools.at("result").at("tools");

    const auto found = std::find_if(listed.begin(), listed.end(), [&](const Json& item) {
        return item.at("name") == "edit_apply";
    });
    ASSERT_NE(listed.end(), found);
    const auto& schema = found->at("inputSchema");
    EXPECT_EQ(Json::array({"project_id", "kind"}), schema.at("required"));
    const auto& kinds = schema.at("properties").at("kind").at("enum");
    for (const char* kind : {"jasm", "jasm_diff", "method_jasm", "method_jasm_diff"}) {
        SCOPED_TRACE(kind);
        EXPECT_NE(kinds.end(), std::find(kinds.begin(), kinds.end(), Json(kind)));
    }
}

//─────────────────────────────────────────────────────────────────────────────
//class 级 diff：edit_apply(kind=jasm_diff)，dry_run 预览不提交。
//─────────────────────────────────────────────────────────────────────────────

TEST(McpJasmPatchToolsTest, ApplyJasmDiffCommitsAndChangesBytecode)
{
    McpSessionHarness server;
    const auto        projectId = openSubjectProject(server);
    const auto        base      = fetchClassJasm(server, projectId, "mcp/Subject");
    ASSERT_NE(std::string::npos, base.find("ldc 1"));
    //构造一个把 ldc 1 改成 ldc 7 的最小 diff：基于 class Jasm 的真实输出，
    //行号由运行时定位，保证 diff 与 canonical base 对齐。
    const auto diff = singleLineDiff(base, "ldc 1", "ldc 7");

    const auto applied = applyEdit(server,
                                   projectId,
                                   "jasm_diff",
                                   {{"uri", "mcp/Subject.class"},
                                    {"diff", diff},
                                    {"expected_revision", 0}});
    ASSERT_FALSE(applied.at("result").at("isError")) << applied;
    EXPECT_TRUE(payload(applied).at("committed"));
    EXPECT_EQ(1, payload(applied).at("revision"));
    //校验改动落地：重新取 class Jasm，应含 ldc 7 而非 ldc 1。
    const auto after = fetchClassJasm(server, projectId, "mcp/Subject");
    EXPECT_NE(std::string::npos, after.find("ldc 7"));
    EXPECT_EQ(std::string::npos, after.find("ldc 1"));
}

TEST(McpJasmPatchToolsTest, ValidateJasmDiffIsDryRunAndDoesNotCommit)
{
    McpSessionHarness server;
    const auto        projectId = openSubjectProject(server);
    const auto        base      = fetchClassJasm(server, projectId, "mcp/Subject");
    const auto        diff      = singleLineDiff(base, "ldc 1", "ldc 2");

    const auto preview = applyEdit(server,
                                   projectId,
                                   "jasm_diff",
                                   {{"uri", "mcp/Subject.class"},
                                    {"diff", diff},
                                    {"dry_run", true}});
    ASSERT_FALSE(preview.at("result").at("isError")) << preview;
    EXPECT_FALSE(payload(preview).at("committed"));
    EXPECT_EQ(0, payload(preview).at("revision"));
    //dry_run 不应改变实际 class。
    const auto after = fetchClassJasm(server, projectId, "mcp/Subject");
    EXPECT_NE(std::string::npos, after.find("ldc 1"));
}

TEST(McpJasmPatchToolsTest, ApplyJasmDiffRejectsContextMismatch)
{
    McpSessionHarness server;
    const auto        projectId = openSubjectProject(server);
    //故意构造一个 context 与真实 base 不符的 diff：行体完全错误。
    const std::string diff =
        "@@ -1,1 +1,1 @@\n"
        "-THIS_LINE_DOES_NOT_EXIST\n"
        "+anything\n";
    const auto failed = applyEdit(server,
                                  projectId,
                                  "jasm_diff",
                                  {{"uri", "mcp/Subject.class"},
                                   {"diff", diff},
                                   {"expected_revision", 0}});
    EXPECT_TRUE(failed.at("result").at("isError"));
    EXPECT_EQ(1, payload(failed).at("failed_hunk").get<int>());
}

//─────────────────────────────────────────────────────────────────────────────
//单方法编辑：edit_apply(kind=method_jasm / method_jasm_diff)。
//─────────────────────────────────────────────────────────────────────────────

TEST(McpJasmPatchToolsTest, MethodApplyJasmReplacesOneMethodKeepingOthersIntact)
{
    McpSessionHarness server;
    const auto        projectId = openSubjectProject(server);
    const auto baseClass   = fetchClassJasm(server, projectId, "mcp/Subject");
    const auto oldMethod   = fetchMethodJasm(server, projectId, "mcp/Subject", "value", "()I");
    ASSERT_FALSE(oldMethod.empty());
    ASSERT_NE(std::string::npos, baseClass.find(oldMethod))
        << "method Jasm 输出应是 class Jasm 的逐字节子串";

    //把 ldc 1 改成 ldc 9，得到新方法 source。
    const auto bipushPos = oldMethod.find("ldc 1");
    ASSERT_NE(std::string::npos, bipushPos);
    std::string newMethod = oldMethod;
    newMethod.replace(bipushPos, 5, "ldc 9");

    const auto applied = applyEdit(server,
                                   projectId,
                                   "method_jasm",
                                   {{"owner", "mcp/Subject"},
                                    {"name", "value"},
                                    {"descriptor", "()I"},
                                    {"source", newMethod},
                                    {"expected_revision", 0}});
    ASSERT_FALSE(applied.at("result").at("isError")) << applied;
    EXPECT_TRUE(payload(applied).at("committed"));
    EXPECT_EQ(1, payload(applied).at("revision"));

    //验证：改动方法已更新，未改部分（counter 字段等）保持不变。
    const auto afterClass = fetchClassJasm(server, projectId, "mcp/Subject");
    EXPECT_NE(std::string::npos, afterClass.find("ldc 9"));
    EXPECT_EQ(std::string::npos, afterClass.find("ldc 1"));
    EXPECT_NE(std::string::npos, afterClass.find("counter I"));
}

TEST(McpJasmPatchToolsTest, MethodValidateJasmIsDryRun)
{
    McpSessionHarness server;
    const auto        projectId = openSubjectProject(server);
    const auto oldMethod = fetchMethodJasm(server, projectId, "mcp/Subject", "value", "()I");
    const auto bipushPos = oldMethod.find("ldc 1");
    ASSERT_NE(std::string::npos, bipushPos);
    std::string newMethod = oldMethod;
    newMethod.replace(bipushPos, 5, "ldc 5");

    const auto preview = applyEdit(server,
                                   projectId,
                                   "method_jasm",
                                   {{"owner", "mcp/Subject"},
                                    {"name", "value"},
                                    {"descriptor", "()I"},
                                    {"source", newMethod},
                                    {"dry_run", true}});
    ASSERT_FALSE(preview.at("result").at("isError")) << preview;
    EXPECT_FALSE(payload(preview).at("committed"));
    EXPECT_EQ(0, payload(preview).at("revision"));
    const auto after = fetchClassJasm(server, projectId, "mcp/Subject");
    EXPECT_NE(std::string::npos, after.find("ldc 1"));
}

TEST(McpJasmPatchToolsTest, MethodApplyJasmDiffAppliesMinimalMethodPatch)
{
    McpSessionHarness server;
    const auto        projectId = openSubjectProject(server);
    const auto oldMethod = fetchMethodJasm(server, projectId, "mcp/Subject", "value", "()I");
    const auto diff      = singleLineDiff(oldMethod, "ldc 1", "ldc 8");

    const auto applied = applyEdit(server,
                                   projectId,
                                   "method_jasm_diff",
                                   {{"owner", "mcp/Subject"},
                                    {"name", "value"},
                                    {"descriptor", "()I"},
                                    {"diff", diff},
                                    {"expected_revision", 0}});
    ASSERT_FALSE(applied.at("result").at("isError")) << applied;
    EXPECT_TRUE(payload(applied).at("committed"));
    EXPECT_EQ(1, payload(applied).at("revision"));
    const auto afterClass = fetchClassJasm(server, projectId, "mcp/Subject");
    EXPECT_NE(std::string::npos, afterClass.find("ldc 8"));
    EXPECT_NE(std::string::npos, afterClass.find("counter I"));
}

TEST(McpJasmPatchToolsTest, MethodApplyJasmDiffRejectsContextMismatch)
{
    McpSessionHarness server;
    const auto        projectId = openSubjectProject(server);
    const std::string diff =
        "@@ -1,1 +1,1 @@\n"
        "-NONEXISTENT_METHOD_LINE\n"
        "+x\n";
    const auto failed = applyEdit(server,
                                  projectId,
                                  "method_jasm_diff",
                                  {{"owner", "mcp/Subject"},
                                   {"name", "value"},
                                   {"descriptor", "()I"},
                                   {"diff", diff},
                                   {"expected_revision", 0}});
    EXPECT_TRUE(failed.at("result").at("isError"));
    EXPECT_EQ(1, payload(failed).at("failed_hunk").get<int>());
}

}   //namespace
}   //namespace mjt::mcp
