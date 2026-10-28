#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "mjbase/workspace/i_workspace.hpp"
#include "shared/jar_workspace.hpp"
#include "tool/project_session.hpp"

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
///所有分析会话（工程 bundle + workspace）与异步任务由本对象拥有，并按
///`project_id` 隔离。该类型只提供工具目录和
///CallToolResult，不解析 JSON-RPC，也不持有 transport/session 状态。一个 HTTP session 必须
///使用一个独立实例；stdio 使用单个实例。该类型不执行 mjt CLI，也不持有 ASM/Jasm 内部节点。
///
///## 锁模型（M1 并发风险修复）
///
///两把锁按固定顺序获取：`executionMutex_` → `mutex_`，避免死锁。
///
///- `mutex_`（状态锁）：保护 `sessions_` / `jobs_` / 计数器 / `decompilerSettings_`。
///session 工具（project_open / project_close、decompile 的 poll / cancel / release
///作业面）只持这把锁，临界区很短，不会被正在执行的长工具阻塞。
///- `executionMutex_`（执行锁）：同一 session 内非 session 工具的执行体串行化，
///避免并发访问同一个 IWorkspace。`call` 先取执行锁，再短持状态锁解析
///ProjectSession 的 `shared_ptr`，随后在**不持状态锁**的情况下运行工具体——长工具
///（同步反编译）执行期间 session 工具与 job worker 的 commit 不被阻塞。
///- `sessions_` 持有 `shared_ptr<ProjectSession>`：工具执行期间 project_close 只是
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
    ///@brief 限时等待槽（工具级超时面）：worker 写结果并唤醒，主线程限时等待。
    struct DeadlineSlot final
    {
        std::mutex              mutex;
        std::condition_variable cv;
        bool                    done = false;
        Json                    result;
    };

    ///@brief 超时后继续执行的孤儿 worker：析构面统一 join，不留悬空线程。
    ///
    ///线程绝不可自摘除（lambda 末尾释放最后一个 shared_ptr 会在线程仍存活时
    ///析构 joinable 的 std::thread → terminate）：worker 退出前置 exited 标志，
    ///登记侧惰性 join 已退出线程回收条目。
    struct OrphanRun final
    {
        std::thread              worker;
        std::atomic<bool>        exited{false};
        bool                     joined = false;
    };

    ///@brief 在临时 worker 上执行 body 并限时等待（工具级超时面）。
    ///
    ///超时返回结构化错误（reason=timeout / elapsed_ms / 预订 project_id），
    ///worker 转孤儿继续执行至自然完成——查询体在孤儿内自行取 executionMutex_，
    ///后续工具照常按锁序排队，stdio 通道与已打开会话不受阻塞。服务析构 join
    ///全部孤儿。timedOut 出参供调用方决定 normalize 是否外透 workspace 元数据
    ///（超时时 workspace 正被孤儿改写，一律不读）。
    Json callWithDeadline(const std::string& name, const Json& arguments,
                          std::shared_ptr<ProjectSession> keepalive,
                          const std::string& reservedProjectId,
                          std::function<Json()>           body,
                          bool&                           timedOut);

    ///@brief project_open 执行体（限时 worker 内运行）：打开 + 登记会话。
    ///@note id 由主线程预订（超时错误可携带）；emplace 时在状态锁下复查冲突。
    Json openProjectBody(const Json& arguments, const std::string& reservedId);

    Json callLocked(const std::string& name, const Json& arguments, ProjectSession& session);
    std::optional<Json> callSessionTool(const std::string& name, const Json& arguments);
    std::optional<Json> callWorkspaceQueryTool(
        const std::string& name,
        const Json& arguments,
        ProjectSession& session);
    std::optional<Json> callDecompilerTool(
        const std::string& name,
        const Json& arguments,
        ProjectSession& session);
    std::optional<Json> callMutationTool(
        const std::string& name,
        const Json& arguments,
        ProjectSession& session);

    std::shared_ptr<ProjectSession> session(const Json& arguments, Json& error);
    std::string nextWorkspaceId();
    std::string nextJobId();

    ///@brief 状态锁：sessions_ / jobs_ / 计数器 / decompilerSettings_。
    std::mutex mutex_;
    ///@brief 执行锁：同一 session 非 session 工具的执行体串行化。
    std::mutex executionMutex_;
    std::unordered_map<std::string, std::shared_ptr<ProjectSession>> sessions_;
    std::unordered_map<std::string, std::shared_ptr<JobState>> jobs_;
    ///@brief 超时转孤儿的执行线程表（worker 自摘除；析构面统一 join）。
    std::vector<std::shared_ptr<OrphanRun>> orphans_;
    ca::u64 nextWorkspace_ = 1;
    ca::u64 nextJob_ = 1;
    DecompilerSessionSettings decompilerSettings_;
};

}   //namespace mjt::mcp
