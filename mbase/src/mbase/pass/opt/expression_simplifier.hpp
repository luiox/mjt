#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 化简代数恒等式和常见混淆表达式形状。
///
///@par 输入与不支持范围
///只处理 matcher 明确登记的连续 peephole 规则，不进行完整栈或 CFG 推理；浮点、溢出、
///异常敏感恒等式、跨 label/frame 和未知 producer 必须保持原样。
///
///@par 结果、所有权与验证
///至少一次完整替换返回 `none`，否则 `all`；方法/frame 分析失效。替换节点必须 clone，
///由 instruction list 接管，构建失败不得部分写回。需要每条规则的反例、roundtrip 和 JVM
///等价 fixture。当前属于 `default-optimize`。

///@par 标签
///- `scope:method`
///- `action:simplify`
///- `stability:opt-in`
class ExpressionSimplifier : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};
}   //namespace mjt::base
