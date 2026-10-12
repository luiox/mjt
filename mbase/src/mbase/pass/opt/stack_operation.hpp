#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@defgroup stack_operation 栈操作处理家族
///
///本家族统一管理 JVM 栈操作（DUP/SWAP/POP 族）的展开与化简。两个 pass 互为配对，
///在 `default-optimize` 的 `phase.fix-pattern-replace` 阶段前后协作：
///
///- `StackOperationSimplifier`：用 `PatternMatcher` 折叠冗余栈操作形状
///（`dup;pop`→∅、`pop;pop`→`pop2`、`ldc;dup`→`ldc;ldc` 等）。
///- `StackOperationExpander`：用 `OriginalSource` 帧分析把常量 producer 上的
///`DUP/DUP_X*/DUP2*/SWAP` 展开为显式常量加载，让后续折叠 pass 看到更规则的形状。
///
///expander 先把混淆的 DUP_X* 拆成显式加载，simplifier 再清理冗余栈操作；两者都是
///`default-optimize` 的成员，但 expander 是 opt-in，只在显式调用或特定 pipeline 中跑。
///匹配窗口外的栈值、未知 producer、跨 label/异常边时都保守保持原样。
///
///@{

///@brief 化简冗余的 JVM 栈操作序列。
///
///@par 作用
///清理 `dup/pop`、重复 `swap`、常量复制和受支持的 category-1/category-2 栈噪声。
///
///@par 输入契约
///仅处理方法级 tree 中连续的、由 matcher 明确列出的 opcode 形状；规则必须区分一槽值和
///两槽值，并且只能复制可安全 clone 的常量加载节点。
///
///@par 不支持范围
///不进行通用栈模拟，不跨 label/frame/line 或异常边匹配，不复制对象创建、调用、字段访问等
///有副作用 producer。无法证明 category 形状时保持原样。
///
///@par 变换规则
///例如 `dup; pop` 删除，`pop; pop` 合并为 `pop2`，常量后的 `dup/dup2` 展开为两个 clone。
///规则迭代到不动点；至少一次替换算 changed。
///
///@par 分析失效
///changed 返回 `none`；unchanged 返回 `all`。
///
///@par 所有权与异常
///matcher 释放被替换节点，builder 生成的新节点由 instruction list 接管；不得把原节点指针
///同时插入多个位置。规则构建失败时不得留下部分序列。
///
///@par 验证与发布状态
///已有一槽/两槽、clone、roundtrip 和 unchanged fixture；属于 `default-optimize`，新增规则必须
///同时提供 JVM 等价证据。
///
///@par 迁移忠实度
///`FAITHFUL`，但覆盖范围仅限已登记规则，不等同于通用栈优化器。
///
///@par 标签
///- `scope:method`
///- `action:simplify`
///- `stability:default`
class StackOperationSimplifier : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 将常量 DUP/SWAP 栈操作展开为显式常量加载。
///
///@par 输入与不支持范围
///依赖 OriginalSource frame，只处理 `DUP/DUP_X1/DUP_X2/DUP2/DUP2_X1/DUP2_X2/SWAP` 且每个
///被消费值只有一个常量 producer 的形状；共享 producer、wide value、未知 frame 或非法
///栈形状保持原方法。
///
///@par 结果、所有权与验证
///采用 pop/replay 生成新节点，成功替换返回 `none`，否则 `all`；方法/frame 分析失效，
///新节点由 instruction list 接管；unchanged/分析异常路径恢复临时 `maxStack` 扩容。
///需要 category-1/category-2、异常边、roundtrip 和 JVM 栈行为 fixture。已进入
///`default-optimize` 的 `phase.fix-pattern-replace`（StackOperationSimplifier 之后、
///NopRemover 之前），对齐 Recaf deobf 的栈操作消除 pass1。
///
///@par 标签
///- `scope:method`
///- `action:rewrite`
///- `stability:default`
class StackOperationExpander : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@}

}   //namespace mjt::base
