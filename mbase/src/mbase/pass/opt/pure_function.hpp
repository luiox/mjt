#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@defgroup pure_function 纯函数家族
///
///本家族围绕「可证明无副作用的 static 方法」展开，三个 pass 形成一条 pipeline：
///
///- `PureFunctionAnnotator`：扫描整个 ResourceContainer，按 JDK 白名单和方法体 opcode
///分析，给纯 static 方法打上 `PureFunction` 不可见注解（固定点迭代处理互相调用的
///纯方法）。
///- `PureFunctionLocalizer`：把跨类的纯 static 被调方法**复制**到 caller 类，改写调用点
///owner，让后续的类内分析/内联能看到完整方法体。
///- `LocalPureFunctionInliner`：在类内**内联**已标记的纯方法调用（参数转发 + 局部变量
///重映射 + 单出口约束），消除调用开销。
///
///三者都依赖 `PureFunction` 注解作为分析中间表示，因此必须按 Annotator → Localizer →
///Inliner 顺序运行。三者都是 opt-in，不进入默认链。私有 helper（注解常量、方法体纯性
///判定、复制/内联 builder）在本家族 `.cpp` 匿名命名空间内，原拆在 `pure_function_detail.hpp`
///的内容已合回。
///@{

///@brief 标记没有可观察副作用的 static concrete 方法。
///
///@par 输入与不支持范围
///需要完整项目 class 集合和固定的 JDK 白名单；实例方法、字段写入、未知调用、异常/同步
///操作、反射和分析不完整的方法一律不标记。只允许在证明到固定点后写入兼容的
///`PureFunction` 标记。
///
///@par 结果与失效
///新增标记时返回 `none`，否则返回 `all`；方法摘要、调用图、引用索引和类元数据失效。
///单个类失败不得写入半成品标记。
///
///@par 验证与发布状态
///需要副作用反例、递归/未知调用、roundtrip 和 JVM fixture。当前为 `opt-in`，不能把标记
///数量作为语义正确性的证明。
///
///@par 标签
///- `scope:global`
///- `action:annotate`
///- `stability:opt-in`
class PureFunctionAnnotator : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};

///@brief 将跨类的纯 static 被调方法复制到 caller 类。
///
///@par 输入与不支持范围
///只处理已由 `PureFunctionAnnotator` 证明、可复制且无递归/异常表/访问冲突的
///`invokestatic` 目标；标记缺失、目标解析失败、重名或跨模块访问不满足时保持原调用。
///
///@par 结果、所有权与失效
///新方法和指令必须由目标 class 拥有并重新分配，成功改写返回 `none`，否则返回 `all`；
///类资源、调用图、引用索引和方法分析失效。任一复制失败时丢弃临时 class 修改。
///当前为 `opt-in`，需要跨类 roundtrip/JVM fixture。
///
///@par 标签
///- `scope:global`
///- `action:rewrite`
///- `stability:opt-in`
class PureFunctionLocalizer : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};

///@brief 在同一 class 内内联已标记为纯函数的 static 方法。
///
///@par 输入与不支持范围
///仅接受无 try/catch、跳转、switch、throw、monitor 和多出口的简单方法体；递归、栈形状
///不匹配、标记缺失或分析失败时跳过。所有替换必须使用新节点，不能复用被调方法节点。
///
///@par 结果与验证
///成功变更返回 `none`，否则返回 `all`，并使方法体、调用图和帧分析失效。需要多出口
///反例、异常控制流、roundtrip 和 JVM 执行 fixture。当前为 `opt-in`。
///
///@par 标签
///- `scope:global`
///- `action:inline`
///- `stability:opt-in`
class LocalPureFunctionInliner : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};

///@}

}   //namespace mjt::base
