/// @file        search_match.hpp
/// @brief       搜索匹配原语共享层：引擎 searchInstructions 与 MCP/pysdk 的
///              analysis.db 查询路径共用的文本匹配/校验/regex 编译面。
///
/// 引擎活扫描（workspace.cpp）与 DB 直查路径（MCP query.cpp / pysdk
/// facadeSearch）共用同一组纯函数：regex 档实现（ECMAScript + 不敏感档
/// icase，regex_search 子串语义）与 query 边界态校验（空/超长/member_kind/
/// opcode 词表/数值字面量）只写一份，两条调用路零漂移——regex 无 SQLite
/// 原生等价，DB 拉取化的客户端过滤必须与引擎 matchesText regex 档逐字节
/// 同源（constant_scan 抽取层单源的搜索面同形先例）。
#pragma once

#include <optional>
#include <regex>
#include <string>
#include <string_view>

#include "workspace_types.hpp"

namespace mjt::workspace {

/// @brief ASCII 小写折叠（引擎匹配面的比较前处理；DB 不敏感档 LOWER 折叠
///        同口径——SQLite LOWER 只折 ASCII）。
std::string lowercase(std::string value);

/// @brief 文本匹配判定：Regex 档走预编译 regex（regex_search 子串语义；
///        @p compiledRegex 为空恒 false——调用方未编译即失配，与引擎同款），
///        Exact/Contains 按不敏感档折叠后比较。prefix 无引擎原生档（消费方
///        按 contains 取超集后自行收紧）。
bool matchesText(const std::string& value, const std::string& query,
                 const SearchOptions& options, const std::regex* compiledRegex);

/// @brief 搜索参数校验：query 非空 ≤1024 字节、member_kind 词表（field/
///        method）、opcode 词表（[-1, 199)）。错误消息与引擎面逐字节同形
///        （`operation + ": " + detail`），消费方按既有错误通道直通。
WorkspaceErrorInfo validateSearchOptions(const std::string&   query,
                                         const SearchOptions& options,
                                         const std::string&   operation);

/// @brief regex 编译入口：先 validateSearchOptions，Regex 档编译（不敏感档
///        icase），其余档返回空可选。非法 pattern → InvalidArgument（携带
///        std::regex_error 文本，与引擎报错逐字节同形）。
WorkspaceResult<std::optional<std::regex>> compileSearchRegex(const std::string&   query,
                                                              const SearchOptions& options,
                                                              const std::string& operation);

/// @brief 类内部名作用域：owner 字节精确等值 / packagePrefix 字节前缀。
bool matchesSearchScope(std::string_view owner, const SearchOptions& options);

/// @brief 数值查询字面量校验（numbers 分面边界态）：stold 完整解析才算过，
///        否则 InvalidArgument（"query must be a complete numeric value"）。
WorkspaceErrorInfo validateNumberQuery(const std::string& query);

}   // namespace mjt::workspace
