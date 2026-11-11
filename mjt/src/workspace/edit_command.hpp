#pragma once

#include <string>
#include <variant>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf16_string.hpp"

#include "mjbase/asm/remapper/member_remap_plan.hpp"
#include "mjbase/asm/remapper/member_remapper.hpp"

#include "class_mutation_service.hpp"
#include "workspace_types.hpp"

namespace mjt::workspace {

class WorkspaceBase;   ///< 前置声明（workspace.hpp；applyEdit 借用会话）

/// @file edit_command.hpp
/// @brief 编辑单入口的指令载体：EditCommand tagged union + applyEdit。
///
/// 编辑族旧形态是 WorkspaceBase 上的成对 validate*/apply* 方法（11 对签名随
/// 分析需求反复冲击公共头）；本文件把「一次编辑」定型为值语义指令——新增
/// 编辑形态 = 加一个载荷 + edit_pipeline.cpp 的 handler 表加一行，公共类头
/// 签名不再变化（核心不变量）。
///
/// dryRun / expectedRevision / 验证门（validationMode）随 MutationRequest
/// 组合进同一条管道：applyEdit 内部按 dryRun 走候选校验路径（validate 语义）
/// 或提交路径（ledger 落账 + 分析库增量同步）。
struct EditCommand final
{
    /// @brief adapter 生成的完整 class 候选字节（目标必须已存在）。
    struct ClassBytes
    {
        ca::str::Utf8String      uri;
        std::vector<ca::u8>      bytes;
    };

    /// @brief 原始字节替换：URI 或内部名选择器 + 尺寸门/CAFEBABE 门/
    ///        内名一致性门（Multi-Release 族单条 URI 语义）。
    struct ReplaceClassBytes
    {
        ca::str::Utf8String      selector;
        std::vector<ca::u8>      bytes;
    };

    /// @brief 完整 class JASM 文本（sourceName 供诊断定位；缺省由调用方拼）。
    struct Jasm
    {
        ca::str::Utf8String      uri;
        ca::str::Utf16String     sourceName;
        ca::str::Utf16String     source;
        /// @brief StackMapTable 处置档（mjbase#190）：缺省 regenerate（MCP
        ///        口径零漂移）；preserve = 坏帧类 validation=off 的逃生门
        ///        （pysdk frame 参数透传，指令面改动按 fail-closed 拒绝）。
        jasm::FrameMode          frameMode = jasm::FrameMode::Regenerate;
    };

    /// @brief 结构化 class 修改（15 种变更种类，见 class_mutation_service）。
    struct ClassMutation
    {
        mjt::workspace::ClassMutation mutation;
    };

    /// @brief 重命名请求（override 风险检查在管道内前置）。
    struct Rename
    {
        RenameRequest request;
    };

    /// @brief 显式重映射计划（rename/mapping 漏斗的公共底座）。
    ///
    /// plan 为调用期借用：applyEdit 返回前须保持有效。
    struct RemapPlan
    {
        const asm_remapper::MemberRemapPlan* plan   = nullptr;
        asm_remapper::RemapPolicy            policy = asm_remapper::RemapPolicy::bytecodeOnly();
    };

    /// @brief pass/pipeline 执行（均空取 default-optimize 的兜底由调用方决定）。
    struct Pipeline
    {
        PipelineRequest request;
    };

    /// @brief jar 修复事务（扫描/删除策略见 jar_ops；旧 analyze/applyJarRepair 合流）。
    struct JarRepair
    {
        JarRepairRequest request;
    };

    /// @brief 泛条目字节写（upsert，pysdk 写边界收敛面）：条目存在则保持
    ///        原资源形态置字节（Manifest 形按文本重析）；不存在则按内容判
    ///        形——骨架解析得出内名 → ClassResource（不看 URI 扩展名，
    ///        SDK add_entry 契约），其余 UnknownResource。无门禁（class 形
    ///        编辑的门禁契约归 ClassBytes；本变体是字节面，空字节也合法）。
    struct ResourceBytes
    {
        ca::str::Utf8String     uri;
        std::vector<ca::u8> bytes;
    };

    /// @brief 条目删除（未登记 → NotFound）。无门禁：删除不改余下条目字节。
    struct RemoveResource
    {
        ca::str::Utf8String uri;
    };

    /// @brief 条目 URI 改名搬移（pysdk 挂账落账面）：资源按新 URI 重建，
    ///        字节原样（任意资源形态；IResource 自带 location 是 jar 导出
    ///        条目名——只换容器 map 键会产出 entry↔location 失配的坏罐）。
    ///        旧 URI 未登记 → NotFound；新 URI 已存在 → AlreadyExists。
    ///        单事务原子——搬移 = 一条台账行，undo 一步回退（组合
    ///        add+remove 会拆成两行、中途失败留半态）。
    struct RenameResource
    {
        ca::str::Utf8String oldUri;
        ca::str::Utf8String newUri;
    };

    /// @brief 指令变体（只加不并；顺序即 edit_pipeline.cpp 的 handler 表序）。
    using Op = std::variant<ClassBytes, ReplaceClassBytes, Jasm, ClassMutation, Rename,
                            RemapPlan, Pipeline, JarRepair, ResourceBytes, RemoveResource,
                            RenameResource>;

    Op op;
};

/// @brief 编辑单入口：EditCommand 变体 × MutationRequest 一条管道。
///
/// 统一折叠：ensureLoaded → expectedRevision 乐观门 → 候选容器构建 →
/// 验证门（validationMode）→ 台账落账 → 分析库指纹/身份/事实增量同步。
/// dryRun = true 走候选校验路径（旧 validate* 语义，不落账不动 revision）。
/// 请求须显式构造（不设默认实参——旧签名上的隐式 {} 不再隐式绑定）。
MutationResult applyEdit(WorkspaceBase&          workspace,
                         const EditCommand&      command,
                         const MutationRequest&  request);

/// @brief 方法级 JASM 的拼接展开：newMethodSource 精确替换进整 class JASM。
///
/// method_jasm / method_jasm_diff 形态的共享预处理（MCP 工具层与 pysdk 会话
/// 同一份）：取单方法规范 Jasm（diff 形态先应用到方法文本），再在整 class
/// JASM 中做唯一性精确替换。返回可直接进 EditCommand::Jasm 的载荷。
///
/// @param workspace 会话工作区（只读查询面）。
/// @param owner/name/descriptor 方法三元组（真名口径——shadow 槽解析归调用方）。
/// @param newMethodSource 最终的新方法 JASM 文本（diff 已应用完毕）。
/// @param sourceName 诊断定位名（空 = owner + "." + name + ".jasm"）。
/// @return 成功为 Jasm 载荷；失败携带 WorkspaceErrorInfo（方法未找到 / 整类
///         Jasm 生成失败 / 方法段缺失或歧义）。
WorkspaceResult<EditCommand::Jasm> spliceMethodJasm(WorkspaceBase&           workspace,
                                                    ca::str::Utf8StringRef  owner,
                                                    ca::str::Utf8StringRef  name,
                                                    ca::str::Utf8StringRef  descriptor,
                                                    ca::str::Utf8StringRef  newMethodSource,
                                                    ca::str::Utf8StringRef  sourceName);

}   // namespace mjt::workspace
