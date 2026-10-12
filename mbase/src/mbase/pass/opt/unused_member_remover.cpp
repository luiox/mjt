#include "mbase/pass/opt/unused_member_remover.hpp"
#include "mbase/pass/opt/structure_detail.hpp"

#include <algorithm>

namespace mjt::base {

//============================================================================
//EmptyClassRemover
//不做可达性分析，只看单个 class 是否无字段无方法。最朴素的空类清理。
//============================================================================

PreservedAnalyses EmptyClassRemover::run(ResourceContainer& container, GlobalAnalysisManager& gam)
{
    std::vector<std::string> toRemove;
    for (const auto& [uri, resource] : container.getResources()) {
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;

        auto*     classResource = ca::core::tag_cast::cast<ClassResource>(resource.get());
        ClassNode classNode;
        if (!readClassNode(*classResource, gam.arena(), classNode)) continue;
        if (classNode.fields.empty() && classNode.methods.empty()) {
            toRemove.push_back(uri);
        }
    }

    if (toRemove.empty()) return PreservedAnalyses::all();
    auto& resources = container.getResources();
    for (const auto& uri : toRemove) {
        resources.erase(uri);
    }
    return PreservedAnalyses::none();
}

//============================================================================
//UnusedClassRemover
//对应 Java: structure/UnusedClassRemover.java。
//用 GlobalReachability 删除不可达 class 资源（按 classfile 头声明名比较，不是资源路径）。
//============================================================================

PreservedAnalyses UnusedClassRemover::run(ResourceContainer& container, GlobalAnalysisManager& gam)
{
    std::unordered_set<std::string> resourceKeepNames;
    if (!collectResourceKeepClasses(container, gam.arena(), resourceKeepNames)) {
        return PreservedAnalyses::all();
    }

    auto& index             = gam.getResult<ReferenceIndexAnalysis>(container);
    auto& callGraph         = gam.getResult<CallGraphAnalysis>(container);
    auto& resolvedCallGraph = gam.getResult<ResolvedCallGraphAnalysis>(container);

    GlobalReachabilityOptions options;
    options.callExpansion = GlobalReachabilityCallExpansion::Resolved;
    for (const auto& className : resourceKeepNames) {
        options.rootClasses.push_back(className);
        for (const auto& method : index.declaredMethodsIn(className)) {
            options.rootMethods.push_back(method.method);
        }
    }
    GlobalReachability reachability;
    reachability.build(index, callGraph, resolvedCallGraph, options);

    //没有默认根或资源根时，通常表示这是 library jar；在调用方提供显式保留策略前保持不变。
    if (reachability.reachableClasses().empty()) return PreservedAnalyses::all();

    std::vector<std::string> toRemove;
    for (const auto& [uri, resource] : container.getResources()) {
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;

        std::string className;
        auto*       classResource = ca::core::tag_cast::cast<ClassResource>(resource.get());
        if (!readClassName(*classResource, gam.arena(), className)) {
            return PreservedAnalyses::all();
        }
        if (!reachability.isClassReachable(className)) {
            toRemove.push_back(uri);
        }
    }

    if (toRemove.empty()) return PreservedAnalyses::all();
    auto& resources = container.getResources();
    for (const auto& uri : toRemove) {
        resources.erase(uri);
    }
    return PreservedAnalyses::none();
}

//============================================================================
//UnusedFieldRemover
//对应 Java: structure/UnusedFieldRemover.java。
//用 ResolvedGlobalReachabilityAnalysis 删除可达类内不可达的 private 字段。
//============================================================================

PreservedAnalyses UnusedFieldRemover::run(ResourceContainer& container, GlobalAnalysisManager& gam)
{
    if (!allClassResourcesReadable(container, gam.arena())) return PreservedAnalyses::all();
    auto& reachability = gam.getResult<ResolvedGlobalReachabilityAnalysis>(container);
    if (reachability.reachableClasses().empty()) return PreservedAnalyses::all();

    std::vector<PendingClassRewrite> rewrites;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;

        auto*     classResource = ca::core::tag_cast::cast<ClassResource>(resource.get());
        ClassNode classNode;
        if (!readClassNode(*classResource, gam.arena(), classNode)) {
            return PreservedAnalyses::all();
        }

        const std::string owner = classNode.name.to_std_string();
        if (!reachability.isClassReachable(owner)) continue;

        bool modifiedClass = false;
        for (auto it = classNode.fields.begin(); it != classNode.fields.end();) {
            FieldNode* field = *it;
            if (!field) {
                ++it;
                continue;
            }
            const std::string name = field->name.to_std_string();
            const std::string desc = field->desc.to_std_string();
            if (isPrivateAccess(field->access) && !hasFieldKeepMetadata(*field) &&
                !reachability.isFieldReachable(owner, name, desc)) {
                it = classNode.fields.erase(it);
                delete field;
                modifiedClass = true;
                continue;
            }
            ++it;
        }

        if (modifiedClass) {
            PendingClassRewrite rewrite;
            rewrite.resource = classResource;
            if (!serializeClassNode(classNode, gam.arena(), rewrite.bytes)) {
                return PreservedAnalyses::all();
            }
            rewrites.push_back(std::move(rewrite));
        }
    }

    if (rewrites.empty()) return PreservedAnalyses::all();
    commitClassRewrites(rewrites);
    return PreservedAnalyses::none();
}

//============================================================================
//UnusedMethodRemover（class-local，AsmClassPass）
//对应 Java: structure/UnusedMethodRemover.java。
//不依赖全局分析，只扫描类内直接调用和 Handle 引用，删除 private static 方法。
//============================================================================

