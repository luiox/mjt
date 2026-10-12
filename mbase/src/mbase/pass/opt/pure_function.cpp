#include "mbase/pass/opt/pure_function.hpp"

#include "mbase/pass/opt/class_io_detail.hpp"
#include "mbase/pass/opt/inline_detail.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "mbase/asm/insn/class_util.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "libca/core/tag_cast.hpp"
#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

//============================================================================
//pure_function 私有实现
//原 pure_function_detail.hpp 的家族 helper：注解常量、纯性判定、方法复制/内联 builder。
//家族 3 个 pass 共享这些工具，单消费者实现细节不再以 _detail.hpp 暴露。
//复用 class_io_detail.hpp 的 readClassNode / writeClassNode；
//复用 inline_detail.hpp 的 inlineMethodKey / loadClassesForInlining。
//============================================================================

namespace {

static const char* kPureAnnotationDesc =
    "Lcom/github/luiox/mjt/service/optimize/structure/PureFunction;";

bool methodHasAnnotation(const MethodNode& method, const char* desc)
{
    auto matches = [desc](AnnotationNode* annotation) {
        return annotation && annotation->desc.equals(ca::str::Utf8StringRef::from_cstr(desc));
    };
    return std::any_of(
               method.visibleAnnotations.begin(), method.visibleAnnotations.end(), matches) ||
           std::any_of(
               method.invisibleAnnotations.begin(), method.invisibleAnnotations.end(), matches);
}

void addPureAnnotation(MethodNode& method)
{
    if (methodHasAnnotation(method, kPureAnnotationDesc)) return;
    method.invisibleAnnotations.push_back(
        method.create<AnnotationNode>(method.arena(), ca::str::Utf8StringRef::from_cstr(kPureAnnotationDesc)));
}

bool isStaticConcreteMethod(MethodNode& method)
{
    return MethodUtil::isStatic(method) && !MethodUtil::isAbstract(method) &&
           !MethodUtil::isNative(method);
}

bool isForbiddenPureOpcode(ca::i32 opcode)
{
    switch (opcode) {
    case Opcodes::PUTSTATIC:
    case Opcodes::PUTFIELD:
    case Opcodes::GETFIELD:
    case Opcodes::GETSTATIC:
    case Opcodes::INVOKEVIRTUAL:
    case Opcodes::INVOKEINTERFACE:
    case Opcodes::INVOKESPECIAL:
    case Opcodes::INVOKEDYNAMIC:
    case Opcodes::NEW:
    case Opcodes::ANEWARRAY:
    case Opcodes::NEWARRAY:
    case Opcodes::MULTIANEWARRAY:
    case Opcodes::ATHROW:
    case Opcodes::MONITORENTER:
    case Opcodes::MONITOREXIT: return true;
    default: return false;
    }
}

bool isJdkPureCall(const MethodInsnNode& call)
{
    const std::string key =
        call.owner.to_std_string() + "#" + call.name.to_std_string() + call.desc.to_std_string();
    static const std::unordered_set<std::string> kPureCalls = {
        "java/lang/Math#abs(I)I",        "java/lang/Math#abs(J)J",
        "java/lang/Math#abs(F)F",        "java/lang/Math#abs(D)D",
        "java/lang/Math#max(II)I",       "java/lang/Math#max(JJ)J",
        "java/lang/Math#max(FF)F",       "java/lang/Math#max(DD)D",
        "java/lang/Math#min(II)I",       "java/lang/Math#min(JJ)J",
        "java/lang/Math#min(FF)F",       "java/lang/Math#min(DD)D",
        "java/lang/Math#floorDiv(II)I",  "java/lang/Math#floorDiv(JJ)J",
        "java/lang/Math#floorMod(II)I",  "java/lang/Math#floorMod(JJ)J",
        "java/lang/StrictMath#abs(I)I",  "java/lang/StrictMath#abs(J)J",
        "java/lang/StrictMath#abs(F)F",  "java/lang/StrictMath#abs(D)D",
        "java/lang/StrictMath#max(II)I", "java/lang/StrictMath#max(JJ)J",
        "java/lang/StrictMath#max(FF)F", "java/lang/StrictMath#max(DD)D",
        "java/lang/StrictMath#min(II)I", "java/lang/StrictMath#min(JJ)J",
        "java/lang/StrictMath#min(FF)F", "java/lang/StrictMath#min(DD)D",
        "java/lang/Integer#rotateLeft(II)I", "java/lang/Integer#rotateRight(II)I",
        "java/lang/Integer#reverse(I)I", "java/lang/Integer#reverseBytes(I)I"};
    return kPureCalls.find(key) != kPureCalls.end();
}

bool isPureMethodBody(MethodNode& method, const std::unordered_set<std::string>& currentPure)
{
    if (method.instructions.isEmpty()) return false;
    for (auto* insn : method.instructions) {
        const ca::i32 opcode = insn->getOpcode();
        if (opcode < 0) continue;
        if (isForbiddenPureOpcode(opcode)) return false;
        if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;

        auto* call = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
        if (call->getOpcode() != Opcodes::INVOKESTATIC) return false;
        const auto callee = inlineMethodKey(
            call->owner.to_std_string(), call->name.to_std_string(), call->desc.to_std_string());
        if (!isJdkPureCall(*call) && currentPure.find(callee) == currentPure.end()) return false;
    }
    return true;
}

std::string sanitizeLocalPureName(const std::string& value)
{
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
        if (std::isalnum(ch) || ch == '_' || ch == '$') {
            out.push_back(static_cast<char>(ch));
        }
        else {
            out.push_back('_');
        }
    }
    return out.empty() ? "method" : out;
}

