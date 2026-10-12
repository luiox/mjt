#include <algorithm>
#include <chrono>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp::test_support {

Json request(const std::string& method, Json params, int id)
{
    return {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", std::move(params)}};
}

Json notification(const std::string& method, Json params)
{
    return {{"jsonrpc", "2.0"}, {"method", method}, {"params", std::move(params)}};
}

::mcp::ServerSession requireSession(McpServer& server)
{
    auto session = server.createSession();
    if (session.is_err()) throw std::runtime_error(session.unwrap_err().to_string());
    return std::move(session).unwrap();
}

McpSessionHarness::McpSessionHarness()
    : session_(requireSession(server_))
{
    initializeResponse_ = handle(
        request("initialize",
                {{"protocolVersion", "2025-11-25"},
                 {"capabilities", Json::object()},
                 {"clientInfo", {{"name", "mjt-test"}, {"version", "1"}}}}));
    const auto initialized = handle(notification("notifications/initialized"));
    if (!initialized.is_null())
        throw std::runtime_error("initialized notification returned a response");
}

Json McpSessionHarness::handle(const Json& value)
{
    const auto encoded = value.dump();
    auto message = ::mcp::JsonRpcMessage::parse(
        ca::str::Utf8StringRef::from_data(
            reinterpret_cast<const ca::u8*>(encoded.data()), encoded.size()));
    if (message.is_err()) throw std::runtime_error(message.unwrap_err().to_string());
    auto response = session_.handle(std::move(message).unwrap());
    if (response.is_err()) throw std::runtime_error(response.unwrap_err().to_string());
    auto output = std::move(response).unwrap();
    if (!output.has_value()) return Json();
    return Json::parse(output->serialize().to_std_string());
}

const Json& McpSessionHarness::initializeResponse() const noexcept
{
    return initializeResponse_;
}

const Json& payload(const Json& response)
{
    return response.at("result").at("structuredContent");
}

Json call(McpSessionHarness& server, const std::string& name, Json arguments, int id)
{
    return server.handle(request("tools/call", {{"name", name}, {"arguments", std::move(arguments)}}, id));
}

bool isTerminalJobState(const std::string& state)
{
    return state == "succeeded" || state == "failed" || state == "cancelled";
}

Json waitForJobState(McpSessionHarness& server, const std::string& jobId,
                     const std::string& expectedState, int attempts)
{
    Json current;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        current = call(server, "job_get", {{"job_id", jobId}});
        if (current.at("result").at("isError")) return current;
        if (payload(current).at("state") == expectedState) return current;
        if (isTerminalJobState(payload(current).at("state").get<std::string>())) return current;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return current;
}

Json waitForTerminalJob(McpSessionHarness& server, const std::string& jobId, int attempts)
{
    Json current;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        current = call(server, "job_get", {{"job_id", jobId}});
        if (current.at("result").at("isError")) return current;
        if (isTerminalJobState(payload(current).at("state").get<std::string>())) return current;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return current;
}

void releaseJob(McpSessionHarness& server, const std::string& jobId)
{
    const auto released = call(server, "job_release", {{"job_id", jobId}});
    if (released.at("result").at("isError") || !payload(released).at("released").get<bool>())
        throw std::runtime_error("failed to release completed MCP job");
}

std::vector<std::filesystem::path> workflowTemporaryArtifacts()
{
    std::vector<std::filesystem::path> result;
    std::error_code                    error;
    const auto root = std::filesystem::temp_directory_path(error);
    if (error) throw std::runtime_error("failed to resolve temporary directory");
    std::filesystem::directory_iterator iterator(root, error);
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const auto& entry = *iterator;
        const auto name = entry.path().filename().string();
        if ((name.rfind("mjt-mcp-decompile-", 0) == 0 && entry.is_directory(error)) ||
            (name.rfind("mjt-workflow-candidate-", 0) == 0 && entry.is_regular_file(error))) {
            result.push_back(entry.path());
        }
        iterator.increment(error);
    }
    if (error) throw std::runtime_error("failed to enumerate decompiler temporary directories");
    std::sort(result.begin(), result.end());
    return result;
}

void expectNoNewWorkflowTemporaryArtifacts(const std::vector<std::filesystem::path>& before)
{
    const auto after = workflowTemporaryArtifacts();
    for (const auto& path : after)
        EXPECT_NE(before.end(), std::find(before.begin(), before.end(), path)) << path.string();
}

void expectDirectToolError(const Json& result, const std::string& operation,
                           ca::StatusCode expectedCode, std::string_view messagePart)
{
    ASSERT_TRUE(result.at("isError").get<bool>()) << result.dump();
    const auto& details = result.at("structuredContent");
    ASSERT_TRUE(details.at("error").is_string());
    EXPECT_NE(std::string::npos,
              details.at("error").get_ref<const std::string&>().find(messagePart))
        << details.dump();
    EXPECT_EQ(operation, details.at("operation"));
    EXPECT_EQ(static_cast<int>(expectedCode), details.at("status_code"));
    ASSERT_TRUE(details.at("status").is_object());
    EXPECT_EQ(static_cast<int>(expectedCode), details.at("status").at("code"));
    EXPECT_FALSE(details.at("status").at("ok").get<bool>());
    EXPECT_TRUE(details.at("workspace_id").is_string());
    EXPECT_TRUE(details.at("revision").is_number_unsigned());
    EXPECT_TRUE(details.at("diagnostics").is_array());
    ASSERT_FALSE(result.at("content").empty());
    EXPECT_EQ(details.dump(), result.at("content").at(0).at("text"));
}

}   //namespace mjt::mcp::test_support
