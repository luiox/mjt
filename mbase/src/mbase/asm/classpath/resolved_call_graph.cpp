#include "mbase/asm/classpath/resolved_call_graph.hpp"

#include <cassert>
#include <unordered_set>

#include "jbytecode/core/opcodes.hpp"
#include "mbase/reference/reference_index_analysis.hpp"

namespace mjt::base {
namespace {

std::string methodKey(const std::string& owner, const std::string& name, const std::string& desc)
{
    return owner + "\n" + name + "\n" + desc;
}

std::string methodKey(const MethodRefKey& key)
{
    return methodKey(key.owner, key.name, key.desc);
}

std::string str(ca::str::Utf8StringRef value)
{
    return value.to_std_string();
}

const std::vector<ResolvedCallEdge>& emptyEdges()
{
    static const std::vector<ResolvedCallEdge> empty;
    return empty;
}

ResolvedCallKind classifyOpcode(ca::i32 opcode)
{
    using jbytecode::core::Opcodes;
    switch (opcode) {
    case Opcodes::INVOKESTATIC: return ResolvedCallKind::Static;
    case Opcodes::INVOKESPECIAL: return ResolvedCallKind::Special;
    case Opcodes::INVOKEVIRTUAL: return ResolvedCallKind::Virtual;
    case Opcodes::INVOKEINTERFACE: return ResolvedCallKind::Interface;
    default: return ResolvedCallKind::Unresolved;
    }
}

bool shouldExpand(ResolvedCallKind kind)
{
    return kind == ResolvedCallKind::Virtual || kind == ResolvedCallKind::Interface;
}

MethodRefKey methodKeyFromHierarchy(const ClassHierarchy& ch, const MethodHierarchy& mh,
                                    ca::i32 methodIndex)
{
    const auto& entry      = mh.entry(methodIndex);
    const auto& ownerEntry = ch.entry(entry.owner);
    auto*       classNode  = ownerEntry.classNode;
    auto*       methodNode = classNode->methods[static_cast<ca::usize>(entry.methodIndex)];
    return {str(ownerEntry.name), str(methodNode->name), str(methodNode->desc)};
}

std::unordered_map<std::string, ca::i32> buildHierarchyMethodLookup(const ClassHierarchy&  ch,
                                                                    const MethodHierarchy& mh)
{
    std::unordered_map<std::string, ca::i32> result;
    for (ca::i32 i = 0; i < mh.methodCount(); ++i) {
        auto key = methodKeyFromHierarchy(ch, mh, i);
        result.emplace(methodKey(key), i);
    }
    return result;
}

///@brief 把调用点编码目标解析为 MethodHierarchy 条目下标。
///
///三级口径（评审跟进卡1）：
///1. 精确：编码 owner 自身声明（常规路径，零额外开销）；
///2. 继承落边：编码 owner 是容器内已知类型但未声明该方法时，沿 superIndex
///父类链首中即停——与 buildRta 的 canonical RTA 解析（resolveClassChain）
///同一口径；类链无命中再沿接口继承链（ancestors 闭包，超类已在父类链
///落空，剩余命中必为接口条目）按 DFS 序首中。子类型静态接收者直调
///（invokevirtual Sub.run，Sub 继承 Base.run）与子接口未重声明
///（invokeinterface I2.m，I2 extends I1）由此落真实声明者；
///3. 缺失：owner 不在层次图（外部/缺失类）或全链无声明（含 broken 尾部、
///Object 外部方法）返回 -1，调用方维持既有"降级编码目标"口径。
///
///环防御：畸形 superIndex 环（同类根因）以 visited 集截断。仅命中失败
///路径触发链行走，精确命中热路径无额外开销。
ca::i32 resolveHierarchyMethod(const ClassHierarchy&                           classHierarchy,
                               const std::unordered_map<std::string, ca::i32>& hierarchyMethods,
                               const MethodRefKey&                             target)
{
    auto it = hierarchyMethods.find(methodKey(target));
    if (it != hierarchyMethods.end()) return it->second;

    auto lookup = [&](ClassIndex idx) -> ca::i32 {
        auto hit = hierarchyMethods.find(
            methodKey(str(classHierarchy.nameOf(idx)), target.name, target.desc));
        return hit == hierarchyMethods.end() ? -1 : hit->second;
    };

    const ClassIndex ownerIdx =
        classHierarchy.indexOf(ca::str::Utf8StringRef::from_cstr(target.owner.c_str()));
    if (ownerIdx == INVALID_CLASS) return -1;

    //父类链：首中即停（canonical 口径）。
    std::unordered_set<ClassIndex> visited;
    for (ClassIndex cur = ownerIdx; cur != INVALID_CLASS && visited.insert(cur).second;) {
        if (const ca::i32 hit = lookup(cur); hit >= 0) return hit;
        cur = classHierarchy.entry(cur).superIndex;
    }

    //接口继承链：ancestors 闭包 DFS 序首中（超类已全部落空）。
    const auto ancestors = classHierarchy.ancestors(ownerIdx);
    for (ca::usize i = 0; i < ancestors.size; ++i) {
        if (const ca::i32 hit = lookup(ancestors[i]); hit >= 0) return hit;
    }
    return -1;
}

void appendUnique(std::vector<MethodRefKey>& targets, const MethodRefKey& target)
{
    const std::string key = methodKey(target);
    for (const auto& existing : targets) {
        if (methodKey(existing) == key) return;
    }
    targets.push_back(target);
}

///@brief Java main 入口判定，与 global_reachability.cpp 的内建发现同口径。
bool isJavaMain(const MethodDeclaration& decl)
{
    constexpr ca::i32 requiredAccess =
        jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC;
    return decl.method.name == "main" && decl.method.desc == "([Ljava/lang/String;)V" &&
           (decl.access & requiredAccess) == requiredAccess;
}

}   //namespace

void ResolvedCallGraph::clear()
{
    edges_.clear();
    edgesBySource_.clear();
    edgesByTarget_.clear();
    instantiatedClasses_.clear();
    rtaInstantiatedCount_    = 0;
    rtaReachableMethodCount_ = 0;
}

void ResolvedCallGraph::setRtaSummaryCounters(ca::usize instantiatedCount, ca::usize reachableCount)
{
    rtaInstantiatedCount_    = instantiatedCount;
    rtaReachableMethodCount_ = reachableCount;
}

void ResolvedCallGraph::appendEdge(const ResolvedCallEdge& edge)
{
    edges_.push_back(edge);
    edgesBySource_[methodKey(edge.site.sourceClass, edge.site.sourceMethod, edge.site.sourceDesc)]
        .push_back(edge);
    edgesByTarget_[methodKey(edge.resolvedTarget)].push_back(edge);
}

void ResolvedCallGraph::build(const ReferenceIndex& index, const CallGraph& callGraph,
                              const ClassHierarchy&  classHierarchy,
                              const MethodHierarchy& methodHierarchy)
{
    (void)callGraph;
    clear();
    const auto hierarchyMethods = buildHierarchyMethodLookup(classHierarchy, methodHierarchy);

    for (const auto& ref : index.methodReferences()) {
        const ResolvedCallKind    kind = classifyOpcode(ref.site.opcode);
        std::vector<MethodRefKey> targets;

        if (shouldExpand(kind)) {
            const ca::i32 methodIdx =
                resolveHierarchyMethod(classHierarchy, hierarchyMethods, ref.target);
            if (methodIdx >= 0) {
                const ca::i32 root      = methodHierarchy.findComponentRoot(methodIdx);
                auto          component = methodHierarchy.componentMethods(root);
                for (ca::usize i = 0; i < component.size; ++i) {
                    appendUnique(
                        targets,
                        methodKeyFromHierarchy(classHierarchy, methodHierarchy, component[i]));
                }
            }
        }

        if (targets.empty()) {
            targets.push_back(ref.target);
        }

        for (const auto& target : targets) {
            ResolvedCallEdge edge;
            edge.site           = ref.site;
            edge.encodedTarget  = ref.target;
            edge.resolvedTarget = target;
            edge.kind           = kind;
            edge.expanded       = methodKey(target) != methodKey(ref.target);
            appendEdge(edge);
        }
    }
}

const std::vector<ResolvedCallEdge>& ResolvedCallGraph::edgesFrom(const std::string& owner,
                                                                  const std::string& name,
                                                                  const std::string& desc) const
{
    auto it = edgesBySource_.find(methodKey(owner, name, desc));
    return it == edgesBySource_.end() ? emptyEdges() : it->second;
}

const std::vector<ResolvedCallEdge>& ResolvedCallGraph::edgesTo(const std::string& owner,
                                                                const std::string& name,
                                                                const std::string& desc) const
{
    auto it = edgesByTarget_.find(methodKey(owner, name, desc));
    return it == edgesByTarget_.end() ? emptyEdges() : it->second;
}

std::vector<MethodRefKey> ResolvedCallGraph::resolvedTargetsFrom(const std::string& owner,
                                                                 const std::string& name,
                                                                 const std::string& desc) const
{
    std::vector<MethodRefKey> targets;
    for (const auto& edge : edgesFrom(owner, name, desc)) {
        appendUnique(targets, edge.resolvedTarget);
    }
    return targets;
}

std::vector<MethodRefKey> ResolvedCallGraph::reverseReachableTo(const std::string& owner,
                                                                const std::string& name,
                                                                const std::string& desc) const
{
    const std::string start = methodKey(owner, name, desc);
    std::unordered_set<std::string> visited;
    visited.insert(start);
    std::vector<MethodRefKey> result;
    std::vector<std::string>  queue;
    queue.push_back(start);

    ca::usize cursor = 0;
    while (cursor < queue.size()) {
        const std::string current = queue[cursor++];
        auto              it      = edgesByTarget_.find(current);
        if (it == edgesByTarget_.end()) continue;
        for (const auto& edge : it->second) {
            const std::string sourceKey =
                methodKey(edge.site.sourceClass, edge.site.sourceMethod, edge.site.sourceDesc);
            if (!visited.insert(sourceKey).second) continue;
            result.push_back(
                {edge.site.sourceClass, edge.site.sourceMethod, edge.site.sourceDesc});
            queue.push_back(sourceKey);
        }
    }
    return result;
}

bool ResolvedCallGraph::hasResolvedEdge(const std::string& sourceOwner,
                                        const std::string& sourceName,
                                        const std::string& sourceDesc,
                                        const std::string& targetOwner,
                                        const std::string& targetName,
                                        const std::string& targetDesc) const
{
    const std::string target = methodKey(targetOwner, targetName, targetDesc);
    for (const auto& edge : edgesFrom(sourceOwner, sourceName, sourceDesc)) {
        if (methodKey(edge.resolvedTarget) == target) return true;
    }
    return false;
}

ResolvedCallGraphAnalysis::Result ResolvedCallGraphAnalysis::run(ResourceContainer&     container,
                                                                 GlobalAnalysisManager& gam)
{
    auto& index           = gam.getResult<ReferenceIndexAnalysis>(container);
    auto& directGraph     = gam.getResult<CallGraphAnalysis>(container);
    auto& classHierarchy  = gam.getResult<ClassHierarchyAnalysis>(container);
    auto& methodHierarchy = gam.getResult<MethodHierarchyAnalysis>(container);

    ResolvedCallGraph graph;
    graph.build(index, directGraph, classHierarchy, methodHierarchy);
    return graph;
}

//── RTA（rapid type analysis）精度档 ─────────────────────────────
//
//入口可达性驱动 + 已实例化类型集合收窄虚分派。两个阶段：
//
//1. fixpoint：worklist 闭包同时增长「可达方法集」与「已实例化类型集」。
//可达方法体内的 `new T`（TypeInstruction + NEW）激活 T；T 激活时重估组件
//条目 owner 落在 T 祖先闭包上的全部已询问虚/接口调用点（晚发现的实例化
//不丢边）。虚/接口调用点的可行 dispatch 口径是 canonical RTA 的方法解析：
//从已实例化类型 T 沿父类链（ClassHierarchy superIndex 链）向上找第一个
//拥有该组件条目的类，运行时 dispatch 即落在该类名下的组件条目——子类
//继承父类具体方法且不覆写时执行的是父类条目，故祖先名下条目可行（继承
//未覆写不再欠近似）；类链命中即停，子类覆写时父类条目不可达（「父类已
//实例化」场景行为不变）；类链无命中且走满到 Object 时组件中的接口条目
//可行（接口 default 方法经 superinterface 继承，同类根因）；类链因
//broken/外部尾部截断时保守放行组件全部条目（宁过近似不丢真实执行边）。
//组件在册但整个闭包零实例化时调用点零边，不回退编码目标——相对 CHA 的
//精度收益来源。外部/缺失虚目标照 CHA 降级为编码目标。
//在 per-root feasible 之上另有调用点粒度接收者静态类型门控（siteReceiverAllows，
//评审 P3-1 引入，R3 评审 P1-(a) 重设计为站点级健全判据）：保留边 iff
//∃ 已实例化类型 S：S <: R（调用点接收者声明类型）且 owner ∈ ancestors(S)，
//另保留 R <: owner 保守补遗（子类型静态接收者直调/接口 default 经祖先条目），
//容器外类型 fail-open。「具体父类 × 无关接口」菱形的真实执行边由此不丢
//（宁过近似不丢真实执行边）。挂起 enqueue 与物化两条路径同口径；实例化
//闭包传播机制不受影响。
//2. 物化：按可达方法发现序 × 引用扫描序产出确定性边表；虚/接口组件按最终
//可行 dispatch 集过滤后逐目标出边（expanded 标记与 CHA 同口径），不可达
//方法的边整体缺席。
void ResolvedCallGraph::buildRta(const ReferenceIndex& index, const CallGraph& callGraph,
                                 const ClassHierarchy& classHierarchy,
                                 const MethodHierarchy& methodHierarchy, const RtaOptions& options)
{
    (void)callGraph;
    clear();

    const auto hierarchyMethods = buildHierarchyMethodLookup(classHierarchy, methodHierarchy);

    //── fixpoint 状态 ──
    std::unordered_set<std::string> reachableKeys;
    std::vector<MethodRefKey>       reachableOrder;
    std::vector<MethodRefKey>       worklist;
    std::unordered_set<std::string> instantiatedSet;
    //instantiatedClasses_ 的 ClassIndex 镜像（同序追加）：接收者门控逐 S 查询
    //用，免去门控热路径上对已实例化类型反复按名 indexOf；外部类型记
    //INVALID_CLASS，由门控跳过（外部实例的分派链全在外部，见 closureOf 口径）。
    std::vector<ClassIndex> instantiatedIdxs;

    //覆盖组件 root 的询价状态：组件目标、owner 集与「可行 dispatch owner」集。
    //feasible 的增长口径是方法解析（见上方 RTA 注释）：实例化类型沿父类链
    //命中的第一个组件 owner；接口成员与破损尾部按注释口径放行。组件由
    //MethodHierarchy 并查集按覆写关系闭包，同一 root 的全部调用点共享一份。
    struct RootState
    {
        std::vector<MethodRefKey>       component;
        std::unordered_set<std::string> ownerSet;
        std::unordered_set<std::string> feasible;
        ca::usize                       folded = 0;   //已折入的 instantiatedClasses_ 前缀
    };
    std::unordered_map<ca::i32, RootState>                rootStates;
    std::unordered_map<std::string, std::vector<ca::i32>> rootsByOwner;

    auto enqueue = [&](const MethodRefKey& method) {
        if (method.owner.empty() || method.name.empty() || method.desc.empty()) return;
        if (!reachableKeys.insert(methodKey(method)).second) return;
        reachableOrder.push_back(method);
        worklist.push_back(method);
    };

    //类链解析：从 type 沿 superIndex 链向上找第一个 ownerSet 命中类。
    //second = 是否走满（终止于 Object）；未走满说明父链有 broken/外部尾部，
    //解析结果不可知，调用方须保守放行。
    auto resolveClassChain = [&](const std::string&                       type,
                                 const std::unordered_set<std::string>& ownerSet) {
        std::pair<std::string, bool> result{"", false};
        const ClassIndex start =
            classHierarchy.indexOf(ca::str::Utf8StringRef::from_cstr(type.c_str()));
        ClassIndex cur = start;
        while (cur != INVALID_CLASS) {
            const std::string name = str(classHierarchy.nameOf(cur));
            if (ownerSet.count(name) != 0) return std::pair<std::string, bool>{name, true};
            const ClassIndex next = classHierarchy.entry(cur).superIndex;
            if (next == INVALID_CLASS) {
                result.second = (cur == classHierarchy.objectIndex());
                break;
            }
            cur = next;
        }
        return result;
    };

    //landed owner 名下的组件条目补入闭包（继承未覆写的目标由此进边）。
    //
    //口径固化（R3 评审 P3-1）：此处不受调用点接收者静态类型门控——可达方法集
    //按落点 owner 全量入闭包，是偏保守的有意设计（门控仅收窄边表的目标选取，
    //不收缩 worklist，从而不影响实例化发现）。边表收窄后会出现「方法可达但无
    //任何入边」的表里差异（R3 评审菱形探针中 A.run 在旧判据下即如此），属
    //设计选择，勿当缺陷修。
    auto enqueueFeasible = [&](RootState& rs, const std::string& owner) {
        for (const auto& target : rs.component) {
            if (target.owner == owner) enqueue(target);
        }
    };

    //T 的祖先闭包（ClassIndex 集，ancestors 含自身）；外部类型返回空集——
    //外部实例的分派链全在外部，落不到容器声明条目（与旧口径一致：外部实例化
    //不激活任何组件）。
    auto closureOf = [&](const std::string& type) {
        std::unordered_set<ClassIndex> closure;
        const ClassIndex tIdx =
            classHierarchy.indexOf(ca::str::Utf8StringRef::from_cstr(type.c_str()));
        if (tIdx == INVALID_CLASS) return closure;
        closure.insert(tIdx);
        const auto ancestors = classHierarchy.ancestors(tIdx);
        for (ca::usize i = 0; i < ancestors.size; ++i) closure.insert(ancestors[i]);
        return closure;
    };

    //调用点粒度接收者静态类型门控（评审 P3-1 引入；R3 评审 P1-(a) 重设计
    //为站点级健全判据）。per-root feasible 按组件共享，"接收者静态类型没有任何
    //已实例化子类型"的调用点也会经共享 feasible 落出与接收者无关 owner 的边；
    //落目标前按站点逐边裁决，保留边 iff：
    //
    //1. ∃ 已实例化类型 S：S <: R（调用点接收者声明类型）且 owner ∈ ancestors(S)
    //（ancestors 自含，即 S <: owner）。这是健全判据：运行时接收者必为某
    //已实例化 S 的实例且同时是 R 的子型，而分派只能落在该实例祖先闭包内的
    //组件条目上——「具体父类 × 无关接口」菱形（owner 与 R 互不为子型、但
    //经已实例化 S 的具体父类链可达，JVMS 5.4.5 具体父类方法优先，R.run→
    //A.run 是确定执行的真边）由此保留。旧双向判据（owner<:R ∨ R<:owner）
    //漏掉该分支，杀真边，违「宁过近似不丢真实执行边」信条（R3 评审红绿
    //实验确证）。注意新谓词并非旧谓词的逐点超集（谓词层面非纯放宽），
//成立的是更强的健全性不变量：不丢任何真实执行边（R9 评审 N1 澄清）。
    //2. R <: owner（原口径的保守补遗，评审卡1 反例B 的 I2.run→I1.run
    //边由此保留）：接收者静态类型本身是 owner 子型时，任何运行时接收者
    //（R 的子型）的祖先闭包都含 owner——调用点真执行时分派可落 owner，
    //即便当前尚无 R 的已实例化子型。过近似方向的安全放宽。
    //
//任一类型不在容器（外部/缺失）fail-open 维持原口径不收窄。裁决作用于挂起
//enqueue 与物化两条路径（同口径）：中间快照可能存在瞬态掉边，物化按终态
//已实例化集重门控，终局边表健全（R9 评审 N1 澄清）。enqueueFeasible 不过
//门控，可达方法集不受扰（见上）；实例化闭包传播机制不受影响。
//纯层级查询 + instantiatedIdxs 顺序扫描，结果与扫描序无关，确定性保持
//（同输入同边表）。
    auto siteReceiverAllows = [&](const std::string& receiverType, const std::string& owner) {
        if (receiverType == owner) return true;
        const ClassIndex receiverIdx =
            classHierarchy.indexOf(ca::str::Utf8StringRef::from_cstr(receiverType.c_str()));
        const ClassIndex ownerIdx =
            classHierarchy.indexOf(ca::str::Utf8StringRef::from_cstr(owner.c_str()));
        if (receiverIdx == INVALID_CLASS || ownerIdx == INVALID_CLASS) return true;
        if (classHierarchy.isSubType(receiverIdx, ownerIdx)) return true;
        for (ca::usize i = 0; i < instantiatedIdxs.size(); ++i) {
            const ClassIndex sIdx = instantiatedIdxs[i];
            if (sIdx == INVALID_CLASS) continue;
            if (classHierarchy.isSubType(sIdx, receiverIdx) &&
                classHierarchy.isSubType(sIdx, ownerIdx)) {
                return true;
            }
        }
        return false;
    };

    //用单个已实例化类型重估一个 root 的可行 dispatch 集（三口径见 RTA 注释）。
    //closure 为 T 的祖先闭包（含自身）；空集（外部类型）直接跳过。
    auto evaluateResolution = [&](RootState& rs, const std::string& type,
                                  const std::unordered_set<ClassIndex>& closure) {
        if (closure.empty()) return;
        const auto [landed, complete] = resolveClassChain(type, rs.ownerSet);
        if (!landed.empty()) {
            if (rs.feasible.insert(landed).second) enqueueFeasible(rs, landed);
            return;
        }
        if (!complete) {
            //broken/外部尾部截断：解析不可知，保守放行组件全部条目。
            for (const auto& target : rs.component) {
                if (rs.feasible.insert(target.owner).second) enqueue(target);
            }
            return;
        }
        //类链无声明且走满：组件中 T 祖先闭包内的接口条目可行——接口 default
        //方法经 superinterface 继承且容器类未覆写时，运行时执行的就是接口
        //条目。（T 未实现的组件接口不可能承接其分派，不得放行；多接口菱形
        //下不区分 max-specific，闭包内保守全收。）
        for (const auto& target : rs.component) {
            const ClassIndex ownerIdx =
                classHierarchy.indexOf(ca::str::Utf8StringRef::from_cstr(target.owner.c_str()));
            if (ownerIdx == INVALID_CLASS || !classHierarchy.isInterface(ownerIdx)) continue;
            if (closure.count(ownerIdx) == 0) continue;
            if (rs.feasible.insert(target.owner).second) enqueue(target);
        }
    };

    //折入 instantiatedClasses_[rs.folded..]：首次询问为全量，其后为增量。
    //
    //不变量固化（评审卡3）：循环体内 evaluateResolution → enqueueFeasible →
    //enqueue 链只增长可达集/worklist，绝不触发 instantiate——instantiate 仅由
    //process 的类型引用扫描调用（与 foldRoot 无调用边），物化阶段更是
    //fixpoint 结束后 instantiatedClasses_ 已封口。故循环内不增长：end 在
    //进入前快照即可，未来把下标循环改写成 range-for/迭代器也不会踩到
    //push_back 扩量后的陈旧 end UB。debug 断言固化该推理——若未来重构在
    //evaluateResolution 链上引入实例化，调试构建立即失火而不是静默漏折。
    auto foldRoot = [&](RootState& rs) {
        const ca::usize end = instantiatedClasses_.size();
        for (ca::usize i = rs.folded; i < end; ++i) {
            evaluateResolution(rs, instantiatedClasses_[i], closureOf(instantiatedClasses_[i]));
            assert(instantiatedClasses_.size() == end &&
                   "foldRoot 迭代期间 instantiatedClasses_ 不得增长（见上不变量）");
        }
        rs.folded = end;
    };

    auto instantiate = [&](const std::string& type) {
        if (type.empty()) return;
        if (!instantiatedSet.insert(type).second) return;
        instantiatedClasses_.push_back(type);
        //门控镜像（同序追加，含外部类型的 INVALID_CLASS 占位，见上声明处）。
        instantiatedIdxs.push_back(
            classHierarchy.indexOf(ca::str::Utf8StringRef::from_cstr(type.c_str())));

        //候选 root：组件条目 owner 落在 T 的祖先闭包内。owner 全在闭包外的
        //root 不可能被 T 的实例 dispatch 命中，无需重估。收集按 ancestors
        //扇平序（确定性），不按闭包集迭代序——候选评估序进入 enqueue 次序，
        //须保持边表确定性。外部类型不在层次图内：容器组件条目 owner 均为
        //容器内声明类，无候选可激活。
        auto closure = closureOf(type);
        if (closure.empty()) return;

        std::vector<ca::i32>        candidates;
        std::unordered_set<ca::i32> seenRoots;
        auto collect = [&](const std::string& owner) {
            auto it = rootsByOwner.find(owner);
            if (it == rootsByOwner.end()) return;
            for (ca::i32 root : it->second) {
                if (seenRoots.insert(root).second) candidates.push_back(root);
            }
        };
        collect(type);
        const ClassIndex tIdx =
            classHierarchy.indexOf(ca::str::Utf8StringRef::from_cstr(type.c_str()));
        const auto ancestors = classHierarchy.ancestors(tIdx);
        for (ca::usize i = 0; i < ancestors.size; ++i) {
            collect(str(classHierarchy.nameOf(ancestors[i])));
        }

        for (ca::i32 root : candidates) {
            auto rsIt = rootStates.find(root);
            if (rsIt == rootStates.end()) continue;
            evaluateResolution(rsIt->second, type, closure);
            rsIt->second.folded = instantiatedClasses_.size();
        }
    };

    auto process = [&](const MethodRefKey& method) {
        for (const auto& tref : index.typeReferencesFrom(method.owner, method.name, method.desc)) {
            if (tref.kind == TypeReferenceKind::TypeInstruction &&
                tref.site.opcode == jbytecode::core::Opcodes::NEW) {
                instantiate(tref.targetType);
            }
        }

        for (const auto& ref : index.methodReferencesFrom(method.owner, method.name, method.desc)) {
            //空描述符反射查找按名落解，口径与 GlobalReachability 对齐。
            if (ref.target.desc.empty()) {
                if (ref.kind == MethodReferenceKind::Reflection ||
                    ref.kind == MethodReferenceKind::ReflectionExecution) {
                    for (const auto& declaration : index.declaredMethodsIn(ref.target.owner)) {
                        if (declaration.method.name == ref.target.name) {
                            enqueue(declaration.method);
                        }
                    }
                }
                continue;
            }

            const ResolvedCallKind kind = classifyOpcode(ref.site.opcode);
            if (!shouldExpand(kind)) {
                enqueue(ref.target);
                continue;
            }

            //编码 owner 未声明时沿类链/接口链落真实声明者（与 CHA 档同源），
            //RTA 侧照旧受实例化集约束；owner 不在容器/全链无声明维持降级。
            const ca::i32 methodIdx =
                resolveHierarchyMethod(classHierarchy, hierarchyMethods, ref.target);
            if (methodIdx < 0) {
                enqueue(ref.target);   //外部/缺失目标：降级精确边
                continue;
            }

            const ca::i32 root = methodHierarchy.findComponentRoot(methodIdx);
            auto [rsIt, inserted] = rootStates.try_emplace(root);
            if (inserted) {
                auto component = methodHierarchy.componentMethods(root);
                for (ca::usize i = 0; i < component.size; ++i) {
                    auto target =
                        methodKeyFromHierarchy(classHierarchy, methodHierarchy, component[i]);
                    rsIt->second.component.push_back(target);
                    rsIt->second.ownerSet.insert(target.owner);
                    rootsByOwner[target.owner].push_back(root);
                }
            }
            foldRoot(rsIt->second);
            for (const auto& target : rsIt->second.component) {
                if (rsIt->second.feasible.count(target.owner) == 0) continue;
                //接收者静态类型门控：与物化同口径（P3-1）。
                if (!siteReceiverAllows(ref.target.owner, target.owner)) continue;
                enqueue(target);
            }
        }
    };

    //根集合：显式 rootMethods + 可选 main 发现。
    for (const auto& root : options.rootMethods) {
        enqueue(root);
    }
    if (options.includeMainMethods) {
        for (const auto& decl : index.methodDeclarations()) {
            if (isJavaMain(decl)) enqueue(decl.method);
        }
    }

    for (ca::usize cursor = 0; cursor < worklist.size(); ++cursor) {
        process(worklist[cursor]);
    }
    rtaInstantiatedCount_    = instantiatedClasses_.size();
    rtaReachableMethodCount_ = reachableOrder.size();

    //── 物化：按可达发现序 × 引用扫描序产出边表 ──
    for (const auto& source : reachableOrder) {
        for (const auto& ref : index.methodReferencesFrom(source.owner, source.name, source.desc)) {
            const ResolvedCallKind    kind = classifyOpcode(ref.site.opcode);
            std::vector<MethodRefKey> targets;

            if (shouldExpand(kind)) {
                //与 fixpoint 同一解析口径：编码 owner 未声明时沿链落真实
                //声明者；同一可达方法集的同一引用必解析出同一 root，故
                //fixpoint 对该 root 必已询问，find 不到按零可行口径处理。
                const ca::i32 methodIdx =
                    resolveHierarchyMethod(classHierarchy, hierarchyMethods, ref.target);
                if (methodIdx < 0) {
                    targets.push_back(ref.target);   //外部/缺失：降级编码目标
                }
                else {
                    const ca::i32 root = methodHierarchy.findComponentRoot(methodIdx);
                    auto          rsIt = rootStates.find(root);
                    if (rsIt != rootStates.end()) {
                        foldRoot(rsIt->second);
                        for (const auto& target : rsIt->second.component) {
                            if (rsIt->second.feasible.count(target.owner) == 0) continue;
                            //接收者静态类型门控：与挂起路径同口径（P3-1）。
                            if (!siteReceiverAllows(ref.target.owner, target.owner)) continue;
                            appendUnique(targets, target);
                        }
                    }
                    //组件在册但零可行 dispatch（闭包零实例化）：零边
                    //（RTA 精度语义，不回退编码目标）。
                }
            }
            else {
                targets.push_back(ref.target);
            }

            for (const auto& target : targets) {
                ResolvedCallEdge edge;
                edge.site           = ref.site;
                edge.encodedTarget  = ref.target;
                edge.resolvedTarget = target;
                edge.kind           = kind;
                edge.expanded       = methodKey(target) != methodKey(ref.target);
                appendEdge(edge);
            }
        }
    }
}

}   //namespace mjt::base
