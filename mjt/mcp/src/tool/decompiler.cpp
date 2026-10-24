#include "service.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "decompiler_support.hpp"
#include "job.hpp"
#include "json.hpp"
#include "limits.hpp"
#include "shared/status_convert.hpp"
#include "tool/shadow_slot.hpp"

#include "mjbase/project/analysis_cache.hpp"
#include "mjbase/project/project_store.hpp"

namespace mjt::mcp {

using namespace detail;
using mjbase::workspace::MutationRequest;
using mjbase::workspace::PipelineRequest;
using mjbase::workspace::WorkflowCandidateCommitRequest;
using mjbase::workspace::WorkflowReferenceQuery;
using mjbase::workspace::WorkflowRequest;
using mjbase::workspace::IWorkspace;

namespace fs = std::filesystem;

namespace {

// ── 条目级反编译缓存（RFC 0001 P3；mjbase AnalysisCacheStore 接线）──────────
//
// 槽 =（登记容器 digest、引擎与版本自述、options profile）；条目级粒度由槽内
// 记录的 entryDigest 承担——entryDigest 是反编译时刻类字节的精确 SHA-256
// （ledgerDigestOfBytes 同口径，从工作副本实际字节算出），编辑一个类只失效
// 该类。缓存引擎不获取 store.lock（单写者锁归台账引擎）；put 驻内存 pending，
// flush 挂点与 flushLedger 同点（project_close / project_export 前）。

///std::string → 借用视图（clone 前提：源串在 clone 调用期内存活）。
ca::str::Utf8StringRef utf8Ref(const std::string& value) noexcept
{
    return ca::str::Utf8StringRef::from_data(
        value.empty() ? nullptr : reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

///produced_by 自述串：引擎 + 引擎版本（缓存失效键之一）。反编译器 jar 未
///自报版本时以解析后的 jar 路径兜底——换 jar 即换键（宁多算不脏读）。
std::string decompileCacheProducer(const detail::DecompilerOptions& options)
{
    const auto availability =
        inspectDecompiler(options.kind, options.decompilerJar, options.javaProgram);
    std::string version = availability.version;
    if (version.empty()) version = availability.decompilerJar;
    return std::string("mjt decompile ") + decompilerKindId(options.kind) + "/" + version;
}

///缓存查询输入（槽 + 类名 + 条目 digest）；inactive = 登记指纹缺失等退化态
///（缓存静默旁路，直接走反编译）。
struct DecompileCacheLookup final
{
    bool                                       active = false;
    mjbase::project::DecompileCacheSlot        slot;
    std::string                                className;
    ca::str::Utf8String                        entryDigest;
};

///构造缓存查询输入：登记 digest 缺失（不可预期的登记态）按旁路处理；类字节
///读不出（反编译自身稍后也会失败）按旁路处理，不抢在反编译前报错。
DecompileCacheLookup prepareDecompileCacheLookup(ProjectSession&    session,
                                                 const detail::DecompilerOptions& options,
                                                 const std::string& className)
{
    DecompileCacheLookup lookup;
    lookup.className = className;
    const auto digest = primaryFingerprint(session.projectDir);
    if (digest.empty() || !session.cacheStore) return lookup;
    auto bytes = session.workspace->readClassBytes(className);
    if (bytes.is_err()) return lookup;
    const auto& value = bytes.unwrap();
    const auto hash =
        mjbase::project::sectionContentHash(value.bytes.data(), value.bytes.size());
    lookup.entryDigest = hash.clone();

    lookup.slot.containerDigest = mjbase::project::copyLedgerText(digest);
    lookup.slot.containerPath   = mjbase::project::copyLedgerText(session.workspace->sourcePath());
    lookup.slot.producer        = mjbase::project::copyLedgerText(decompileCacheProducer(options));
    //options profile：当前工具面的反编译参数里没有影响产物文本的旋钮
    //（timeout/内存/返回上限只影响执行与截断），profile 恒空串（≡ 未设置，
    //R52/CP-3 归一口径）；引入内容性参数时在此拼接。
    lookup.active = true;
    return lookup;
}

///pending 镜像键（slot 名+类名+digest；查询与缓冲共用同一构造）。
std::string decompileCacheMirrorKey(const DecompileCacheLookup& lookup)
{
    std::ostringstream key;
    key << decompileCacheSectionName(lookup.slot) << '\n' << lookup.className << '\n'
        << lookup.entryDigest.to_std_string();
    return key.str();
}

///缓存查询：先查 pending 镜像（put 驻内存期可见），再查 bundle 已 flush 的
///section（跨会话）。命中返回源码文本。命中/未命中计数照记；查询错误按
///fail-closed 上浮（清单不可载 = Io 错，读路径报错是缓存引擎的明示纪律）。
ca::StatusOr<std::string> lookupDecompileCache(ProjectSession& session,
                                               const DecompileCacheLookup& lookup)
{
    if (!lookup.active) return ca::Ok(std::string{});
    auto& pending = *session.cachePending;
    std::lock_guard<std::mutex> lock(pending.mutex);
    const auto mirrorKey = decompileCacheMirrorKey(lookup);
    const auto found = pending.sources.find(mirrorKey);
    if (found != pending.sources.end()) {
        ++session.cacheHits;
        return ca::Ok(found->second);
    }
    auto hit = session.cacheStore->lookupDecompile(lookup.slot,
                                                   lookup.className,
                                                   lookup.entryDigest.ref());
    if (hit.is_err())
        return ca::Err(projectStoreStatus(std::move(hit).unwrap_err(),
                                          "lookup decompile cache"));
    auto section = std::move(hit).unwrap();
    if (section.has_value()) {
        ++session.cacheHits;
        return ca::Ok(section->to_std_string());
    }
    ++session.cacheMisses;
    return ca::Ok(std::string{});
}

///反编译成功后的缓存缓冲（pending 驻内存 + 镜像同步可见；flush 批量落盘并
///退役同族陈旧槽）。写不进即本次不缓存（缓存可重算，永不值得为它报错；
///async worker 面无错误通道，同一自愈语义）。
void putDecompileCacheShared(const std::shared_ptr<mjbase::project::AnalysisCacheStore>& store,
                             ProjectSession::CachePendingMirror&                         pending,
                             const DecompileCacheLookup&                                 lookup,
                             std::string_view                                            source)
{
    if (!store || !lookup.active) return;
    std::lock_guard<std::mutex> lock(pending.mutex);
    (void)store->putDecompile(lookup.slot,
                              lookup.className,
                              lookup.entryDigest.ref(),
                              source);
    pending.sources[decompileCacheMirrorKey(lookup)] = std::string(source);
}

void putDecompileCache(ProjectSession&             session,
                       const DecompileCacheLookup& lookup,
                       std::string_view            source)
{
    putDecompileCacheShared(session.cacheStore, *session.cachePending, lookup, source);
}

///缓存命中源码的返回上限复核（与 readDecompilerSource 同一口径）：返回上限
///不进缓存键（不影响产物文本），截断纪律在返回面复核——超限报错与首次
///反编译逐字一致。
ca::Status checkCachedSourceLimit(const detail::DecompilerOptions& options,
                                  const std::string&               source)
{
    if (source.size() > options.maxSourceBytes)
        return ca::ErrStatus(ca::StatusCode::RESOURCE_EXHAUSTED,
                             "反编译源码超过返回上限 " +
                                 std::to_string(options.maxSourceBytes) + " bytes");
    return ca::OkStatus();
}

}   //namespace

std::optional<Json> McpToolService::callDecompilerTool(
    const std::string& name, const Json& arguments, ProjectSession& session)
{
    IWorkspace* current = session.workspace.get();
    if (name == "decompile") {
        //decompile 的执行体（action=decompile；poll/cancel/release 是 session
        //工具，见 session.cpp）：class 或方法三元组二选一寻址。
        const auto action = arguments.value("action", std::string{"decompile"});
        if (action != "decompile") return toolError("unknown decompile action: " + action);
        const bool byMethod = arguments.contains("owner") || arguments.contains("name") ||
                              arguments.contains("descriptor");
        DecompilerOptions options;
        {
            //decompilerSettings_ 默认值由服务持有；执行体不持状态锁，此处短持读取。
            std::lock_guard<std::mutex> lock(mutex_);
            const auto optionsStatus = parseDecompilerOptions(arguments, decompilerSettings_, options);
            if (optionsStatus.is_err()) return toolError("invalid decompiler options", &optionsStatus);
        }

        //name 槽：class/owner/name 同查询面口径——Ref 全量解析，
        //raw 全量核表（反编译目标必须是 jar 内已定义类，核表零漂移）。
        std::string className;
        Json        slotError;
        if (byMethod) {
            const auto slots = slot::loadWorkspaceShadowTable(*current);
            std::string owner;
            std::string member;
            std::string descriptor;
            if (!slot::resolveMethodSlots(slots, arguments, true, owner, member, descriptor, slotError))
                return slotError;
            auto methodInfo = current->methodInfo(owner, member, descriptor);
            if (methodInfo.is_err()) {
                const auto err = shared::toCaStatus(methodInfo.unwrap_err());
                return toolError("get method source failed", &err);
            }
            className = owner;
        }
        else if (!slot::resolveClassArgument(*current, arguments, "class", true, className, slotError)) {
            return slotError;
        }
        if (className.empty()) return toolError("class is required");

        //条目级反编译缓存查询（RFC 0001 P3）：命中直接返回（响应标注 cached）；
        //未命中反编译成功后缓冲（pending，flush 挂点与 flushLedger 同点）。
        auto lookup = prepareDecompileCacheLookup(session, options, className);
        auto cached = lookupDecompileCache(session, lookup);
        if (cached.is_err())
            { const auto err = cached.unwrap_err(); return toolError("lookup decompile cache failed", &err); }
        const std::string cachedSource = std::move(cached).unwrap();

        if (arguments.value("async", false)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (jobs_.size() >= kMaximumRetainedJobs)
                    return toolError("too many retained jobs; release completed jobs before starting another");
            }
            auto classInfo = current->classInfo(className);
            if (classInfo.is_err()) {
                const auto err = shared::toCaStatus(classInfo.unwrap_err());
                return toolError("get decompile class failed", &err);
            }
            const auto internalName = classInfo.unwrap().internalName;

            auto temporary = createDecompilerTemporaryDirectory();
            if (temporary.is_err()) { const auto err = temporary.unwrap_err(); return toolError("create decompiler snapshot failed", &err); }
            const auto snapshotDirectory = std::move(temporary).unwrap();
            const auto snapshot = snapshotDirectory / "workspace.jar";
            const auto exportStatus = current->exportJar(snapshot.string());
            if (exportStatus.is_err()) {
                std::error_code cleanupError;
                fs::remove_all(snapshotDirectory, cleanupError);
                const auto err = shared::toCaStatus(exportStatus.unwrap_err());
                return toolError("snapshot decompiler workspace failed", &err);
            }

            auto job = std::make_shared<JobState>();
            job->projectId      = arguments.value("project_id", std::string{});
            job->kind           = "decompile";
            job->sourceRevision = current->revision();
            {
                //计数器与 jobs_ 在状态锁保护下（M1 修复后执行体不再持有状态锁，
                //此处短持）。
                std::lock_guard<std::mutex> lock(mutex_);
                job->id = nextJobId();
                jobs_.emplace(job->id, job);
            }

            try {
                job->worker = std::thread([job,
                                           snapshotDirectory,
                                           snapshot,
                                           internalName,
                                           options,
                                           lookup = std::move(lookup),
                                           cachedSource,
                                           cacheStore  = session.cacheStore,
                                           cachePending = session.cachePending] {
                    ScopedTemporaryDirectory directory(snapshotDirectory);
                    const auto finish = [&](const std::string& state, Json result, bool isError) {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->state         = state;
                        job->result        = std::move(result);
                        job->resultIsError = isError;
                    };
                    try {
                        const auto cancelled = [&] {
                            return ca::ErrStatus(ca::StatusCode::CANCELLED,
                                                 "decompile job cancelled");
                        };
                        {
                            std::lock_guard<std::mutex> lock(job->mutex);
                            if (job->cancellationRequested.load()) {
                                const auto status = cancelled();
                                job->state = "cancelled";
                                job->result = {{"error", "decompiler job cancelled"},
                                               {"status", statusJson(status)}};
                                job->resultIsError = true;
                                return;
                            }
                            job->state = "running";
                        }
                        const auto cancellationRequested = [job] {
                            return job->cancellationRequested.load();
                        };
                        const auto fail = [&](const std::string& operation, const ca::Status& status) {
                            //N2 async 超时侧车：DEADLINE_EXCEEDED（反编译子进程超时，
                            //全仓唯一产出点 mjt/src/shared/decompiler.cpp）与同步路径
                            //normalizeToolError 同口径补 partial 元数据，job_get 消费者
                            //可据此区分「超时失败」与「即时业务错误」。仅超时路径写入，
                            //其余失败与取消零变化；received/total/cursor 对超时无意义，
                            //按 P2-3 侧车 schema 同口径省略。
                            Json payload = {{"error", operation}, {"status", statusJson(status)}};
                            if (status.code() == ca::StatusCode::DEADLINE_EXCEEDED) {
                                payload["partial"] = true;
                                payload["reason"]  = "timeout";
                            }
                            finish(status.code() == ca::StatusCode::CANCELLED ? "cancelled" : "failed",
                                   std::move(payload),
                                   true);
                        };

                        //缓存命中短路：作业契约面零变化（仍走 poll 取结果），
                        //worker 直接以缓存源收敛，不再拉起反编译器子进程。
                        if (!cachedSource.empty()) {
                            const auto limit = checkCachedSourceLimit(options, cachedSource);
                            if (limit.is_err()) {
                                fail("decompile class failed", limit);
                                return;
                            }
                            detail::DecompiledSource result;
                            result.internalName   = internalName;
                            result.backend        = decompilerKindId(options.kind);
                            result.decompilerJar  = options.decompilerJar;
                            result.source         = cachedSource;
                            finish("succeeded", decompiledSourceJson(result, "class", true), false);
                            return;
                        }

                        auto source = decompileSnapshotClass(snapshot,
                                                             internalName,
                                                             options,
                                                             cancellationRequested);
                        if (source.is_err()) {
                            fail("decompile class failed", source.unwrap_err());
                            return;
                        }
                        if (cancellationRequested()) {
                            const auto status = cancelled();
                            fail("decompiler job cancelled", status);
                            return;
                        }
                        //反编译成功 → 缓存缓冲（pending；flush 挂点在 close/export）。
                        //缓存错误不推翻已成功的产物（缓存可重算，永不值得为它报
                        //错——写不进即本次不缓存，下次重算自愈）。
                        putDecompileCacheShared(cacheStore, *cachePending, lookup,
                                                source.unwrap().source);
                        finish("succeeded", decompiledSourceJson(source.unwrap(), "class"), false);
                    }
                    catch (const std::exception& exception) {
                        finish("failed", {{"error", exception.what()}}, true);
                    }
                    catch (...) {
                        finish("failed", {{"error", "unknown decompiler job failure"}}, true);
                    }
                });
            }
            catch (const std::exception& exception) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    jobs_.erase(job->id);
                }
                std::error_code cleanupError;
                fs::remove_all(snapshotDirectory, cleanupError);
                return toolError("start decompiler job failed: " + std::string(exception.what()));
            }
            return toolResult(jobStateJson(job));
        }

