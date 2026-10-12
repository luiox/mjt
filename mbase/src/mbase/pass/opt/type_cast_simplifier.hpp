#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 折叠基本类型转换和受支持的常量转换辅助调用。
///
///@par 输入与不支持范围
///只处理 JVM 语义已固定的 primitive cast、`Float.intBitsToFloat`、`Integer.parseInt` 等
///白名单；解析失败、溢出、未知 overload、调用副作用和 frame/label 边界保持原样。
///
///@par 结果、失效与验证
///成功替换返回 `none`，否则 `all`；方法/frame 分析失效，常量节点必须新建并由列表拥有。
///需要 NaN、signed zero、窄化边界、非法 parse、roundtrip/JVM fixture。当前属于 `default-optimize`。

///@par 标签
///- `scope:method`
///- `action:simplify`
///- `stability:opt-in`
class TypeCastSimplifier : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};
}   //namespace mjt::base
