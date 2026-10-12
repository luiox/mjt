#pragma once

#include <atomic>
#include <istream>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <vector>

#include <mcp/server.hpp>

namespace mjt::mcp {

class McpToolService;
struct HttpRuntime;

///@brief mjt MCP Streamable HTTP 监听、安全与 Origin 配置。
struct HttpOptions final
{
    std::string              host = "127.0.0.1";
    int                      port = 8765;
    std::string              token;
    std::vector<std::string> allowedOrigins;
};

///@brief 将 mjt 工具业务适配到 libmcp session 与 transport。
///
///stdio 使用一个进程内工具服务；HTTP session factory 为每个 session 创建独立工具服务，
///从而隔离 workspace 与 job。JSON-RPC lifecycle、工具分发、Origin、session ID、SSE 与重放
///均由 libmcp 负责。
class McpServer final
{
public:
    McpServer();
    ~McpServer();

    McpServer(const McpServer&)            = delete;
    McpServer& operator=(const McpServer&) = delete;

    ///@brief 为当前 stdio/测试工具服务创建一个新的 libmcp session。
    ::mcp::McpResult<::mcp::ServerSession> createSession();

    ///@brief 通过 libmcp newline-delimited stdio transport 运行到 EOF。
    int runStdio(std::istream& input, std::ostream& output);

    ///@brief 在当前线程运行 libmcp Streamable HTTP endpoint，直到 stopHttp。
    ///@note port 为 0 时由系统分配端口，实际端口可通过 httpPort 查询。
    ///@warning 非 loopback 地址必须配置 bearer token。
    int runHttp(const HttpOptions& options);

    ///@brief 线程安全地停止当前 HTTP listener；未运行时无操作。
    void stopHttp() noexcept;

    ///@brief 返回当前 HTTP listener 的实际端口；未绑定时返回 0。
    int httpPort() const noexcept;

private:
    std::shared_ptr<McpToolService> toolService_;
    mutable std::mutex              httpMutex_;
    std::shared_ptr<HttpRuntime>    httpRuntime_;
    std::atomic<int>                httpPort_{0};
};

}   //namespace mjt::mcp
