#include "mbase/pass/opt/local_constant_propagation.hpp"
#include "mbase/asm/insn/insn_util.hpp"

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

PreservedAnalyses LocalConstantPropagation::run(jbytecode::tree::MethodNode& methodNode,
                                                MethodAnalysisManager&       context)
{
    (void)context;
    auto& insns = methodNode.instructions;
    if (insns.isEmpty()) return PreservedAnalyses::all();

    bool changed     = true;
    bool modifiedAny = false;
    while (changed) {
        changed = false;
        //Collect replacements: {old_insn → new_constant_insn}
        std::vector<std::pair<AbstractInsnNode*, AbstractInsnNode*>> replacements;
        //Map: var_index → constant instruction node
        std::unordered_map<int, AbstractInsnNode*> constMap;

        for (auto* insn : insns) {
            if (!insn) continue;
            int op = insn->getOpcode();

            if (insn->getType() == AbstractInsnNode::LABEL ||
                insn->getType() == AbstractInsnNode::FRAME || op == Opcodes::GOTO ||
                InsnUtil::isIf(op) || InsnUtil::isReturn(op) || op == Opcodes::ATHROW ||
                op == Opcodes::TABLESWITCH || op == Opcodes::LOOKUPSWITCH) {
                constMap.clear();
                continue;
            }

            //IINC 原地修改 slot，旧常量不再代表当前值；必须先失效追踪状态。
            if (insn->getType() == AbstractInsnNode::IINC_INSN) {
                constMap.erase(ca::core::tag_cast::cast<IincInsnNode>(insn)->var);
                continue;
            }

            //Track constant stores: store after a constant load
            {
                int  varIdx     = -1;
                bool isIntStore = false, isLongStore = false;
                bool isFloatStore = false, isDoubleStore = false;
                bool isRefStore = false;

                if (op == Opcodes::ISTORE) {
                    varIdx     = ca::core::tag_cast::cast<VarInsnNode>(insn)->var;
                    isIntStore = true;
                }
                else if (op == Opcodes::LSTORE) {
                    varIdx      = ca::core::tag_cast::cast<VarInsnNode>(insn)->var;
                    isLongStore = true;
                }
                else if (op == Opcodes::FSTORE) {
                    varIdx       = ca::core::tag_cast::cast<VarInsnNode>(insn)->var;
                    isFloatStore = true;
                }
                else if (op == Opcodes::DSTORE) {
                    varIdx        = ca::core::tag_cast::cast<VarInsnNode>(insn)->var;
                    isDoubleStore = true;
                }
                else if (op == Opcodes::ASTORE) {
                    varIdx     = ca::core::tag_cast::cast<VarInsnNode>(insn)->var;
                    isRefStore = true;
                }
                else if (op >= Constants::ISTORE_0 && op <= Constants::ISTORE_3) {
                    varIdx     = op - Constants::ISTORE_0;
                    isIntStore = true;
                }
                else if (op >= Constants::LSTORE_0 && op <= Constants::LSTORE_3) {
                    varIdx      = op - Constants::LSTORE_0;
                    isLongStore = true;
                }
                else if (op >= Constants::FSTORE_0 && op <= Constants::FSTORE_3) {
                    varIdx       = op - Constants::FSTORE_0;
                    isFloatStore = true;
                }
                else if (op >= Constants::DSTORE_0 && op <= Constants::DSTORE_3) {
                    varIdx        = op - Constants::DSTORE_0;
                    isDoubleStore = true;
                }
                else if (op >= Constants::ASTORE_0 && op <= Constants::ASTORE_3) {
                    varIdx     = op - Constants::ASTORE_0;
                    isRefStore = true;
                }

                if (varIdx >= 0) {
                    auto* prev    = insn->getPrevious();
                    bool  isConst = false;
                    if (prev && isIntStore) {
                        int val;
                        if (InsnUtil::getIntValue(prev, val)) isConst = true;
                    }
                    else if (prev && isLongStore) {
                        ca::i64 val;
                        if (InsnUtil::getLongValue(prev, val)) isConst = true;
                    }
                    else if (prev && isFloatStore) {
                        float val;
                        if (InsnUtil::getFloatValue(prev, val)) isConst = true;
                    }
                    else if (prev && isDoubleStore) {
                        double val;
                        if (InsnUtil::getDoubleValue(prev, val)) isConst = true;
                    }
                    else if (prev && isRefStore) {
                        isConst = (prev->getOpcode() == Opcodes::ACONST_NULL);
                    }

                    if (isConst)
                        constMap[varIdx] = prev;
                    else
                        constMap.erase(varIdx);
                    continue;
                }
            }

            //Resolve ILOAD to constant
            if (op == Opcodes::ILOAD) {
                auto* varInsn = ca::core::tag_cast::cast<VarInsnNode>(insn);
                int   varIdx  = varInsn->var;
                auto  it      = constMap.find(varIdx);
                if (it != constMap.end()) {
                    std::unordered_map<Label*, Label*> clonedLabels;
                    //B8 新 clone 协议：克隆经 insns.set 挂回 methodNode.instructions，
                    //产物归该方法 arena（常量生产者同 arena，Label 保身份直返）。
                    auto clone = it->second->clone(methodNode.arena(), clonedLabels);
                    replacements.emplace_back(insn, clone);
                    changed = true;
                }
                continue;
            }

            //Handle shorthand loads ILOAD_0 through ALOAD_3
            if (op >= Constants::ILOAD_0 && op <= Constants::ALOAD_3) {
                int varIdx;
                if (op >= Constants::ILOAD_0 && op <= Constants::ILOAD_3)
                    varIdx = op - Constants::ILOAD_0;
                else if (op >= Constants::LLOAD_0 && op <= Constants::LLOAD_3)
                    varIdx = op - Constants::LLOAD_0;
                else if (op >= Constants::FLOAD_0 && op <= Constants::FLOAD_3)
                    varIdx = op - Constants::FLOAD_0;
                else if (op >= Constants::DLOAD_0 && op <= Constants::DLOAD_3)
                    varIdx = op - Constants::DLOAD_0;
                else
                    varIdx = op - Constants::ALOAD_0;

                auto it = constMap.find(varIdx);
                if (it != constMap.end()) {
                    std::unordered_map<Label*, Label*> clonedLabels;
                    //B8 新 clone 协议：同上，产物归 methodNode 的 arena。
                    auto clone = it->second->clone(methodNode.arena(), clonedLabels);
                    replacements.emplace_back(insn, clone);
                    changed = true;
                }
            }
        }

        //Apply all replacements after collection (safe: no concurrent modification)
        for (auto& [oldInsn, newInsn] : replacements) {
            insns.set(oldInsn, newInsn);
            //NOTE: InsnList::set already deletes oldNode internally.
            //Do NOT delete oldInsn here.
            modifiedAny = true;
        }
    }
    return modifiedAny ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
