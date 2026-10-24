#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"
#include "tool/limits.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

///收敛后的 11 个粗粒度工具（RFC 0001 §8.1）：工具面契约冻结在此——新增或
///删除工具、复活旧名都须改这张表并说明评审结论。
const std::vector<std::string>& coarseToolNames()
{
    static const std::vector<std::string> names = {
        "class_query",
        "container_mount",
        "decompile",
        "edit_apply",
        "edit_history",
        "project_close",
        "project_export",
        "project_info",
        "project_open",
        "search",
        "xref_query"};
    return names;
}

TEST(McpServerTest, PublishesFrozenCanonicalToolSchemas)
{
    McpToolService service;
    const auto tools = service.tools();
    ASSERT_TRUE(tools.is_array());

    std::vector<std::string> actualNames;
    for (const auto& item : tools) {
        ASSERT_TRUE(item.at("name").is_string());
        ASSERT_TRUE(item.at("description").is_string());
        EXPECT_FALSE(item.at("description").get_ref<const std::string&>().empty());
        const auto& schema = item.at("inputSchema");
        EXPECT_EQ("object", schema.at("type"));
        EXPECT_EQ(false, schema.at("additionalProperties"));
        ASSERT_TRUE(schema.at("properties").is_object());
        if (schema.contains("required")) {
            ASSERT_TRUE(schema.at("required").is_array());
            std::vector<std::string> required;
            for (const auto& name : schema.at("required")) {
                ASSERT_TRUE(name.is_string());
                const auto value = name.get<std::string>();
                EXPECT_TRUE(schema.at("properties").contains(value)) << item.at("name");
                required.push_back(value);
            }
            auto sorted = required;
            std::sort(sorted.begin(), sorted.end());
            EXPECT_EQ(sorted.end(), std::adjacent_find(sorted.begin(), sorted.end()))
                << item.at("name");
        }
        actualNames.push_back(item.at("name").get<std::string>());
    }
    std::sort(actualNames.begin(), actualNames.end());
    EXPECT_EQ(coarseToolNames(), actualNames);
    EXPECT_EQ(11U, actualNames.size());
}

///旧细粒度工具名全部删除（同版本切换、无别名期）：任何旧名调用都按未知
///工具回落 NOT_FOUND，而不是落到相近的粗工具分支。
TEST(McpServerTest, RetiresEveryLegacyToolNameWithoutAlias)
{
    McpToolService service;
    const std::vector<std::string> legacyNames = {
        "analysis_evaluate", "analysis_workflow", "class_bytes", "class_fidelity_check",
        "class_get", "class_jasm", "class_list", "class_relations", "class_source",
        "decompiler_compare", "decompiler_decompile", "decompiler_list",
        "decompiler_settings_get", "decompiler_settings_set", "jar_diff",
        "jar_fix_analyze", "jar_fix_apply", "job_cancel", "job_get", "job_release",
        "mapping_analyze", "mapping_apply", "method_apply_jasm",
        "method_apply_jasm_diff", "method_bytecode", "method_callees",
        "method_callers", "method_get", "method_jasm", "method_overrides",
        "method_source", "method_validate_jasm", "method_validate_jasm_diff",
        "opt_apply", "opt_preview", "pass_describe", "pass_run", "patch_apply",
        "patch_apply_jasm", "patch_apply_jasm_diff", "patch_replace_class_bytes",
        "patch_validate_jasm", "patch_validate_jasm_diff", "raw_to_shadow",
        "rename_analyze", "rename_apply", "resource_manifest", "resource_read",
        "resource_strings", "search_classes", "search_instructions",
        "search_members", "search_numbers", "search_strings", "shadow_generate",
        "shadow_prune", "shadow_to_raw", "workspace_add_support", "workspace_close",
        "workspace_export", "workspace_info", "workspace_list_support",
        "workspace_modified_classes", "workspace_open", "workspace_redo",
        "workspace_remove_support", "workspace_resources", "workspace_restore",
        "workspace_revert_class", "workspace_savepoint", "workspace_undo",
        "xref_class", "xref_field", "xref_method"};
    for (const auto& name : legacyNames) {
        SCOPED_TRACE(name);
        const auto called = service.call(name, Json::object());
        ASSERT_TRUE(called.at("isError").get<bool>());
        const auto& details = called.at("structuredContent");
        EXPECT_NE(std::string::npos,
                  details.at("error").get_ref<const std::string&>().find("unknown tool"))
            << details.dump();
    }
}

