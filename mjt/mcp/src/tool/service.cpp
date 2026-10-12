#include "service.hpp"

#include "contract.hpp"
#include "job.hpp"
#include "json.hpp"

#include <exception>
#include <utility>

namespace mjt::mcp {

using namespace detail;

using mjt::base::workspace::IWorkspace;

std::string McpToolService::nextWorkspaceId()
{
    return "workspace-" + std::to_string(nextWorkspace_++);
}

McpToolService::~McpToolService()
{
    for (const auto& [_, job] : jobs_) job->cancellationRequested.store(true);
    for (const auto& [_, job] : jobs_) {
        if (job->worker.joinable()) job->worker.join();
    }
}

std::string McpToolService::nextJobId()
{
    return "job-" + std::to_string(nextJob_++);
}

std::shared_ptr<IWorkspace> McpToolService::workspace(const Json& arguments, Json& error)
{
    const auto id = arguments.value("workspace_id", std::string{});
    if (id.empty()) {
        error = toolError("workspace_id is required");
        return nullptr;
    }
    const auto found = workspaces_.find(id);
    if (found == workspaces_.end()) {
        const auto status = ca::ErrStatus(ca::StatusCode::NOT_FOUND,
                                          "workspace not found: " + id);
        error = toolError("workspace not found: " + id, &status);
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
    std::shared_ptr<IWorkspace> current;
    try {
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

        //session 工具只访问共享状态（jobs_ / workspaces_ / settings / 计数器），只持状态锁，
        //临界区很短，不会被正在执行的长工具阻塞（M1 并发风险修复）。
        //返回结果与 workspace 工具一样统一经 normalizeToolError 补充 operation 等字段。
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (auto result = callSessionTool(name, arguments))
                return normalizeToolError(name, arguments, std::move(*result), nullptr);
        }

        //其余工具：执行体在 executionMutex_ 下串行（同一 session 内工具不会并发访问同一个
        //IWorkspace），仅短持状态锁解析 workspace 的 shared_ptr；工具执行期间不持状态锁，
        //同步反编译等长工具不再阻塞 job_get/job_cancel 与 job worker 的 commit。
        std::lock_guard<std::mutex> executionLock(executionMutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            Json error;
            current = workspace(arguments, error);
            if (!current) return normalizeToolError(name, arguments, std::move(error), nullptr);
        }
        return normalizeToolError(
            name, arguments, callLocked(name, arguments, current.get()), current.get());
    }
    catch (const Json::exception& exception) {
        const auto status = ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                                          "decode tool arguments: " + std::string(exception.what()));
        return normalizeToolError(
            name, arguments, toolError("invalid arguments", &status), current.get());
    }
    catch (const std::exception& exception) {
        const auto status = ca::ErrStatus(ca::StatusCode::INTERNAL,
                                          "execute tool: " + std::string(exception.what()));
        return normalizeToolError(name, arguments, toolError("tool failed", &status), current.get());
    }
}

Json McpToolService::callLocked(const std::string& name, const Json& arguments, IWorkspace* current)
{
    if (auto result = callWorkspaceQueryTool(name, arguments, current)) return std::move(*result);
    if (auto result = callDecompilerTool(name, arguments, current)) return std::move(*result);
    if (auto result = callMutationTool(name, arguments, current)) return std::move(*result);
    return toolError("unknown tool: " + name);
}

}   //namespace mjt::mcp
