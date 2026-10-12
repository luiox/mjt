#pragma once

#include <memory>
#include <unordered_map>
#include <vector>

#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/core/primitives.hpp"
#include "jbytecode/tree/insn_list.hpp"
#include "jbytecode/tree/nodes.hpp"
#include "mbase/asm/cfg/basic_block.hpp"
#include "mbase/asm/cfg/exception_edge.hpp"
#include "mbase/asm/cfg/jump_edge.hpp"

namespace mjt::base::ast::cfg {

///@brief 控制流图构建器。
///
///从 MethodNode 构建 CFG：
///1. 预处理：在跳转指令后插入 LabelNode 保证块分割
///2. 分割 BasicBlock（以 LabelNode 为边界）
///3. 构造跳转边（条件/无条件/switch/fallthrough）
///4. 构造前驱关系
///5. 构造异常边
class ControlFlowGraph
{
public:
    ControlFlowGraph() = default;

    ///从 MethodNode 构建 CFG
    void create(jbytecode::tree::MethodNode& methodNode);

    ///创建基本块（工厂方法，禁止手动构造）
    BasicBlock* createBasicBlock();

    const std::vector<std::unique_ptr<BasicBlock>>& basicBlocks() const { return basicBlocks_; }
    BasicBlock*                                     entryBlock() const
    {
        return basicBlocks_.empty() ? nullptr : basicBlocks_[0].get();
    }

    const std::vector<std::unique_ptr<JumpEdge>>&      jumpEdges() const { return jumpEdges_; }
    const std::vector<std::unique_ptr<ExceptionEdge>>& exceptionEdges() const
    {
        return exceptionEdges_;
    }

    ///通过 core::Label 查找所属的 BasicBlock
    BasicBlock* findBlockByLabel(jbytecode::core::Label* label) const;

    ///从 CFG 生成 InsnList，接管 methodNode 原始指令的所有权
    ///注意：调用前需清空 methodNode.instructions
    jbytecode::tree::InsnList toInsnList();

private:
    void preProcess(jbytecode::tree::MethodNode& methodNode);
    void buildSuccessorEdges();
    void buildPredecessorEdges();
    void buildExceptionEdges(jbytecode::tree::MethodNode& methodNode);

    std::vector<std::unique_ptr<BasicBlock>>    basicBlocks_;
    std::vector<std::unique_ptr<JumpEdge>>      jumpEdges_;
    std::vector<std::unique_ptr<ExceptionEdge>> exceptionEdges_;

    //预处理后的指令列表（含插入的 LabelNode）
    std::vector<jbytecode::tree::AbstractInsnNode*> instructions_;

    //core::Label → BasicBlock 快速查找（LabelNode::label 或 JumpInsnNode.label）
    std::unordered_map<jbytecode::core::Label*, BasicBlock*> labelToBlock_;

    //合成（预处理插入）的 LabelNode，需要管理生命周期
    std::vector<std::unique_ptr<jbytecode::tree::LabelNode>> syntheticLabels_;

    //源方法：toInsnList 据此绑定 owner arena（arena 节点随方法域回收，见混合清除 B7）
    jbytecode::tree::MethodNode* sourceMethod_ = nullptr;

    int32_t nextBlockId_ = 0;
};

///检查块的末尾是否为 return/athrow（终止块）
bool isTerminatedBlock(BasicBlock* block);

}   //namespace mjt::base::ast::cfg
