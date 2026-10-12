#pragma once

#include <string>
#include <unordered_set>
#include <vector>

#include "mbase/asm/classpath/call_graph.hpp"
#include "mbase/asm/classpath/resolved_call_graph.hpp"
#include "mbase/reference/reflection_binding_analyzer.hpp"
#include "mbase/reference/reference_index.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

///@brief Method-call expansion policy for GlobalReachability.
enum class GlobalReachabilityCallExpansion
{
    ///@brief Use exact encoded direct call graph edges only.
    Direct,
    ///@brief Use `ResolvedCallGraph` virtual/interface target expansion.
    Resolved
};

///@brief Root policy for GlobalReachability.
///
///The reachability model is intentionally policy-light. Production cleanup
///passes should provide explicit roots from CLI configuration, entry-point
///discovery, keep rules, or product-specific contracts. The built-in main
///discovery is only a safe default for ordinary application jars.
struct GlobalReachabilityOptions
{
    ///@brief Classes that must be considered live before closure expansion.
    std::vector<std::string> rootClasses;

    ///@brief Methods that must be considered live before closure expansion.
    std::vector<MethodRefKey> rootMethods;

    ///@brief Fields that must be considered live before closure expansion.
    std::vector<FieldRefKey> rootFields;

    ///@brief Treat Java `public static main(String[])` declarations as roots.
    ///
    ///This keeps the default GlobalReachabilityAnalysis useful for application
    ///jars while still allowing library-oriented passes to disable the policy
    ///and supply explicit roots.
    bool includeMainMethods = true;

    ///@brief Mark a reachable class initializer as live with its class.
    ///
    ///Class initialization is a side-effect boundary. Keeping `<clinit>` with
    ///its class prevents later passes from deleting static initialization code
    ///merely because no direct call instruction targets it.
    bool includeClassInitializers = true;

    ///@brief Select whether reachable method bodies expand direct or resolved calls.
    ///
    ///The default keeps historical direct `CallGraph` behavior. Passes that
    ///need hierarchy-aware liveness can call the resolved build overload or use
    ///`ResolvedGlobalReachabilityAnalysis`.
    GlobalReachabilityCallExpansion callExpansion = GlobalReachabilityCallExpansion::Direct;

    ///@brief Skip the raw encoded method-reference expansion in Resolved mode.
    ///
    ///Under a profile-narrowed resolved graph (RTA), the encoded call-site
    ///target may be absent from the edge table (dispatch narrowed to zero
    ///feasible targets). Re-enqueuing raw encoded targets would re-import
    ///owners the profile removed, making the reachable closure disagree with
    ///the resolved edge table. When set together with
    ///`callExpansion == Resolved`, method bodies expand through resolved edges
    ///only; the empty-descriptor reflection by-name expansion is kept because
    ///the RTA build applies the same policy inside its own closure. Direct
    ///mode and the plain Resolved mode (CHA) are unaffected.
    bool resolvedEdgesExclusive = false;
};

