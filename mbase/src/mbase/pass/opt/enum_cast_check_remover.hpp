#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 移除枚举类方法中可证明冗余的 `INVOKEVIRTUAL`/`CHECKCAST` 相邻指令对。
///
///@par 输入与不支持范围
///只处理枚举类的首个方法，并且调用描述符必须是当前枚举数组类型 `[L当前类;`。调用后
///必须物理紧邻 `CHECKCAST`；调用没有 cast、跨越 metadata 或出现在其他方法时保持原样。
///不推断栈别名、调用返回值用途或其他枚举方法的语义。
///
///@par 变换规则、失效与所有权
///仅成对删除调用和紧邻的 `CHECKCAST`，两者由当前 `InsnList` 释放；未命中时保持字节码不变
///并返回 `all`，命中时返回 `none`。异常输入不得部分修改。
///
///@par 验证与发布状态
///需要枚举/非枚举、缺少 cast、相邻 cast、reader/writer roundtrip 和 JVM 行为 fixture。
///当前为 opt-in 的保守迁移，不进入默认链。

///@par 标签
///- `scope:class`
///- `action:eliminate`
///- `stability:opt-in`
class EnumCastCheckRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode,
                          ClassAnalysisManager&       cam) override;
};
}   //namespace mjt::base
