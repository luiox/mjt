#include "mbase/pass/opt/try_catch_cleanup.hpp"

#include "jbytecode/core/opcodes.hpp"
#include "mbase/analysis/method_frame_analysis.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "libca/core/tag_cast.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

//============================================================================
//try_catch_cleanup 私有实现
//家族共享的 label 定位 / try range 校验 / NPE 形状匹配 helper。原分散在
//control_flow_detail.hpp（indexOfLabelInInsns）、clean_pass_detail.hpp
//（isEmptyTryRange / findLabelNode）、structure_detail.hpp（NPE matchers），
//因家族聚合后单消费者，合回本 TU。
//============================================================================

namespace {

static int indexOfLabelInInsns(InsnList& insns, jbytecode::core::Label* label)
{
    int idx = 0;
    for (auto* node = insns.getFirst(); node; node = node->getNext(), ++idx) {
        if (node->getType() == AbstractInsnNode::LABEL) {
            auto* labelNode = ca::core::tag_cast::cast<LabelNode*>(node);
            if (labelNode->label == label) return idx;
        }
    }
    return -1;
}

//从指令列表中查找指定 Label 对应的 LabelNode
static LabelNode* findLabelNode(InsnList& insns, jbytecode::core::Label* label)
{
    for (auto* node = insns.getFirst(); node; node = node->getNext()) {
        if (node->getType() == AbstractInsnNode::LABEL) {
            auto* ln = ca::core::tag_cast::cast<LabelNode*>(node);
            if (ln->label == label) return ln;
        }
    }
    return nullptr;
}

//判断 start 和 end 标签之间是否有实际指令（无实际指令则 try-catch 范围为空）
static bool isEmptyTryRange(InsnList& insns, jbytecode::core::Label* startLabel,
                            jbytecode::core::Label* endLabel)
{
    bool foundStart = false;
    for (auto* node = insns.getFirst(); node; node = node->getNext()) {
        if (node->getType() == AbstractInsnNode::LABEL) {
            auto* ln = ca::core::tag_cast::cast<LabelNode*>(node);
            if (!foundStart && ln->label == startLabel) {
                foundStart = true;
                continue;
            }
            if (foundStart && ln->label == endLabel) {
                return true;   //到达 end 时未发现任何实际指令
            }
        }
        if (foundStart) {
            auto t = node->getType();
            if (t != AbstractInsnNode::LABEL && t != AbstractInsnNode::LINE &&
                t != AbstractInsnNode::FRAME) {
                return false;   //找到了实际指令
            }
        }
    }
    return false;
}

bool isSupportedNpeCatch(const TryCatchBlockNode& tryCatch)
{
    return tryCatch.type.is_empty() || tryCatch.type.equals("java/lang/NullPointerException");
}

bool matchNpeTryBody(LabelNode* startNode, LabelNode* endNode,
                     AbstractInsnNode*& nullInsn, AbstractInsnNode*& throwInsn)
{
    nullInsn = startNode ? startNode->getNext() : nullptr;
    throwInsn = nullInsn ? nullInsn->getNext() : nullptr;
    if (!nullInsn || !throwInsn || throwInsn->getNext() != endNode) return false;
    return nullInsn->getOpcode() == Opcodes::ACONST_NULL &&
           throwInsn->getOpcode() == Opcodes::ATHROW;
}

//============================================================================
//F9（Recaf 审计项移植，对应上游 27a75ca85 / c64716819 / 7aa4e25dc）共享 helper。
//两条新规则都挂在 RedundantTryCatchRemover 上（上游同属
//RedundantTryCatchRemovingTransformer），判据全部保守：宁可漏删不可误删——
//删错 catch 条目会改变异常语义，属正确性缺陷。
//============================================================================

///异常表条目在指令序列上的索引视图：指令数组、节点→下标、label→下标。
struct ExceptionTableIndex
{
    std::vector<AbstractInsnNode*>   insns;
    std::unordered_map<AbstractInsnNode*, int> indexOf;
    std::unordered_map<Label*, int>  labelIndex;
};

///指令是否为元数据节点（label / 行号 / stack map frame），不参与异常判定。
bool isMetadataNode(const AbstractInsnNode* node)
{
    const int t = node->getType();
    return t == AbstractInsnNode::LABEL || t == AbstractInsnNode::LINE ||
           t == AbstractInsnNode::FRAME;
}

///构建指令索引（在帧分析成功后调用，instructions 非空）。
void buildExceptionTableIndex(MethodNode& method, ExceptionTableIndex& index)
{
    index.insns   = method.instructions.toArray();
    const int size = static_cast<int>(index.insns.size());
    for (int i = 0; i < size; ++i) {
        auto* node = index.insns[static_cast<std::size_t>(i)];
        index.indexOf[node] = i;
        if (node->getType() == AbstractInsnNode::LABEL) {
            index.labelIndex[ca::core::tag_cast::cast<LabelNode*>(node)->label] = i;
        }
    }
}

///F9 规则2：按 opcode 类别判定「该指令可能抛出异常」的保守集合。
///对齐 Recaf AsmInsnUtil.canThrow（含 7aa4e25dc 计入的分配/类型解析类指令、
///c64716819 计入的 invokedynamic），并保守并入 FDIV 族（上游
///canInsnThrowAnyException 口径）。命中即视为可能抛出匹配异常，宁多勿漏。
bool insnCanThrowAny(AbstractInsnNode* insn)
{
    const int op = insn->getOpcode();
    if (op == Opcodes::ATHROW) return true;
    //方法调用（含 invokedynamic）一律视为可能抛出任意异常。
    if (insn->getType() == AbstractInsnNode::METHOD_INSN ||
        insn->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN)
        return true;
    //类型解析 / 分配类指令可在正常结果产生前抛出（NEW 触发类解析、<clinit> 失败等）。
    if (op == Opcodes::NEW || op == Opcodes::ANEWARRAY || op == Opcodes::NEWARRAY ||
        op == Opcodes::MULTIANEWARRAY || op == Opcodes::CHECKCAST ||
        op == Opcodes::INSTANCEOF)
        return true;
    //字段 / 数组访问的 NPE、AIOOBE；监视器指令的 IMSE；整数除法的 ArithmeticException。
    if (op == Opcodes::GETFIELD || op == Opcodes::PUTFIELD || op == Opcodes::ARRAYLENGTH)
        return true;
    //数组读写区间按上游口径整段计入（含普通局部 store，方向保守：多保留条目）。
    if (op >= Opcodes::IALOAD && op <= Opcodes::SASTORE) return true;
    if (op == Opcodes::MONITORENTER || op == Opcodes::MONITOREXIT) return true;
    if (op == Opcodes::IDIV || op == Opcodes::IREM || op == Opcodes::LDIV ||
        op == Opcodes::LREM || op == Opcodes::FDIV || op == Opcodes::FREM ||
        op == Opcodes::DDIV || op == Opcodes::DREM)
        return true;
    return false;
}

///普通控制流图（忽略异常派发边）：前驱表 + 从方法入口的可达集。
struct NormalFlowGraph
{
    ///每个下标的普通前驱（与 succ 边一一对应的反向边）。
    std::vector<std::vector<int>> preds;
    ///从下标 0 出发沿普通控制流可达的节点。
    std::vector<bool>             reachable;
};

///计算下标 i 的普通控制流后继。结构无法静态判定时返回 false（调用方放弃规则1）：
///JSR/RET 子例程、跳转目标缺失、非终结指令悬空在方法末尾。
bool normalSuccessors(const ExceptionTableIndex& index, int i, std::vector<int>& out)
{
    auto* node     = index.insns[static_cast<std::size_t>(i)];
    const int size = static_cast<int>(index.insns.size());

    if (isMetadataNode(node)) {
        if (i + 1 < size) out.push_back(i + 1);
        return true;
    }

    const int op = node->getOpcode();
    //JSR(168) / JSR_W(201) / RET(169)：子例程语义本图不建模，一律放弃。
    if (op == Opcodes::JSR || op == Opcodes::RET || op == 201) return false;

    auto pushLabel = [&](Label* label) {
        if (!label) return false;
        auto it = index.labelIndex.find(label);
        if (it == index.labelIndex.end()) return false;
        out.push_back(it->second);
        return true;
    };

    if (node->getType() == AbstractInsnNode::JUMP_INSN) {
        auto* jump = ca::core::tag_cast::cast<JumpInsnNode*>(node);
        //GOTO(167) / GOTO_W(200) 无条件转移；其余 JUMP_INSN 均为条件跳转（双后继）。
        if (op == Opcodes::GOTO || op == 200) return pushLabel(jump->label);
        if (!pushLabel(jump->label)) return false;
        if (i + 1 < size) out.push_back(i + 1);
        return true;
    }

    if (op == Opcodes::TABLESWITCH) {
        auto* sw = ca::core::tag_cast::cast<TableSwitchInsnNode*>(node);
        if (!pushLabel(sw->dflt)) return false;
        for (auto* target : sw->labels)
            if (!pushLabel(target)) return false;
        return true;
    }
    if (op == Opcodes::LOOKUPSWITCH) {
        auto* sw = ca::core::tag_cast::cast<LookupSwitchInsnNode*>(node);
        if (!pushLabel(sw->dflt)) return false;
        for (auto* target : sw->labels)
            if (!pushLabel(target)) return false;
        return true;
    }

    //终结指令：athrow 与各类 return 没有普通后继。
    if (op == Opcodes::ATHROW || op == Opcodes::IRETURN || op == Opcodes::LRETURN ||
        op == Opcodes::FRETURN || op == Opcodes::DRETURN || op == Opcodes::ARETURN ||
        op == Opcodes::RETURN)
        return true;

    //顺序指令顺序前进；非终结指令不允许悬空在方法末尾（顺序流无处可去，结构可疑）。
    if (i + 1 >= size) return false;
    out.push_back(i + 1);
    return true;
}

///从方法入口（下标 0）做普通控制流可达性 BFS。结构不支持时返回 false。
bool buildNormalFlowGraph(const ExceptionTableIndex& index, NormalFlowGraph& graph)
{
    const int size = static_cast<int>(index.insns.size());
    if (size <= 0) return false;

    //GCC 对 vector(n, {}) 报歧义重载（initializer_list vs 序列构造），显式给出元素类型。
    std::vector<std::vector<int>> succs(static_cast<std::size_t>(size), std::vector<int>{});
    graph.preds.assign(static_cast<std::size_t>(size), std::vector<int>{});
    for (int i = 0; i < size; ++i) {
        std::vector<int> out;
        if (!normalSuccessors(index, i, out)) return false;
        succs[static_cast<std::size_t>(i)] = out;
        for (int s : out) graph.preds[static_cast<std::size_t>(s)].push_back(i);
    }

    graph.reachable.assign(static_cast<std::size_t>(size), false);
    graph.reachable[0] = true;
    std::vector<int> queue;
    queue.push_back(0);
    for (std::size_t head = 0; head < queue.size(); ++head) {
        for (int s : succs[static_cast<std::size_t>(queue[head])]) {
            if (!graph.reachable[static_cast<std::size_t>(s)]) {
                graph.reachable[static_cast<std::size_t>(s)] = true;
                queue.push_back(s);
            }
        }
    }
    return true;
}

///F9 规则1（对齐 Recaf 27a75ca85 isTransparentRethrowHandler）：
///catch 体只把捕获异常原样 athrow——直接 `athrow`，或 `astore X; aload X; athrow`
///同槽恒等传递——且该 handler 仅能经本异常表项进入（普通控制流不可达、体内无来自
///体外可达代码的普通流入、handler 不被其他条目共享）时，删除该条目不改变可观察行为
///（athrow 重抛原异常对象不改写栈回溯，捕获后立即重抛与不捕获逐位等价）。
///
///本仓相对上游追加的保守条件：任何其他异常表项与候选保护区间重叠、其保护区间覆盖
///handler 或 handler 体内任一节点（即存在其他异常边进入 handler 体）时一律不删。
///上游缺失该检查：重叠区间按异常表顺序派发时，删除靠前条目会让原本被重抛透传的
///异常改落入靠后条目的 handler，属行为改变。
bool isTransparentRethrowRemoval(const TryCatchBlockNode&                 candidate,
                                 const std::vector<TryCatchBlockNode*>&   allEntries,
                                 const ExceptionTableIndex&               index,
                                 const NormalFlowGraph&                   graph)
{
    if (!candidate.start || !candidate.end || !candidate.handler) return false;

    auto labelIdx = [&](Label* label) -> int {
        auto it = index.labelIndex.find(label);
        return it == index.labelIndex.end() ? -1 : it->second;
    };
    const int startIdx   = labelIdx(candidate.start);
    const int endIdx     = labelIdx(candidate.end);
    const int handlerIdx = labelIdx(candidate.handler);
    //标签无法定位或范围退化：交由 TryCatchRepair / 既有退化条目规则处理，本规则不碰。
    if (startIdx < 0 || endIdx < 0 || handlerIdx < 0) return false;
    if (startIdx >= endIdx) return false;
    if (handlerIdx >= startIdx && handlerIdx < endIdx) return false;

    //handler 被多条异常表项共享时可能是共享恢复代码，单边冗余判定不成立。
    int sharedCount = 0;
    for (auto* block : allEntries)
        if (block && block->handler == candidate.handler) ++sharedCount;
    if (sharedCount != 1) return false;

    //handler 可由普通控制流到达，或存在来自可达代码的普通流入：是真实应用代码，不动。
    if (graph.reachable[static_cast<std::size_t>(handlerIdx)]) return false;
    for (int pred : graph.preds[static_cast<std::size_t>(handlerIdx)])
        if (graph.reachable[static_cast<std::size_t>(pred)]) return false;

    //收集 handler 体直到终结 athrow（攒满 3 条真实指令仍未见到 athrow 即非透明形状）。
    std::vector<int>               bodyIndices;
    std::vector<AbstractInsnNode*> realInsns;
    for (auto* cur = index.insns[static_cast<std::size_t>(handlerIdx)]; cur; cur = cur->getNext()) {
        auto it = index.indexOf.find(cur);
        if (it == index.indexOf.end()) return false;
        bodyIndices.push_back(it->second);
        if (isMetadataNode(cur)) continue;
        realInsns.push_back(cur);
        if (cur->getOpcode() == Opcodes::ATHROW || realInsns.size() >= 3) break;
    }

    //仅两种透明形状：直接 athrow / 恒等 astore X; aload X; athrow（同一局部槽）。
    const bool direct = realInsns.size() == 1 && realInsns[0]->getOpcode() == Opcodes::ATHROW;
    bool       identity = false;
    if (realInsns.size() == 3 && realInsns[0]->getOpcode() == Opcodes::ASTORE &&
        realInsns[1]->getOpcode() == Opcodes::ALOAD &&
        realInsns[2]->getOpcode() == Opcodes::ATHROW) {
        identity = ca::core::tag_cast::cast<VarInsnNode*>(realInsns[0])->var ==
                   ca::core::tag_cast::cast<VarInsnNode*>(realInsns[1])->var;
    }
    if (!direct && !identity) return false;

    //handler 体内任一节点若可由体外可达代码普通流入，即为真实控制流，不动。
    std::unordered_set<int> bodySet(bodyIndices.begin(), bodyIndices.end());
    for (int bodyIndex : bodyIndices) {
        if (graph.reachable[static_cast<std::size_t>(bodyIndex)]) return false;
        for (int pred : graph.preds[static_cast<std::size_t>(bodyIndex)])
            if (graph.reachable[static_cast<std::size_t>(pred)] && bodySet.count(pred) == 0)
                return false;
    }

    //保守追加（上游缺失）：其他条目与候选区间重叠、或其保护区覆盖 handler / handler
    //体（存在其他异常边进入 handler 体）时，handler 体的死活受其他条目影响，不删。
    for (auto* other : allEntries) {
        if (!other || other == &candidate) continue;
        if (!other->start || !other->end) return false;
        const int os = labelIdx(other->start);
        const int oe = labelIdx(other->end);
        if (os < 0 || oe < 0) return false;
        if (os >= oe) return false;   //无法确认的退化区间按「可能重叠」保守处理
        const bool overlapsRange = os < endIdx && startIdx < oe;
        const bool coversHandler = os <= handlerIdx && handlerIdx < oe;
        bool       coversBody    = false;
        for (int bodyIndex : bodyIndices)
            if (os <= bodyIndex && bodyIndex < oe) { coversBody = true; break; }
        if (overlapsRange || coversHandler || coversBody) return false;
    }
    return true;
}

///F9 规则2（对齐 Recaf c64716819 + 7aa4e25dc 的 canThrow 思路）：
///保护区间内不存在任何「可能抛出异常」的可达指令时，该条目在运行期永不可能派发，可删。
///
///可达性取自帧分析结论：帧为空的指令不可执行（跳过）；帧非空的指令一律视为可能执行，
///且只要落在 insnCanThrowAny 集合内就视为可能抛出。本仓无类型格，不做上游
///canInsnThrowCaughtException 的按类型细化（如 GETFIELD 只可能抛 NPE），对带类型的
///catch 更保守：区间内没有任何可能抛出「任意」异常的指令才删。
bool isImpossibleCatchRemoval(const TryCatchBlockNode&                            candidate,
                              const ExceptionTableIndex&                          index,
                              const std::vector<OriginalSourceFrame*>&            frames)
{
    if (!candidate.start || !candidate.end || !candidate.handler) return false;

    auto labelIdx = [&](Label* label) -> int {
        auto it = index.labelIndex.find(label);
        return it == index.labelIndex.end() ? -1 : it->second;
    };
    const int startIdx   = labelIdx(candidate.start);
    const int endIdx     = labelIdx(candidate.end);
    const int handlerIdx = labelIdx(candidate.handler);
    if (startIdx < 0 || endIdx < 0 || handlerIdx < 0) return false;
    if (startIdx >= endIdx) return false;
    if (handlerIdx >= startIdx && handlerIdx < endIdx) return false;

    if (frames.size() != index.insns.size()) return false;
    for (int i = startIdx; i < endIdx; ++i) {
        auto* insn = index.insns[static_cast<std::size_t>(i)];
        if (isMetadataNode(insn)) continue;
        //帧为空 = 分析证明该指令不可达，不会执行、不会抛出。
        if (frames[static_cast<std::size_t>(i)] == nullptr) continue;
        if (insnCanThrowAny(insn)) return false;
    }
    return true;
}

bool matchNpeHandler(LabelNode* handlerNode, const std::unordered_set<Label*>& knownLabels,
                     Label*& continuation)
{
    auto* pop = handlerNode ? handlerNode->getNext() : nullptr;
    if (!pop || pop->getOpcode() != Opcodes::POP) return false;

    auto* next = pop->getNext();
    if (!next) return false;
    if (next->getType() == AbstractInsnNode::LABEL) {
        continuation = ca::core::tag_cast::cast<LabelNode*>(next)->label;
        return continuation != handlerNode->label;
    }
    if (next->getOpcode() != Opcodes::GOTO) return false;

    auto* jump = ca::core::tag_cast::cast<JumpInsnNode*>(next);
    if (!jump->label || knownLabels.count(jump->label) == 0) return false;
    auto* boundary = next->getNext();
    if (!boundary || boundary->getType() != AbstractInsnNode::LABEL) return false;
    continuation = jump->label;
    return continuation != handlerNode->label;
}

}   //namespace