///@defgroup global_reachability 全局可达性闭包
///
///保守的全局可达性闭包。结合 ReferenceIndex 声明/引用事实和调用图边，为后续
///清理 pass 提供稳定的查询：哪些 class/method/field 从选定根可达。
///
///## 目标
///
///`GlobalReachability` 是 analysis-query 基础的第一个 P3 消费者。它结合直接
///引用事实和直接调用图边，对声明的 class/method/field 计算保守闭包。
///
///模块不删除字节码。它给后续 pass 一个在删除前的稳定查询：
///
///- 此声明的 class 是否可达？
///- 此声明的 method 是否可达？
///- 此声明的 field 是否可达？
///- 哪些声明的条目从选定根当前不可达？
///
///## 输入
///
///可达性闭包由以下构成：
///
///- `ReferenceIndex`：声明、class-header 引用、描述符类型、方法体类型引用、字段引用
///- `CallGraph`：精确编码的直接调用边
///- `ResolvedCallGraph`：可选的 hierarchy 感知虚/接口调用边
///- `GlobalReachabilityOptions`：显式 keep 根、可选的 Java `main` 发现、类初始化器
///保留和调用扩展模式
///
///默认 analysis wrapper 使用直接 `CallGraph` 模式保持现有语义。需要 hierarchy 感知
///活跃度的消费者使用 `ResolvedGlobalReachabilityAnalysis` 或
///`GlobalReachabilityCallExpansion::Resolved` 构建重载。
///
///## 闭包规则
///
///闭包使用独立的 class、method 和 field 队列。
///
///Class 可达性只扩展声明级类型事实：
///
///- 直接超类
///- 直接实现的接口
///- 字段声明描述符对象类型
///
///这有意比 `typeReferencesFromClass` 整体更窄。可达 class 不自动使该 class 中
///每个方法体生效。方法体仅在方法通过根或调用边成为可达时才展开。
///
///Method 可达性扩展：
///
///- owner class
///- 直接调用图出边（或 resolved 模式下的解析图边）
///- `ReferenceIndex` 中的精确方法指令引用
///- 方法体中的字段访问
///- 方法体和描述符类型引用
///
///Resolved 模式下使用 `ResolvedCallGraph` 替代精确的直接方法引用视图。
///字段和类型引用仍来自 `ReferenceIndex`，因为它们不涉及分派。RTA 等收窄档
///可再置 `resolvedEdgesExclusive`：编码方法引用不再单独入队，方法扩展只走
///解析边集，闭包与边表严格同源（空描述符反射按名落解保留，与 RTA fixpoint
///同口径）。
///
///反射动态边：`weaveReflectionBindings` 把反射绑定分析器的方法级 Resolved
///绑定追加为 `ResolvedCallKind::Reflect` 边。织入后的边与静态 CHA 边同路参与
///resolved 模式闭包，统计口径分列；direct 模式与默认 analysis wrapper 不受
///影响。
///
///Field 可达性扩展 owner class。字段描述符类型引用已由声明级 class 扩展覆盖，
///这对后续未使用字段清理是保守的。
///
///## 根策略
///
///生产清理 pass 应从 CLI keep 规则、入口点发现、框架契约或产品特定策略传递显式根。
///
///默认 analysis wrapper 包含 Java `main(String[])` 方法作为根，使普通应用 jar
///无需额外配置即可产生有用闭包。库导向的删除 pass 应直接实例化
///`GlobalReachability` 并禁用 `includeMainMethods`（除非调用者提供 keep 根）。
///
///类初始化器默认随可达 class 保留。`<clinit>` 是副作用边界，仅因没有直接调用指令
///指向它就删除是不安全的。
///
///## 消费者计划
///
///首个预期消费者：
///
///1. `UnusedClassRemover`：删除 class 闭包外的声明 class
///2. `UnusedFieldRemover`：删除 field 闭包外的声明 field
///3. 跨 class `GlobalUnusedMethodRemover`：删除 method 闭包外的声明 method
///
///每个消费者仍拥有删除策略。例如，清理 pass 可能保留 public API class、构造函数、
///注解或反射敏感名称，即使原始可达性闭包标记它们不可达。
///
///首个清理消费者现在使用解析的可达性，因为它对破坏性删除更安全。
///`UnusedClassRemover` 解析每个 `ClassResource` 比较声明类名和可达 class 集合，
///保留格式错误的 class 字节和非 class 资源，当默认分析未发现根时无操作。
///`UnusedFieldRemover` 和 `GlobalUnusedMethodRemover` 当前只从可达 class 中删除
///private member。这避免在 CLI keep 规则和库入口策略前删除库 jar、虚/接口目标
///或 public API。
///
///## 验证
///
///`GlobalReachabilityTest` 构建内存中 class 集合，包含：
///
///- 应用入口方法
///- 直接超类和接口声明
///- 描述符引用另一个 class 的静态字段
///- 直接方法调用
///- 未使用 class 和未使用 method
///
///测试验证显式根、默认 Java `main` 根、类初始化器保留、声明不可达快照、
///`GlobalAnalysisManager` 集成和虚 override 目标的 resolved 模式扩展。
class GlobalReachability
{
public:
    ///@brief Rebuild the closure from direct reference and call graph analyses.
    ///
    ///Existing results are discarded. The resulting vectors preserve first-seen
    ///discovery order, which is stable enough for diagnostics and focused tests
    ///but should not be treated as a semantic ordering contract.
    void build(const ReferenceIndex& index, const CallGraph& callGraph,
               const GlobalReachabilityOptions& options = {});

