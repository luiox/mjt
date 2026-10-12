#include "mbase/pass/opt/enum_cast_check_remover.hpp"
#include "mbase/pass/opt/structure_detail.hpp"

namespace mjt::base {

PreservedAnalyses EnumCastCheckRemover::run(ClassNode& classNode, ClassAnalysisManager& cam)
{
    (void)cam;
    if ((classNode.access & Opcodes::ACC_ENUM) == 0) return PreservedAnalyses::all();

    //对齐 Java desc = "[L" + classNode.name + ";"
    ca::str::Utf8StringBuilder sb;
    sb.append("[L");
    sb.append(classNode.name);
    sb.append(";");
    auto desc = sb.build();

    if (classNode.methods.empty()) return PreservedAnalyses::all();
    auto* valueMethod = classNode.methods.front();
    if (!valueMethod) return PreservedAnalyses::all();

    bool                           modified = false;
    std::vector<AbstractInsnNode*> toRemove;
    for (auto* insn : valueMethod->instructions.toArray()) {
        if (insn->getType() == AbstractInsnNode::METHOD_INSN) {
            auto* methodInsn = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
            if (methodInsn->getOpcode() == Opcodes::INVOKEVIRTUAL &&
                methodInsn->desc.equals(desc)) {
                auto* next = insn->getNext();
                if (!next || next->getOpcode() != Opcodes::CHECKCAST) continue;
                toRemove.push_back(insn);
                toRemove.push_back(next);
                modified = true;
            }
        }
    }
    for (auto* insn : toRemove) {
        valueMethod->instructions.removeAndDelete(insn);
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