bool hasAnyMethodName(const ClassNode& classNode, const std::string& name)
{
    for (auto* method : classNode.methods) {
        if (method && method->name.to_std_string() == name) return true;
    }
    return false;
}

std::string allocateLocalPureName(const ClassNode& classNode, const std::string& sourceName,
                                  ca::usize sequence)
{
    std::string base =
        "local$pure$" + sanitizeLocalPureName(sourceName) + "$" + std::to_string(sequence);
    std::string name = base;
    ca::usize   idx  = 0;
    while (hasAnyMethodName(classNode, name)) {
        ++idx;
        name = base + "$" + std::to_string(idx);
    }
    return name;
}

ca::i32 normalizeLocalPureAccess(ca::i32 sourceAccess)
{
    ca::i32 access = sourceAccess;
    access &= ~(Opcodes::ACC_PUBLIC | Opcodes::ACC_PROTECTED);
    access |= Opcodes::ACC_PRIVATE | Opcodes::ACC_STATIC;
    access &= ~(Opcodes::ACC_ABSTRACT | Opcodes::ACC_NATIVE);
    return access;
}

MethodNode* clonePureLocalMethod(MethodNode& source, ca::i32 access, ca::str::Utf8StringRef newName)
{
    auto* copied =
        new MethodNode(access, newName, source.desc, source.signature, source.exceptions);
    std::unordered_map<Label*, Label*> labels;
    for (auto* insn : source.instructions.toArray()) {
        //B8 新 clone 协议：产物归 copied 的内容 arena（克隆体最终挂 copied->instructions）。
        copied->instructions.add(insn->clone(copied->arena(), labels));
    }
    for (auto* tryCatch : source.tryCatchBlocks) {
        if (!tryCatch) continue;
        copied->tryCatchBlocks.push_back(
            copied->create<TryCatchBlockNode>(copied->resolveCloneLabel(labels, tryCatch->start),
                                  copied->resolveCloneLabel(labels, tryCatch->end),
                                  copied->resolveCloneLabel(labels, tryCatch->handler),
                                  tryCatch->type));
    }
    copied->maxStack  = source.maxStack;
    copied->maxLocals = source.maxLocals;
    addPureAnnotation(*copied);
    return copied;
}

bool canInlinePureLocal(MethodNode& callee)
{
    if (MethodUtil::isNative(callee) || MethodUtil::isAbstract(callee)) return false;
    if (!callee.tryCatchBlocks.empty()) return false;
    if (callee.instructions.isEmpty()) return false;

    ca::i32           returnCount = 0;
    AbstractInsnNode* lastReal    = nullptr;
    for (auto* insn : callee.instructions) {
        const ca::i32 opcode = insn->getOpcode();
        if (opcode < 0) continue;
        lastReal = insn;
        if (opcode >= Opcodes::IRETURN && opcode <= Opcodes::RETURN) {
            ++returnCount;
            continue;
        }
        if (insn->getType() == AbstractInsnNode::JUMP_INSN ||
            insn->getType() == AbstractInsnNode::LOOKUPSWITCH_INSN ||
            insn->getType() == AbstractInsnNode::TABLESWITCH_INSN) {
            return false;
        }
        if (opcode == Opcodes::ATHROW || opcode == Opcodes::MONITORENTER ||
            opcode == Opcodes::MONITOREXIT || opcode == Opcodes::JSR || opcode == Opcodes::RET) {
            return false;
        }
    }
    return returnCount == 1 && lastReal && lastReal->getOpcode() >= Opcodes::IRETURN &&
           lastReal->getOpcode() <= Opcodes::RETURN;
}

