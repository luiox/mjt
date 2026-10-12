#include "mcp_server.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string_view>
#include <utility>

#include <libca/http/server.hpp>
#include <libca/io/error.hpp>
#include <libca/io/reader.hpp>
#include <libca/io/writer.hpp>
#include <libca/json/json.hpp>
#include <libca/net/address.hpp>

#include <mcp/stdio_transport.hpp>
#include <mcp/streamable_http_server.hpp>
#include <mcp/tool_registry.hpp>

#include "tool/service.hpp"
#include "tool/limits.hpp"

namespace mjt::mcp {

struct HttpRuntime final
{
    HttpRuntime(ca::http::HttpServer serverValue, ::mcp::StreamableHttpServer adapterValue)
        : server(std::move(serverValue))
        , adapter(std::move(adapterValue))
    {}

    ca::http::HttpServer       server;
    ::mcp::StreamableHttpServer adapter;
};

namespace {

ca::str::Utf8StringRef utf8Ref(const std::string& value) noexcept
{
    return ca::str::Utf8StringRef::from_data(
        value.empty() ? nullptr : reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

Json fromLibmcpJson(const ca::json::JsonValue& value)
{
    using ca::json::JsonType;
    switch (value.type()) {
    case JsonType::Null: return nullptr;
    case JsonType::Bool: return value.as_bool();
    case JsonType::Int: return value.as_int();
    case JsonType::Float: return value.as_float();
    case JsonType::String: return value.as_string().to_std_string();
    case JsonType::Array:
    {
        Json result = Json::array();
        for (const auto& item : value.as_array()) result.push_back(fromLibmcpJson(item));
        return result;
    }
    case JsonType::Object:
    {
        Json result = Json::object();
        for (const auto& item : value.as_object())
            result[item.first.to_std_string()] = fromLibmcpJson(item.second);
        return result;
    }
    }
    return nullptr;
}

::mcp::MethodResult toLibmcpDocument(const Json& value)
{
    const auto encoded = value.dump();
    auto       parsed  = ca::json::JsonReader::read(utf8Ref(encoded));
    if (parsed.is_err()) {
        const auto error = std::move(parsed).unwrap_err();
        return ca::core::Err(::mcp::MethodError::internal(
            "mjt tool result JSON conversion failed: " + error.message.to_std_string()));
    }
    return ca::core::Ok(std::move(parsed).unwrap());
}

::mcp::McpResult<::mcp::ServerSession> makeSession(
    const std::shared_ptr<McpToolService>& service)
{
    auto registry = std::make_shared<::mcp::ToolRegistry>();
    for (const auto& descriptor : service->tools()) {
        const auto encoded    = descriptor.dump();
        auto       definition = ::mcp::ToolDefinition::parse(utf8Ref(encoded));
        if (definition.is_err()) return ca::core::Err(std::move(definition).unwrap_err());
        const auto name = descriptor.at("name").get<std::string>();
        auto registered = registry->register_tool(
            std::move(definition).unwrap(),
            [service, name](const ca::json::JsonValue& arguments) -> ::mcp::MethodResult {
                try {
                    return toLibmcpDocument(service->call(name, fromLibmcpJson(arguments)));
                }
                catch (const std::exception& exception) {
                    return ca::core::Err(::mcp::MethodError::internal(exception.what()));
                }
            });
        if (registered.is_err()) return ca::core::Err(std::move(registered).unwrap_err());
    }

    ::mcp::ServerOptions options;
    options.name        = "mjt-mcp";
    options.version     = "2.1.0";
    options.title       = "mjt JVM analysis workspace";
    options.description = "Transactional JVM bytecode analysis and editing tools";
    auto session        = ::mcp::ServerSession::create(std::move(options));
    if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
    auto value     = std::move(session).unwrap();
    auto installed = value.install_tools(std::move(registry));
    if (installed.is_err()) return ca::core::Err(std::move(installed).unwrap_err());
    return ca::core::Ok(std::move(value));
}

class IstreamReader final : public ca::io::Reader
{
public:
    explicit IstreamReader(std::istream& input) noexcept
        : input_(input)
    {}

    ca::io::IoResult<ca::usize> read(ca::u8* buffer, ca::usize capacity) override
    {
        if (capacity == 0) return ca::core::Ok(static_cast<ca::usize>(0));
        auto*      stream = input_.rdbuf();
        const auto first  = stream->sbumpc();
        if (std::char_traits<char>::eq_int_type(first, std::char_traits<char>::eof())) {
            if (input_.bad())
                return ca::core::Err(ca::io::IoError::from_kind(ca::io::IoErrorKind::Other,
                                                                "read stdio input failed"));
            return ca::core::Ok(static_cast<ca::usize>(0));
        }
        buffer[0] = static_cast<ca::u8>(std::char_traits<char>::to_char_type(first));
        if (capacity == 1) return ca::core::Ok(static_cast<ca::usize>(1));

        //对非 seekable 的 stdin（pipe），in_avail 返回 <=0。原实现在此 early
        //return 只读 1 字节，导致 24 MiB 大消息触发多达 2400 万次 sbumpc 系统调用
        //（L6 性能问题）。改为直接用 sgetn 阻塞读满剩余 capacity：streambuf 在
        //无数据时会阻塞等待，避免逐字节读取。
        const auto available = stream->in_avail();
        const auto amount    = available > 0
                                ? static_cast<std::streamsize>(
                                   std::min(capacity - 1,
                                            static_cast<ca::usize>(std::min(
                                                available, std::numeric_limits<std::streamsize>::max()))))
                                : static_cast<std::streamsize>(capacity - 1);
        const auto count = stream->sgetn(reinterpret_cast<char*>(buffer + 1), amount);
        //sgetn 可能因 EOF/无更多数据而少于 amount；以实际读到的为准。
        return ca::core::Ok(static_cast<ca::usize>(count) + 1);
    }

private:
    std::istream& input_;
};

class OstreamWriter final : public ca::io::Writer
{
public:
    explicit OstreamWriter(std::ostream& output) noexcept
        : output_(output)
    {}

    ca::io::IoResult<ca::usize> write(const ca::u8* data, ca::usize length) override
    {
        output_.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(length));
        if (!output_)
            return ca::core::Err(ca::io::IoError::from_kind(
                ca::io::IoErrorKind::Other, "write stdio output failed"));
        return ca::core::Ok(length);
    }

    ca::io::IoResult<void> flush() override
    {
        output_.flush();
        if (!output_)
            return ca::core::Err(ca::io::IoError::from_kind(
                ca::io::IoErrorKind::Other, "flush stdio output failed"));
        return ca::core::Ok();
    }

private:
    std::ostream& output_;
};

bool isLoopback(const ca::net::IpAddress& address) noexcept
{
    if (address.is_ipv4()) return address.octets()[0] == 127;
    return address == ca::net::IpAddress::localhost_v6();
}

std::string originHost(const ca::net::IpAddress& address)
{
    const auto value = address.to_string();
    return address.is_ipv6() ? "[" + value + "]" : value;
}

std::vector<std::string> allowedOrigins(const HttpOptions& options,
                                        const ca::net::SocketAddress& address)
{
    if (!options.allowedOrigins.empty()) return options.allowedOrigins;
    std::vector<std::string> result;
    const auto append = [&](std::string origin) {
        if (std::find(result.begin(), result.end(), origin) == result.end())
            result.push_back(std::move(origin));
    };
    append("http://" + originHost(address.ip()) + ":" + std::to_string(address.port()));
    if (isLoopback(address.ip())) {
        append("http://127.0.0.1:" + std::to_string(address.port()));
        append("http://localhost:" + std::to_string(address.port()));
        append("http://[::1]:" + std::to_string(address.port()));
    }
    return result;
}

ca::http::HttpServerResponse unauthorizedResponse()
{
    static constexpr std::string_view body = R"({"error":"unauthorized"})";
    ca::http::HttpResponse response;
    response.status = 401;
    response.headers.append("Content-Type", "application/json");
    response.headers.append("WWW-Authenticate", "Bearer");
    response.body = ca::core::Bytes::copy_from_slice(
        reinterpret_cast<const ca::u8*>(body.data()), body.size());
    return ca::http::HttpServerResponse::buffered(std::move(response));
}

}   //namespace

McpServer::McpServer()
    : toolService_(std::make_shared<McpToolService>())
{}

McpServer::~McpServer()
{
    stopHttp();
}

::mcp::McpResult<::mcp::ServerSession> McpServer::createSession()
{
    return makeSession(toolService_);
}

int McpServer::runStdio(std::istream& input, std::ostream& output)
{
    auto session = createSession();
    if (session.is_err()) return 1;
    IstreamReader                reader(input);
    OstreamWriter                writer(output);
    ::mcp::StdioTransportOptions transportOptions;
    transportOptions.max_message_bytes = detail::kMaximumMessageBytes;
    auto transport = ::mcp::StdioTransport::create(reader, writer, transportOptions);
    if (transport.is_err()) return 1;
    auto sessionValue   = std::move(session).unwrap();
    auto transportValue = std::move(transport).unwrap();
    auto served         = sessionValue.serve_stdio(transportValue);
    return served.is_ok() ? 0 : 1;
}

int McpServer::runHttp(const HttpOptions& options)
{
    if (options.port < 0 || options.port > 65535) return 2;
    auto address = options.host == "localhost"
                       ? ca::core::Ok(ca::net::IpAddress::localhost_v4())
                       : ca::net::IpAddress::parse(options.host);
    if (address.is_err()) return 2;
    const auto ip = std::move(address).unwrap();
    if (!isLoopback(ip) && options.token.empty()) return 2;

    std::unique_lock<std::mutex> runtimeLock(httpMutex_);
    if (httpRuntime_) return 2;

    ca::http::HttpServerOptions serverOptions;
    //与 stdio 共用 detail::kMaximumMessageBytes，保证两条传输路径工具对等。
    serverOptions.request_limits.max_body_bytes = detail::kMaximumMessageBytes;
    auto bound = ca::http::HttpServer::bind(
        ca::net::SocketAddress(ip, static_cast<ca::u16>(options.port)), serverOptions);
    if (bound.is_err()) return 1;
    auto server       = std::move(bound).unwrap();
    auto localAddress = server.local_address();
    if (localAddress.is_err()) return 1;
    const auto actualAddress = std::move(localAddress).unwrap();

    ::mcp::StreamableHttpServerOptions adapterOptions;
    adapterOptions.allowed_origins = allowedOrigins(options, actualAddress);
    adapterOptions.sse.emplace();
    if (!isLoopback(ip)) {
        const auto token = options.token;
        adapterOptions.authorizer =
            [token](const ca::http::HttpServerRequestContext& context)
            -> ca::http::HttpResult<::mcp::HttpAuthorizationDecision> {
            const auto authorizations = context.request().headers.get_all("Authorization");
            if (authorizations.size() != 1 || authorizations.front() != "Bearer " + token)
                return ca::core::Ok(
                    ::mcp::HttpAuthorizationDecision::rejected(unauthorizedResponse()));
            return ca::core::Ok(::mcp::HttpAuthorizationDecision::authorized("mjt-bearer"));
        };
    }

    auto adapter = ::mcp::StreamableHttpServer::create(
        [](const ::mcp::HttpSessionContext&) {
            return makeSession(std::make_shared<McpToolService>());
        },
        std::move(adapterOptions));
    if (adapter.is_err()) return 1;
    auto adapterValue = std::move(adapter).unwrap();
    auto installed    = adapterValue.install(server);
    if (installed.is_err()) return 1;

    auto runtime  = std::make_shared<HttpRuntime>(std::move(server), std::move(adapterValue));
    httpRuntime_  = runtime;
    httpPort_.store(actualAddress.port());
    runtimeLock.unlock();

    const auto served = runtime->server.serve();
    runtimeLock.lock();
    if (httpRuntime_ == runtime) httpRuntime_.reset();
    httpPort_.store(0);
    return served.is_ok() ? 0 : 1;
}

void McpServer::stopHttp() noexcept
{
    std::shared_ptr<HttpRuntime> runtime;
    {
        std::lock_guard<std::mutex> lock(httpMutex_);
        runtime = httpRuntime_;
    }
    if (runtime) runtime->server.stop();
}

int McpServer::httpPort() const noexcept
{
    return httpPort_.load();
}

}   //namespace mjt::mcp
