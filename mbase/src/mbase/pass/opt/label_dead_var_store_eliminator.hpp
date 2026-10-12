#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 在单个标签区间内消除可证明成对的局部 `ISTORE`/`ILOAD`。
///
///@par 输入与不支持范围
///仅处理以同一 slot 的 `ILOAD` 开始、以 `ISTORE` 结束，且中间只有数值常量、算术和
///栈操作的连续区间；其他类型 VarInsn、IINC、调用、字段/数组访问、异常边和复杂 CFG
///保持原样。首尾合成 label 只用于临时划界，不属于输出契约。
///
///@par 结果、所有权与失效
///只删除相邻的 `ISTORE i; ILOAD i` 对，删除节点由当前 instruction list 释放；没有命中时
///必须保持指令和元数据完全不变并返回 `all`，命中时返回 `none`。分析异常或非法
///输入不得部分修改。
///
///@par 验证与发布状态
///需要 unchanged 字节一致、首尾无 label、roundtrip、JVM 和跨 label/异常边 fixture。
///当前为 opt-in 的 legacy 保守迁移，不进入默认链。

///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class LabelDeadVarStoreEliminator : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};
}   //namespace mjt::base