ca::i32 remapPureInlineVar(ca::i32 oldVar, ca::i32 paramSlots,
                           const std::unordered_map<ca::i32, ca::i32>& paramStartSlotMap,
                           ca::i32                                     localBase)
{
    if (oldVar < paramSlots) {
        auto it = paramStartSlotMap.find(oldVar);
        if (it != paramStartSlotMap.end()) return it->second;
    }
    return localBase + std::max<ca::i32>(0, oldVar - paramSlots);
}

void addPureInlineBody(InsnList& target, MethodNode& caller, MethodNode& callee, ca::i32& nextLocal)
{
    const auto           args = Type::getArgumentTypes(callee.desc);
    std::vector<ca::i32> argVars(args.size(), 0);
    for (ca::i32 i = static_cast<ca::i32>(args.size()) - 1; i >= 0; --i) {
        argVars[static_cast<ca::usize>(i)] = nextLocal;
        nextLocal += args[static_cast<ca::usize>(i)].getSize();
        target.add(
            target.create<VarInsnNode>(args[static_cast<ca::usize>(i)].getOpcode(Opcodes::ISTORE),
                                       argVars[static_cast<ca::usize>(i)]));
    }

    std::unordered_map<ca::i32, ca::i32> paramStartSlotMap;
    ca::i32                              paramSlots = 0;
    for (ca::usize i = 0; i < args.size(); ++i) {
        paramStartSlotMap[paramSlots] = argVars[i];
        paramSlots += args[i].getSize();
    }

    const ca::i32                      localBase = nextLocal;
    std::unordered_map<Label*, Label*> labels;
    for (auto* insn : callee.instructions.toArray()) {
        const ca::i32 opcode = insn->getOpcode();
        if (opcode >= Opcodes::IRETURN && opcode <= Opcodes::RETURN) continue;

        //B8 新 clone 协议：内联体最终进 caller->instructions，产物归 caller 的 arena。
        auto* cloned = insn->clone(caller.arena(), labels);
        if (cloned->getType() == AbstractInsnNode::VAR_INSN) {
            auto* var = ca::core::tag_cast::cast<VarInsnNode*>(cloned);
            var->var  = remapPureInlineVar(var->var, paramSlots, paramStartSlotMap, localBase);
        }
        else if (cloned->getType() == AbstractInsnNode::IINC_INSN) {
            auto* iinc = ca::core::tag_cast::cast<IincInsnNode*>(cloned);
            iinc->var  = remapPureInlineVar(iinc->var, paramSlots, paramStartSlotMap, localBase);
        }
        target.add(cloned);
    }

    caller.maxLocals =
        std::max(caller.maxLocals, localBase + std::max(0, callee.maxLocals - paramSlots));
    caller.maxStack = std::max(caller.maxStack, callee.maxStack);
    nextLocal       = caller.maxLocals;
}

std::unordered_map<std::string, MethodNode*> collectLocalPureMethods(ClassNode& classNode)
{
    std::unordered_map<std::string, MethodNode*> methods;
    for (auto* method : classNode.methods) {
        if (!method || !MethodUtil::isStatic(*method)) continue;
        if (!methodHasAnnotation(*method, kPureAnnotationDesc)) continue;
        methods[method->name.to_std_string() + "\n" + method->desc.to_std_string()] = method;
    }
    return methods;
}

