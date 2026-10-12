#pragma once

///@defgroup resolved_call_graph 已解析调用图模块
///
///## 目的
///
///`ResolvedCallGraph` 是 P3.1 层次感知调用图层。它位于现有直接 `CallGraph` 之上，
///回答一个独立的问题：
///
///- 直接 `CallGraph`：字节码指令中编码的是哪个方法键？
///- 已解析调用图：经过虚分派或接口分派扩展后，哪些项目局部方法可能在运行时可达？
///
///直接图保持不变。除非显式请求 `ResolvedCallGraphAnalysis`，
///现有消费者保持精确的字节码语义。
///
///## 输入
///
///已解析图从以下组件构建：
///
///- `ReferenceIndex`，用于方法调用点和原始操作码/调用点元数据；
///- `CallGraph`，用于直接图边界和外部方法键；
///- `ClassHierarchy`，用于类/接口继承链；
///- `MethodHierarchy`，用于覆盖组件。
///
///该分析通过 `ResolvedCallGraphAnalysis` 可用，并由默认的 jar 管道引导
///与其他全局分析一起注册。
///
///## 扩展规则
///
///对于每个 `MethodInsnNode` 引用：
///
///- `invokestatic` 保持精确编码目标；
///- `invokespecial` 保持精确编码目标；
///- `invokevirtual` 通过目标方法的覆盖组件扩展；
///- `invokeinterface` 通过接口方法的覆盖组件扩展；
///- 编码 owner 未声明该方法但 owner 是容器内已知类型时，沿 superIndex 父类链
///（首中即停，与 RTA canonical 解析同口径）与接口继承链找真实声明者后按其
///覆盖组件扩展（子类型静态接收者直调、子接口未重声明由此落真实边）；
///- owner 不在容器或全链无声明的缺失、格式错误、外部目标降级为原始编码目标。
///
///每个已解析边同时保留编码目标和扩展目标，让后续通行证决定是报告
///字节码表面还是基于保守运行时目标集操作。
///
///## 边界
///
///第一层有意不执行：
///
///- 推断接收者分配类型；
///- 解析 `invokedynamic` 引导目标；
///- 解析反射字符串；
///- 从可能目标集中移除抽象/接口声明；
///- 重写字节码。
///
///另有两个已知的"合法角落实为欠近似"（既有 `MethodHierarchy` 覆盖组件口径，
///第二刀处理）：类实现两个互不继承的接口（一侧 default、一侧 abstract）且类不
///声明该方法时，abstract 侧组件缺少 default 实现；类方法覆盖接口 default
///（class-wins）时，invokeinterface 的接口侧组件缺少超类具体实现。
///
///这些属于独立的策略层。将此图保持为仅查询分析使其安全地在
///未使用方法扩展、纯函数分析和内联器开始消费之前引入。
///
///## RTA 精度档
///
///`buildRta` 在 CHA 之上提供 RTA（rapid type analysis）档：
///
///- 入口可达性驱动：从 `RtaOptions` 根方法（显式根 + 可选 main 发现）做
///worklist 闭包，只有入口可达方法的方法体被展开，不可达方法的边整体缺席；
///- 已实例化类型集合收窄虚分派：入口可达方法体内 `new T` 把 T 加入已实例化
///集合（fixpoint，晚发现的实例化会重估早先挂起的虚调用点）；虚/接口调用点
///按 canonical RTA 的方法解析口径收窄——从已实例化类型 T 沿父类链找第一个
///拥有组件条目的类，dispatch 落在该类名下条目。子类继承父类/接口 default
///具体方法且不覆写时，祖先名下条目可行（继承未覆写不是欠近似）；子类覆写
///时解析停在子类，父类条目不出边；组件在册但闭包零实例化的调用点零边
///（相对 CHA 的精度收益来源）；
///- 调用点粒度接收者静态类型门控（评审 P3-1 引入，R3 评审 P1-(a) 重
///设计为站点级健全判据）：per-root feasible 按组件共享，落在目标 owner 上的
///边还要求 ∃ 已实例化类型 S：S <: R（接收者声明类型）且 owner ∈ ancestors(S)
///——「具体父类 × 无关接口」菱形下确定执行的真边由此保留（宁过近似不丢真实
///执行边）；另保留 R <: owner 的保守补遗（子类型静态接收者直调/接口 default
///经祖先条目）。接收者类型在容器外/未知时 fail-open 维持原口径。该门控仅
///筛选调用点目标（挂起与物化同口径），不改实例化集合与 fixpoint 传播机制；
///- 外部/缺失虚目标仍降级为编码目标（与 CHA 相同），编码 owner 未声明时与
///CHA 档同源沿链落真实声明者（仍受实例化集约束），空描述符反射查找按名
///落解口径与 `GlobalReachability` 对齐。
///
///代价：需要入口根集合（roots 变化即边集变化），精度换保守性的剩余欠近似
///面在反射实例化、native、classpath 外实例化（如反序列化）——RTA 档的
///结果不应直接作为删除依据，供调用图报告与 CHA 档对照。
///
///## 消费者
///
///计划中的消费者包括：
///
///- `ResolvedGlobalReachabilityAnalysis`，用于层次感知活跃性；
///- 未使用清理通行证，在删除类资源或私有成员前使用已解析可达性；
///- 小型内联器，如 getter/短方法/参数转发内联器；
///- 访问收紧和方法重命名安全检查。
///
///## 验证
///
///`ResolvedCallGraphTest` 构建内存中类覆盖：
///
///- `invokevirtual Base.run` 扩展到 `Base.run` 和 `Child.run`；
///- `invokeinterface I.run` 扩展到 `I.run` 和 `Impl.run`；
///- `invokestatic Helper.call` 保持直接；
///- 缺失虚目标降级为编码的外部键；
///- `GlobalAnalysisManager` 集成。

