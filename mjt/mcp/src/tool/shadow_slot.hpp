#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "mbase/renamer/shadow_alias.hpp"
#include "mbase/renamer/shadow_token.hpp"

namespace mjt::base::workspace {
class IWorkspace;
}

namespace mjt::mcp::slot {

using Json = nlohmann::json;

///@brief workspace 缺省 shadow sidecar 的加载产物（T2 name 槽接线用）。
///
///`present` = sidecar 文件存在且成功解析。文件缺失**或损坏**都按
///`present=false`（空表）处理——raw 路径零漂移（今日无 sidecar 工作流不受
///损），引用形状则由 resolveShadowSlot 判 missing-sidecar 硬报错，消息里
///带上 `note`（absent/invalid 原因），不静默回落裸名。这与 mjt CLI
///loadShadowDisplay 的"schema 错误告警 + 空表继续"失配容忍同口径。
struct WorkspaceShadowTable
{
    mjt::base::ShadowAliasTable table;
    bool        present = false;
    std::string path;   ///< 参与判定的 sidecar 路径（诊断用）
    std::string note;   ///< present=false 的原因（absent / invalid: …）
};

///@brief 加载 workspace 的缺省 sidecar（`<jar>.mjt-shadow`；`file` 显式覆盖）。
///读取/解析失败不抛：记入 note 并按缺表继续（见类型注释）。
WorkspaceShadowTable loadWorkspaceShadowTable(mjt::base::workspace::IWorkspace& workspace,
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
bool resourceUriOfResolved(mjt::base::workspace::IWorkspace& workspace,
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

///@brief arguments 内 class 语义键（`class`）就地解析：Ref → 内部名；
///raw → verifyRaw 核表后原样。空值直通（workspace 既有 canonical 错误）。
///@return false 时 `error` 已填结构化 toolError。
bool resolveClassArgument(mjt::base::workspace::IWorkspace& workspace,
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
bool resolveClassUriArgument(mjt::base::workspace::IWorkspace& workspace,
                             const Json&                           arguments,
                             const std::string&                    key,
                             std::string&                          out,
                             Json&                                 error);

///@brief 错误 Json（toolError 同形）：四分类码前缀 + 诊断。
Json slotError(const std::string& message);

}   //namespace mjt::mcp::slot
