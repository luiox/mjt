#pragma once

#include <cstdint>
#include <set>
#include <vector>

#include "jbytecode/tree/insn_list.hpp"
#include "jbytecode/tree/nodes.hpp"

namespace mjt::base::ast::cfg {

class JumpEdge;
class ExceptionEdge;

///@brief 基本块：以 LabelNode 起始的连续指令序列。
///
///设计参考 IDA Pro 控制流图。
///- 每个块保证以 LabelNode 开头
///- 最后一个指令决定跳转类型（goto/if/switch/return/athrow）
///- 前驱边集由 CFG 构建器填充
class BasicBlock
{
public:
    explicit BasicBlock(int32_t id)
        : id_(id)
    {}

    int32_t id() const { return id_; }

    const std::vector<jbytecode::tree::AbstractInsnNode*>& instructions() const
    {
        return instructions_;
    }
    std::vector<jbytecode::tree::AbstractInsnNode*>& instructions() { return instructions_; }

    void addInstruction(jbytecode::tree::AbstractInsnNode* insn) { instructions_.push_back(insn); }

    jbytecode::tree::AbstractInsnNode* firstInsn() const
    {
        return instructions_.empty() ? nullptr : instructions_.front();
    }
    jbytecode::tree::AbstractInsnNode* lastInsn() const
    {
        return instructions_.empty() ? nullptr : instructions_.back();
    }

    //跳转边
    void      setJumpEdge(JumpEdge* edge) { jumpEdge_ = edge; }
    JumpEdge* jumpEdge() const { return jumpEdge_; }

    //前驱跳转边
    const std::set<JumpEdge*>& prevJumpEdges() const { return prevJumpEdges_; }
    void                       addPrevJumpEdge(JumpEdge* edge) { prevJumpEdges_.insert(edge); }

    //异常边
    const std::set<ExceptionEdge*>& exceptionEdges() const { return exceptionEdges_; }
    void addExceptionEdge(ExceptionEdge* edge) { exceptionEdges_.insert(edge); }
    bool isInException() const { return !exceptionEdges_.empty(); }

private:
    int32_t                                         id_;
    std::vector<jbytecode::tree::AbstractInsnNode*> instructions_;
    JumpEdge*                                       jumpEdge_ = nullptr;
    std::set<JumpEdge*>                             prevJumpEdges_;
    std::set<ExceptionEdge*>                        exceptionEdges_;
};

}   //namespace mjt::base::ast::cfg
