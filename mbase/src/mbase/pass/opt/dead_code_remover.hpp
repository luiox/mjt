#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

///@defgroup dead_code_family 死代码清理家族
///
///本家族统一管理「删除不可达 / 无副作用指令」的所有 method pass：从不可达块的整段
///删除（`SafeDeadCodeRemover`）、到 `nop` 规范化（`NopRemover`）、再到局部 store/pop
///级别的精确清理（`DeadLocalStoreRemover` / `PopUnUsedLocalVariables` /
///`UselessPopCleaner`）。
///
///家族下又分两层契约：
///
///- `SafeDeadCodeRemover` 和 `NopRemover` 是**结构级死代码清理**，见下方
///`@defgroup structural_dead_code`。
///- `DeadLocalStoreRemover` / `PopUnUsedLocalVariables` / `UselessPopCleaner` 等是
///**局部存储清理**，见下方 `@defgroup local_store_cleanup`。
///@{

///@defgroup local_store_cleanup 局部存储清理
///本文是 `DeadLocalStoreRemover`、`PopUnUsedLocalVariables` 和 `UselessPopCleaner`
///的行为契约。它描述当前实现能够证明的范围，不把尚未验证的规则算作支持。
///
///这组 pass 在优化流水线后段清理不会到达可观察使用点的局部变量存储和栈生产者。
///三者的职责必须保持分离：
///
///* `DeadLocalStoreRemover` 删除没有到达有效 sink 的 store，以及只为该 store 服务的
///纯计算链；无法证明生产者无副作用时，只把 store 改成 `pop`/`pop2`。
///* `PopUnUsedLocalVariables` 是默认链成员。它只根据分析到的精确来源把无读取的
///store 改成匹配宽度的 `pop`/`pop2`，不负责删除生产者。
///* `UselessPopCleaner` 在后续阶段删除只被目标 `pop`/`pop2` 消费、且无副作用的
///常量或局部加载生产者；`pop2` 只剩一个窄值时降级为 `pop`。
///
///@par 输入形状与明确不支持范围
///三个 pass 都依赖 `OriginalSourceInterpreter` 的方法级 frame。分析失败、frame 数量
///与指令数量不一致、非法栈形状或缺少必要来源时，必须保持方法不变并返回
///`PreservedAnalyses::all`。
///`DeadLocalStoreRemover` 当前按局部 slot 做保守判定：只要该 slot 存在可到达有效
///读取，相关 store 不主动删除。这会保留部分本可删除的旧 store，但不会因槽位复用
///而误删；跨分支、异常边、宽值覆盖和类型不一致形状没有扩大规则覆盖。
///有效 sink 包括返回、分支/ switch、调用、字段写入、数组写入、抛出和 monitor 操作。
///调用、字段/数组访问、对象创建、类型检查、异常和同步操作都视为有副作用，不能从
///纯生产者链中删除。共享生产者或存在分析外消费者时同样保持原样或降级为 pop。
///
///@par 结果、失效与所有权
///发生任何字节码替换或删除时返回 `PreservedAnalyses::none`；无变化、分析失败和
///不支持输入返回 `all`。方法、frame 和数据流分析不在变换后继续复用。
///分析可能临时扩大 `maxStack`。只有确认发生变换时才提交该容量；无变化和异常路径
///必须恢复原值。被删除节点由 `InsnList` 的规划删除流程释放，新建替换节点交由
///`InsnList` 接管，pass 不重复释放，也不转移仍在方法中的节点所有权。
///
///@par 验证最低要求
///每次新增规则至少要有：
///1. category-1 与 category-2 的真实或半真实 fixture；
///2. reader/writer roundtrip，且重新读取后结构仍合法；
///3. 有 `javac`/JVM 时的前后行为等价验证；
///4. 槽位复用、分支/label、异常边和副作用生产者的保守性回归；
///5. unchanged、分析失败和异常路径不会泄漏临时元数据的测试。
///当前已固定的证据包括：窄值纯链删除、宽值纯链 JVM 等价、宽值副作用降级 `pop2`、
///槽位复用保守保留、跨 label 的 pop 清理，以及 roundtrip 重新加载。
///未覆盖的生产者或异常形状不得在默认流水线中宣称完成。
///
///@par 默认流水线边界
///`PopUnUsedLocalVariables` 和 `UselessPopCleaner` 可在默认清理阶段使用；
///`DeadLocalStoreRemover` 仍是 opt-in，直到跨异常边、宽值和更多真实 fixture 的证据
///足够。任何扩大默认范围的改动必须同步更新 pass 头文件、注册表状态和本文件。
///@{

