#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/status.hpp"
#include "mbase/jasm/jvm_jasm_service.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {
class ResourceContainer;
}

namespace mjt::base::workspace {

///@brief 结构化 class 修改的目标成员标识。
struct MemberSelector final
{
    std::string name;
    std::string descriptor;
};

///@brief 可由结构化编辑接口构造的常用 JVM 指令种类。
enum class InstructionKind : ca::u8
{
    Simple,
    Int,
    Var,
    Type,
    Field,
    Method,
    Jump,
    Label,
    LdcString,
    LdcInt,
    LdcLong,
    LdcFloat,
    LdcDouble,
    LdcType,
    LdcHandle,
    LdcDynamic,
    InvokeDynamic,
    Iinc,
    TableSwitch,
    LookupSwitch,
    MultiANewArray,
    Line,
};

///@brief JVM method handle 的拥有型请求表示。
struct HandleSpec final
{
    ca::i32     tag = 0;
    std::string owner;
    std::string name;
    std::string descriptor;
    bool        interfaceReference = false;
};

///@brief LDC、invokedynamic 和 condy bootstrap 参数支持的常量种类。
enum class ConstantKind : ca::u8
{
    String,
    Integer,
    Long,
    Float,
    Double,
    Type,
    Handle,
    Dynamic,
};

///@brief 不持有 AST 指针的 JVM 常量请求。
struct ConstantSpec final
{
    ConstantKind              kind = ConstantKind::String;
    std::string               stringValue;
    ca::i32                   intValue    = 0;
    ca::i64                   longValue   = 0;
    float                     floatValue  = 0.0F;
    double                    doubleValue = 0.0;
    std::string               descriptor;
    std::string               name;
    HandleSpec                handle;
    HandleSpec                bootstrapMethod;
    std::vector<ConstantSpec> bootstrapArguments;
};

///@brief 单条结构化字节码指令；字段按 @p kind 解释。
struct InstructionSpec final
{
    InstructionKind           kind                = InstructionKind::Simple;
    ca::i32                   opcode              = -1;
    ca::i32                   operand             = 0;
    ca::i32                   increment           = 0;
    bool                      interfaceInvocation = false;
    std::string               owner;
    std::string               name;
    std::string               descriptor;
    std::string               stringValue;
    ca::i64                   longValue   = 0;
    float                     floatValue  = 0.0F;
    double                    doubleValue = 0.0;
    std::string               label;
    std::string               defaultLabel;
    std::vector<std::string>  labels;
    std::vector<ca::i32>      keys;
    ca::i32                   minimum    = 0;
    ca::i32                   maximum    = -1;
    ca::i32                   dimensions = 0;
    ca::i32                   line       = 0;
    HandleSpec                handle;
    HandleSpec                bootstrapMethod;
    std::vector<ConstantSpec> bootstrapArguments;
};

///@brief 以现有 LabelNode 指令索引描述的 try/catch 块。
struct TryCatchSpec final
{
    ca::i32     startInstruction   = -1;
    ca::i32     endInstruction     = -1;
    ca::i32     handlerInstruction = -1;
    std::string type;
    std::string startLabel;
    std::string endLabel;
    std::string handlerLabel;
};

///@brief 将请求内符号 label 绑定到现有方法的 LabelNode 指令索引。
struct LabelBindingSpec final
{
    std::string id;
    ca::i32     instructionIndex = -1;
};

///@brief 整方法体替换时重建的局部变量调试信息。
struct LocalVariableSpec final
{
    std::string name;
    std::string descriptor;
    std::string signature;
    std::string startLabel;
    std::string endLabel;
    ca::i32     index = -1;
};

///@brief ClassMutationService 支持的便利编辑操作。
enum class ClassMutationKind : ca::u8
{
    SetClassAccess,
    SetFieldAccess,
    SetMethodAccess,
    AddField,
    RemoveField,
    AddMethod,
    RemoveMethod,
    AddMarkerAnnotation,
    RemoveAnnotation,
    RenameLocal,
    ReplaceTryCatchBlocks,
    ReplaceMethodBody,
    InsertInstruction,
    ReplaceInstruction,
    RemoveInstruction,
};

///@brief 结构化 annotation 修改的附着位置。
enum class AnnotationTarget : ca::u8
{
    Class,
    Field,
    Method,
};

///@brief 一次只修改一个 class 的结构化变更请求。
///
///`classUri` 是工作区 class 资源 URI；涉及成员的操作使用 `member` 精确消除重载歧义。
///AddMethod 的非 abstract/native 方法必须提供能独立通过验证的 `initialInstructions`。
struct ClassMutation final
{
    std::string                    classUri;
    ClassMutationKind              kind = ClassMutationKind::SetClassAccess;
    MemberSelector                 member;
    ca::i32                        access           = 0;
    ca::i32                        instructionIndex = -1;
    bool                           insertAfter      = false;
    ca::i32                        localIndex       = -1;
    std::string                    localName;
    std::string                    replacementLocalName;
    AnnotationTarget               annotationTarget  = AnnotationTarget::Class;
    bool                           annotationVisible = true;
    std::string                    annotationDescriptor;
    InstructionSpec                instruction;
    std::string                    signature;
    std::vector<std::string>       exceptions;
    bool                           hasFieldValue = false;
    ConstantSpec                   fieldValue;
    std::vector<LabelBindingSpec>  existingLabels;
    std::vector<InstructionSpec>   initialInstructions;
    std::vector<TryCatchSpec>      tryCatchBlocks;
    std::vector<LocalVariableSpec> localVariables;
};

///@brief 候选 class 修改的底层结果；Workspace 将其包装为统一 MutationResult。
struct ClassMutationOutcome final
{
    ca::Status                    status = ca::OkStatus();
    std::vector<jasm::Diagnostic> diagnostics;
};

///@brief 在一个候选 ResourceContainer 中执行完整 Jasm 或结构化 class 修改。
///
///本服务不持有 revision、历史或原始快照。调用方必须在隔离的候选容器上使用它，并仅在
///`status` 成功后提交候选容器。完整 Jasm 修改委托 ResourceJasmService；结构化修改委托
///InsnEditor 和 ClassWriter，统一使用 COMPUTE_MAXS | COMPUTE_FRAMES 后再经 Jasm 验证。
class ClassMutationService final
{
public:
    ClassMutationService(ResourceContainer& container, GlobalAnalysisManager& analysisManager)
        : container_(&container)
        , analysisManager_(&analysisManager)
    {}

    ///@brief 汇编完整 Jasm 并事务替换候选容器中的目标 class。
    jasm::Result<jasm::JvmJasmClass> applyJasm(const std::string&      uri,
                                               ca::str::Utf16StringRef sourceName,
                                               ca::str::Utf16StringRef source);

    ///@brief 应用一项结构化修改；失败时不会替换目标 ClassResource 的字节。
    ClassMutationOutcome apply(const ClassMutation& mutation);

private:
    ResourceContainer*     container_;
    GlobalAnalysisManager* analysisManager_;
};

}   //namespace mjt::base::workspace
