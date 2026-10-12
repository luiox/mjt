#pragma once

#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@defgroup fix_pattern_simplifier 固定模式简化器
///
///用更简单的等价字节码替换已登记的固定混淆模式。
///
///## 目标
///
///`FixPatternSimplifier` 是 `phase.fix-pattern-replace` 中的最终固定模式清理
///pass。针对传统混淆器输出中常见的字节码形状，这些形状过于特定，不适合用通用
///代数或数据流抽象处理。
///
///与通用简化器的关系：
///
///- `ArithmeticSimplifier`：折叠直接的常量算术。
///- `ExpressionSimplifier`：处理可复用的代数恒等式。
///- `FixPatternSimplifier`：处理命名的字节码惯用模式，有显式的 before/after 形状。
///
///## 规则风格
///
///规则刻意保持具体。每条规则匹配一个短指令窗口并生成替换指令列表。替换拥有新节点
///或匹配节点的 clone；绝不复用匹配的原始节点，因为 matcher 应用会在策略运行后删除
///原始匹配区间。
///
///pass 迭代至无规则可应用。可能决定不转换的规则必须将安全检查放入匹配本身，
///否则可能永远匹配同一段不变的字节码。
///
///## 整数比较规则
///
///支持两种比较模式：
///
///1. `loadInt; iadd; loadInt; if_icmp*`
///
///等值/不等值比较可按 JVM int 环绕将左侧常数移至比较右侧；有符号大小比较只有
///左侧常数为 0 时才变换。例如 `(x + 1) == 2` 变为 `x == 1`，而 `(x + 1) > 2`
///保持原样以避免 int 环绕改变大小关系。
///
///2. `loadInt; istore s; loadInt; iload s; if_icmp*`
///
///通过在 reload 处 clone 原始 producer 来移除局部变量中转。仅当 `istore` 和
///`iload` 使用相同 slot 时匹配。
///
///第二个规则故意不是通用的局部传播 pass。它不追逐任意定义或跨越 label，
///只识别确切的遗留固定模式。
///
///## 边界
///
///pass 不推理：
///
///- 匹配窗口外的变量活跃性
///- 异常边或栈帧（stack map）结构
///- 列出模式外的交换性
///- 明确匹配的安全加载形式外的 producer 副作用
///- 用 hashCode 推断字符串相等（hash 碰撞使其不等价）
///- 删除 `newarray` 等仍可能抛异常的 producer
///
///更广泛的传播属于帧/数据流分析。新增固定规则应保持小规模，在 header Doxygen 中
///包含示例，并添加 roundtrip 测试验证生成 class 可重载。
///
///## 输入与不支持范围
///
///只匹配明确登记的指令序列和 slot 关系，不跨 label/frame/异常边，不猜测未登记模式。
///新规则必须先固定栈形状、producer 所有权和失败保持原样。
///
///## 验证覆盖
///
///- 核心位操作固定模式
///- 六种 `IF_ICMP*` 的安全比较折叠和非零偏移大小比较保留
///- hash 碰撞字符串比较保留
///- 被 pop 的数组分配保持原样
///- 同 slot store/load 比较重写（带 clone producer 节点）
///- 不同 slot store/load 保持不变
///- 每次重写后的 class 重载
///
///@par 标签
///- `scope:method`
///- `action:simplify`
///- `stability:opt-in`
class FixPatternSimplifier : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};
}   //namespace mjt::base
