#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@defgroup constant_fold 常量折叠模块
///
///本模块覆盖所有把可证明为常量的字节码区间替换为等价常量加载的 pass，按实现路径分两类：
///
///- `ArithmeticSimplifier`：用 `PatternMatcher` 折叠直接的常量算术
///（`iconst_1; iconst_2; iadd` → `iconst_3`）。
///- `SimpleConstantFolder` / `LocalConstantFolder` / `ConstantFolder`：用帧分析
///（`SourceInterpreter` / `ConstantInterpreter`）折叠常量指令组。
///
///两条路径目标一致（折叠常量算术），只是 matcher 版本只看连续指令形状、不建立帧分析，
///适合作为 fix-pattern 阶段的早期轻量折叠；帧分析版本能处理跨指令的常量传播，覆盖更广。
///两者都在 `default-optimize` 中跑：matcher 版在 `phase.fix-pattern-replace`，
///帧分析版在 `phase.simple-constant-fold`。
///
///将可证明为常量的字节码区间替换为等价的常量加载指令，简化混淆后的算术表达式，
///使后续清理 pass 看到更小、更规则的指令流。设计原则是保守：
///如果 frame 分析不能安全证明等价，pass 保持方法不变。
///
///## 管线位置
///
///`phase.simple-constant-fold` 在第一轮 fix-pattern 之后、控制流简化之前运行：
///
///1. `UnusedLabelRemover` —— label 清理使 frame 分析输入更小
///2. `ConstantFolder` —— 主体常量折叠
///
///在控制流简化之前做常量折叠，给后续的跳转和死代码 pass 提供更确定性的常量。
///
///## 改写安全性
///
///不把原始指令移到新列表。在原 instruction list 上就地编辑替换为计算结果常量，
///迭代至指令数不动点。不继承旧的 matcher 替换所有权风险。
///
///## 验证覆盖
///
///- 单 pass `ConstantFolder` roundtrip：算术链、类型转换、幂等性
///- `ConstantFolder` 焦点测试：常量组、非常量操作数
///- 默认管线测试：class 经 default-optimize 重写、重读后常量算术链被折叠
///
///新增常量物化类型时必须同时添加直接 pass 测试和 roundtrip/重载测试，
///因为此类失败多数是 writer/reader 或指令所有权问题，而非简单的错误 opcode。

///@brief 用 PatternMatcher 折叠直接的常量算术运算。
///
///@par 作用
///在表达式化简前消除整数、长整数、浮点和双精度常量运算噪声，并迭代到规则不再命中。
///
///@par 输入契约
///仅处理方法级 tree 中连续、无元数据边界的常量加载与受支持算术 opcode；不依赖外部
///analysis。常量读取和新指令构造必须通过 `InsnUtil`。
///
///@par 不支持范围
///整数除零/取模保持原样以保留 `ArithmeticException`；不跨 label/frame/line 匹配，不折叠
///方法调用、字段读取、动态常量或未知数值类型。
///
///@par 变换规则
///例如 `iconst_1; iconst_2; iadd` 变为 `iconst_3`。至少一次规则替换算 changed；达到不动点
///或无规则匹配时为 unchanged。
///
///@par 分析失效
///changed 返回 `none`；unchanged 返回 `all`。当前不声明 CFG 等局部分析可保留。
///
///@par 所有权与异常
///`PatternMatcher` 负责移除旧节点并把 builder 产生的新节点交给 instruction list；规则不得
///复用已在列表中的节点，必须 clone。无法读取常量时保持原序列。
///
///@par 验证与发布状态
///已有除零不变、roundtrip 和常量算术 fixture；属于 `default-optimize`。
///
///@par 迁移忠实度
///`FAITHFUL`，新数值规则必须补 JVM 边界测试后才能启用。
///
///@par 标签
///- `scope:method`
///- `action:fold`
///- `stability:default`
class ArithmeticSimplifier : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///简单整数常量折叠器。
///
///对应 Java SimpleConstantFolder 的忠实移植。
///使用 SourceInterpreter 帧分析，对 IADD/ISUB/IMUL/IDIV/IREM/IXOR 操作进行
///整数常量折叠。仅处理两个操作数都是已知整型常量的情况。
///分析失败、除零、未知值或 frame 数量不一致时保持方法不变；发生替换返回 `none`，
///无变化返回 `all`，并使方法/frame 分析失效。新节点由 instruction list 接管。需要
///除零、溢出、roundtrip 和 verifier fixture；当前为 opt-in。
///
///@par 标签
///- `scope:method`
///- `action:fold`
///- `stability:opt-in`
class SimpleConstantFolder : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///基本块内滑动窗口常量折叠器。
///
///对应 Java LocalConstantFolder 的忠实移植。
///使用 ConstantInterpreter 帧分析，在基本块内以滑动窗口方式识别纯计算区间，
///通过栈帧安全线校验保证不读取窗口前未知栈元素，最终将区间替换为计算结果常量。
///只处理基本块内可证明的纯计算区间；frame/窗口失败时 fail-closed。变更返回 `none`，
///否则 `all`，方法/frame 分析失效。需要分支、category-2、异常边、roundtrip/JVM fixture；
///当前为 opt-in。
///
///@par 标签
///- `scope:method`
///- `action:fold`
///- `stability:opt-in`
class LocalConstantFolder : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///常量折叠器（默认管线入口，主区域折叠器）。
///
///对应 Java 常量分组折叠实现。
///算法：粗扫描出「纯常量指令组」（起始必须是数值常量加载，组内只能含常量加载/受支持
///栈操作/算术/类型转换），再用 ConstantInterpreter 比较候选起止 frame 的既有栈前缀。
///只有前缀不变、净新增栈值全为可物化常量且替换能缩短指令数时，才将区间替换为等价常量加载。
///迭代到指令数不动点。与 LocalConstantFolder 的窗口扫描不同：这是按「组」折叠。
///
///只折叠 ConstantInterpreter 证明等价的区间，并在不动点重新分析；未知值、异常边、
///动态调用和 frame 失败保持原样。`DUP_X*`、数组、对象和 class literal 不在默认实现的支持
///范围内。发生改写返回 `none`，否则 `all`；指令/frame 分析失效，节点由 instruction
///list 管理。需要 category-2、roundtrip/verifier fixture。当前进入默认优化链，迁移忠实度为
///支持形状内 `FAITHFUL`。
///
///@par 标签
///- `scope:method`
///- `action:fold`
///- `stability:default`
class ConstantFolder : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

}   //namespace mjt::base
