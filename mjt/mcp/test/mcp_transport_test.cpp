#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

TEST(McpServerTest, KeepsDecompilerSettingsIsolatedPerMcpSession)
{
    McpSessionHarness first;
    McpSessionHarness second;

    const auto changed = call(first,
                              "decompiler_settings_set",
                              {{"backend", "vineflower"},
                               {"timeout_ms", 1234},
                               {"max_memory_mib", 321}});
    ASSERT_FALSE(changed.at("result").at("isError"));
    const auto firstSettings = call(first, "decompiler_settings_get", Json::object());
    const auto secondSettings = call(second, "decompiler_settings_get", Json::object());
    ASSERT_FALSE(firstSettings.at("result").at("isError"));
    ASSERT_FALSE(secondSettings.at("result").at("isError"));
    EXPECT_EQ("vineflower", payload(firstSettings).at("settings").at("backend"));
    EXPECT_EQ(1234, payload(firstSettings).at("settings").at("timeout_ms"));
    EXPECT_EQ("cfr", payload(secondSettings).at("settings").at("backend"));
    EXPECT_EQ(120000, payload(secondSettings).at("settings").at("timeout_ms"));
}

TEST(McpServerTest, KeepsStdioAndHttpToolErrorsEquivalentBeyondOldBodyLimit)
{
    McpSessionHarness stdio;
    const Json invalidArguments = {{"unexpected", true}};
    const auto stdioError = call(stdio, "decompiler_list", invalidArguments, 90);
    ASSERT_TRUE(stdioError.at("result").at("isError"));

    HttpServerRunner http;
    ASSERT_TRUE(http.ready());
    auto client = makeHttpClient();
    const auto url = http.url();
    auto initialized = sendHttp(client, url, mcpPost(initializeHttpRequest(91)));
    ASSERT_EQ(200, initialized.status);
    const auto sessionHeader = initialized.headers.get("MCP-Session-Id");
    ASSERT_TRUE(sessionHeader.has_value());
    const std::string session(*sessionHeader);
    EXPECT_EQ(202,
              sendHttp(client,
                       url,
                       mcpPost(notification("notifications/initialized"), session))
                  .status);

    auto httpErrorResponse = sendHttp(
        client,
        url,
        mcpPost(request("tools/call",
                        {{"name", "decompiler_list"},
                         {"arguments", invalidArguments}},
                        92),
                session));
    ASSERT_EQ(200, httpErrorResponse.status);
    const auto httpError = httpJson(httpErrorResponse);
    ASSERT_TRUE(httpError.at("result").at("isError"));
    EXPECT_EQ(payload(stdioError), httpError.at("result").at("structuredContent"));

    const Json largeArguments = {{"padding", std::string(9U * 1024U * 1024U, 'x')}};
    auto largeResponse = sendHttp(
        client,
        url,
        mcpPost(request("tools/call",
                        {{"name", "decompiler_list"},
                         {"arguments", largeArguments}},
                        93),
                session));
    ASSERT_EQ(200, largeResponse.status);
    const auto largeError = httpJson(largeResponse);
    ASSERT_TRUE(largeError.at("result").at("isError"));
    EXPECT_NE(std::string::npos,
              largeError.at("result").at("structuredContent").at("error")
                  .get_ref<const std::string&>().find("is not an allowed property"));
    EXPECT_EQ(204, sendHttp(client, url, mcpSessionRequest("DELETE", session)).status);
}

