#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

TEST(McpServerTest, PublishesFrozenCanonicalToolSchemas)
{
    McpToolService service;
    const auto tools = service.tools();
    ASSERT_TRUE(tools.is_array());

    const std::vector<std::string> expectedNames = {
        "analysis_evaluate", "analysis_workflow", "class_bytes",
        "class_fidelity_check", "class_get",
        "class_jasm", "class_list", "class_relations", "class_source",
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
        "shadow_prune", "shadow_to_raw",
        "workspace_add_support", "workspace_close",
        "workspace_export", "workspace_info", "workspace_list_support",
        "workspace_modified_classes", "workspace_open", "workspace_redo",
        "workspace_remove_support", "workspace_resources", "workspace_restore",
        "workspace_revert_class", "workspace_savepoint", "workspace_undo",
        "xref_class", "xref_field", "xref_method"};

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
    EXPECT_EQ(expectedNames, actualNames);

    const auto schemaFor = [&](std::string_view name) {
        const auto found = std::find_if(tools.begin(), tools.end(), [&](const Json& item) {
            return item.at("name").get_ref<const std::string&>() == name;
        });
        EXPECT_NE(tools.end(), found);
        return found == tools.end() ? Json::object() : found->at("inputSchema");
    };
    const auto patch = schemaFor("patch_apply");
    EXPECT_EQ(Json::array({"workspace_id", "class_uri", "kind"}), patch.at("required"));
    EXPECT_EQ(Json::array({"set_class_access", "set_field_access", "set_method_access",
                           "add_field", "remove_field", "add_method", "remove_method",
                           "add_marker_annotation", "remove_annotation", "rename_local",
                           "replace_try_catch_blocks", "replace_method_body",
                           "insert_instruction", "replace_instruction", "remove_instruction"}),
              patch.at("properties").at("kind").at("enum"));
    EXPECT_EQ(true,
              patch.at("properties").at("access_flags").at("uniqueItems"));
    EXPECT_EQ(0,
              patch.at("properties").at("existing_labels").at("items")
                  .at("properties").at("instruction_index").at("minimum"));
    const auto rawBytes = schemaFor("patch_replace_class_bytes");
    EXPECT_EQ(Json::array({"workspace_id", "class_uri", "content_base64"}),
              rawBytes.at("required"));
    EXPECT_EQ(((mjbase::workspace::kMaximumClassReplacementSize + 2) / 3) * 4,
              rawBytes.at("properties").at("content_base64").at("maxLength"));
    const auto classList = schemaFor("class_list");
    EXPECT_EQ(0, classList.at("properties").at("offset").at("minimum"));
    EXPECT_EQ(1000, classList.at("properties").at("limit").at("maximum"));
    EXPECT_TRUE(classList.at("properties").at("cursor").contains("description"));
    const auto classJasm = schemaFor("class_jasm");
    EXPECT_EQ(Json::array({"workspace_id", "class"}), classJasm.at("required"));
    EXPECT_TRUE(classJasm.at("properties")
                    .at("class")
                    .at("description")
                    .get<std::string>()
                    .find("JVM internal name") != std::string::npos);
    const auto stringSearch = schemaFor("search_strings");
    EXPECT_TRUE(stringSearch.at("properties")
                    .at("query")
                    .at("description")
                    .get<std::string>()
                    .find("constant pools") != std::string::npos);
    for (const auto& name :
         {"decompiler_list", "decompiler_settings_get", "decompiler_settings_set"}) {
        const auto found = std::find_if(
            tools.begin(), tools.end(), [&](const Json& item) { return item.at("name") == name; });
        ASSERT_NE(tools.end(), found);
        EXPECT_FALSE(found->at("inputSchema").at("properties").contains("workspace_id"));
        EXPECT_TRUE(
            found->at("description").get<std::string>().find("no workspace_id is required") !=
            std::string::npos);
    }
    const auto restore = schemaFor("workspace_restore");
    EXPECT_EQ(0, restore.at("properties").at("expected_revision").at("minimum"));
    const auto settings = schemaFor("decompiler_settings_set");
    EXPECT_EQ(300000, settings.at("properties").at("timeout_ms").at("maximum"));
    EXPECT_EQ(4096, settings.at("properties").at("max_memory_mib").at("maximum"));
}

