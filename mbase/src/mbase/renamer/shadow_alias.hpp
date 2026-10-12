#pragma once

#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mbase/mapping/intermediate_mapping.hpp"

namespace mjt::base {

class ResourceContainer;

///@brief shadow 别名条目的符号类别。
enum class ShadowKind : ca::u8
{
    Class,
    Field,
    Method,
};

///@brief 一条 shadow 别名：真名（raw）→ 仅显示层名字（shadow）。
///
///所有字段都是 `ShadowAliasTable` 私有 arena 的视图；寿命 = 表对象寿命。
struct ShadowAliasEntry
{
    ShadowKind             kind       = ShadowKind::Class;
    ca::str::Utf8StringRef owner      = {};  ///< class 条目为空
    ca::str::Utf8StringRef desc       = {};  ///< class 条目为空
    ca::str::Utf8StringRef rawName    = {};
    ca::str::Utf8StringRef shadowName = {};
    ///引用 token：8 个小写 hex 字符（uint32 %08x 语义），shadow 名分配时
    ///由 CSPRNG 生成、表内唯一。旧格式 sidecar（v1）加载的条目为空；
    ///`${Class_N.token8}` 引用解析属 T2，本卡只做表层存取与生成。
    ca::str::Utf8StringRef token8     = {};
};

///@brief 机械占位名生成统计（cmd shadow 摘要用）。
struct ShadowGenStats
{
    ca::usize assignedClasses = 0;
    ca::usize assignedMethods = 0;
    ca::usize assignedFields  = 0;
    ca::usize keptEntries     = 0;  ///< 生成前已登记、本次保留的条目数
};

///@brief shadow name 表：真名 → 显示名映射，仅服务显示层，永不写回字节码。
///
///与真实 remap 数据零混写（不进 MemberRemapPlan）；sidecar 经
///`ShadowAliasFormat`（注册名 "Shadow-Alias"）读写，解析产物
///`IntermediateMapping`（oldName=raw、newName=shadow）由 `fromMapping` 收编。
///
///-- arena 死亡点声明（memory-governance-plan §5-P1③）--
///本成员持有表私有的 Utf8StringArena。指向本 arena 的视图包括：entries_ 各
///字段、classByRaw_/memberByOwnerKey_ 的 key 与 value、byShadowName_/
///byRawName_ 的 key。视图寿命 = 本对象寿命；死亡点：clear、析构、移动赋值
///（arena 移动赋值释放旧 chunk，见 libca utf8_string_arena.hpp 警告）。
///消费者不得把 entries/查询返回的视图带过本对象的死亡点。
class ShadowAliasTable
{
public:
    ShadowAliasTable() = default;

    ///@brief 从 sidecar 解析产物构造（同符号重复登记以先到者优先，返回 false）。
    ///v2 产物条目的 token 列随行收编；token 缺省为空。
    static ShadowAliasTable fromMapping(const IntermediateMapping& mappings);

    ///@brief 导出为 sidecar 产物（writeShadowAliasText / registry "Shadow-Alias" 消费）。
    ///token8 随行导出到产物条目 token 列。
    IntermediateMapping toMapping() const;

    ///@brief 登记 class 别名；同一 raw 符号已登记时返回 false（保留先登记者）。
    ///token8 缺省为空（旧格式加载/无 token 场景）。
    bool addClass(ca::str::Utf8StringRef raw, ca::str::Utf8StringRef shadow,
                  ca::str::Utf8StringRef token8 = {});

    ///@brief 登记 field 别名；同一 (owner, desc, raw) 已登记时返回 false。
    bool addField(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef desc,
                  ca::str::Utf8StringRef raw, ca::str::Utf8StringRef shadow,
                  ca::str::Utf8StringRef token8 = {});

    ///@brief 登记 method 别名；同一 (owner, desc, raw) 已登记时返回 false。
    bool addMethod(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef desc,
                   ca::str::Utf8StringRef raw, ca::str::Utf8StringRef shadow,
                   ca::str::Utf8StringRef token8 = {});

    bool    empty() const { return entries_.empty(); }
    ca::usize size() const { return entries_.size(); }

    ///@brief 全部条目（登记序；视图寿命见类注释）。
    const std::vector<ShadowAliasEntry>& entries() const { return entries_; }

    //── 正向查询：未登记返回空视图 ──────────────────────────────────
    ca::str::Utf8StringRef classShadow(ca::str::Utf8StringRef raw) const;
    ca::str::Utf8StringRef fieldShadow(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef raw,
                                       ca::str::Utf8StringRef desc) const;
    ca::str::Utf8StringRef methodShadow(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef raw,
                                        ca::str::Utf8StringRef desc) const;

    //── 显示渲染（钉死：shadow 命中 → "shadow (原始名)"，否则原样；
    //两段均经 IdentifierUtil::printReadable 转义，与既有打印口径逐字节一致）──
    ///@brief class 内部名渲染。
    ca::str::Utf8String renderClass(ca::str::Utf8StringRef raw) const;

    ///@brief 成员名字片段渲染（不含 owner/desc；方法标签等分离打印点用）。
    ca::str::Utf8String renderMemberName(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef raw,
                                         ca::str::Utf8StringRef desc) const;

