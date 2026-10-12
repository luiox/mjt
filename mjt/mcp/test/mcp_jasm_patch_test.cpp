#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

///@brief 打开默认 fixture（mcp/Subject 含 valueI 返回 ldc 1; counter I），
///返回 workspace_id。
std::string openSubjectWorkspace(McpSessionHarness& server)
{
    const auto fixture    = writeFixture();
    const auto opened     = call(server, "workspace_open", {{"path", fixture.string()}});
    EXPECT_FALSE(opened.at("result").at("isError"));
    return payload(opened).at("workspace_id").get<std::string>();
}

///@brief 取 class Jasm 的完整 source 文本。
std::string fetchClassJasm(McpSessionHarness& server,
                           const std::string& workspaceId,
                           const std::string& clazz)
{
    const auto result = call(server, "class_jasm", {{"workspace_id", workspaceId}, {"class", clazz}});
    EXPECT_FALSE(result.at("result").at("isError"));
    return payload(result).at("source").get<std::string>();
}

///@brief 取单方法 Jasm 的完整 source 文本。
std::string fetchMethodJasm(McpSessionHarness& server,
                            const std::string& workspaceId,
                            const std::string& owner,
                            const std::string& name,
                            const std::string& descriptor)
{
    const auto result = call(server,
                             "method_jasm",
                             {{"workspace_id", workspaceId},
                              {"owner", owner},
                              {"name", name},
                              {"descriptor", descriptor}});
    EXPECT_FALSE(result.at("result").at("isError"));
    return payload(result).at("source").get<std::string>();
}

//─────────────────────────────────────────────────────────────────────────────
//工具注册：新工具应出现在 tools/list 并具备正确的 required 集合。
//─────────────────────────────────────────────────────────────────────────────

TEST(McpJasmPatchToolsTest, NewDiffAndMethodToolsAreRegistered)
{
    McpSessionHarness server;
    const auto tools  = server.handle(request("tools/list"));
    const auto listed = tools.at("result").at("tools");

    const auto find = [&](const char* name) {
        return std::find_if(listed.begin(), listed.end(), [&](const Json& item) {
            return item.at("name") == name;
        });
    };
    for (const char* name :
         {"patch_apply_jasm_diff", "patch_validate_jasm_diff", "method_apply_jasm",
          "method_validate_jasm", "method_apply_jasm_diff", "method_validate_jasm_diff"}) {
        SCOPED_TRACE(name);
        ASSERT_NE(listed.end(), find(name));
    }
    const auto applyDiff = find("patch_apply_jasm_diff");
    EXPECT_EQ(Json::array({"workspace_id", "uri", "diff"}),
              applyDiff->at("inputSchema").at("required"));
    const auto methodApply = find("method_apply_jasm");
    EXPECT_EQ(Json::array({"workspace_id", "owner", "name", "descriptor", "source"}),
              methodApply->at("inputSchema").at("required"));
    const auto methodDiff = find("method_apply_jasm_diff");
    EXPECT_EQ(Json::array({"workspace_id", "owner", "name", "descriptor", "diff"}),
              methodDiff->at("inputSchema").at("required"));
}

//─────────────────────────────────────────────────────────────────────────────
//class 级 diff：patch_apply_jasm_diff / patch_validate_jasm_diff
//─────────────────────────────────────────────────────────────────────────────

TEST(McpJasmPatchToolsTest, ApplyJasmDiffCommitsAndChangesBytecode)
{
    McpSessionHarness server;
    const auto        workspaceId = openSubjectWorkspace(server);
    const auto        base        = fetchClassJasm(server, workspaceId, "mcp/Subject");
    ASSERT_NE(std::string::npos, base.find("ldc 1"));
    //构造一个把 ldc 1 改成 ldc 7 的最小 diff：基于 class_jasm 的真实输出，
    //行号由运行时定位，保证 diff 与 canonical base 对齐。
    const auto marker = base.find("ldc 1");
    ASSERT_NE(std::string::npos, marker);
    const auto lineStart = base.rfind('\n', marker) + 1;
    //统计该行是第几行（1-based）。
    ca::usize lineNumber = 1;
    for (ca::usize i = 0; i < lineStart; ++i)
        if (base[i] == '\n') ++lineNumber;
    const auto oldValueLine = base.substr(lineStart, base.find('\n', lineStart) - lineStart);
    const std::string newValueLine =
        std::string(oldValueLine).replace(oldValueLine.find("ldc 1"), 5, "ldc 7");
    const std::string diff =
        "@@ -" + std::to_string(lineNumber) + ",1 +" + std::to_string(lineNumber) + ",1 @@\n" +
        "-" + oldValueLine + "\n+" + newValueLine + "\n";

    const auto applied = call(server,
                              "patch_apply_jasm_diff",
                              {{"workspace_id", workspaceId},
                               {"uri", "mcp/Subject.class"},
                               {"diff", diff},
                               {"expected_revision", 0}});
    ASSERT_FALSE(applied.at("result").at("isError")) << applied;
    EXPECT_TRUE(payload(applied).at("committed"));
    EXPECT_EQ(1, payload(applied).at("revision"));
    //校验改动落地：重新取 class_jasm，应含 ldc 7 而非 ldc 1。
    const auto after = fetchClassJasm(server, workspaceId, "mcp/Subject");
    EXPECT_NE(std::string::npos, after.find("ldc 7"));
    EXPECT_EQ(std::string::npos, after.find("ldc 1"));
}

