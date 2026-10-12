#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 类成员排序器：对字段（先按 desc 再按 name 字典序）和方法
///（<clinit> 最前，其次 <init>，其余按 name 字典序）排序，对抗字段/方法打乱混淆。
///
///输入必须是可解析 class；排序失败或存在依赖顺序不确定的资源时保持原样。发生排序时
///返回 `none` 并使 class/member analysis 失效，否则返回 `all`。需要 reader/writer
///roundtrip 和 JVM 行为 fixture；当前为 `opt-in`，不得进入默认优化链。

///@par 标签
///- `scope:class`
///- `action:reorder`
///- `stability:opt-in`
class ClassMemberSorter : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode,
                          ClassAnalysisManager&       cam) override;
};
}   //namespace mjt::base
