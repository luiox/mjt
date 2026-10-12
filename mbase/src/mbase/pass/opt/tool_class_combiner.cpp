#include "mbase/pass/opt/tool_class_combiner.hpp"
#include "mbase/pass/opt/inline_detail.hpp"
#include "class_io_detail.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "mbase/asm/insn/class_util.hpp"
#include "mbase/asm/remapper/member_remapper.hpp"
#include "libca/core/tag_cast.hpp"
#include <memory>
#include <string>

namespace mjt::base {

//=== 合并自 tool_combiner_detail.hpp（单消费者实现细节，原 _detail.hpp 拆分文件）===

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

bool isEmptyOrSafeToolClinit(const MethodNode& method)
{
    if (!method.name.equals(ca::str::Utf8StringRef::from_cstr("<clinit>"))) return false;
    for (auto* insn : method.instructions) {
        const auto opcode = insn->getOpcode();
        if (opcode < 0) continue;
        if (opcode == Opcodes::RETURN) continue;
        return false;
    }
    return true;
}

bool isSimpleToolMethodBody(const MethodNode& method)
{
    for (auto* insn : method.instructions) {
        if (!insn || insn->getOpcode() < 0) continue;
        if (insn->getType() == AbstractInsnNode::METHOD_INSN ||
            insn->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN) {
            return false;
        }
        if (insn->getOpcode() == Opcodes::PUTSTATIC) return false;
    }
    return true;
}

bool isToolClassCandidate(ClassNode& classNode)
{
    if (ClassUtil::isInterface(classNode)) return false;
    if ((classNode.access & Opcodes::ACC_ANNOTATION) != 0) return false;
    if (!classNode.superName.equals(ca::str::Utf8StringRef::from_cstr("java/lang/Object")))
        return false;
    if (classNode.name.to_std_string().find('$') != std::string::npos) return false;

    for (auto* field : classNode.fields) {
        if (!field || !FieldUtil::isStatic(*field)) return false;
    }
    bool hasMovableMember = !classNode.fields.empty();
    for (auto* method : classNode.methods) {
        if (!method) return false;
        if (MethodUtil::isConstructor(*method)) continue;
        if (MethodUtil::isClinit(*method)) {
            if (!isEmptyOrSafeToolClinit(*method)) return false;
            continue;
        }
        if (!MethodUtil::isStatic(*method)) return false;
        if (!isSimpleToolMethodBody(*method)) return false;
        hasMovableMember = true;
    }
    return hasMovableMember;
}

std::unique_ptr<ClassNode> newMergedToolClass(ca::str::Utf8StringArena& arena, ca::usize index)
{
    auto merged       = std::make_unique<ClassNode>();
    merged->version   = Opcodes::V1_8;
    merged->access    = Opcodes::ACC_PUBLIC | Opcodes::ACC_SUPER;
    merged->name      = arena.intern(ca::str::Utf8StringRef::from_cstr(
        ("mjt/internal/ToolClassMergedClass_" + std::to_string(index)).c_str()));
    merged->superName = ca::str::Utf8StringRef::from_cstr("java/lang/Object");

    auto* init = new MethodNode(Opcodes::ACC_PUBLIC,
                                ca::str::Utf8StringRef::from_cstr("<init>"),
                                ca::str::Utf8StringRef::from_cstr("()V"),
                                ca::str::Utf8StringRef(),
                                {});
    init->instructions.add(init->create<VarInsnNode>(Opcodes::ALOAD, 0));
    init->instructions.add(init->create<MethodInsnNode>(Opcodes::INVOKESPECIAL,
                                              ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
                                              ca::str::Utf8StringRef::from_cstr("<init>"),
                                              ca::str::Utf8StringRef::from_cstr("()V"),
                                              false));
    init->instructions.add(init->create<InsnNode>(Opcodes::RETURN));
    init->maxStack  = 1;
    init->maxLocals = 1;
    merged->methods.push_back(init);
    return merged;
}

bool hasMergedFieldCollision(const ClassNode& merged, const FieldNode& field)
{
    for (auto* existing : merged.fields) {
        if (existing && existing->name.equals(field.name)) return true;
    }
    return false;
}

bool hasMergedMethodCollision(const ClassNode& merged, const MethodNode& method)
{
    for (auto* existing : merged.methods) {
        if (existing && existing->name.equals(method.name) && existing->desc.equals(method.desc))
            return true;
    }
    return false;
}

bool canMoveToolClassInto(const ClassNode& merged, ClassNode& source)
{
    if (merged.methods.size() + source.methods.size() > 100) return false;
    for (auto* field : source.fields) {
        if (field && hasMergedFieldCollision(merged, *field)) return false;
    }
    for (auto* method : source.methods) {
        if (!method || MethodUtil::isConstructor(*method) || MethodUtil::isClinit(*method))
            continue;
        if (hasMergedMethodCollision(merged, *method)) return false;
    }
    return true;
}

void moveToolClassMembers(ClassNode& source, ClassNode& merged)
{
    for (auto*& field : source.fields) {
        if (!field) continue;
        merged.fields.push_back(field);
        field = nullptr;
    }
    for (auto*& method : source.methods) {
        if (!method || MethodUtil::isConstructor(*method) || MethodUtil::isClinit(*method))
            continue;
        merged.methods.push_back(method);
        method = nullptr;
    }
}

}   //namespace


