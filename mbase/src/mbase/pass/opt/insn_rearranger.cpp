#include "mbase/pass/opt/insn_rearranger.hpp"

#include "mbase/asm/insn/insn_util.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "libca/core/tag_cast.hpp"
#include <memory>
#include <unordered_map>
#include <vector>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

//============================================================================
//insn_rearranger 私有实现
//两个 pass 共享的连续指令块扫描 helper。原位于 dead_code_remover.cpp 匿名命名空间，
//因这两个 pass 不是死代码清理（是指令重排），现按家族聚合到本 TU。
//============================================================================

namespace {

void addCloned(InsnList& target, AbstractInsnNode* source, garena::Arena& arena,
               std::unordered_map<Label*, Label*>& clonedLabels)
{
    //B8 新 clone 协议：产物在 arena 上分配（消费方 run 传入目标方法的 arena）。
    target.add(source->clone(arena, clonedLabels));
}

//── CommutativeOperatorReorder 专用 helper ─────────────────────────────

enum class OperandKind
{
    Int,
    Long
};

struct Chain
{
    int                            startInclusive = 0;
    int                            endExclusive   = 0;
    std::vector<AbstractInsnNode*> operands;
    std::vector<AbstractInsnNode*> ops;
    OperandKind                    kind = OperandKind::Int;
};

bool isInstruction(AbstractInsnNode* insn)
{
    return insn && insn->getOpcode() >= 0;
}

bool isIntConstant(AbstractInsnNode* insn)
{
    int value = 0;
    return InsnUtil::getIntValue(insn, value);
}

bool isLongConstant(AbstractInsnNode* insn)
{
    ca::i64 value = 0;
    return InsnUtil::getLongValue(insn, value);
}

bool isTargetCommutativeOp(int opcode)
{
    switch (opcode) {
    case Opcodes::IADD:
    case Opcodes::IMUL:
    case Opcodes::IXOR:
    case Opcodes::IOR:
    case Opcodes::IAND:
    case Opcodes::LADD:
    case Opcodes::LMUL:
    case Opcodes::LXOR:
    case Opcodes::LOR:
    case Opcodes::LAND: return true;
    default: return false;
    }
}

OperandKind opKind(int opcode)
{
    switch (opcode) {
    case Opcodes::LADD:
    case Opcodes::LMUL:
    case Opcodes::LXOR:
    case Opcodes::LOR:
    case Opcodes::LAND: return OperandKind::Long;
    default: return OperandKind::Int;
    }
}

bool isIntOperand(AbstractInsnNode* insn)
{
    if (isIntConstant(insn)) return true;
    if (insn->getOpcode() == Opcodes::ILOAD) return true;
    return false;
}

bool isLongOperand(AbstractInsnNode* insn)
{
    if (isLongConstant(insn)) return true;
    if (insn->getOpcode() == Opcodes::LLOAD) return true;
    return false;
}

bool isSupportedOperandProducer(AbstractInsnNode* insn, OperandKind kind)
{
    if (!isInstruction(insn)) return false;
    return kind == OperandKind::Int ? isIntOperand(insn) : isLongOperand(insn);
}

bool isConstantForKind(AbstractInsnNode* insn, OperandKind kind)
{
    return kind == OperandKind::Int ? isIntConstant(insn) : isLongConstant(insn);
}

bool tryParseChain(const std::vector<AbstractInsnNode*>& instructions, int start, Chain& out)
{
    if (start >= static_cast<int>(instructions.size())) return false;

    int firstOpIndex = -1;
    for (int i = start; i < static_cast<int>(instructions.size()); ++i) {
        if (isTargetCommutativeOp(instructions[i]->getOpcode())) {
            firstOpIndex = i;
            break;
        }
        if (!isInstruction(instructions[i])) {
            return false;
        }
    }

    if (firstOpIndex < 0 || firstOpIndex == start) return false;

    const int  opcode = instructions[firstOpIndex]->getOpcode();
    const auto kind   = opKind(opcode);

    std::vector<AbstractInsnNode*> operands;
    for (int i = start; i < firstOpIndex; ++i) {
        if (!isSupportedOperandProducer(instructions[i], kind)) return false;
        operands.push_back(instructions[i]);
    }

    int                            cursor = firstOpIndex;
    std::vector<AbstractInsnNode*> ops;
    while (cursor < static_cast<int>(instructions.size()) &&
           instructions[cursor]->getOpcode() == opcode) {
        ops.push_back(instructions[cursor]);
        ++cursor;
    }

    if (ops.empty() || operands.size() != ops.size() + 1) return false;

    out.startInclusive = start;
    out.endExclusive   = cursor;
    out.operands       = std::move(operands);
    out.ops            = std::move(ops);
    out.kind           = kind;
    return true;
}

std::vector<AbstractInsnNode*> reorderOperands(const Chain& chain)
{
    std::vector<AbstractInsnNode*> reordered;
    reordered.reserve(chain.operands.size());
    for (auto* operand : chain.operands) {
        if (!isConstantForKind(operand, chain.kind)) reordered.push_back(operand);
    }
    for (auto* operand : chain.operands) {
        if (isConstantForKind(operand, chain.kind)) reordered.push_back(operand);
    }
    return reordered;
}

bool isSameOrder(const std::vector<AbstractInsnNode*>& first,
                 const std::vector<AbstractInsnNode*>& second)
{
    if (first.size() != second.size()) return false;
    for (ca::usize i = 0; i < first.size(); ++i) {
        if (first[i] != second[i]) return false;
    }
    return true;
}

//── ArithmeticInsnRearranger 专用 helper（对齐 Java tryMatchChunk）──────
//tree IR 中 ILOAD_0 已规范化为 ILOAD。

struct ArithmeticAssignChunk
{
    int                            varSlot;
    int                            arithmeticOpcode;
    std::vector<AbstractInsnNode*> nodes;
    int                            length() const { return static_cast<int>(nodes.size()); }
};

static bool isIntReorderSafeOp(int opcode)
{
    return opcode == Opcodes::IADD || opcode == Opcodes::IAND || opcode == Opcodes::IMUL ||
           opcode == Opcodes::IOR || opcode == Opcodes::IXOR;
}
static bool isLongReorderSafeOp(int opcode)
{
    return opcode == Opcodes::LADD || opcode == Opcodes::LAND || opcode == Opcodes::LMUL ||
           opcode == Opcodes::LOR || opcode == Opcodes::LXOR;
}
static bool isValidPair(int loadOpcode, int storeOpcode, int arithmeticOpcode)
{
    if (loadOpcode == Opcodes::ILOAD && storeOpcode == Opcodes::ISTORE)
        return isIntReorderSafeOp(arithmeticOpcode);
    if (loadOpcode == Opcodes::LLOAD && storeOpcode == Opcodes::LSTORE)
        return isLongReorderSafeOp(arithmeticOpcode);
    return false;
}

static std::unique_ptr<ArithmeticAssignChunk> tryMatchChunk(
    const std::vector<AbstractInsnNode*>& instructions, int index)
{
    if (index + 3 >= static_cast<int>(instructions.size())) return nullptr;
    AbstractInsnNode* loadInsn       = instructions[index];
    AbstractInsnNode* valueInsn      = instructions[index + 1];
    AbstractInsnNode* arithmeticInsn = instructions[index + 2];
    AbstractInsnNode* storeInsn      = instructions[index + 3];

    if (loadInsn->getType() != AbstractInsnNode::VAR_INSN ||
        storeInsn->getType() != AbstractInsnNode::VAR_INSN)
        return nullptr;
    auto* loadVarInsn  = ca::core::tag_cast::cast<VarInsnNode*>(loadInsn);
    auto* storeVarInsn = ca::core::tag_cast::cast<VarInsnNode*>(storeInsn);
    if (loadVarInsn->var != storeVarInsn->var) return nullptr;
    //tree 中 var load/store 都是规范形（ILOAD..ALOAD / ISTORE..ASTORE）
    int lop = loadInsn->getOpcode();
    int sop = storeInsn->getOpcode();
    if (!(lop >= Opcodes::ILOAD && lop <= Opcodes::ALOAD)) return nullptr;
    if (!(sop >= Opcodes::ISTORE && sop <= Opcodes::ASTORE)) return nullptr;
    if (!InsnUtil::isConstant(valueInsn->getOpcode()))
        return nullptr;   //isNumber ≈ isConstant(数值)
    if (!isValidPair(lop, sop, arithmeticInsn->getOpcode())) return nullptr;

    auto chunk              = std::make_unique<ArithmeticAssignChunk>();
    chunk->varSlot          = loadVarInsn->var;
    chunk->arithmeticOpcode = arithmeticInsn->getOpcode();
    chunk->nodes            = {loadInsn, valueInsn, arithmeticInsn, storeInsn};
    return chunk;
}

}   //namespace

