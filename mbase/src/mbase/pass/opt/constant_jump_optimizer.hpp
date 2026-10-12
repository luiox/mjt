#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@defgroup constant_jump_optimizer 常量条件跳转折叠家族
///
///本家族把可静态证明为常量的条件跳转 / switch / 状态机折叠为确定性的 `goto` 或直接
///删除。三个 pass 共享同一主题：**把常量条件的控制流分支折叠掉**，区别只在被折叠的
///控制流结构（if / switch / state machine）。
///
///- `ConsJumpOptimizer`：折叠常量 `if*` 跳转为 `goto` 或删除。
///- `ConstantSwitchFolder`：折叠常量 `lookup/tableswitch` 选择器为 `goto` 目标。
///- `ConstantStateMachineEliminator`：消除可证明的常量状态机（混淆器常用的 state
///dispatch 模式），把多轮 dispatcher 调用串成直线控制流。
///
///共享的 jump predicate / switch 校验 / goto 替换 helper 在本家族 `.cpp` 匿名命名空间内，
///原拆在 `control_flow_detail.hpp` 的相关区段已合回。
///@{

///@brief 将结果可证明的常量条件跳转化为 `goto` 或删除。
///
///@par 输入与不支持范围
///仅处理单/双操作数比较的已知常量；未知栈值、异常控制流、JSR/RET、非法 label 和
///可能改变操作数消费语义的形状保持原样。
///
///@par 结果、失效与验证
///changed 返回 `none`，unchanged 返回 `all`；控制流和 frame 分析失效。已覆盖 true/false、
///双槽比较、null 分支、roundtrip 与 JVM 行为 fixture；异常边和新 predicate 仍需单独证据。
///当前属于默认控制流阶段。
///
///@par 标签
///- `scope:method`
///- `action:fold`
///- `stability:default`
class ConsJumpOptimizer : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 将可证明的常量 switch 选择器折叠为直接跳转。
///
///@par 输入与不支持范围
///只处理 frame/栈类型能证明 selector 为常量且所有目标 label 有效的方法；未知值、
///动态 switch、异常边或 malformed label 时保持原方法。
///
///@par 结果、失效与验证
///成功替换为等价 `goto` 或目标块时返回 `none`，否则返回 `all`；控制流、frame 和
///方法指令分析失效。需要 default/缺省分支、异常边、roundtrip 和 JVM verifier fixture。
///当前位于 `default-optimize` 控制流阶段，迁移忠实度为保守 `FAITHFUL`。
///
///@par 标签
///- `scope:method`
///- `action:fold`
///- `stability:default`
class ConstantSwitchFolder : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 消除可静态证明的常量状态机控制流。
///
///@par 输入契约与不支持范围
///仅接受已有 frame/CFG 能证明状态值唯一的方法；遇到合流、异常边、动态 switch 或
///未知状态时必须保持原样。
///
///@par 变换、失效与所有权
///任何跳转/指令改写都返回 `none`；无变化返回 `all`。被替换的指令由 instruction
///list 管理，label 引用必须在写回前保持有效。
///
///@par 验证与发布状态
///当前为实验性 opt-in，不得进入 `default-optimize`，直到有半真实样本和 JVM 行为验证。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class ConstantStateMachineEliminator : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@}

}   //namespace mjt::base
