#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "mjbase/renamer/shadow_alias.hpp"
#include "mjbase/renamer/shadow_token.hpp"

namespace mjbase::workspace {
class IWorkspace;
}

namespace mjt::mcp::slot {

using Json = nlohmann::json;

///@brief workspace 缺省 shadow sidecar 的加载产物（name 槽接线用）。
///
///`present` = sidecar 文件存在且成功解析。文件缺失**或损坏**都按
///`present=false`（空表）处理——raw 路径零漂移（今日无 sidecar 工作流不受
///损），引用形状则由 resolveShadowSlot 判 missing-sidecar 硬报错，消息里
///带上 `note`（absent/invalid 原因），不静默回落裸名。这与 mjt CLI
///loadShadowDisplay 的"schema 错误告警 + 空表继续"失配容忍同口径。
struct WorkspaceShadowTable
{
    mjbase::renamer::ShadowAliasTable table;
    bool        present = false;
    std::string path;   ///< 参与判定的 sidecar 路径（诊断用）
    std::string note;   ///< present=false 的原因（absent / invalid: …）
};

///@brief 加载 workspace 的缺省 sidecar（`<jar>.mjt-shadow`；`file` 显式覆盖）。
///读取/解析失败不抛：记入 note 并按缺表继续（见类型注释）。
WorkspaceShadowTable loadWorkspaceShadowTable(mjbase::workspace::IWorkspace& workspace,
                                              const std::string& file = {});

///@brief name 槽解析结果（接线层薄包装）。
struct SlotName
{
    std::string raw;        ///< Ref 展开 → 真实名；raw 恒等 → 原样
    std::string descriptor; ///< MemberRef/raw 成员命中的表内 desc（类槽为空）
    bool        fromHandle = false;
};

///@brief class 整串槽（class 等）：Ref → 表展开；raw → `verifyRaw` 时核表
///（不存在 → raw-not-found），直通面传 false。
///@return false 时 `error` 已填结构化 toolError（含四分类码前缀）。
bool resolveClassSlot(const WorkspaceShadowTable& slots,
                      const std::string&          input,
                      bool                        verifyRaw,
                      SlotName&                   out,
                      Json&                       error);

///@brief 资源 URI 槽的 Ref 换算：解析出的内部名 → 精确资源 URI
///（classInfo 语义；exact-URI 消费面 patch_*/workspace_revert_class 用）。
///raw 输入不经此函数（原样直通，契约不变）。
///@return false 时 `error` 已填结构化 toolError。
bool resourceUriOfResolved(mjbase::workspace::IWorkspace& workspace,
                           const std::string&                    internalName,
                           std::string&                          uri,
                           Json&                                 error);

///@brief 成员槽（name / member_name / method_name；owner 已解析）。
///MemberRef → (raw, desc)（desc 自表回填）；raw → 核表/直通同上。
///`descHint` 非空且与解析出的 desc 不一致 → 结构化错误（句柄解析
///本身成功，属调用方输入矛盾，不入四分类）。
///@return false 时 `error` 已填结构化 toolError。
bool resolveMemberSlot(const WorkspaceShadowTable& slots,
                       const std::string&          ownerRaw,
                       const std::string&          input,
                       const std::string&          descHint,
                       bool                        verifyRaw,
                       SlotName&                   out,
                       Json&                       error);

///@brief method 三元组槽便捷面：从 arguments 取 owner/name/descriptor 并解析。
///
///owner=ClassRef/raw、name=MemberRef/raw（desc 自表回填或核对）。`verifyRaw`
///直通语义同上。ownerRaw 返回解析后的 owner（后续 classInfo 用）。
///@return false 时 `error` 已填结构化 toolError。
bool resolveMethodSlots(const WorkspaceShadowTable& slots,
                        const Json&                 arguments,
                        bool                        verifyRaw,
                        std::string&                ownerRaw,
                        std::string&                name,
                        std::string&                descriptor,
                        Json&                       error);

///@brief descriptor 省略时的名称解析回填。
///
///owner+name 无 descriptor 时按宿主类真实成员解析：唯一命中回填 descriptor
///正常执行；名字不存在报错并说明 owner 内无此名（该名字以另一形态存在时
///附提示，复用既有的候选枚举机）；真重载（同多名异描述符）报歧义并列出
///全部候选描述符，响应附同一格式的 diagnostics 数组。owner 类不可解析
///时按无操作放行（descriptor 保持为空），由下游 workspace 给出 canonical
///错误（表外类如 java/lang/Object 的直通查询不受影响）。
///@param field true 按字段池解析（xref target=field），false 按方法池。
///@return false 时 `error` 已填结构化 toolError（含 diagnostics）。
bool backfillMemberDescriptor(mjbase::workspace::IWorkspace& workspace,
                              const std::string&             ownerRaw,
                              const std::string&             memberName,
                              bool                           field,
                              std::string&                   descriptor,
                              Json&                          error);

///@brief arguments 内 class 语义键（`class`）就地解析：Ref → 内部名；
///raw → verifyRaw 核表后原样。空值直通（workspace 既有 canonical 错误）。
///@return false 时 `error` 已填结构化 toolError。
bool resolveClassArgument(mjbase::workspace::IWorkspace& workspace,
                          const Json&                           arguments,
                          const std::string&                    key,
                          bool                                  verifyRaw,
                          std::string&                          out,
                          Json&                                 error);

///@brief arguments 内资源 URI 语义键（`uri` / `class_uri`）就地解析。
///
///Ref → 内部名 → 精确资源 URI（exact-URI 消费面契约）；raw → 按内部名核表
///（miss → raw-not-found），命中后**原样透传**（URI 契约零漂移）。空值直通。
///@return false 时 `error` 已填结构化 toolError。
bool resolveClassUriArgument(mjbase::workspace::IWorkspace& workspace,
                             const Json&                           arguments,
                             const std::string&                    key,
                             std::string&                          out,
                             Json&                                 error);

///@brief 显式描述符失配诊断：name 在宿主类存在但 descriptor 与任一候选都
///不符时，给结构化错误并列出该名全部候选描述符（与省略 descriptor 时的
///歧义路径同一呈现面）。name 不存在 / owner 不可解析 / descriptor 恰好命中
///时返回 true（不适配——失败另有其因，走 canonical 错误）。
///@return false 时 `error` 已填结构化 toolResult（含 diagnostics）。
bool memberDescriptorMismatch(mjbase::workspace::IWorkspace& workspace,
                              const std::string&             ownerRaw,
                              const std::string&             memberName,
                              const std::string&             descriptor,
                              bool                           field,
                              Json&                          error);

///@brief 错误 Json（toolError 同形）：四分类码前缀 + 诊断。
Json slotError(const std::string& message);

}   //namespace mjt::mcp::slot
