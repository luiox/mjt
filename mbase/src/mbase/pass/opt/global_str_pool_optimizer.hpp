#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 将项目字符串常量迁移到生成的全局字符串池。
///
///@par 输入与不支持范围
///只处理可解析、可生成 helper class 且字符串索引稳定的资源；动态拼接、不可写回的
///class、已有同名 helper、资源冲突和 writer 失败必须保持原样。
///
///@par 结果、所有权与验证
///将 `ldc` 改写为池访问并生成 `<clinit>` 初始化类；所有新 class/指令由容器和目标 class
///拥有。成功变更返回 `none`，否则 `all`，并使资源列表、调用图、引用索引和帧分析失效。
///需要字符串内容、初始化顺序、roundtrip/JVM fixture。当前为 `opt-in`。

///@par 标签
///- `scope:global`
///- `action:rewrite`
///- `stability:opt-in`
class GlobalStrPoolOptimizer : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};
}   //namespace mjt::base
