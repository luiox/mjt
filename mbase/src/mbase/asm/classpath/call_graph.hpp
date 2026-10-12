#pragma once

///@defgroup call_graph 调用图模块
///
///`CallGraph` 是位于 `ReferenceIndex` 之上的直接方法调用连接层。
///它将确切的 `MethodInsnNode` 引用和已确认的反射执行事实转换为图节点和边，
///同时将继承、虚分派扩展和活跃性策略保持在图之外。
///
///## 架构
///
///- `ReferenceIndex` 负责扫描类资源并记录精确的字节码事实。
///- `CallGraph` 负责图节点、内部方法标识符以及入边/出边集合。
///- `CallGraphAnalysis` 从 `ReferenceIndexAnalysis` 构建，
///使全局通行证可以共享一次扫描并同时查询引用位置和图连通性。
///
///节点以 `(owner, name, desc)` 为键。已扫描类资源中声明的方法标记为
///`isDeclared == true` 并保留访问标志。在扫描资源集中未声明的被引用方法
///仍可作为外部节点保留（默认行为），从而保留对 JDK、库和缺失类的调用，
///用于报告和可达性边界逻辑。
///
///## 边模型
///
///边是精确编码的调用加上已确认的方法局部反射执行：
///
///- 直接、方法句柄和 lambda 方法引用仍然是图边；
///- 仅当 `ReferenceIndex` 证明目标并解析其描述符时，
///`Method.invoke`、`Class.newInstance` 和 `Constructor.newInstance` 才成为图边；
///- `Class.getMethod`/`getDeclaredMethod` 和构造函数查找事实本身不会创建图边。
///
///- `getOutgoingCalls` 返回当前节点调用的方法。
///- `getIncomingCalls` 返回调用当前节点的方法。
///- `outgoingCallsOf(owner, name, desc)` 和 `incomingCallsOf(owner, name, desc)`
///直接从图公开相同方向，缺失节点返回空集。
///- `callSiteCountOf(owner, name, desc)` 返回指向该方法的调用点数（不去重
///调用方），供内联等按调用次数决策的消费者使用。
///
///保留旧 Java 兼容名称 `getCallers` 和 `getCallees` 以保持源代码兼容性，
///但新代码应优先使用显式的 incoming/outgoing 名称，因为它们匹配图方向。
///
///图还公开用于下游全程序通行证的小型节点过滤器：已声明节点、外部节点和
///同一类拥有的节点。这些辅助函数不暗示可达性或虚分派策略；
///它们仅对节点表中已有的直接图事实进行分区。
///
///直接调用可达性辅助函数沿两个方向遍历图。它们用于影响分析、保守失效
///和首次未使用成员实验。闭包仍限于精确编码的调用边，不扩展可能的虚目标。
///`hasPath` 为热查询位置提供相同直接边语义的布尔谓词。
///
///## 范围
///
///本图不解析虚分派、接口实现者、桥接方法、任意 invokedynamic 引导目标、
///反射字符串、仅查找反射事实、过程间反射别名或层次闭包。
///需要可能运行时目标语义的层次应结合 `CallGraph` 与类和方法层次分析。
///
///`ResolvedCallGraph` 是独立的层次感知层，它消费直接图加上
///`ClassHierarchy`/`MethodHierarchy`，扩展 `invokevirtual`/`invokeinterface`
///调用点而不改变此处记录的直接图契约。
///
///## 验证
///
///单元测试覆盖：
///
///- 资源容器图构建；
///- 已声明项目节点和外部库节点；
///- 入边和出边查询；
///- 调用点计数查询（callSiteCountOf，同一调用方多处调用分别计数）；
///- 可选的外部目标丢弃；
///- 仅查找反射事实的排除和已确认反射执行边的包含；
///- 已声明/外部/拥有者节点过滤器；
///- 正向和反向直接调用可达性闭包；
///- 通过 `CallGraphAnalysis` 的 `GlobalAnalysisManager` 集成。

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mbase/util/triple.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/reference/reference_index.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

