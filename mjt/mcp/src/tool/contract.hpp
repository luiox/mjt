#pragma once

#include <string>

#include "service.hpp"

namespace mjt::mcp::detail {

///@brief 返回完整的 MCP 工具描述与输入 schema。
Json toolsList();

///@brief 按工具 schema 和全局 JSON 边界校验一次工具调用参数。
bool validateToolArguments(const std::string& name, const Json& arguments, std::string& error);

///@brief 名字是否在收敛后的 11 工具注册面内；未注册名一律 "unknown tool"。
bool isRegisteredTool(const std::string& name);

}   //namespace mjt::mcp::detail