TEST(McpJasmPatchToolsTest, ValidateJasmDiffIsDryRunAndDoesNotCommit)
{
    McpSessionHarness server;
    const auto        workspaceId = openSubjectWorkspace(server);
    const auto        base        = fetchClassJasm(server, workspaceId, "mcp/Subject");
    const auto        marker      = base.find("ldc 1");
    ASSERT_NE(std::string::npos, marker);
    const auto lineStart = base.rfind('\n', marker) + 1;
    ca::usize lineNumber = 1;
    for (ca::usize i = 0; i < lineStart; ++i)
        if (base[i] == '\n') ++lineNumber;
    const auto oldValueLine = base.substr(lineStart, base.find('\n', lineStart) - lineStart);
    const std::string newValueLine =
        std::string(oldValueLine).replace(oldValueLine.find("ldc 1"), 5, "ldc 2");
    const std::string diff =
        "@@ -" + std::to_string(lineNumber) + ",1 +" + std::to_string(lineNumber) + ",1 @@\n" +
        "-" + oldValueLine + "\n+" + newValueLine + "\n";

    const auto preview = call(server,
                              "patch_validate_jasm_diff",
                              {{"workspace_id", workspaceId},
                               {"uri", "mcp/Subject.class"},
                               {"diff", diff},
                               {"dry_run", true}});
    ASSERT_FALSE(preview.at("result").at("isError")) << preview;
    EXPECT_FALSE(payload(preview).at("committed"));
    EXPECT_EQ(0, payload(preview).at("revision"));
    //dry_run 不应改变实际 class。
    const auto after = fetchClassJasm(server, workspaceId, "mcp/Subject");
    EXPECT_NE(std::string::npos, after.find("ldc 1"));
}

TEST(McpJasmPatchToolsTest, ApplyJasmDiffRejectsContextMismatch)
{
    McpSessionHarness server;
    const auto        workspaceId = openSubjectWorkspace(server);
    //故意构造一个 context 与真实 base 不符的 diff：行体完全错误。
    const std::string diff =
        "@@ -1,1 +1,1 @@\n"
        "-THIS_LINE_DOES_NOT_EXIST\n"
        "+anything\n";
    const auto failed = call(server,
                             "patch_apply_jasm_diff",
                             {{"workspace_id", workspaceId},
                              {"uri", "mcp/Subject.class"},
                              {"diff", diff},
                              {"expected_revision", 0}});
    EXPECT_TRUE(failed.at("result").at("isError"));
    EXPECT_EQ(1, payload(failed).at("failed_hunk").get<int>());
}

//─────────────────────────────────────────────────────────────────────────────
//单方法编辑：method_apply_jasm / method_apply_jasm_diff / 对应 validate
//─────────────────────────────────────────────────────────────────────────────

TEST(McpJasmPatchToolsTest, MethodApplyJasmReplacesOneMethodKeepingOthersIntact)
{
    McpSessionHarness server;
    const auto        workspaceId = openSubjectWorkspace(server);
    const auto baseClass   = fetchClassJasm(server, workspaceId, "mcp/Subject");
    const auto oldMethod   = fetchMethodJasm(server, workspaceId, "mcp/Subject", "value", "()I");
    ASSERT_FALSE(oldMethod.empty());
    ASSERT_NE(std::string::npos, baseClass.find(oldMethod))
        << "method_jasm 输出应是 class_jasm 的逐字节子串";

    //把 ldc 1 改成 ldc 9，得到新方法 source。
    const auto bipushPos = oldMethod.find("ldc 1");
    ASSERT_NE(std::string::npos, bipushPos);
    std::string newMethod = oldMethod;
    newMethod.replace(bipushPos, 5, "ldc 9");

    const auto applied = call(server,
                              "method_apply_jasm",
                              {{"workspace_id", workspaceId},
                               {"owner", "mcp/Subject"},
                               {"name", "value"},
                               {"descriptor", "()I"},
                               {"source", newMethod},
                               {"expected_revision", 0}});
    ASSERT_FALSE(applied.at("result").at("isError")) << applied;
    EXPECT_TRUE(payload(applied).at("committed"));
    EXPECT_EQ(1, payload(applied).at("revision"));

    //验证：改动方法已更新，未改部分（counter 字段等）保持不变。
    const auto afterClass = fetchClassJasm(server, workspaceId, "mcp/Subject");
    EXPECT_NE(std::string::npos, afterClass.find("ldc 9"));
    EXPECT_EQ(std::string::npos, afterClass.find("ldc 1"));
    EXPECT_NE(std::string::npos, afterClass.find("counter I"));
}

