#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/util/span.hpp"

///@defgroup class_hierarchy 继承图模块
///
///基于整数索引并行数组的高性能继承图，替代旧版 string-keyed map + 四套分散 LCA 实现。
///
///## 设计目标
///
///查询速度 > 构建速度 > 内存占用。热路径操作（isSubType、superClass、commonSuper）
///每个方法解析都触发，需 O(1) 或 O(depth)；构建在 jar 启动时一次，可接受 O(N log N)。
///
///## 数据结构
///
///全部由 ClassIndex（i32）索引：
///- `entries_[]`：ClassEntry（name, superIndex, interfaces 区间, flags）
///- `interfaces_[]`：拍平接口列表
///- `ancestors_[]` + `ancestorSpans_[]`：全量 eager DFS 预计算祖先闭包
///- `nameToIndex_`：名称 ↔ 索引映射
///
///## 构建四阶段
///
///Phase 1 — 扫描：遍历全部 ClassResource 创建 ClassEntry，填充 nameToIndex_
///Phase 2 — 边解析：解析 superName/interfaces，缺类通过 lookup 回调 / phantom / tombstone 处理
///Phase 3 — 祖先闭包：eager DFS 全量预计算，深度优先序拍平存入 ancestors_
///Phase 3.5 — Broken 传播：broken 标记传播给所有后代
///
///## Phantom 机制
///
///| 状态 | 含义 |
///|------|------|
///| Real | 存在于输入集或通过 lookup 解析 |
///| Broken | 父类不在输入集且 lookup 无结果 |
///| Tombstone | 曾经查过但不存在 |
///
///pass 通过 isBroken/hasMissingDep 决定降级行为。
///
///## 关键查询
///
///- `isSubType(A,B)`：扫描 A 的深度序祖先列表（O(depth)，depth ≤ 20）
///- `commonSuper(A,B)`：嵌套线性扫描 A 和 B 的祖先（O(depth²) ≤ 400）
///- `superClass/ancestors/interfaces`：O(1) 或区间 span
///
///## 设计决策
///
///- 整数索引并行数组 → O(1) entry 访问，cache-friendly
///- isSubType 全量 eager DFS 线性扫描 → depth 上限 ~20，无需 hash/sort 结构
///- phantom + broken + tombstone → 构造完整图，pass 可查状态
///- 方法覆盖用并查集 connected component → rename 正确性前提
///- remap 支持 rename 后更新 → 避免重建整个 hierarchy
///
///更多细节（MethodHierarchy、FieldHierarchy、实现计划）见 `doc/_archive/inheritance-graph-design.md`。
///
///@par 标签
///- `scope:global`
///- `action:analyze`
///- `stability:core`

namespace mjt::base {

using ClassIndex                   = ca::i32;
constexpr ClassIndex INVALID_CLASS = -1;

enum ClassFlag : ca::u8
{
    CF_Interface     = 1 << 0,
    CF_Enum          = 1 << 1,
    CF_Broken        = 1 << 2,
    CF_HasMissingDep = 1 << 3,
};

struct ClassEntry
{
    ca::str::Utf8StringRef      name;
    ClassIndex                  index;
    ClassIndex                  superIndex;   //INVALID_CLASS for phantom/Object
    ca::i32                     ifStart;
    ca::i32                     ifEnd;
    ca::u8                      flags;
    jbytecode::tree::ClassNode* classNode = nullptr;
};

class ClassHierarchy
{
public:
    using ClassLookup = std::function<jbytecode::tree::ClassNode*(ca::str::Utf8StringRef)>;

    ClassHierarchy();

    void build(ResourceContainer& container, ca::str::Utf8StringArena& arena,
               ClassLookup lookup = nullptr);

    ClassIndex             indexOf(ca::str::Utf8StringRef name) const;
    ca::str::Utf8StringRef nameOf(ClassIndex idx) const;
    const ClassEntry&      entry(ClassIndex idx) const;
    ca::usize              classCount() const { return entries_.size(); }

    ClassIndex             superClass(ClassIndex idx) const;
    Span<const ClassIndex> interfaces(ClassIndex idx) const;
    bool                   isSubType(ClassIndex sub, ClassIndex super) const;
    ClassIndex             commonSuper(ClassIndex a, ClassIndex b) const;
    Span<const ClassIndex> ancestors(ClassIndex idx) const;

    bool isInterface(ClassIndex idx) const;
    bool isEnum(ClassIndex idx) const;
    bool isBroken(ClassIndex idx) const;
    bool hasMissingDep(ClassIndex idx) const;

    void remap(Span<std::pair<ca::str::Utf8StringRef, ca::str::Utf8StringRef>> mapping);

    ClassIndex objectIndex() const { return objectIndex_; }

private:
    struct AncestorSpan
    {
        ca::i32 start;
        ca::i32 end;
    };

    ClassIndex findOrCreateEntry(ca::str::Utf8StringRef name);
    ClassIndex resolveSuper(ca::str::Utf8StringRef superName);
    ClassIndex addLookedUpClass(jbytecode::tree::ClassNode* classNode);
    void       computeAncestors();
    void       computeSubtypeIntervals();
    void       propagateBroken();

    std::vector<ClassEntry>                                entries_;
    std::vector<ClassIndex>                                interfaces_;
    std::vector<ClassIndex>                                ancestors_;
    std::vector<AncestorSpan>                              ancestorSpans_;
    std::vector<ca::u32>                                   subtypeIntervalStart_;
    std::vector<ca::u32>                                   subtypeIntervalEnd_;
    std::unordered_map<ca::str::Utf8StringRef, ClassIndex> nameToIndex_;

    std::unordered_set<ca::str::Utf8StringRef>               tombstones_;
    ClassLookup                                              lookup_;
    //-- arena 死亡点声明（memory-governance-plan §5-P1③）--
    //本成员是借用指针（不拥有 arena），由 build(container, arena, ...) 记账
    //（class_hierarchy.cpp:31 arena_ = &arena）。build/remap 会把全部类名、
    //super 名、接口名与 tombstone intern 进该 arena；nameOf(idx) 与
    //entry(idx).name 返回的视图都指向它。
    //视图寿命 ≠ 本对象寿命，= build 时传入的那个 arena 的寿命；死亡点分两种：
    //① ClassHierarchyAnalysis（class_hierarchy_analysis.hpp）传
    //GlobalAnalysisManager::arena，即 WorkspaceBase::ownedArena_，视图活到
    //WorkspaceBase 析构，本对象被 AnalysisManager 缓存期间视图始终有效；
    //② WorkspaceBase::classRelations（workspace.cpp:2803）等调用点传方法局部
    //arena，方法返回即亡。
    //本对象自身析构不结束任何视图寿命；反之传入的 arena 先亡则视图全部悬空，
    //此时本对象也不得再调用 nameOf/entry/isSubType 等按名查询。
    //消费者不得把视图带过传入 arena 的死亡点；需跨期持有则 intern 进自己的
    //arena 或深拷贝。
    ca::str::Utf8StringArena*                                arena_       = nullptr;
    ClassIndex                                               objectIndex_ = INVALID_CLASS;
    std::vector<std::unique_ptr<jbytecode::tree::ClassNode>> ownedClassNodes_;
};

}   //namespace mjt::base
