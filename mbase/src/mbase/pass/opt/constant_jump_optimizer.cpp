#include "mbase/pass/opt/constant_jump_optimizer.hpp"

#include "mbase/asm/analyzer/constant_interpreter.hpp"
#include "mbase/asm/cfg/control_flow_graph.hpp"
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
using namespace jbytecode::analysis;

//============================================================================
//constant_jump_optimizer 私有实现
//家族共享的 jump predicate / switch 校验 / goto 替换 helper。原位于
//control_flow_detail.hpp，因家族聚合后单消费者，合回本 TU。
//============================================================================

namespace {

static bool isIntInsn(AbstractInsnNode* insn, int& value)
{
    return insn && InsnUtil::getIntValue(insn, value);
}

static bool isNullInsn(AbstractInsnNode* insn)
{
    return insn && insn->getOpcode() == Opcodes::ACONST_NULL;
}

static bool containsLabel(const InsnList& instructions, Label* label)
{
    if (!label) return false;
    for (auto* node = instructions.getFirst(); node; node = node->getNext()) {
        if (node->getType() == AbstractInsnNode::LABEL &&
            ca::core::tag_cast::cast<LabelNode*>(node)->label == label) {
            return true;
        }
    }
    return false;
}

static bool hasValidLookupTargets(const InsnList& instructions, const LookupSwitchInsnNode& sw)
{
    if (sw.keys.size() != sw.labels.size() || !containsLabel(instructions, sw.dflt)) return false;
    for (auto* label : sw.labels) {
        if (!containsLabel(instructions, label)) return false;
    }
    return true;
}

static bool hasValidTableTargets(const InsnList& instructions, const TableSwitchInsnNode& sw)
{
    if (sw.min > sw.max || !containsLabel(instructions, sw.dflt)) return false;
    const auto expected = static_cast<unsigned long long>(
        static_cast<long long>(sw.max) - static_cast<long long>(sw.min) + 1);
    if (expected != sw.labels.size()) return false;
    for (auto* label : sw.labels) {
        if (!containsLabel(instructions, label)) return false;
    }
    return true;
}

static bool isOneSlotJump(int opcode)
{
    //Java is1ConditionJump 仅 IFEQ..IFLE；IFNULL/IFNONNULL 是 C++ 扩展
    //（Java 注释的老算法里有，matcher 活代码丢了）。保留：混淆样本里
    //aconst_null + ifnull/ifnonnull 常见，不处理会漏优化。
    switch (opcode) {
    case Opcodes::IFEQ:
    case Opcodes::IFNE:
    case Opcodes::IFLT:
    case Opcodes::IFGE:
    case Opcodes::IFGT:
    case Opcodes::IFLE:
    case Opcodes::IFNULL:
    case Opcodes::IFNONNULL: return true;
    default: return false;
    }
}

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

static bool predicateForOneSlot(int opcode, int value)
{
    switch (opcode) {
    case Opcodes::IFEQ: return value == 0;
    case Opcodes::IFNE: return value != 0;
    case Opcodes::IFLT: return value < 0;
    case Opcodes::IFGE: return value >= 0;
    case Opcodes::IFGT: return value > 0;
    case Opcodes::IFLE: return value <= 0;
    case Opcodes::IFNULL: return true;
    case Opcodes::IFNONNULL: return false;
    default: return false;
    }
}

static bool predicateForTwoSlot(int opcode, int value1, int value2)
{
    switch (opcode) {
    case Opcodes::IF_ICMPEQ: return value1 == value2;
    case Opcodes::IF_ICMPNE: return value1 != value2;
    case Opcodes::IF_ICMPLT: return value1 < value2;
    case Opcodes::IF_ICMPGE: return value1 >= value2;
    case Opcodes::IF_ICMPGT: return value1 > value2;
    case Opcodes::IF_ICMPLE: return value1 <= value2;
    case Opcodes::IF_ACMPEQ: return false;
    case Opcodes::IF_ACMPNE: return false;
    default: return false;
    }
}

static void replaceWithGoto(InsnList& insns, AbstractInsnNode* insn, Label* target)
{
    insns.set(insn, insns.create<JumpInsnNode>(Opcodes::GOTO, target));
}

///Label* → LabelNode*（折叠窗口落点引用检查用）。折叠只删指令不建 label，
///建一次即可；被删指令窗口随活链表取，map 中残留的悬空节点不会命中窗口。
struct LabelNodeMap
{
    std::unordered_map<Label*, LabelNode*> byLabel;

