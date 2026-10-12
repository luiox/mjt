#include "mbase/pass/opt/label_dead_var_store_eliminator.hpp"
#include "mbase/pass/opt/structure_detail.hpp"
#include "mbase/asm/insn/insn_util.hpp"

namespace mjt::base {

PreservedAnalyses LabelDeadVarStoreEliminator::run(MethodNode&            methodNode,
                                                   MethodAnalysisManager& am)
{
    (void)am;
    auto& insns = methodNode.instructions;
    if (insns.isEmpty()) return PreservedAnalyses::all();

    //首尾 label 只作为临时分析边界，不能在 unchanged 路径泄漏到输入（dev_plan E pilot：
    //合成边界样板收敛到 InsnUtil::ensureBoundaryLabels；B8 起合成边界落方法内容 arena）。
    const auto boundary = InsnUtil::ensureBoundaryLabels(insns, methodNode.arena());

    std::unordered_set<AbstractInsnNode*> needRemove;
    for (int i = 0; i < insns.size() - 1;) {
        //找到区间起始 LabelNode
        AbstractInsnNode* start = insns.get(i);
        if (start->getType() != AbstractInsnNode::LABEL) {
            i++;
            continue;
        }

        int j = i + 1;
        //找到下一个 LabelNode，作为区间终点
        while (j < insns.size() && insns.get(j)->getType() != AbstractInsnNode::LABEL) {
            j++;
        }
        if (j - i <= 2) {
            i = j;
            continue;
        }

        //收缩区间，找到 iload 开始和 istore 结束
        int left  = i + 1;
        int right = j - 1;
        while (left < right && insns.get(left)->getOpcode() != Opcodes::ILOAD) {
            left++;
        }
        while (right > left && insns.get(right)->getOpcode() != Opcodes::ISTORE) {
            right--;
        }
        if (left >= right) {
            i = j;
            continue;
        }

        AbstractInsnNode* first = insns.get(left);
        AbstractInsnNode* last  = insns.get(right);
        if (first->getType() != AbstractInsnNode::VAR_INSN ||
            last->getType() != AbstractInsnNode::VAR_INSN) {
            i = j;
            continue;
        }
        auto* firstVar = ca::core::tag_cast::cast<VarInsnNode*>(first);
        auto* lastVar  = ca::core::tag_cast::cast<VarInsnNode*>(last);
        if (firstVar->getOpcode() != Opcodes::ILOAD || lastVar->getOpcode() != Opcodes::ISTORE ||
            firstVar->var != lastVar->var) {
            i = j;
            continue;
        }

        //检查区间 [left, right] 内是否只包含允许的指令
        bool valid = true;
        for (int k = left; k <= right; k++) {
            AbstractInsnNode* insn = insns.get(k);
            if (insn->getType() == AbstractInsnNode::VAR_INSN) {
                int op = insn->getOpcode();
                if (op != Opcodes::ILOAD && op != Opcodes::ISTORE) {
                    valid = false;
                    break;
                }
            }
            else if (!InsnUtil::isConstant(insn->getOpcode()) && !isArithmeticInsn(insn) &&
                     !InsnUtil::isStackOp(insn->getOpcode())) {
                valid = false;
                break;
            }
        }
        if (!valid) {
            i = j;
            continue;
        }

        //区间 [left, right] 内查找连续 istore i 和 iload i 对，消除
        for (int k = left + 1; k < right; k++) {
            AbstractInsnNode* prev = insns.get(k - 1);
            AbstractInsnNode* curr = insns.get(k);
            if (prev->getType() == AbstractInsnNode::VAR_INSN &&
                curr->getType() == AbstractInsnNode::VAR_INSN) {
                auto* p = ca::core::tag_cast::cast<VarInsnNode*>(prev);
                auto* c = ca::core::tag_cast::cast<VarInsnNode*>(curr);
                if (p->getOpcode() == Opcodes::ISTORE && c->getOpcode() == Opcodes::ILOAD &&
                    p->var == c->var && p->var == firstVar->var) {
                    needRemove.insert(p);
                    needRemove.insert(c);
                }
            }
        }

        i = j;
    }

    //合成边界不参与变换，任何返回路径都必须撤销，避免虚假的 changed。
    InsnUtil::removeBoundaryLabels(insns, boundary);

    if (needRemove.empty()) return PreservedAnalyses::all();

    //移除无效指令
    for (auto* insn : needRemove) {
        insns.removeAndDelete(insn);
    }

    return PreservedAnalyses::none();
}

}   //namespace mjt::base
