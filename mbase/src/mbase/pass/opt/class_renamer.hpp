#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 类重命名器：按规则重命名类（跨类，需全程序 remap）。
///
///输入必须拥有完整全局 remap 表和资源索引；名称冲突、字符串/描述符/签名无法同步更新、
///反射入口未知或任一 writer 失败时保持全部资源不变。成功重命名使资源列表、引用索引、
///调用图和 class analysis 失效；当前为 `opt-in/experimental`，需跨类 roundtrip/JVM 验证。

///@par 标签
///- `scope:global`
///- `action:rewrite`
///- `stability:opt-in`
class ClassRenamer : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};
}   //namespace mjt::base