    explicit LabelNodeMap(MethodNode& method)
    {
        for (auto* n = method.instructions.getFirst(); n; n = n->getNext()) {
            if (n->getType() == AbstractInsnNode::LABEL) {
                auto* ln = ca::core::tag_cast::cast<LabelNode*>(n);
                byLabel.emplace(ln->label, ln);
            }
        }
    }
};

///@brief [from, to] 窗口内的 label 是否被其他控制流引用。
///
///折叠 `const; IF*` / `const; switch` 为 goto 会取消该条件跳转的一次 pop。
///若外部跳转/异常边界恰好落入窗口内的 label（即落在条件跳转自身上），
///那条路径的栈残留会失去这次消费，产生 VerifyError。self 自身的出边
///（向后跳回窗口的自环）不计——自环折叠成 goto 后栈语义不变。
static bool windowExternallyReferenced(MethodNode& method, AbstractInsnNode* from,
                                       AbstractInsnNode* to, AbstractInsnNode* self,
                                       const LabelNodeMap& labels)
{
    std::unordered_set<AbstractInsnNode*> window;
    for (auto* n = from; n; n = n->getNext()) {
        if (n->getType() == AbstractInsnNode::LABEL) window.insert(n);
        if (n == to) break;
    }
    if (window.empty()) return false;

    auto inWindow = [&](Label* label) -> bool {
        if (!label) return false;
        auto it = labels.byLabel.find(label);
        return it != labels.byLabel.end() && window.count(it->second) > 0;
    };
    for (auto* insn = method.instructions.getFirst(); insn; insn = insn->getNext()) {
        if (insn == self) continue;
        const int t = insn->getType();
        if (t == AbstractInsnNode::JUMP_INSN) {
            if (inWindow(ca::core::tag_cast::cast<JumpInsnNode*>(insn)->label)) return true;
        }
        else if (t == AbstractInsnNode::TABLESWITCH_INSN) {
            auto* sw = ca::core::tag_cast::cast<TableSwitchInsnNode*>(insn);
            if (inWindow(sw->dflt)) return true;
            for (auto* label : sw->labels) {
                if (inWindow(label)) return true;
            }
        }
        else if (t == AbstractInsnNode::LOOKUPSWITCH_INSN) {
            auto* sw = ca::core::tag_cast::cast<LookupSwitchInsnNode*>(insn);
            if (inWindow(sw->dflt)) return true;
            for (auto* label : sw->labels) {
                if (inWindow(label)) return true;
            }
        }
    }
    for (const auto* block : method.tryCatchBlocks) {
        if (inWindow(block->start) || inWindow(block->end) || inWindow(block->handler)) {
            return true;
        }
    }
    return false;
}

}   //namespace

//============================================================================
//ConsJumpOptimizer
//对应 Java controlflow/ConsJumpOptimizer.java。折叠常量条件跳转为 goto 或删除。
//============================================================================

