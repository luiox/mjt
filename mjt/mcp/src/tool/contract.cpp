#include "contract.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "limits.hpp"

namespace mjt::mcp::detail {

bool validateJsonBounds(const Json& value, const std::string& path, ca::usize depth,
                        std::string& error)
{
    //DoS 上限常量已上移至 limits.hpp，便于测试引用（M5）。
    if (depth > kMaximumJsonDepth) {
        error = path + " exceeds maximum JSON depth 32";
        return false;
    }
    if (value.is_array() && value.size() > kMaximumJsonArrayItems) {
        error = path + " exceeds maximum array length 4096";
        return false;
    }
    if (value.is_object() && value.size() > kMaximumJsonObjectProperties) {
        error = path + " exceeds maximum object property count 256";
        return false;
    }
    if (value.is_string() && value.get_ref<const std::string&>().size() > kMaximumJsonStringBytes) {
        error = path + " exceeds maximum string length";
        return false;
    }
    if (value.is_array()) {
        for (ca::usize index = 0; index < value.size(); ++index) {
            if (!validateJsonBounds(value[index],
                                    path + "[" + std::to_string(index) + "]",
                                    depth + 1,
                                    error))
                return false;
        }
    }
    if (value.is_object()) {
        for (const auto& [name, member] : value.items()) {
            if (!validateJsonBounds(member, path + "." + name, depth + 1, error))
                return false;
        }
    }
    return true;
}

bool validateSchemaValue(const Json& value, const Json& schema, const std::string& path,
                         ca::usize depth, std::string& error)
{
    if (schema.contains("type")) {
        const auto type = schema.at("type").get<std::string>();
        const bool matches = type == "object"  ? value.is_object()
                             : type == "array" ? value.is_array()
                             : type == "string" ? value.is_string()
                             : type == "integer" ? value.is_number_integer() || value.is_number_unsigned()
                             : type == "number" ? value.is_number()
                             : type == "boolean" ? value.is_boolean()
                             : false;
        if (!matches) {
            const auto article = type == "array" || type == "integer" || type == "object" ? "an " : "a ";
            error = path + " must be " + article + type;
            return false;
        }
    }
    if (schema.contains("enum")) {
        const auto found = std::find(schema.at("enum").begin(), schema.at("enum").end(), value);
        if (found == schema.at("enum").end()) {
            error = path + " is not one of the allowed values";
            return false;
        }
    }
    if (value.is_number()) {
        const long double number = value.is_number_unsigned()
                                       ? static_cast<long double>(value.get<ca::u64>())
                                   : value.is_number_integer()
                                       ? static_cast<long double>(value.get<ca::i64>())
                                       : static_cast<long double>(value.get<double>());
        if (schema.contains("minimum") && number < schema.at("minimum").get<long double>()) {
            error = path + " is below its minimum";
            return false;
        }
        if (schema.contains("maximum") && number > schema.at("maximum").get<long double>()) {
            error = path + " exceeds its maximum";
            return false;
        }
    }
    if (value.is_string()) {
        const auto length = value.get_ref<const std::string&>().size();
        if (schema.contains("minLength") && length < schema.at("minLength").get<ca::usize>()) {
            error = path + " is shorter than minLength";
            return false;
        }
        if (schema.contains("maxLength") && length > schema.at("maxLength").get<ca::usize>()) {
            error = path + " exceeds maxLength";
            return false;
        }
    }
    if (value.is_array()) {
        if (schema.contains("minItems") && value.size() < schema.at("minItems").get<ca::usize>()) {
            error = path + " has fewer than minItems";
            return false;
        }
        if (schema.contains("maxItems") && value.size() > schema.at("maxItems").get<ca::usize>()) {
            error = path + " exceeds maxItems";
            return false;
        }
        if (schema.value("uniqueItems", false)) {
            std::unordered_set<std::string> seen;
            for (const auto& item : value) {
                if (!seen.insert(item.dump()).second) {
                    error = path + " must contain unique items";
                    return false;
                }
            }
        }
        if (schema.contains("items")) {
            for (ca::usize index = 0; index < value.size(); ++index) {
                if (!validateSchemaValue(value[index],
                                         schema.at("items"),
                                         path + "[" + std::to_string(index) + "]",
                                         depth + 1,
                                         error))
                    return false;
            }
        }
    }
    if (value.is_object()) {
        if (schema.contains("required")) {
            for (const auto& required : schema.at("required")) {
                const auto name = required.get<std::string>();
                if (!value.contains(name)) {
                    error = path + "." + name + " is required";
                    return false;
                }
            }
        }
        const Json emptyProperties = Json::object();
        const Json& properties = schema.contains("properties") ? schema.at("properties")
                                                               : emptyProperties;
        for (const auto& [name, member] : value.items()) {
            if (properties.contains(name)) {
                if (!validateSchemaValue(member,
                                         properties.at(name),
                                         path + "." + name,
                                         depth + 1,
                                         error))
                    return false;
                continue;
            }
            if (!schema.contains("additionalProperties")) continue;
            const auto& additional = schema.at("additionalProperties");
            if (additional.is_boolean() && !additional.get<bool>()) {
                error = path + "." + name + " is not an allowed property";
                return false;
            }
            if (additional.is_object() &&
                !validateSchemaValue(member, additional, path + "." + name, depth + 1, error))
                return false;
        }
    }
    return true;
}

Json toolsList()
{
    const auto tool = [](const char* name, const char* description, Json properties,
                         std::vector<std::string> required = {}) {
        Json schema = {{"type", "object"},
                       {"properties", std::move(properties)},
                       {"additionalProperties", false}};
        if (!required.empty()) schema["required"] = std::move(required);
        return Json{{"name", name}, {"description", description}, {"inputSchema", std::move(schema)}};
    };
    const Json pagination = {
        {"cursor",
         {{"type", "string"},
          {"description",
           "Opaque decimal cursor returned by the preceding page; mutually exclusive with "
           "offset."}}},
        {"offset",
         {{"type", "integer"},
          {"minimum", 0},
          {"description", "Zero-based result offset; mutually exclusive with cursor."}}},
        {"limit",
         {{"type", "integer"},
          {"minimum", 1},
          {"maximum", kMaximumPageSize},
          {"description", "Maximum number of items to return in this page."}}}};
    const Json classSelector   = {{"type", "string"},
                                  {"minLength", 1},
                                  {"description",
                                   "JVM internal name such as java/lang/String, or an exact class "
                                     "resource URI such as java/lang/String.class."}};
    const Json mutationOptions = {{"expected_revision", {{"type", "integer"}, {"minimum", 0}}},
                                  {"dry_run", {{"type", "boolean"}}},
                                  {"label", {{"type", "string"}}}};
    const Json method = {{"owner", {{"type", "string"}}},
                         {"name", {{"type", "string"}}},
                         {"descriptor", {{"type", "string"}}}};
    const Json handleSchema = {{"type", "object"},
                               {"properties",
                                {{"tag", {{"type", "integer"}, {"minimum", 1}, {"maximum", 9}}},
                                 {"owner", {{"type", "string"}, {"minLength", 1}}},
                                 {"name", {{"type", "string"}, {"minLength", 1}}},
                                 {"descriptor", {{"type", "string"}, {"minLength", 1}}},
                                 {"interface", {{"type", "boolean"}}}}},
                               {"required", {"tag", "owner", "name", "descriptor"}},
                               {"additionalProperties", false}};
    Json constantSchema;
    for (ca::i32 depth = static_cast<ca::i32>(kMaximumConstantDynamicDepth); depth >= 0; --depth) {
        Json bootstrapArguments = {{"type", "array"},
                                   {"maxItems", depth == static_cast<ca::i32>(kMaximumConstantDynamicDepth) ? 0 : 128}};
        if (depth < static_cast<ca::i32>(kMaximumConstantDynamicDepth))
            bootstrapArguments["items"] = constantSchema;
        constantSchema = {{"type", "object"},
                          {"properties",
                           {{"kind", {{"type", "string"},
                                       {"enum", {"string", "int", "long", "float", "double", "type", "handle", "dynamic"}}}},
                            {"string_value", {{"type", "string"}}},
                            {"int_value", {{"type", "integer"}}},
                            {"long_value", {{"type", "integer"}}},
                            {"float_value", {{"type", "number"}}},
                            {"double_value", {{"type", "number"}}},
                            {"descriptor", {{"type", "string"}}},
                            {"name", {{"type", "string"}}},
                            {"handle", handleSchema},
                            {"bootstrap_method", handleSchema},
                            {"bootstrap_arguments", std::move(bootstrapArguments)}}},
                          {"required", {"kind"}},
                          {"additionalProperties", false}};
    }
    const Json instructionSchema = {{"type", "object"},
                                    {"properties",
                                     {{"kind", {{"type", "string"},
                                                 {"enum", {"simple", "int", "var", "type", "field", "method", "jump", "label", "ldc_string", "ldc_int", "ldc_long", "ldc_float", "ldc_double", "ldc_type", "ldc_handle", "ldc_dynamic", "invokedynamic", "iinc", "tableswitch", "lookupswitch", "multianewarray", "line"}}}},
                                      {"opcode", {{"type", "integer"}, {"minimum", 0}, {"maximum", 199}}},
                                      {"operand", {{"type", "integer"}}},
                                      {"increment", {{"type", "integer"}}},
                                      {"interface", {{"type", "boolean"}}},
                                      {"owner", {{"type", "string"}}},
                                      {"name", {{"type", "string"}}},
                                      {"descriptor", {{"type", "string"}}},
                                      {"string_value", {{"type", "string"}}},
                                      {"long_value", {{"type", "integer"}}},
                                      {"float_value", {{"type", "number"}}},
                                      {"double_value", {{"type", "number"}}},
                                      {"label", {{"type", "string"}}},
                                      {"default_label", {{"type", "string"}}},
                                      {"labels", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                                      {"keys", {{"type", "array"}, {"items", {{"type", "integer"}}}}},
                                      {"minimum", {{"type", "integer"}}},
                                      {"maximum", {{"type", "integer"}}},
                                      {"dimensions", {{"type", "integer"}, {"minimum", 1}, {"maximum", 255}}},
                                      {"line", {{"type", "integer"}, {"minimum", 1}}},
                                      {"handle", handleSchema},
                                      {"bootstrap_method", handleSchema},
                                      {"bootstrap_arguments", {{"type", "array"},
                                                                {"maxItems", 128},
                                                                {"items", constantSchema}}}}},
                                    {"required", {"kind"}},
                                    {"additionalProperties", false}};
    const Json labelBindingSchema = {{"type", "object"},
                                     {"properties", {{"id", {{"type", "string"}, {"minLength", 1}}},
                                                     {"instruction_index", {{"type", "integer"}, {"minimum", 0}}}}},
                                     {"required", {"id", "instruction_index"}},
                                     {"additionalProperties", false}};
    const Json tryCatchSchema = {{"type", "object"},
                                 {"properties", {{"start_instruction", {{"type", "integer"}, {"minimum", 0}}},
                                                 {"end_instruction", {{"type", "integer"}, {"minimum", 0}}},
                                                 {"handler_instruction", {{"type", "integer"}, {"minimum", 0}}},
                                                 {"start_label", {{"type", "string"}}},
                                                 {"end_label", {{"type", "string"}}},
                                                 {"handler_label", {{"type", "string"}}},
                                                 {"type", {{"type", "string"}}}}},
                                 {"additionalProperties", false}};
    const Json localVariableSchema = {{"type", "object"},
                                      {"properties", {{"name", {{"type", "string"}, {"minLength", 1}}},
                                                      {"descriptor", {{"type", "string"}, {"minLength", 1}}},
                                                      {"signature", {{"type", "string"}}},
                                                      {"start_label", {{"type", "string"}, {"minLength", 1}}},
                                                      {"end_label", {{"type", "string"}, {"minLength", 1}}},
                                                      {"index", {{"type", "integer"}, {"minimum", 0}}}}},
                                      {"required", {"name", "descriptor", "start_label", "end_label", "index"}},
                                      {"additionalProperties", false}};
    const Json decompilerRuntime = {{"decompiler_jar", {{"type", "string"}}},
                                    {"java", {{"type", "string"}}},
                                    {"timeout_ms", {{"type", "integer"},
                                                    {"minimum", 1},
                                                    {"maximum", kMaximumDecompilerTimeoutMilliseconds}}},
                                    {"max_memory_mib", {{"type", "integer"},
                                                        {"minimum", 1},
                                                        {"maximum", kMaximumDecompilerMemoryMiB}}},
                                    {"max_source_bytes", {{"type", "integer"},
                                                          {"minimum", 1},
                                                          {"maximum", kMaximumDecompilerSourceBytes}}}};
    Json decompiler = decompilerRuntime;
    decompiler["decompiler"] = {{"type", "string"}, {"enum", {"cfr", "vineflower"}}};
    decompiler["async"] = {{"type", "boolean"}};
    const Json pipeline = {{"pipeline", {{"type", "string"}}},
                           {"passes", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                           {"jdk_path", {{"type", "string"}}}};
    const auto workspaceTool       = [&](const char*              name,
                                   const char*              description,
                                   Json                     properties,
                                   std::vector<std::string> required = {}) {
        properties["workspace_id"] = {{"type", "string"},
                                      {"minLength", 1},
                                      {"description", "Workspace ID returned by workspace_open."}};
        required.insert(required.begin(), "workspace_id");
        return tool(name, description, std::move(properties), std::move(required));
    };
    const auto with = [](Json first, const Json& second) {
        for (const auto& [key, value] : second.items()) first[key] = value;
        return first;
    };
    const Json workflowReference = {{"type", "object"},
                                    {"properties", {{"kind", {{"type", "string"}, {"enum", {"class", "field", "method"}}}},
                                                    {"owner", {{"type", "string"}}},
                                                    {"name", {{"type", "string"}}},
                                                    {"descriptor", {{"type", "string"}}}}},
                                    {"required", {"kind", "owner"}},
                                    {"additionalProperties", false}};
    Json workflow = with(
        with(mutationOptions, pipeline),
        {{"run_pipeline", {{"type", "boolean"}}},
         {"mapping_format", {{"type", "string"}}},
         {"mapping_text", {{"type", "string"}}},
         {"mapping_policy", {{"type", "string"},
                              {"enum", {"bytecode-only", "reflection", "metadata", "all"}}}},
         {"export_path", {{"type", "string"}}},
         {"references", {{"type", "array"}, {"items", workflowReference}}}});
    workflow["decompile_class"]   = classSelector;
    workflow["decompile_classes"] = {{"type", "array"},
                                     {"minItems", 1},
                                     {"maxItems", kMaximumWorkflowDecompileClasses},
                                     {"items", {{"type", "string"}}}};
    workflow["decompiler"] = {{"type", "string"}, {"enum", {"cfr", "vineflower"}}};
    workflow["async"] = {{"type", "boolean"}};
    for (const auto& [key, value] : decompilerRuntime.items()) workflow[key] = value;
    Json result = Json::array();
    result.push_back(
        tool("decompiler_list",
             "List decompiler backends for this MCP session; no workspace_id is required.",
             Json::object()));
    result.push_back(tool("decompiler_settings_get",
                          "Read defaults scoped to this MCP session; no workspace_id is required.",
                          Json::object()));
    result.push_back(tool(
        "decompiler_settings_set",
        "Atomically update defaults scoped to this MCP session; no workspace_id is required.",
        {{"backend", {{"type", "string"}, {"enum", {"cfr", "vineflower"}}}},
         {"cfr_jar", {{"type", "string"}}},
         {"vineflower_jar", {{"type", "string"}}},
         {"java", {{"type", "string"}}},
         {"timeout_ms",
          {{"type", "integer"},
           {"minimum", 1},
           {"maximum", kMaximumDecompilerTimeoutMilliseconds}}},
         {"max_memory_mib",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaximumDecompilerMemoryMiB}}},
         {"max_source_bytes",
          {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaximumDecompilerSourceBytes}}}}));
    result.push_back(tool("pass_describe", "List registered bytecode passes and pipelines.",
                          {{"name", {{"type", "string"}}},
                           {"kind", {{"type", "string"}, {"enum", {"pass", "pipeline"}}}}}));
    result.push_back(tool("analysis_evaluate", "Evaluate or MBA-simplify a bounded integer expression. "
                          "Experimental: not part of the v2.1 default capability set.",
                          {{"expression", {{"type", "string"}}},
                           {"bindings", {{"type", "object"},
                                         {"additionalProperties", {{"type", "integer"}}}}},
                           {"simplify", {{"type", "boolean"}}},
                           {"max_depth", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}}}},
                          {"expression"}));
    result.push_back(tool("job_get", "Read the state and completed result of a background decompiler job.",
                          {{"job_id", {{"type", "string"}}}}, {"job_id"}));
    result.push_back(tool("job_cancel", "Cancel a queued or running background decompiler job.",
                          {{"job_id", {{"type", "string"}}}}, {"job_id"}));
    result.push_back(tool("job_release", "Release a completed background job and its retained result.",
                          {{"job_id", {{"type", "string"}}}}, {"job_id"}));
    result.push_back(workspaceTool(
        "analysis_workflow",
        "Preview or atomically run mapping, pipeline, validation, exact xref, candidate decompile and export stages.",
        workflow));
    result.push_back(tool(
        "workspace_open",
        "Open a JAR in an isolated native workspace.",
        {{"path",
          {{"type", "string"}, {"description", "Filesystem path of the primary JAR to open."}}},
         {"workspace_id",
          {{"type", "string"},
           {"description",
            "Optional caller-selected workspace ID; otherwise the server generates one."}}}},
        {"path"}));
    result.push_back(workspaceTool("workspace_close", "Close an isolated workspace.", Json::object()));
    result.push_back(workspaceTool("workspace_info", "Return workspace revision, dirty state and support resources.", Json::object()));
    result.push_back(workspaceTool("workspace_modified_classes",
                                   "List classes changed from the workspace's original JAR snapshot.",
                                   pagination));
    result.push_back(workspaceTool("workspace_add_support", "Attach a read-only supporting JAR.",
                                   {{"path", {{"type", "string"}}}}, {"path"}));
    result.push_back(workspaceTool("workspace_list_support", "List attached read-only supporting JARs.",
                                   pagination));
    result.push_back(workspaceTool("workspace_remove_support", "Detach one supporting JAR by stable ID.",
                                   {{"support_id", {{"type", "string"}}}}, {"support_id"}));
    result.push_back(workspaceTool("workspace_resources", "List primary resources with pagination.",
                                   with(pagination, {{"include_classes", {{"type", "boolean"}}}})));
    result.push_back(workspaceTool("resource_read", "Read a primary resource as bounded base64 bytes.",
                                   with(pagination,
                                        {{"uri", {{"type", "string"}}},
                                         {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaximumResourceChunkSize}}}}),
                                   {"uri"}));
    result.push_back(workspaceTool("resource_manifest", "Read parsed MANIFEST main attributes.", Json::object()));
    result.push_back(workspaceTool("resource_strings", "Extract printable strings from non-class resources.",
                                   with(pagination, {{"minimum_length", {{"type", "integer"}, {"minimum", 1}}}})));
    const Json search = {
        {"query",
         {{"type", "string"},
          {"minLength", 1},
          {"maxLength", 1024},
          {"description",
           "Text or ECMAScript regular expression to match, according to match_mode."}}},
        {"match_mode", {{"type", "string"}, {"enum", {"contains", "exact", "regex"}}}},
        {"case_sensitive", {{"type", "boolean"}}},
        {"owner", {{"type", "string"}}},
        {"package_prefix", {{"type", "string"}}}};
    result.push_back(workspaceTool("search_classes", "Search class names and resource URIs with stable pagination.",
                                   with(pagination, search), {"query"}));
    result.push_back(workspaceTool("search_members", "Search field and method owner/name/descriptor metadata.",
                                   with(with(pagination, search),
                                        {{"member_kind", {{"type", "string"},
                                                          {"enum", {"field", "method"}}}}}),
                                   {"query"}));
    auto stringSearch = with(pagination, search);
    stringSearch["query"]["description"] =
        "String text to find in class constant pools and printable non-class resources; "
        "interpreted according to match_mode.";
    result.push_back(workspaceTool(
        "search_strings",
        "Search class constant-pool and printable non-class resource strings using query.",
        std::move(stringSearch),
        {"query"}));
    result.push_back(workspaceTool("search_numbers", "Search constant-pool and instruction numeric literals.",
                                   with(with(pagination,
                                             {{"query", {{"type", "string"},
                                                          {"minLength", 1},
                                                          {"maxLength", 1024}}},
                                              {"owner", {{"type", "string"}}},
                                              {"package_prefix", {{"type", "string"}}}}),
                                        {{"opcode", {{"type", "integer"},
                                                      {"minimum", 0},
                                                      {"maximum", 199}}}}),
                                   {"query"}));
    result.push_back(workspaceTool("search_instructions", "Search instruction mnemonic, opcode, type and operands.",
                                   with(with(pagination, search),
                                        {{"opcode", {{"type", "integer"},
                                                      {"minimum", 0},
                                                      {"maximum", 199}}}}),
                                   {"query"}));
    result.push_back(workspaceTool("class_list", "List parsed primary classes.", pagination));
    result.push_back(workspaceTool("class_get",
                                   "Get exact class, fields, methods and annotations.",
                                   {{"class", classSelector}},
                                   {"class"}));
    result.push_back(workspaceTool(
        "class_bytes",
        "Read bounded raw class bytes and a content fingerprint.",
        with(pagination,
             {{"class", classSelector},
              {"limit",
               {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaximumResourceChunkSize}}}}),
        {"class"}));
    result.push_back(workspaceTool("class_jasm",
                                   "Get normalized complete-class Jasm.",
                                   {{"class", classSelector}},
                                   {"class"}));
    result.push_back(workspaceTool(
        "method_get", "Get exact method metadata.", method, {"owner", "name", "descriptor"}));
    result.push_back(workspaceTool("method_bytecode", "Get exact method instruction summaries with normalized Jasm source.", method,
                                   {"owner", "name", "descriptor"}));
    result.push_back(workspaceTool("method_jasm", "Get one exact method as normalized Jasm.", method,
                                   {"owner", "name", "descriptor"}));
    //shadow 双向查询：显示名 ↔ 真名；sidecar 缺省按 <jar>.mjt-shadow
    //解析，`file` 显式覆盖。机械占位名跨类重号，命中一律返回列表。
    const Json shadowSelector = {
        {"name",
         {{"type", "string"},
          {"minLength", 1},
          {"description",
           "Symbol name: a shadow display name (shadow_to_raw) or a raw name "
           "(raw_to_shadow)."}}},
        {"file",
         {{"type", "string"},
          {"description",
           "Optional shadow sidecar path; defaults to `<jar>.mjt-shadow` next to the "
           "workspace source."}}},
        {"owner", {{"type", "string"}, {"description", "Optional owner internal-name filter."}}},
        {"kind",
         {{"type", "string"},
          {"enum", {"class", "field", "method"}},
          {"description", "Optional symbol kind filter."}}}};
    result.push_back(workspaceTool("shadow_to_raw",
                                   "Resolve raw symbol names behind a shadow display name.",
                                   shadowSelector,
                                   {"name"}));
    result.push_back(workspaceTool("raw_to_shadow",
                                   "Find shadow display names for a raw symbol name.",
                                   shadowSelector,
                                   {"name"}));
    //shadow sidecar 管理：生成 = 批量机械占位名（幂等合并）+ 写 sidecar；
    //prune = 清理指向容器中不存在符号的过期条目。只写 sidecar 文件，永不改 jar。
    const Json shadowTarget = {
        {"file",
         {{"type", "string"},
          {"minLength", 1},
          {"description",
           "Optional shadow sidecar path; defaults to `<jar>.mjt-shadow` next to the "
           "workspace source. Must not be the JAR itself."}}}};
    result.push_back(workspaceTool("shadow_generate",
                                   "Generate mechanical shadow display names for the workspace "
                                   "JAR and write the shadow sidecar (idempotent merge).",
                                   shadowTarget,
                                   {}));
    result.push_back(workspaceTool("shadow_prune",
                                   "Drop shadow alias entries whose symbols are absent from "
                                   "the workspace JAR.",
                                   shadowTarget,
                                   {}));
    result.push_back(workspaceTool("class_source",
                                   "Decompile a class from the current workspace revision.",
                                   with(decompiler, {{"class", classSelector}}),
                                   {"class"}));
    result.push_back(workspaceTool("method_source",
                                   "Decompile the class that contains one exact method.",
                                   with(decompiler, method),
                                   {"owner", "name", "descriptor"}));
    result.push_back(workspaceTool("decompiler_decompile",
                                   "Decompile a class from the current workspace revision.",
                                   with(decompiler, {{"class", classSelector}}),
                                   {"class"}));
    result.push_back(
        workspaceTool("decompiler_compare",
                      "Decompile a class with CFR and Vineflower for source comparison.",
                      with(decompilerRuntime, {{"class", classSelector}}),
                      {"class"}));
    result.push_back(
        workspaceTool("pass_run",
                      "Run registered passes or a pipeline through a workspace transaction.",
                      with(mutationOptions, pipeline)));
    result.push_back(workspaceTool("opt_preview",
                                   "Preview default or named optimization pipeline without committing.",
                                   with(mutationOptions, pipeline)));
    result.push_back(workspaceTool("opt_apply",
                                   "Atomically apply default or named optimization pipeline.",
                                   with(mutationOptions, pipeline)));
    result.push_back(workspaceTool("jar_diff",
                                   "Compare all current workspace resources with an external JAR.",
                                   with(pagination, {{"path", {{"type", "string"}}}}),
                                   {"path"}));
    const Json jarRepair = with(
        mutationOptions,
        {{"remove_invalid_class_entries", {{"type", "boolean"}}},
         {"remove_corrupt_output_markers", {{"type", "boolean"}}}});
    result.push_back(workspaceTool("jar_fix_analyze",
                                   "Scan and preview selected JAR repair removals without committing.",
                                   jarRepair));
    result.push_back(workspaceTool("jar_fix_apply",
                                   "Atomically apply explicitly selected JAR repair removals.",
                                   jarRepair));
    result.push_back(workspaceTool("xref_method", "Find exact owner/name/descriptor method references.",
                                   with(method, pagination), {"owner", "name", "descriptor"}));
    result.push_back(workspaceTool("method_callers", "List caller sites for an exact method.",
                                   with(method, pagination), {"owner", "name", "descriptor"}));
    result.push_back(workspaceTool("xref_field", "Find exact owner/name/descriptor field references.",
                                   with(method, pagination), {"owner", "name", "descriptor"}));
    result.push_back(workspaceTool("xref_class",
                                   "Find direct and metadata class references.",
                                   with(pagination, {{"class", classSelector}}),
                                   {"class"}));
    result.push_back(
        workspaceTool("method_callees",
                      "Get direct or resolved method call edges.",
                      with(with(method, pagination), {{"resolved", {{"type", "boolean"}}}}),
                      {"owner", "name", "descriptor"}));
    result.push_back(workspaceTool("class_relations",
                                   "Get hierarchy, subclasses and implementors.",
                                   {{"class", classSelector}},
                                   {"class"}));
    result.push_back(workspaceTool("method_overrides",
                                   "Get an override component before method rename.",
                                   method,
                                   {"owner", "name", "descriptor"}));
    const Json jasmPatch = with(mutationOptions,
                                {{"uri", {{"type", "string"}}},
                                 {"source_name", {{"type", "string"}}},
                                 {"source", {{"type", "string"}}}});
    result.push_back(workspaceTool("patch_validate_jasm", "Preview complete-class Jasm without committing.",
                                   jasmPatch, {"uri", "source"}));
    result.push_back(workspaceTool("patch_apply_jasm", "Atomically apply complete-class Jasm with revision control.",
                                   jasmPatch, {"uri", "source"}));
    const Json jasmDiff = with(mutationOptions,
                               {{"uri", {{"type", "string"}}},
                                {"source_name", {{"type", "string"}}},
                                {"diff", {{"type", "string"},
                                          {"description",
                                           "Unified diff against the current canonical class Jasm "
                                           "(as returned by class_jasm). Context lines must match "
                                           "exactly; no fuzz or 3way merge."}}}});
    result.push_back(workspaceTool(
        "patch_validate_jasm_diff",
        "Preview applying a unified diff against the current class Jasm without committing.",
        jasmDiff, {"uri", "diff"}));
    result.push_back(workspaceTool(
        "patch_apply_jasm_diff",
        "Atomically apply a unified diff against the current class Jasm with revision control.",
        jasmDiff, {"uri", "diff"}));
    const Json methodJasm = with(
        mutationOptions,
        {{"owner", {{"type", "string"}, {"minLength", 1}}},
         {"name", {{"type", "string"}, {"minLength", 1}}},
         {"descriptor", {{"type", "string"}, {"minLength", 1}}},
         {"source_name", {{"type", "string"}}},
         {"source", {{"type", "string"},
                     {"description",
                      "Complete replacement for the single .method Jasm declaration as returned "
                      "by method_jasm. Other members are kept byte-for-byte."}}}});
    result.push_back(workspaceTool(
        "method_validate_jasm",
        "Preview replacing one exact method's Jasm in its class without committing.",
        methodJasm, {"owner", "name", "descriptor", "source"}));
    result.push_back(workspaceTool(
        "method_apply_jasm",
        "Atomically replace one exact method's Jasm with revision control.",
        methodJasm, {"owner", "name", "descriptor", "source"}));
    const Json methodJasmDiff = with(
        mutationOptions,
        {{"owner", {{"type", "string"}, {"minLength", 1}}},
         {"name", {{"type", "string"}, {"minLength", 1}}},
         {"descriptor", {{"type", "string"}, {"minLength", 1}}},
         {"source_name", {{"type", "string"}}},
         {"diff", {{"type", "string"},
                   {"description",
                    "Unified diff against the single .method Jasm returned by method_jasm. "
                    "Context lines must match exactly; no fuzz or 3way merge."}}}});
    result.push_back(workspaceTool(
        "method_validate_jasm_diff",
        "Preview applying a unified diff to one exact method's Jasm without committing.",
        methodJasmDiff, {"owner", "name", "descriptor", "diff"}));
    result.push_back(workspaceTool(
        "method_apply_jasm_diff",
        "Atomically apply a unified diff to one exact method's Jasm with revision control.",
        methodJasmDiff, {"owner", "name", "descriptor", "diff"}));
    const Json classBytesPatch = with(
        mutationOptions,
        {{"class_uri", {{"type", "string"}, {"minLength", 1}}},
         {"content_base64", {{"type", "string"},
                              {"minLength", 4},
                              {"maxLength", ((mjt::base::workspace::kMaximumClassReplacementSize + 2) / 3) * 4}}}});
    result.push_back(workspaceTool("patch_replace_class_bytes",
                                   "Validate and atomically replace one class from strict base64 bytes.",
                                   classBytesPatch,
                                   {"class_uri", "content_base64"}));
    const Json structuredPatch = with(
        mutationOptions,
        {{"class_uri", {{"type", "string"}}},
         {"kind", {{"type", "string"},
                   {"enum", {"set_class_access", "set_field_access", "set_method_access", "add_field", "remove_field", "add_method", "remove_method", "add_marker_annotation", "remove_annotation", "rename_local", "replace_try_catch_blocks", "replace_method_body", "insert_instruction", "replace_instruction", "remove_instruction"}}}},
         {"member_name", {{"type", "string"}}},
         {"member_descriptor", {{"type", "string"}}},
         {"access", {{"type", "integer"}}},
         {"access_flags", {{"type", "array"},
                            {"uniqueItems", true},
                            {"items", {{"type", "string"},
                                       {"enum", {"public", "private", "protected", "static", "final", "synthetic", "deprecated", "super", "interface", "abstract", "annotation", "enum", "module", "record", "sealed", "volatile", "transient", "synchronized", "bridge", "varargs", "native", "strict"}}}}}},
         {"signature", {{"type", "string"}}},
         {"exceptions", {{"type", "array"}, {"items", {{"type", "string"}}}}},
         {"field_value", constantSchema},
         {"instruction_index", {{"type", "integer"}}},
         {"insert_after", {{"type", "boolean"}}},
         {"local_index", {{"type", "integer"}}},
         {"local_name", {{"type", "string"}}},
         {"replacement_local_name", {{"type", "string"}}},
         {"annotation_target", {{"type", "string"},
                                {"enum", {"class", "field", "method"}}}},
         {"annotation_visible", {{"type", "boolean"}}},
         {"annotation_descriptor", {{"type", "string"}}},
         {"instruction", instructionSchema},
         {"existing_labels", {{"type", "array"}, {"items", labelBindingSchema}}},
         {"initial_instructions", {{"type", "array"}, {"items", instructionSchema}}},
         {"try_catch_blocks", {{"type", "array"}, {"items", tryCatchSchema}}},
         {"local_variables", {{"type", "array"}, {"items", localVariableSchema}}}});
    result.push_back(workspaceTool("patch_apply", "Preview or apply a structured ClassMutation.",
                                   structuredPatch, {"class_uri", "kind"}));
    const Json rename = with(mutationOptions,
                             {{"kind", {{"type", "string"}, {"enum", {"class", "package", "field", "method", "local"}}}},
                              {"owner", {{"type", "string"}}},
                              {"method_name", {{"type", "string"}}},
                              {"name", {{"type", "string"}}},
                              {"descriptor", {{"type", "string"}}},
                              {"new_name", {{"type", "string"}}},
                              {"local_index", {{"type", "integer"}}},
                              {"cascade_overrides", {{"type", "boolean"}}},
                              {"policy", {{"type", "string"}, {"enum", {"bytecode", "reflection", "metadata", "all"}}}}});
    result.push_back(workspaceTool("rename_analyze", "Return rename mappings, impacts and override risks.",
                                   rename, {"kind", "owner", "new_name"}));
    result.push_back(workspaceTool("rename_apply", "Apply an approved workspace rename with revision control.",
                                   rename, {"kind", "owner", "new_name"}));
    const Json mapping = with(
        mutationOptions,
        {{"format", {{"type", "string"}}},
         {"mapping_text", {{"type", "string"}}},
         {"policy", {{"type", "string"},
                     {"enum", {"bytecode-only", "reflection", "metadata", "all"}}}}});
    result.push_back(workspaceTool("mapping_analyze",
                                   "Preview a parsed mapping against the current workspace without committing.",
                                   mapping, {"format", "mapping_text"}));
    result.push_back(workspaceTool("mapping_apply",
                                   "Atomically apply parsed mappings with revision control and dry-run support.",
                                   mapping, {"format", "mapping_text"}));
    result.push_back(workspaceTool("workspace_savepoint", "Create a named workspace snapshot.",
                                   {{"name", {{"type", "string"}}}}, {"name"}));
    result.push_back(workspaceTool("workspace_restore", "Restore a named snapshot as a new revision.",
                                   with(mutationOptions, {{"name", {{"type", "string"}}}}), {"name"}));
    result.push_back(workspaceTool("workspace_revert_class",
                                   "Restore one class from the original JAR snapshot.",
                                   with(mutationOptions,
                                        {{"class_uri", {{"type", "string"}, {"minLength", 1}}}}),
                                   {"class_uri"}));
    result.push_back(workspaceTool("workspace_undo", "Undo the last committed workspace transaction.", mutationOptions));
    result.push_back(workspaceTool("workspace_redo", "Redo the last undone workspace transaction.", mutationOptions));
    result.push_back(workspaceTool("workspace_export", "Atomically export a workspace to a new JAR path.",
                                   {{"path", {{"type", "string"}}}}, {"path"}));
    return result;
}