///@brief Build policy for CallGraph.
struct CallGraphOptions
{
    ///@brief Keep targets that are referenced but not declared in the scanned
    ///resource set.
    ///
    ///When true, calls into JDK/library/missing classes become graph nodes with
    ///isDeclared == false. This preserves edges for reporting and reachability
    ///frontiers without pretending those methods are project-local.
    bool includeExternalTargets = true;
};

///@brief 调用图节点：唯一标识一个方法 (owner, name, desc)。
///
///对应 Java 版 asm.classpath.call.CallGraphNode。
///owner/name/desc 为 Utf8StringRef，指向所属 CallGraph 的 arena。
///
///注意（沿用 Java 语义，字段名保持一致）：
///- callers：本方法所「调用」的方法集合（即出边目标）
///- callees：「调用」本方法的方法集合（即入边来源）
///字段命名与直觉相反，但与 Java 版行为一致，便于对照移植。
class CallGraphNode
{
public:
    CallGraphNode() = default;
    CallGraphNode(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                  ca::str::Utf8StringRef desc)
        : owner_(owner)
        , name_(name)
        , desc_(desc)
    {}

    ca::str::Utf8StringRef getOwner() const { return owner_; }
    ca::str::Utf8StringRef getName() const { return name_; }
    ca::str::Utf8StringRef getDesc() const { return desc_; }
    ca::i32                getAccess() const { return access_; }
    bool                   isDeclared() const { return declared_; }

    void addCaller(CallGraphNode* caller)
    {
        if (caller) callers_.insert(caller);
    }
    void addCallee(CallGraphNode* callee)
    {
        if (callee) callees_.insert(callee);
    }
    void markDeclared(ca::i32 access)
    {
        declared_ = true;
        access_   = access;
    }

    const std::unordered_set<CallGraphNode*>& getCallers() const { return callers_; }
    const std::unordered_set<CallGraphNode*>& getCallees() const { return callees_; }
    const std::unordered_set<CallGraphNode*>& getOutgoingCalls() const { return callers_; }
    const std::unordered_set<CallGraphNode*>& getIncomingCalls() const { return callees_; }

    ///@brief 指向本方法的调用点数（按边添加次数计，同一调用方的多处调用分别计数）。
    ///
    ///与 getIncomingCalls（去重调用方集合）互补：内联等需要"共几个调用点"
    ///的消费者应使用本计数。
    ca::i32 incomingCallSiteCount() const { return incomingCallSites_; }

    void addIncomingCallSite() { ++incomingCallSites_; }

private:
    ca::str::Utf8StringRef             owner_;
    ca::str::Utf8StringRef             name_;
    ca::str::Utf8StringRef             desc_;
    ca::i32                            access_   = 0;
    bool                               declared_ = false;
    std::unordered_set<CallGraphNode*> callers_;   ///< 本方法调用的方法
    std::unordered_set<CallGraphNode*> callees_;   ///< 调用本方法的方法
    ca::i32                            incomingCallSites_ = 0;   ///< 入边（调用点）次数
};

///@brief 方法调用图。
///
///对应 Java 版 asm.classpath.call.CallGraph。
///两遍构建：第一遍把所有方法加入为节点，第二遍遍历每个方法的指令，
///对其中的 MethodInsn 建立调用边。
///
///节点以 (owner, name, desc) 三元组（均为 arena 内 Utf8StringRef）为键，
///存于 node-based 的 unordered_map，rehash 不会使节点指针失效，
///故 callers/callees 可安全持有 CallGraphNode*。
class CallGraph
{
public:
    using Key = Triple<ca::str::Utf8StringRef, ca::str::Utf8StringRef, ca::str::Utf8StringRef>;

    ///@brief 按 (owner, name, desc) 查方法节点。
    ///@return 节点指针；不存在返回 nullptr。
    CallGraphNode*       getMethod(const ca::str::Utf8StringRef& owner,
                                   const ca::str::Utf8StringRef& name,
                                   const ca::str::Utf8StringRef& desc);
    const CallGraphNode* getMethod(const ca::str::Utf8StringRef& owner,
                                   const ca::str::Utf8StringRef& name,
                                   const ca::str::Utf8StringRef& desc) const;

