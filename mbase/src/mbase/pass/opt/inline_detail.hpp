#pragma once

//inline_detail.hpp
//
//小型方法内联器（GetterInliner / ShortMethodInliner / ArgForwardMethodInliner）共享的
//私有类型和分析工具。对应 method_inliner.hpp 的 small inliner 家族；不声明 Asm*Pass。
//
//拆分自原 structure_detail.hpp 的 "Small inliners" 区段。
//
//复用 class_io_detail.hpp 的 readClassNode / writeClassNode 完成 class resource 读写。

#include "class_io_detail.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "mbase/asm/insn/class_util.hpp"
#include "mbase/reference/reference_index_analysis.hpp"
#include "libca/core/tag_cast.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

//============================================================================
//Small inliners
//P3.4 保守小型内联族：只替换调用点，不删除 callee，不改签名。
//============================================================================

enum class SmallInlineKind
{
    Getter,
    Constant,
    Forward
};

struct LoadedClass
{
    ClassResource*             resource = nullptr;
    std::unique_ptr<ClassNode> node;
    std::string                uri;
};

struct InlineReplacement
{
    SmallInlineKind        kind = SmallInlineKind::Getter;
    MethodRefKey           method;
    ca::i32                opcode = -1;
    ca::str::Utf8StringRef owner;
    ca::str::Utf8StringRef name;
    ca::str::Utf8StringRef desc;
    bool                   isInterface = false;
    AbstractInsnNode*      constant    = nullptr;
};

std::string inlineMethodKey(const std::string& owner, const std::string& name,
                            const std::string& desc)
{
    return owner + "\n" + name + "\n" + desc;
}

std::string inlineMethodKey(const MethodRefKey& method)
{
    return inlineMethodKey(method.owner, method.name, method.desc);
}

std::vector<LoadedClass> loadClassesForInlining(ResourceContainer&        container,
                                                ca::str::Utf8StringArena& arena)
{
    std::vector<LoadedClass> classes;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto* classResource = ca::core::tag_cast::cast<ClassResource>(resource.get());
        auto  classNode     = std::make_unique<ClassNode>();
        if (!readClassNode(*classResource, arena, *classNode)) continue;
        classes.push_back({classResource, std::move(classNode), uri});
    }
    return classes;
}

std::vector<AbstractInsnNode*> meaningfulInstructions(MethodNode& method)
{
    std::vector<AbstractInsnNode*> result;
    for (auto* insn : method.instructions.toArray()) {
        if (!insn || insn->getOpcode() < 0) continue;
        result.push_back(insn);
    }
    return result;
}

bool isReturnForType(ca::i32 opcode, const Type& type)
{
    if (type == Type::VOID_TYPE) return opcode == Opcodes::RETURN;
    return opcode == type.getOpcode(Opcodes::IRETURN);
}

bool isNoArgMethod(const MethodNode& method)
{
    return Type::getArgumentTypes(method.desc).empty();
}

bool isStaticMethod(const MethodNode& method)
{
    return (method.access & Opcodes::ACC_STATIC) != 0;
}

MethodRefKey declaredMethodKey(const ClassNode& owner, const MethodNode& method)
{
    return {owner.name.to_std_string(), method.name.to_std_string(), method.desc.to_std_string()};
}

