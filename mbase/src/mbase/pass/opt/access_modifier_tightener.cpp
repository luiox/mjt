#include "mbase/pass/opt/access_modifier_tightener.hpp"
#include "mbase/pass/opt/recover_detail.hpp"
#include "mbase/spi/class_resource_pending_commit.hpp"

namespace mjt::base {

PreservedAnalyses AccessModifierTightener::run(ResourceContainer&     container,
                                               GlobalAnalysisManager& gam)
{
    auto classes = loadRecoverClasses(container, gam.arena());
    if (classes.empty()) return PreservedAnalyses::all();

    std::unordered_map<std::string, StaticMemberUse> methodUses;
    std::unordered_map<std::string, StaticMemberUse> fieldUses;

    for (const auto& loaded : classes) {
        const auto ownerName = loaded.classNode->name;
        for (auto* method : loaded.classNode->methods) {
            if (!method) continue;
            for (auto* insn : method->instructions.toArray()) {
                if (!insn) continue;
                if (insn->getType() == AbstractInsnNode::METHOD_INSN &&
                    insn->getOpcode() == Opcodes::INVOKESTATIC) {
                    auto* methodInsn = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
                    auto& use        = methodUses[memberUseKey(
                        methodInsn->owner, methodInsn->name, methodInsn->desc)];
                    if (!methodInsn->owner.equals(ownerName)) use.usedOuter = true;
                }
                else if (insn->getType() == AbstractInsnNode::FIELD_INSN &&
                         (insn->getOpcode() == Opcodes::GETSTATIC ||
                          insn->getOpcode() == Opcodes::PUTSTATIC)) {
                    auto* fieldInsn = ca::core::tag_cast::cast<FieldInsnNode*>(insn);
                    auto& use =
                        fieldUses[memberUseKey(fieldInsn->owner, fieldInsn->name, fieldInsn->desc)];
                    if (!fieldInsn->owner.equals(ownerName)) use.usedOuter = true;
                }
            }
        }
    }

    bool modifiedAny = false;
    ClassResourcePendingCommit pending;
    for (auto& loaded : classes) {
        bool modifiedClass = false;
        for (auto* method : loaded.classNode->methods) {
            if (!method) continue;
            const auto it =
                methodUses.find(memberUseKey(loaded.classNode->name, method->name, method->desc));
            if (it == methodUses.end() || it->second.usedOuter) continue;
            if (MethodUtil::isStatic(*method) && MethodUtil::isPublic(*method)) {
                method->access = (method->access & ~Opcodes::ACC_PUBLIC) | Opcodes::ACC_PRIVATE;
                modifiedClass  = true;
            }
        }
        for (auto* field : loaded.classNode->fields) {
            if (!field) continue;
            const auto it =
                fieldUses.find(memberUseKey(loaded.classNode->name, field->name, field->desc));
            if (it == fieldUses.end() || it->second.usedOuter) continue;
            if (FieldUtil::isStatic(*field) && FieldUtil::isPublic(*field)) {
                field->access = (field->access & ~Opcodes::ACC_PUBLIC) | Opcodes::ACC_PRIVATE;
                modifiedClass = true;
            }
        }
        if (!modifiedClass || !loaded.resource) continue;
        //两阶段提交：写回推迟到循环结束（作业级原子性）。
        pending.stage(loaded.resource,
                     writeRecoverClassNode(gam.arena(), *loaded.classNode));
        modifiedAny = true;
    }

    pending.commit();
    return modifiedAny ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