namespace {

///标识符语义参数名集合（；R42 P3 收口扩充）。覆盖「JVM 常量池 UTF8」
///语义的键：类/成员选择器（class/owner/name/method_name/member_name/new_name/
///decompile_class/package_prefix）、资源 URI（uri/class_uri）、方法/字段描述符
///（descriptor/member_descriptor/annotation_descriptor）、泛型签名（signature）
///与 SourceFile 名（source_name）。path（文件系统路径）、query、mapping_text、
///source/diff 等自由文本参数不入集。
const std::unordered_set<std::string>& identifierSelectorKeys()
{
    static const std::unordered_set<std::string> keys = {"class",
                                                         "class_uri",
                                                         "uri",
                                                         "owner",
                                                         "name",
                                                         "method_name",
                                                         "member_name",
                                                         "new_name",
                                                         "decompile_class",
                                                         "package_prefix",
                                                         "descriptor",
                                                         "member_descriptor",
                                                         "annotation_descriptor",
                                                         "signature",
                                                         "source_name"};
    return keys;
}

///单个选择器的长度校验：超限产出稳定文案（含参数路径与实际长度）。
bool checkIdentifierLength(const std::string& path, const std::string& value, std::string& error)
{
    if (value.size() <= kMaximumIdentifierBytes) return true;
    error = path + " identifier length " + std::to_string(value.size()) +
            " exceeds classfile limit " + std::to_string(kMaximumIdentifierBytes);
    return false;
}

///字符串数组的逐元素校验（decompile_classes 与 R42 P3 收口的 exceptions[]）。
bool checkStringArrayLengths(const std::string& key, const Json& value, std::string& error)
{
    for (ca::usize index = 0; index < value.size(); ++index) {
        const auto& item = value[index];
        if (!item.is_string()) continue;
        if (!checkIdentifierLength("$." + key + "[" + std::to_string(index) + "]",
                                   item.get_ref<const std::string&>(),
                                   error))
            return false;
    }
    return true;
}

///对象数组内指定字符串成员的校验（：references[].owner；R42 P3 收口：
///try_catch_blocks[].type 与 local_variables[].name/descriptor/signature）。
///members 用定序 vector，保证同一元素多个超长成员时的报错顺序确定。
bool checkMemberArrayLengths(const std::string& key, const Json& value,
                             const std::vector<std::string>& members, std::string& error)
{
    for (ca::usize index = 0; index < value.size(); ++index) {
        const auto& item = value[index];
        if (!item.is_object()) continue;
        for (const auto& member : members) {
            if (!item.contains(member)) continue;
            const auto& memberValue = item.at(member);
            if (!memberValue.is_string()) continue;
            if (!checkIdentifierLength("$." + key + "[" + std::to_string(index) + "]." + member,
                                       memberValue.get_ref<const std::string&>(),
                                       error))
                return false;
        }
    }
    return true;
}

///「name」等键在这些工具里是标签/pass 名等自由文本，不是 JVM 标识符，
///不适用 classfile 长度守卫。豁免是整工具粒度：这些工具的 schema 日后新增
///标识符语义键（descriptor/signature/source_name/exceptions 等）时必须迁出
///豁免表，否则会绕过 65535 守卫（R42 处方 P3）。
const std::unordered_set<std::string>& identifierGuardExemptTools()
{
    static const std::unordered_set<std::string> tools = {"workspace_savepoint",
                                                          "workspace_restore",
                                                          "pass_describe",
                                                          "pass_run"};
    return tools;
}

///工具边界统一校验（第二条；R42 P3 收口扩充键集）：对 class/owner/URI
///选择器与 descriptor/signature/source_name 等常量池 UTF8 语义参数强制
///classfile 规格对齐的长度上限（65535 字节），并覆盖 exceptions[]、
///try_catch_blocks[].type、local_variables[].name/descriptor/signature 数组
///白名单。合法长名（819/4096 等）不受影响；超限返回结构化错误，杜绝静默
///挂起/静默截断。指令树（instruction/initial_instructions/handle/field_value）
///内的同名嵌套键仍在口径外。
bool validateIdentifierBounds(const std::string& name, const Json& arguments, std::string& error)
{
    if (identifierGuardExemptTools().count(name) != 0) return true;
    for (const auto& [key, value] : arguments.items()) {
        if (value.is_string() && identifierSelectorKeys().count(key) != 0) {
            if (!checkIdentifierLength("$." + key, value.get_ref<const std::string&>(), error))
                return false;
            continue;
        }
        //数组白名单：字符串数组逐元素；对象数组仅校验白名单成员
        //（R42 P3 收口：exceptions[]、try_catch_blocks[].type、
        //local_variables[].name/descriptor/signature 并入）。
        if (!value.is_array()) continue;
        if (key == "decompile_classes" || key == "exceptions") {
            if (!checkStringArrayLengths(key, value, error)) return false;
        } else if (key == "references") {
            if (!checkMemberArrayLengths(key, value, {"owner"}, error)) return false;
        } else if (key == "try_catch_blocks") {
            if (!checkMemberArrayLengths(key, value, {"type"}, error)) return false;
        } else if (key == "local_variables") {
            if (!checkMemberArrayLengths(key, value, {"name", "descriptor", "signature"}, error))
                return false;
        }
    }
    return true;
}

}   //namespace