bool tryBuildGetterReplacement(ClassNode& owner, MethodNode& method, InlineReplacement& out)
{
    if (!isNoArgMethod(method)) return false;
    const Type returnType = Type::getReturnType(method.desc);
    if (returnType == Type::VOID_TYPE) return false;

    auto insns = meaningfulInstructions(method);
    if (isStaticMethod(method)) {
        if (insns.size() != 2) return false;
        if (insns[0]->getType() != AbstractInsnNode::FIELD_INSN ||
            insns[0]->getOpcode() != Opcodes::GETSTATIC ||
            !isReturnForType(insns[1]->getOpcode(), returnType)) {
            return false;
        }
        auto* field = ca::core::tag_cast::cast<FieldInsnNode*>(insns[0]);
        out.kind    = SmallInlineKind::Getter;
        out.method  = declaredMethodKey(owner, method);
        out.opcode  = Opcodes::GETSTATIC;
        out.owner   = field->owner;
        out.name    = field->name;
        out.desc    = field->desc;
        return true;
    }

    if (insns.size() != 3) return false;
    if (insns[0]->getOpcode() != Opcodes::ALOAD ||
        insns[0]->getType() != AbstractInsnNode::VAR_INSN ||
        ca::core::tag_cast::cast<VarInsnNode*>(insns[0])->var != 0 ||
        insns[1]->getType() != AbstractInsnNode::FIELD_INSN ||
        insns[1]->getOpcode() != Opcodes::GETFIELD ||
        !isReturnForType(insns[2]->getOpcode(), returnType)) {
        return false;
    }
    auto* field = ca::core::tag_cast::cast<FieldInsnNode*>(insns[1]);
    out.kind    = SmallInlineKind::Getter;
    out.method  = declaredMethodKey(owner, method);
    out.opcode  = Opcodes::GETFIELD;
    out.owner   = field->owner;
    out.name    = field->name;
    out.desc    = field->desc;
    return true;
}

bool tryBuildConstantReplacement(ClassNode& owner, MethodNode& method, InlineReplacement& out)
{
    if (!isStaticMethod(method) || !isNoArgMethod(method)) return false;
    const Type returnType = Type::getReturnType(method.desc);
    if (returnType == Type::VOID_TYPE) return false;

    auto insns = meaningfulInstructions(method);
    if (insns.size() != 2 || !isReturnForType(insns[1]->getOpcode(), returnType)) return false;
    if (!InsnUtil::isConstant(insns[0]->getOpcode())) return false;

    out.kind     = SmallInlineKind::Constant;
    out.method   = declaredMethodKey(owner, method);
    out.constant = insns[0];
    return true;
}

ca::i32 loadOpcodeFor(const Type& type)
{
    return type.getOpcode(Opcodes::ILOAD);
}

bool hasOrderedStaticArgumentLoads(MethodNode& method, const std::vector<AbstractInsnNode*>& insns,
                                   ca::usize callIndex)
{
    const auto args = Type::getArgumentTypes(method.desc);
    if (callIndex != args.size()) return false;

    ca::i32 slot = 0;
    for (ca::usize i = 0; i < args.size(); ++i) {
        auto* insn = insns[i];
        if (insn->getType() != AbstractInsnNode::VAR_INSN ||
            insn->getOpcode() != loadOpcodeFor(args[i])) {
            return false;
        }
        auto* load = ca::core::tag_cast::cast<VarInsnNode*>(insn);
        if (load->var != slot) return false;
        slot += args[i].getSize();
    }
    return true;
}

bool tryBuildForwardReplacement(ClassNode& owner, MethodNode& method, InlineReplacement& out)
{
    if (!isStaticMethod(method)) return false;
    const Type returnType = Type::getReturnType(method.desc);

    auto       insns = meaningfulInstructions(method);
    const auto args  = Type::getArgumentTypes(method.desc);
    if (insns.size() != args.size() + 2) return false;

    const ca::usize callIndex = args.size();
    if (!hasOrderedStaticArgumentLoads(method, insns, callIndex)) return false;
    if (insns[callIndex]->getType() != AbstractInsnNode::METHOD_INSN) return false;
    if (!isReturnForType(insns[callIndex + 1]->getOpcode(), returnType)) return false;

    auto* target = ca::core::tag_cast::cast<MethodInsnNode*>(insns[callIndex]);
    if (!target->desc.equals(method.desc)) return false;
    auto         source = declaredMethodKey(owner, method);
    MethodRefKey targetKey{
        target->owner.to_std_string(), target->name.to_std_string(), target->desc.to_std_string()};
    if (inlineMethodKey(source) == inlineMethodKey(targetKey)) return false;

    out.kind        = SmallInlineKind::Forward;
    out.method      = source;
    out.opcode      = target->getOpcode();
    out.owner       = target->owner;
    out.name        = target->name;
    out.desc        = target->desc;
    out.isInterface = target->isInterface;
    return true;
}