bool inlineLocalPureCallsInClass(ClassNode& classNode)
{
    auto localPureMethods = collectLocalPureMethods(classNode);
    if (localPureMethods.empty()) return false;

    bool modifiedClass = false;
    bool changed       = false;
    do {
        changed = false;
        for (auto* caller : classNode.methods) {
            if (!caller || caller->instructions.isEmpty()) continue;

            //重建表绑定 caller arena：未触发替换的迭代会带着 arena 克隆产物走
            //continue/析构路径，unbound 表会误 delete arena 节点（堆损坏）。
            InsnList                           result(&caller->arena());
            std::unordered_map<Label*, Label*> callerLabels;
            ca::i32                            nextLocal      = caller->maxLocals;
            bool                               modifiedMethod = false;

            for (auto* insn : caller->instructions.toArray()) {
                if (insn->getType() == AbstractInsnNode::METHOD_INSN) {
                    auto* call = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
                    if (call->getOpcode() == Opcodes::INVOKESTATIC &&
                        call->owner.equals(classNode.name)) {
                        const std::string key =
                            call->name.to_std_string() + "\n" + call->desc.to_std_string();
                        auto it = localPureMethods.find(key);
                        if (it != localPureMethods.end() && it->second != caller &&
                            canInlinePureLocal(*it->second)) {
                            addPureInlineBody(result, *caller, *it->second, nextLocal);
                            modifiedMethod = true;
                            changed        = true;
                            continue;
                        }
                    }
                }
                //B8 新 clone 协议：result 最终并回 caller->instructions，产物归 caller 的
                //arena；原指令的 Label 已归属同 arena，resolveCloneLabel 同域直返（保身份）。
                result.add(insn->clone(caller->arena(), callerLabels));
            }

            if (!modifiedMethod) continue;
            caller->instructions.clear();
            caller->instructions.add(result);
            modifiedClass = true;
        }
    } while (changed);

    return modifiedClass;
}

}   //namespace

//============================================================================
//PureFunctionAnnotator
//对应 Java: structure/PureFunctionAnnotator.java。
//按 JDK 白名单 + 方法体 opcode 分析，给纯 static 方法打 PureFunction 注解（不动点迭代）。
//============================================================================

