#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 将常量 char 数组构造的 `new String(char[])` 折叠为字符串常量。
///
///@par 输入与不支持范围
///只处理完整、连续的下列形状：`new java/lang/String` + `dup` + 常量长度表达式 +
///`newarray char` + 若干 `dup`/常量下标/常量值/`castore` + `invokespecial
///java/lang/String.<init> ([C)V`。长度、下标、每个元素值都必须是可证明的整型常量表达式
///（`ldc`/`iconst`/`bipush`/`sipush` 与 `ixor`/`iand`/`ior`/`iadd`/`isub`/`imul`/移位/
///`ineg`/`i2c`/`i2b`/`i2s` 的组合，不含可能抛异常的 `idiv`/`irem`）。任意别名、逃逸、
///部分初始化、越界或未覆盖的下标、非 `T_CHAR` 数组、非直接常量或分析失败都保持原指令。
///
///变换前：
///@code
///new java/lang/String
///dup
///ldc -163092100
///ldc -163092098
///ixor
///newarray char
///dup
///... ; 每个元素的常量 ixor/ldc 表达式 + i2c + castore
///invokespecial java/lang/String.<init> ([C)V
///@endcode
///变换后：
///@code
///ldc "..."
///@endcode
///
///@par 结果、失效与发布状态
///只有完整替换时返回 `PreservedAnalyses::none`，否则返回 `all`；方法指令、常量池和帧
///分析失效。新 `ldc` 常量 intern 进作业级 arena（`am.arena`），失败时不得部分写回。
///`castore` 遵循 JVM int→char 截断，`char[]` 按 UTF-16 code unit 还原为 UTF-8；非法
///surrogate 保持原指令。这是 deobfuscate/recover 的 `opt-in` pass，不进入默认链。
///
///@par 标签
///- `scope:method`
///- `action:fold`
///- `stability:opt-in`
class ConstCharArrayStringFolder : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

}   //namespace mjt::base
