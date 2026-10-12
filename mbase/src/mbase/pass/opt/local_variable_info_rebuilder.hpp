#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 局部变量信息重建器：根据 slot 使用情况重建 localVariables 表，
///对抗局部变量信息抹除。
///
///输入必须是可解析方法且 local slot 使用信息完整；无法推断类型、参数范围或 owner 类名时
///保持 localVariables 原样。成功重建返回 `none`，否则 `all`，并使方法调试/帧分析失效。
///需要静态/实例、宽类型 slot、异常控制流和 reader/writer fixture；当前为 `opt-in`。缺少
///owner 类名是明确的不支持范围，不得猜测描述符。

///@par 标签
///- `scope:method`
///- `action:recover`
///- `stability:opt-in`
class LocalVariableInfoRebuilder : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;

private:
    //从 VarInsnNode opcode 推断变量 desc（对齐 Java analyzeType）。
    static ca::str::Utf8String analyzeType(int opcode);
    //局部变量漂亮命名（对齐 Java getPrettyName(int)）。
    static ca::str::Utf8String getPrettyName(int index);
};
}   //namespace mjt::base