PreservedAnalyses ToolClassCombiner::run(ResourceContainer& container, GlobalAnalysisManager& gam)
{
    auto classes = loadClassesForInlining(container, gam.arena());
    if (classes.empty()) return PreservedAnalyses::all();

    std::vector<LoadedClass*> candidates;
    for (auto& loaded : classes) {
        if (!loaded.node ||
            loaded.node->name.to_std_string().find("ToolClassMergedClass_") != std::string::npos)
            continue;
        if (isToolClassCandidate(*loaded.node)) candidates.push_back(&loaded);
    }
    if (candidates.size() < 2) return PreservedAnalyses::all();

    std::vector<std::unique_ptr<ClassNode>> mergedClasses;
    MemberRemapPlan                         plan;
    std::vector<std::string>                deleteUris;

    for (auto* candidate : candidates) {
        ClassNode* target = nullptr;
        for (auto& merged : mergedClasses) {
            if (canMoveToolClassInto(*merged, *candidate->node)) {
                target = merged.get();
                break;
            }
        }
        if (!target) {
            mergedClasses.push_back(newMergedToolClass(gam.arena(), mergedClasses.size()));
            target = mergedClasses.back().get();
        }

        plan.mapClass(candidate->node->name, target->name);
        moveToolClassMembers(*candidate->node, *target);
        deleteUris.push_back(candidate->uri);
    }

    if (plan.empty() || mergedClasses.empty()) return PreservedAnalyses::all();

    for (auto& loaded : classes) {
        if (!loaded.node) continue;
        if (std::find(deleteUris.begin(), deleteUris.end(), loaded.uri) != deleteUris.end())
            continue;
        applyMemberRemapPlan(*loaded.node, gam.arena(), plan);
    }
    for (auto& merged : mergedClasses) {
        applyMemberRemapPlan(*merged, gam.arena(), plan);
    }

    for (const auto& uri : deleteUris) {
        container.getResources().erase(uri);
    }

    for (auto& loaded : classes) {
        if (!loaded.resource || !loaded.node) continue;
        if (std::find(deleteUris.begin(), deleteUris.end(), loaded.uri) != deleteUris.end())
            continue;
        writeClassNode(*loaded.resource, gam.arena(), *loaded.node);
    }

    for (auto& merged : mergedClasses) {
        ClassWriter writer(gam.arena(), 0);
        merged->accept(&writer);
        const auto uri = merged->name.to_std_string() + ".class";
        //合并类 URI 有意覆盖容器中残留的原类，不走 first-wins 导入语义。
        container.replaceResource(uri,
                                  std::make_unique<ClassResource>(uri, writer.toByteArray()));
    }

    return PreservedAnalyses::none();
}

}   //namespace mjt::base
