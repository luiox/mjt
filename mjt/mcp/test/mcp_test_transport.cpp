#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "mcp_test_support.hpp"

namespace mjt::mcp::test_support {

ca::core::Bytes httpBody(std::string_view value)
{
    return ca::core::Bytes::copy_from_slice(
        reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

std::string httpBodyText(const ca::http::HttpResponse& response)
{
    return std::string(reinterpret_cast<const char*>(response.body.as_ptr()),
                       response.body.remaining());
}

ca::http::HttpRequest mcpPost(const Json& value,
                              const std::string& sessionId,
                              std::string_view accept,
                              const std::string& authorization,
                              const std::string& origin)
{
    ca::http::HttpRequest result;
    result.method = "POST";
    result.body   = httpBody(value.dump());
    if (result.headers.append("Content-Type", "application/json; charset=utf-8").is_err() ||
        result.headers.append("Accept", std::string(accept)).is_err())
        throw std::runtime_error("failed to create MCP HTTP headers");
    if (!sessionId.empty()) {
        if (result.headers.append("MCP-Session-Id", sessionId).is_err() ||
            result.headers.append("MCP-Protocol-Version", "2025-11-25").is_err())
            throw std::runtime_error("failed to create MCP session headers");
    }
    if (!authorization.empty() && result.headers.append("Authorization", authorization).is_err())
        throw std::runtime_error("failed to create MCP authorization header");
    if (!origin.empty() && result.headers.append("Origin", origin).is_err())
        throw std::runtime_error("failed to create MCP Origin header");
    return result;
}

ca::http::HttpRequest mcpSessionRequest(const std::string& method,
                                        const std::string& sessionId,
                                        const std::string& authorization)
{
    ca::http::HttpRequest result;
    result.method = method;
    if (result.headers.append("Accept", "text/event-stream").is_err() ||
        result.headers.append("MCP-Session-Id", sessionId).is_err() ||
        result.headers.append("MCP-Protocol-Version", "2025-11-25").is_err())
        throw std::runtime_error("failed to create MCP session request");
    if (!authorization.empty() && result.headers.append("Authorization", authorization).is_err())
        throw std::runtime_error("failed to create MCP authorization header");
    return result;
}

ca::http::HttpClient makeHttpClient()
{
    auto client = ca::http::HttpClient::create();
    if (client.is_err()) throw std::runtime_error(client.unwrap_err().to_string());
    return std::move(client).unwrap();
}

ca::http::HttpResponse sendHttp(ca::http::HttpClient& client,
                                const ca::http::HttpUrl& url,
                                ca::http::HttpRequest requestValue)
{
    auto response = client.request(url, std::move(requestValue));
    if (response.is_err()) throw std::runtime_error(response.unwrap_err().to_string());
    return std::move(response).unwrap();
}

std::vector<SseEvent> parseSse(const ca::http::HttpResponse& response)
{
    const auto encoded = httpBodyText(response);
    std::vector<SseEvent> result;
    ca::usize offset = 0;
    while (offset < encoded.size()) {
        const auto ending = encoded.find("\n\n", offset);
        if (ending == std::string::npos) throw std::runtime_error("unterminated SSE event");
        const auto block = std::string_view(encoded).substr(offset, ending - offset);
        SseEvent event;
        ca::usize lineStart = 0;
        while (lineStart <= block.size()) {
            const auto lineEnd = block.find('\n', lineStart);
            const auto line = block.substr(lineStart, lineEnd - lineStart);
            if (line.substr(0, 4) == "id: ") event.id = std::string(line.substr(4));
            else if (line == "data:") event.data.clear();
            else if (line.substr(0, 6) == "data: ") event.data = std::string(line.substr(6));
            if (lineEnd == std::string_view::npos) break;
            lineStart = lineEnd + 1;
        }
        result.push_back(std::move(event));
        offset = ending + 2;
    }
    return result;
}

Json httpJson(const ca::http::HttpResponse& response)
{
    const auto contentType = response.headers.get("Content-Type");
    if (contentType.has_value() && contentType->find("text/event-stream") == 0) {
        const auto events = parseSse(response);
        for (auto item = events.rbegin(); item != events.rend(); ++item) {
            if (!item->data.empty()) return Json::parse(item->data);
        }
        throw std::runtime_error("SSE response has no JSON event");
    }
    return Json::parse(httpBodyText(response));
}

Json initializeHttpRequest(int id)
{
    return request("initialize",
                   {{"protocolVersion", "2025-11-25"},
                    {"capabilities", Json::object()},
                    {"clientInfo", {{"name", "mjt-http-test"}, {"version", "1"}}}},
                   id);
}

HttpServerRunner::HttpServerRunner(HttpOptions options)
    : thread_([this, options = std::move(options)] {
        result_.store(server_.runHttp(options));
    })
{
    for (ca::usize attempt = 0; attempt < 500 && server_.httpPort() == 0 && result_.load() < 0;
         ++attempt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
}

HttpServerRunner::~HttpServerRunner()
{
    server_.stopHttp();
    if (thread_.joinable()) thread_.join();
}

bool HttpServerRunner::ready() const noexcept
{
    return server_.httpPort() != 0;
}

ca::http::HttpUrl HttpServerRunner::url() const
{
    auto parsed = ca::http::HttpUrl::parse(
        "http://127.0.0.1:" + std::to_string(server_.httpPort()) + "/mcp");
    if (parsed.is_err()) throw std::runtime_error(parsed.unwrap_err().to_string());
    return std::move(parsed).unwrap();
}


}   //namespace mjt::mcp::test_support