namespace mjt::base {

///@defgroup structural_dead_code 结构级死代码清理
///
///本文是 `SafeDeadCodeRemover` 和 `NopRemover` 的行为契约。
///
///- `SafeDeadCodeRemover`：基于保守前向 CFG 分析删除不可达普通指令。
///- `NopRemover`：删除 `NOP` 指令做规范化，常作为其他 pass 的收尾清理。
///
///两者都进入 `default-optimize` 的清理阶段；都 fail closed：分析失败或形状不合法时
///保持原方法不变。`SafeDeadCodeRemover` 不主动改写仍可能被异常边到达的结构；
///`NopRemover` 不解析 raw class bytes，也不删 label/frame/line/try-catch 边界。
///@{

///@brief 删除由保守前向 CFG 分析证明不可达的普通指令。
///
///@par 作用
///清理无条件跳转或终止指令后的死代码，但不主动改写仍可能被异常边到达的结构。
///
///@par 输入契约与不支持范围
///输入必须是可建立 `BasicValue` frame 的方法级 tree；分析抛异常、frame 数量与指令数
///不一致、遇到未知控制流或异常边无法建模时保持原方法不变。
///
///@par 变换、失效与所有权
///仅删除 opcode>=0 且没有 reachable frame 的指令；删除发生时返回 `none`，否则返回
///`all`。删除节点由 instruction list 当前 pass 释放，label 和元数据不主动删除。
///
///@par 验证与发布状态
///已覆盖不可达 island、异常 handler 可达性、frame 失败、writer roundtrip 和 JVM 行为
///fixture；当前属于 `default-optimize` 的保守控制流阶段。新增控制流形状仍需 verifier
///或 JVM 等价证据。
///
///@par 迁移忠实度
///`FAITHFUL`，失败路径采用 fail-closed 策略。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:default`
class SafeDeadCodeRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 删除方法中的 JVM `nop` 指令。
///
///@par 输入与不支持范围
///只接受有效 method instruction list 中的普通 `InsnNode(NOP)`；不解析 raw class bytes，
///不删除 label/frame/line/try-catch 边界，畸形列表由调用方拒绝。
///
///@par 结果、所有权与验证
///删除至少一个节点返回 `none`，无 NOP 返回 `all`；指令位置和方法分析视为失效。
///被删节点由 instruction list 释放，不保留裸指针。需要 label 引用和 writer roundtrip fixture。
///当前属于 `default-optimize` 的保守清理阶段。
///
///@par 标签
///- `scope:method`
///- `action:normalize`
///- `stability:default`
class NopRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@}

///@brief 删除值不会到达有效 sink 的局部变量存储及其纯 producer。
///
///根据 `OriginalSourceInterpreter` 的 frame 分析找出没有流向 meaningful sink 的
///store，并递归删除只被这些 store 消费的纯 producer。无法证明生产者无副作用时，
///只把 store 改成 `pop`/`pop2`。
///
///@par 中文契约
///输入是方法级 tree，必须能建立 OriginalSource frame；分析失败、frame 数量不匹配或
///producer 所有权无法证明时保持原方法不变。只删除无副作用且只被 dead store 消费的
///producer；发生删除或 store->pop 替换返回 `none`，否则返回 `all`。被删除节点由
///规划删除器释放，debug local-variable 元数据不改写；分析所需的临时 `maxStack` 扩容在
///unchanged 或异常路径恢复。
///
///@par 默认流水线边界
///当前为 opt-in，不进入默认链，直到跨异常边、宽值和更多真实 fixture 的证据足够。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class DeadLocalStoreRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 删除没有读取的整数局部存储，无法证明 producer 可删时降级为 `pop`。
///
///@par 输入与不支持范围
///只处理整个方法内仅出现 `ISTORE`、没有 `ILOAD`/`IINC`/其他类型 VarInsn 的 slot。
///不处理 wide/reference/float 槽、类型复用、显式短 opcode、CFG 精确活跃区间或跨节点
///producer 链；这些形状保持原样。
///
///@par 变换、失效与所有权
///紧邻 store 的整数常量与 `ISTORE` 一并删除；其他 producer 保留并把 store 改成 `POP`，
///因而调用等副作用继续执行。变更返回 `none`，否则 `all`。删除集合去重，旧节点由
///规划删除器释放，新 `POP` 由 instruction list 接管，禁止沿用 legacy 的“盲删前一节点”。
///
///@par 验证与发布状态
///需要连续 store、IINC、类型槽复用、副作用 producer、roundtrip 和 JVM fixture。当前为
///opt-in；该实现是对 legacy 不安全规则的保守收缩，不以迁移数量计入默认能力。

