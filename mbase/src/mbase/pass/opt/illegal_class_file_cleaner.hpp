#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 使用 libjclassfile 清理畸形 class-file 结构。
///
///@par 中文契约
///输入是 `ResourceContainer` 中的 class bytes；非 class 资源不触碰。解析、transform 或
///writer 任一步失败都保留原 bytes，不返回部分结果；bytes 发生替换时返回 `none`，全部
///保持时返回 `all`。容器拥有资源 bytes，本 pass 不保存裸指针。进入 clean/deobfuscate
///preflight 前必须通过真实畸形 class fixture 和重新读取验证，当前不进入 `default-optimize`。

///@par 标签
///- `scope:global`
///- `action:sanitize`
///- `stability:opt-in`
class IllegalClassFileCleaner : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& am) override;
};
}   //namespace mjt::base