#include <string>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"
#include "mbase/asm/classpath/call_graph.hpp"
#include "mbase/hierarchy/class_hierarchy.hpp"
#include "mbase/hierarchy/class_hierarchy_analysis.hpp"
#include "mbase/hierarchy/method_hierarchy.hpp"
#include "mbase/hierarchy/method_hierarchy_analysis.hpp"
#include "mbase/reference/reference_index.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

///@brief Dispatch category used when expanding one encoded call site.
///
///`ResolvedCallGraph` keeps the direct bytecode target and records why extra
///targets were added. Consumers can use this to distinguish exact static calls
///from conservative virtual/interface expansion without re-reading bytecode.
enum class ResolvedCallKind
{
    Direct,
    Special,
    Static,
    Virtual,
    Interface,
    Unresolved,
    ///@brief Dynamic edge woven in after build (reflection binding), not
    ///backed by an encoded call-site target. Provenance marker only: build
    ///never emits this kind, and statistics keep it separate from the static
    ///CHA buckets above.
    Reflect
};

///@brief One resolved edge from a source method to a possible runtime target.
///
///`encodedTarget` is the owner/name/descriptor stored in the original
///`MethodInsnNode`; `resolvedTarget` is one declared or external method that
///may be reached after hierarchy expansion. For static/special/private or
///unresolved calls the two keys are normally identical.
struct ResolvedCallEdge
{
    ReferenceSite    site;
    MethodRefKey     encodedTarget;
    MethodRefKey     resolvedTarget;
    ResolvedCallKind kind     = ResolvedCallKind::Unresolved;
    bool             expanded = false;
};

///@brief RTA 构建选项（buildRta）。
struct RtaOptions
{
    ///@brief 显式入口根方法（与 main 发现并集）。
    std::vector<MethodRefKey> rootMethods;

    ///@brief 把容器内 `public static void main(String[])` 声明并入根集合。
    bool includeMainMethods = true;
};

///@brief Hierarchy-aware method connectivity view.
///
///This graph is layered above the direct `CallGraph`. It does not change direct
///graph semantics; instead it reads `ReferenceIndex` call sites and expands
///`invokevirtual` / `invokeinterface` targets through `MethodHierarchy`
///override components. Missing or external targets degrade to the original
///encoded method key.
class ResolvedCallGraph
{
public:
    ///@brief Rebuild resolved edges from direct references and hierarchy data.
    ///
    ///The direct `CallGraph` parameter is currently used as the declared /
    ///external method frontier and kept in the signature so future policies can
    ///choose whether to retain external direct nodes.
    void build(const ReferenceIndex& index, const CallGraph& callGraph,
               const ClassHierarchy& classHierarchy, const MethodHierarchy& methodHierarchy);

    ///@brief Rebuild as an RTA (rapid type analysis) graph.
    ///
    ///Entry-reachability driven: only bodies of methods reachable from
    ///`options` roots (+ optional main discovery) contribute edges. Virtual and
    ///interface expansion follows canonical RTA method resolution: for an
    ///instantiated type T, the feasible dispatch target is the override
    ///component entry under the first class on T's superclass chain that owns
    ///one — so inherited (unoverridden) superclass/interface-default entries
    ///stay reachable, while an overriding subclass stops resolution and keeps
    ///ancestor entries out. A site whose component gains no feasible owner
    ///contributes zero edges. Missing/external virtual targets degrade to the
    ///encoded target exactly like the CHA build.
    void buildRta(const ReferenceIndex& index, const CallGraph& callGraph,
                  const ClassHierarchy& classHierarchy, const MethodHierarchy& methodHierarchy,
                  const RtaOptions& options);

    ///@brief Remove every edge and lookup table.
    void clear();

    ///@brief Return all resolved outgoing edges emitted by one source method.
    const std::vector<ResolvedCallEdge>& edgesFrom(const std::string& owner,
                                                   const std::string& name,
                                                   const std::string& desc) const;

    ///@brief Return all resolved incoming edges targeting one possible method.
    const std::vector<ResolvedCallEdge>& edgesTo(const std::string& owner, const std::string& name,
                                                 const std::string& desc) const;

