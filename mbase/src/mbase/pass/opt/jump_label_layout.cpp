#include "mbase/pass/opt/jump_label_layout.hpp"

#include "mbase/asm/insn/insn_util.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "libca/core/tag_cast.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

//============================================================================
//jump_label_layout 私有实现
//家族共享的 jump predicate / label 引用规范化 helper。原位于 control_flow_detail.hpp，
//因家族聚合后单消费者，合回本 TU。dead helper（eraseNodes / replaceWithPops）已删除。
//============================================================================

namespace {

static bool isTwoSlotJump(int opcode)
{
    switch (opcode) {
    case Opcodes::IF_ICMPEQ:
    case Opcodes::IF_ICMPNE:
    case Opcodes::IF_ICMPLT:
    case Opcodes::IF_ICMPGE:
    case Opcodes::IF_ICMPGT:
    case Opcodes::IF_ICMPLE:
    case Opcodes::IF_ACMPEQ:
    case Opcodes::IF_ACMPNE: return true;
    default: return false;
    }
}

}   //namespace

//============================================================================
//RedundantJumpRemover
//对应 Java controlflow/RedundantJumpRemover.java。
//删除落到下一个 label 的 goto；条件跳转用 pop 序列替换以保留栈平衡。
//============================================================================

PreservedAnalyses RedundantJumpRemover::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();

    bool  modified = false;
    auto* node     = methodNode.instructions.getFirst();
    while (node) {
        auto* next = node->getNext();
        if (node->getType() == AbstractInsnNode::JUMP_INSN) {
            auto* jump = ca::core::tag_cast::cast<JumpInsnNode*>(node);
            if (next && next->getType() == AbstractInsnNode::LABEL) {
                auto* label = ca::core::tag_cast::cast<LabelNode*>(next);
                //缺失目标标签属于畸形输入，不能把两个 null 当成可优化的相邻目标。
                if (jump->label && label->label && label->label == jump->label) {
                    if (jump->getOpcode() == Opcodes::GOTO) {
                        methodNode.instructions.removeAndDelete(node);
                    }
                    else {
                        const int popCount = isTwoSlotJump(jump->getOpcode()) ? 2 : 1;
                        auto*     firstPop = methodNode.create<InsnNode>(Opcodes::POP);
                        methodNode.instructions.set(node, firstPop);
                        if (popCount > 1) {
                            methodNode.instructions.insert(label, methodNode.create<InsnNode>(Opcodes::POP));
                        }
                    }
                    modified = true;
                    node     = methodNode.instructions.getFirst();
                    continue;
                }
            }
        }
        node = next;
    }

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//GotoRearranger
//对应 Java controlflow/GotoRearranger.java。
//把单入边的简单基本块搬到 goto 前并删除该 goto，减少跳转次数。
//============================================================================

