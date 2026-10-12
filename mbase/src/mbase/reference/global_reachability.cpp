#include "mbase/reference/global_reachability.hpp"

#include <unordered_set>

#include "jbytecode/jbytecode.hpp"
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

std::string fieldKey(const std::string& owner, const std::string& name, const std::string& desc)
{
    return owner + "\n" + name + "\n" + desc;
}

std::string fieldKey(const FieldRefKey& key)
{
    return fieldKey(key.owner, key.name, key.desc);
}

std::string str(ca::str::Utf8StringRef value)
{
    return value.to_std_string();
}

bool isDeclarationTypeReference(TypeReferenceKind kind)
{
    return kind == TypeReferenceKind::ClassSuper || kind == TypeReferenceKind::ClassInterface ||
           kind == TypeReferenceKind::FieldDeclarationDescriptor ||
           kind == TypeReferenceKind::Signature || kind == TypeReferenceKind::Annotation ||
           kind == TypeReferenceKind::StructuralMetadata;
}

bool isJavaMain(const MethodDeclaration& decl)
{
    constexpr ca::i32 requiredAccess =
        jbytecode::core::Opcodes::ACC_PUBLIC | jbytecode::core::Opcodes::ACC_STATIC;
    return decl.method.name == "main" && decl.method.desc == "([Ljava/lang/String;)V" &&
           (decl.access & requiredAccess) == requiredAccess;
}

}   //namespace

void GlobalReachability::clear()
{
    reachableClassKeys_.clear();
    reachableMethodKeys_.clear();
    reachableFieldKeys_.clear();
    reachableClasses_.clear();
    reachableMethods_.clear();
    reachableFields_.clear();
    entryMethods_.clear();
}

bool GlobalReachability::markClass(const std::string& owner)
{
    if (owner.empty()) return false;
    if (!reachableClassKeys_.insert(owner).second) return false;
    reachableClasses_.push_back(owner);
    return true;
}

bool GlobalReachability::markMethod(const MethodRefKey& method)
{
    if (method.owner.empty() || method.name.empty() || method.desc.empty()) return false;
    if (!reachableMethodKeys_.insert(methodKey(method)).second) return false;
    reachableMethods_.push_back(method);
    return true;
}

bool GlobalReachability::markField(const FieldRefKey& field)
{
    if (field.owner.empty() || field.name.empty() || field.desc.empty()) return false;
    if (!reachableFieldKeys_.insert(fieldKey(field)).second) return false;
    reachableFields_.push_back(field);
    return true;
}

void GlobalReachability::build(const ReferenceIndex& index, const CallGraph& callGraph,
                               const GlobalReachabilityOptions& options)
{
    buildInternal(index, callGraph, nullptr, options);
}

void GlobalReachability::build(const ReferenceIndex& index, const CallGraph& callGraph,
                               const ResolvedCallGraph&         resolvedCallGraph,
                               const GlobalReachabilityOptions& options)
{
    buildInternal(index, callGraph, &resolvedCallGraph, options);
}