TEST(McpServerTest, RejectsMaliciousToolArgumentsAndUnsafeExports)
{
    McpToolService service;
    expectDirectToolError(service.call("class_list", Json::array()),
                          "class_list", ca::StatusCode::INVALID_ARGUMENT, "must be an object");
    expectDirectToolError(service.call("decompiler_list", {{"unexpected", true}}),
                          "decompiler_list", ca::StatusCode::INVALID_ARGUMENT,
                          "is not an allowed property");
    expectDirectToolError(service.call("workspace_undo",
                                       {{"workspace_id", "missing"}, {"expected_revision", -1}}),
                          "workspace_undo", ca::StatusCode::INVALID_ARGUMENT,
                          "below its minimum");
    expectDirectToolError(service.call("class_list",
                                       {{"workspace_id", "missing"}, {"limit", 1001}}),
                          "class_list", ca::StatusCode::INVALID_ARGUMENT,
                          "exceeds its maximum");
    expectDirectToolError(service.call("class_list",
                                       {{"workspace_id", "missing"},
                                        {"cursor", "1"},
                                        {"offset", 1}}),
                          "class_list", ca::StatusCode::INVALID_ARGUMENT,
                          "mutually exclusive");
    expectDirectToolError(
        service.call(
            "class_jasm",
            {{"workspace_id", "missing"}, {"class", "mcp/Subject"}, {"uri", "mcp/Subject.class"}}),
        "class_jasm",
        ca::StatusCode::INVALID_ARGUMENT,
        "is not an allowed property");
    for (const auto& cursor : {"", "-1", "+1", "1x", "184467440737095516160"}) {
        expectDirectToolError(service.call("class_list",
                                           {{"workspace_id", "missing"},
                                            {"cursor", cursor}}),
                              "class_list", ca::StatusCode::INVALID_ARGUMENT,
                              "unsigned decimal offset");
    }

    Json deep = 0;
    for (ca::usize index = 0; index < 34; ++index) deep = {{"nested", std::move(deep)}};
    expectDirectToolError(service.call("analysis_evaluate",
                                       {{"expression", "1"}, {"bindings", std::move(deep)}}),
                          "analysis_evaluate", ca::StatusCode::INVALID_ARGUMENT,
                          "maximum JSON depth 32");
    Json oversizedArray = Json::array();
    for (ca::usize index = 0; index < 4097; ++index) oversizedArray.push_back("public");
    expectDirectToolError(service.call("patch_apply",
                                       {{"workspace_id", "missing"},
                                        {"class_uri", "mcp/Subject.class"},
                                        {"kind", "set_class_access"},
                                        {"access_flags", std::move(oversizedArray)}}),
                          "patch_apply", ca::StatusCode::INVALID_ARGUMENT,
                          "maximum array length 4096");
    expectDirectToolError(service.call(
                              "patch_apply",
                              {{"workspace_id", "missing"},
                               {"class_uri", "mcp/Subject.class"},
                               {"kind", "add_field"},
                               {"field_value",
                                {{"kind", "dynamic"},
                                 {"bootstrap_arguments",
                                  Json::array({{{"kind", "int"}, {"unexpected", true}}})}}}}),
                          "patch_apply", ca::StatusCode::INVALID_ARGUMENT,
                          "is not an allowed property");
    expectDirectToolError(service.call("workspace_info", {{"workspace_id", "missing"}}),
                          "workspace_info", ca::StatusCode::NOT_FOUND,
                          "workspace not found");
    expectDirectToolError(service.call("job_get", {{"job_id", "missing"}}),
                          "job_get", ca::StatusCode::NOT_FOUND, "job not found");

    const auto fixture = writeFixture();
    const auto opened = service.call("workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("isError").get<bool>());
    const auto workspaceId = opened.at("structuredContent").at("workspace_id").get<std::string>();
    expectDirectToolError(service.call("patch_replace_class_bytes",
                                       {{"workspace_id", workspaceId},
                                        {"class_uri", "mcp/Subject.class"},
                                        {"content_base64", "%%%%"}}),
                          "patch_replace_class_bytes", ca::StatusCode::INVALID_ARGUMENT,
                          "strict RFC 4648 base64");
    const auto maximumBase64 =
        ((mjbase::workspace::kMaximumClassReplacementSize + 2) / 3) * 4;
    expectDirectToolError(service.call("patch_replace_class_bytes",
                                       {{"workspace_id", workspaceId},
                                        {"class_uri", "mcp/Subject.class"},
                                        {"content_base64", std::string(maximumBase64 + 4, 'A')}}),
                          "patch_replace_class_bytes", ca::StatusCode::INVALID_ARGUMENT,
                          "exceeds maxLength");

    expectDirectToolError(service.call("workspace_export",
                                       {{"workspace_id", workspaceId},
                                        {"path", fixture.string()}}),
                          "workspace_export", ca::StatusCode::PERMISSION_DENIED,
                          "export workspace failed");
    const auto existingOutput = test::getTempOutPath("mjt_mcp_existing_export.jar");
    std::ofstream(existingOutput, std::ios::binary | std::ios::trunc) << "occupied";
    expectDirectToolError(service.call("workspace_export",
                                       {{"workspace_id", workspaceId},
                                        {"path", existingOutput.string()}}),
                          "workspace_export", ca::StatusCode::ALREADY_EXISTS,
                          "export workspace failed");
}

}   //namespace
}   //namespace mjt::mcp