    ///@brief Rebuild the closure with optional hierarchy-aware call expansion.
    ///
    ///When `options.callExpansion` is `Resolved`, reachable method bodies use
    ///`resolvedCallGraph` edges instead of exact direct edges. Other closure
    ///rules are identical to the direct build overload.
    void build(const ReferenceIndex& index, const CallGraph& callGraph,
               const ResolvedCallGraph&         resolvedCallGraph,
               const GlobalReachabilityOptions& options = {});

    ///@brief Remove all reachable sets and snapshots.
    void clear();

    ///@brief Return true if a class name is in the reachable closure.
    bool isClassReachable(const std::string& owner) const;

    ///@brief Return true if a method key is in the reachable closure.
    bool isMethodReachable(const std::string& owner, const std::string& name,
                           const std::string& desc) const;

    ///@brief Return true if a field key is in the reachable closure.
    bool isFieldReachable(const std::string& owner, const std::string& name,
                          const std::string& desc) const;

    ///@brief Reachable classes in first-seen discovery order.
    const std::vector<std::string>& reachableClasses() const { return reachableClasses_; }

    ///@brief Reachable methods in first-seen discovery order.
    const std::vector<MethodRefKey>& reachableMethods() const { return reachableMethods_; }

    ///@brief Reachable fields in first-seen discovery order.
    const std::vector<FieldRefKey>& reachableFields() const { return reachableFields_; }

    ///@brief Root methods that seeded the closure, in enqueue order.
    ///
    ///This is the union of `GlobalReachabilityOptions::rootMethods` and the
    ///`main(String[])` declarations discovered when `includeMainMethods` is
    ///enabled. It lets callers report entry counts and entry lists without
    ///re-running main discovery. Valid only after build; empty after clear.
    const std::vector<MethodRefKey>& entryMethods() const { return entryMethods_; }

    ///@brief Declared classes not present in the reachable class closure.
    std::vector<ClassDeclaration> unreachableDeclaredClasses(const ReferenceIndex& index) const;

    ///@brief Declared methods not present in the reachable method closure.
    std::vector<MethodDeclaration> unreachableDeclaredMethods(const ReferenceIndex& index) const;

    ///@brief Declared fields not present in the reachable field closure.
    std::vector<FieldDeclaration> unreachableDeclaredFields(const ReferenceIndex& index) const;

private:
    void buildInternal(const ReferenceIndex& index, const CallGraph& callGraph,
                       const ResolvedCallGraph*         resolvedCallGraph,
                       const GlobalReachabilityOptions& options);

    bool markClass(const std::string& owner);
    bool markMethod(const MethodRefKey& method);
    bool markField(const FieldRefKey& field);

    std::unordered_set<std::string> reachableClassKeys_;
    std::unordered_set<std::string> reachableMethodKeys_;
    std::unordered_set<std::string> reachableFieldKeys_;
    std::vector<std::string>        reachableClasses_;
    std::vector<MethodRefKey>       reachableMethods_;
    std::vector<FieldRefKey>        reachableFields_;
    std::vector<MethodRefKey>       entryMethods_;
};

///@brief GlobalAnalysisManager wrapper for the default reachability policy.
///
///The default policy includes Java main methods and class initializers. Passes
///that need keep-rule or library-specific roots should instantiate
///GlobalReachability directly with their own GlobalReachabilityOptions.
//前向声明：dependencyInvalidated 只需类型名（analysisID / inv.invalidate），无需完整定义。
struct ReferenceIndexAnalysis;
struct CallGraphAnalysis;
struct ResolvedCallGraphAnalysis;

struct GlobalReachabilityAnalysis
{
    using Result = GlobalReachability;

