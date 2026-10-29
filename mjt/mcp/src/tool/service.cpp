#include "service.hpp"

#include "contract.hpp"
#include "job.hpp"
#include "json.hpp"
#include "limits.hpp"

#include <chrono>
#include <exception>
#include <utility>

namespace mjt::mcp {

using namespace detail;

using mjt::workspace::IWorkspace;

std::string McpToolService::nextWorkspaceId()
{
    return "project-" + std::to_string(nextWorkspace_++);
}

McpToolService::~McpToolService()
{
    for (const auto& [_, job] : jobs_) job->cancellationRequested.store(true);
    for (const auto& [_, job] : jobs_) {
        if (job->worker.joinable()) job->worker.join();
    }
    //超时转孤儿的执行线程：与 job worker 同纪律，析构面统一 join——孤儿持有的
    //executionMutex_ 与会话 shared_ptr 随线程退出自然释放。
    for (const auto& orphan : orphans_) {
        if (!orphan->joined && orphan->worker.joinable()) orphan->worker.join();
    }
}

std::string McpToolService::nextJobId()
{
    return "job-" + std::to_string(nextJob_++);
}

std::shared_ptr<ProjectSession> McpToolService::session(const Json& arguments, Json& error)
{
    const auto id = arguments.contains("project_id") && arguments.at("project_id").is_string()
                        ? arguments.at("project_id").get<std::string>()
                        : std::string{};
    if (id.empty()) {
        error = toolError("project_id is required");
        return nullptr;
    }
    const auto found = sessions_.find(id);
    if (found == sessions_.end()) {
        const auto status =
            ca::ErrStatus(ca::StatusCode::NOT_FOUND, "project not found: " + id);
        error = toolError("project not found: " + id, &status);
        return nullptr;
    }
    return found->second;
}

Json McpToolService::tools() const
{
    return toolsList();
}

Json McpToolService::call(const std::string& name, const Json& arguments)
{
    std::shared_ptr<ProjectSession> current;
    try {
        //注册面即白名单：未注册名（含已退役的旧工具名）一律 "unknown tool"，
        //不进入参数校验与会话解析（P4 收敛后无别名过渡期）。
        if (!isRegisteredTool(name)) {
            return normalizeToolError(name, arguments, toolError("unknown tool: " + name), nullptr);
        }
        //参数校验先于分派：session 工具与 workspace 工具统一按契约 schema 校验
        //（保持 v2.1 既有的错误语义，如 "is not an allowed property"）。
        std::string validationError;
        if (!validateToolArguments(name, arguments, validationError)) {
            const auto status = ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                                              "validate tool arguments: " + validationError);
            return normalizeToolError(name,
                                      arguments,
                                      toolError(validationError, &status),
                                      nullptr);
        }

        //工具级超时面（#25）：project_open / class_query / search / xref_query
        //执行体放临时 worker，主线程限时等待。超时返回结构化错误（reason=timeout
        //+ operation/project_id/elapsed_ms），worker 转孤儿执行至自然完成——查询
        //体在孤儿内自行取执行锁，后续工具照常按锁序排队，stdio 通道与其余工具
        //（session 面 / 作业面）不受阻塞。project_open 的 id 在预订期生成：超时
        //错误可携带，孤儿完成后会话照常可用。
        if (name == "project_open" || name == "class_query" || name == "search" ||
            name == "xref_query") {
            std::shared_ptr<ProjectSession> current;
            std::string                     reservedId;
            if (name == "project_open") {
                std::lock_guard<std::mutex> lock(mutex_);
                //限额与显式 id 冲突在预订期快速失败（执行前的纯状态检查，不付
                //worker 成本）；孤儿完成后的正式 emplace 在状态锁下再复查一次。
                if (arguments.value("path", std::string{}).empty())
                    return normalizeToolError(name, arguments, toolError("path is required"), nullptr);
                if (sessions_.size() >= kMaximumRetainedWorkspaces)
                    return normalizeToolError(name,
                                              arguments,
                                              toolError("too many open projects (limit " +
                                                        std::to_string(kMaximumRetainedWorkspaces) +
                                                        "); close one first"),
                                              nullptr);
                reservedId = arguments.value("project_id", std::string{});
                if (reservedId.empty()) reservedId = nextWorkspaceId();
                if (sessions_.find(reservedId) != sessions_.end())
                    return normalizeToolError(
                        name, arguments, toolError("project already exists: " + reservedId), nullptr);
            }
            else {
                std::lock_guard<std::mutex> lock(mutex_);
                Json                        error;
                current = session(arguments, error);
                if (!current) return normalizeToolError(name, arguments, std::move(error), nullptr);
            }
            bool timedOut = false;
            Json result;
            if (name == "project_open") {
                result = callWithDeadline(
                    name,
                    arguments,
                    nullptr,
                    reservedId,
                    [this, arguments, reservedId] {
                        return openProjectBody(arguments, reservedId);
                    },
                    timedOut);
            }
            else {
                result = callWithDeadline(
                    name,
                    arguments,
                    current,
                    std::string{},
                    [this, name, arguments, current]() -> Json {
                        std::lock_guard<std::mutex> executionLock(executionMutex_);
                        return callLocked(name, arguments, *current);
                    },
                    timedOut);
            }
            //超时面不外透 workspace 元数据：workspace 正被孤儿改写，读 revision
            //是数据竞争；normalize 对 workspace=nullptr 记 revision=0。
            return normalizeToolError(name,
                                      arguments,
                                      std::move(result),
                                      timedOut ? nullptr
                                               : (current ? current->workspace.get() : nullptr));
        }

        //session 工具只访问共享状态（sessions_ / jobs_ / settings / 计数器），只持状态锁，
        //临界区很短，不会被正在执行的长工具阻塞（M1 并发风险修复）。
        //返回结果与 workspace 工具一样统一经 normalizeToolError 补充 operation 等字段。
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (auto result = callSessionTool(name, arguments))
                return normalizeToolError(name, arguments, std::move(*result), nullptr);
        }