    ///@brief 完整成员形式 `owner.name:desc`；未命中输出与既有 printReadable
    ///拼接逐字节一致。命中时 owner/name 段按 `shadow (raw)` 并列；
    ///P0-2 起尾随 ` (完整原始形式)` 与前段逐字段重合（纯重复）
    ///不再输出——仅当格式变动致任一字段只在前段外可见时才保留。
    ca::str::Utf8String renderMember(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                     ca::str::Utf8StringRef desc) const;

    //── 反向 / 范围查询（MCP 双向查询、jasm legend 用）────────────────
    ///@brief shadow 名 → 条目列表（机械占位名跨类重号，可多条）。
    std::vector<ShadowAliasEntry> findByShadowName(ca::str::Utf8StringRef shadow) const;

    ///@brief 真名 → 条目列表（class 为内部名；成员为裸名，跨 owner 可多条）。
    std::vector<ShadowAliasEntry> findByRawName(ca::str::Utf8StringRef raw) const;

    ///@brief 某 class 名下全部条目（class 条目自身 + 以它为 owner 的成员条目）。
    std::vector<ShadowAliasEntry> entriesOfClass(ca::str::Utf8StringRef owner) const;

    ///@brief class 条目精确查询；未登记返回 nullptr。
    const ShadowAliasEntry* classEntry(ca::str::Utf8StringRef raw) const;

    ///@brief 给缺 token8 的条目补 CSPRNG 随机号：8 位小写 hex、表内
    ///唯一（碰撞重生成）；已有 token 的条目原样保留（幂等，重跑不换号）。
    ///shadow_generate 写出路径收尾调用；返回本次补号条数。
    ///随机源 = libca `ca::crypto::secure_random_bytes`（系统 CSPRNG）；
    ///系统 RNG 不可用时抛 std::runtime_error（不自造随机源、不静默降级）。
    ca::usize backfillTokens();

    //── 生成期占用检查 ─────────────────────────────────────────────
    ///@brief shadow 名是否已被其它 class 占用（除 exceptRaw 外）。
    bool classShadowTaken(ca::str::Utf8StringRef shadow, ca::str::Utf8StringRef exceptRaw) const;

    ///@brief shadow 名是否已被该 owner 作用域内的其它成员占用。
    bool memberShadowTaken(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef shadow) const;

    ///@brief 清空全表（视图全部失效；prune 重建路径用）。
    void clear();

private:
    ///成员表复合 key："owner\x1fname\x1fdesc"（owner 为空也走同款拼装）。
    ca::str::Utf8StringRef memberKey(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef raw,
                                     ca::str::Utf8StringRef desc);

    ca::str::Utf8StringArena                       arena_;
    std::vector<ShadowAliasEntry>                  entries_;
    std::unordered_map<ca::str::Utf8StringRef, ca::usize> classByRaw_;
    std::unordered_map<ca::str::Utf8StringRef, ca::usize> memberByOwnerKey_;
    std::unordered_map<ca::str::Utf8StringRef, std::vector<ca::usize>> byShadowName_;
    std::unordered_map<ca::str::Utf8StringRef, std::vector<ca::usize>> byRawName_;
};

///@brief 一键批量生成机械指代名（钉死规则）。
///
///- class：全 jar 按内部名字典序全局排序，位置 i（1 起）→ `Class_i`；
///- method/field：类内按 (name, desc) 字典序排序，位置 N（1 起）→
///`Method_N` / `Field_N`（作用域 = 所属 class）；
///- 已登记 shadow 的符号保留原条目（幂等：重跑不换号，手工名不丢）；
///- 生成的名字如与作用域内已有 shadow 名撞车则 N 递增跳过；
///- 收尾 `backfillTokens`：新条目与历史缺号条目（v1 sidecar 遗留）一律补
///CSPRNG token8，已有 token 原样保留——`mjt shadow` 与 MCP shadow_generate
///两条生成路径共享本函数，token 语义一致。
///确定性：jar 不变则影子名编号输出不变（token8 为随机列，不参与编号与排序）；
///容器遍历序无关（先排序后编号）。
ShadowGenStats generateMechanicalShadowNames(ResourceContainer& container,
                                             ShadowAliasTable&  table);

///@brief 清理指向不存在符号的过期条目（jar 变更后的失配容忍补充，--shadow-prune）。
///
///存在性口径：class 条目要求容器内有该 class；成员条目要求 owner 存在且
///声明了同 (desc, name) 成员。返回移除条数。
ca::usize pruneShadowAliases(ResourceContainer& container, ShadowAliasTable& table);

///@brief jasm 文本显示层 shadow 改写（显示层；输出不再是可回编译 jasm）。
///
///整 token 替换：class 条目按转义后内部名全 jar 范围替换（含描述符内
///`L<name>;` 的 L 前缀连带）；成员条目仅替换 ownerClass 名下的裸名 token 与
///`owner.name` 复合 token。引号字符串字面量内部不动。配合
///`entriesOfClass` 在文本前打 legend（`shadow = raw` 对照）。
ca::str::Utf8String rewriteJasmForDisplay(const ca::str::Utf8String& jasmText,
                                          const ShadowAliasTable&    table,
                                          ca::str::Utf8StringRef     ownerClass);

}   //namespace mjt::base
