#include <algorithm>
#include <string>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

///libjbc 包消费试点：class_fidelity_check 的保真契约与结构摘要。
TEST(McpServerTest, FidelityCheckReportsByteIdenticalRoundTripAndSummary)
{
    McpSessionHarness server;

    const auto& initialize = server.initializeResponse();
    EXPECT_EQ("mjt-mcp", initialize.at("result").at("serverInfo").at("name"));

    //工具已在注册表：schema 以 contract 为准，这里只看名字在列。
    const auto tools = server.handle(request("tools/list"));
    const auto listed = tools.at("result").at("tools");
    ASSERT_TRUE(std::any_of(listed.begin(), listed.end(), [](const Json& item) {
        return item.at("name") == "class_fidelity_check";
    }));

    const auto fixture = writeFixture();
    const auto opened  = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

    //fixture 的两个 jasm 组装类：解析 → 不触碰写回必须字节恒等，摘要与
    //classSource 形状一致（1 字段 1 方法，父类 java/lang/Object）。
    for (const char* name : {"mcp/Subject", "mcp/Second"}) {
        const auto response = call(server,
                                   "class_fidelity_check",
                                   {{"workspace_id", workspaceId}, {"class", name}});
        ASSERT_FALSE(response.at("result").at("isError")) << name;
        const auto& value = payload(response);
        EXPECT_EQ(name, value.at("internal_name").get<std::string>()) << name;
        EXPECT_TRUE(value.at("identical").get<bool>()) << name;
        EXPECT_EQ(value.at("size_in"), value.at("size_out")) << name;
        EXPECT_EQ("validated", value.at("parse_state").get<std::string>()) << name;
        EXPECT_EQ(0, value.at("diagnostic_count").get<int>()) << name;
        EXPECT_EQ("java/lang/Object", value.at("super_class").get<std::string>()) << name;
        EXPECT_EQ(1, value.at("field_count").get<int>()) << name;
        EXPECT_EQ(1, value.at("method_count").get<int>()) << name;
        EXPECT_TRUE(value.at("class_attributes").is_array()) << name;
        EXPECT_TRUE(value.contains("libjbc_version")) << name;
    }
}

///字节恒等破坏面之外的常规错误路径：类不存在走 canonical 工具错误。
TEST(McpServerTest, FidelityCheckUnknownClassIsToolError)
{
    McpSessionHarness server;

    const auto fixture = writeFixture();
    const auto opened  = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();

    const auto response = call(server,
                               "class_fidelity_check",
                               {{"workspace_id", workspaceId}, {"class", "mcp/Missing"}});
    ASSERT_TRUE(response.at("result").at("isError"));
}

}   //namespace
}   //namespace mjt::mcp