PreservedAnalyses ArithmeticInsnRearranger::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.size() < 8) return PreservedAnalyses::all();

    auto                               instructions = methodNode.instructions.toArray();
    //绑定方法 arena：!modified 早退时 result 带着 arena 克隆产物析构
    //（unbound 表会误 delete arena 节点）。
    InsnList                           result(&methodNode.arena());
    std::unordered_map<Label*, Label*> clonedLabels;

    int  index    = 0;
    bool modified = false;
    while (index < static_cast<int>(instructions.size())) {
        auto first = tryMatchChunk(instructions, index);
        if (!first) {
            addCloned(result, instructions[index], methodNode.arena(), clonedLabels);
            index++;
            continue;
        }
        std::vector<ArithmeticAssignChunk> window;
        window.push_back(*first);
        int cursor = index + first->length();
        while (cursor < static_cast<int>(instructions.size())) {
            auto next = tryMatchChunk(instructions, cursor);
            if (!next) break;
            window.push_back(*next);
            cursor += next->length();
        }
        if (window.size() < 2) {
            for (auto* n : window.front().nodes)
                addCloned(result, n, methodNode.arena(), clonedLabels);
            index += window.front().length();
            continue;
        }
        //rearrange：按 slot 首次出现顺序分组（对齐 Java LinkedHashMap）
        std::unordered_map<int, size_t>                  varOrder;
        std::vector<std::vector<ArithmeticAssignChunk*>> byVar;
        for (auto& chunk : window) {
            if (!varOrder.count(chunk.varSlot)) {
                varOrder[chunk.varSlot] = byVar.size();
                byVar.emplace_back();
            }
        }
        for (auto& chunk : window) {
            byVar[varOrder[chunk.varSlot]].push_back(&chunk);
        }
        //拼接（每个变量内部块顺序保持原序）
        std::vector<ArithmeticAssignChunk*> resultOrder;
        for (auto& varChunks : byVar) {
            for (auto* c : varChunks) resultOrder.push_back(c);
        }
        //isSameOrder：比较指针顺序
        bool same = (resultOrder.size() == window.size());
        for (size_t i = 0; same && i < resultOrder.size(); i++) {
            if (resultOrder[i] != &window[i]) same = false;
        }
        if (!same) modified = true;
        for (auto* c : resultOrder) {
            for (auto* n : c->nodes) addCloned(result, n, methodNode.arena(), clonedLabels);
        }
        index = cursor;
    }

    if (modified) {
        methodNode.instructions.clear();
        methodNode.instructions.add(result);
        return PreservedAnalyses::none();
    }
    return PreservedAnalyses::all();
}