        //其余工具：执行体在 executionMutex_ 下串行（同一 session 内工具不会并发访问同一个
        //IWorkspace），仅短持状态锁解析会话的 shared_ptr；工具执行期间不持状态锁，
        //同步反编译等长工具不再阻塞 job_get/job_cancel 与 job worker 的 commit。
        std::lock_guard<std::mutex> executionLock(executionMutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Json error;
            current = session(arguments, error);
            if (!current) return normalizeToolError(name, arguments, std::move(error), nullptr);
        }
        return normalizeToolError(
            name, arguments, callLocked(name, arguments, *current), current->workspace.get());
    }
    catch (const Json::exception& exception) {
        const auto status = ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                                          "decode tool arguments: " + std::string(exception.what()));
        return normalizeToolError(
            name, arguments, toolError("invalid arguments", &status), current ? current->workspace.get() : nullptr);
    }
    catch (const std::exception& exception) {
        const auto status = ca::ErrStatus(ca::StatusCode::INTERNAL,
                                          "execute tool: " + std::string(exception.what()));
        return normalizeToolError(name, arguments, toolError("tool failed", &status),
                                  current ? current->workspace.get() : nullptr);
    }
}

Json McpToolService::callWithDeadline(const std::string&       name,
                                      const Json&              arguments,
                                      std::shared_ptr<ProjectSession> keepalive,
                                      const std::string&       reservedProjectId,
                                      std::function<Json()>    body,
                                      bool&                    timedOut)
{
    const auto timeoutMs =
        arguments.value("timeout_ms", detail::kDefaultToolTimeoutMilliseconds);
    const auto started = std::chrono::steady_clock::now();

    //worker 与主线程经共享槽交接结果；runner 由双方 shared_ptr 持有——超时侧
    //登记进 orphans_（析构 join），完成侧由 worker 自摘除后主线程 join 收尾。
    auto slot   = std::make_shared<DeadlineSlot>();
    auto runner = std::make_shared<OrphanRun>();
    runner->worker = std::thread(
        [this, slot, runner, body = std::move(body), keepalive]() mutable {
            Json result;
            try {
                result = body();
            }
            catch (const Json::exception& exception) {
                const auto status = ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                                                  "decode tool arguments: " +
                                                      std::string(exception.what()));
                result = toolError("invalid arguments", &status);
            }
            catch (const std::exception& exception) {
                const auto status = ca::ErrStatus(ca::StatusCode::INTERNAL,
                                                  "execute tool: " +
                                                      std::string(exception.what()));
                result = toolError("tool failed", &status);
            }
            {
                std::lock_guard<std::mutex> guard(slot->mutex);
                slot->result = std::move(result);
                slot->done   = true;
            }
            slot->cv.notify_all();
            //不做自摘除：lambda 末尾若释放最后一个 shared_ptr，线程仍在收尾时
            //joinable 的 std::thread 就被析构（terminate）。退出标志留给登记侧
            //惰性回收；此刻 body 的执行锁（若有）已释放。
            runner->exited.store(true);
        });

    Json result;
    {
        std::unique_lock<std::mutex> guard(slot->mutex);
        const bool finished = slot->cv.wait_for(guard,
                                                std::chrono::milliseconds(timeoutMs),
                                                [&] { return slot->done; });
        if (finished) {
            result = std::move(slot->result);
            guard.unlock();
            timedOut = false;
            //完成路径 join：runner 仍在 orphans_（无人自摘除），同步置 joined
            //避免析构/惰性回收二次 join（joinable 已为 false，双保险仅冗余）。
            if (runner->worker.joinable()) runner->worker.join();
            runner->joined = true;
            return result;
        }
    }

    timedOut = true;
    {
        std::lock_guard<std::mutex> stateLock(mutex_);
        orphans_.push_back(std::move(runner));
        //惰性回收：join 已退出的孤儿条目（join 对结束线程立即返回）。主线程
        //此刻不持执行锁，worker 收尾不依赖任何锁，join 阻塞仅剩线程拆除开销。
        for (auto& orphan : orphans_) {
            if (!orphan->joined && orphan->exited.load()) {
                if (orphan->worker.joinable()) orphan->worker.join();
                orphan->joined = true;
            }
        }
    }
    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                              started)
            .count();
    //结构化超时错误：status 取 DEADLINE_EXCEEDED（normalizeToolError 的超时侧车
    //据此自动补 partial/reason 字段），operation/project_id 由同一处统一补齐；
    //project_open 的预订 id 直接写入 payload（arguments 里没有 project_id 可读，
    //normalize 对已带非空 id 的 payload 不回填）。
    const auto status =
        ca::ErrStatus(ca::StatusCode::DEADLINE_EXCEEDED,
                      "operation timed out after " + std::to_string(elapsedMs) +
                          " ms (timeout_ms=" + std::to_string(timeoutMs) + ")");
    Json payload = {{"error", "operation timed out"},
                    {"status", statusJson(status)},
                    {"elapsed_ms", elapsedMs},
                    {"timeout_ms", timeoutMs}};
    if (!reservedProjectId.empty()) payload["project_id"] = reservedProjectId;
    return toolResult(std::move(payload), true);
}

