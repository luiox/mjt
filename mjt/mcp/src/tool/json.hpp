#pragma once

#include <optional>
#include <string>
#include <vector>

#include "service.hpp"

namespace mjt::mcp::detail {

ca::str::Utf16String utf16(const std::string& value);
std::string utf8(ca::str::Utf16StringRef value);
///ASCII 小写副本（prefix 档与结构化过滤的大小写不敏感比较用）。
std::string lowercased(const std::string& value);
Json statusJson(const ca::Status& status);
Json diagnosticJson(const mjbase::jasm::Diagnostic& diagnostic);
Json methodJson(const mjt::workspace::MethodInfo& method);
Json supportingResourceJson(const mjt::workspace::SupportingResourceInfo& support);
Json classJson(const mjt::workspace::ClassInfo& clazz);

///@brief instructions 搜索命中 value（拼接文本）的结构化拆解。
///
///value 由 mjbase 按 `<mnemonic> <node_type> <opcode>[ 操作数…]` 拼接；
///node_type 由 mjbase instructionNodeType 语义归类（invoke/field/ldc/
///cp_ref/var/array/jump/const/…），拆解时跳过。cst 是常量/引用语义的摘要
///文本（仅 CP 支持的助记符非空，即 ldc/field/invoke/new/cast/instanceof 族）。
struct InstructionHitParts final
{
    std::string mnemonic;
    std::string operand;
    std::string cst;
};

bool parseInstructionHitValue(const std::string& value, InstructionHitParts& output);

///@brief 常量池裸 UTF8 行的 string_kind 分面（响应与过滤共用同一判定）。
///
///按内容形态细分：utf8_descriptor（方法描述符 '(' 开头）、utf8_classref
///（类型引用：内部名 / L…; / 数组描述符）、utf8_name（其余：属性名、
///成员名等）。
std::string utf8StringKind(const std::string& value);

///@brief 折叠工具分支的参数键契约。
struct BranchKeys final
{
    ///必填定位键（selectorAware 分支的定位契约）。
    std::vector<std::string> selectors;
    ///可选定位键（成员分支的 descriptor；词表内、缺省合法）。
    std::vector<std::string> optionalSelectors;
    ///分支其余合法参数键（含分页/投影/过滤键）。
    std::vector<std::string> supported;
};

///@brief kind/target/direction → 分支参数键契约的单一事实源。
///
///class_query/search/xref_query 的每个分支在这里声明合法键；contract 校验层
///（未声明键的 unknown parameter 文案）与分发层 checkBranchKeys 共用同一张
///表，两侧文案的键集保证一致。分支无法识别（kind 缺失/非法、references 的
///target 非法）返回 nullopt：schema 的 enum/required 先行拦截。
std::optional<BranchKeys> branchKeysFor(const std::string& tool,
                                        const std::string& kind,
                                        const std::string& target = {},
                                        const std::string& direction = {});

///@brief kind/target 分支的参数键检查。
///
///selectorAware 分支（class_query/xref_query）的 selectors 是定位键契约
///（class/owner/name/descriptor/uri 词表的子集）：参数里出现契约外定位键时
///报 `<label> requires <spec>; got <keys>`，缺定位键时报
///`<label> requires <spec>`；selectors 为空仍按词表拒绝全部定位键。
///optionalSelectors是词表内的可选定位键：缺省合法、不进 requires
///文案的 spec，但同样不计入契约外定位键。非 selectorAware 分支（search 等）：
///定位词表键就是普通参数，不参与词表检查。其余键必须逐个出现在 supportedKeys
///（project_id/kind/target 恒放行，分支支持的完整键表），违者报
///`<label> does not support key(s) <keys>; supported: <all>`。
///
///@return 空 optional = 通过；否则是可直接返回的结构化 toolError。
std::optional<Json> checkBranchKeys(const std::string& label,
                                    const Json& arguments,
                                    bool selectorAware,
                                    const std::vector<std::string>& selectors,
                                    const std::vector<std::string>& supportedKeys,
                                    const std::vector<std::string>& optionalSelectors = {});

///@brief 缺失/非法枚举选择器的统一文案。
///
///缺失报 `<key>: Missing key (expected a, b or c)`；非法值报
///`<key>: Invalid value "x" (expected a, b or c)`。
std::string selectorKeyError(const std::string& key,
                             const std::string& value,
                             const std::vector<std::string>& expected);

Json searchHitJson(const mjt::workspace::SearchHit& hit);
Json mutationJson(const mjt::workspace::MutationResult& mutation);
Json jarRepairJson(const mjt::workspace::JarRepairResult& repair);
Json referenceJson(const mjt::workspace::ReferenceInfo& reference);
Json workflowJson(const mjt::workspace::WorkflowResult& workflow);
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

///@brief 列表分页 + fields 投影（fields 按静态行键校验）。
///
///页大小：page_size 优先，其次沿用 limit，均缺省 kDefaultPageSize（50），
///两者互斥、上限 kMaximumPageSize。fields 是键名数组：items 只保留指定键，
///total/offset 等外层键不受投影影响。键名合法性按 `rowKeys`（该 kind 的静
///态行键全集）判定，不依赖结果集样本——空结果集也有可校验面：合法键照常
///返回空 rows，非法键报 unknown key 并列出该 kind 合法键全集。仅截断路径
///写入 partial 侧车五键，完整页零变化。
///
///@param rowKeys 该分面的静态行键全集（调用方按分支声明）。
///@return true 时 out 携带分页文档；false 时 error 携带结构化 toolError。
bool page(Json values, const Json& arguments, Json& out, std::string& error,
          const std::vector<std::string>& rowKeys);
bool classSummaryPage(const std::vector<mjt::workspace::ClassSummary>& values,
                      const Json& arguments,
                      Json& out,
                      std::string& error);
Json impactJson(const mjt::workspace::ImpactAnalysis& impact);
Json normalizeToolError(const std::string& name,
                        const Json& arguments,
                        Json result,
                        const mjt::workspace::IWorkspace* workspace);

}   //namespace mjt::mcp::detail