//============================================================================
//RedundantTryCatchRemover
//对应 Java controlflow/RedundantTryCatchRemover.java，F9 扩展对齐 Recaf
//RedundantTryCatchRemovingTransformer（27a75ca85 / c64716819 / 7aa4e25dc）。
//三条判据：退化条目删除；透明重抛 handler 条目删除；区间内无可能抛出匹配异常
//指令的条目删除。全部只动异常表，不改写指令流。
//============================================================================

PreservedAnalyses RedundantTryCatchRemover::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    if (methodNode.tryCatchBlocks.empty()) return PreservedAnalyses::all();

    bool modified = false;

    //规则A：start==end && handler==end 的退化异常表项。
    {
        auto it = std::remove_if(
            methodNode.tryCatchBlocks.begin(),
            methodNode.tryCatchBlocks.end(),
            [](TryCatchBlockNode* tcb) {
                return tcb && tcb->start == tcb->end && tcb->handler == tcb->end;
            });
        if (it != methodNode.tryCatchBlocks.end()) {
            for (auto iter = it; iter != methodNode.tryCatchBlocks.end(); ++iter)
                if (!methodNode.arena().owns(*iter)) delete *iter;   //混合清除(B7)
            methodNode.tryCatchBlocks.erase(it, methodNode.tryCatchBlocks.end());
            modified = true;
        }
    }

    if (methodNode.tryCatchBlocks.empty())
        return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();

    //无任何有效条目时新规则无事可做（也避免在未注册分析的裸 AM 上触发契约错误）。
    const bool hasValidEntry = std::any_of(methodNode.tryCatchBlocks.begin(),
                                           methodNode.tryCatchBlocks.end(),
                                           [](const TryCatchBlockNode* tcb) { return tcb != nullptr; });
    if (!hasValidEntry)
        return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();

    //F9 共同前置：帧分析失败（异常 / 超限 / 帧数不齐）一律放弃新规则，保守回退。
    //只读分析不产生副作用，无结果也不污染后续 pass。
    const auto& frameResult = am.getResult<OriginalSourceFrameAnalysis>(methodNode);
    if (!frameResult.ok)
        return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();

    ExceptionTableIndex index;
    buildExceptionTableIndex(methodNode, index);
    if (frameResult.frames.size() != index.insns.size())
        return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();

    //规则1 需要普通控制流可达性；JSR/RET 等结构不支持时跳过规则1（规则2 不依赖 CFG）。
    NormalFlowGraph graph;
    const bool flowOk = buildNormalFlowGraph(index, graph);

    auto&                           entries = methodNode.tryCatchBlocks;
    std::vector<TryCatchBlockNode*> toRemove;
    for (auto* tcb : entries) {
        if (!tcb) continue;
        //规则1：透明重抛 handler；结构不支持时跳过。
        if (flowOk && isTransparentRethrowRemoval(*tcb, entries, index, graph)) {
            toRemove.push_back(tcb);
            continue;
        }
        //规则2：保护区间内不存在可能抛出匹配异常的指令。
        if (isImpossibleCatchRemoval(*tcb, index, frameResult.frames)) {
            if (std::find(toRemove.begin(), toRemove.end(), tcb) == toRemove.end())
                toRemove.push_back(tcb);
        }
    }

    if (toRemove.empty())
        return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();

    for (auto* tcb : toRemove) {
        entries.erase(std::remove(entries.begin(), entries.end(), tcb), entries.end());
        if (!methodNode.arena().owns(tcb)) delete tcb;   //混合清除(B7)
    }
    return PreservedAnalyses::none();
}

