#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 堆栈跟踪恢复：恢复 sourceFile/sourceDebug，在每个 try-catch handler
///头部插入 dup;printStackTrace，并在每个 label 后插入行号。
///
///只处理可解析且具备完整异常表的 class；无法确定 handler、行号或 source 信息时保持原样。
///写入调试信息会使 class/frame analysis 失效，失败时不得留下部分节点。需要异常堆栈、
///roundtrip 和 JVM fixture；当前为 `opt-in`。

///@par 标签
///- `scope:class`
///- `action:recover`
///- `stability:opt-in`
class StackTraceRecover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode,
                          ClassAnalysisManager&       cam) override;
};
}   //namespace mjt::base