        //缓存命中：直接以缓存源构造响应（不再拉起反编译器子进程）。返回上限
        //在返回面复核（不进缓存键，截断纪律与首次反编译一致）。
        if (!cachedSource.empty()) {
            const auto limit = checkCachedSourceLimit(options, cachedSource);
            if (limit.is_err())
                return toolError("decompile class failed", &limit);
            auto info = current->classInfo(className);
            if (info.is_err())
                { const auto err = shared::toCaStatus(info.unwrap_err()); return toolError("get decompile class failed", &err); }
            detail::DecompiledSource result;
            result.internalName  = info.unwrap().internalName;
            result.backend       = decompilerKindId(options.kind);
            result.decompilerJar = options.decompilerJar;
            result.source        = cachedSource;
            return toolResult(decompiledSourceJson(result, "class", true));
        }

        auto source = decompileWorkspaceClass(*current, className, options);
        if (source.is_err()) { const auto err = source.unwrap_err(); return toolError("decompile class failed", &err); }
        //反编译成功 → 缓存缓冲（pending；写不进即本次不缓存，自愈语义同 async）。
        putDecompileCache(session, lookup, source.unwrap().source);
        return toolResult(decompiledSourceJson(source.unwrap(), "class"));
    }

    return std::nullopt;
}

}   //namespace mjt::mcp
