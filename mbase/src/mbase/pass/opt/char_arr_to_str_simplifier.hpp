#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 将常量 char 数组构造替换为字符串常量加 `String.toCharArray`。
///
///@par 输入与不支持范围
///仅处理完整、连续且每个元素都由 UTF-16 code unit 常量初始化的 `newarray char` 形状。
///非法 surrogate、部分初始化、别名/逃逸数组、任意拼接和分析失败必须保持原指令。
///
///变换前：
///@code
///iconst_2
///newarray char
///dup
///iconst_0
///bipush 72
///castore
///dup
///iconst_1
///bipush 105
///castore
///@endcode
///变换后：
///@code
///ldc "Hi"
///invokevirtual java/lang/String.toCharArray [C
///@endcode
///
///@par 结果、失效与发布状态
///只有完整替换时返回 `PreservedAnalyses::none`，否则返回 `all`；方法指令、常量池和
///帧分析失效。新节点由方法指令表拥有，失败时不得部分写回。需要 surrogate、roundtrip
///和 JVM 行为 fixture；这是 deobfuscate/recover 的 `opt-in` pass，不进入默认链。

///@par 标签
///- `scope:method`
///- `action:simplify`
///- `stability:opt-in`
class CharArrToStrSimplifier : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};
}   //namespace mjt::base