PreservedAnalyses CommutativeOperatorReorder::run(MethodNode&            methodNode,
                                                   MethodAnalysisManager& context)
{
    (void)context;
    auto instructions = methodNode.instructions.toArray();
    if (instructions.size() < 3) return PreservedAnalyses::all();

    //绑定方法 arena：!modified 早退时 result 带着 arena 克隆产物析构
    //（unbound 表会误 delete arena 节点）。
    InsnList                           result(&methodNode.arena());
    std::unordered_map<Label*, Label*> clonedLabels;
    bool                               modified = false;

    int index = 0;
    while (index < static_cast<int>(instructions.size())) {
        Chain chain;
        if (!tryParseChain(instructions, index, chain)) {
            addCloned(result, instructions[index], methodNode.arena(), clonedLabels);
            ++index;
            continue;
        }

        auto reordered = reorderOperands(chain);
        modified       = modified || !isSameOrder(chain.operands, reordered);
        for (auto* operand : reordered) {
            addCloned(result, operand, methodNode.arena(), clonedLabels);
        }
        for (auto* op : chain.ops) {
            addCloned(result, op, methodNode.arena(), clonedLabels);
        }
        index = chain.endExclusive;
    }

    if (!modified) return PreservedAnalyses::all();

    methodNode.instructions.clear();
    methodNode.instructions.add(result);
    return PreservedAnalyses::none();
}

}   //namespace mjt::base
