#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@defgroup try_catch_cleanup 异常表清理家族
///
///本家族全部聚焦在 `methodNode.tryCatchBlocks` 异常表上，按不同的「异常表项非法 /
///冗余 / 等价」判据删除或修复条目。四个 pass 共享同一主题：**清理 try-catch 异常表**，
///区别只在删除判据（退化区间、非法顺序、NPE 等价包裹、空范围/重抛 handler）。
///
///- `RedundantTryCatchRemover`：删除退化条目、透明重抛条目和区间内无可能抛出异常
///指令的不可命中条目（F9，对齐 Recaf RedundantTryCatchRemovingTransformer）。
///- `TryCatchRepair`：按 label 顺序清理非法（重叠/缺失/顺序错误）异常表项和 stale throws。
///- `UselessTryCatchRemover`：消除 `ACONST_NULL;ATHROW` + catch-all/NPE handler 的等价包裹。
///- `InvalidTryCatchRemover`：删除空保护范围或 handler 直接 `athrow` 的条目。
///
///都不改写指令流本身（除 `UselessTryCatchRemover` 把 NPE 抛出点改写为 goto），只动异常表。
///共享的 label 定位 / try range 校验 / NPE 形状匹配 helper 在本家族 `.cpp` 匿名命名空间，
///原分散在 `control_flow_detail.hpp` / `clean_pass_detail.hpp` / `structure_detail.hpp`
///的相关区段已合回。
///@{

