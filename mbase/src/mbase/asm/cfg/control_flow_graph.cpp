#include "mbase/asm/cfg/control_flow_graph.hpp"

namespace mjt::base::ast::cfg {

void ControlFlowGraph::create(jbytecode::tree::MethodNode& methodNode)
{
    sourceMethod_ = &methodNode;
    preProcess(methodNode);
    buildSuccessorEdges();
    buildPredecessorEdges();
    buildExceptionEdges(methodNode);
}

BasicBlock* ControlFlowGraph::createBasicBlock()
{
    auto  block = std::make_unique<BasicBlock>(nextBlockId_++);
    auto* ptr   = block.get();
    basicBlocks_.push_back(std::move(block));
    return ptr;
}

BasicBlock* ControlFlowGraph::findBlockByLabel(jbytecode::core::Label* label) const
{
    auto it = labelToBlock_.find(label);
    return it != labelToBlock_.end() ? it->second : nullptr;
}

void ControlFlowGraph::preProcess(jbytecode::tree::MethodNode& methodNode)
{
    instructions_.clear();
    labelToBlock_.clear();
    basicBlocks_.clear();
    jumpEdges_.clear();
    exceptionEdges_.clear();
    nextBlockId_ = 0;
    syntheticLabels_.clear();

    auto makeSynLabel = [&]() -> jbytecode::tree::LabelNode* {
        //合成 Label 归源方法 arena（B8 标准建点）：LabelNode 转入方法表后原件随方法域
        //回收，不再堆泄漏（preProcess 仅由 create 调用，sourceMethod_ 必非空）。
        auto  syn = std::make_unique<jbytecode::tree::LabelNode>(sourceMethod_->createLabel());
        auto* ptr = syn.get();
        syntheticLabels_.push_back(std::move(syn));
        return ptr;
    };

    auto& origInsns = methodNode.instructions;

    //收集原始指令，转移所有权至 instructions_
    bool firstIsLabel = origInsns.getFirst() != nullptr &&
                        origInsns.getFirst()->getType() == jbytecode::tree::AbstractInsnNode::LABEL;
    if (!firstIsLabel) {
        instructions_.push_back(makeSynLabel());
    }

    //转移原始节点所有权（摘出所有节点，origInsns 变为空）
    for (auto* insn = origInsns.getFirst(); insn;) {
        auto* next = insn->getNext();
        origInsns.remove(insn);   //摘出但不 delete（InsnList::remove 只改链表指针）
        instructions_.push_back(insn);
        int32_t opcode = insn->getOpcode();

        bool isTerminating = (opcode >= jbytecode::core::Opcodes::IRETURN &&
                              opcode <= jbytecode::core::Opcodes::RETURN) ||
                             opcode == jbytecode::core::Opcodes::ATHROW ||
                             opcode == jbytecode::core::Opcodes::GOTO ||
                             (opcode >= jbytecode::core::Opcodes::IFEQ &&
                              opcode <= jbytecode::core::Opcodes::IF_ACMPNE) ||
                             opcode == jbytecode::core::Opcodes::IFNULL ||
                             opcode == jbytecode::core::Opcodes::IFNONNULL ||
                             opcode == jbytecode::core::Opcodes::JSR ||
                             opcode == jbytecode::core::Opcodes::TABLESWITCH ||
                             opcode == jbytecode::core::Opcodes::LOOKUPSWITCH;

        if (isTerminating && next) {
            if (next->getType() != jbytecode::tree::AbstractInsnNode::LABEL) {
                instructions_.push_back(makeSynLabel());
            }
        }
        insn = next;
    }

    //保证最后一个是 LabelNode（仅当最后一条不是 RETURN/ATHROW 时）
    if (!instructions_.empty()) {
        auto*   last     = instructions_.back();
        int32_t lastOp   = last->getOpcode();
        bool    isReturn = (lastOp >= jbytecode::core::Opcodes::IRETURN &&
                         lastOp <= jbytecode::core::Opcodes::RETURN) ||
                        lastOp == jbytecode::core::Opcodes::ATHROW;
        if (!isReturn && last->getType() != jbytecode::tree::AbstractInsnNode::LABEL) {
            instructions_.push_back(makeSynLabel());
        }
    }

    //切割 BasicBlock
    size_t pos = 0;
    while (pos < instructions_.size()) {
        auto* block = createBasicBlock();
        if (instructions_[pos]->getType() != jbytecode::tree::AbstractInsnNode::LABEL) {
            break;
        }
        auto* labelNode = static_cast<jbytecode::tree::LabelNode*>(instructions_[pos]);
        block->addInstruction(instructions_[pos]);
        pos++;
        while (pos < instructions_.size()) {
            auto* insn = instructions_[pos];
            if (insn->getType() == jbytecode::tree::AbstractInsnNode::LABEL) break;
            block->addInstruction(insn);
            pos++;
        }
        labelToBlock_[labelNode->label] = block;
    }
}

void ControlFlowGraph::buildSuccessorEdges()
{
    for (size_t i = 0; i + 1 < basicBlocks_.size(); i++) {
        auto* block     = basicBlocks_[i].get();
        auto* nextBlock = basicBlocks_[i + 1].get();
        auto* lastInsn  = block->lastInsn();
        if (!lastInsn) continue;

        int32_t opcode = lastInsn->getOpcode();

        if (opcode == jbytecode::core::Opcodes::GOTO &&
            lastInsn->getType() == jbytecode::tree::AbstractInsnNode::JUMP_INSN) {
            auto* jumpInsn = static_cast<jbytecode::tree::JumpInsnNode*>(lastInsn);
            auto* target   = findBlockByLabel(jumpInsn->label);
            if (target) {
                jumpEdges_.push_back(std::make_unique<UnconditionJumpEdge>(block, target));
                block->setJumpEdge(jumpEdges_.back().get());
            }
            continue;
        }

        bool isCondJump = (opcode >= jbytecode::core::Opcodes::IFEQ &&
                           opcode <= jbytecode::core::Opcodes::IF_ACMPNE) ||
                          opcode == jbytecode::core::Opcodes::IFNULL ||
                          opcode == jbytecode::core::Opcodes::IFNONNULL;
        if (isCondJump && lastInsn->getType() == jbytecode::tree::AbstractInsnNode::JUMP_INSN) {
            auto* jumpInsn   = static_cast<jbytecode::tree::JumpInsnNode*>(lastInsn);
            auto* trueTarget = findBlockByLabel(jumpInsn->label);
            if (trueTarget) {
                jumpEdges_.push_back(
                    std::make_unique<ConditionJumpEdge>(block, opcode, trueTarget, nextBlock));
                block->setJumpEdge(jumpEdges_.back().get());
            }
            continue;
        }

        if (opcode == jbytecode::core::Opcodes::TABLESWITCH &&
            lastInsn->getType() == jbytecode::tree::AbstractInsnNode::TABLESWITCH_INSN) {
            auto* tableSwitch = static_cast<jbytecode::tree::TableSwitchInsnNode*>(lastInsn);
            std::vector<BasicBlock*> targets;
            for (auto* label : tableSwitch->labels) {
                targets.push_back(findBlockByLabel(label));
            }
            auto* dflt = findBlockByLabel(tableSwitch->dflt);
            jumpEdges_.push_back(std::make_unique<TableSwitchJumpEdge>(
                block, tableSwitch->min, tableSwitch->max, std::move(targets), dflt));
            block->setJumpEdge(jumpEdges_.back().get());
            continue;
        }

        if (opcode == jbytecode::core::Opcodes::LOOKUPSWITCH &&
            lastInsn->getType() == jbytecode::tree::AbstractInsnNode::LOOKUPSWITCH_INSN) {
            auto* lookupSwitch = static_cast<jbytecode::tree::LookupSwitchInsnNode*>(lastInsn);
            std::map<int32_t, BasicBlock*> targets;
            for (size_t j = 0; j < lookupSwitch->keys.size() && j < lookupSwitch->labels.size();
                 j++) {
                targets[lookupSwitch->keys[j]] = findBlockByLabel(lookupSwitch->labels[j]);
            }
            auto* dflt = findBlockByLabel(lookupSwitch->dflt);
            jumpEdges_.push_back(
                std::make_unique<LookupSwitchJumpEdge>(block, std::move(targets), dflt));
            block->setJumpEdge(jumpEdges_.back().get());
            continue;
        }

        if (isTerminatedBlock(block)) {
            continue;
        }

        jumpEdges_.push_back(std::make_unique<FallThroughEdge>(block, nextBlock));
        block->setJumpEdge(jumpEdges_.back().get());
    }
}

void ControlFlowGraph::buildPredecessorEdges()
{
    for (auto& edge : jumpEdges_) {
        switch (edge->type()) {
        case JumpType::Unconditional:
        {
            auto* ue = static_cast<UnconditionJumpEdge*>(edge.get());
            ue->target()->addPrevJumpEdge(ue);
            break;
        }
        case JumpType::Condition:
        {
            auto* ce = static_cast<ConditionJumpEdge*>(edge.get());
            ce->trueTarget()->addPrevJumpEdge(ce);
            ce->falseTarget()->addPrevJumpEdge(ce);
            break;
        }
        case JumpType::TableSwitch:
        {
            auto* te = static_cast<TableSwitchJumpEdge*>(edge.get());
            if (te->defaultTarget()) te->defaultTarget()->addPrevJumpEdge(te);
            for (auto* t : te->targets()) {
                if (t) t->addPrevJumpEdge(te);
            }
            break;
        }
        case JumpType::LookupSwitch:
        {
            auto* le = static_cast<LookupSwitchJumpEdge*>(edge.get());
            if (le->defaultTarget()) le->defaultTarget()->addPrevJumpEdge(le);
            for (auto& [_, t] : le->targets()) {
                if (t) t->addPrevJumpEdge(le);
            }
            break;
        }
        case JumpType::FallThrough:
        {
            auto* fe = static_cast<FallThroughEdge*>(edge.get());
            fe->next()->addPrevJumpEdge(fe);
            break;
        }
        }
    }
}

void ControlFlowGraph::buildExceptionEdges(jbytecode::tree::MethodNode& methodNode)
{
    for (auto* tryCatch : methodNode.tryCatchBlocks) {
        auto* tryStart = findBlockByLabel(tryCatch->start);
        auto* tryEnd   = findBlockByLabel(tryCatch->end);
        auto* handler  = findBlockByLabel(tryCatch->handler);
        if (!tryStart || !tryEnd || !handler) continue;

        for (auto* insn = tryStart->firstInsn(); insn && insn != tryEnd->firstInsn();
             insn       = insn->getNext()) {
            if (insn->getType() == jbytecode::tree::AbstractInsnNode::LABEL) {
                auto* labelNode = static_cast<jbytecode::tree::LabelNode*>(insn);
                auto* block     = findBlockByLabel(labelNode->label);
                if (block) {
                    exceptionEdges_.push_back(std::make_unique<ExceptionEdge>(
                        block, tryCatch, tryStart, tryEnd, handler));
                    block->addExceptionEdge(exceptionEdges_.back().get());
                }
            }
        }
    }
}

jbytecode::tree::InsnList ControlFlowGraph::toInsnList()
{
    //绑定源方法 arena：arena 节点析构只摘链（随方法域回收），合成堆 LabelNode 照旧 delete
    jbytecode::tree::InsnList result(sourceMethod_ ? &sourceMethod_->arena() : nullptr);
    for (auto& block : basicBlocks_) {
        for (auto* insn : block->instructions()) {
            result.add(insn);
        }
    }
    //所有指令所有权已转移到 result（synthetic 节点从 unique_ptr 释放）
    for (auto& syn : syntheticLabels_) {
        (void)syn.release();
    }
    syntheticLabels_.clear();
    return result;
}

bool isTerminatedBlock(BasicBlock* block)
{
    auto* last = block->lastInsn();
    if (!last) return false;
    int32_t opcode = last->getOpcode();
    return (opcode >= jbytecode::core::Opcodes::IRETURN &&
            opcode <= jbytecode::core::Opcodes::RETURN) ||
           opcode == jbytecode::core::Opcodes::ATHROW;
}

}   //namespace mjt::base::ast::cfg