TEST(McpServerTest, ServesIsolatedLibmcpHttpSessionsWithSseReplay)
{
    HttpOptions options;
    options.port = 0;
    HttpServerRunner http(options);
    ASSERT_TRUE(http.ready());
    auto client = makeHttpClient();
    const auto url = http.url();
    const auto trustedOrigin = "http://127.0.0.1:" + std::to_string(url.port());

    auto badOrigin = sendHttp(client,
                              url,
                              mcpPost(initializeHttpRequest(100), {},
                                      "application/json, text/event-stream", {},
                                      "http://untrusted.invalid"));
    EXPECT_EQ(403, badOrigin.status);

    auto initializedA = sendHttp(
        client,
        url,
        mcpPost(initializeHttpRequest(101), {}, "application/json, text/event-stream", {},
                trustedOrigin));
    ASSERT_EQ(200, initializedA.status);
    const auto sessionAHeader = initializedA.headers.get("MCP-Session-Id");
    ASSERT_TRUE(sessionAHeader.has_value());
    const std::string sessionA(*sessionAHeader);
    EXPECT_EQ(64U, sessionA.size());
    EXPECT_EQ("mjt-mcp", httpJson(initializedA).at("result").at("serverInfo").at("name"));
    auto readyA = sendHttp(
        client, url, mcpPost(notification("notifications/initialized"), sessionA));
    EXPECT_EQ(202, readyA.status);

    const auto fixture = writeFixture();
    auto opened = sendHttp(
        client,
        url,
        mcpPost(request("tools/call",
                        {{"name", "workspace_open"},
                         {"arguments",
                          {{"path", fixture.string()}, {"workspace_id", "session-private"}}}},
                        102),
                sessionA));
    ASSERT_EQ(200, opened.status);
    EXPECT_FALSE(httpJson(opened).at("result").at("isError"));

    auto initializedB = sendHttp(client, url, mcpPost(initializeHttpRequest(103)));
    ASSERT_EQ(200, initializedB.status);
    const auto sessionBHeader = initializedB.headers.get("MCP-Session-Id");
    ASSERT_TRUE(sessionBHeader.has_value());
    const std::string sessionB(*sessionBHeader);
    EXPECT_NE(sessionA, sessionB);
    EXPECT_EQ(202,
              sendHttp(client,
                       url,
                       mcpPost(notification("notifications/initialized"), sessionB))
                  .status);

    auto hidden = sendHttp(
        client,
        url,
        mcpPost(request("tools/call",
                        {{"name", "class_list"},
                         {"arguments", {{"workspace_id", "session-private"}}}},
                        104),
                sessionB));
    ASSERT_EQ(200, hidden.status);
    EXPECT_TRUE(httpJson(hidden).at("result").at("isError"));

    auto streamed = sendHttp(
        client,
        url,
        mcpPost(request("tools/call",
                        {{"name", "class_list"},
                         {"arguments", {{"workspace_id", "session-private"}}}},
                        105),
                sessionA,
                "application/json, text/event-stream"));
    ASSERT_EQ(200, streamed.status);
    EXPECT_EQ("text/event-stream", streamed.headers.get("Content-Type"));
    EXPECT_FALSE(httpJson(streamed).at("result").at("isError"));
    const auto streamedEvents = parseSse(streamed);
    ASSERT_EQ(2U, streamedEvents.size());
    EXPECT_TRUE(streamedEvents.front().data.empty());

    auto replay = mcpSessionRequest("GET", sessionA);
    ASSERT_TRUE(replay.headers.append("Last-Event-ID", streamedEvents.front().id).is_ok());
    auto replayed = sendHttp(client, url, std::move(replay));
    ASSERT_EQ(200, replayed.status);
    const auto replayedEvents = parseSse(replayed);
    ASSERT_EQ(1U, replayedEvents.size());
    EXPECT_EQ(streamedEvents.back().id, replayedEvents.front().id);

    EXPECT_EQ(204,
              sendHttp(client, url, mcpSessionRequest("DELETE", sessionA)).status);
    auto closed = sendHttp(
        client,
        url,
        mcpPost(request("tools/list", Json::object(), 106), sessionA));
    EXPECT_EQ(404, closed.status);
    EXPECT_EQ(204,
              sendHttp(client, url, mcpSessionRequest("DELETE", sessionB)).status);
}