///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class NoLoadStoreEliminater : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 将从未读取的局部变量 store 替换为 `pop/pop2`。
///
///是默认链成员。只根据分析到的精确来源把无读取的 store 改成匹配宽度的 `pop/pop2`，
///不负责删除生产者。
///
///@par 中文契约
///输入必须可建立 OriginalSource frame；失败或 frame/指令数量不一致时保持不变。只把
///未被 load/iinc 使用的 store 改成匹配槽宽的 `pop/pop2`，不删除 producer。替换发生时
///返回 `none`，否则 `all`；新节点由 instruction list 接管，分析临时扩容不得改变
///unchanged 输入。该 pass 已在默认清理链中，已有 int/wide store roundtrip、分支保守性
///和 JVM 等价证据；新增规则仍需同等级验证。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:default`
class PopUnUsedLocalVariables : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 跨类纯函数调用消除：找到结果被丢弃、且被调方法已证明 Pure 的项目内调用，
///用
///`iconst_1` 占位（让其他 pass 清理）。
///
///Java 是 AsmGlobalPass，三遍扫描整个
///ResourceContainer：
///1. 全局收集 `invoke; pop/pop2` 候选方法签名 `(owner, name, desc)`；
///2. 对每个候选方法，消费 `MethodEffectAnalysis`，只保留项目内已证明 `Pure`
///的目标；`ReadOnly` / `Impure` / `Unknown` 一律保守剔除；
///3. 全局改写：候选方法的 `invoke; pop` → `iconst_1`，`invoke; pop2` → 两条 `iconst_1`。
///
///@par 输入与不支持范围
///仅处理当前 `ResourceContainer` 内定义、且由 `MethodEffectAnalysis` 证明为 `Pure` 的
///项目内调用；外部库、缺失 owner、继承方法、ReadOnly/Impure/Unknown 和分析失败均保留。
///
///变换前：
///@code
///invokestatic cV.x I //cV.x 是纯函数（只 return 一个常量）
///pop
///@endcode
///变换后：
///@code
///iconst_1 //由后续 UselessPopCleaner 等清理
///pop
///@endcode
///
///@par 结果、失效与验证
///`invoke; pop/pop2` 成功替换为保持栈平衡的常量占位时返回 `none`，否则 `all`；资源、
///调用图、引用索引和方法分析失效。单个 owner 写回失败必须丢弃临时修改。需要跨类纯/非纯、
///category-2、roundtrip 和 JVM fixture。当前为 deobfuscate `opt-in`，迁移范围比 legacy 保守。

///@par 标签
///- `scope:global`
///- `action:eliminate`
///- `stability:opt-in`
class RedundantInvokeRemover : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& am) override;
};

///@brief 删除值立即被 pop 丢弃的字面常量加载。
///
///@par 输入与不支持范围
///只删除没有副作用的 literal `const; pop/pop2`，不触碰局部、字段、数组加载或调用；
///category-2 宽度、标签/异常边或指令链不完整时保持原样。
///
///@par 结果、失效与验证
///删除成功返回 `none`，否则 `all`；方法/frame 分析失效，删除节点由 instruction list
///负责释放。需要字符串/数值、宽值、异常边、roundtrip 和 JVM fixture。当前为默认
///fix-pattern 阶段。

///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class RedundantLoadValEliminator : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 冗余 store 消除：同 slot 的两次写入之间没有读取时，将前一次替换为 `pop/pop2`。
///
///@par 输入与不支持范围
///使用完整的 load/store slot 识别（包括 `_0.._3` 短 opcode）。遇到 `IINC`、跨 label 或
///分支合流的精确活跃关系、非法 frame、wide 类型混用时保守保持原样；不把 IINC 当作普通
///load，避免丢失它对旧 slot 值的读取。
///
///@par 结果、失效与所有权
///只替换能证明没有中间读取的前一次 store，保持 producer 和副作用不变；变更返回
///`none`，否则 `all`。重建时克隆原节点并由新 instruction list 接管，旧 list 不再重复
///释放。当前为 opt-in，新增规则必须补 roundtrip、JVM 和异常路径证据。