//============================================================================
//TryCatchRepair
//对应 Java controlflow/TryCatchRepair.java。
//按 label 顺序清理非法异常表项（重叠/缺失/顺序错误）和 stale throws。
//============================================================================

PreservedAnalyses TryCatchRepair::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    if (std::any_of(methodNode.tryCatchBlocks.begin(),
                    methodNode.tryCatchBlocks.end(),
                    [](const TryCatchBlockNode* tcb) { return tcb == nullptr; }))
        return PreservedAnalyses::all();

    bool modified = false;

    //移除无效的 try-catch 块
    auto it = std::remove_if(
        methodNode.tryCatchBlocks.begin(),
        methodNode.tryCatchBlocks.end(),
        [&](TryCatchBlockNode* tcb) {
            //标签重叠
            if (tcb->start == tcb->end || tcb->start == tcb->handler || tcb->end == tcb->handler) {
                modified = true;
                return true;
            }
            const int startIdx   = indexOfLabelInInsns(methodNode.instructions, tcb->start);
            const int handlerIdx = indexOfLabelInInsns(methodNode.instructions, tcb->handler);
            const int endIdx     = indexOfLabelInInsns(methodNode.instructions, tcb->end);
            //标签不在指令序列中
            if (startIdx == -1 || handlerIdx == -1 || endIdx == -1) {
                modified = true;
                return true;
            }
            //标签顺序错误
            if (startIdx >= handlerIdx || startIdx >= endIdx || handlerIdx <= endIdx) {
                modified = true;
                return true;
            }
            return false;
        });

    for (auto iter = it; iter != methodNode.tryCatchBlocks.end(); ++iter)
        if (!methodNode.arena().owns(*iter)) delete *iter;   //混合清除(B7)
    if (it != methodNode.tryCatchBlocks.end()) {
        methodNode.tryCatchBlocks.erase(it, methodNode.tryCatchBlocks.end());
    }

    //移除不被任何 try-catch 捕获的异常类型
    auto eit = std::remove_if(methodNode.exceptions.begin(),
                              methodNode.exceptions.end(),
                              [&](const ca::str::Utf8StringRef& exception) {
                                  for (auto* tcb : methodNode.tryCatchBlocks) {
                                      if (!tcb->type.is_empty() && tcb->type == exception)
                                          return false;
                                  }
                                  return true;
                              });
    if (eit != methodNode.exceptions.end()) {
        methodNode.exceptions.erase(eit, methodNode.exceptions.end());
        modified = true;
    }

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//UselessTryCatchRemover
//对应 Java controlflow/UselessTryCatchRemover.java。
//消除仅抛 NPE（aconst_null;athrow）+ handler 仅吞异常（pop[;goto]）的 try-catch。
//============================================================================