    Result run(ResourceContainer& container, GlobalAnalysisManager& gam);

    ///依赖 ReferenceIndex / CallGraph：任一被失效则可达性连锁失效（传递失效）。
    static bool dependencyInvalidated(ResourceContainer& container, const PreservedAnalyses& pa,
                                      GlobalAnalysisManager::Invalidator& inv)
    {
        return inv.invalidate<ReferenceIndexAnalysis>(container, pa) ||
               inv.invalidate<CallGraphAnalysis>(container, pa);
    }
};

///@brief GlobalAnalysisManager wrapper using hierarchy-aware call expansion.
///
///This keeps the default direct analysis stable while giving P3 consumers an
///explicit analysis type for virtual/interface-aware liveness.
struct ResolvedGlobalReachabilityAnalysis
{
    using Result = GlobalReachability;

    Result run(ResourceContainer& container, GlobalAnalysisManager& gam);

    ///依赖 ReferenceIndex / CallGraph / ResolvedCallGraph：任一被失效则连锁失效。
    static bool dependencyInvalidated(ResourceContainer& container, const PreservedAnalyses& pa,
                                      GlobalAnalysisManager::Invalidator& inv)
    {
        return inv.invalidate<ReferenceIndexAnalysis>(container, pa) ||
               inv.invalidate<CallGraphAnalysis>(container, pa) ||
               inv.invalidate<ResolvedCallGraphAnalysis>(container, pa);
    }
};

///@brief 计量报告：反射绑定织入调用图的结果分布。
///
///各字段口径：
///- `sites`：方法级执行 sink 绑定数（Method.invoke / Constructor.newInstance /
///Class.newInstance，含 Pending）。
///- `resolved`：其中 state == Resolved 的绑定数（含织入复核未过者，与 edges
///分列：Resolved-but-dropped 时 resolved > edges）。
///- `edges`：实际织入图的边数（Resolved 且目标命中容器内声明）。
///- `dropped`：未织入的方法级绑定数 = sites - edges。包含静态无法落解
///（Pending）与 Resolved 但目标未落入容器声明（容器边界、描述符口径差异）
///两类，归因不静默丢。
///- `classes`：类级 sink（Class.forName / ClassLoader.loadClass）Resolved 的
///去重目标类数。类加载不产生调用边，仅计量。
struct ReflectionWeaveReport
{
    ca::usize sites    = 0;
    ca::usize resolved = 0;
    ca::usize edges    = 0;
    ca::usize dropped  = 0;
    ca::usize classes  = 0;
};

///@brief 把反射绑定分析器的方法级 Resolved 绑定织入既有解析调用图。
///
///对每个 `ReflectionBinding`：
///
///- 方法级执行 sink（Method.invoke / Constructor.newInstance / Class.newInstance）
///且 state == Resolved：从绑定调用点所在方法（site.source*）向
///`binding.target` 追加一条 `ResolvedCallKind::Reflect` 动态边。encoded 与
///resolved 目标同为反射落解目标（反射边没有字节码编码目标）。目标未命中
///`ReferenceIndex` 声明时计入 `dropped`，不产生边。
///- 类级 sink（Class.forName / ClassLoader.loadClass）且 Resolved：目标类名进
///`classes` 计量，不产生边。
///- 字段访问与成员查找 sink：不产生边（字段访问不进调用图；查找本身由对应
///执行 sink 承载）。
///
///`GlobalReachability` 的 resolved 扩展模式读取 `edgesFrom`，织入后反射边与
///静态 CHA 边同路参与可达闭包；不改变两个既有设施任何既有接口语义。
///
///@param graph 既有解析调用图，原位追加 Reflect 边（build 会丢弃）。
///@param index 容器引用事实索引，用于目标存在性的织入端复核。
///@param bindings 反射绑定分析器的全量绑定列表。
///@return 织入计量报告（字段口径见 ReflectionWeaveReport）。
ReflectionWeaveReport weaveReflectionBindings(ResolvedCallGraph& graph, const ReferenceIndex& index,
                                              const std::vector<ReflectionBinding>& bindings);

}   //namespace mjt::base
