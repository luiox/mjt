#pragma once

#include "jbytecode/tree/nodes.hpp"

namespace mjt::base::ast::cfg {

class BasicBlock;

///@brief 异常边：描述 try-catch 区域内的块与 handler 的关系。
///
///一个异常边表明 source 块位于 try 区域内，如果抛出异常则转到 handlerBlock。
class ExceptionEdge
{
public:
    ExceptionEdge(BasicBlock* source, jbytecode::tree::TryCatchBlockNode* tryCatch,
                  BasicBlock* startBlock, BasicBlock* endBlock, BasicBlock* handlerBlock)
        : source_(source)
        , tryCatchNode_(tryCatch)
        , startBlock_(startBlock)
        , endBlock_(endBlock)
        , handlerBlock_(handlerBlock)
    {}

    BasicBlock*                         source() const { return source_; }
    jbytecode::tree::TryCatchBlockNode* tryCatchNode() const { return tryCatchNode_; }
    BasicBlock*                         startBlock() const { return startBlock_; }
    BasicBlock*                         endBlock() const { return endBlock_; }
    BasicBlock*                         handlerBlock() const { return handlerBlock_; }

private:
    BasicBlock*                         source_;
    jbytecode::tree::TryCatchBlockNode* tryCatchNode_;
    BasicBlock*                         startBlock_;
    BasicBlock*                         endBlock_;
    BasicBlock*                         handlerBlock_;
};

}   //namespace mjt::base::ast::cfg