PreservedAnalyses UselessTryCatchRemover::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    if (methodNode.tryCatchBlocks.empty()) return PreservedAnalyses::all();

    //建 Label* -> LabelNode* 映射（C++ 的 TryCatchBlockNode 是 Label*）
    std::unordered_map<Label*, LabelNode*> labelToNode;
    for (auto* insn : methodNode.instructions) {
        if (insn->getType() == AbstractInsnNode::LABEL) {
            labelToNode[ca::core::tag_cast::cast<LabelNode*>(insn)->label] = ca::core::tag_cast::cast<LabelNode*>(insn);
        }
    }

    struct Rewrite {
        TryCatchBlockNode* tryCatch;
        AbstractInsnNode*  nullInsn;
        AbstractInsnNode*  throwInsn;
        Label*             continuation;
    };
    std::unordered_map<AbstractInsnNode*, int> instructionIndex;
    auto                                      instructions = methodNode.instructions.toArray();
    for (int i = 0; i < static_cast<int>(instructions.size()); ++i) {
        instructionIndex[instructions[static_cast<std::size_t>(i)]] = i;
    }

    std::unordered_set<Label*> knownLabels;
    for (const auto& entry : labelToNode) knownLabels.insert(entry.first);
    std::vector<Rewrite> rewrites;

    for (auto* tryCatch : methodNode.tryCatchBlocks) {
        if (!tryCatch || !isSupportedNpeCatch(*tryCatch)) continue;
        auto itStart   = labelToNode.find(tryCatch->start);
        auto itEnd     = labelToNode.find(tryCatch->end);
        auto itHandler = labelToNode.find(tryCatch->handler);
        if (itStart == labelToNode.end() || itEnd == labelToNode.end() ||
            itHandler == labelToNode.end())
            continue;
        LabelNode* startNode   = itStart->second;
        LabelNode* endNode     = itEnd->second;
        LabelNode* handlerNode = itHandler->second;
        const int  startIndex  = instructionIndex[startNode];
        const int  endIndex    = instructionIndex[endNode];
        const int  handlerIndex = instructionIndex[handlerNode];
        if (startIndex >= endIndex || (handlerIndex >= startIndex && handlerIndex < endIndex)) continue;

        AbstractInsnNode* nullInsn  = nullptr;
        AbstractInsnNode* throwInsn = nullptr;
        if (!matchNpeTryBody(startNode, endNode, nullInsn, throwInsn)) continue;
        Label* continuation = nullptr;
        if (!matchNpeHandler(handlerNode, knownLabels, continuation)) continue;

        bool overlapsAnotherRange = false;
        for (auto* other : methodNode.tryCatchBlocks) {
            if (!other || other == tryCatch) continue;
            auto otherStart = labelToNode.find(other->start);
            auto otherEnd   = labelToNode.find(other->end);
            if (otherStart == labelToNode.end() || otherEnd == labelToNode.end()) continue;
            const int otherStartIndex = instructionIndex[otherStart->second];
            const int otherEndIndex   = instructionIndex[otherEnd->second];
            if (otherStartIndex >= otherEndIndex) {
                overlapsAnotherRange = true;
                break;
            }
            if (otherStartIndex < endIndex && startIndex < otherEndIndex) {
                overlapsAnotherRange = true;
                break;
            }
        }
        if (overlapsAnotherRange) continue;

        rewrites.push_back({tryCatch, nullInsn, throwInsn, continuation});
    }

    if (rewrites.empty()) return PreservedAnalyses::all();

    //先移除异常表项，再把必然抛 NPE 的两条指令改写为等价的 continuation 跳转。
    for (const auto& rewrite : rewrites) {
        methodNode.tryCatchBlocks.erase(
            std::remove(methodNode.tryCatchBlocks.begin(), methodNode.tryCatchBlocks.end(),
                        rewrite.tryCatch),
            methodNode.tryCatchBlocks.end());
        //混合清除:arena 节点交由 arena 回收(B7)
        if (!methodNode.arena().owns(rewrite.tryCatch)) delete rewrite.tryCatch;
        methodNode.instructions.set(rewrite.nullInsn,
                                    methodNode.create<JumpInsnNode>(Opcodes::GOTO, rewrite.continuation));
        methodNode.instructions.removeAndDelete(rewrite.throwInsn);
    }
    return PreservedAnalyses::none();
}