PreservedAnalyses UnusedMethodRemover::run(ClassNode& classNode, ClassAnalysisManager& cam)
{
    (void)cam;
    //owner+name+desc(用 MethodNode* 直接作 key，类内唯一) -> 候选方法
    std::vector<MethodNode*> candidates;
    for (auto* method : classNode.methods) {
        if (!method) continue;
        if (MethodUtil::isConstructor(*method)) continue;
        if (MethodUtil::isClinit(*method)) continue;
        if (MethodUtil::isStatic(*method) && MethodUtil::isPrivate(*method)) {
            candidates.push_back(method);
        }
    }
    if (candidates.empty()) return PreservedAnalyses::all();

    //候选集合（指针）
    std::unordered_set<MethodNode*> unused(candidates.begin(), candidates.end());

    auto markMethodReference = [&](const ca::str::Utf8StringRef& owner,
                                   const ca::str::Utf8StringRef& name,
                                   const ca::str::Utf8StringRef& desc) {
        if (!owner.equals(classNode.name)) return;
        for (auto it = unused.begin(); it != unused.end(); ++it) {
            if ((*it)->name.equals(name) && (*it)->desc.equals(desc)) {
                unused.erase(it);
                break;
            }
        }
    };
    auto markHandleReference = [&](const Handle& handle) {
        markMethodReference(handle.getOwner(), handle.getName(), handle.getDesc());
    };

    //遍历类内所有方法的直接调用和方法句柄引用，命中候选即排除。
    for (auto* method : classNode.methods) {
        if (!method) continue;
        for (auto* insn : method->instructions) {
            if (insn->getType() == AbstractInsnNode::METHOD_INSN) {
                auto* methodInsn = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
                markMethodReference(methodInsn->owner, methodInsn->name, methodInsn->desc);
            }
            else if (insn->getType() == AbstractInsnNode::LDC_INSN) {
                auto* ldc = ca::core::tag_cast::cast<LdcInsnNode*>(insn);
                if (auto* handle = ldc->cst.as<Handle>()) {
                    markHandleReference(*handle);
                }
                else if (auto* dynamic = ldc->cst.as<ConstantDynamic>()) {
                    //ConstantDynamic 可以递归携带任意 bootstrap 参数；未知嵌套形状不冒险删方法。
                    markHandleReference(dynamic->getBootstrapMethod());
                    for (const auto& arg : dynamic->getBootstrapMethodArguments()) {
                        if (auto* handle = arg.as<Handle>()) markHandleReference(*handle);
                        else if (arg.is<ConstantDynamic>()) unused.clear();
                    }
                }
            }
            else if (insn->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN) {
                auto* indy = ca::core::tag_cast::cast<InvokeDynamicInsnNode*>(insn);
                markHandleReference(indy->bsm);
                for (const auto& arg : indy->bsmArgs) {
                    if (auto* handle = arg.as<Handle>()) markHandleReference(*handle);
                }
            }
        }
    }

    if (unused.empty()) return PreservedAnalyses::all();
    bool modified = false;
    for (auto* method : unused) {
        classNode.methods.erase(
            std::remove(classNode.methods.begin(), classNode.methods.end(), method),
            classNode.methods.end());
        delete method;
        modified = true;
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//GlobalUnusedMethodRemover（global，AsmGlobalPass）
//对应 Java: structure/GlobalUnusedMethodRemover.java。
//用 ResolvedGlobalReachabilityAnalysis 删除可达类内不可达的 private 方法；构造器、
//<clinit>、native/abstract、keep metadata 和反射敏感方法均保留。
//============================================================================

PreservedAnalyses GlobalUnusedMethodRemover::run(ResourceContainer&     container,
                                                 GlobalAnalysisManager& gam)
{
    if (!allClassResourcesReadable(container, gam.arena())) return PreservedAnalyses::all();
    auto& reachability = gam.getResult<ResolvedGlobalReachabilityAnalysis>(container);
    if (reachability.reachableClasses().empty()) return PreservedAnalyses::all();

    std::vector<PendingClassRewrite> rewrites;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;

        auto*     classResource = ca::core::tag_cast::cast<ClassResource>(resource.get());
        ClassNode classNode;
        if (!readClassNode(*classResource, gam.arena(), classNode)) {
            return PreservedAnalyses::all();
        }

        const std::string owner = classNode.name.to_std_string();
        if (!reachability.isClassReachable(owner)) continue;

        bool modifiedClass = false;
        for (auto it = classNode.methods.begin(); it != classNode.methods.end();) {
            MethodNode* method = *it;
            if (!method) {
                ++it;
                continue;
            }
            if (MethodUtil::isConstructor(*method) || MethodUtil::isClinit(*method)) {
                ++it;
                continue;
            }

            const std::string name = method->name.to_std_string();
            const std::string desc = method->desc.to_std_string();
            const bool unsupportedBody =
                (method->access & (Opcodes::ACC_NATIVE | Opcodes::ACC_ABSTRACT)) != 0;
            if (isPrivateAccess(method->access) && !unsupportedBody &&
                !hasMethodKeepMetadata(*method) &&
                !reachability.isMethodReachable(owner, name, desc)) {
                it = classNode.methods.erase(it);
                delete method;
                modifiedClass = true;
                continue;
            }
            ++it;
        }

        if (modifiedClass) {
            PendingClassRewrite rewrite;
            rewrite.resource = classResource;
            if (!serializeClassNode(classNode, gam.arena(), rewrite.bytes)) {
                return PreservedAnalyses::all();
            }
            rewrites.push_back(std::move(rewrite));
        }
    }

    if (rewrites.empty()) return PreservedAnalyses::all();
    commitClassRewrites(rewrites);
    return PreservedAnalyses::none();
}

}   //namespace mjt::base