void GlobalReachability::buildInternal(const ReferenceIndex& index, const CallGraph& callGraph,
                                       const ResolvedCallGraph*         resolvedCallGraph,
                                       const GlobalReachabilityOptions& options)
{
    clear();

    std::vector<std::string>  classQueue;
    std::vector<MethodRefKey> methodQueue;
    std::vector<FieldRefKey>  fieldQueue;

    auto enqueueClass = [&](const std::string& owner) {
        if (markClass(owner)) classQueue.push_back(owner);
    };
    auto enqueueMethod = [&](const MethodRefKey& method) {
        if (markMethod(method)) methodQueue.push_back(method);
    };
    auto enqueueField = [&](const FieldRefKey& field) {
        if (markField(field)) fieldQueue.push_back(field);
    };

    for (const auto& owner : options.rootClasses) {
        enqueueClass(owner);
    }
    for (const auto& method : options.rootMethods) {
        enqueueMethod(method);
    }
    for (const auto& field : options.rootFields) {
        enqueueField(field);
    }
    if (options.includeMainMethods) {
        for (const auto& decl : index.methodDeclarations()) {
            if (isJavaMain(decl)) enqueueMethod(decl.method);
        }
    }

    //methodQueue 此时恰好是全部方法级根（显式 rootMethods + 发现的 main），
    //快照为入口清单供报告方查询；闭包展开从下面才开始。
    entryMethods_ = methodQueue;

    ca::usize classCursor  = 0;
    ca::usize methodCursor = 0;
    ca::usize fieldCursor  = 0;

    while (classCursor < classQueue.size() || methodCursor < methodQueue.size() ||
           fieldCursor < fieldQueue.size()) {
        while (classCursor < classQueue.size()) {
            const std::string owner = classQueue[classCursor++];

            //可达类会保留其直接声明类型事实；这里不主动展开方法体，方法体必须由可达方法根
            //或调用边进入闭包。
            for (const auto& ref : index.typeReferencesFromClass(owner)) {
                if (isDeclarationTypeReference(ref.kind)) {
                    enqueueClass(ref.targetType);
                }
            }

            if (options.includeClassInitializers) {
                if (const auto* clinit = index.declaredMethod(owner, "<clinit>", "()V")) {
                    enqueueMethod(clinit->method);
                }
            }
        }

        while (methodCursor < methodQueue.size()) {
            const MethodRefKey method = methodQueue[methodCursor++];
            enqueueClass(method.owner);

            if (options.callExpansion == GlobalReachabilityCallExpansion::Resolved &&
                resolvedCallGraph) {
                for (const auto& edge :
                     resolvedCallGraph->edgesFrom(method.owner, method.name, method.desc)) {
                    enqueueMethod(edge.resolvedTarget);
                }
            }
            else {
                for (auto* callee :
                     callGraph.outgoingCallsOf(method.owner, method.name, method.desc)) {
                    if (!callee) continue;
                    enqueueMethod(
                        {str(callee->getOwner()), str(callee->getName()), str(callee->getDesc())});
                }
            }

            //ReferenceIndex 还记录 MethodHandle、invokedynamic 引导参数和已支持的反射事实；
            //这些引用不一定出现在任一调用图实现中。resolvedEdgesExclusive（RTA 档）
            //时编码目标改由解析边表承载——RTA 物化对可达方法的每条方法引用都
            //产出边（非展开引用即编码目标兜底边），raw 循环再入队会把被 RTA
            //收窄掉的 owner 拉回闭包，令可达集 ≠ 边表。空描述符反射按名落解
            //保留：RTA fixpoint 闭包内部用同一口径。
            const bool resolvedExclusive =
                options.resolvedEdgesExclusive &&
                options.callExpansion == GlobalReachabilityCallExpansion::Resolved &&
                resolvedCallGraph != nullptr;
            for (const auto& ref :
                 index.methodReferencesFrom(method.owner, method.name, method.desc)) {
                if (!ref.target.desc.empty()) {
                    if (!resolvedExclusive) enqueueMethod(ref.target);
                    continue;
                }
                if (ref.kind != MethodReferenceKind::Reflection &&
                    ref.kind != MethodReferenceKind::ReflectionExecution) {
                    continue;
                }
                for (const auto& declaration : index.declaredMethodsIn(ref.target.owner)) {
                    if (declaration.method.name == ref.target.name) {
                        enqueueMethod(declaration.method);
                    }
                }
            }
            for (const auto& ref :
                 index.fieldReferencesFrom(method.owner, method.name, method.desc)) {
                if (!ref.target.desc.empty()) {
                    enqueueField(ref.target);
                    continue;
                }
                if (ref.kind != FieldReferenceKind::Reflection &&
                    ref.kind != FieldReferenceKind::ReflectionAccess) {
                    continue;
                }
                for (const auto& declaration : index.declaredFieldsIn(ref.target.owner)) {
                    if (declaration.field.name == ref.target.name) {
                        enqueueField(declaration.field);
                    }
                }
            }
            for (const auto& ref :
                 index.typeReferencesFrom(method.owner, method.name, method.desc)) {
                enqueueClass(ref.targetType);
            }
        }

        while (fieldCursor < fieldQueue.size()) {
            const FieldRefKey field = fieldQueue[fieldCursor++];
            enqueueClass(field.owner);
        }
    }
}

bool GlobalReachability::isClassReachable(const std::string& owner) const
{
    return reachableClassKeys_.find(owner) != reachableClassKeys_.end();
}

bool GlobalReachability::isMethodReachable(const std::string& owner, const std::string& name,
                                           const std::string& desc) const
{
    return reachableMethodKeys_.find(methodKey(owner, name, desc)) != reachableMethodKeys_.end();
}

bool GlobalReachability::isFieldReachable(const std::string& owner, const std::string& name,
                                          const std::string& desc) const
{
    return reachableFieldKeys_.find(fieldKey(owner, name, desc)) != reachableFieldKeys_.end();
}