Json McpToolService::openProjectBody(const Json& arguments, const std::string& reservedId)
{
    const auto path = arguments.value("path", std::string{});
    std::shared_ptr<ProjectSession> instance;
    bool        created     = false;
    std::string fingerprint;
    const auto status = openProjectSession(path,
                                           arguments.value("project_dir", std::string{}),
                                           instance,
                                           created,
                                           fingerprint);
    if (status.is_err()) return toolError("open project failed", &status);
    //结果文档先于登记构建（实例字段尚是本地态）；登记在状态锁下做冲突复查——
    //预订与 emplace 之间可能有并发 open 用同一显式 id 抢先登记，落败方丢弃
    //本次登记（workspace 随局部 shared_ptr 析构，临时工程 bundle 一并回收）。
    Json result = {{"project_id", reservedId},
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
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!sessions_.emplace(reservedId, instance).second)
            return toolError("project already exists: " + reservedId);
    }
    return toolResult(std::move(result));
}

Json McpToolService::callLocked(const std::string& name, const Json& arguments, ProjectSession& session)
{
    if (auto result = callWorkspaceQueryTool(name, arguments, session)) return std::move(*result);
    if (auto result = callDecompilerTool(name, arguments, session)) return std::move(*result);
    if (auto result = callMutationTool(name, arguments, session)) return std::move(*result);
    return toolError("unknown tool: " + name);
}

}   //namespace mjt::mcp