//============================================================================
//InvalidTryCatchRemover
//对应 Java controlflow/InvalidTryCatchRemover.java。
//删除空保护范围或 handler 直接 athrow 的异常表项。
//============================================================================

PreservedAnalyses InvalidTryCatchRemover::run(jbytecode::tree::MethodNode& methodNode,
                                              MethodAnalysisManager&       context)
{
    (void)context;
    auto& tryCatchBlocks = methodNode.tryCatchBlocks;
    if (tryCatchBlocks.empty()) return PreservedAnalyses::all();

    auto& insns    = methodNode.instructions;
    bool  modified = false;
    auto  it       = tryCatchBlocks.begin();
    while (it != tryCatchBlocks.end()) {
        auto* block        = *it;
        bool  shouldRemove = false;

        //条件1：start 和 end 之间没有实际指令
        if (isEmptyTryRange(insns, block->start, block->end)) {
            shouldRemove = true;
        }

        //条件2：handler 第一条指令是 athrow
        if (!shouldRemove && block->handler) {
            auto* handlerLabelNode = findLabelNode(insns, block->handler);
            if (handlerLabelNode) {
                auto* next = handlerLabelNode->getNext();
                if (next && next->getOpcode() == Opcodes::ATHROW) {
                    shouldRemove = true;
                }
            }
        }

        if (shouldRemove) {
            if (!methodNode.arena().owns(block)) delete block;   //混合清除(B7)
            it       = tryCatchBlocks.erase(it);
            modified = true;
        }
        else {
            ++it;
        }
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//SelfCaughtThrowRewriter（issue ）
//try 体恰为 new E;dup;invokespecial E.<init>V;athrow 且被自身 handler 接住时，把 athrow
//改写为 goto handler（栈顶 E 与异常派发进入 handler 的栈形状一致），并删除异常表项。
//============================================================================

namespace {

//从 startNode 之后、endNode 之前收集真实指令（跳过 LINE/FRAME）。区间内出现 LABEL 视为
//「有其他控制流」，返回 false（sawLabel=true）表示不符合「无额外代码」的形状。
bool collectTryBody(LabelNode* startNode, LabelNode* endNode,
                    std::vector<AbstractInsnNode*>& body, bool& sawLabel)
{
    sawLabel = false;
    for (auto* n = startNode->getNext(); n && n != endNode; n = n->getNext()) {
        const int t = n->getType();
        if (t == AbstractInsnNode::LINE || t == AbstractInsnNode::FRAME) continue;
        if (t == AbstractInsnNode::LABEL) {
            sawLabel = true;
            return false;
        }
        body.push_back(n);
    }
    return true;
}

//精确匹配 new E; dup; invokespecial E.<init>V; athrow，返回异常内部名 E 与 athrow 节点。
bool matchThrowNewBody(const std::vector<AbstractInsnNode*>& body,
                       ca::str::Utf8StringRef& newType, AbstractInsnNode*& athrowInsn)
{
    if (body.size() != 4) return false;
    if (body[0]->getType() != AbstractInsnNode::TYPE_INSN ||
        body[0]->getOpcode() != Opcodes::NEW)
        return false;
    newType = ca::core::tag_cast::cast<TypeInsnNode*>(body[0])->desc;
    if (body[1]->getOpcode() != Opcodes::DUP) return false;
    if (body[2]->getType() != AbstractInsnNode::METHOD_INSN ||
        body[2]->getOpcode() != Opcodes::INVOKESPECIAL)
        return false;
    auto* init = ca::core::tag_cast::cast<MethodInsnNode*>(body[2]);
    if (!init->owner.equals(newType) || !init->name.equals("<init>") || !init->desc.equals("()V"))
        return false;
    if (body[3]->getOpcode() != Opcodes::ATHROW) return false;
    athrowInsn = body[3];
    return true;
}

}   //namespace

PreservedAnalyses SelfCaughtThrowRewriter::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    if (methodNode.tryCatchBlocks.empty()) return PreservedAnalyses::all();

    //帧分析：验证 athrow 处操作数栈恰为 1（仅异常对象），fail-open。
    const auto& frameResult = am.getResult<OriginalSourceFrameAnalysis>(methodNode);
    if (!frameResult.ok) return PreservedAnalyses::all();
    const auto& frames       = frameResult.frames;
    auto        instructions = methodNode.instructions.toArray();
    if (frames.size() != instructions.size()) return PreservedAnalyses::all();

    std::unordered_map<Label*, LabelNode*>           labelToNode;
    std::unordered_map<const AbstractInsnNode*, int> indexOf;
    for (int i = 0; i < static_cast<int>(instructions.size()); ++i) {
        indexOf[instructions[static_cast<std::size_t>(i)]] = i;
        auto* insn                                         = instructions[static_cast<std::size_t>(i)];
        if (insn->getType() == AbstractInsnNode::LABEL) {
            labelToNode[ca::core::tag_cast::cast<LabelNode*>(insn)->label] = ca::core::tag_cast::cast<LabelNode*>(insn);
        }
    }

    struct Rewrite {
        TryCatchBlockNode* tryCatch;
        AbstractInsnNode*  athrow;
        Label*             handler;
    };
    std::vector<Rewrite> rewrites;

    for (auto* tryCatch : methodNode.tryCatchBlocks) {
        if (!tryCatch) continue;
        auto sIt = labelToNode.find(tryCatch->start);
        auto eIt = labelToNode.find(tryCatch->end);
        auto hIt = labelToNode.find(tryCatch->handler);
        if (sIt == labelToNode.end() || eIt == labelToNode.end() || hIt == labelToNode.end())
            continue;
        const int startIdx   = indexOf[sIt->second];
        const int endIdx     = indexOf[eIt->second];
        const int handlerIdx = indexOf[hIt->second];
        if (startIdx >= endIdx) continue;
        if (handlerIdx >= startIdx && handlerIdx < endIdx) continue;   //handler 落在保护区内

        std::vector<AbstractInsnNode*> body;
        bool                           sawLabel = false;
        if (!collectTryBody(sIt->second, eIt->second, body, sawLabel) || sawLabel) continue;

        ca::str::Utf8StringRef newType;
        AbstractInsnNode*      athrow = nullptr;
        if (!matchThrowNewBody(body, newType, athrow)) continue;

        //catch 类型：catch-all 或恰为 new 出的类型。
        if (!(tryCatch->type.is_empty() || tryCatch->type.equals(newType))) continue;

        //athrow 处栈深必须恰为 1（仅异常对象），否则 goto 无法复刻异常派发对栈的清空。
        auto* frame = frames[static_cast<std::size_t>(indexOf[athrow])];
        if (!frame || frame->getStackSize() != 1) continue;

        //与其他 try-catch 区间重叠时保守跳过。
        bool overlaps = false;
        for (auto* other : methodNode.tryCatchBlocks) {
            if (!other || other == tryCatch) continue;
            auto oS = labelToNode.find(other->start);
            auto oE = labelToNode.find(other->end);
            if (oS == labelToNode.end() || oE == labelToNode.end()) continue;
            const int os = indexOf[oS->second];
            const int oe = indexOf[oE->second];
            if (os >= oe) {
                overlaps = true;
                break;
            }
            if (os < endIdx && startIdx < oe) {
                overlaps = true;
                break;
            }
        }
        if (overlaps) continue;

        rewrites.push_back({tryCatch, athrow, tryCatch->handler});
    }

    if (rewrites.empty()) return PreservedAnalyses::all();

    for (const auto& rewrite : rewrites) {
        methodNode.tryCatchBlocks.erase(
            std::remove(methodNode.tryCatchBlocks.begin(), methodNode.tryCatchBlocks.end(),
                        rewrite.tryCatch),
            methodNode.tryCatchBlocks.end());
        //混合清除:arena 节点交由 arena 回收(B7)
        if (!methodNode.arena().owns(rewrite.tryCatch)) delete rewrite.tryCatch;
        //set 会释放旧的 athrow 节点，勿再 delete。
        methodNode.instructions.set(rewrite.athrow,
                                    methodNode.create<JumpInsnNode>(Opcodes::GOTO, rewrite.handler));
    }
    return PreservedAnalyses::none();
}

}   //namespace mjt::base