PreservedAnalyses PureFunctionAnnotator::run(ResourceContainer&     container,
                                             GlobalAnalysisManager& gam)
{
    auto classes = loadClassesForInlining(container, gam.arena());
    if (classes.empty()) return PreservedAnalyses::all();

    std::unordered_map<std::string, MethodNode*> staticMethods;
    for (auto& loaded : classes) {
        if (!loaded.node) continue;
        for (auto* method : loaded.node->methods) {
            if (!method || MethodUtil::isConstructor(*method) || MethodUtil::isClinit(*method))
                continue;
            if (!isStaticConcreteMethod(*method)) continue;
            staticMethods[inlineMethodKey(loaded.node->name.to_std_string(),
                                          method->name.to_std_string(),
                                          method->desc.to_std_string())] = method;
        }
    }
    if (staticMethods.empty()) return PreservedAnalyses::all();

    std::unordered_set<std::string> pure;
    bool                            changed = false;
    do {
        changed = false;
        for (auto& [key, method] : staticMethods) {
            if (!method || pure.find(key) != pure.end()) continue;
            if (isPureMethodBody(*method, pure)) {
                pure.insert(key);
                changed = true;
            }
        }
    } while (changed);
    if (pure.empty()) return PreservedAnalyses::all();

    bool modifiedAny = false;
    for (auto& loaded : classes) {
        if (!loaded.resource || !loaded.node) continue;
        bool modifiedClass = false;
        for (auto* method : loaded.node->methods) {
            if (!method) continue;
            const auto key = inlineMethodKey(loaded.node->name.to_std_string(),
                                             method->name.to_std_string(),
                                             method->desc.to_std_string());
            if (pure.find(key) == pure.end()) continue;
            if (methodHasAnnotation(*method, kPureAnnotationDesc)) continue;
            addPureAnnotation(*method);
            modifiedClass = true;
        }
        if (!modifiedClass) continue;
        writeClassNode(*loaded.resource, gam.arena(), *loaded.node);
        modifiedAny = true;
    }

    return modifiedAny ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//PureFunctionLocalizer
//对应 Java: structure/PureFunctionLocalizer.java。
//把跨类的纯 static 被调方法复制到 caller 类，改写调用点 owner，让后续类内分析可见。
//============================================================================

PreservedAnalyses PureFunctionLocalizer::run(ResourceContainer&     container,
                                             GlobalAnalysisManager& gam)
{
    auto classes = loadClassesForInlining(container, gam.arena());
    if (classes.empty()) return PreservedAnalyses::all();

    std::unordered_map<std::string, MethodNode*> pureMethods;
    for (auto& loaded : classes) {
        if (!loaded.node) continue;
        for (auto* method : loaded.node->methods) {
            if (!method || !methodHasAnnotation(*method, kPureAnnotationDesc)) continue;
            pureMethods[inlineMethodKey(loaded.node->name.to_std_string(),
                                        method->name.to_std_string(),
                                        method->desc.to_std_string())] = method;
        }
    }
    if (pureMethods.empty()) return PreservedAnalyses::all();

    bool modifiedAny = false;
    for (auto& loaded : classes) {
        if (!loaded.resource || !loaded.node) continue;
        auto& classNode = *loaded.node;
        if (ClassUtil::isInterface(classNode)) continue;

        std::vector<MethodRefKey>       requiredTargets;
        std::unordered_set<std::string> requiredKeys;
        for (auto* caller : classNode.methods) {
            if (!caller || caller->instructions.isEmpty()) continue;
            for (auto* insn : caller->instructions.toArray()) {
                if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
                auto* call = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
                if (call->getOpcode() != Opcodes::INVOKESTATIC ||
                    call->owner.equals(classNode.name))
                    continue;
                const auto key = inlineMethodKey(call->owner.to_std_string(),
                                                 call->name.to_std_string(),
                                                 call->desc.to_std_string());
                if (pureMethods.find(key) == pureMethods.end()) continue;
                if (!requiredKeys.insert(key).second) continue;
                requiredTargets.push_back({call->owner.to_std_string(),
                                           call->name.to_std_string(),
                                           call->desc.to_std_string()});
            }
        }
        if (requiredTargets.empty()) continue;

        std::unordered_map<std::string, MethodRefKey> localMap;
        for (ca::usize i = 0; i < requiredTargets.size(); ++i) {
            const auto& target = requiredTargets[i];
            const auto  key    = inlineMethodKey(target);
            auto        it     = pureMethods.find(key);
            if (it == pureMethods.end() || !it->second) continue;

            const auto localName =
                allocateLocalPureName(classNode, it->second->name.to_std_string(), i);
            auto* copied = clonePureLocalMethod(
                *it->second,
                normalizeLocalPureAccess(it->second->access),
                gam.arena().intern(ca::str::Utf8StringRef::from_cstr(localName.c_str())));
            localMap[key] = {
                classNode.name.to_std_string(), localName, copied->desc.to_std_string()};
            classNode.methods.push_back(copied);
        }

        bool modifiedClass = !localMap.empty();
        for (auto* caller : classNode.methods) {
            if (!caller || caller->instructions.isEmpty()) continue;
            for (auto* insn : caller->instructions.toArray()) {
                if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
                auto* call = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
                if (call->getOpcode() != Opcodes::INVOKESTATIC ||
                    call->owner.equals(classNode.name))
                    continue;
                const auto key = inlineMethodKey(call->owner.to_std_string(),
                                                 call->name.to_std_string(),
                                                 call->desc.to_std_string());
                auto       it  = localMap.find(key);
                if (it == localMap.end()) continue;
                call->owner = classNode.name;
                call->name =
                    gam.arena().intern(ca::str::Utf8StringRef::from_cstr(it->second.name.c_str()));
                call->desc =
                    gam.arena().intern(ca::str::Utf8StringRef::from_cstr(it->second.desc.c_str()));
                call->isInterface = false;
                modifiedClass     = true;
            }
        }

        if (!modifiedClass) continue;
        writeClassNode(*loaded.resource, gam.arena(), classNode);
        modifiedAny = true;
    }

    return modifiedAny ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//LocalPureFunctionInliner
//对应 Java: structure/LocalPureFunctionInliner.java。
//类内内联已标记的纯方法调用；要求单出口、无跳转/异常/同步。
//============================================================================

PreservedAnalyses LocalPureFunctionInliner::run(ResourceContainer&     container,
                                                 GlobalAnalysisManager& gam)
{
    auto classes = loadClassesForInlining(container, gam.arena());
    if (classes.empty()) return PreservedAnalyses::all();

    bool modifiedAny = false;
    for (auto& loaded : classes) {
        if (!loaded.resource || !loaded.node) continue;
        if (!inlineLocalPureCallsInClass(*loaded.node)) continue;
        writeClassNode(*loaded.resource, gam.arena(), *loaded.node);
        modifiedAny = true;
    }
    return modifiedAny ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