PreservedAnalyses ConsJumpOptimizer::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();

    bool  modified = false;
    LabelNodeMap labelNodes(methodNode);
    auto* node     = methodNode.instructions.getFirst();
    while (node) {
        auto* next = node->getNext();
        if (next && next->getType() == AbstractInsnNode::JUMP_INSN) {
            auto*     jump   = ca::core::tag_cast::cast<JumpInsnNode*>(next);
            const int opcode = jump->getOpcode();
            if (isOneSlotJump(opcode)) {
                int value = 0;
                if (isNullInsn(node)) {
                    if (opcode == Opcodes::IFNULL || opcode == Opcodes::IFNONNULL) {
                        const bool taken = predicateForOneSlot(opcode, 0);
                        if (windowExternallyReferenced(methodNode, node, next, next,
                                                       labelNodes)) {
                            node = next;
                            continue;
                        }
                        if (taken) {
                            replaceWithGoto(methodNode.instructions, node, jump->label);
                            methodNode.instructions.removeAndDelete(next);
                        }
                        else {
                            methodNode.instructions.removeAndDelete(node);
                            methodNode.instructions.removeAndDelete(next);
                        }
                        modified = true;
                        node     = methodNode.instructions.getFirst();
                        continue;
                    }
                }
                else if (isIntInsn(node, value)) {
                    const bool taken = predicateForOneSlot(opcode, value);
                    if (windowExternallyReferenced(methodNode, node, next, next,
                                                   labelNodes)) {
                        node = next;
                        continue;
                    }
                    if (taken) {
                        replaceWithGoto(methodNode.instructions, node, jump->label);
                        methodNode.instructions.removeAndDelete(next);
                    }
                    else {
                        methodNode.instructions.removeAndDelete(node);
                        methodNode.instructions.removeAndDelete(next);
                    }
                    modified = true;
                    node     = methodNode.instructions.getFirst();
                    continue;
                }
            }
            else if (isTwoSlotJump(opcode)) {
                auto* prev   = node->getPrevious();
                int   value2 = 0;
                int   value1 = 0;
                if (isIntInsn(prev, value1) && isIntInsn(node, value2)) {
                    const bool taken      = predicateForTwoSlot(opcode, value1, value2);
                    auto*      firstConst = prev;
                    if (windowExternallyReferenced(methodNode, firstConst, next, next,
                                                   labelNodes)) {
                        node = next;
                        continue;
                    }
                    if (taken) {
                        replaceWithGoto(methodNode.instructions, firstConst, jump->label);
                        methodNode.instructions.removeAndDelete(node);
                        methodNode.instructions.removeAndDelete(next);
                    }
                    else {
                        methodNode.instructions.removeAndDelete(firstConst);
                        methodNode.instructions.removeAndDelete(node);
                        methodNode.instructions.removeAndDelete(next);
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
//ConstantSwitchFolder
//对应 Java controlflow/ConstantSwitchFolder.java。折叠常量 switch selector 为 goto。
//============================================================================

PreservedAnalyses ConstantSwitchFolder::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();

    bool  modified = false;
    LabelNodeMap labelNodes(methodNode);
    auto* node     = methodNode.instructions.getFirst();
    while (node) {
        auto* next  = node->getNext();
        int   value = 0;
        if (!isIntInsn(node, value)) {
            node = next;
            continue;
        }

        if (next && next->getType() == AbstractInsnNode::LOOKUPSWITCH_INSN) {
            auto*  sw     = ca::core::tag_cast::cast<LookupSwitchInsnNode*>(next);
            if (!hasValidLookupTargets(methodNode.instructions, *sw)) {
                node = next;
                continue;
            }
            if (windowExternallyReferenced(methodNode, node, next, next, labelNodes)) {
                node = next;
                continue;
            }
            Label* target = sw->dflt;
            for (ca::usize i = 0; i < sw->keys.size(); ++i) {
                if (sw->keys[i] == value) {
                    target = sw->labels[i];
                    break;
                }
            }
            replaceWithGoto(methodNode.instructions, node, target);
            methodNode.instructions.removeAndDelete(next);
            modified = true;
            node     = methodNode.instructions.getFirst();
            continue;
        }
        if (next && next->getType() == AbstractInsnNode::TABLESWITCH_INSN) {
            auto*  sw     = ca::core::tag_cast::cast<TableSwitchInsnNode*>(next);
            if (!hasValidTableTargets(methodNode.instructions, *sw)) {
                node = next;
                continue;
            }
            if (windowExternallyReferenced(methodNode, node, next, next, labelNodes)) {
                node = next;
                continue;
            }
            Label* target = sw->dflt;
            if (value >= sw->min && value <= sw->max) {
                const int idx = value - sw->min;
                if (idx >= 0 && idx < static_cast<int>(sw->labels.size())) {
                    target = sw->labels[static_cast<std::size_t>(idx)];
                }
            }
            replaceWithGoto(methodNode.instructions, node, target);
            methodNode.instructions.removeAndDelete(next);
            modified = true;
            node     = methodNode.instructions.getFirst();
            continue;
        }

        node = next;
    }

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//ConstantStateMachineEliminator
//对应 Java controlflow/ConstantStateMachineEliminator.java。
//消除常量状态机（混淆器常用的 state dispatch 模式），把多轮调用串成直线控制流。
//实验性 opt-in，需要 frame/CFG 证明状态值唯一。
//============================================================================

PreservedAnalyses ConstantStateMachineEliminator::run(MethodNode&            methodNode,
                                                      MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty() || !methodNode.tryCatchBlocks.empty())
        return PreservedAnalyses::all();

    auto nodes = methodNode.instructions.toArray();
    for (int i = 0; i + 5 < static_cast<int>(nodes.size()); ++i) {
        int initialState = 0;
        if (!isIntInsn(nodes[i], initialState) || nodes[i + 1]->getOpcode() != Opcodes::ISTORE ||
            nodes[i + 2]->getOpcode() != Opcodes::GOTO ||
            nodes[i + 3]->getType() != AbstractInsnNode::LABEL ||
            nodes[i + 4]->getOpcode() != Opcodes::ILOAD)
            continue;

        auto* stateStore = ca::core::tag_cast::cast<VarInsnNode*>(nodes[i + 1]);
        auto* dispatcher = ca::core::tag_cast::cast<LabelNode*>(nodes[i + 3])->label;
        auto* stateLoad  = ca::core::tag_cast::cast<VarInsnNode*>(nodes[i + 4]);
        if (stateStore->var != stateLoad->var) continue;

        //Confirm the initial state with the same constant value lattice used
        //by the constant-folding passes; direct opcode matching alone is not
        //the only source of constant semantics in this pass.
        BasicReferenceHandler refHandler;
        ConstantInterpreter   constantInterpreter(&refHandler);
        const auto            initialValue = constantInterpreter.newOperation(nodes[i]);
        if (!initialValue.isKnown() || !initialValue.isInteger() ||
            initialValue.getAsInteger() != initialState)
            continue;
        if (nodes[i + 5]->getType() != AbstractInsnNode::LOOKUPSWITCH_INSN &&
            nodes[i + 5]->getType() != AbstractInsnNode::TABLESWITCH_INSN)
            continue;

        auto* initialGoto = ca::core::tag_cast::cast<JumpInsnNode*>(nodes[i + 2]);
        if (initialGoto->label != dispatcher) continue;
        std::vector<int>    keys;
        std::vector<Label*> targets;
        Label*              defaultTarget = nullptr;
        if (nodes[i + 5]->getType() == AbstractInsnNode::LOOKUPSWITCH_INSN) {
            auto* sw = ca::core::tag_cast::cast<LookupSwitchInsnNode*>(nodes[i + 5]);
            keys.assign(sw->keys.begin(), sw->keys.end());
            targets.assign(sw->labels.begin(), sw->labels.end());
            defaultTarget = sw->dflt;
        }
        else {
            auto* sw      = ca::core::tag_cast::cast<TableSwitchInsnNode*>(nodes[i + 5]);
            defaultTarget = sw->dflt;
            for (int key = sw->min; key <= sw->max; ++key) {
                keys.push_back(key);
                targets.push_back(sw->labels[static_cast<std::size_t>(key - sw->min)]);
            }
        }
        if (keys.size() != targets.size() || targets.empty()) continue;
        std::unordered_map<int, Label*> dispatch;
        bool                            duplicate = false;
        for (size_t k = 0; k < keys.size(); ++k) {
            if (!dispatch.emplace(keys[k], targets[k]).second) duplicate = true;
        }
        if (duplicate) continue;

        std::unordered_map<Label*, int> labelIndex;
        for (int n = i + 6; n < static_cast<int>(nodes.size()); ++n) {
            if (nodes[n]->getType() == AbstractInsnNode::LABEL)
                labelIndex[ca::core::tag_cast::cast<LabelNode*>(nodes[n])->label] = n;
        }
        if (!labelIndex.count(defaultTarget)) continue;
        bool missingLabel = false;
        for (auto* target : targets) {
            if (!labelIndex.count(target)) missingLabel = true;
        }
        if (missingLabel) continue;

        struct CaseInfo
        {
            Label*            label          = nullptr;
            bool              terminal       = false;
            int               nextState      = 0;
            int               start          = 0;
            int               end            = 0;
            JumpInsnNode*     back           = nullptr;
            AbstractInsnNode* stateConst     = nullptr;
            AbstractInsnNode* stateStoreInsn = nullptr;
        };
        std::unordered_map<Label*, CaseInfo> cases;
        bool                                 invalid    = false;
        std::vector<Label*>                  allTargets = targets;
        allTargets.push_back(defaultTarget);
        for (auto* label : allTargets) {
            if (cases.count(label)) continue;
            int start = labelIndex[label];
            int end   = static_cast<int>(nodes.size());
            for (int n = start + 1; n < static_cast<int>(nodes.size()); ++n) {
                if (nodes[n]->getType() == AbstractInsnNode::LABEL) {
                    end = n;
                    break;
                }
            }
            if (end <= start + 1) {
                invalid = true;
                break;
            }
            CaseInfo info;
            info.label = label;
            info.start = start;
            info.end   = end;
            auto* last = nodes[end - 1];
            if (last->getOpcode() == Opcodes::IRETURN || last->getOpcode() == Opcodes::RETURN ||
                last->getOpcode() == Opcodes::ARETURN || last->getOpcode() == Opcodes::LRETURN ||
                last->getOpcode() == Opcodes::FRETURN || last->getOpcode() == Opcodes::DRETURN) {
                info.terminal = true;
            }
            else if (end >= start + 4 && nodes[end - 2]->getOpcode() == Opcodes::ISTORE &&
                     nodes[end - 1]->getOpcode() == Opcodes::GOTO &&
                     isIntInsn(nodes[end - 3], info.nextState)) {
                auto* store = ca::core::tag_cast::cast<VarInsnNode*>(nodes[end - 2]);
                auto* back  = ca::core::tag_cast::cast<JumpInsnNode*>(nodes[end - 1]);
                if (store->var != stateStore->var || back->label != dispatcher) {
                    invalid = true;
                    break;
                }
                info.back           = back;
                info.stateConst     = nodes[end - 3];
                info.stateStoreInsn = nodes[end - 2];
            }
            else {
                invalid = true;
                break;
            }
            for (int n = start + 1; n < end - (info.terminal ? 0 : 3); ++n) {
                auto* node = nodes[n];
                if (node->getType() == AbstractInsnNode::JUMP_INSN ||
                    node->getType() == AbstractInsnNode::TABLESWITCH_INSN ||
                    node->getType() == AbstractInsnNode::LOOKUPSWITCH_INSN ||
                    node->getType() == AbstractInsnNode::METHOD_INSN ||
                    node->getType() == AbstractInsnNode::FIELD_INSN ||
                    node->getType() == AbstractInsnNode::TYPE_INSN ||
                    node->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN ||
                    node->getType() == AbstractInsnNode::MULTIANEWARRAY_INSN ||
                    node->getOpcode() == Opcodes::ATHROW || node->getOpcode() == Opcodes::IDIV ||
                    node->getOpcode() == Opcodes::IREM ||
                    (node->getOpcode() >= Opcodes::IALOAD &&
                     node->getOpcode() <= Opcodes::SALOAD) ||
                    (node->getOpcode() >= Opcodes::IASTORE &&
                     node->getOpcode() <= Opcodes::SASTORE) ||
                    node->getOpcode() == Opcodes::ARRAYLENGTH ||
                    node->getOpcode() == Opcodes::MONITORENTER ||
                    node->getOpcode() == Opcodes::MONITOREXIT)
                    invalid = true;
                if (node->getOpcode() == Opcodes::ISTORE) invalid = true;
                if (node->getType() == AbstractInsnNode::VAR_INSN &&
                    node->getOpcode() == Opcodes::ILOAD &&
                    ca::core::tag_cast::cast<VarInsnNode*>(node)->var == stateStore->var)
                    invalid = true;
            }
            if (invalid) break;
            cases.emplace(label, info);
        }
        if (invalid) continue;
        auto defaultCase = cases.find(defaultTarget);
        if (defaultCase == cases.end() || !defaultCase->second.terminal) continue;

        std::unordered_set<Label*> visited;
        std::vector<CaseInfo*>     chain;
        int                        state = initialState;
        while (true) {
            Label* label = defaultTarget;
            auto   it    = dispatch.find(state);
            if (it != dispatch.end()) label = it->second;
            if (!visited.insert(label).second) {
                invalid = true;
                break;
            }
            auto caseIt = cases.find(label);
            if (caseIt == cases.end()) {
                invalid = true;
                break;
            }
            chain.push_back(&caseIt->second);
            if (caseIt->second.terminal) break;
            state = caseIt->second.nextState;
        }
        if (invalid || chain.empty()) continue;

        //Reject jumps into the recognized island from outside the dispatcher.
        std::unordered_set<Label*> caseLabels;
        for (const auto& [label, info] : cases) caseLabels.insert(label);
        for (auto* node : nodes) {
            if (node->getType() != AbstractInsnNode::JUMP_INSN) continue;
            auto* jump = ca::core::tag_cast::cast<JumpInsnNode*>(node);
            if (jump == initialGoto) continue;
            bool isBack = false;
            for (const auto& [label, info] : cases)
                if (jump == info.back) isBack = true;
            if ((jump->label == dispatcher && !isBack) ||
                (caseLabels.count(jump->label) && !isBack)) {
                invalid = true;
                break;
            }
        }
        if (invalid) continue;

        //The CFG is built only after validation, so unsupported input remains untouched.
        ast::cfg::ControlFlowGraph cfg;
        cfg.create(methodNode);

        std::unordered_set<AbstractInsnNode*> keep;
        std::vector<AbstractInsnNode*>        ordered;
        for (int n = 0; n < i; ++n) ordered.push_back(nodes[n]);
        for (auto* info : chain) {
            auto* block = cfg.findBlockByLabel(info->label);
            if (!block) {
                methodNode.instructions = cfg.toInsnList();
                //IR 已被改写却返回 all 会违反失效链契约：changed==false 使
                //写回门控跳过重序列化（改写静默丢失），且方法级分析缓存不失效，
                //后续 pass 可能命中指向旧节点的陈旧帧。与成功路径一致返回 none
                //（none 隐含 changed==true）。
                return PreservedAnalyses::none();
            }
            for (auto* node : block->instructions()) {
                if (node == info->stateConst || node == info->stateStoreInsn || node == info->back)
                    continue;
                ordered.push_back(node);
            }
        }
        int machineEnd = 0;
        for (const auto& [label, info] : cases) machineEnd = std::max(machineEnd, info.end);
        for (int n = machineEnd; n < static_cast<int>(nodes.size()); ++n)
            ordered.push_back(nodes[n]);
        for (auto* node : ordered) keep.insert(node);

        auto allNodes = cfg.toInsnList();
        for (auto* node = allNodes.getFirst(); node;) {
            auto* next = node->getNext();
            allNodes.remove(node);
            if (!keep.count(node) && !methodNode.arena().owns(node)) delete node;   //混合清除(B7)
            node = next;
        }
        for (auto* node : ordered) methodNode.instructions.add(node);
        return PreservedAnalyses::none();
    }
    return PreservedAnalyses::all();
}

}   //namespace mjt::base