///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class RedundantStoreEliminater : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 无用检查清理：实例方法中 aload0;invokestatic Objects.requireNonNull;pop → nop。
///
///Java: dataflow/UnusedCheckCleaner.java。Java 用 PatternMatcher 但替换策略为空（删匹配三指令）。
///仅实例方法（this 必非 null）。
///只处理精确的 `aload_0; requireNonNull; pop` 形状；方法类型、调用描述符或 owner 不匹配时
///保持原样。变更返回 `none`，否则 `all`；当前为 opt-in，需 NPE/JVM roundtrip fixture。

///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class UnusedCheckCleaner : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 删除结果立即丢弃且局部可证明无异常/副作用的标准库调用。
///
///Java: dataflow/UnusedResultCallEliminater.java。legacy run 体没有真实改写；
///P3.3 起 C++ 提供保守实现，只处理紧邻常量 producer：
///`ldc "x"; invokevirtual String.length/isEmpty; pop`，以及常量参数的
///`Math`/`StrictMath` 无异常纯函数结果丢弃。项目内方法调用仍由
///`RedundantInvokeRemover` 结合 `MethodEffectAnalysis` 处理。
///
///风险边界：不删除可能触发 NPE、class-init、随机值、overflow exception
///或用户方法副作用的调用；不能证明 producer 全部为相邻常量时保持 no-op。
///发生删除返回 `none`，否则 `all`；方法/frame 分析失效。当前为 opt-in，需异常、class-init
///和 JVM 行为 fixture。

///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class UnusedResultCallEliminater : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 在后续阶段删除只被目标 `pop`/`pop2` 消费、且无副作用的常量或局部加载生产者；
///`pop2` 只剩一个窄值时降级为 `pop`。
///
///通过 `OriginalSourceInterpreter` 的 frame 分析定位 `pop`/`pop2` 的 producer，
///仅删除可证明无副作用且没有其他消费者的 producer；`POP2` 半边可删时降级为 `POP`。
///
///@par 中文契约
///只接受可建立 OriginalSource frame 的方法；分析失败时不修改输入。仅删除无副作用且
///没有其他消费者的常量、局部加载和栈复制 producer；`pop2` 半边可删时降为 `pop`。
///任意删除或替换返回 `none`，无变化返回 `all`；节点释放由规划删除器统一负责。
///unchanged/分析异常路径恢复临时 `maxStack` 扩容。当前位于 `default-optimize` 的保守
///清理阶段；已有常量 producer/pop roundtrip 和 JVM 等价证据，新增 producer 规则必须先
///完成跨 label、wide value 和 JVM 行为验证。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:default`
class UselessPopCleaner : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 消除「无副作用调用（System.nanoTime/currentTimeMillis、Math.random）的返回值
///经纯运算链最终被 pop 丢弃」的干扰死代码（对应 issue ）。
///
///混淆器常插入形如 `1 / ((int)System.nanoTime | 1)` 后 `pop` 的垃圾片段：调用一个无副作用
///的 native 方法，对返回值做一串无意义算术/位运算，最后整体 `pop` 丢弃，纯粹干扰静态分析。
///变换前：
///@code
///invokestatic java/lang/System.nanoTime J
///l2i ; iconst_1 ; ior ; iconst_1 ; swap ; idiv ; pop
///@endcode
///变换后：整链（含调用）删除。
///
///@par 判定（基于 OriginalSourceFrame 帧溯源）
///从每个 `pop`/`pop2` 出发回溯被丢弃值的完整生产树，要求：树内每个节点都是纯计算
///（常量/算术/类型转换/栈操作）或白名单内的无副作用调用；树内至少含一个白名单调用（否则纯常量
///链留给默认清理）；树内节点无「链外消费者」（值只喂给这个 pop）；且从链首到 pop 在指令序列上
///连续、其间无 `LabelNode`（不跨基本块，跳转不会落入链中）。全部满足才整链删除。
///
///@par 白名单与安全边界
///仅 `System.nanoTimeJ` / `System.currentTimeMillisJ` / `Math.randomD`——零参、返回值
///非常量但**无可观察副作用**、且此处返回值被丢弃，故删除安全。不含可能 NPE/类初始化副作用/
///抛异常的调用；项目内纯函数调用仍由 `RedundantInvokeRemover` + `MethodEffectAnalysis` 处理。
///
///@par 结果、失效与所有权
///发生删除返回 `none`，否则 `all`；帧/方法分析失效。删除节点由规划删除器释放。帧分析失败、
///frame 与指令数不一致时 fail-open。当前 opt-in，需混淆样本、宽值、跨 label 保守性与 JVM 行为
///等价证据。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class DeadSideEffectFreeCallRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@} //local_store_cleanup

///@} //dead_code_family

}   //namespace mjt::base
