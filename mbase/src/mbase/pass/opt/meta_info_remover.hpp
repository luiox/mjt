#pragma once

#include "jbytecode/jbytecode.hpp"
#include "mbase/spi/asm_pass.hpp"

namespace mjt::base {

///@brief 清除 class、field、method 中列明的非语义 metadata。
///
///@par 中文契约
///输入是 class-level tree；只删除已列明的 debug/framework metadata，未知属性、指令、
///frame、签名、module、nestmate 和 record 结构保持不变。任一 metadata 删除返回 `none`，
///否则 `all`；annotation/metadata 节点由本 pass 删除并释放。该破坏性 pass 仅 opt-in，
///必须通过 writer roundtrip 和用户可接受的 metadata fixture。
///
///@par 标签
///- `scope:class`
///- `action:eliminate`
///- `stability:opt-in`
class MetaInfoRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 移除字段的 `ACC_FINAL`。
///输入为可写 ClassNode；只改访问标志，不处理 final 语义、继承或反射。变更返回 `none`，
///否则 `all`；类由调用方拥有。当前为 metadata opt-in，需 writer roundtrip/verifier fixture。
///
///@par 标签
///- `scope:class`
///- `action:eliminate`
///- `stability:opt-in`
class FinalRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 移除 class/method/field 的 synthetic 及 method 的 bridge 标志。
///只改访问 metadata，不推断编译器语义；变更返回 `none`，否则 `all`。当前为破坏性
///metadata opt-in，需要反射/bridge roundtrip fixture。
///
///@par 标签
///- `scope:class`
///- `action:eliminate`
///- `stability:opt-in`
class SyntheticBridgeRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 移除 `@kotlin/Metadata` 及 jetbrains 注解。
///只按固定 descriptor 删除 annotation，解析失败保持原样；变更返回 `none`，否则 `all`。
///当前为破坏性 metadata opt-in，需 annotation roundtrip fixture。
///
///@par 标签
///- `scope:class`
///- `action:eliminate`
///- `stability:opt-in`
class KotlinMetadataRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 清除 `sourceFile/sourceDebug`。
///只改 class metadata，不改指令和 frame；变更返回 `none`，否则 `all`。当前为 metadata
///opt-in，需合法/缺失 source 的 roundtrip fixture。
///
///@par 标签
///- `scope:class`
///- `action:eliminate`
///- `stability:opt-in`
class SourceInfoRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 清除 class/method/field 的 signature metadata。
///这是破坏性操作，不修改 descriptor 或指令；变更返回 `none`，否则 `all`。当前为
///metadata opt-in，不进入默认链，需 generic/roundtrip fixture。
///
///@par 标签
///- `scope:class`
///- `action:eliminate`
///- `stability:opt-in`
class SignatureRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 清理 class、method、field metadata 中格式非法的 annotation。
///
///只校验 descriptor 形状，不执行框架黑名单；解析失败或无法定位 annotation 时保持原样。
///删除返回 `none`，否则 `all`；当前为 clean/deobfuscate preflight opt-in，不进入默认链，
///需要 malformed descriptor 和 writer roundtrip fixture。
///
///@par 标签
///- `scope:class`
///- `action:sanitize`
///- `stability:opt-in`
class IllegalAnnotationRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 清理 class、method、field 中解析器拒绝的非法 generic signature。
///
///只删除 parser 明确拒绝的签名，合法签名保持不变；解析器异常、资源不完整时保持原样。
///删除返回 `none`，否则 `all`；当前为 clean/deobfuscate preflight opt-in，不进入默认链，
///需要非法/合法签名和 writer roundtrip fixture。
///
///@par 标签
///- `scope:class`
///- `action:sanitize`
///- `stability:opt-in`
class IllegalSignatureRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 清理 class/method 中格式非法的 type annotation。
///
///只处理当前 C++ tree 暴露的 annotation vector，FieldNode 未暴露的范围明确不支持；非法
///descriptor 删除返回 `none`，否则 `all`。当前为 clean/deobfuscate preflight opt-in，
///需 malformed descriptor 和 roundtrip fixture。
///
///@par 标签
///- `scope:class`
///- `action:sanitize`
///- `stability:opt-in`
class IllegalTypeAnnotationRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 清除格式非法的 `sourceFile`/`sourceDebug` metadata。
///
///只删除过长、非法文件名或不符合既定字符规则的值；合法 source、行号和局部变量表保持。
///变更返回 `none`，否则 `all`；当前为 clean/deobfuscate preflight opt-in，需要合法/非法
///名称和 writer roundtrip fixture。
///
///@par 标签
///- `scope:class`
///- `action:sanitize`
///- `stability:opt-in`
class IllegalSourceInfoRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 清理 class/method/field metadata 中被标记为 unknown 的 raw attribute。
///
///只删除 `Attribute::isUnknown` 为真的节点，已知属性、指令和访问标志保持。删除返回
///`none`，否则 `all`；当前为 clean/deobfuscate preflight opt-in，需要 unknown/known
///attribute、roundtrip 和 writer fixture。
///
///@par 标签
///- `scope:class`
///- `action:sanitize`
///- `stability:opt-in`
class UnknownAttributeCleaner : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 清除 class、method、field 和 parameter 上明确非法的访问标志。
///
///@par 中文契约
///输入是可写的 `ClassNode`；只清除当前成员种类明确非法的 bit，不推断可见性、不改模块
///或接口语义。实际 bit 发生变化返回 `none`，无变化返回 `all`；类/成员对象由调用方
///所有，本 pass 不释放节点。class writer roundtrip 和 verifier 是 opt-in 发布证据，当前
///不进入 `default-optimize`。
///
///@par 标签
///- `scope:class`
///- `action:sanitize`
///- `stability:opt-in`
class IllegalAccessRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 移除指定描述符或格式非法的注解。
///
///@par 中文契约
///输入是 class/field/method/parameter annotation 可遍历的 `ClassNode`；只按固定 descriptor
///黑名单和非法 descriptor 规则删除，不做框架推断。发生删除返回 `none`，无变化返回
///`all`；annotation 节点由本 pass 释放。该清理保持 opt-in，需 roundtrip 和 metadata fixture。
///
///@par 标签
///- `scope:class`
///- `action:eliminate`
///- `stability:opt-in`
class SpecificAnnotationRemover : public AsmClassPass
{
public:
    PreservedAnalyses run(jbytecode::tree::ClassNode& classNode, ClassAnalysisManager& am) override;
};

///@brief 删除方法内所有 `LineNumberNode`。
///只移除调试节点，不改变控制流；变更返回 `none`，否则 `all`，节点由 MethodNode 管理。
///当前为 metadata opt-in，需 writer roundtrip fixture。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class LineNumberRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 清空方法 `localVariables`。
///不修改 slot 指令，只删除调试表；变更返回 `none`，否则 `all`。当前为 metadata opt-in。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class LocalVariableInfoRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 清空方法 parameters metadata。
///不修改 descriptor 或参数指令；变更返回 `none`，否则 `all`。当前为 metadata opt-in。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class ParametersInfoRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 清空方法声明的 exceptions metadata。
///不修改 try-catch；变更返回 `none`，否则 `all`。当前为破坏性 metadata opt-in，需要
///checked-exception roundtrip/JVM fixture。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:opt-in`
class ExceptionInfoRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 将超过 64 字符的 indy 名缩减为稳定的 `indyMethod`+hash。
///只处理可解析且 hash 不冲突的名称；冲突或 writer 失败保持原样。变更返回 `none`，否则
///`all`；当前为 metadata opt-in，需要 bootstrap/roundtrip fixture。
///
///@par 标签
///- `scope:method`
///- `action:rewrite`
///- `stability:opt-in`
class LongIndyNameOptimizer : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 删除方法内所有 `FrameNode`（StackMapTable metadata）。
///仅处理可重新计算 frame 的方法；frame 删除会使 method/frame analysis 失效并返回 `none`。
///当前为高风险 metadata opt-in，必须有 verifier/JVM fixture，不进入默认链。
///
///@par 标签
///- `scope:method`
///- `action:eliminate`
///- `stability:unregistered`
class FrameInfoRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 当最后参数不是数组时清除 method 的 `ACC_VARARGS`。
///
///只清除明确不可能的标志，不推断或添加 varargs；描述符解析失败时保持原样。变更返回
///`none`，否则 `all`；当前为 clean/deobfuscate preflight opt-in，需要 descriptor、
///writer roundtrip 和 verifier fixture。
///
///@par 标签
///- `scope:method`
///- `action:sanitize`
///- `stability:opt-in`
class IllegalVarargsRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

///@brief 删除名称首字节为非 ASCII 的局部变量表项。
///
///只过滤 local variable metadata，不修改 load/store 指令；名称读取失败时保持原条目。
///删除返回 `none`，否则 `all`；当前为 clean/deobfuscate preflight opt-in，需要合法/非法
///名称和 writer roundtrip fixture。
///
///@par 标签
///- `scope:method`
///- `action:sanitize`
///- `stability:opt-in`
class IllegalLocalVariableInfoRemover : public AsmMethodPass
{
public:
    PreservedAnalyses run(jbytecode::tree::MethodNode& methodNode,
                          MethodAnalysisManager&       am) override;
};

}   //namespace mjt::base
