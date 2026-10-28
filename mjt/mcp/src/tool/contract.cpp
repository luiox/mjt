#include "contract.hpp"

#include <algorithm>
#include <charconv>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "json.hpp"
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
          {"description", "Maximum number of items to return in this page."}}},
        {"page_size",
         {{"type", "integer"},
          {"minimum", 1},
          {"maximum", kMaximumPageSize},
          {"description",
           "Items per page; defaults to 50. Mutually exclusive with limit."}}},
        {"fields",
         {{"type", "array"},
          {"items", {{"type", "string"}}},
          {"uniqueItems", true},
          {"description",
           "Projection: return only these keys on each item; omitted returns full items. "
           "Unknown names are rejected listing the static key set of this result kind "
           "(independent of the result set, so legal keys stay valid on empty results). "
           "Outer keys (total/offset/partial sidecar) are unaffected."}}}};
    const Json classSelector   = {{"type", "string"},
                                  {"minLength", 1},
                                  {"description",
                                   "JVM internal name such as java/lang/String, or an exact class "
                                     "resource URI such as java/lang/String.class."}};
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
    const auto projectTool = [&](const char*              name,
                                 const char*              description,
                                 Json                     properties,
                                 std::vector<std::string> required = {}) {
        properties["project_id"] = {{"type", "string"},
                                    {"minLength", 1},
                                    {"description", "Project ID returned by project_open."}};
        required.insert(required.begin(), "project_id");
        return tool(name, description, std::move(properties), std::move(required));
    };
    const auto with = [](Json first, const Json& second) {
        for (const auto& [key, value] : second.items()) first[key] = value;
        return first;
    };
    const Json mutationOptions = {{"expected_revision", {{"type", "integer"}, {"minimum", 0}}},
                                  {"dry_run", {{"type", "boolean"}}},
                                  {"label", {{"type", "string"}}}};
    //工具级超时面（#25）：四个长工具的可选执行上限，缺省 180000（3 分钟）。
    //超时返回结构化错误（reason=timeout / elapsed_ms），服务端中止等待，通道与
    //已打开会话保持可用；上限与 decompile 的 timeout_ms 对齐（300000）。
    const Json toolTimeout = {{"timeout_ms",
                               {{"type", "integer"},
                                {"minimum", 1},
                                {"maximum", kMaximumToolTimeoutMilliseconds},
                                {"description",
                                 "Optional execution deadline in milliseconds; the call "
                                 "returns a structured timeout error when exceeded. Default "
                                 "180000 (3 minutes)."}}}};
    Json result = Json::array();
    //工程会话面（RFC 0001 §8.1）：11 个粗粒度工具，同版本切换取代全部旧名。
    result.push_back(tool(
        "project_open",
        "Open a JAR in a persistent analysis project; creates or reopens the project bundle "
        "and takes its single-writer store lock.",
        with({{"path",
               {{"type", "string"}, {"description", "Filesystem path of the primary JAR to open."}}},
              {"project_dir",
               {{"type", "string"},
                {"description",
                 "Optional project bundle directory; omitted opens a temporary project whose "
                 "bundle is deleted on project_close. Reopening the same project_dir resumes "
                 "its persisted state."}}},
              {"project_id",
               {{"type", "string"},
                {"description",
                 "Optional caller-selected project ID; otherwise the server generates one."}}}},
             toolTimeout),
        {"path"}));
    result.push_back(projectTool("project_close",
                                 "Flush pending ledger transactions and close the project "
                                 "session, releasing its store lock.",
                                 Json::object()));
    result.push_back(projectTool(
        "project_info",
        "Project overview: containers with roles and fingerprints, revision, ledger depth and "
        "compaction health, available decompiler backends and the store lock holder.",
        Json::object()));
    result.push_back(projectTool(
        "container_mount",
        "Mount or detach a supporting container and register it in project.json; the primary "
        "container is bound by project_open. library/runtime roles enter the role-addressed "
        "container stack (primary > library > runtime resolution, shadowed-entry tracking); "
        "support is the legacy reference role.",
        {{"action",
          {{"type", "string"},
           {"enum", {"mount", "detach"}},
           {"description", "Mount registers a new container; detach removes one by id."}}},
         {"role",
          {{"type", "string"},
           {"enum", {"primary", "support", "library", "runtime"}},
           {"description",
            "Container role; primary is rejected (bound by project_open), support/library/"
            "runtime are mountable."}}},
         {"path", {{"type", "string"}, {"description", "Mount only: filesystem path of the container JAR."}}},
         {"support_id", {{"type", "string"}, {"description", "Detach only: stable container id from project_info."}}}}));
    result.push_back(projectTool(
        "class_query",
        "Exact query of one class or resource: structure summary, full metadata, raw bytes, "
        "Jasm, method details, bytecode or parsed MANIFEST (kind selector). Selectors per "
        "kind: list/manifest take none; class/bytes/jasm require class; resource_bytes "
        "requires uri; method/bytecode/method_jasm require owner + name, with descriptor "
        "optional there: when omitted it is resolved by member name in the host class "
        "(unique match backfills, real overloads report ambiguity with candidate "
        "descriptors, absent names are rejected). "
        "cursor/offset/limit apply to kind=list (page), bytes and resource_bytes (chunk); "
        "page_size/fields apply to kind=list only. "
        "instructions[] node_type is a coarse semantic label; operands, jump targets and "
        "line numbers are authoritative in the jasm text.",
        with(with(pagination,
             {{"kind",
               {{"type", "string"},
                {"enum",
                 {"list", "class", "bytes", "jasm", "resource_bytes", "manifest", "method",
                  "bytecode", "method_jasm"}}}},
              {"class", classSelector},
              {"uri",
               {{"type", "string"},
                {"minLength", 1},
                {"description", "Required by kind=resource_bytes: exact resource URI."}}},
              {"limit",
               {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaximumResourceChunkSize}}},
              {"owner",
               {{"type", "string"},
                {"description",
                 "Required by kind=method|bytecode|method_jasm: host class internal name. "
                 "Other kinds reject it."}}},
              {"name",
               {{"type", "string"},
                {"description",
                 "Required by kind=method|bytecode|method_jasm: member name; a member "
                 "reference handle also resolves (descriptor backfills from the sidecar)."}}},
              {"descriptor",
               {{"type", "string"},
                {"description",
                 "Optional for kind=method|bytecode|method_jasm: member descriptor; omitted "
                 "values resolve by member name in the host class (unique match backfills; "
                 "ambiguous overloads and absent names are rejected), and a member reference "
                 "handle also backfills."}}}}), toolTimeout),
        {"kind"}));
    result.push_back(projectTool(
        "search",
        "Unified search: classes, members, strings, numbers, instructions, resource listing "
        "and shadow display-name resolution in both directions (kind selector). Supported "
        "keys per kind: classes = query/match_mode/case_sensitive/package_prefix/role; "
        "members = those + owner/member_kind; strings = members + string_kind; numbers = "
        "query/case_sensitive/owner/package_prefix/opcode/role; instructions = members + "
        "opcode + mnemonic/operand/cst (structured filters, mutually exclusive with query); "
        "resources = query/match_mode/case_sensitive/include_classes/role; shadow = name/"
        "owner/direction/symbol_kind/file (no pagination). Unknown keys per kind are "
        "rejected, never ignored.",
        with(with(pagination,
             {{"kind",
               {{"type", "string"},
                {"enum",
                 {"classes", "members", "strings", "numbers", "instructions", "resources",
                  "shadow"}}}},
              {"query",
               {{"type", "string"},
                {"minLength", 1},
                {"maxLength", 1024},
                {"description",
                 "Text or ECMAScript regular expression to match, according to match_mode."}}},
              {"match_mode",
               {{"type", "string"},
                {"enum", {"contains", "exact", "prefix", "regex"}},
                {"description",
                 "contains (default), exact, prefix or regex; prefix anchors the match at "
                 "the start of the searched texts."}}},
              {"case_sensitive", {{"type", "boolean"}}},
              {"owner",
               {{"type", "string"},
                {"description",
                 "members/strings/numbers/instructions kinds: scope filter on the host "
                 "class internal name; shadow kind: owner filter on matches."}}},
              {"name",
               {{"type", "string"},
                {"description",
                 "shadow kind only: symbol name to resolve (raw or shadow display name); "
                 "a full reference handle is also accepted for shadow_to_raw."}}},
              {"package_prefix", {{"type", "string"}}},
              {"opcode", {{"type", "integer"}, {"minimum", 0}, {"maximum", 199}}},
              {"member_kind", {{"type", "string"}, {"enum", {"field", "method"}}}},
              {"include_classes", {{"type", "boolean"}}},
              {"string_kind",
               {{"type", "string"},
                {"enum",
                 {"string_literal", "utf8_classref", "utf8_descriptor", "utf8_name"}},
                {"description",
                 "strings kind only: string_literal keeps string constants (ldc-loaded "
                 "constant_pool_string and printable strings of non-class resources); raw "
                 "constant-pool UTF8 entries are classified by content shape: utf8_classref "
                 "(type references: internal names, L...; forms, array descriptors), "
                 "utf8_descriptor (method descriptors starting with '('), utf8_name "
                 "(everything else: attribute names, member names); omitted keeps all."}}},
              {"mnemonic",
               {{"type", "string"},
                {"description",
                 "instructions kind only: exact mnemonic match (structured filter; mutually "
                 "exclusive with query)."}}},
              {"operand",
               {{"type", "string"},
                {"description",
                 "instructions kind only: substring match on the operand text (structured "
                 "filter; mutually exclusive with query)."}}},
              {"cst",
               {{"type", "string"},
                {"description",
                 "instructions kind only: substring match on the constant/reference part "
                 "(non-empty for ldc/field/invoke/new/cast/instanceof mnemonics; structured "
                 "filter; mutually exclusive with query)."}}},
              {"direction",
               {{"type", "string"},
                {"enum", {"shadow_to_raw", "raw_to_shadow"}},
                {"description", "shadow kind only; defaults to shadow_to_raw."}}},
              {"symbol_kind",
               {{"type", "string"},
                {"enum", {"class", "field", "method"}},
                {"description", "shadow kind only: optional symbol kind filter."}}},
              {"role",
               {{"type", "string"},
                {"enum", {"primary", "support", "library", "runtime"}},
                {"description",
                 "Optional container-role filter. library/runtime restrict the query to "
                 "containers mounted with that role (classes and resources kinds); primary "
                 "keeps the default primary-container scope."}}},
              {"file",
               {{"type", "string"},
                {"description",
                 "shadow kind only: optional sidecar path; defaults to `<jar>.mjt-shadow` "
                 "next to the primary container."}}}}), toolTimeout),
        {"kind"}));
    result.push_back(projectTool(
        "xref_query",
        "Reference graph query: incoming and outgoing references, call edges, hierarchy "
        "relations and override components (kind/target/direction selectors). references "
        "pages reference sites and supports role filters; relations/overrides return single "
        "payloads (no pagination). For references target=method|field the descriptor may be "
        "omitted: it resolves by member name in the host class (unique match backfills; "
        "ambiguous overloads and absent names are rejected). When the referenced member name "
        "exists but the descriptor does not, the response carries a diagnostics array "
        "listing the real descriptors.",
        with(with(pagination,
             {{"kind",
               {{"type", "string"},
                {"enum", {"references", "relations", "overrides"}},
                {"description",
                 "references: paged reference sites / call edges (target selector); "
                 "relations: hierarchy of one class (class key); overrides: override "
                 "components of one method (owner + name + descriptor)."}}},
              {"target",
               {{"type", "string"},
                {"enum", {"method", "field", "class"}},
                {"description",
                 "references kind only. target=method|field require owner + name "
                 "(descriptor optional, resolved by member name when omitted); "
                 "target=class requires class. Wrong selector keys are rejected."}}},
              {"direction",
               {{"type", "string"},
                {"enum", {"to", "from"}},
                {"description",
                 "references kind only: to lists reference sites, from lists call edges "
                 "(method target only, enables resolved); defaults to to."}}},
              {"resolved",
               {{"type", "boolean"},
                {"description",
                 "references kind only with target=method and direction=from: resolve call "
                 "edges to declared members."}}},
              {"class",
               {{"type", "string"},
                {"description",
                 "Required by references target=class and kind=relations: JVM internal name "
                 "or exact class resource URI."}}},
              {"owner",
               {{"type", "string"},
                {"description",
                 "Required by references target=method|field and kind=overrides: host class "
                 "internal name."}}},
              {"name",
               {{"type", "string"},
                {"description",
                 "Required by references target=method|field and kind=overrides: member "
                 "name; a member reference handle also resolves."}}},
              {"descriptor",
               {{"type", "string"},
                {"description",
                 "Required by kind=overrides; optional for references target=method|field: "
                 "member descriptor. Omitted values resolve by member name in the host "
                 "class (unique match backfills; ambiguous overloads and absent names are "
                 "rejected), and a member reference handle also backfills."}}},
              {"role",
               {{"type", "string"},
                {"enum", {"primary", "support", "library", "runtime"}},
                {"description",
                 "references kind only: optional container-role filter on reference sites; "
                 "defaults to all roles."}}}}), toolTimeout),
        {"kind"}));
    result.push_back(projectTool(
        "decompile",
        "Decompile one class or the class containing one exact method; async runs return a "
        "job_id polled with action=poll, cancelled with action=cancel and released with "
        "action=release.",
        with(decompiler,
             {{"action",
               {{"type", "string"},
                {"enum", {"decompile", "poll", "cancel", "release"}},
                {"description", "Defaults to decompile; the other actions operate on job_id."}}},
              {"job_id", {{"type", "string"}}},
              {"class", classSelector},
              {"owner", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"descriptor", {{"type", "string"}}}})));
    //validation 三档：relaxed（默认）对齐 JVM 赋值协变且只校验受影响
    //类；strict 保留精确类型匹配；off 跳过门禁（逃生通道）。
    const Json validationModeSchema = {
        {"type", "string"},
        {"enum", {"relaxed", "strict", "off"}},
        {"default", "relaxed"},
        {"description",
         "Edit validation gate: relaxed (default) accepts JVM-assignment-compatible bytecode "
         "and only validates the classes an operation affects; strict keeps exact type "
         "matching; off skips the gate entirely."}};
    const Json structuredPatch = with(
        mutationOptions,
        {{"class_uri", {{"type", "string"}}},
         {"mutation_kind",
          {{"type", "string"},
           {"enum", {"change_class_access", "set_field_access", "set_method_access", "add_field", "remove_field", "add_method", "remove_method", "add_marker_annotation", "remove_annotation", "rename_local", "replace_try_catch_blocks", "replace_method_body", "insert_instruction", "replace_instruction", "remove_instruction"}},
           {"description", "change_class_access applies incremental access flags: set/clear name lists, unmentioned bits are preserved (ACC_SUPER survives); deprecated maps to the Deprecated attribute instead of an access bit. It replaces the destructive whole-replacement set_class_access."}}},
         {"set", {{"type", "array"},
                   {"uniqueItems", true},
                   {"items", {{"type", "string"}, {"minLength", 1}}},
                   {"description", "change_class_access only: flag names to set (JVM lowercase mnemonics)."}}},
         {"clear", {{"type", "array"},
                    {"uniqueItems", true},
                    {"items", {{"type", "string"}, {"minLength", 1}}},
                    {"description", "change_class_access only: flag names to clear; set and clear must not overlap."}}},
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
    const Json rename = with(
        mutationOptions,
        {{"rename_kind",
          {{"type", "string"},
           {"enum", {"class", "package", "field", "method", "local"}},
           {"description", "Rename target kind; named rename_kind to stay distinct from the "
                           "edit_apply kind selector."}}},
         {"owner", {{"type", "string"}}},
         {"method_name", {{"type", "string"}}},
         {"name", {{"type", "string"}}},
         {"descriptor", {{"type", "string"}}},
         {"new_name", {{"type", "string"}}},
         {"local_index", {{"type", "integer"}}},
         {"cascade_overrides", {{"type", "boolean"}}},
         {"policy", {{"type", "string"}, {"enum", {"bytecode", "reflection", "metadata", "all"}}}}});
    const Json mapping = with(
        mutationOptions,
        {{"format", {{"type", "string"}}},
         {"mapping_text", {{"type", "string"}}},
         {"policy", {{"type", "string"},
                     {"enum", {"bytecode-only", "reflection", "metadata", "all"}}}}});
    result.push_back(projectTool(
        "edit_apply",
        "Single edit entry point: structured ClassMutation, Jasm or unified diff, class byte "
        "replacement, optimization pipeline, rename or mapping; dry_run previews without "
        "committing and expected_revision guards optimistic concurrency.",
        with(mutationOptions,
             {{"kind",
               {{"type", "string"},
                {"enum",
                 {"structured", "jasm", "jasm_diff", "method_jasm", "method_jasm_diff",
                  "class_bytes", "pipeline", "rename", "mapping"}}}},
              {"class_uri", {{"type", "string"}}},
              {"mutation_kind", structuredPatch.at("mutation_kind")},
              {"set", structuredPatch.at("set")},
              {"clear", structuredPatch.at("clear")},
              {"member_name", {{"type", "string"}}},
              {"member_descriptor", {{"type", "string"}}},
              {"access", {{"type", "integer"}}},
              {"access_flags", structuredPatch.at("access_flags")},
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
              {"try_catch_blocks", structuredPatch.at("try_catch_blocks")},
              {"local_variables", structuredPatch.at("local_variables")},
              {"uri", {{"type", "string"}}},
              {"source_name", {{"type", "string"}}},
              {"source", {{"type", "string"}}},
              {"diff", {{"type", "string"},
                        {"description",
                         "Unified diff against the current canonical Jasm (class or single "
                         "method, as returned by class_query kind=jasm/method_jasm). Context "
                         "lines must match exactly; no fuzz or 3way merge."}}},
              {"owner", {{"type", "string"}}},
              {"name", {{"type", "string"}}},
              {"descriptor", {{"type", "string"}}},
              {"content_base64",
               {{"type", "string"},
                {"minLength", 4},
                {"maxLength", ((mjbase::workspace::kMaximumClassReplacementSize + 2) / 3) * 4}}},
              {"pipeline", pipeline.at("pipeline")},
              {"passes", pipeline.at("passes")},
              {"jdk_path", pipeline.at("jdk_path")},
              {"rename_kind", rename.at("rename_kind")},
              {"method_name", {{"type", "string"}}},
              {"new_name", {{"type", "string"}}},
              {"cascade_overrides", {{"type", "boolean"}}},
              {"policy", {{"type", "string"},
                          {"enum", {"bytecode", "reflection", "metadata", "all",
                                    "bytecode-only"}}}},
              {"format", {{"type", "string"}}},
              {"mapping_text", {{"type", "string"}}},
              {"validation", validationModeSchema}}),
        {"kind"}));
    result.push_back(projectTool(
        "edit_history",
        "Edit ledger operations and queries: savepoint, restore, revert_class, undo, redo, "
        "manual compaction, ledger listing and modified classes.",
        with(pagination,
             {{"action",
               {{"type", "string"},
                {"enum",
                 {"savepoint", "restore", "revert_class", "undo", "redo", "compact", "list",
                  "modified_classes"}}}},
              {"name",
               {{"type", "string"},
                {"description", "Bookmark name for savepoint and restore."}}},
              {"class_uri", {{"type", "string"}, {"minLength", 1}}},
              {"keep_recent_bookmarks",
               {{"type", "integer"},
                {"minimum", 0},
                {"description",
                 "compact only: keep the N most recent bookmarks; omitted keeps all."}}},
              {"keep_undo_depth",
               {{"type", "integer"},
                {"minimum", 0},
                {"description",
                 "compact only: undo depth to preserve; 0 compacts as far as bookmark "
                 "constraints allow."}}},
              {"expected_revision", {{"type", "integer"}, {"minimum", 0}}},
              {"dry_run", {{"type", "boolean"}}},
              {"label", {{"type", "string"}}},
              {"validation", validationModeSchema}}),
        {"action"}));
    result.push_back(projectTool(
        "project_export",
        "Materialize the current edit state into a derived JAR at an explicit path; atomic and "
        "never touches the primary container.",
        {{"path", {{"type", "string"}, {"description", "Filesystem path of the exported JAR."}}}},
        {"path"}));
    return result;
}

namespace {

///标识符语义参数名集合。覆盖「JVM 常量池 UTF8」
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

///字符串数组的逐元素校验（decompile_classes 与 exceptions[]）。
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

///对象数组内指定字符串成员的校验（references[].owner、
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

///「name」键在 edit_history(savepoint) 里是书签标签（自由文本，允许超过
///classfile 65535 上限），不是 JVM 标识符；但同工具的 class_uri 仍是选择器，
///必须继续受守卫。豁免因此收窄为（edit_history, name）单键粒度，不再整工具
///豁免——整工具豁免会让日后新增的标识符语义键绕过 65535 守卫。
bool isFreeTextIdentifierKey(const std::string& tool, const std::string& key)
{
    return tool == "edit_history" && key == "name";
}

///工具边界统一校验（第二条）：对 class/owner/URI
///选择器与 descriptor/signature/source_name 等常量池 UTF8 语义参数强制
///classfile 规格对齐的长度上限（65535 字节），并覆盖 exceptions[]、
///try_catch_blocks[].type、local_variables[].name/descriptor/signature 数组
///白名单。合法长名（819/4096 等）不受影响；超限返回结构化错误，杜绝静默
///挂起/静默截断。指令树（instruction/initial_instructions/handle/field_value）
///内的同名嵌套键仍在口径外。
bool validateIdentifierBounds(const std::string& name, const Json& arguments, std::string& error)
{
    for (const auto& [key, value] : arguments.items()) {
        if (isFreeTextIdentifierKey(name, key)) continue;
        if (value.is_string() && identifierSelectorKeys().count(key) != 0) {
            if (!checkIdentifierLength("$." + key, value.get_ref<const std::string&>(), error))
                return false;
            continue;
        }
        //数组白名单：字符串数组逐元素；对象数组仅校验白名单成员
        //（exceptions[]、try_catch_blocks[].type、
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
    //折叠工具分支的未声明键在契约层直接报，消息形如
    //`unknown parameter "k" for kind=v; expected: …`，expected 列出当前分支的
    //全部合法键（branchKeysFor 单一事实源），不再依赖通用 additionalProperties
    //文案。分支无法定位（kind/target 缺失或非法）时跳过，schema 的 enum/
    //required 照常拦截；工具级已声明的键不在此拦（声明键的分支错配由分发层
    //checkBranchKeys 的 does-not-support 文案负责）。非对象参数直接跳过，交给
    //schema 的 type 检查报 must be an object。
    if ((name == "class_query" || name == "search" || name == "xref_query") &&
        arguments.is_object()) {
        const auto branch = branchKeysFor(name,
                                          arguments.value("kind", std::string{}),
                                          arguments.value("target", std::string{}),
                                          arguments.value("direction", std::string{}));
        if (branch.has_value()) {
            const auto& properties = schema.at("properties");
            std::vector<std::string> unknown;
            for (const auto& [key, _] : arguments.items()) {
                (void)_;
                if (properties.contains(key)) continue;
                unknown.push_back(key);
            }
            if (!unknown.empty()) {
                std::vector<std::string> expected = branch->selectors;
                expected.insert(expected.end(),
                                branch->optionalSelectors.begin(),
                                branch->optionalSelectors.end());
                expected.insert(expected.end(),
                                branch->supported.begin(),
                                branch->supported.end());
                std::sort(unknown.begin(), unknown.end());
                std::string message;
                for (const auto& key : unknown) {
                    if (!message.empty()) message += ", ";
                    message += "\"" + key + "\"";
                }
                message = "unknown parameter " + message + " for kind=" +
                          arguments.value("kind", std::string{});
                if (name == "xref_query" && arguments.contains("target"))
                    message += " target=" + arguments.value("target", std::string{});
                message += "; expected: ";
                for (std::size_t index = 0; index < expected.size(); ++index) {
                    if (index > 0) message += ", ";
                    message += expected[index];
                }
                error = message;
                return false;
            }
        }
    }
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

bool isRegisteredTool(const std::string& name)
{
    static const Json definitions = toolsList();
    return std::any_of(definitions.begin(), definitions.end(), [&](const Json& tool) {
        return tool.at("name") == name;
    });
}

}   //namespace mjt::mcp::detail
