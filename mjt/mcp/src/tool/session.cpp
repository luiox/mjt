#include "service.hpp"

#include <memory>
#include <mutex>
#include <utility>

#include "decompiler_support.hpp"
#include "job.hpp"
#include "json.hpp"
#include "shared/status_convert.hpp"
#include "limits.hpp"

namespace mjt::mcp {

using namespace detail;
using mjbase::workspace::IWorkspace;

Json jobStateJson(const std::shared_ptr<JobState>& job)
{
    std::lock_guard<std::mutex> lock(job->mutex);
    Json result = {{"job_id", job->id},
                   {"project_id", job->projectId},
                   {"kind", job->kind},
                   {"source_revision", job->sourceRevision},
                   {"state", job->state},
                   {"cancel_requested", job->cancellationRequested.load()}};
    if (!job->result.is_null()) {
        result["result"] = job->result;
        result["result_is_error"] = job->resultIsError;
    }
    return result;
}

std::optional<Json> McpToolService::callSessionTool(const std::string& name, const Json& arguments)
{
    //decompile 的作业面（poll/cancel/release）是 session 工具：jobs_ 只受
    //状态锁保护，与旧 job_get/job_cancel/job_release 同一锁纪律（release 仅在
    //terminal 后 join，worker 的 commit 锁序 executionMutex_ → mutex_ 不会与
    //此处持有的状态锁成环）。action=decompile 不在此处理，落执行体。
    if (name == "decompile") {
        const auto action = arguments.value("action", std::string{"decompile"});
        if (action == "decompile") return std::nullopt;
        if (action != "poll" && action != "cancel" && action != "release")
            return toolError("unknown decompile action: " + action);
        const auto id = arguments.value("job_id", std::string{});
        if (id.empty()) return toolError("job_id is required");
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) {
            const auto status = ca::ErrStatus(ca::StatusCode::NOT_FOUND,
                                              "job not found: " + id);
            return toolError("job not found: " + id, &status);
        }
        const auto job = found->second;
        //作业按 project 归属隔离：跨 project 的 job_id 一律按不存在处理，
        //避免横向探测其它 project 会话的作业。
        if (job->projectId != arguments.value("project_id", std::string{})) {
            const auto status = ca::ErrStatus(ca::StatusCode::NOT_FOUND,
                                              "job not found: " + id);
            return toolError("job not found: " + id, &status);
        }
        if (action == "poll") return toolResult(jobStateJson(job));

        std::unique_lock<std::mutex> lock(job->mutex);
        const bool terminal = job->state == "succeeded" || job->state == "failed" ||
                              job->state == "cancelled";
        if (action == "cancel") {
            if (job->state == "committing")
                return toolError("job is committing and can no longer be cancelled");
            if (!terminal) job->cancellationRequested.store(true);
            return toolResult({{"job_id", job->id},
                               {"state", job->state},
                               {"cancel_requested", job->cancellationRequested.load()},
                               {"terminal", terminal}});
        }
        if (!terminal) return toolError("running job must be cancelled or completed before release");
        const auto released = job->id;
        lock.unlock();
        //join 在持有 mutex_ 的 call 上下文内进行：worker 的 commit 阶段需要按
        //executionMutex_ → mutex_ 顺序取锁（见 service.hpp 锁模型），此处持状态锁等待
        //join 会与 commit 竞争。但 release 仅在 job 已 terminal（worker 的 finish 已
        //写完结果并即将返回）时才走到这里，join 立即返回，实际不阻塞。
        if (job->worker.joinable()) job->worker.join();
        jobs_.erase(found);
        return toolResult({{"job_id", released}, {"released", true}});
    }
    if (name == "project_open") {
        const auto path = arguments.value("path", std::string{});
        if (path.empty()) return toolError("path is required");
        //与 kMaximumRetainedJobs 对齐的资源上限：会话常驻内存（整个
        //ResourceContainer + 工程 bundle 锁），不设上限会让长会话无界增长。
        if (sessions_.size() >= detail::kMaximumRetainedWorkspaces)
            return toolError("too many open projects (limit " +
                             std::to_string(detail::kMaximumRetainedWorkspaces) +
                             "); close one first");
        auto id = arguments.value("project_id", std::string{});
        if (id.empty()) id = nextWorkspaceId();
        if (sessions_.find(id) != sessions_.end())
            return toolError("project already exists: " + id);
        std::shared_ptr<ProjectSession> instance;
        bool        created     = false;
        std::string fingerprint;
        const auto status = openProjectSession(path,
                                               arguments.value("project_dir", std::string{}),
                                               instance,
                                               created,
                                               fingerprint);
        if (status.is_err()) return toolError("open project failed", &status);
        sessions_.emplace(id, instance);
        //持久性自描述。temporary 工程在 project_close 即删 bundle，模型
        //需要显式提示才知道把成果留在盘上要传 project_dir 重开。
        Json result = {{"project_id", id},
                       {"revision", 0},
                       {"project_dir", instance->projectDir},
                       {"persistence", instance->temporary ? "temporary" : "project"},
                       {"created", created},
                       {"container", {{"role", "primary"},
                                      {"path", instance->workspace->sourcePath()},
                                      {"fingerprint", fingerprint}}}};
        if (instance->temporary)
            result["persistence_hint"] =
                "this project is temporary and its bundle is deleted on project_close; "
                "reopen with the same project_dir to persist analysis state and edits";
        return toolResult(std::move(result));
    }
    if (name == "project_close") {
        const auto id = arguments.value("project_id", std::string{});
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) {
            const auto status = ca::ErrStatus(ca::StatusCode::NOT_FOUND,
                                              "project not found: " + id);
            return toolError("project not found: " + id, &status);
        }
        //close/export 前的 flush 挂点：pending overlay 与台账事务落盘（幂等，
        //无 pending 时 no-op）；分析缓存合并 flush 同点执行——缓存与台账同一
        //崩溃窗语义。任一失败不关闭——数据安全优先于会话回收。
        const auto flushed = found->second->workspace->flushLedger();
        if (flushed.is_err()) {
            const auto err = shared::toCaStatus(flushed.unwrap_err());
            return toolError("flush ledger failed", &err);
        }
        const auto cacheFlushed = flushAnalysisCache(*found->second);
        if (cacheFlushed.is_err()) return toolError("flush analysis cache failed", &cacheFlushed);
        //临时工程随会话析构整体删除（ProjectSession::~ProjectSession 先释放
        //store.lock 再清目录）；显式工程目录原样保留在盘上。
        sessions_.erase(found);
        return toolResult({{"project_id", id}, {"closed", true}, {"flushed", true}});
    }
    return std::nullopt;
}

}   //namespace mjt::mcp
