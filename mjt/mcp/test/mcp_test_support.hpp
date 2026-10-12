#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <libca/http/client.hpp>
#include <libca/http/url.hpp>

#include <mcp/json_rpc.hpp>

#include "mcp_server.hpp"
#include "tool/service.hpp"
#include "test_helper.hpp"

namespace mjt::mcp::test_support {

ca::str::Utf16String utf16(const std::string& value);
std::string encodeBase64(const std::vector<ca::u8>& value);
std::string classSourceFor(const std::string& name, const std::string& value);
std::string classSource(const std::string& value);
std::vector<ca::u8> withClassNameAndSignature(const std::vector<ca::u8>& bytes,
                                               const std::string&         internalName,
                                               const std::string&         signature);
std::filesystem::path writeLargeClassListFixture(const std::string& signature);
std::filesystem::path writeFixture();
std::filesystem::path writeQuerySupportingFixture();
std::vector<ca::u8> assembleFixtureClass(const std::string& sourceName,
                                         const std::string& source);
std::filesystem::path writeParityFixture();

///长名 fixture：count 个类、内部名总长 totalLength 的合法 jar
///（另有普通名对照类 mcp/Normal；totalLength 取 0 时全部普通名作基线）。
///长名类经 ClassWriter 重写内部名生成。
std::string longInternalNameWithSuffix(std::size_t totalLength, const std::string& suffix);
std::string longInternalName(std::size_t totalLength);
std::filesystem::path writeLongNameFixture(std::size_t totalLength);
std::filesystem::path writeBulkLongNameFixture(std::size_t count, std::size_t totalLength);

Json request(const std::string& method, Json params = Json::object(), int id = 1);
Json notification(const std::string& method, Json params = Json::object());

class McpSessionHarness final
{
public:
    McpSessionHarness();

    Json handle(const Json& value);
    const Json& initializeResponse() const noexcept;

private:
    McpServer           server_;
    ::mcp::ServerSession session_;
    Json                initializeResponse_;
};

const Json& payload(const Json& response);
Json call(McpSessionHarness& server, const std::string& name, Json arguments, int id = 1);
Json waitForJobState(McpSessionHarness& server,
                     const std::string& jobId,
                     const std::string& expectedState,
                     int attempts = 300);
Json waitForTerminalJob(McpSessionHarness& server, const std::string& jobId, int attempts = 300);
void releaseJob(McpSessionHarness& server, const std::string& jobId);
std::vector<std::filesystem::path> workflowTemporaryArtifacts();
void expectNoNewWorkflowTemporaryArtifacts(const std::vector<std::filesystem::path>& before);
void expectDirectToolError(const Json& result,
                           const std::string& operation,
                           ca::StatusCode expectedCode,
                           std::string_view messagePart);

ca::http::HttpRequest mcpPost(const Json& value,
                              const std::string& sessionId = {},
                              std::string_view accept = "application/json, text/event-stream",
                              const std::string& authorization = {},
                              const std::string& origin = {});
ca::http::HttpRequest mcpSessionRequest(const std::string& method,
                                        const std::string& sessionId,
                                        const std::string& authorization = {});
ca::http::HttpClient makeHttpClient();
ca::http::HttpResponse sendHttp(ca::http::HttpClient& client,
                                const ca::http::HttpUrl& url,
                                ca::http::HttpRequest requestValue);

struct SseEvent final
{
    std::string id;
    std::string data;
};

std::vector<SseEvent> parseSse(const ca::http::HttpResponse& response);
Json httpJson(const ca::http::HttpResponse& response);
Json initializeHttpRequest(int id);

class HttpServerRunner final
{
public:
    explicit HttpServerRunner(HttpOptions options = HttpOptions());
    ~HttpServerRunner();

    bool ready() const noexcept;
    ca::http::HttpUrl url() const;

private:
    McpServer       server_;
    std::atomic<int> result_{-1};
    std::thread     thread_;
};

}   //namespace mjt::mcp::test_support

namespace mjt::mcp {

int runFakeDecompilerProcess(int argc, char** argv);

}   //namespace mjt::mcp
