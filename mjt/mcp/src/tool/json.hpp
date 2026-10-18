#pragma once

#include <string>
#include <vector>

#include "service.hpp"

namespace mjt::mcp::detail {

ca::str::Utf16String utf16(const std::string& value);
std::string utf8(ca::str::Utf16StringRef value);
Json statusJson(const ca::Status& status);
Json diagnosticJson(const mjbase::jasm::Diagnostic& diagnostic);
Json methodJson(const mjbase::workspace::MethodInfo& method);
Json supportingResourceJson(const mjbase::workspace::SupportingResourceInfo& support);
Json classJson(const mjbase::workspace::ClassInfo& clazz);
Json searchHitJson(const mjbase::workspace::SearchHit& hit);
Json mutationJson(const mjbase::workspace::MutationResult& mutation);
Json jarRepairJson(const mjbase::workspace::JarRepairResult& repair);
Json referenceJson(const mjbase::workspace::ReferenceInfo& reference);
Json workflowJson(const mjbase::workspace::WorkflowResult& workflow);
Json toolResult(Json payload, bool isError = false);
Json toolError(const std::string& message, const ca::Status* status = nullptr);
std::string base64(const std::vector<ca::u8>& value);
bool decodeBase64(const std::string& encoded,
                  std::vector<ca::u8>& output,
                  std::string& error);

///@brief 统一解析分页 `cursor`（字符串形式的十进制无符号字节偏移）。
///
///与 contract.cpp 的 `validateToolArguments` 使用同一 `std::from_chars` 规则：
///拒绝非字符串值、空串、`+1`/`-1`/`1x` 等非纯十进制及溢出值。`offset` 会被
///clamp 到 `[0, total]`，使非法/越界 cursor 一律退化到空尾页，避免
///page/classSummaryPage 抛异常后被 service 兜底成 INTERNAL 错误。
///
///@param arguments 工具调用参数；含 `cursor` 时解析，否则保留传入 offset。
///@param total 结果总数，用于 clamp。
///@param offset [in,out] 入参为默认 offset，出参为最终 offset（已 clamp）。
///@return true 解析成功或无 cursor；false cursor 存在但非法（offset 不变）。
bool parseCursor(const Json& arguments, ca::usize total, ca::usize& offset);

Json page(Json values, const Json& arguments);
Json classSummaryPage(const std::vector<mjbase::workspace::ClassSummary>& values,
                      const Json& arguments);
Json impactJson(const mjbase::workspace::ImpactAnalysis& impact);
Json normalizeToolError(const std::string& name,
                        const Json& arguments,
                        Json result,
                        const mjbase::workspace::IWorkspace* workspace);

}   //namespace mjt::mcp::detail
