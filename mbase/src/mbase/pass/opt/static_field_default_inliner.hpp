#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief static 字段默认值内联：clinit 中 putstatic 常量 → 写入字段 ConstantValue（忠实移植 Java
///StaticFieldDefaultInliner）。
///
///@par 输入与不支持范围
///只处理 `<clinit>` 中单次写入、值为 JVM 常量且字段未被其他路径覆盖的 static 字段。
///多次写入、动态值、异常控制流、已有 ConstantValue 冲突或分析失败时保持 class 原样。
///
///@par 结果、失效与验证
///成功写入 ConstantValue 并删除等价初始化时返回 `none`，否则返回 `all`；类元数据、
///方法/帧分析失效。写回失败必须保留原始 class。需要常量类型、类初始化顺序、roundtrip
///和 JVM fixture。当前为 `opt-in`。

///@par 标签
///- `scope:class`
///- `action:propagate`
///- `stability:opt-in`
class StaticFieldDefaultInliner : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode,
                          ClassAnalysisManager&       cam) override;
};
}   //namespace mjt::base