PreservedAnalyses GotoRearranger::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty() || !methodNode.instructions.getFirst()) {
        return PreservedAnalyses::all();
    }

    bool modified = false;
    bool anyModified;

    //缓存：Label -> LabelNode 映射
    std::unordered_map<jbytecode::core::Label*, LabelNode*> labelToNode;
    for (auto* insn : methodNode.instructions) {
        if (insn->getType() == AbstractInsnNode::LABEL) {
            auto* ln               = ca::core::tag_cast::cast<LabelNode*>(insn);
            labelToNode[ln->label] = ln;
        }
    }

    do {
        anyModified = false;

        //统计每个 Label 被跳转的次数
        std::unordered_map<jbytecode::core::Label*, int> jumpCount;
        for (auto* insn : methodNode.instructions) {
            if (insn->getType() == AbstractInsnNode::JUMP_INSN) {
                auto* jump = ca::core::tag_cast::cast<JumpInsnNode*>(insn);
                jumpCount[jump->label] += 1;
            }
            else if (insn->getType() == AbstractInsnNode::TABLESWITCH_INSN) {
                auto* sw = ca::core::tag_cast::cast<TableSwitchInsnNode*>(insn);
                jumpCount[sw->dflt] += 1;
                for (auto* l : sw->labels) jumpCount[l] += 1;
            }
            else if (insn->getType() == AbstractInsnNode::LOOKUPSWITCH_INSN) {
                auto* sw = ca::core::tag_cast::cast<LookupSwitchInsnNode*>(insn);
                jumpCount[sw->dflt] += 1;
                for (auto* l : sw->labels) jumpCount[l] += 1;
            }
        }

        //try-catch 的 label 不可移动（设大跳转计数以保护）
        if (!methodNode.tryCatchBlocks.empty()) {
            for (auto* tcb : methodNode.tryCatchBlocks) {
                jumpCount[tcb->start]   = 999;
                jumpCount[tcb->end]     = 999;
                jumpCount[tcb->handler] = 999;
            }
        }

        //遍历指令，寻找可优化的 goto。
        //显式 next 预存写法（safety.md 快照法）：本循环尾部会 removeAndDelete(insn)
        //后立即 break 重启外循环——不用 range-for（活 Iterator 会让 InsnList 的
        //debug「迭代中改表」断言拒绝这种删除后即弃形态）。
        for (auto* insn = methodNode.instructions.getFirst(); insn; insn = insn->getNext()) {
            if (insn->getType() != AbstractInsnNode::JUMP_INSN) continue;
            auto* cast = ca::core::tag_cast::cast<JumpInsnNode*>(insn);
            if (cast->getOpcode() != Opcodes::GOTO) continue;
            if (jumpCount[cast->label] != 1) continue;

            auto* targetLabel = labelToNode[cast->label];
            if (!targetLabel) continue;

            //Java 活代码：检查目标 label 前一条指令是否为终止指令
            auto* prev = targetLabel->getPrevious();
            if (!prev) continue;
            int  prevOp = prev->getOpcode();
            bool ok     = (prevOp >= Opcodes::IRETURN && prevOp <= Opcodes::RETURN) ||
                      prevOp == Opcodes::ATHROW || prevOp == Opcodes::GOTO ||
                      prevOp == Opcodes::TABLESWITCH || prevOp == Opcodes::LOOKUPSWITCH;

            //从 targetLabel 开始往后走，直到终止指令或遇到被引用的 label
            AbstractInsnNode* walk = targetLabel;
            while (walk && ok) {
                if (walk == insn) {
                    ok = false;
                    break;
                }

                //检查 try-catch 边界
                for (auto* tcb : methodNode.tryCatchBlocks) {
                    auto* tcbStartNode = labelToNode[tcb->start];
                    auto* tcbEndNode   = labelToNode[tcb->end];
                    if (!tcbStartNode || !tcbEndNode) continue;
                    int startIdx = methodNode.instructions.indexOf(tcbStartNode);
                    int midIdx   = methodNode.instructions.indexOf(walk);
                    int endIdx   = methodNode.instructions.indexOf(tcbEndNode);
                    if (startIdx <= midIdx && midIdx < endIdx) {
                        int insnIdx = methodNode.instructions.indexOf(insn);
                        if (insnIdx < startIdx || insnIdx >= endIdx) {
                            ok = false;
                        }
                    }
                    if (!ok) break;
                }
                if (!ok) break;

                auto* nextWalk = walk->getNext();
                //中途有其他跳转引用的 label 则不可移动（但不包括目标 label 自身）
                if (walk != targetLabel && walk->getType() == AbstractInsnNode::LABEL) {
                    auto* ln = ca::core::tag_cast::cast<LabelNode*>(walk);
                    if (jumpCount[ln->label] > 0) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) break;

                //遇到终止指令则停止（终止指令属于被移动的块）
                int op2 = walk->getOpcode();
                if ((op2 >= Opcodes::IRETURN && op2 <= Opcodes::RETURN) || op2 == Opcodes::ATHROW ||
                    op2 == Opcodes::GOTO) {
                    break;
                }
                walk = nextWalk;
            }

            if (!ok) continue;

            //收集 targetLabel 后到终止指令（含）的所有节点
            auto*                          collectStart = targetLabel->getNext();
            std::vector<AbstractInsnNode*> toMove;
            for (auto* n = collectStart; n; n = n->getNext()) {
                toMove.push_back(n);
                int op2 = n->getOpcode();
                if ((op2 >= Opcodes::IRETURN && op2 <= Opcodes::RETURN) || op2 == Opcodes::ATHROW ||
                    op2 == Opcodes::GOTO) {
                    break;
                }
            }

            //将基本块移到 goto 后，再移除 goto 自身（moved 绑定方法 arena：中转
            //期间节点属方法域产物，异常路径析构只摘链不 delete，B7）
            InsnList moved(&methodNode.arena());
            for (auto* n : toMove) {
                methodNode.instructions.remove(n);
                moved.add(n);
            }
            if (auto* afterGoto = insn->getNext()) {
                methodNode.instructions.insert(afterGoto, moved);
            }
            else {
                methodNode.instructions.add(moved);
            }
            methodNode.instructions.removeAndDelete(insn);

            anyModified = true;
            modified    = true;
            break;   //重启外循环
        }

    } while (anyModified);

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//ContinuousLabelMerger
//对应 Java controlflow/ContinuousLabelMerger.java。
//合并物理连续的 LabelNode，重定向跳转/switch/try-catch/局部变量/行号引用到首个 label。
//============================================================================

