#pragma once

///@defgroup small_inliner 小型方法内联器模块
///
///## 目的
///
///P3.4 小型内联器系列迁移首批低风险结构内联器，而不打开完整的签名重映射问题。
///这些通行证仅重写调用点；它们保留被调方法在原位，因此公共 API 形状、反射
///和映射输出保持不变。
///
///## 通行证
///
///- `GetterInliner` 识别方法体恰好为 `getstatic; return` 或 `aload_0; getfield; return`
///的方法。调用被替换为匹配的字段读取，实例 getter 保留已在栈上的原始接收者。
///- `ShortMethodInliner` 识别无参 `const; return` 静态方法。调用被替换为克隆的常量指令。
///- `ArgForwardMethodInliner` 识别按顺序加载每个参数、用相同描述符调用另一方法并返回结果的
///静态方法。调用被重写为转发的目标。
///
///## 边界
///
///实现忽略含有分支、携带真实控制流的标签、try-catch 块、存储、分配、字段写入、
///invokedynamic 或描述符变更的方法。它不移除已内联的方法。一旦 keep 规则和
///重映射策略可用，后续阶段可将这些通行证与可达性清理结合。

#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 通用方法内联的保守阈值与功能开关。
struct MethodInlinerOptions
{
    ///@brief 普通候选方法允许的最大真实指令数。
    ca::i32 maxInlineInstructions = 64;

    ///@brief 单个 caller 内联后的最大指令数。
    ca::i32 maxResultingInstructions = 10000;

    ///@brief 是否允许忽略普通长度阈值，内联仅有一个调用点的方法。
    bool inlineSingleInvocation = true;

    ///@brief 是否允许满足访问和类初始化约束的跨类内联。
    bool allowCrossClass = true;
};

///@brief 将可证明调用目标唯一的方法体内联到项目内调用点。
///
///@par 输入契约
///输入必须是包含完整 class/method 资源的 `ResourceContainer`，并且
///`GlobalAnalysisManager` 能提供调用图、引用索引和类层次信息。只处理可解析的 JVM
///方法描述符；分析缺失或资源索引不一致时保持容器原样。
///
///@par 不支持范围
///构造器、同步方法、JSR/RET、递归调用链、动态派发目标不唯一，以及会产生不安全跨类
///成员访问的候选必须拒绝。超过阈值或无法重写异常表/栈映射时也必须跳过，不得部分写回。
///
///@par 变换与结果
///支持静态方法，以及 private、final 或 final class 上不会发生动态派发的实例方法。
///内联过程会保存参数和 receiver、重映射局部变量与全部控制流标签、复制异常表，
///并把多个 return 改写到统一出口。
///发生至少一次完整内联时返回 `PreservedAnalyses::none`；没有候选、候选全部拒绝或
///分析失败时返回 `PreservedAnalyses::all`。调用图、引用索引、帧和方法体分析均视为失效。
///
///@par 所有权与异常
///新的指令、标签和异常表节点由目标方法拥有；候选方法只读，不把其节点指针直接挂到
///caller。单个候选重写失败时丢弃临时副本并保留原 caller，禁止留下半写入资源。
///
///@par 验证与发布状态
///需要使用 writer/reader roundtrip、JVM 执行 fixture 和异常控制流 fixture 验证；当前
///为 `opt-in`，不进入 `default-optimize`。该实现用于验证迁移忠实度，不能仅以成功内联
///数量作为完成标准。
///
///@par 标签
///- `scope:global`
///- `action:inline`
///- `stability:opt-in`
class MethodInliner final : public AsmGlobalPass
{
public:
    MethodInliner() = default;

    explicit MethodInliner(MethodInlinerOptions options)
        : options_(options)
    {}

    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;

private:
    MethodInlinerOptions options_;
};

///@brief 在项目内调用点保守内联简单字段 getter。
///
///@par 输入与不支持范围
///只接受完整的 global class 资源和唯一的静态/实例 getter 形状：`getstatic; return` 或
///`aload_0; getfield; return`。重载不匹配、访问权限不满足、异常控制流、跨模块调用和
///分析缺失时跳过并保留原调用。
///
///@par 结果与失效
///调用点全部成功替换才返回 `none`，否则返回 `all`；方法体和调用图分析失效。
///被调方法保留，避免改变外部兼容性；临时指令失败时不写回 caller。
///
///@par 验证与发布状态
///需要静态/实例 getter、重载、访问边界和 JVM roundtrip fixture。当前为 `opt-in`。
///
///@par 标签
///- `scope:global`
///- `action:inline`
///- `stability:opt-in`
class GetterInliner : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};

///@brief 在调用点内联只返回一个常量的无参 static 方法。
///
///@par 输入与不支持范围
///仅接受描述符为 `T` 且方法体严格为 `const; xreturn` 的候选；异常表、分支、调用、
///未知 opcode、类型不匹配或分析失败时保持调用原样。
///
///@par 结果、所有权与验证
///使用新分配或 clone 的 producer 节点替换调用，原方法保留。至少一次完整替换返回 `none`，
///否则返回 `all`；方法体、调用图和帧分析失效。需要常量类型、异常输入、roundtrip/JVM
///fixture。当前为 `opt-in`。
///
///@par 标签
///- `scope:global`
///- `action:inline`
///- `stability:opt-in`
class ShortMethodInliner : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};

///@brief 内联签名相同且只转发参数的 static 方法。
///
///@par 输入与不支持范围
///候选必须按顺序加载全部参数、调用同描述符目标并立即返回；构造器、额外副作用、
///分支/异常表、递归、目标不唯一和分析失败均不支持。
///
///@par 结果与验证
///只替换栈形状和签名保持不变的调用点；成功变更返回 `none`，否则返回 `all`，并使
///调用图、引用索引和帧分析失效。需要重载、递归、异常和 JVM roundtrip fixture。当前为 `opt-in`。
///
///@par 标签
///- `scope:global`
///- `action:inline`
///- `stability:opt-in`
class ArgForwardMethodInliner : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};

}   //namespace mjt::base
