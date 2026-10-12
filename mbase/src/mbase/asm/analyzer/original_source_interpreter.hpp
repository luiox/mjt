#pragma once

#include <vector>

#include "jbytecode/analysis/interpreter.hpp"
#include "jbytecode/core/type.hpp"
#include "jbytecode/tree/insn_list.hpp"
#include "original_source_value.hpp"

namespace mjt::base {

///@defgroup original_source_analysis 原始来源分析
///
///字节码值的 mbase 溯源层。基于 libjbytecode 的 ASM 风格
///`Analyzer<Frame<Value>>` 执行模型，但记录比当前 producer 指令集更多的信息。
///目标是为优化器提供可靠答案：经过加载、存储、栈复制、简单算术和控制流合并后，
///值的来源是什么。
///
///## 角色
///
///`OriginalSourceInterpreter` 适用于需要局部推理的解混淆和优化 pass，无需提交
///到全方法符号执行。典型消费者：分支预测、switch 剪枝、死 producer 清理和未来
///的 source-to-consumer 重写。
///
///分析记录四类事实：
///
///- producer 指令（继承自 `SourceValue`）
///- 局部重载和栈复制的 copied-from 链
///- 方法参数标识（含方法入口处的局部 slot）
///- JVM 基本类型和字符串的简单常量
///
///这些事实有意保持局部性。解释器不建模 heap 写入、字段值、任意方法体或类初始化。
///通用方法折叠仍通过 `SimpleInterpreter` 方法解释器 opt in。
///
///## 值模型
///
///`OriginalSourceValue` 在整个 analyzer worklist 中按值复制。由于帧在合并传播
///中会被复制和移动，copied-from 链接绝不能别名调用者的临时栈或局部 slot。
///因此每次复制操作拥有源值的快照。后续 `OriginalSourceValue` 复制共享已拥有的
///父链而非复制它，使长溯源链保持轻量同时保证稳定生命周期。
///
///`originalSource_` 指针在每次复制、移动或赋值后归一化。它指向当前值或
///copied-from 链拥有的节点，使下游 pass 可走到稳定根而不依赖过期帧存储。
///
///方法参数同时携带布尔标记和局部 slot。防止两个相同大小的参数在方法入口被合并为
///一个不可区分的"参数"值。
///
///## 常量传播
///
///当两个操作数都已知时，解释器折叠标准 JVM 一元、二元和比较指令的常量。
///整数算术使用 JVM 风格的绕回和移位掩码。整数除法和取模零不折叠，因为字节码
///会在运行时抛出。浮点算术遵循原生 IEEE 操作，比较指令保留 JVM NaN 方向
///（`FCMPL`/`DCMPL` 产生 `-1`，`FCMPG`/`DCMPG` 产生 `1`）。
///
///方法调用折叠独立于操作码折叠。`SimpleInterpreter` 包含稳定库方法的小型方法
///解释器（如 parse 和位转换辅助函数）。这些回调接收参数值的本地副本，不可改变
///analyzer 帧状态。
///
///## 合并规则
///
///控制流合并是保守的：
///
///- producer 指令集取并集（匹配 `SourceInterpreter`）
///- 常量仅当两个入值有相同载荷时存活
///- 参数标识仅当两条边引用相同局部 slot 时存活
///- copied-from 链仅当两条边一致时存活，否则仅保留 producer 并集
///
///这使得分析对简单混淆模式有用，同时避免跨分支、异常处理器和 switch 合并点的
///虚假精度。
///
///## 边界
///
///原始来源分析应保持为基础，而非 pass 特定的符号执行引擎。新的折叠只应属于
///JVM 本地、确定且对所有消费者安全的地方。pass 特定假设、classpath 敏感方法
///摘要和 heap 事实应在专用分析或 reference-index 层实现。
///
///@brief Analyzer interpreter that preserves original value provenance.
///
///OriginalSourceInterpreter extends SourceInterpreter-style producer tracking
///with method-parameter markers, copied-from chains and conservative constant
///propagation. Optimizers can use the resulting Frame<OriginalSourceValue> to
///answer questions such as "did this comparison operand come from a parameter",
///"which instruction originally produced this local reload", and "is this
///branch target statically knowable after simple arithmetic".
///
///The interpreter is intentionally side-effect free: copyOperation snapshots
///the input value, merge only retains provenance when both incoming values agree,
///and constant folding refuses arithmetic cases whose JVM behavior would throw
///at runtime, such as integer division by zero.
class OriginalSourceInterpreter : public jbytecode::analysis::Interpreter<OriginalSourceValue>
{
public:
    OriginalSourceInterpreter() = default;

    ///@brief Creates an anonymous value for locals, stack slots and return types.
    ///
    ///A null type represents an empty local slot in the ASM analysis contract and
    ///therefore uses size 1; void remains the only zero-sized value.
    OriginalSourceValue newValue(const jbytecode::core::Type* type) override;

    ///@brief Creates a method-entry local and marks it as a parameter source.
    OriginalSourceValue newParameterValue(bool isInstanceMethod, int local,
                                          const jbytecode::core::Type& type) override;

    OriginalSourceValue newOperation(const jbytecode::tree::AbstractInsnNode* insn) override;

    ///@brief Creates a copy value that owns a snapshot of the copied source.
    OriginalSourceValue copyOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                      const OriginalSourceValue&               value) override;

    OriginalSourceValue unaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                       const OriginalSourceValue&               value) override;

    OriginalSourceValue binaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                        const OriginalSourceValue&               value1,
                                        const OriginalSourceValue&               value2) override;

    OriginalSourceValue ternaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                         const OriginalSourceValue&               value1,
                                         const OriginalSourceValue&               value2,
                                         const OriginalSourceValue&               value3) override;

    OriginalSourceValue naryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                      const std::vector<OriginalSourceValue>&  values) override;

    void returnOperation(const jbytecode::tree::AbstractInsnNode* insn,
                         const OriginalSourceValue&               value,
                         const OriginalSourceValue&               expected) override;

    ///@brief Merges two dataflow values while preserving only stable facts.
    ///
    ///Constants, method-parameter markers and copied-from chains survive only
    ///when both incoming edges describe the same fact. Producer instruction sets
    ///are still unioned, matching SourceInterpreter behavior.
    OriginalSourceValue merge(const OriginalSourceValue& value1,
                              const OriginalSourceValue& value2) override;
};

}   //namespace mjt::base
