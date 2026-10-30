#include "service.hpp"

#include <algorithm>
#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include "decompiler_support.hpp"
#include "json.hpp"
#include "shared/status_convert.hpp"
#include "limits.hpp"
#include "parser.hpp"
#include "shared/shadow_display.hpp"
#include "tool/shadow_slot.hpp"
#include "jbc/core/reader.hpp"
#include "mjbase/mapping/shadow_alias_format.hpp"
#include "mjbase/model/container_stack.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/edit_ledger.hpp"
#include "mjbase/project/project_store.hpp"
#include "mjbase/renamer/shadow_alias.hpp"

namespace mjt::mcp {

using namespace detail;
using mjt::workspace::SearchHit;
using mjt::workspace::SearchOptions;
using mjt::workspace::IWorkspace;

namespace {

///resources 主路径的文本匹配工厂（contains/exact/prefix/regex + case_sensitive）：
///与 role 过滤路径同一语义。prefix 由 match_mode 档引入；resources 主路径的 query
///从静默丢弃改为生效。非法正则返回 false 并置 error。
bool makeResourceTextMatcher(const Json& arguments,
                             std::function<bool(const std::string&)>& out,
                             Json& error)
{
    const auto query = arguments.value("query", std::string{});
    const auto mode = arguments.value("match_mode", std::string{"contains"});
    const bool caseSensitive = arguments.value("case_sensitive", false);
    const auto toLower = [](const std::string& value) {
        std::string lowered;
        lowered.reserve(value.size());
        for (const char character : value)
            lowered.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        return lowered;
    };
    const auto loweredQuery = toLower(query);
    std::optional<std::regex> compiled;
    if (mode == "regex") {
        try {
            compiled = std::regex(query,
                                  caseSensitive ? std::regex::ECMAScript
                                                : std::regex::ECMAScript | std::regex::icase);
        }
        catch (const std::regex_error&) {
            error = toolError("invalid regular expression: " + query);
            return false;
        }
    }
    out = [query, mode, caseSensitive, loweredQuery, compiled = std::move(compiled), toLower](
              const std::string& text) {
        if (mode == "exact") return text == query;
        if (mode == "prefix") {
            if (caseSensitive) return text.rfind(query, 0) == 0;
            return toLower(text).rfind(loweredQuery, 0) == 0;
        }
        if (compiled) return std::regex_search(text, *compiled);
        if (caseSensitive) return text.find(query) != std::string::npos;
        return toLower(text).find(loweredQuery) != std::string::npos;
    };
    return true;
}

}   //namespace

namespace {

///search 各分面的静态行键全集：searchHitJson 的行键随命中种类增减
///（类行 = class 键；strings 行带 string_kind；instructions 行带
///mnemonic/operand/cst），按 op 给出该分面的全集——投影按 contains 取值，
///缺键行照常省略该键，fields 校验不依赖结果集。
const std::vector<std::string>& searchRowKeys(const std::string& op)
{
    static const std::vector<std::string> kClassRowKeys = {
        "class", "descriptor", "instruction_index", "kind", "name", "resource_uri", "value"};
    static const std::vector<std::string> kMemberRowKeys = {
        "descriptor", "instruction_index", "kind", "name", "owner", "resource_uri", "value"};
    static const std::vector<std::string> kStringRowKeys = {
        "descriptor", "instruction_index", "kind", "name", "owner", "resource_uri",
        "string_kind", "value"};
    static const std::vector<std::string> kInstructionRowKeys = {
        "cst", "descriptor", "instruction_index", "kind", "mnemonic", "name", "operand",
        "owner", "resource_uri", "value"};
    if (op == "search_classes") return kClassRowKeys;
    if (op == "search_members" || op == "search_numbers") return kMemberRowKeys;
    if (op == "search_strings") return kStringRowKeys;
    return kInstructionRowKeys;
}

}   //namespace

std::optional<Json> McpToolService::callWorkspaceQueryTool(
    const std::string& name, const Json& arguments, ProjectSession& session)
{
    IWorkspace* current = session.workspace.get();
    //class_query / search 的 kind 选择器在入口翻译成内部分支名：分支体与响应
    //形状保持与旧细粒度工具逐字节一致，只有寻址面换名（RFC §8.2 折叠口径）。
    //分支定位键契约与参数键白名单在入口检查：用错键立即报
    //「requires X; got Y」，不支持键报「does not support; supported」，
    //不再静默丢弃。
    static const std::vector<std::string> kClassQueryKinds = {
        "list", "class", "bytes", "jasm", "resource_bytes", "manifest", "method",
        "bytecode", "method_jasm"};
    static const std::vector<std::string> kSearchKinds = {
        "classes", "members", "strings", "numbers", "instructions", "resources", "shadow"};
    std::string op = name;
    if (name == "class_query") {
        const auto kind = arguments.value("kind", std::string{});
        op = kind == "list"           ? "class_list"
             : kind == "class"        ? "class_get"
             : kind == "bytes"        ? "class_bytes"
             : kind == "jasm"         ? "class_jasm"
             : kind == "resource_bytes" ? "resource_read"
             : kind == "manifest"     ? "resource_manifest"
             : kind == "method"       ? "method_get"
             : kind == "bytecode"     ? "method_bytecode"
             : kind == "method_jasm"  ? "method_jasm"
                                      : "";
        if (op.empty()) return toolError(selectorKeyError("kind", kind, kClassQueryKinds));
        //分支参数键白名单来自 branchKeysFor（单一事实源：与 contract 校验
        //层的 unknown parameter 文案共用同一张表）。
        const auto branch = branchKeysFor("class_query", kind);
        if (!branch.has_value()) return toolError(selectorKeyError("kind", kind, kClassQueryKinds));
        if (auto error = checkBranchKeys("class_query kind=" + kind,
                                         arguments,
                                         true,
                                         branch->selectors,
                                         branch->supported,
                                         branch->optionalSelectors))
            return std::move(*error);
    }
    if (name == "search") {
        const auto kind = arguments.value("kind", std::string{});
        op = kind == "classes"      ? "search_classes"
             : kind == "members"    ? "search_members"
             : kind == "strings"    ? "search_strings"
             : kind == "numbers"    ? "search_numbers"
             : kind == "instructions" ? "search_instructions"
             : kind == "resources"  ? "workspace_resources"
             : kind == "shadow"     ? "shadow_query"
                                    : "";
        if (op.empty()) return toolError(selectorKeyError("kind", kind, kSearchKinds));
        //分支参数键白名单：过滤类参数只在该生效的 kind 上合法，键表
        //来自 branchKeysFor 单一事实源。
        const auto branch = branchKeysFor("search", kind);
        if (!branch.has_value()) return toolError(selectorKeyError("kind", kind, kSearchKinds));
        if (auto error = checkBranchKeys("search kind=" + kind,
                                         arguments,
                                         false,
                                         {},
                                         branch->supported))
            return std::move(*error);

        //role 过滤（RFC 0001 P5）：library/runtime 走挂载栈角色遍历，条目寻址
        //用复合挂载链地址 `<containerId>!/entry`。引擎五类搜索只覆盖主容器，
        //不把角色过滤伪装成跨容器语义——其余 kind 显式拒绝。primary 即主容器
        //缺省范围（走原路径）；support 容器是只读参考（引擎面不可搜索）。
        const auto roleFilter = arguments.value("role", std::string{});
        if (roleFilter == "library" || roleFilter == "runtime") {
            if (kind != "classes" && kind != "resources")
                return toolError("role filter supports kind=classes|resources only; kind '" +
                                 kind + "' searches the primary container");
            const auto wanted = roleFilter == "library" ? mjbase::model::ContainerRole::Library
                                                        : mjbase::model::ContainerRole::Runtime;
            if (!session.containerStack) return toolError("container stack is not available");
            const auto query  = arguments.value("query", std::string{});
            const auto mode   = arguments.value("match_mode", std::string{"contains"});
            const bool caseSensitive = arguments.value("case_sensitive", false);
            const auto toLower = [](const std::string& value) {
                std::string lowered;
                lowered.reserve(value.size());
                for (const char character : value)
                    lowered.push_back(static_cast<char>(
                        std::tolower(static_cast<unsigned char>(character))));
                return lowered;
            };
            const auto haystack = caseSensitive ? query : toLower(query);
            std::optional<std::regex> compiled;
            if (mode == "regex") {
                try {
                    compiled = std::regex(
                        query,
                        caseSensitive ? std::regex::ECMAScript
                                      : std::regex::ECMAScript | std::regex::icase);
                }
                catch (const std::regex_error&) {
                    return toolError("invalid regular expression: " + query);
                }
            }
            const auto matchesText = [&](const std::string& text) {
                if (mode == "exact") return text == query;
                //prefix 档：role 过滤路径与主路径同一语义。
                if (mode == "prefix") {
                    if (caseSensitive) return text.rfind(query, 0) == 0;
                    return toLower(text).rfind(haystack, 0) == 0;
                }
                if (compiled) return std::regex_search(text, *compiled);
                if (caseSensitive) return text.find(query) != std::string::npos;
                const auto lowered = toLower(text);
                return lowered.find(haystack) != std::string::npos;
            };
            //资源字节与类型名（workspace 内部 bytesOf/resourceTypeName 的等位
            //物；模型层无公开字节序列化面，条目类型此处逐一投影）。
            const auto bytesOf = [](const mjbase::model::IResource& resource)
                -> std::optional<std::vector<ca::u8>> {
                if (const auto* classResource = ca::core::tag_cast::dyn_cast<
                        const mjbase::model::ClassResource>(&resource))
                    return classResource->get();
                if (const auto* manifestResource = ca::core::tag_cast::dyn_cast<
                        const mjbase::model::ManifestResource>(&resource))
                    return manifestResource->get().toBytes();
                if (const auto* unknownResource = ca::core::tag_cast::dyn_cast<
                        const mjbase::model::UnknownResource>(&resource))
                    return unknownResource->get();
                return std::nullopt;
            };
            const auto typeNameOf = [](mjbase::model::ResourceType type) {
                switch (type) {
                case mjbase::model::ResourceType::Class: return "class";
                case mjbase::model::ResourceType::Manifest: return "manifest";
                case mjbase::model::ResourceType::Jar: return "jar";
                }
                return "resource";
            };
            Json values = Json::array();
            //直接遍历挂载记录（visitRole 的访问器首参是条目 URI，不带容器 id；
            //复合地址需要容器 id 与条目 URI 两段）。
            for (const auto& mounted : session.containerStack->containers()) {
                if (mounted.role != wanted) continue;
                const std::string id = mounted.id.to_std_string();
                mounted.container->visitResources(
                    [&](std::string_view entryUriView, const mjbase::model::IResource& resource) {
                        const std::string entryUri{entryUriView};
                        if (kind == "resources") {
                            const auto bytes = bytesOf(resource);
                            if (!bytes.has_value()) return;
                            if (!matchesText(entryUri)) return;
                            values.push_back(
                                {{"uri", mjbase::model::MountedContainerSet::mountAddress(id, entryUri).to_std_string()},
                                 {"container", id},
                                 {"type", typeNameOf(resource.getType())},
                                 {"size", bytes->size()}});
                            return;
                        }
                        const auto* classResource = ca::core::tag_cast::dyn_cast<
                            const mjbase::model::ClassResource>(&resource);
                        if (classResource == nullptr) return;
                        auto parsed = jbc::core::readClass(jbc::core::ByteView(classResource->get()));
                        if (parsed.is_err()) return;
                        const auto internalName =
                            std::string{std::move(parsed).unwrap().file->thisName()};
                        if (internalName.empty()) return;
                        if (!matchesText(internalName)) return;
                        values.push_back(
                            {{"uri", mjbase::model::MountedContainerSet::mountAddress(
                                         id, internalName + ".class")
                                         .to_std_string()},
                             {"container", id},
                             {"kind", "class"},
                             {"class", internalName}});
                    });
            }
            //role 过滤路径的行键按 kind 给出（资源行 / 类行两套）。
            const std::vector<std::string> roleRowKeys = kind == "resources"
                ? std::vector<std::string>{"container", "size", "type", "uri"}
                : std::vector<std::string>{"class", "container", "kind", "uri"};
            Json paged;
        std::string pageError;
        if (!page(std::move(values), arguments, paged, pageError, roleRowKeys))
            return toolError(pageError);
        return toolResult(std::move(paged));
        }
        if (roleFilter == "support")
            return toolError("support containers are reference-only and not searchable; "
                             "use xref role filters for reference-site attribution");
    }
    if (name == "project_info") {
        //查询前先 flush：台账健康反映含未落盘事务的活动链，flush 幂等且不动
        //revision，顺带触发自动压缩阈值检查；分析缓存合并 flush 同点（缓存
        //健康里的 pending 计数与槽清单因此与内存一致）。
        const auto flushed = session.workspace->flushLedger();
        if (flushed.is_err()) {
            const auto err = shared::toCaStatus(flushed.unwrap_err());
            return toolError("flush ledger failed", &err);
        }
        const auto cacheFlushed = flushAnalysisCache(session);
        if (cacheFlushed.is_err()) return toolError("flush analysis cache failed", &cacheFlushed);
        auto ledger = readPersistedLedger(session.projectDir);
        if (ledger.is_err()) {
            const auto status = projectStoreStatus(std::move(ledger).unwrap_err(), "read ledger");
            return toolError("read ledger failed", &status);
        }
        auto snapshot = std::move(ledger).unwrap();

        Json containers = Json::array();
        containers.push_back({{"role", "primary"},
                              {"path", current->sourcePath()},
                              {"fingerprint", primaryFingerprint(session.projectDir)}});
        //support/library/runtime 全部以 workspace 在册表为单一事实源：
        //角色化登记让 library/runtime 也只产生一条记录（role 是记录标签，
        //support 为缺省遗留值），不再叠挂载栈第二张表；id 只在 workspace 侧
        //稳定，工程登记面（project.json）的对应条目由 container_mount 维护。
        for (const auto& item : current->supportingResources())
            containers.push_back({{"role", item.role.empty() ? "support" : item.role},
                                  {"id", item.id},
                                  {"path", item.path},
                                  {"fingerprint", item.fingerprint}});

        Json ledgerJson = {{"bound", true}, {"generation", snapshot.generation}};
        ca::u64 base    = 0;
        ca::u64 head    = 0;
        ca::u64 txnCount = 0;
        std::string baseContainer;
        const mjbase::project::LedgerAnchor* anchor = nullptr;
        for (const auto& row : snapshot.rows) {
            if (row.kind == mjbase::project::LedgerRow::Kind::Anchor) {
                anchor = &row.anchor;
                base   = row.anchor.txn;
                if (!row.anchor.container.is_empty()) baseContainer = row.anchor.container.to_std_string();
                continue;
            }
            if (row.kind != mjbase::project::LedgerRow::Kind::Transaction) continue;
            ++txnCount;
            head = row.transaction.txn;
            if (baseContainer.empty())
                baseContainer = row.transaction.baseContainer.to_std_string();
        }
        ledgerJson["chain_base_txn"]  = base;
        ledgerJson["chain_head_txn"]  = head;
        ledgerJson["transactions"]    = txnCount;
        ledgerJson["base_container"]  = baseContainer;
        ledgerJson["has_baseline"]    = anchor != nullptr;
        if (anchor != nullptr)
            ledgerJson["baseline_section"] = anchor->baselineSection.to_std_string();
        Json bookmarks = Json::array();
        for (const auto& row : snapshot.rows) {
            if (row.kind != mjbase::project::LedgerRow::Kind::Bookmark) continue;
            bookmarks.push_back({{"name", row.bookmark.name.to_std_string()},
                                 {"txn", row.bookmark.txn}});
        }
        ledgerJson["bookmarks"] = std::move(bookmarks);
        const auto& config = snapshot.manifest.ledgerConfig.has_value()
                                 ? *snapshot.manifest.ledgerConfig
                                 : mjbase::project::LedgerEngineConfig{};
        ledgerJson["compression"] = {{"auto_compact", config.autoCompact},
                                     {"compact_max_transactions", config.compactMaxTransactions},
                                     {"compact_max_ledger_bytes", config.compactMaxLedgerBytes},
                                     {"compact_max_distance", config.compactMaxDistance},
                                     {"min_undo_depth", config.minUndoDepth}};

        Json lockJson = nullptr;
        {
            std::ifstream lockFile(std::filesystem::path(session.projectDir) / "store.lock",
                                   std::ios::binary);
            if (lockFile) {
                std::string text{std::istreambuf_iterator<char>(lockFile),
                                 std::istreambuf_iterator<char>()};
                try {
                    const auto parsed = Json::parse(text);
                    lockJson          = {{"pid", parsed.value("pid", 0)},
                                         {"created_at", parsed.value("created_at", std::string{})},
                                         {"owner", parsed.value("owner", std::string{})}};
                }
                catch (const Json::exception&) {
                    //锁文件不可解析按活锁 fail-closed；总览仍照常返回，
                    //持有者信息省略。
                }
            }
        }

        //缓存健康（RFC 0001 P3）：槽清单来自 flush 后的 store.json 清单扫描，
        //命中计数是本会话运行时面（缓存命中数自会话起算，不落盘）。
        Json cacheJson = {{"bound", session.cacheStore != nullptr}};
        if (session.cacheStore) {
            ca::usize decompileSlots = 0;
            ca::usize xrefSlots      = 0;
            auto manifest = mjbase::project::readStoreManifestFile(
                ca::str::Utf8StringRef::from_data(
                    reinterpret_cast<const ca::u8*>(session.projectDir.data()),
                    session.projectDir.size()));
            if (manifest.is_ok()) {
                auto loaded = std::move(manifest).unwrap();
                for (const auto& entry : loaded.sections) {
                    const auto name = entry.name.ref().to_std_string();
                    if (name.rfind(mjbase::project::kDecompileCacheSectionPrefix, 0) == 0)
                        ++decompileSlots;
                    else if (name.rfind(mjbase::project::kXrefCacheSectionPrefix, 0) == 0)
                        ++xrefSlots;
                }
            }
            std::lock_guard<std::mutex> lock(session.cachePending->mutex);
            cacheJson["decompile_slots"]   = decompileSlots;
            cacheJson["xref_slots"]        = xrefSlots;
            cacheJson["pending_records"]   = session.cacheStore->pendingDecompileCount();
            cacheJson["session_hits"]      = session.cacheHits;
            cacheJson["session_misses"]    = session.cacheMisses;
        }

        return toolResult({{"project_id", arguments.at("project_id")},
                           {"project_dir", session.projectDir},
                           {"persistence", session.temporary ? "temporary" : "project"},
                           {"source_path", current->sourcePath()},
                           {"revision", current->revision()},
                           {"dirty", current->isDirty()},
                           {"exportable", current->canExport()},
                           {"containers", std::move(containers)},
                           {"ledger", std::move(ledgerJson)},
                           {"cache", std::move(cacheJson)},
                           {"decompilers", [&] {
                               //settings 由 session 工具在状态锁下修改；执行体
                               //不持状态锁，读取默认值时短持（M1 同一口径）。
                               std::lock_guard<std::mutex> lock(mutex_);
                               return decompilerAvailabilityList(decompilerSettings_);
                           }()},
                           {"store_lock", std::move(lockJson)}});
    }
    if (name == "workspace_info") {
        Json support = Json::array();
        for (const auto& item : current->supportingResources())
            support.push_back(supportingResourceJson(item));
        return toolResult({{"workspace_id", arguments.at("workspace_id")},
                           {"source_path", current->sourcePath()},
                           {"revision", current->revision()},
                           {"dirty", current->isDirty()},
                           {"exportable", current->canExport()},
                           {"supporting_resources", std::move(support)}});
    }
    if (name == "workspace_modified_classes") {
        auto modified = current->modifiedClasses();
        if (modified.is_err())
            { const auto err = shared::toCaStatus(modified.unwrap_err()); return toolError("list modified classes failed", &err); }
        Json values = Json::array();
        for (const auto& item : modified.unwrap()) {
            values.push_back({{"resource_uri", item.resourceUri},
                              {"original_internal_name", item.originalInternalName},
                              {"current_internal_name", item.currentInternalName},
                              {"original_fingerprint", item.originalFingerprint},
                              {"current_fingerprint", item.currentFingerprint},
                              {"original_size", item.originalSize},
                              {"current_size", item.currentSize}});
        }
        Json paged;
        std::string pageError;
        if (!page(std::move(values), arguments, paged, pageError,
                  {"current_fingerprint", "current_internal_name", "current_size",
                   "original_fingerprint", "original_internal_name", "original_size",
                   "resource_uri"}))
            return toolError(pageError);
        return toolResult(std::move(paged));
    }
    if (name == "workspace_add_support") {
        auto result = current->addSupportingJar(arguments.value("path", std::string{}));
        if (result.is_err()) { const auto err = shared::toCaStatus(result.unwrap_err()); return toolError("add supporting resource failed", &err); }
        return toolResult({{"supporting_resource", supportingResourceJson(result.unwrap())}});
    }
    if (name == "workspace_list_support") {
        Json values = Json::array();
        for (const auto& item : current->supportingResources())
            values.push_back(supportingResourceJson(item));
        Json paged;
        std::string pageError;
        if (!page(std::move(values), arguments, paged, pageError,
                  {"class_count", "fingerprint", "id", "path", "resource_count", "role"}))
            return toolError(pageError);
        return toolResult(std::move(paged));
    }
    if (name == "workspace_remove_support") {
        auto result = current->removeSupportingJar(arguments.value("support_id", std::string{}));
        if (result.is_err()) { const auto err = shared::toCaStatus(result.unwrap_err()); return toolError("remove supporting resource failed", &err); }
        return toolResult({{"removed", supportingResourceJson(result.unwrap())}});
    }
    if (op == "workspace_resources") {
        auto resources = current->listResources(arguments.value("include_classes", true));
        if (resources.is_err()) { const auto err = shared::toCaStatus(resources.unwrap_err()); return toolError("list resources failed", &err); }
        //query/match_mode 在主路径生效（与 role 过滤路径同一语义），
        //不再静默丢弃。
        std::function<bool(const std::string&)> matchesText;
        Json matcherError;
        if (!makeResourceTextMatcher(arguments, matchesText, matcherError))
            return matcherError;
        Json values = Json::array();
        for (const auto& item : resources.unwrap()) {
            if (!matchesText(item.uri)) continue;
            values.push_back({{"uri", item.uri}, {"type", item.type}, {"size", item.size}});
        }
        Json paged;
        std::string pageError;
        if (!page(std::move(values), arguments, paged, pageError, {"size", "type", "uri"}))
            return toolError(pageError);
        return toolResult(std::move(paged));
    }
    if (op == "resource_read") {
        auto content = current->readResource(arguments.value("uri", std::string{}));
        if (content.is_err()) { const auto err = shared::toCaStatus(content.unwrap_err()); return toolError("read resource failed", &err); }
        auto value = std::move(content).unwrap();
        auto offset = arguments.value("offset", static_cast<ca::usize>(0));
        if (!parseCursor(arguments, value.bytes.size(), offset)) {
            return toolError("cursor must be an unsigned byte offset");
        }
        const auto requested = arguments.value("limit", kMaximumResourceChunkSize);
        if (requested == 0) return toolError("limit must be greater than zero");
        const auto limit = std::min(requested, kMaximumResourceChunkSize);
        offset = std::min(offset, value.bytes.size());
        const auto end = std::min(value.bytes.size(), offset + limit);
        std::vector<ca::u8> chunk(value.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                  value.bytes.begin() + static_cast<std::ptrdiff_t>(end));
        Json result = {{"uri", value.summary.uri},
                       {"type", value.summary.type},
                       {"size", value.summary.size},
                       {"offset", offset},
                       {"encoding", "base64"},
                       {"content", base64(chunk)}};
        if (end < value.bytes.size()) {
            //字节分页侧车：与列表分页（page/classSummaryPage 的 P2-3 契约）
            //同口径五键 {partial, reason:"truncated", received, total, cursor}——
            //received=本页实际字节数，total=资源总字节数，cursor=下一页字节偏移
            //（与 next_cursor 同值）。仅截断路径写入，完整页零变化。
            const auto cursor = std::to_string(end);
            result["next_cursor"] = cursor;
            result["partial"]     = true;
            result["reason"]      = "truncated";
            result["received"]    = chunk.size();
            result["total"]       = value.bytes.size();
            result["cursor"]      = cursor;
        }
        return toolResult(std::move(result));
    }
    if (op == "resource_manifest") {
        auto attributes = current->manifestAttributes();
        if (attributes.is_err()) { const auto err = shared::toCaStatus(attributes.unwrap_err()); return toolError("read manifest failed", &err); }
        Json values = Json::array();
        for (const auto& item : attributes.unwrap()) values.push_back({{"key", item.key}, {"value", item.value}});
        return toolResult({{"attributes", std::move(values)}});
    }
    if (name == "resource_strings") {
        auto strings = current->resourceStrings(arguments.value("minimum_length", static_cast<ca::usize>(4)));
        if (strings.is_err()) { const auto err = shared::toCaStatus(strings.unwrap_err()); return toolError("extract resource strings failed", &err); }
        Json values = Json::array();
        for (const auto& item : strings.unwrap())
            values.push_back({{"uri", item.uri}, {"offset", item.offset}, {"value", item.value}});
        Json paged;
        std::string pageError;
        if (!page(std::move(values), arguments, paged, pageError, {"offset", "uri", "value"}))
            return toolError(pageError);
        return toolResult(std::move(paged));
    }
    if (op == "search_classes" || op == "search_members" || op == "search_strings" ||
        op == "search_numbers" || op == "search_instructions") {
        const auto query = arguments.value("query", std::string{});
        const auto matchMode = arguments.value("match_mode", std::string{"contains"});
        //strings 分面；instructions 结构化过滤（与拼接文本 query 互斥，
        //严格切换不给混搭语义）。
        const auto stringKindFilter = arguments.value("string_kind", std::string{});
        const auto mnemonicFilter   = arguments.value("mnemonic", std::string{});
        auto operandFilter          = arguments.value("operand", std::string{});
        //JasmBody 槽：operand 过滤器带 `${` 引用形状 → 操作数行展开
        //（类 handle / owner.name 复合 → raw 文本再子串匹配）；raw 过滤器
        //原样直通（过滤契约零漂移）。
        if (!operandFilter.empty() && operandFilter.find("${") != std::string::npos) {
            const auto  slots = slot::loadWorkspaceShadowTable(*current);
            std::string resolvedOperand;
            Json        operandSlotError;
            if (!slot::resolveJasmBodyArgument(slots, operandFilter, resolvedOperand, operandSlotError))
                return operandSlotError;
            operandFilter = std::move(resolvedOperand);
        }
        const auto cstFilter        = arguments.value("cst", std::string{});
        const bool structured =
            !mnemonicFilter.empty() || !operandFilter.empty() || !cstFilter.empty();
        if (structured && !query.empty())
            return toolError("query and mnemonic/operand/cst are mutually exclusive; "
                             "use one matching mode");
        SearchOptions options;
        std::string parseError;
        if (!parseSearchOptions(arguments, options, parseError)) return toolError(parseError);
        //结构化模式借道全量指令集（mjbase 拒绝空 query，单空格恒命中拼
        //接文本），命中后再按结构化字段收紧。
        auto hits =
            op == "search_classes"
                ? current->searchClasses(query, options)
                : op == "search_members"
                      ? current->searchMembers(query, options)
                      : op == "search_strings"
                            ? current->searchStrings(query, options)
                            : op == "search_numbers"
                                  ? current->searchNumbers(query, options)
                                  : current->searchInstructions(
                                        structured ? std::string(" ") : query, options);
        if (hits.is_err()) { const auto err = shared::toCaStatus(hits.unwrap_err()); return toolError("workspace search failed", &err); }
        auto hitList = std::move(hits).unwrap();
        //prefix 档在 mjt 侧收紧（contains 结果集恒为前缀命中的超集）。
        if (matchMode == "prefix") {
            const bool caseSensitive = options.caseSensitive;
            const auto hasPrefix = [&](const std::string& text) {
                const auto& haystack = caseSensitive ? text : detail::lowercased(text);
                const auto& needle   = caseSensitive ? query : detail::lowercased(query);
                return haystack.rfind(needle, 0) == 0;
            };
            hitList.erase(std::remove_if(hitList.begin(),
                                         hitList.end(),
                                         [&](const mjt::workspace::SearchHit& hit) {
                                             //判别文本面与 mjbase 的匹配面同源：
                                             //类行=内部名/URI，成员行=owner/name/
                                             //descriptor/value，其余=拼接 value。
                                             if (hit.kind == "class")
                                                 return !hasPrefix(hit.owner) &&
                                                        !hasPrefix(hit.resourceUri);
                                             if (hit.kind == "field" || hit.kind == "method")
                                                 return !hasPrefix(hit.owner) &&
                                                        !hasPrefix(hit.name) &&
                                                        !hasPrefix(hit.descriptor) &&
                                                        !hasPrefix(hit.value);
                                             return !hasPrefix(hit.value);
                                         }),
                        hitList.end());
        }
        if (op == "search_strings" && !stringKindFilter.empty()) {
            //分面过滤：string_literal 放行串常量（ldc 加载 + 非 class 资源
            //可打印串）；裸 UTF8 行按内容形态细分，与响应 string_kind 同一
            //判定（utf8StringKind）。
            const bool wantLiteral = stringKindFilter == "string_literal";
            hitList.erase(std::remove_if(hitList.begin(),
                                         hitList.end(),
                                         [&](const mjt::workspace::SearchHit& hit) {
                                             if (wantLiteral)
                                                 return hit.kind != "constant_pool_string" &&
                                                        hit.kind != "resource_string";
                                             if (hit.kind != "constant_pool_utf8") return true;
                                             return detail::utf8StringKind(hit.value) !=
                                                    stringKindFilter;
                                         }),
                        hitList.end());
        }
        if (structured) {
            const bool caseSensitive = options.caseSensitive;
            const auto containsPart = [&](const std::string& text, const std::string& part) {
                if (part.empty()) return true;
                if (caseSensitive) return text.find(part) != std::string::npos;
                return detail::lowercased(text).find(detail::lowercased(part)) !=
                       std::string::npos;
            };
            hitList.erase(std::remove_if(hitList.begin(),
                                         hitList.end(),
                                         [&](const mjt::workspace::SearchHit& hit) {
                                             detail::InstructionHitParts parts;
                                             if (!detail::parseInstructionHitValue(hit.value, parts))
                                                 return true;
                                             //mnemonic 精确（区分大小写）；operand/cst
                                             //按 case_sensitive 子串；cst 只看常量/
                                             //引用摘要。
                                             if (!mnemonicFilter.empty() &&
                                                 parts.mnemonic != mnemonicFilter)
                                                 return true;
                                             if (!containsPart(parts.operand, operandFilter))
                                                 return true;
                                             if (!containsPart(parts.cst, cstFilter))
                                                 return true;
                                             return false;
                                         }),
                        hitList.end());
        }
        Json values = Json::array();
        for (const auto& hit : hitList) values.push_back(searchHitJson(hit));
        Json paged;
        std::string pageError;
        if (!page(std::move(values), arguments, paged, pageError, searchRowKeys(op)))
            return toolError(pageError);
        return toolResult(std::move(paged));
    }
    if (op == "class_list") {
        auto classes = current->listClassSummaries();
        if (classes.is_err()) { const auto err = shared::toCaStatus(classes.unwrap_err()); return toolError("list classes failed", &err); }
        Json paged;
        std::string pageError;
        if (!classSummaryPage(classes.unwrap(), arguments, paged, pageError))
            return toolError(pageError);
        return toolResult(std::move(paged));
    }
    if (op == "class_get") {
        //name 槽：class 整串 = Ref（${Class_N.token8}）或 raw 二选一；
        //sidecar 活跃时 raw 缺表 → raw-not-found（§6 四分类），Ref 展开失败按
        //四分类报错；无 sidecar 时 raw 零漂移（空表恒等直通）。
        std::string className;
        Json        slotError;
        if (!slot::resolveClassArgument(*current, arguments, "class", true, className, slotError))
            return slotError;
        auto value = current->classInfo(className);
        if (value.is_err()) { const auto err = shared::toCaStatus(value.unwrap_err()); return toolError("get class failed", &err); }
        return toolResult(classJson(value.unwrap()));
    }
    if (op == "class_bytes") {
        std::string className;
        Json        slotError;
        if (!slot::resolveClassArgument(*current, arguments, "class", true, className, slotError))
            return slotError;
        auto content = current->readClassBytes(className);
        if (content.is_err()) { const auto err = shared::toCaStatus(content.unwrap_err()); return toolError("read class bytes failed", &err); }
        auto value = std::move(content).unwrap();
        auto offset = arguments.value("offset", static_cast<ca::usize>(0));
        if (!parseCursor(arguments, value.bytes.size(), offset)) {
            return toolError("cursor must be an unsigned byte offset");
        }
        const auto requested = arguments.value("limit", kMaximumResourceChunkSize);
        if (requested == 0) return toolError("limit must be greater than zero");
        const auto limit = std::min(requested, kMaximumResourceChunkSize);
        offset = std::min(offset, value.bytes.size());
        const auto end = std::min(value.bytes.size(), offset + limit);
        std::vector<ca::u8> chunk(value.bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                  value.bytes.begin() + static_cast<std::ptrdiff_t>(end));
        Json result = {{"resource_uri", value.resourceUri},
                       {"class", value.internalName},
                       {"fingerprint", value.fingerprint},
                       {"size", value.bytes.size()},
                       {"offset", offset},
                       {"encoding", "base64"},
                       {"content", base64(chunk)}};
        if (end < value.bytes.size()) {
            //同 resource_read：字节分页侧车五键，仅截断路径写入，完整页零变化。
            const auto cursor = std::to_string(end);
            result["next_cursor"] = cursor;
            result["partial"]     = true;
            result["reason"]      = "truncated";
            result["received"]    = chunk.size();
            result["total"]       = value.bytes.size();
            result["cursor"]      = cursor;
        }
        return toolResult(std::move(result));
    }
    if (op == "class_jasm") {
        std::string className;
        Json        slotError;
        if (!slot::resolveClassArgument(*current, arguments, "class", true, className, slotError))
            return slotError;
        auto selected = current->classInfo(className);
        if (selected.is_err())
            { const auto err = shared::toCaStatus(selected.unwrap_err()); return toolError("select class for Jasm failed", &err); }
        auto value = current->getJasm(selected.unwrap().resourceUri);
        if (!value.isOk()) {
            Json diagnostics = Json::array();
            for (const auto& item : value.diagnostics) diagnostics.push_back(diagnosticJson(item));
            return toolResult({{"diagnostics", std::move(diagnostics)}}, true);
        }
        return toolResult({{"class", value.value->internalName.to_std_string()},
                           {"jasm", utf8(value.value->source.ref())}});
    }
    if (op == "method_get" || op == "method_bytecode" || op == "method_jasm") {
        //name 槽：owner=ClassRef/raw、name=MemberRef/raw（desc 不进
        //句柄，MemberRef 命中时自表回填）。
        const auto slots = slot::loadWorkspaceShadowTable(*current);
        std::string ownerRaw;
        std::string memberName;
        std::string descriptor;
        Json        slotError;
        if (!slot::resolveMethodSlots(slots, arguments, true, ownerRaw, memberName, descriptor, slotError))
            return slotError;
        //descriptor 省略时按宿主类名称解析回填——唯一命中即用；名字
        //不存在报错（说明 owner 内无此名，复用既有的候选枚举机）；真重载
        //报歧义并列出全部候选（同一 diagnostics 格式）。显式传
        //descriptor 时行为零变化（含 mismatch 诊断路径）。
        if (descriptor.empty() &&
            !slot::backfillMemberDescriptor(*current, ownerRaw, memberName, false, descriptor, slotError))
            return slotError;
        //显式 descriptor 失配诊断：名字存在但描述符不符时列出候选（与省略
        //descriptor 的歧义路径同一呈现面）；名字不存在/owner 表外走 canonical。
        Json mismatchError;
        if (!slot::memberDescriptorMismatch(*current, ownerRaw, memberName, descriptor,
                                            false, mismatchError))
            return mismatchError;
        if (op == "method_get") {
            auto value = current->methodInfo(ownerRaw, memberName, descriptor);
            if (value.is_err()) { const auto err = shared::toCaStatus(value.unwrap_err()); return toolError("get method failed", &err); }
            return toolResult(methodJson(value.unwrap()));
        }
        if (op == "method_bytecode") {
            auto value = current->methodBytecode(ownerRaw, memberName, descriptor);
            if (value.is_err()) { const auto err = shared::toCaStatus(value.unwrap_err()); return toolError("get method bytecode failed", &err); }
            Json instructions = Json::array();
            for (const auto& item : value.unwrap().instructions)
                instructions.push_back({{"index", item.index},
                                        {"opcode", item.opcode},
                                        {"mnemonic", item.mnemonic},
                                        //mjbase instructionNodeType 语义归类
                                        //（invoke/field/ldc/cp_ref/…），单一
                                        //事实源在基座，此处直取不再本地重建。
                                        {"node_type", item.nodeType}});
            //指令摘要只有 index/opcode/mnemonic/node_type,操作数级证据在
            //jasm 全文;两份一次取齐,调用方不必退回自写解析器。
            Json payload{{"method", methodJson(value.unwrap().method)},
                         {"instructions", std::move(instructions)}};
            auto jasm = current->getMethodJasm(ownerRaw, memberName, descriptor);
            if (jasm.isOk()) {
                payload["jasm"] = utf8(jasm.value->source.ref());
            } else {
                Json diagnostics = Json::array();
                for (const auto& item : jasm.diagnostics) diagnostics.push_back(diagnosticJson(item));
                payload["jasm_diagnostics"] = std::move(diagnostics);
            }
            return toolResult(std::move(payload));
        }
        auto value = current->getMethodJasm(ownerRaw, memberName, descriptor);
        if (!value.isOk()) {
            Json diagnostics = Json::array();
            for (const auto& item : value.diagnostics) diagnostics.push_back(diagnosticJson(item));
            return toolResult({{"diagnostics", std::move(diagnostics)}}, true);
        }
        return toolResult({{"class", value.value->internalName.to_std_string()},
                           {"name", value.value->name.to_std_string()},
                           {"descriptor", value.value->descriptor.to_std_string()},
                           {"jasm", utf8(value.value->source.ref())}});
    }
    if (name == "search" &&
        arguments.value("kind", std::string{}) == "shadow") {
        //shadow 双向查询（search kind=shadow）：direction=shadow_to_raw（缺省）
        //= 显示名 → 真名，raw_to_shadow = 真名 → 显示名。sidecar 缺省解析
        //`<jar>.mjt-shadow`，`file` 参数显式覆盖。机械占位名跨类重号，命中返回
        //列表。T2：matches[] 增发 `ref` 引用句柄（§2 形态，缺 token/手工名
        //省略列）；shadow_to_raw 接受整串 Ref 输入（handle → 条目反查，四分类
        //同名槽口径）；`owner` 过滤接受 ClassRef；symbol_kind 是符号种类过滤
        //（search 的 kind 槽已被工具级选择器占用）。
        const auto lookupName  = arguments.value("name", std::string{});
        const auto fileArg     = arguments.value("file", std::string{});
        const auto ownerFilter = arguments.value("owner", std::string{});
        const auto kindFilter  = arguments.value("symbol_kind", std::string{});
        const auto toRaw = arguments.value("direction", std::string{"shadow_to_raw"}) ==
                           "shadow_to_raw";
        if (lookupName.empty()) return toolError("name must be a non-empty string");
        const auto sidecarPath =
            fileArg.empty() ? mjt::resolveShadowPath(current->sourcePath(), "") : fileArg;
        std::error_code fsError;
        if (!std::filesystem::exists(sidecarPath, fsError)) {
            return toolError("shadow sidecar not found: " + sidecarPath +
                             " (generate it with `mjt shadow -i <jar>`)");
        }
        auto sidecar = mjbase::mapping::readShadowAliasFile(
            ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(sidecarPath.data()),
                                              sidecarPath.size()));
        if (sidecar.is_err()) {
            auto       err     = std::move(sidecar).unwrap_err();
            const auto message = err.message.to_std_string();
            return toolError("read shadow sidecar failed: " + message);
        }
        const auto table =
            mjbase::renamer::ShadowAliasTable::fromMapping(std::move(sidecar).unwrap());
        std::vector<mjbase::renamer::ShadowAliasEntry> matches;
        mjbase::renamer::ShadowSlotResult              handleResult;
        auto handleEntry =
            mjbase::renamer::resolveShadowHandleEntry(table, lookupName, handleResult);
        if (handleEntry.has_value()) {
            //整串 Ref 输入：仅 shadow_to_raw 语义成立（handle → 真名）。
            if (!toRaw) {
                return toolError(
                    "raw_to_shadow expects a raw name; pass the reference to shadow_to_raw");
            }
            matches.push_back(*handleEntry);
        }
        else if (handleResult.error != mjbase::renamer::ShadowSlotError::None) {
            return toolError(handleResult.message);
        }
        else {
            matches = toRaw ? table.findByShadowName(lookupName)
                            : table.findByRawName(lookupName);
        }
        std::string ownerFilterRaw = ownerFilter;
        if (!ownerFilterRaw.empty()) {
            //owner 过滤槽同 name 槽口径：Ref → 解析为真实名（四分类同源）。
            const auto ownerForm = mjbase::renamer::parseShadowRef(ownerFilterRaw).form;
            if (ownerForm == mjbase::renamer::ShadowRefForm::ClassRef ||
                ownerForm == mjbase::renamer::ShadowRefForm::MemberRef) {
                mjbase::renamer::ShadowSlotResult ownerResult;
                const auto ownerEntry =
                    mjbase::renamer::resolveShadowHandleEntry(table, ownerFilterRaw, ownerResult);
                if (!ownerEntry.has_value()) {
                    if (ownerResult.error != mjbase::renamer::ShadowSlotError::None)
                        return toolError(ownerResult.message);
                    return toolError("owner filter must be a raw internal name or a class "
                                     "reference like ${Class_N.token8}");
                }
                ownerFilterRaw = std::string(ownerEntry->rawName);
            }
        }
        auto kindLabelOf = [](mjbase::renamer::ShadowKind kind) {
            switch (kind) {
            case mjbase::renamer::ShadowKind::Class: return "class";
            case mjbase::renamer::ShadowKind::Field: return "field";
            case mjbase::renamer::ShadowKind::Method: return "method";
            }
            return "unknown";
        };
        //条目字段是表内池的借用视图：进 Json 前统一转 std::string 值。
        auto toStd = [](std::string_view value) { return std::string(value); };
        Json values = Json::array();
        for (const auto& entry : matches) {
            if (!ownerFilterRaw.empty() && toStd(entry.owner) != ownerFilterRaw) continue;
            if (!kindFilter.empty() && kindLabelOf(entry.kind) != kindFilter) continue;
            Json match = {{"kind", kindLabelOf(entry.kind)},
                          {"owner", toStd(entry.owner)},
                          {"name", toStd(entry.rawName)},
                          {"desc", toStd(entry.desc)},
                          {"shadow", toStd(entry.shadowName)}};
            const auto ref = mjbase::renamer::shadowRefHandleOf(table, entry);
            if (!ref.empty()) match["ref"] = ref;
            values.push_back(std::move(match));
        }
        return toolResult(
            {{"file", sidecarPath}, {"count", values.size()}, {"matches", std::move(values)}});
    }
    return std::nullopt;
}

}   //namespace mjt::mcp