    ///@brief 按 std::string owner/name/desc 查方法节点。
    CallGraphNode*       getMethod(const std::string& owner, const std::string& name,
                                   const std::string& desc);
    const CallGraphNode* getMethod(const std::string& owner, const std::string& name,
                                   const std::string& desc) const;

    const std::unordered_map<Key, CallGraphNode>& getMethods() const { return methods_; }

    ///@brief Return methods directly called by the requested method.
    ///
    ///This convenience API uses graph terminology instead of the legacy
    ///`callers`/`callees` field names. Missing methods return an empty set.
    const std::unordered_set<CallGraphNode*>& outgoingCallsOf(const std::string& owner,
                                                              const std::string& name,
                                                              const std::string& desc) const;
    const std::unordered_set<CallGraphNode*>& outgoingCallsOf(
        const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& desc) const;

    ///@brief Return methods that directly call the requested method.
    ///
    ///Missing methods return an empty set. Returned node pointers are owned by
    ///this CallGraph and stay valid until the graph is rebuilt or cleared.
    const std::unordered_set<CallGraphNode*>& incomingCallsOf(const std::string& owner,
                                                              const std::string& name,
                                                              const std::string& desc) const;
    const std::unordered_set<CallGraphNode*>& incomingCallsOf(
        const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
        const ca::str::Utf8StringRef& desc) const;

    ///@brief 指向 (owner, name, desc) 的调用点数（含同一调用方的多处调用）。
    ///
    ///节点不存在或无入边返回 0。与 incomingCallsOf 的去重集合互补，
    ///供"单次调用才内联"类消费者使用。
    ca::i32 callSiteCountOf(const std::string& owner, const std::string& name,
                            const std::string& desc) const;
    ca::i32 callSiteCountOf(const ca::str::Utf8StringRef& owner,
                            const ca::str::Utf8StringRef& name,
                            const ca::str::Utf8StringRef& desc) const;

    ///@brief Return declared project-local method nodes.
    ///
    ///Nodes are owned by the graph and remain valid until the graph is rebuilt
    ///or cleared. The returned vector is a snapshot of matching node pointers.
    std::vector<const CallGraphNode*> declaredMethods() const;

    ///@brief Return referenced method nodes that were not declared in the scanned resources.
    std::vector<const CallGraphNode*> externalMethods() const;

    ///@brief Return all graph nodes owned by one internal JVM class name.
    std::vector<const CallGraphNode*> methodsOwnedBy(const std::string& owner) const;
    std::vector<const CallGraphNode*> methodsOwnedBy(const ca::str::Utf8StringRef& owner) const;

    ///@brief Return the direct-call transitive closure from one method.
    ///
    ///The start node itself is not included. Edges are the exact encoded call
    ///graph edges only; virtual dispatch expansion and hierarchy policy remain
    ///outside this layer.
    std::vector<const CallGraphNode*> reachableFrom(const std::string& owner,
                                                    const std::string& name,
                                                    const std::string& desc) const;
    std::vector<const CallGraphNode*> reachableFrom(const ca::str::Utf8StringRef& owner,
                                                    const ca::str::Utf8StringRef& name,
                                                    const ca::str::Utf8StringRef& desc) const;

    ///@brief Return the reverse direct-call transitive closure to one method.
    ///
    ///The target node itself is not included. This is useful for impact
    ///analysis and conservative invalidation of methods that may reach a
    ///changed callee.
    std::vector<const CallGraphNode*> reverseReachableTo(const std::string& owner,
                                                         const std::string& name,
                                                         const std::string& desc) const;
    std::vector<const CallGraphNode*> reverseReachableTo(const ca::str::Utf8StringRef& owner,
                                                         const ca::str::Utf8StringRef& name,
                                                         const ca::str::Utf8StringRef& desc) const;