    ///@brief Return unique possible targets called by one source method.
    ///
    ///Ordering follows first-seen edge order. Repeated call sites to the same
    ///possible target are collapsed in this convenience view.
    std::vector<MethodRefKey> resolvedTargetsFrom(const std::string& owner, const std::string& name,
                                                  const std::string& desc) const;

    ///@brief Return the reverse resolved-edge transitive closure to one method.
    ///
    ///The result lists methods from which a resolved edge path can reach the
    ///requested target, excluding the target itself. Walk follows
    ///`resolvedTarget` backwards through the call-site sources, so virtual and
    ///interface expansion participate in the closure. Missing targets return an
    ///empty list; cycles are safe. Ordering follows first-seen discovery order.
    std::vector<MethodRefKey> reverseReachableTo(const std::string& owner, const std::string& name,
                                                 const std::string& desc) const;

    ///@brief Append one pre-built edge to the graph.
    ///
    ///Build-time code uses this to register encoded call-site edges. It stays
    ///public so post-build augmentation (the reflection binding weave) can add
    ///`ResolvedCallKind::Reflect` edges without re-running build; the caller
    ///owns dedup and provenance policy. The next build discards appended
    ///edges.
    void appendEdge(const ResolvedCallEdge& edge);

    ///@brief Return true if a hierarchy-expanded edge exists.
    bool hasResolvedEdge(const std::string& sourceOwner, const std::string& sourceName,
                         const std::string& sourceDesc, const std::string& targetOwner,
                         const std::string& targetName, const std::string& targetDesc) const;

    ///@brief Number of resolved edges, including per-call-site duplicates.
    ca::usize edgeCount() const { return edges_.size(); }

    ///@brief Return every resolved edge in build order.
    ///
    ///Edges are per encoded call site: a virtual/interface site expanded to N
    ///targets contributes N entries. Consumers that need stable output should
    ///sort by (site, resolvedTarget) themselves; the stored order follows
    ///ReferenceIndex scan order and is deterministic per input.
    const std::vector<ResolvedCallEdge>& edges() const { return edges_; }

    ///@brief RTA 构建后的已实例化类型集合（发现序）。
    ///
    ///含外部类名（可达代码中 `new` 出的 JDK/库类型）；仅容器内声明的类参与
    ///虚分派收窄。仅 buildRta 后非空；磁盘还原路径（.rcg v1.1）只序列化
    ///计数不序列化名单，此时本视图为空而 rtaInstantiatedCount 为真实值。
    const std::vector<std::string>& instantiatedClasses() const { return instantiatedClasses_; }

    ///@brief RTA 构建后的已实例化类型计数。
    ///
    ///进程内 buildRta 后与 instantiatedClasses.size 恒等；磁盘还原路径
    ///（.rcg v1.1 RTA 摘要段）为文件记录的真实计数。CHA 档恒 0。
    ca::usize rtaInstantiatedCount() const { return rtaInstantiatedCount_; }

    ///@brief RTA 构建后的入口可达方法数（含外部目标）。
    ca::usize rtaReachableMethodCount() const { return rtaReachableMethodCount_; }

    ///@brief 还原 RTA 摘要计数（.rcg 读取端专用）。
    ///
    ///仅持久化还原路径调用：文件只落计数不落名单，边表与查询视图不受影响。
    void setRtaSummaryCounters(ca::usize instantiatedCount, ca::usize reachableCount);

private:
    std::vector<ResolvedCallEdge>                                  edges_;
    std::unordered_map<std::string, std::vector<ResolvedCallEdge>> edgesBySource_;
    std::unordered_map<std::string, std::vector<ResolvedCallEdge>> edgesByTarget_;
    std::vector<std::string>                                       instantiatedClasses_;
    ca::usize                                                      rtaInstantiatedCount_    = 0;
    ca::usize                                                      rtaReachableMethodCount_ = 0;
};

///@brief Global analysis wrapper for `ResolvedCallGraph`.
//前向声明：dependencyInvalidated 只需类型名（analysisID / inv.invalidate），无需完整定义。
struct ReferenceIndexAnalysis;
struct CallGraphAnalysis;
struct ClassHierarchyAnalysis;
struct MethodHierarchyAnalysis;

struct ResolvedCallGraphAnalysis
{
    using Result = ResolvedCallGraph;

    Result run(ResourceContainer& container, GlobalAnalysisManager& gam);

    ///依赖 ReferenceIndex / CallGraph / Class+Method 层次：任一被失效则本图连锁失效。
    static bool dependencyInvalidated(ResourceContainer& container, const PreservedAnalyses& pa,
                                      GlobalAnalysisManager::Invalidator& inv)
    {
        return inv.invalidate<ReferenceIndexAnalysis>(container, pa) ||
               inv.invalidate<CallGraphAnalysis>(container, pa) ||
               inv.invalidate<ClassHierarchyAnalysis>(container, pa) ||
               inv.invalidate<MethodHierarchyAnalysis>(container, pa);
    }
};

}   //namespace mjt::base
