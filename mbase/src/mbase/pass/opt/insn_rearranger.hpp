#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@defgroup insn_rearranger 指令重排家族
///
///本家族的两个 pass 都用手写扫描 + clone 重建的方式重排连续指令块，目标一致：
///把指令整理成更规则的形状，让后续的常量折叠 / 表达式化简更容易命中。两者都不依赖
///帧分析，只看连续指令窗口的 opcode 形状。
///
///- `ArithmeticInsnRearranger`：按 slot 分组重排连续的
///`xLOAD;CONST;COMMUTATIVE;xSTORE` 块（可交换算术赋值）。
///- `CommutativeOperatorReorder`：在交换律运算链里把常量移到一起，方便后续折叠。
///
///两者都是 opt-in，只在显式调用或 `phase.fix-pattern-replace` 这类 pipeline 中跑。
///跨 label、异常边、副作用节点、栈宽度不一致时都保守保持原样；移动只复制/重建节点，
///不转移原节点所有权。
///
///@{

///@brief 重排连续的局部变量整数/长整数算术赋值块。
///
///Java: dataflow/ArithmeticInsnRearranger.java。块模式 xLOAD;CONST;COMMUTATIVE;xSTORE，
///按变量 slot 分组重排，仅在 ADD/MUL/AND/OR/XOR（可交换）时启用。
///整数溢出保持 JVM 的补码环绕语义。
///只有连续块且 frame/slot 形状完整时才允许重排；成功返回 `none`，否则 `all`，并使
///方法/frame 分析失效。需要溢出、long、label 和 JVM roundtrip fixture；当前为 opt-in。
///
///@par 标签
///- `scope:method`
///- `action:reorder`
///- `stability:opt-in`
class ArithmeticInsnRearranger : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 重排窄范围整数/长整数交换律链，使常量相邻。
///
///@par 输入与不支持范围
///只处理连续且 producer 安全的 int/long 常量、`iload/lload` 和 `getstatic I/J`；不跨 label、
///frame、调用、对象创建或其他副作用节点。分析或栈宽度不一致时保持原样。
///
///@par 结果、失效与验证
///发生重排返回 `none`，否则 `all`；方法、frame 和数据流分析失效，移动只复制/重建
///节点，不转移原节点所有权。需要交换律边界、溢出、category-2、roundtrip/JVM fixture。
///当前为默认 fix-pattern 阶段，规则必须保持保守。
///
///@par 标签
///- `scope:method`
///- `action:reorder`
///- `stability:opt-in`
class CommutativeOperatorReorder : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@}

}   //namespace mjt::base