    ///@brief Return true when a direct-call path exists from source to target.
    ///
    ///The path uses the same exact encoded edges as reachableFrom. Source and
    ///target may be the same method; in that case the result is true only when
    ///a non-empty cycle reaches the source again.
    bool hasPath(const std::string& sourceOwner, const std::string& sourceName,
                 const std::string& sourceDesc, const std::string& targetOwner,
                 const std::string& targetName, const std::string& targetDesc) const;
    bool hasPath(const ca::str::Utf8StringRef& sourceOwner,
                 const ca::str::Utf8StringRef& sourceName, const ca::str::Utf8StringRef& sourceDesc,
                 const ca::str::Utf8StringRef& targetOwner,
                 const ca::str::Utf8StringRef& targetName,
                 const ca::str::Utf8StringRef& targetDesc) const;

    ///@brief 由一组 class 字节构建调用图。
    void buildGraph(const std::vector<std::vector<ca::u8>>& classBytesList,
                    const CallGraphOptions&                 options = {});

    ///@brief Build from a resource container by first collecting direct
    ///references with ReferenceIndex.
    void build(ResourceContainer& container, ca::str::Utf8StringArena& arena,
               const CallGraphOptions& options = {});

    ///@brief Build from an already computed ReferenceIndex.
    ///
    ///This is the preferred entry point for pass pipelines that already request
    ///ReferenceIndexAnalysis, because it avoids reparsing class resources.
    void buildFromIndex(const ReferenceIndex& index, const CallGraphOptions& options = {});

    ///@brief 方法节点数量。
    std::size_t size() const { return methods_.size(); }

    ///@brief Remove all nodes and interned names.
    void clear();

private:
    //加入方法节点（幂等），返回节点指针。名字 intern 进 arena。
    CallGraphNode* addMethod(const ca::str::Utf8StringRef& owner,
                             const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc,
                             bool declared = false, ca::i32 access = 0);

    ///-- arena 死亡点声明（memory-governance-plan §5-P1③）--
    ///本成员持有 CallGraph 私有的 Utf8StringArena：addMethod intern 的全部
    ///方法名/类名/描述符。getMethod/getMethods/outgoingCallsOf/incomingCallsOf/
    ///reachableFrom/reverseReachableTo 返回的 CallGraphNode（及其 owner/name/desc
    ///视图）均指向本 arena。
    ///视图寿命 = 本 CallGraph 寿命；死亡点：随 GlobalAnalysisManager 的 per-unit
    ///缓存条目析构——CallGraphAnalysis::run 按值移入缓存后，由 PassManager 每 pass
    ///后按 PreservedAnalyses invalidate、clear/clearAll 或管理器析构时释放。下游
    ///ResolvedCallGraphAnalysis 与本分析连锁失效，且其 MethodRefKey 为 std::string
    ///深拷贝，不持本 arena 视图。
    ///消费者（method_inliner / global_reachability / unused_member_remover 等）
    ///不得把视图带过本缓存条目的失效点；需跨期持有则 intern 进自己的 arena 或深拷贝。
    ca::str::Utf8StringArena arena_;
    std::unordered_map<Key, CallGraphNode> methods_;
};

///@brief Global analysis wrapper for CallGraph.
///
///The graph is built from ReferenceIndexAnalysis so whole-program passes can
///share one class scan and then query both exact reference sites and graph
///connectivity.
//前向声明：dependencyInvalidated 只需类型名（analysisID / inv.invalidate），无需完整定义。
struct ReferenceIndexAnalysis;

struct CallGraphAnalysis
{
    using Result = CallGraph;

    Result run(ResourceContainer& container, GlobalAnalysisManager& gam);

    ///依赖 ReferenceIndexAnalysis：引用索引被失效则调用图连锁失效（传递失效）。
    static bool dependencyInvalidated(ResourceContainer& container, const PreservedAnalyses& pa,
                                      GlobalAnalysisManager::Invalidator& inv)
    {
        return inv.invalidate<ReferenceIndexAnalysis>(container, pa);
    }
};

}   //namespace mjt::base