std::vector<ClassDeclaration> GlobalReachability::unreachableDeclaredClasses(
    const ReferenceIndex& index) const
{
    std::vector<ClassDeclaration> result;
    for (const auto& decl : index.classDeclarations()) {
        if (!isClassReachable(decl.name)) result.push_back(decl);
    }
    return result;
}

std::vector<MethodDeclaration> GlobalReachability::unreachableDeclaredMethods(
    const ReferenceIndex& index) const
{
    std::vector<MethodDeclaration> result;
    for (const auto& decl : index.methodDeclarations()) {
        if (!isMethodReachable(decl.method.owner, decl.method.name, decl.method.desc)) {
            result.push_back(decl);
        }
    }
    return result;
}

std::vector<FieldDeclaration> GlobalReachability::unreachableDeclaredFields(
    const ReferenceIndex& index) const
{
    std::vector<FieldDeclaration> result;
    for (const auto& decl : index.fieldDeclarations()) {
        if (!isFieldReachable(decl.field.owner, decl.field.name, decl.field.desc)) {
            result.push_back(decl);
        }
    }
    return result;
}

GlobalReachabilityAnalysis::Result GlobalReachabilityAnalysis::run(ResourceContainer&     container,
                                                                   GlobalAnalysisManager& gam)
{
    auto& index     = gam.getResult<ReferenceIndexAnalysis>(container);
    auto& callGraph = gam.getResult<CallGraphAnalysis>(container);

    GlobalReachability reachability;
    reachability.build(index, callGraph);
    return reachability;
}

ResolvedGlobalReachabilityAnalysis::Result ResolvedGlobalReachabilityAnalysis::run(
    ResourceContainer& container, GlobalAnalysisManager& gam)
{
    auto& index             = gam.getResult<ReferenceIndexAnalysis>(container);
    auto& callGraph         = gam.getResult<CallGraphAnalysis>(container);
    auto& resolvedCallGraph = gam.getResult<ResolvedCallGraphAnalysis>(container);

    GlobalReachabilityOptions options;
    options.callExpansion = GlobalReachabilityCallExpansion::Resolved;

    GlobalReachability reachability;
    reachability.build(index, callGraph, resolvedCallGraph, options);
    return reachability;
}

ReflectionWeaveReport weaveReflectionBindings(ResolvedCallGraph& graph, const ReferenceIndex& index,
                                              const std::vector<ReflectionBinding>& bindings)
{
    ReflectionWeaveReport           report;
    std::unordered_set<std::string> resolvedClassNames;

    for (const auto& binding : bindings) {
        switch (binding.api) {
        case ReflectionApiKind::MethodInvoke:
        case ReflectionApiKind::ConstructorNewInstance:
        case ReflectionApiKind::ClassNewInstance:
        {
            ++report.sites;
            if (binding.state != ReflectionBindingState::Resolved) {
                //静态无法落解：没有可织入目标，计入未落图，不静默丢。
                ++report.dropped;
                continue;
            }
            //resolved 计分析器结论为 Resolved 的绑定数（含织入复核未过者），
            //与 edges（实际织入）分离：Resolved-but-dropped 时 resolved > edges。
            ++report.resolved;

            //织入端复核：Resolved 绑定的目标以 ReferenceIndex 声明为准再验一次。
            //分析器验证与织入之间可能存在容器边界或描述符口径差异，落不进声明
            //集的目标宁可丢弃并计量。
            if (index.declaredMethod(
                    binding.target.owner, binding.target.name, binding.target.desc) == nullptr) {
                ++report.dropped;
                continue;
            }

            const MethodRefKey target{
                binding.target.owner, binding.target.name, binding.target.desc};
            ResolvedCallEdge edge;
            edge.site           = binding.site;
            edge.encodedTarget  = target;
            edge.resolvedTarget = target;
            edge.kind           = ResolvedCallKind::Reflect;
            edge.expanded       = false;
            graph.appendEdge(edge);

            ++report.edges;
            break;
        }
        case ReflectionApiKind::ClassForName:
        case ReflectionApiKind::ClassLoaderLoadClass:
            //类加载不产生调用边：Resolved 目标类名去重计量。
            if (binding.state == ReflectionBindingState::Resolved && !binding.className.empty()) {
                resolvedClassNames.insert(binding.className);
            }
            break;
        case ReflectionApiKind::MemberLookup:
        case ReflectionApiKind::FieldAccess:
        case ReflectionApiKind::LookupMethodHandle:
            //成员查找 / 字段访问 / Lookup 目录项不产生调用边。
            break;
        }
    }

    report.classes = resolvedClassNames.size();
    return report;
}

}   //namespace mjt::base