///@brief 删除退化为单一 label 的异常表项，并按 F9 判据删除两类可证明冗余的条目。
///
///@par 输入契约与不支持范围
///三条判据，全部保守多重条件，宁可漏删不可误删：
///1. `start == end && handler == end` 的退化条目（原有规则）；
///2. 透明重抛条目（对齐 Recaf 27a75ca85）：catch 体只把捕获异常原样 `athrow`——直接
///`athrow`，或 `astore X; aload X; athrow` 同槽恒等传递——且该 handler 仅能经本
///异常表项进入（普通控制流不可达、handler 体内无来自体外可达代码的普通流入、
///handler 不被其他条目共享）时删除。本仓额外收紧：与其他条目保护区间的任何重叠、
///或其他条目保护区覆盖 handler / handler 体（存在其他异常边进入 handler 体）时
///一律不删，避免改变异常表顺序派发语义；
///3. 不可命中条目（对齐 Recaf c64716819 + 7aa4e25dc 的 canThrow 集合）：帧分析成功且
///保护区间内所有可达指令均不属于「可能抛出异常」的 opcode 集合（athrow、方法调用、
///invokedynamic、new/分配/类型解析类、字段与数组访问、监视器、除法余法）时删除。
///帧分析失败（ok=false 或帧数不齐）、JSR/RET 子例程、标签缺失、范围退化、handler 落在
///保护区间内等情况一律保守回退，不删除。本仓无类型格，规则3 不做按 catch 类型细化，
///对带类型条目更保守（区间内没有任何可能抛出任意异常的指令才删）。
///
///@par 变换、失效与所有权
///只删异常表项，不改写指令流（被删条目的 handler 体成为死代码，交由后续 dead-code 类
///pass 收敛）。异常表发生删除时返回 `none`，否则返回 `all`；label 节点不由本 pass
///释放，由所属 `MethodNode` 统一管理。
///
///@par 验证与发布状态
///需要空范围、有效范围、透明重抛、不可命中、分析失败回退和 writer/verifier fixture；
///当前保持 opt-in。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class RedundantTryCatchRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 按 label 顺序清理明显非法的异常表项和 stale throws 元数据。
///
///@par 输入契约与不支持范围
///只处理能由当前方法 label 顺序直接证明非法的条目；不拆分或合并重叠有效区间，
///不推断复杂异常数据流，缺少 label 时保持输入不变。
///
///@par 变换、失效与所有权
///删除异常表或 throws 项时返回 `none`，无变化返回 `all`；异常类型字符串和 label
///由方法/arena 所有，本 pass 不释放共享对象。
///
///@par 验证与发布状态
///必须覆盖嵌套 handler、非法顺序、writer roundtrip 和 JVM verifier；当前为 opt-in。
///
///@par 迁移忠实度
///`FAITHFUL`，更复杂的异常表变换需要单独设计和验证后再引入。
///
///@par 标签
///- `scope:method`
///- `action:normalize`
///- `stability:opt-in`
class TryCatchRepair : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 在可证明等价的形状下消除无效 Try-Catch 包裹。
///
///@par 输入与不支持范围
///只接受 catch-all 或精确捕获 `java/lang/NullPointerException` 的异常表项；try 区间必须
///物理上恰为 `ACONST_NULL; ATHROW`，handler 必须恰为 `POP` 加可选 `GOTO`。异常区间与
///其他 try-catch 重叠、标签缺失、handler 形状不明确或其他异常类型均保持原样。
///
///@par 变换规则、失效与所有权
///删除异常表项，把 try 体替换为直接跳转到 handler 的 continuation；handler 指令本身不删，
///以保留共享标签和外部引用。成功返回 `none`，无匹配返回 `all`；删除的异常表项由
///当前 pass 释放，指令节点由 `InsnList::set/remove` 负责释放。
///
///@par 验证与发布状态
///需要异常类型、重叠区间、handler 边界、reader/writer roundtrip 和 JVM 行为验证。当前为
///opt-in 的保守迁移，不进入默认链。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class UselessTryCatchRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 移除无效的 try-catch 块。
///
///@par 中文契约
///输入是方法级 tree；只删除空保护范围或 handler 直接重新抛出的条目，不重排指令和
///label。异常表发生删除返回 `none`，无变化返回 `all`；被删的 try-catch 节点由
///方法容器释放。label 缺失或范围无法判断时保持不变，需 roundtrip/verifier fixture。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class InvalidTryCatchRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 把「try 体仅 new/dup/invokespecial<init>/athrow、被自身 handler 接住」的伪异常
///跳转改写为直接 `goto handler`，并删除该异常表项（对应 issue ）。
///
///混淆器常用 `try { throw new E; } catch (E e) { ... }` 把一次无条件控制转移伪装成异常
///派发。本 pass 识别 try 区间物理上恰为 `NEW E; DUP; INVOKESPECIAL E.<init>V; ATHROW`、
///区间内无其他指令/label、catch 类型为 catch-all 或恰为 E、且 athrow 处操作数栈恰为 1
///（仅刚 new 出的异常对象）的形状，把 `athrow` 改写为 `GOTO handler`——此时栈顶正是那个
///异常对象，与 JVM 异常派发进入 handler 时的栈形状（`[E]`）逐位一致，故语义等价。随后删除
///该异常表项。保留 `new/dup/invokespecial`，交由 `RedundantJumpRemover` / 死代码清理等后续
///pass 进一步收敛（跳转目标即 handler 时可退化为顺序执行）。
///
///@par 输入与不支持范围
///仅接受上述精确形状。athrow 处栈深不为 1（异常对象下方还压着值，goto 无法复刻异常派发对栈的
///清空）、catch 类型为 E 的严格父类型（需类层级信息，本 pass 不做，只认 catch-all 或精确
///相等）、区间与其他 try-catch 重叠、label 缺失或帧分析失败时，一律保持原样。构造函数
///`E.<init>V` 被假定不抛异常（标准 Throwable 子类成立）。
///
///@par 结果、失效与所有权
///发生改写返回 `none`，否则 `all`；被删异常表项由本 pass `delete`，`athrow` 旧节点由
///`InsnList::set` 释放，label 与其余指令节点仍由 `MethodNode` 管理。当前 opt-in，需
///reader/writer roundtrip 与 JVM 行为等价证据。
///
///@par 标签
///- `scope:method`
///- `action:rewrite`
///- `stability:opt-in`
class SelfCaughtThrowRewriter : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@}

}   //namespace mjt::base