bool validateToolArguments(const std::string& name, const Json& arguments, std::string& error)
{
    if (!validateJsonBounds(arguments, "$", 0, error)) return false;
    static const Json definitions = toolsList();
    const auto found = std::find_if(definitions.begin(), definitions.end(), [&](const Json& tool) {
        return tool.at("name") == name;
    });
    if (found == definitions.end()) return true;
    const auto& schema = found->at("inputSchema");
    if (!validateSchemaValue(arguments, schema, "$", 0, error)) return false;
    if (!validateIdentifierBounds(name, arguments, error)) return false;
    if (!schema.at("properties").contains("cursor")) return true;
    if (arguments.contains("cursor") && arguments.contains("offset")) {
        error = "$.cursor and $.offset are mutually exclusive";
        return false;
    }
    if (!arguments.contains("cursor")) return true;
    const auto& cursor = arguments.at("cursor").get_ref<const std::string&>();
    ca::usize offset = 0;
    const auto parsed = std::from_chars(cursor.data(), cursor.data() + cursor.size(), offset);
    if (cursor.empty() || parsed.ec != std::errc() || parsed.ptr != cursor.data() + cursor.size()) {
        error = "$.cursor must be a non-empty unsigned decimal offset";
        return false;
    }
    return true;
}

}   //namespace mjt::mcp::detail
