#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 常量局部变量优化器（ConsLocalVarOptimizer）。
///
///对应 Java constant/ConsLocalVarOptimizer.java（AsmMethodPass）。
///逻辑：若某 slot 仅被 store 一次，且 store 前一条指令是数值常量加载，则把该常量复制到
///所有 load 处（Java 注释：「只要一次 store 无数次 load 才能优化」）。
///分支合流、slot 重用、wide value、producer 有副作用或 frame 失败时保持原样；发生替换
///返回 `none`，否则 `all`，并使方法/frame 分析失效。已进入 `default-optimize` 的
///`phase.simple-constant-fold`（ConstantFolder 之后），对齐 Recaf deobf VariableFolding
///的变量常量折叠能力。

///@par 标签
///- `scope:method`
///- `action:propagate`
///- `stability:default`
class ConsLocalVarOptimizer : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 标签间常量局部变量传播器（LabelConsLocalVarPropagator）。
///
///对应 Java constant/LabelConsLocalVarPropagator.java（AsmMethodPass）。
///逻辑：在相邻两个 label 之间，若某 slot 的 store 前是数值常量，且 store 到 load 之间无
///其他 label，则把该 load 替换为常量加载（不删 store，对齐 Java）。
///只在 label 边界和 slot 生命周期可证明时改写；分析失败或跨 label 时保持原样。成功返回
///`none`，否则 `all`，并使方法/frame 分析失效。当前为 opt-in，需要标签、异常边和 roundtrip fixture。

///@par 标签
///- `scope:method`
///- `action:propagate`
///- `stability:opt-in`
class LabelConsLocalVarPropagator : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 常量返回值传播（ConsReturnPropagator）。
///
///对应 Java constant/ConsReturnPropagator.java（AsmGlobalPass）。
///消费共享 MethodEffectAnalysis 的纯函数常量返回摘要，把同一类中的静态调用

///替换为等价常量。调用参数的求值仍然保留，并按 JVM slot 宽度插入 POP/POP2；
///跨类调用暂不折叠，以避免错误删除目标类初始化语义。
///分析缺失、摘要不纯或 slot 宽度不匹配时保持原调用；发生改写返回 `none`，否则
///`all`，并使调用图、方法/frame 分析失效。已进入 `default-optimize` 的全局段
///（对齐 Recaf deobf CallResultInlining 的全程序能力）。

///@par 标签
///- `scope:global`
///- `action:propagate`
///- `stability:default`
class ConsReturnPropagator : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& am) override;
};

///@brief 常量 static 字段替换器（ConsStaticFieldReplacer）。
///
///对应 Java constant/ConsStaticFieldReplacer.java（AsmGlobalPass）。
///逻辑：static 字段若仅被初始化为常量（全程序无 putstatic 引用），则把所有 getstatic
///替换为该常量加载。只替换不删除字段定义。全程序三遍扫描：建字段表→统计 put/get 次数→替换。
///字段解析、初始化次数、常量类型或全局索引不满足条件时保持原样；成功改写返回 `none`，
///否则 `all`，并使引用索引、调用图、方法/frame 分析失效。已进入 `default-optimize`
///的全局段（对齐 Recaf deobf StaticValueInlining 的全程序能力，putstatic 审计更严格）。

///@par 标签
///- `scope:global`
///- `action:propagate`
///- `stability:default`
class ConsStaticFieldReplacer : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& am) override;
};
}   //namespace mjt::base