TEST(McpServerTest, ShapesCoarseToolSelectors)
{
    McpToolService service;
    const auto tools = service.tools();
    const auto schemaFor = [&](std::string_view name) {
        const auto found = std::find_if(tools.begin(), tools.end(), [&](const Json& item) {
            return item.at("name").get_ref<const std::string&>() == name;
        });
        EXPECT_NE(tools.end(), found);
        return found == tools.end() ? Json::object() : found->at("inputSchema");
    };
    //project_id 取代 workspace_id 成为统一会话寻址键。
    for (const auto& name : coarseToolNames()) {
        if (name == "project_open") continue;
        const auto schema = schemaFor(name);
        EXPECT_TRUE(schema.at("properties").contains("project_id")) << name;
        ASSERT_FALSE(schema.at("required").empty()) << name;
        EXPECT_EQ("project_id", schema.at("required").at(0)) << name;
        EXPECT_FALSE(schema.at("properties").contains("workspace_id")) << name;
    }
    const auto editApply = schemaFor("edit_apply");
    EXPECT_EQ(Json::array({"project_id", "kind"}), editApply.at("required"));
    EXPECT_EQ(Json::array({"structured", "jasm", "jasm_diff", "method_jasm",
                           "method_jasm_diff", "class_bytes", "pipeline", "rename",
                           "mapping"}),
              editApply.at("properties").at("kind").at("enum"));
    EXPECT_EQ(Json::array({"change_class_access", "set_field_access", "set_method_access",
                           "add_field", "remove_field", "add_method", "remove_method",
                           "add_marker_annotation", "remove_annotation", "rename_local",
                           "replace_try_catch_blocks", "replace_method_body",
                           "insert_instruction", "replace_instruction", "remove_instruction"}),
              editApply.at("properties").at("mutation_kind").at("enum"));
    EXPECT_EQ(Json::array({"class", "package", "field", "method", "local"}),
              editApply.at("properties").at("rename_kind").at("enum"));
    EXPECT_EQ(true, editApply.at("properties").at("access_flags").at("uniqueItems"));
    EXPECT_EQ(0, editApply.at("properties").at("existing_labels").at("items")
                     .at("properties").at("instruction_index").at("minimum"));
    const auto classQuery = schemaFor("class_query");
    EXPECT_EQ(0, classQuery.at("properties").at("offset").at("minimum"));
    //class_query 的 limit 复用为 bytes/resource_bytes 的分块上限，故覆盖分页默认。
    EXPECT_EQ(ca::usize{detail::kMaximumResourceChunkSize},
              classQuery.at("properties").at("limit").at("maximum"));
    EXPECT_EQ(ca::usize{detail::kMaximumPageSize},
              schemaFor("search").at("properties").at("limit").at("maximum"));
    EXPECT_TRUE(classQuery.at("properties").at("cursor").contains("description"));
    EXPECT_EQ(((mjbase::workspace::kMaximumClassReplacementSize + 2) / 3) * 4,
              editApply.at("properties").at("content_base64").at("maxLength"));
    const auto search = schemaFor("search");
    EXPECT_EQ(Json::array({"classes", "members", "strings", "numbers", "instructions",
                           "resources", "shadow"}),
              search.at("properties").at("kind").at("enum"));
    EXPECT_EQ(Json::array({"shadow_to_raw", "raw_to_shadow"}),
              search.at("properties").at("direction").at("enum"));
    const auto xref = schemaFor("xref_query");
    EXPECT_EQ(Json::array({"references", "relations", "overrides"}),
              xref.at("properties").at("kind").at("enum"));
    EXPECT_EQ(300000,
              schemaFor("decompile").at("properties").at("timeout_ms").at("maximum"));
    EXPECT_EQ(4096,
              schemaFor("decompile").at("properties").at("max_memory_mib").at("maximum"));
    const auto decompile = schemaFor("decompile");
    EXPECT_EQ(Json::array({"decompile", "poll", "cancel", "release"}),
              decompile.at("properties").at("action").at("enum"));
    const auto mount = schemaFor("container_mount");
    EXPECT_EQ(Json::array({"mount", "detach"}),
              mount.at("properties").at("action").at("enum"));
    EXPECT_EQ(Json::array({"primary", "support", "library", "runtime"}),
              mount.at("properties").at("role").at("enum"));
    const auto history = schemaFor("edit_history");
    EXPECT_EQ(0, history.at("properties").at("expected_revision").at("minimum"));
}

