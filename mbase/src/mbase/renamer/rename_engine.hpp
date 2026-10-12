#pragma once

#include <vector>

#include "libca/core/status.hpp"
#include "libca/str/utf8_string.hpp"
#include "mbase/asm/remapper/member_remap_plan.hpp"
#include "mbase/asm/remapper/member_remapper.hpp"
#include "mbase/mapping/intermediate_mapping.hpp"
#include "mbase/mapping/mapping_format_registry.hpp"
#include "mbase/renamer/name_provider.hpp"
#include "mbase/renamer/name_proposal.hpp"

namespace mjt::base {

class ResourceContainer;

///@brief 引擎运行选项（luiox/Q6：只留跨线共性开关）。
struct RenamerOptions
{
    ///true（默认）：只构造 `MemberRemapPlan` 与审计记录，不改容器——最安全口径。
    ///false：构造后经 `applyResourceRemapPlan(container, arena, plan, policy)` 应用。
    bool planOnly = true;
    ///应用面门控（bytecode/reflectionStrings/annotations/signatures/resources），planOnly=false
    ///时生效。 注意：默认 `RemapPolicy{}` 不开 resources——类改名只改字节码/映射，资源文件 URI（如
    ///META-INF、文本索引）不同步；需同步时显式开 `policy.resources` 或 `all`。
    RemapPolicy policy{};
};

///@brief 一次 run 的计数摘要。
struct RenameRunStats
{
    ca::usize proposed       = 0;   ///< 收到的建议总数（各 provider 流合计）
    ca::usize accepted       = 0;   ///< 通过裁决的 proposal 数（不含闭包展开）
    ca::usize rejected       = 0;   ///< 被拒 proposal 数（含 provider 级失败）
    ca::usize renamedMembers = 0;   ///< 展开后落 plan 的改名条目数（类+字段+方法闭包成员）
    ca::usize suffixResolved = 0;   ///< `_N` 后缀消解次数
};

///@brief 一条已接受改名的溯源条目。
struct AcceptedChange
{
    ProposalKind        kind = ProposalKind::Class;
    ca::str::Utf8String owner;
    ca::str::Utf8String oldName;
    ca::str::Utf8String desc;
    ca::str::Utf8String newName;
    ca::str::Utf8String source;
    double              confidence  = 0.0;
    bool                fromClosure = false;   ///< override 闭包整组展开出来的成员（非主建议）
    ///建议理由（AI 建议契约 rationale 透传；P4 起随 accepted 流水留痕，可空）。
    ca::str::Utf8String rationale;
    ///证据（AI 建议契约 evidence 透传；留痕口径同 rationale）。
    ca::str::Utf8String evidence;
};

///@brief 一条被拒建议的流水条目（含完整建议原文 + 原因 + 细节）。
struct RejectedProposal
{
    NameProposal        proposal;
    RejectionReason     reason = RejectionReason::None;
    ca::str::Utf8String detail;
};

///@brief 改名审计记录：`IntermediateMapping` 底座 + source/confidence 溯源 + 被拒流水。
///
///映射底座可经 `MappingFormatRegistry` 导出（Tiny-V2 往返恒等为验收基准，
///luiox/Q5）；溯源与被拒流水随记录对象在内存中供审计。
class NameChangeRecord
{
public:
    NameChangeRecord()                                   = default;
    NameChangeRecord(NameChangeRecord&&)                 = default;
    NameChangeRecord& operator=(NameChangeRecord&&)      = default;
    NameChangeRecord(const NameChangeRecord&)            = delete;
    NameChangeRecord& operator=(const NameChangeRecord&) = delete;

    ///@brief 映射底座（接受条目按 kind 落 package/class/field/method 四类映射）。
    const IntermediateMapping& mapping() const { return mapping_; }

    const std::vector<AcceptedChange>&   accepted() const { return accepted_; }
    const std::vector<RejectedProposal>& rejected() const { return rejected_; }

    ///@brief 经注册格式导出映射文本（如 "Tiny-V2"）；未注册格式返回 INVALID_ARGUMENT。
    ca::StatusOr<ca::str::Utf8String> exportMapping(ca::str::Utf8StringRef       formatName,
                                                    const MappingFormatRegistry& registry) const;

    ///@brief 静态导出通道：任意 `IntermediateMapping` 底座复用同一导出路径
    ///（P1 起 mjt-deobf `NameRestoreRecord` 等外部记录经此导出，
    ///luiox/P1）。语义与实例方法完全一致。
    static ca::StatusOr<ca::str::Utf8String> exportMapping(
        const IntermediateMapping& mapping, ca::str::Utf8StringRef formatName,
        const MappingFormatRegistry& registry);

private:
    //以下 mutator 仅 RenamerEngine 填充。
    friend class RenamerEngine;

    IntermediateMapping& mappingMut() { return mapping_; }
    void                 addAccepted(AcceptedChange change);
    void addRejected(NameProposal proposal, RejectionReason reason, ca::str::Utf8String detail);

    IntermediateMapping           mapping_;
    std::vector<AcceptedChange>   accepted_;
    std::vector<RejectedProposal> rejected_;
};

///@brief 一次 run 的产物：状态 + plan + 审计记录 + 计数。
///
///`plan` 内 arena 视图寿命 = 本对象寿命（member_remap_plan 死亡点纪律）。
///`status` 非 Ok 表示 run 失败（视图构建失败或应用失败）；应用失败为
///全量不落盘口径——容器由既有 remapper 原子性兜底，plan/record 仅供排查。
struct RenamerRunResult
{
    ca::Status       status;
    bool             applied = false;   ///< planOnly=false 且应用成功
    RenameRunStats   stats;
    MemberRemapPlan  plan;
    NameChangeRecord record;
};

///@brief 统一 renamer 编排引擎（luiox//P0 骨架）。
///
///流水线：收集建议流 → 合法性过滤（复用 IdentifierUtil，`<init>`/`<clinit>`
///豁免，内部名分段）→ 同 target confidence 裁决（同 confidence 先到先得）
///→ method override 闭包整组同改 → package 前缀展开（all-or-reject）→
///残余撞名 `_N` 后缀消解 → 构造 `MemberRemapPlan` → plan-only 或
///`applyResourceRemapPlan`+`RemapPolicy` 应用 → `NameChangeRecord`。
///
///fail-closed：schema/建议错误逐条拒并记录不中断；应用阶段失败全量不落盘。
///存量接线进度：deobf（P1 ）→ obf（P2 ）已走引擎；workspace/MCP 面
///的语义差盘点与部分收敛见 luiox/P3（override 闭包游走经
///`RenameContext` 共用；analyzeRename/applyRename 行为零变化保留现路径）。
class RenamerEngine
{
public:
    ///@brief 对 container 跑一轮建议收集与（可选）应用。
    ///
    ///@param container 应用目标；plan-only 时仅用于只读视图与撞名检测。
    ///@param providers 建议来源列表，按序收集（到达序参与 tie-break）。
    RenamerRunResult run(ResourceContainer& container, const std::vector<NameProvider*>& providers,
                         const RenamerOptions& options);
};

}   //namespace mjt::base
