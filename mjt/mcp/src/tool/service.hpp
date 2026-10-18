#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "mjbase/workspace/i_workspace.hpp"
#include "shared/jar_workspace.hpp"

namespace mjt::mcp {

using Json = nlohmann::json;

struct JobState;

///@brief 单个 MCP session 使用的外部反编译器默认值。
struct DecompilerSessionSettings final
{
    ///@brief 未被单次调用覆盖时使用的 backend ID。
    std::string backend = "cfr";
    ///@brief CFR JAR；空值继续使用共享 discovery。
    std::string cfrJar;
    ///@brief Vineflower JAR；空值继续使用共享 discovery。
    std::string vineflowerJar;
    ///@brief Java 程序；空值继续使用共享 discovery。
    std::string javaProgram;
    ///@brief 子进程超时毫秒数。
    ca::i64     timeoutMilliseconds = 120000;
    ///@brief JVM 最大堆内存 MiB。
    ca::i32     maxMemoryMiB = 1024;
    ///@brief 单项返回源码的 byte 上限。
    ca::usize   maxSourceBytes = 2 * 1024 * 1024;
};

///@brief 直接适配 mbase Workspace 的 MCP 工具业务服务。
///
///所有工作区和异步任务由本对象拥有，并按 `workspace_id` 隔离。该类型只提供工具目录和
///CallToolResult，不解析 JSON-RPC，也不持有 transport/session 状态。一个 HTTP session 必须
///使用一个独立实例；stdio 使用单个实例。该类型不执行 mjt CLI，也不持有 ASM/Jasm 内部节点。
///
///## 锁模型（M1 并发风险修复）
///
///两把锁按固定顺序获取：`executionMutex_` → `mutex_`，避免死锁。
///
///- `mutex_`（状态锁）：保护 `workspaces_` / `jobs_` / 计数器 / `decompilerSettings_`。
///session 工具（job_get / job_cancel / job_release、workspace_open / workspace_close、
///decompiler_settings_*、pass_describe、analysis_evaluate）只持这把锁，临界区很短，
///不会被正在执行的长工具阻塞。
///- `executionMutex_`（执行锁）：同一 session 内非 session 工具的执行体串行化，
///避免并发访问同一个 IWorkspace。`call` 先取执行锁，再短持状态锁解析 workspace
///的 `shared_ptr`，随后在**不持状态锁**的情况下运行工具体——长工具（同步反编译、
///analysis_workflow）执行期间 session 工具与 job worker 的 commit 不被阻塞。
///- `workspaces_` 持有 `shared_ptr<IWorkspace>`：工具执行期间 workspace_close 只是
///从表中摘除，执行中的工具靠局部 shared_ptr 保活，安全完成。
///- job worker 的 commit 阶段按 `executionMutex_` → `mutex_` 顺序取锁，与工具执行体
///串行；worker 置 terminal 状态（`finish`）后才返回，因此 job_release 在 terminal
///时 join 立即返回，不会与 commit 竞争。
class McpToolService final
{
public:
    McpToolService() = default;
    ~McpToolService();

    ///@brief 返回可注册到 libmcp ToolRegistry 的完整工具 descriptor 数组。
    Json tools() const;

    ///@brief 执行一个工具并返回 MCP CallToolResult。
    ///@note 本方法线程安全；工具业务错误通过 `isError=true` 返回。
    Json call(const std::string& name, const Json& arguments);

private:
    Json callLocked(const std::string& name,
                    const Json& arguments,
                    mjbase::workspace::IWorkspace* current);
    std::optional<Json> callSessionTool(const std::string& name, const Json& arguments);
    std::optional<Json> callWorkspaceQueryTool(
        const std::string& name,
        const Json& arguments,
        mjbase::workspace::IWorkspace* current);
    std::optional<Json> callDecompilerTool(
        const std::string& name,
        const Json& arguments,
        mjbase::workspace::IWorkspace* current);
    std::optional<Json> callMutationTool(
        const std::string& name,
        const Json& arguments,
        mjbase::workspace::IWorkspace* current);

    std::shared_ptr<mjbase::workspace::IWorkspace> workspace(const Json& arguments,
                                                                    Json& error);
    std::string nextWorkspaceId();
    std::string nextJobId();

    ///@brief 状态锁：workspaces_ / jobs_ / 计数器 / decompilerSettings_。
    std::mutex mutex_;
    ///@brief 执行锁：同一 session 非 session 工具的执行体串行化。
    std::mutex executionMutex_;
    std::unordered_map<std::string,
                       std::shared_ptr<mjbase::workspace::IWorkspace>>
        workspaces_;
    std::unordered_map<std::string, std::shared_ptr<JobState>> jobs_;
    ca::u64 nextWorkspace_ = 1;
    ca::u64 nextJob_ = 1;
    DecompilerSessionSettings decompilerSettings_;
};

}   //namespace mjt::mcp
