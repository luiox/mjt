#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 删除没有任何控制流或调试元数据引用的 label 节点。
///
///@par 作用
///
///清理由跳转、switch 或元数据重写遗留的孤立 label，减少 writer 输出中的无用锚点。
///
///@par
///输入契约
///仅接受方法级 tree。方法的 jump、switch、行号、异常表、局部变量和局部变量注解必须
///
///使用同一 instruction list 中的 `Label`；首尾 label 按 writer 边界保守保留。
///
///@par
///不支持范围
///不修复悬空 label 指针，不重排基本块，不重定向分支，也不推断 JSR/RET 子程序。
///
///输入引用关系不完整时调用方必须先拒绝该方法。
///
///@par 变换规则
///
///先收集所有元数据引用，再删除未出现在集合中的 `LabelNode`。删除至少一个节点算 changed；
///
///没有孤立 label 时保持 unchanged。
///
///@par 分析失效
///changed 返回
///`PreservedAnalyses::none`，因为指令索引和调试位置已变化；unchanged 返回
///`all`。
///
///@par
///所有权与异常
///从 instruction list 移除的 `LabelNode` 由本 pass 立即释放；仍被引用的 `Label`
///和元数据
///继续由 `MethodNode` 管理。该 pass 不捕获畸形输入造成的异常，也不提供部分回滚。
///
///
///@par 验证与发布状态
///已有 jump、行号、局部变量和 writer roundtrip fixture。它属于
///
///`default-optimize` 的清理阶段，只允许在会维护 label 引用的 pass 之后运行。
///
///@par 迁移忠实度

///`FAITHFUL`，并额外保守保留首尾边界 label。

///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:default`
class UnusedLabelRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};
}   //namespace mjt::base
