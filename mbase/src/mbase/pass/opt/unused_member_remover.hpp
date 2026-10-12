#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

///@defgroup unused_member_cleanup 未使用成员清理家族
///
///本家族把「可达性 / 引用索引分析证明不可达」的类、字段、方法转换为字节码删除。
///五个 pass 按作用域和分析来源分层：
///
///- `EmptyClassRemover`：最朴素，只看单个 class 是否无字段无方法，不做可达性分析。
///- `UnusedMethodRemover`：class-local，扫描类内直接调用和 Handle 引用，删除 private static
///方法。不依赖全局分析。
///- `UnusedClassRemover` / `UnusedFieldRemover` / `GlobalUnusedMethodRemover`：global，
///消费 `ResolvedGlobalReachabilityAnalysis`，删除不可达的 class / private 字段 /
///private 方法。构造器、`<clinit>`、非 private 成员、keep metadata 和反射敏感成员均保留。
///
///@par 共享策略
///三个 global pass 共享 `GlobalAnalysisManager` 提供的 `ResolvedGlobalReachabilityAnalysis`。
///默认根策略是 Java `public static main(String[])` 加上类初始化器保留；方法体展开使用
///`ResolvedCallGraph`，因此会在删除前保留虚调用和接口调用的覆写目标。若未发现任何可达类，
///pass 原样保留输入。这样可以在 CLI keep 规则和库入口策略可配置之前，避免误删库型 JAR。
///畸形 class 保留，非 class 资源保留。只有在 class 资源成功解析且至少删除了一个
///符合条件的成员时，pass 才会重写该资源。
///
///@par 验证
///测试在内存中构造 class，并通过注册了 `ReferenceIndexAnalysis`、`CallGraphAnalysis`
///和 `GlobalReachabilityAnalysis` 的 `GlobalAnalysisManager` 运行这些 pass。
///覆盖范围包括：
///- 删除不可达类，同时保留 main 根、直接被调用的方法和非 class 资源；
///- 没有默认根的库型容器保持不变；
///- 仅通过解析后的虚调用到达的子类覆写目标仍然保留；
///- 只删除不可达 private 字段，同时保留被引用的 private 字段和 public 字段；
///- 只删除不可达 private 方法，同时保留被引用的 private 方法、构造器策略和 Java main 根；
///- 空类删除保留非 class 资源。
///
///@par 发布边界
///上述 pass 当前均为 `opt-in`。它们依赖可达性、引用和调用图分析，且尚未覆盖反射、
///服务加载、外部入口和所有成员元数据策略；在 keep 规则和资源写回事务边界固定前，
///不得计入默认优化能力。
///@{

namespace mjt::base {

///@brief 删除没有字段和方法的空 class 资源。
///
///@par 输入与不支持范围
///只接受能够完整解析的 `ClassResource`；非 class 资源、畸形字节和带有字段/方法的类保持不变。
///不执行可达性分析，也不推断反射、服务加载或外部入口。
///
///@par 结果与失效
///仅在确实删除资源时返回 `PreservedAnalyses::none`；没有删除或解析失败返回 `all`。
///删除资源会使全局可达性、引用索引和资源列表失效。
///
///@par 所有权、验证与发布状态
///由 `ResourceContainer` 负责被删除资源的所有权；遍历期间不得保存悬空指针。
///需要 reader/writer roundtrip 和保留非 class 资源的 fixture 验证。当前为 `opt-in`。
///
///@par 标签
///- `scope:global`
///- `action:eliminate`
///- `stability:opt-in`
class EmptyClassRemover : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};

///@brief 根据全局可达性删除不可达 class 资源。
///
///解析每个 `ClassResource`，取得声明的内部类名。当该名称不在可达类闭包中时
///删除资源。比较的是 classfile 头中的声明名，而不是资源路径，因为 JAR 条目
///可能被重命名、重定位，或与 classfile 头彼此不一致。
///
///@par 输入与不支持范围
///使用默认 `main` 根以及 module、服务文件和 Manifest 入口建立可达闭包。所需分析未注册时
///抛出 `std::logic_error`；根集合为空、类解析失败、非 class 资源和不能精确解析的反射引用
///均保持原样。
///
///@par 结果、失效与所有权
///只删除明确位于可达闭包之外的 class，并在发生删除时返回 `none`，否则返回 `all`。
///资源删除使全局可达性、引用索引和后续 class analysis 失效；容器拥有资源，pass 不得
///保存删除对象的指针。
///
///所有 class 解析完成后才提交删除；任一解析失败均保持整个容器不变。
///
///@par 标签
///- `scope:global`
///- `action:eliminate`
///- `stability:opt-in`
class UnusedClassRemover : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};

