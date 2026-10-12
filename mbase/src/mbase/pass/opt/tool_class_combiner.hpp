#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 保守合并简单的 static 工具类。
///
///@par 输入与不支持范围
///仅接受以 Object 为父类、名称不含 `$`、无接口、只含 static 字段/方法及构造器的 class。
///继承、接口、实例成员、反射/服务入口、重名或 remap 不完整时保持原资源。
///
///@par 结果、失效与发布状态
///合并后必须同步重映射所有字段/方法 owner；任何 class 写回失败都回滚本次合并。成功时
///返回 `none`，并使资源列表、引用索引、调用图和 class/frame 分析失效。需要跨类 JVM
///roundtrip fixture；当前为 `opt-in`。

///@par 标签
///- `scope:global`
///- `action:rewrite`
///- `stability:opt-in`
class ToolClassCombiner : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};
}   //namespace mjt::base
