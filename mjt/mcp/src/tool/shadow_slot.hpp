#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "mjbase/renamer/shadow_alias.hpp"
#include "mjbase/renamer/shadow_token.hpp"
#include "workspace/workspace_types.hpp"

namespace mjt::workspace {
class WorkspaceBase;
}

namespace mjt::mcp::slot {

using Json = nlohmann::json;

///@brief workspace shadow 表的加载产物（name/descriptor/jasm 槽接线用）。
///
///加载序（shadow 落分析库）：workspace 绑定了分析库 → renames 覆盖层整表
///还原（DB 是唯一真源；sidecar 已退役为 openProjectSession 的一次性导入
///源，运行时不写回）；未绑库（无会话测试面/旧工作流）或 `file` 显式覆盖
///→ sidecar 文件 `<jar>.mjt-shadow`。
///
///`present` = 加载成功（DB 或 sidecar）。加载失败按 `present=false`（空表）
///处理——raw 路径零漂移（无 shadow 工作流不受损），引用形状则由
///resolveShadowSlot 硬报错，消息里带上 `note`（absent/invalid 原因），
///不静默回落裸名。
struct WorkspaceShadowTable
{
    mjbase::renamer::ShadowAliasTable table;
    bool        present = false;
    std::string path;   ///< 参与判定的源（DB 占位名或 sidecar 路径；诊断用）
    std::string note;   ///< present=false 的原因（absent / invalid: …）
};

///@brief 加载 workspace 的 shadow 表（DB 真源优先，sidecar 回退；见类型注释）。
///读取/解析失败不抛：记入 note 并按缺表继续（见类型注释）。
WorkspaceShadowTable loadWorkspaceShadowTable(mjt::workspace::WorkspaceBase& workspace,
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
bool resourceUriOfResolved(mjt::workspace::WorkspaceBase& workspace,
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
bool backfillMemberDescriptor(mjt::workspace::WorkspaceBase& workspace,
                              const std::string&             ownerRaw,
                              const std::string&             memberName,
                              bool                           field,
                              std::string&                   descriptor,
                              Json&                          error);

///@brief arguments 内 class 语义键（`class`）就地解析：Ref → 内部名；
///raw → verifyRaw 核表后原样。空值直通（workspace 既有 canonical 错误）。
///@return false 时 `error` 已填结构化 toolError。
bool resolveClassArgument(mjt::workspace::WorkspaceBase& workspace,
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
bool resolveClassUriArgument(mjt::workspace::WorkspaceBase& workspace,
                             const Json&                           arguments,
                             const std::string&                    key,
                             std::string&                          out,
                             Json&                                 error);

///@brief 描述符实参槽：值含 `${` 引用形状 → Descriptor 槽展开（内嵌类
///handle → 真名，四分类错误直传）；否则原样直通（raw 描述符零漂移，其文法
///校验与 canonical 错误面留在下游，不因槽接线收紧）。空值直通。
///@return false 时 `error` 已填结构化 toolError。
bool resolveDescriptorArgument(const WorkspaceShadowTable& slots,
                               const std::string&          input,
                               std::string&                out,
                               Json&                       error);

///@brief jasm 操作数实参槽（指令搜索 operand 过滤器用）：值含 `${` 引用
///形状 → JasmBody 槽展开（类 handle / owner.name 复合 / 尾随描述符）；
///否则原样直通。空值直通。
///@return false 时 `error` 已填结构化 toolError。
bool resolveJasmBodyArgument(const WorkspaceShadowTable& slots,
                             const std::string&          input,
                             std::string&                out,
                             Json&                       error);

///@brief patch_apply 结构化实参的嵌套引用就地改写（Json 子树面）。
///
///覆盖（值含 `${` 引用形状才动作，raw 零漂移直通）：
///- `member_descriptor`——全 kind 提升：add_field/add_method 的新成员
///  类型描述符内嵌类名也是引用语义（member_name 仍按新符号字面量豁免）；
///- `annotation_descriptor` / `local_variables[].descriptor`——Descriptor 槽
///  （`L…;` 类型位展开）；
///- `exceptions[]` / `try_catch_blocks[].type`——Name 槽（开放类名面：
///  verifyRaw=false，表外 raw 如 JDK 类直通）；
///- `field_value` 常量树——kind=type 的 descriptor（裸内部类名 → Name 槽）、
///  kind=dynamic 的 descriptor（真描述符）、handle 的 owner+name+descriptor、
///  handle/bootstrap_method 子对象、bootstrap_arguments 递归；dynamic 的
///  name 是 linkage 字面量（新符号），不接成员槽；
///- `instruction` / `initial_instructions[]`——owner 全类指令接 Name 槽；
///  descriptor 按位置分槽（type/ldc_type 裸内部类名走 Name 槽，field/
///  method/multianewarray 等真描述符走 Descriptor 槽）；name 仅 field/method
///  指令接成员槽（ldc_dynamic/invokedynamic 的 name 是 call site linkage
///  字面量，不按成员解析）。
///
///`signature`（类/成员/局部变量）刻意不接：Descriptor 槽对泛型 `<>`
///fail-loud，泛型签名展开是 mjbase v2 挂账。
///@return false 时 `error` 已填结构化 toolError。
bool resolveStructuredMutationRefs(const WorkspaceShadowTable& slots,
                                   Json&                      args,
                                   Json&                      error);

///@brief 显式描述符失配诊断：name 在宿主类存在但 descriptor 与任一候选都
///不符时，给结构化错误并列出该名全部候选描述符（与省略 descriptor 时的
///歧义路径同一呈现面）。name 不存在 / owner 不可解析 / descriptor 恰好命中
///时返回 true（不适配——失败另有其因，走 canonical 错误）。
///@return false 时 `error` 已填结构化 toolResult（含 diagnostics）。
bool memberDescriptorMismatch(mjt::workspace::WorkspaceBase& workspace,
                              const std::string&             ownerRaw,
                              const std::string&             memberName,
                              const std::string&             descriptor,
                              bool                           field,
                              Json&                          error);

///@brief 错误 Json（toolError 同形）：四分类码前缀 + 诊断。
Json slotError(const std::string& message);

}   //namespace mjt::mcp::slot