///@brief 从可达类中删除明确不可达的 private 字段。
///
///重写可达 owner class，并只删除不在可达字段闭包中的 private 字段。
///即使当前扫描的字节码没有引用，public 和 protected 字段也会保留。这样可以在
///keep 规则落地之前，避免破坏公共 API、反射和框架注入。
///
///@par 输入与不支持范围
///依赖全局可达性和成员引用索引，只改写可解析且 owner 可达的 class。public/protected、
///带注解或可能被反射访问的字段，以及分析不完整的字段必须保留。
///
///@par 结果、失效与失败处理
///发生字段删除返回 `none`，否则返回 `all`；类结构、引用索引、字段/帧分析均失效。
///带 signature/type annotation/annotation 的字段和未解析反射目标必须 retain。所有 class
///先完成解析和序列化，再一次提交；解析或写回失败时保留全部原始 class bytes，不得部分
///修改。所需分析未注册时抛出 `std::logic_error`。
///
///@par 标签
///- `scope:global`
///- `action:eliminate`
///- `stability:opt-in`
class UnusedFieldRemover : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};

///@brief 在引用索引可证明安全时移除类内未使用的 private static 方法。
///
///@par 输入与不支持范围
///只候选 private static 且非构造器/`<clinit>` 的方法；直接调用、LDC 方法句柄和
///`INVOKEDYNAMIC` 的 bootstrap/参数句柄均视为引用。ConstantDynamic 的未知递归参数、
///反射/外部类资源和不完整分析均保守保留，不宣称完成全局可达性证明。
///
///@par 结果、失效与所有权
///只删除没有被上述引用命中的候选；删除的 `MethodNode` 从 `ClassNode` 列表移除后由当前
///pass 释放。发生删除返回 `none`，无删除返回 `all`；类成员、引用索引和方法分析失效。
///
///@par 验证与发布状态
///需要直接调用、LDC Handle、`INVOKEDYNAMIC`、ConstantDynamic、入口方法和 roundtrip/JVM
///fixture。当前为 opt-in 的类内保守迁移，不替代 `GlobalUnusedMethodRemover`。
///
///@par 标签
///- `scope:class`
///- `action:eliminate`
///- `stability:opt-in`
class UnusedMethodRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode,
                          ClassAnalysisManager&       cam) override;
};

///@brief 使用全局可达性删除不可达的 private 方法。
///
///是旧的类内 `UnusedMethodRemover` 的全局对应实现。重写可达 owner class，并删除
///不在可达方法闭包中的 private 方法。构造器和类初始化器始终保留。为了兼容 API、
///覆写、反射和框架约定，keep 规则与层次感知调用图完成前，非 private 方法也会保留。
///
///@par 输入与不支持范围
///只处理可解析且 owner 可达的 class；构造器、`<clinit>`、非 private 方法、入口根、
///反射/服务约定方法和分析不完整的方法必须保留。
///
///@par 结果、失效与失败处理
///删除至少一个方法返回 `none`，否则返回 `all`；调用图、引用索引、方法/帧分析失效。
///MethodHandle、invokedynamic、已知或未解析反射成员、signature/annotation、native/abstract
///方法均保守保留。所有 class 先完成解析和序列化，再一次提交；写回失败时恢复原 class，
///容器继续拥有全部资源。所需分析未注册时抛出 `std::logic_error`。
///
///@par 标签
///- `scope:global`
///- `action:eliminate`
///- `stability:opt-in`
class GlobalUnusedMethodRemover : public AsmGlobalPass
{
public:
    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;
};

///@}

}   //namespace mjt::base