std::unordered_map<std::string, InlineReplacement> collectInlineReplacements(
    const std::vector<LoadedClass>& classes, SmallInlineKind kind)
{
    std::unordered_map<std::string, InlineReplacement> replacements;
    for (const auto& loaded : classes) {
        if (!loaded.node) continue;
        for (auto* method : loaded.node->methods) {
            if (!method || MethodUtil::isConstructor(*method) || MethodUtil::isClinit(*method))
                continue;

            InlineReplacement replacement;
            bool              matched = false;
            if (kind == SmallInlineKind::Getter) {
                matched = tryBuildGetterReplacement(*loaded.node, *method, replacement);
            }
            else if (kind == SmallInlineKind::Constant) {
                matched = tryBuildConstantReplacement(*loaded.node, *method, replacement);
            }
            else if (kind == SmallInlineKind::Forward) {
                matched = tryBuildForwardReplacement(*loaded.node, *method, replacement);
            }
            if (!matched) continue;
            replacements[inlineMethodKey(replacement.method)] = replacement;
        }
    }
    return replacements;
}

AbstractInsnNode* makeInlineReplacementNode(const InlineReplacement&            replacement,
                                            garena::Arena&                      arena,
                                            std::unordered_map<Label*, Label*>& clonedLabels)
{
    if (replacement.kind == SmallInlineKind::Getter) {
        return arena.create<FieldInsnNode>(
            replacement.opcode, replacement.owner, replacement.name, replacement.desc);
    }
    if (replacement.kind == SmallInlineKind::Forward) {
        return arena.create<MethodInsnNode>(replacement.opcode,
                                            replacement.owner,
                                            replacement.name,
                                            replacement.desc,
                                            replacement.isInterface);
    }
    //B8 新 clone 协议：常量替身最终进 method->instructions，产物归该方法的 arena。
    return replacement.constant ? replacement.constant->clone(arena, clonedLabels) : nullptr;
}

bool rewriteInlineCalls(ClassNode&                                                classNode,
                        const std::unordered_map<std::string, InlineReplacement>& replacements)
{
    bool modifiedClass = false;
    for (auto* method : classNode.methods) {
        if (!method || method->instructions.isEmpty()) continue;

        bool                               modifiedMethod = false;
        //绑定方法 arena：!modifiedMethod 早退时析构掠过 arena 克隆产物（unbound
        //表会误 delete arena 节点）。
        InsnList                           result(&method->arena());
        std::unordered_map<Label*, Label*> clonedLabels;
        for (auto* insn : method->instructions.toArray()) {
            if (insn->getType() == AbstractInsnNode::METHOD_INSN) {
                auto*             call = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
                const std::string key  = inlineMethodKey(call->owner.to_std_string(),
                                                        call->name.to_std_string(),
                                                        call->desc.to_std_string());
                auto              it   = replacements.find(key);
                if (it != replacements.end()) {
                    auto* replacement =
                        makeInlineReplacementNode(it->second, method->arena(), clonedLabels);
                    if (replacement) {
                        result.add(replacement);
                        modifiedMethod = true;
                        continue;
                    }
                }
            }
            //B8 新 clone 协议：result 最终并回 method->instructions，产物归其 arena。
            result.add(insn->clone(method->arena(), clonedLabels));
        }

        if (!modifiedMethod) continue;
        method->instructions.clear();
        method->instructions.add(result);
        modifiedClass = true;
    }
    return modifiedClass;
}

PreservedAnalyses runSmallInliner(ResourceContainer& container, GlobalAnalysisManager& gam,
                                  SmallInlineKind kind)
{
    auto classes = loadClassesForInlining(container, gam.arena());
    if (classes.empty()) return PreservedAnalyses::all();

    auto replacements = collectInlineReplacements(classes, kind);
    if (replacements.empty()) return PreservedAnalyses::all();

    bool modifiedAny = false;
    for (auto& loaded : classes) {
        if (!loaded.resource || !loaded.node) continue;
        if (!rewriteInlineCalls(*loaded.node, replacements)) continue;
        writeClassNode(*loaded.resource, gam.arena(), *loaded.node);
        modifiedAny = true;
    }
    return modifiedAny ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace

}   //namespace mjt::base