TEST(McpServerTest, DeletesLibmcpHttpSessionAndCleansRunningWorkflowJob)
{
    const auto temporaryBefore = workflowTemporaryArtifacts();
    HttpServerRunner http;
    ASSERT_TRUE(http.ready());
    auto client = makeHttpClient();
    const auto url = http.url();
    const auto fixture = writeFixture();
    const auto slowDecompiler = test::getTempOutPath("fake-mcp-decompiler-slow.jar");
    std::ofstream(slowDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");
    const auto output = test::getTempOutPath("mjt_mcp_deleted_http_session_workflow.jar");
    std::error_code outputError;
    std::filesystem::remove(output, outputError);

    auto initialized = sendHttp(client, url, mcpPost(initializeHttpRequest(301)));
    ASSERT_EQ(200, initialized.status);
    const auto sessionHeader = initialized.headers.get("MCP-Session-Id");
    ASSERT_TRUE(sessionHeader.has_value());
    const std::string session(*sessionHeader);
    EXPECT_EQ(202,
              sendHttp(client,
                       url,
                       mcpPost(notification("notifications/initialized"), session))
                  .status);

    auto opened = sendHttp(
        client,
        url,
        mcpPost(request("tools/call",
                        {{"name", "workspace_open"},
                         {"arguments",
                          {{"path", fixture.string()}, {"workspace_id", "delete-session"}}}},
                        302),
                session));
    ASSERT_EQ(200, opened.status);
    ASSERT_FALSE(httpJson(opened).at("result").at("isError"));

    auto started = sendHttp(
        client,
        url,
        mcpPost(request("tools/call",
                        {{"name", "analysis_workflow"},
                         {"arguments",
                          {{"workspace_id", "delete-session"},
                           {"decompile_class", "mcp/Subject"},
                           {"decompiler_jar", slowDecompiler.string()},
                           {"java", fakeJava.string()},
                           {"export_path", output.string()},
                           {"async", true}}}},
                        303),
                session));
    ASSERT_EQ(200, started.status);
    ASSERT_FALSE(httpJson(started).at("result").at("isError"));

    EXPECT_EQ(204,
              sendHttp(client, url, mcpSessionRequest("DELETE", session)).status);
    auto closed = sendHttp(
        client,
        url,
        mcpPost(request("tools/list", Json::object(), 304), session));
    EXPECT_EQ(404, closed.status);
    EXPECT_FALSE(std::filesystem::exists(output));
    expectNoNewWorkflowTemporaryArtifacts(temporaryBefore);
}

TEST(McpServerTest, RequiresBearerAuthenticationForNonLoopbackLibmcpHttp)
{
    HttpOptions options;
    options.host  = "0.0.0.0";
    options.port  = 0;
    options.token = "unit-test-token";
    HttpServerRunner http(options);
    ASSERT_TRUE(http.ready());
    auto client = makeHttpClient();
    const auto url = http.url();

    auto unauthorized = sendHttp(client, url, mcpPost(initializeHttpRequest(201)));
    EXPECT_EQ(401, unauthorized.status);
    EXPECT_EQ("Bearer", unauthorized.headers.get("WWW-Authenticate"));

    auto authenticated = sendHttp(
        client,
        url,
        mcpPost(initializeHttpRequest(202), {}, "application/json, text/event-stream",
                "Bearer unit-test-token"));
    ASSERT_EQ(200, authenticated.status);
    const auto sessionHeader = authenticated.headers.get("MCP-Session-Id");
    ASSERT_TRUE(sessionHeader.has_value());
    const std::string session(*sessionHeader);

    auto unauthenticatedSession = sendHttp(
        client, url, mcpPost(notification("notifications/initialized"), session));
    EXPECT_EQ(401, unauthenticatedSession.status);

    auto repeatedAuthorization =
        mcpPost(initializeHttpRequest(203), {}, "application/json, text/event-stream",
                "Bearer unit-test-token");
    ASSERT_TRUE(repeatedAuthorization.headers
                    .append("Authorization", "Bearer unit-test-token")
                    .is_ok());
    EXPECT_EQ(401, sendHttp(client, url, std::move(repeatedAuthorization)).status);

    EXPECT_EQ(202,
              sendHttp(client,
                       url,
                       mcpPost(notification("notifications/initialized"), session,
                               "application/json, text/event-stream", "Bearer unit-test-token"))
                  .status);

    auto badOrigin = sendHttp(
        client,
        url,
        mcpPost(initializeHttpRequest(204), {}, "application/json, text/event-stream",
                "Bearer unit-test-token", "http://untrusted.invalid"));
    EXPECT_EQ(403, badOrigin.status);

    EXPECT_EQ(204,
              sendHttp(client,
                       url,
                       mcpSessionRequest("DELETE", session, "Bearer unit-test-token"))
                  .status);
}

TEST(McpServerTest, ReportsProtocolErrorsAndRejectsUnauthenticatedRemoteHttp)
{
    McpServer server;
    std::istringstream malformed("not JSON\n");
    std::ostringstream output;
    EXPECT_EQ(0, server.runStdio(malformed, output));
    const auto response = Json::parse(output.str());
    EXPECT_EQ(-32700, response.at("error").at("code"));

    HttpOptions options;
    options.host = "0.0.0.0";
    EXPECT_EQ(2, server.runHttp(options));
}

}   //namespace
}   //namespace mjt::mcp