TEST(McpServerTest, RejectsMaliciousToolArgumentsAndUnsafeExports)
{
    McpToolService service;
    expectDirectToolError(service.call("class_query", Json::array()),
                          "class_query", ca::StatusCode::INVALID_ARGUMENT, "must be an object");
    expectDirectToolError(service.call("project_info", {{"project_id", "missing"},
                                                        {"unexpected", true}}),
                          "project_info", ca::StatusCode::INVALID_ARGUMENT,
                          "is not an allowed property");
    expectDirectToolError(service.call("edit_history",
                                       {{"project_id", "missing"},
                                        {"action", "undo"},
                                        {"expected_revision", -1}}),
                          "edit_history", ca::StatusCode::INVALID_ARGUMENT,
                          "below its minimum");
    //search 的 limit 上限是分页默认（1000）；class_query 的 limit 复用为
    //bytes 分块上限（1 MiB），故这里用 search 钉分页上限。
    expectDirectToolError(service.call("search",
                                       {{"project_id", "missing"},
                                        {"kind", "classes"},
                                        {"query", "x"},
                                        {"limit", 1001}}),
                          "search", ca::StatusCode::INVALID_ARGUMENT,
                          "exceeds its maximum");
    expectDirectToolError(service.call("class_query",
                                       {{"project_id", "missing"},
                                        {"kind", "list"},
                                        {"limit", 0}}),
                          "class_query", ca::StatusCode::INVALID_ARGUMENT,
                          "below its minimum");
    expectDirectToolError(service.call("class_query",
                                       {{"project_id", "missing"},
                                        {"kind", "list"},
                                        {"cursor", "1"},
                                        {"offset", 1}}),
                          "class_query", ca::StatusCode::INVALID_ARGUMENT,
                          "mutually exclusive");
    for (const auto& cursor : {"", "-1", "+1", "1x", "184467440737095516160"}) {
        expectDirectToolError(service.call("class_query",
                                           {{"project_id", "missing"},
                                            {"kind", "list"},
                                            {"cursor", cursor}}),
                              "class_query", ca::StatusCode::INVALID_ARGUMENT,
                              "unsigned decimal offset");
    }

    Json deep = 0;
    for (ca::usize index = 0; index < 34; ++index) deep = {{"nested", std::move(deep)}};
    expectDirectToolError(service.call("edit_apply",
                                       {{"project_id", "missing"},
                                        {"kind", "pipeline"},
                                        {"label", std::move(deep)}}),
                          "edit_apply", ca::StatusCode::INVALID_ARGUMENT,
                          "maximum JSON depth 32");
    Json oversizedArray = Json::array();
    for (ca::usize index = 0; index < 4097; ++index) oversizedArray.push_back("public");
    expectDirectToolError(service.call("edit_apply",
                                       {{"project_id", "missing"},
                                        {"kind", "structured"},
                                        {"class_uri", "mcp/Subject.class"},
                                        {"mutation_kind", "change_class_access"},
                                        {"access_flags", std::move(oversizedArray)}}),
                          "edit_apply", ca::StatusCode::INVALID_ARGUMENT,
                          "maximum array length 4096");
    expectDirectToolError(service.call(
                              "edit_apply",
                              {{"project_id", "missing"},
                               {"kind", "structured"},
                               {"class_uri", "mcp/Subject.class"},
                               {"mutation_kind", "add_field"},
                               {"field_value",
                                {{"kind", "dynamic"},
                                 {"bootstrap_arguments",
                                  Json::array({{{"kind", "int"}, {"unexpected", true}}})}}}}),
                          "edit_apply", ca::StatusCode::INVALID_ARGUMENT,
                          "is not an allowed property");
    expectDirectToolError(service.call("project_info", {{"project_id", "missing"}}),
                          "project_info", ca::StatusCode::NOT_FOUND,
                          "project not found");
    expectDirectToolError(service.call("decompile",
                                       {{"project_id", "missing"},
                                        {"action", "poll"},
                                        {"job_id", "missing"}}),
                          "decompile", ca::StatusCode::NOT_FOUND, "job not found");

    const auto fixture = writeFixture();
    const auto opened = service.call("project_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("isError").get<bool>());
    const auto projectId = opened.at("structuredContent").at("project_id").get<std::string>();
    //role=primary 是 project_open 的职责，container_mount 显式拒绝。
    expectDirectToolError(service.call("container_mount",
                                       {{"project_id", projectId},
                                        {"action", "mount"},
                                        {"role", "primary"},
                                        {"path", "whatever.jar"}}),
                          "container_mount", ca::StatusCode::INVALID_ARGUMENT,
                          "bound by project_open");
    expectDirectToolError(service.call("edit_apply",
                                       {{"project_id", projectId},
                                        {"kind", "class_bytes"},
                                        {"class_uri", "mcp/Subject.class"},
                                        {"content_base64", "%%%%"}}),
                          "edit_apply", ca::StatusCode::INVALID_ARGUMENT,
                          "strict RFC 4648 base64");
    const auto maximumBase64 =
        ((mjbase::workspace::kMaximumClassReplacementSize + 2) / 3) * 4;
    expectDirectToolError(service.call("edit_apply",
                                       {{"project_id", projectId},
                                        {"kind", "class_bytes"},
                                        {"class_uri", "mcp/Subject.class"},
                                        {"content_base64", std::string(maximumBase64 + 4, 'A')}}),
                          "edit_apply", ca::StatusCode::INVALID_ARGUMENT,
                          "exceeds maxLength");

    expectDirectToolError(service.call("project_export",
                                       {{"project_id", projectId},
                                        {"path", fixture.string()}}),
                          "project_export", ca::StatusCode::PERMISSION_DENIED,
                          "export project failed");
    const auto existingOutput = test::getTempOutPath("mjt_mcp_existing_export.jar");
    std::ofstream(existingOutput, std::ios::binary | std::ios::trunc) << "occupied";
    expectDirectToolError(service.call("project_export",
                                       {{"project_id", projectId},
                                        {"path", existingOutput.string()}}),
                          "project_export", ca::StatusCode::ALREADY_EXISTS,
                          "export project failed");
    const auto closed = service.call("project_close", {{"project_id", projectId}});
    ASSERT_FALSE(closed.at("isError").get<bool>());
}

}   //namespace
}   //namespace mjt::mcp