PreservedAnalyses ContinuousLabelMerger::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    auto& insns = methodNode.instructions;
    if (insns.isEmpty()) return PreservedAnalyses::all();

    //先搜索连续的 label 区域 [start, end)
    auto                             instructions = insns.toArray();
    std::vector<std::pair<int, int>> continuousLabelAreas;
    for (int i = 0; i < static_cast<int>(instructions.size());) {
        auto* insn = instructions[static_cast<std::size_t>(i)];
        if (insn->getType() == AbstractInsnNode::LABEL) {
            int start = i;
            while (i < static_cast<int>(instructions.size()) &&
                   instructions[static_cast<std::size_t>(i)]->getType() ==
                       AbstractInsnNode::LABEL) {
                i++;
            }
            continuousLabelAreas.emplace_back(start, i);
        }
        else {
            i++;
        }
    }

    //剔除 end - start 为 1 的（只有一个 label 的区间）
    continuousLabelAreas.erase(std::remove_if(continuousLabelAreas.begin(),
                                              continuousLabelAreas.end(),
                                              [](const std::pair<int, int>& area) {
                                                  return area.second - area.first == 1;
                                              }),
                               continuousLabelAreas.end());

    if (continuousLabelAreas.empty()) return PreservedAnalyses::all();

    //Frame 中的 Label 表示未初始化对象的 NEW 指令位置。当前不改写 Any 内的引用，
    //因此这些 Label 即使与前一个 Label 连续也必须保留。
    std::unordered_set<Label*> frameLabels;
    for (auto* insn : instructions) {
        if (insn->getType() != AbstractInsnNode::FRAME) continue;
        auto* frame = ca::core::tag_cast::cast<FrameNode*>(insn);
        InsnUtil::collectFrameLabels(frame->local, frameLabels);
        InsnUtil::collectFrameLabels(frame->stack, frameLabels);
    }

    //构造替换表：把第一个起始的 Label 作为目标值
    std::unordered_map<Label*, Label*> replaceTable;
    std::vector<AbstractInsnNode*>     deleteList;

    for (const auto& area : continuousLabelAreas) {
        int   start = area.first;
        int   end   = area.second;
        auto* target =
            ca::core::tag_cast::cast<LabelNode*>(instructions[static_cast<std::size_t>(start)])->label;
        for (int i = start + 1; i < end; i++) {
            auto* redundantLabel =
                ca::core::tag_cast::cast<LabelNode*>(instructions[static_cast<std::size_t>(i)])->label;
            if (frameLabels.count(redundantLabel) != 0) continue;
            replaceTable[redundantLabel] = target;
            deleteList.push_back(instructions[static_cast<std::size_t>(i)]);
        }
    }

    if (deleteList.empty()) return PreservedAnalyses::all();

    //扫描 tryCatchBlocks 中的 label 引用，进行替换
    for (auto* tryBlock : methodNode.tryCatchBlocks) {
        if (!tryBlock) continue;
        if (replaceTable.count(tryBlock->start)) {
            tryBlock->start = replaceTable[tryBlock->start];
        }
        if (replaceTable.count(tryBlock->end)) {
            tryBlock->end = replaceTable[tryBlock->end];
        }
        if (replaceTable.count(tryBlock->handler)) {
            tryBlock->handler = replaceTable[tryBlock->handler];
        }
    }

    //扫描所有指令中的 label 引用，进行替换
    for (auto* insn : instructions) {
        if (insn->getType() == AbstractInsnNode::JUMP_INSN) {
            auto* jumpInsn = ca::core::tag_cast::cast<JumpInsnNode*>(insn);
            auto  it       = replaceTable.find(jumpInsn->label);
            if (it != replaceTable.end()) {
                jumpInsn->label = it->second;
            }
        }
        else if (insn->getType() == AbstractInsnNode::TABLESWITCH_INSN) {
            auto* tableSwitch = ca::core::tag_cast::cast<TableSwitchInsnNode*>(insn);
            auto  it          = replaceTable.find(tableSwitch->dflt);
            if (it != replaceTable.end()) {
                tableSwitch->dflt = it->second;
            }
            std::vector<Label*> newLabels;
            newLabels.reserve(tableSwitch->labels.size());
            for (auto* entry : tableSwitch->labels) {
                auto it2 = replaceTable.find(entry);
                if (it2 != replaceTable.end()) {
                    newLabels.push_back(it2->second);
                }
                else {
                    newLabels.push_back(entry);
                }
            }
            tableSwitch->labels = std::move(newLabels);
        }
        else if (insn->getType() == AbstractInsnNode::LOOKUPSWITCH_INSN) {
            auto* lookupSwitch = ca::core::tag_cast::cast<LookupSwitchInsnNode*>(insn);
            auto  it           = replaceTable.find(lookupSwitch->dflt);
            if (it != replaceTable.end()) {
                lookupSwitch->dflt = it->second;
            }
            std::vector<Label*> newLabels;
            newLabels.reserve(lookupSwitch->labels.size());
            for (auto* entry : lookupSwitch->labels) {
                auto it2 = replaceTable.find(entry);
                if (it2 != replaceTable.end()) {
                    newLabels.push_back(it2->second);
                }
                else {
                    newLabels.push_back(entry);
                }
            }
            lookupSwitch->labels = std::move(newLabels);
        }
        else if (insn->getType() == AbstractInsnNode::LINE) {
            auto* line = ca::core::tag_cast::cast<LineNumberNode*>(insn);
            auto  it   = replaceTable.find(line->start);
            if (it != replaceTable.end()) line->start = it->second;
        }
    }

    for (auto* local : methodNode.localVariables) {
        if (!local) continue;
        auto startIt = replaceTable.find(local->start);
        if (startIt != replaceTable.end()) local->start = startIt->second;
        auto endIt = replaceTable.find(local->end);
        if (endIt != replaceTable.end()) local->end = endIt->second;
    }

    auto remapLocalAnnotations = [&](std::vector<LocalVariableAnnotationNode*>& annotations) {
        for (auto* annotation : annotations) {
            if (!annotation) continue;
            for (auto*& start : annotation->start) {
                auto it = replaceTable.find(start);
                if (it != replaceTable.end()) start = it->second;
            }
            for (auto*& end : annotation->end) {
                auto it = replaceTable.find(end);
                if (it != replaceTable.end()) end = it->second;
            }
        }
    };
    remapLocalAnnotations(methodNode.visibleLocalVariableAnnotations);
    remapLocalAnnotations(methodNode.invisibleLocalVariableAnnotations);

    //删除多余的 LabelNode
    for (auto* insn : deleteList) {
        insns.removeAndDelete(insn);
    }

    return PreservedAnalyses::none();
}

//============================================================================
//ImpossibleJumpEliminater
//对应 Java controlflow/ImpossibleJumpEliminater.java。
//删除 goto 到下一个 label 之间的不可达指令岛。
//============================================================================

PreservedAnalyses ImpossibleJumpEliminater::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();

    bool  modified = false;
    auto* node     = methodNode.instructions.getFirst();
    while (node) {
        auto* next = node->getNext();
        if (node->getType() == AbstractInsnNode::JUMP_INSN && node->getOpcode() == Opcodes::GOTO) {
            //跳过 goto 到下一个 label 之间的死代码
            while (next && next->getType() != AbstractInsnNode::LABEL) {
                auto* dead = next;
                next       = next->getNext();
                methodNode.instructions.removeAndDelete(dead);
                modified = true;
            }
            node = next;
            continue;
        }
        node = next;
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
