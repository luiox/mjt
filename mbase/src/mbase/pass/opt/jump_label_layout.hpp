#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@defgroup jump_label_layout 跳转/标签布局规范化家族
///
///本家族整理跳转指令和 label 的物理布局，清理 goto 噪声、合并冗余 label、删除 goto
///后的死代码岛。四个 pass 共享同一主题：**让控制流的物理布局更规则、更紧凑**，区别
///只在处理的形状（goto-to-next-label、单入边块搬运、连续 label 合并、goto 后死代码）。
///
///- `RedundantJumpRemover`：删除落到下一个 label 的 `goto`；条件跳转改 `pop` 序列。
///- `GotoRearranger`：把单入边的简单基本块搬到 `goto` 前并删除该 `goto`。
///- `ContinuousLabelMerger`：合并物理连续的 label，把所有引用重定向到首个 label。
///- `ImpossibleJumpEliminater`：删除 `goto` 到下一个 label 之间的不可达指令岛。
///
///这组 pass 通常在常量折叠之后、死代码清理之前跑，让后续 pass 看到更干净的控制流。
///@{

///@brief 删除目标就是下一条 label 的冗余跳转。
///
///@par 作用
///消除直接落到自身目标的 `goto`；对条件跳转保留其操作数消费语义，用 `pop` 序列替换。
///
///@par 输入契约
///仅处理方法级 tree 中紧邻目标 `LabelNode` 的 `JumpInsnNode`。条件跳转必须属于 JVM
///已知的一槽或两槽比较 opcode，instruction list 和 label 引用必须有效。
///
///@par 不支持范围
///不跨 line/frame/其他 label 寻找目标，不处理 JSR/RET，不改变异常表，也不折叠非紧邻跳转。
///
///@par 变换规则
///`goto L; L:` 删除 `goto`；单操作数条件跳转替换为一个 `pop`；双操作数比较替换为两个
///`pop`。命中任一规则算 changed，否则 unchanged。
///
///@par 分析失效
///changed 返回 `none`，unchanged 返回 `all`；控制流边和指令位置均不声明保留。
///
///@par 所有权与异常
///删除的 `goto` 由本 pass 释放；替换条件跳转时 instruction list 接管新 `InsnNode`，旧节点
///由 `set` 的所有权协议释放。失败不做部分重写。
///
///@par 验证与发布状态
///已有 roundtrip fixture；条件跳转还需 JVM 等价测试。该 pass 位于
///`default-optimize` 的控制流阶段。
///
///@par 迁移忠实度
///`FAITHFUL`，对应 `controlFlowSimplifyManager` 中的同名 pass。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:default`
class RedundantJumpRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 将只有一个入边且不跨保护区的简单基本块移动到 `goto` 前。
///
///@par 输入与不支持范围
///需要完整 CFG；共享 jump/switch target、try-catch 保护 label、非终止块、JSR/RET、
///frame/label 不一致时保持原方法。
///
///@par 结果、失效与验证
///发生移动返回 `none`，否则返回 `all`；控制流、frame 和异常表分析失效。移动不转移
///label 生命周期，失败时不得部分写回。需要 loop、共享 target、异常区间、roundtrip 和
///JVM verifier fixture。当前为 `opt-in`。
///
///@par 标签
///- `scope:method`
///- `action:reorder`
///- `stability:opt-in`
class GotoRearranger : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 合并连续的 `LabelNode`，并将全部指令及元数据引用重定向到首个 label。
///
///@par 输入与不支持范围
///只处理物理上连续的 label，不跨 line/frame/普通指令合并。畸形 label、无法识别的自定义
///attribute 保持原样；被 frame 中未初始化对象引用的 label 保守保留，不参与合并。新增引用
///类型必须先纳入重定向或保护清单。
///
///@par 变换规则、失效与所有权
///跳转、switch、try-catch、行号、局部变量范围和局部变量类型注解全部改指向首个 label，
///再删除其余 `LabelNode`。命中返回 `none`，无连续 label 返回 `all`；被删节点经
///`removeAndDelete` 摘链（arena 节点不即时析构，随方法域 arena 统一回收），底层
///`Label` 归方法域 arena（B8 `owner_` 盖章归属制，旧 label 池已物理退役）。
///
///@par 验证与发布状态
///需要控制流、调试元数据、局部类型注解、未初始化对象 frame、reader/writer roundtrip 和
///unchanged fixture。
///当前为 opt-in 的 `FAITHFUL` 迁移，引用面未完整覆盖前不得进入默认链。
///
///@par 标签
///- `scope:method`
///- `action:normalize`
///- `stability:opt-in`
class ContinuousLabelMerger : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 删除无条件跳转后的不可达指令岛。
///
///@par 输入契约与不支持范围
///仅接受方法级 tree，并只处理 `goto` 到下一个 label 之间的普通指令；不重定向跳转，
///不跨越异常表边界，不处理 JSR/RET、动态控制流或缺失 label。
///
///@par 变换、失效与所有权
///删除至少一条指令时返回 `none`，未命中返回 `all`；被删除节点由 instruction list
///的当前 pass 负责释放，label/try-catch 元数据保持原有引用。
///
///@par 验证与发布状态
///必须通过不可达岛 roundtrip、异常表边界和 verifier fixture；当前为 opt-in 清理 pass。
///
///@par 迁移忠实度
///`FAITHFUL`，对应 legacy `ImpossibleJumpEliminater` 的保守范围。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class ImpossibleJumpEliminater : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@}

}   //namespace mjt::base