TEST(McpJasmPatchToolsTest, MethodValidateJasmIsDryRun)
{
    McpSessionHarness server;
    const auto        workspaceId = openSubjectWorkspace(server);
    const auto oldMethod = fetchMethodJasm(server, workspaceId, "mcp/Subject", "value", "()I");
    const auto bipushPos = oldMethod.find("ldc 1");
    ASSERT_NE(std::string::npos, bipushPos);
    std::string newMethod = oldMethod;
    newMethod.replace(bipushPos, 5, "ldc 5");

    const auto preview = call(server,
                              "method_validate_jasm",
                              {{"workspace_id", workspaceId},
                               {"owner", "mcp/Subject"},
                               {"name", "value"},
                               {"descriptor", "()I"},
                               {"source", newMethod},
                               {"dry_run", true}});
    ASSERT_FALSE(preview.at("result").at("isError")) << preview;
    EXPECT_FALSE(payload(preview).at("committed"));
    EXPECT_EQ(0, payload(preview).at("revision"));
    const auto after = fetchClassJasm(server, workspaceId, "mcp/Subject");
    EXPECT_NE(std::string::npos, after.find("ldc 1"));
}

TEST(McpJasmPatchToolsTest, MethodApplyJasmDiffAppliesMinimalMethodPatch)
{
    McpSessionHarness server;
    const auto        workspaceId = openSubjectWorkspace(server);
    const auto oldMethod = fetchMethodJasm(server, workspaceId, "mcp/Subject", "value", "()I");
    const auto bipushPos = oldMethod.find("ldc 1");
    ASSERT_NE(std::string::npos, bipushPos);
    const auto lineStart = oldMethod.rfind('\n', bipushPos) + 1;
    ca::usize lineNumber = 1;
    for (ca::usize i = 0; i < lineStart; ++i)
        if (oldMethod[i] == '\n') ++lineNumber;
    const auto oldValueLine =
        oldMethod.substr(lineStart, oldMethod.find('\n', lineStart) - lineStart);
    const std::string newValueLine =
        std::string(oldValueLine).replace(oldValueLine.find("ldc 1"), 5, "ldc 8");
    const std::string diff = "@@ -" + std::to_string(lineNumber) + ",1 +" +
                             std::to_string(lineNumber) + ",1 @@\n-" + oldValueLine + "\n+" +
                             newValueLine + "\n";

    const auto applied = call(server,
                              "method_apply_jasm_diff",
                              {{"workspace_id", workspaceId},
                               {"owner", "mcp/Subject"},
                               {"name", "value"},
                               {"descriptor", "()I"},
                               {"diff", diff},
                               {"expected_revision", 0}});
    ASSERT_FALSE(applied.at("result").at("isError")) << applied;
    EXPECT_TRUE(payload(applied).at("committed"));
    EXPECT_EQ(1, payload(applied).at("revision"));
    const auto afterClass = fetchClassJasm(server, workspaceId, "mcp/Subject");
    EXPECT_NE(std::string::npos, afterClass.find("ldc 8"));
    EXPECT_NE(std::string::npos, afterClass.find("counter I"));
}

TEST(McpJasmPatchToolsTest, MethodApplyJasmDiffRejectsContextMismatch)
{
    McpSessionHarness server;
    const auto        workspaceId = openSubjectWorkspace(server);
    const std::string diff =
        "@@ -1,1 +1,1 @@\n"
        "-NONEXISTENT_METHOD_LINE\n"
        "+x\n";
    const auto failed = call(server,
                             "method_apply_jasm_diff",
                             {{"workspace_id", workspaceId},
                              {"owner", "mcp/Subject"},
                              {"name", "value"},
                              {"descriptor", "()I"},
                              {"diff", diff},
                              {"expected_revision", 0}});
    EXPECT_TRUE(failed.at("result").at("isError"));
    EXPECT_EQ(1, payload(failed).at("failed_hunk").get<int>());
}

}   //namespace
}   //namespace mjt::mcp
