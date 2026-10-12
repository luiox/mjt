#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 访问修饰符收紧器：把无外部引用的 public static 成员收紧为 private。
///
///依赖全程序 owner+name+desc 引用普查、反射/服务 keep 规则和可回滚 writer；缺少任一分析
///或存在外部入口时保持访问修饰符。当前实现若不能证明安全必须返回 `all`，状态为
///`opt-in/experimental`，需要跨类 roundtrip 和 JVM 访问验证后才能扩大规则。

///@par 标签
///- `scope:global`
///- `action:rewrite`
///- `stability:opt-in`
class AccessModifierTightener : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};
}   //namespace mjt::base
