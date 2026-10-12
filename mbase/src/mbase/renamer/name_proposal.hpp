#pragma once

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"

namespace mjt::base {

///@brief 建议改名的目标类别（luiox//P0 四类）。
enum class ProposalKind : ca::u8
{
    Class,    ///< 类改名：oldName = 内部类名（a/b/C），owner 与 desc 留空
    Package,  ///< 包改名：oldName = 旧包路径（a/b），owner 与 desc 留空；引擎展开为逐类映射
    Field,    ///< 字段改名：owner + oldName + desc 必填
    Method,   ///< 方法改名：owner + oldName + desc 必填；引擎做 override 闭包整组同改
};

///@brief 建议被拒原因（fail-closed 流水列）。
enum class RejectionReason : ca::u8
{
    None = 0,
    Schema,             ///< 契约字段缺失/越界：desc 缺失、confidence 出 [0,1]、空名、no-op 等
    IllegalIdentifier,  ///< 新名不是合法 Java 标识符 / 内部名分段非法
    Keyword,            ///< 新名命中 Java 关键字表
    ReservedName,       ///< `<init>`/`<clinit>` 豁免不改，也不可作为新名
    NotFound,           ///< 改名目标（类/包前缀/成员 owner）在容器中不存在
    Conflict,           ///< 同 target 裁决败者 / 残余撞名整组失败
    ProviderError,      ///< provider propose 整体硬失败（该 provider 建议流全弃）
    ApplyFailed,        ///< 应用阶段失败（既有 remapper 原子性口径，全量不落盘）
};

///@brief RejectionReason 的稳定字符串标签（审计/日志用）。
ca::str::Utf8StringRef rejectionReasonName(RejectionReason reason);

///@brief 一条改名建议（值语义边界对象，luiox/Q2 结论）。
///
///字符串一律 `Utf8String` 值持有：provider 输出（未来 JSON 契约）天然值语义，
///引擎内部构造 plan 时再按需 intern。`Utf8String` 不可拷贝，本类型 move-only。
class NameProposal
{
public:
    NameProposal() = default;

    NameProposal(ProposalKind kind,
                 ca::str::Utf8String owner,
                 ca::str::Utf8String oldName,
                 ca::str::Utf8String desc,
                 ca::str::Utf8String newName,
                 double              confidence,
                 ca::str::Utf8String source)
        : kind(kind)
        , owner(std::move(owner))
        , oldName(std::move(oldName))
        , desc(std::move(desc))
        , newName(std::move(newName))
        , confidence(confidence)
        , source(std::move(source))
    {}

    NameProposal(NameProposal&&) noexcept            = default;
    NameProposal& operator=(NameProposal&&) noexcept = default;
    NameProposal(const NameProposal&)                = delete;
    NameProposal& operator=(const NameProposal&)     = delete;

    ///目标类别。
    ProposalKind kind = ProposalKind::Class;
    ///字段/方法的 owner 内部类名；class/package 建议必须留空（oldName 即路径）。
    ca::str::Utf8String owner;
    ///改名前名字：class=内部类名 / package=包路径 / field/method=成员名。
    ca::str::Utf8String oldName;
    ///字段/方法描述符；class/package 建议必须留空。field/method 建议必填（exact 规则）。
    ca::str::Utf8String desc;
    ///改名后名字。
    ca::str::Utf8String newName;
    ///建议置信度，契约范围 [0,1]；越界按 Schema 拒。
    double confidence = 0.0;
    ///来源标签（provider 名 / "ai:xxx" / "restore" 等）；为空时引擎填 provider 名。
    ca::str::Utf8String source;
    ///建议理由（AI 建议契约 rationale 字段透传；可空）。
    ///引擎不改写其内容，仅随 accepted/rejected 流水留痕（审计溯源）。
    ca::str::Utf8String rationale;
    ///证据（AI 建议契约 evidence 字段透传；可空）。留痕口径同 rationale。
    ca::str::Utf8String evidence;
};

}   //namespace mjt::base
