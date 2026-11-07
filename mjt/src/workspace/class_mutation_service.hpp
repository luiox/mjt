#pragma once

#include <string>
#include <vector>

#include "libca/str/utf8_string.hpp"

#include "libca/core/datatype.hpp"

#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "workspace_types.hpp"

namespace mjt::workspace {

/// @file class_mutation_service.hpp
/// @brief 结构化 class 修改服务（事务与校验语义，指令载体按 libjbc 定型）。

/// @brief 深拷贝元素型列表（Utf8String 与各 Spec 均提供 clone()）。
template <typename T>
std::vector<T> cloneList(const std::vector<T>& values)
{
    std::vector<T> result;
    result.reserve(values.size());
    for (const T& value : values) result.push_back(value.clone());
    return result;
}

/// @brief 结构化 class 修改的目标成员标识。
struct MemberSelector final
{
    ca::str::Utf8String name;
    ca::str::Utf8String descriptor;

    /// @brief 深拷贝（Utf8String 禁拷贝；按值持有/改写 DTO 的消费点走 clone 族）。
    MemberSelector clone() const { return {name.clone(), descriptor.clone()}; }
};

/// @brief 可由结构化编辑接口构造的常用 JVM 指令种类。
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

/// @brief JVM method handle 的拥有型请求表示。
struct HandleSpec final
{
    ca::i32             tag = 0;
    ca::str::Utf8String owner;
    ca::str::Utf8String name;
    ca::str::Utf8String descriptor;
    bool                interfaceReference = false;

    /// @brief 深拷贝（同 MemberSelector）。
    HandleSpec clone() const
    {
        return {tag, owner.clone(), name.clone(), descriptor.clone(), interfaceReference};
    }
};

/// @brief LDC、invokedynamic 和 condy bootstrap 参数支持的常量种类。
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

/// @brief 不持有 IR 句柄的 JVM 常量请求。
struct ConstantSpec final
{
    ConstantKind              kind = ConstantKind::String;
    ca::str::Utf8String       stringValue;
    ca::i32                   intValue    = 0;
    ca::i64                   longValue   = 0;
    float                     floatValue  = 0.0F;
    double                    doubleValue = 0.0;
    ca::str::Utf8String       descriptor;
    ca::str::Utf8String       name;
    HandleSpec                handle;
    HandleSpec                bootstrapMethod;
    std::vector<ConstantSpec> bootstrapArguments;

    /// @brief 深拷贝（bootstrapArguments 递归走 cloneList）。
    ConstantSpec clone() const
    {
        return {kind,
                stringValue.clone(),
                intValue,
                longValue,
                floatValue,
                doubleValue,
                descriptor.clone(),
                name.clone(),
                handle.clone(),
                bootstrapMethod.clone(),
                cloneList(bootstrapArguments)};
    }
};

/// @brief 单条结构化字节码指令；字段按 @p kind 解释。
///
/// 索引语义（类型面定型）：`instructionIndex`/label 绑定的
/// instructionIndex 均为方法体内**代码指令序号**（CodeDom 程序顺序，不含
/// label/line 节点——mjbase 底座中标签是句柄不是指令）。
struct InstructionSpec final
{
    InstructionKind           kind                = InstructionKind::Simple;
    ca::i32                   opcode              = -1;
    ca::i32                   operand             = 0;
    ca::i32                   increment           = 0;
    bool                      interfaceInvocation = false;
    ca::str::Utf8String       owner;
    ca::str::Utf8String       name;
    ca::str::Utf8String       descriptor;
    ca::str::Utf8String       stringValue;
    ca::i64                   longValue   = 0;
    float                     floatValue  = 0.0F;
    double                    doubleValue = 0.0;
    ca::str::Utf8String       label;
    ca::str::Utf8String       defaultLabel;
    std::vector<ca::str::Utf8String> labels;
    std::vector<ca::i32>      keys;
    ca::i32                   minimum    = 0;
    ca::i32                   maximum    = -1;
    ca::i32                   dimensions = 0;
    ca::i32                   line       = 0;
    HandleSpec                handle;
    HandleSpec                bootstrapMethod;
    std::vector<ConstantSpec> bootstrapArguments;

    /// @brief 深拷贝（嵌套 handle/bootstrap 子树走各自 clone）。
    InstructionSpec clone() const
    {
        return {kind,
                opcode,
                operand,
                increment,
                interfaceInvocation,
                owner.clone(),
                name.clone(),
                descriptor.clone(),
                stringValue.clone(),
                longValue,
                floatValue,
                doubleValue,
                label.clone(),
                defaultLabel.clone(),
                cloneList(labels),
                keys,
                minimum,
                maximum,
                dimensions,
                line,
                handle.clone(),
                bootstrapMethod.clone(),
                cloneList(bootstrapArguments)};
    }
};

/// @brief 以代码指令序号或符号 label 描述的 try/catch 块边界。
struct TryCatchSpec final
{
    ca::i32             startInstruction   = -1;
    ca::i32             endInstruction     = -1;
    ca::i32             handlerInstruction = -1;
    ca::str::Utf8String type;
    ca::str::Utf8String startLabel;
    ca::str::Utf8String endLabel;
    ca::str::Utf8String handlerLabel;

    /// @brief 深拷贝（同 MemberSelector）。
    TryCatchSpec clone() const
    {
        return {startInstruction,
                endInstruction,
                handlerInstruction,
                type.clone(),
                startLabel.clone(),
                endLabel.clone(),
                handlerLabel.clone()};
    }
};

/// @brief 将请求内符号 label 绑定到现有方法的代码指令序号（指令前置边界）。
struct LabelBindingSpec final
{
    ca::str::Utf8String id;
    ca::i32             instructionIndex = -1;

    /// @brief 深拷贝（同 MemberSelector）。
    LabelBindingSpec clone() const { return {id.clone(), instructionIndex}; }
};

/// @brief 整方法体替换时重建的局部变量调试信息。
struct LocalVariableSpec final
{
    ca::str::Utf8String name;
    ca::str::Utf8String descriptor;
    ca::str::Utf8String signature;
    ca::str::Utf8String startLabel;
    ca::str::Utf8String endLabel;
    ca::i32             index = -1;

    /// @brief 深拷贝（同 MemberSelector）。
    LocalVariableSpec clone() const
    {
        return {name.clone(),
                descriptor.clone(),
                signature.clone(),
                startLabel.clone(),
                endLabel.clone(),
                index};
    }
};

/// @brief ClassMutationService 支持的便利编辑操作。
enum class ClassMutationKind : ca::u8
{
    SetClassAccess,
    ChangeClassAccess,   ///< 增量访问位编辑（set/clear 名单，未提及位保留）
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

/// @brief 结构化 annotation 修改的附着位置。
enum class AnnotationTarget : ca::u8
{
    Class,
    Field,
    Method,
};

/// @brief 一次只修改一个 class 的结构化变更请求。
///
/// `classUri` 是工作区 class 资源 URI；涉及成员的操作使用 `member` 精确消除重载歧义。
/// AddMethod 的非 abstract/native 方法必须提供能独立通过验证的 `initialInstructions`。
///
/// 访问位编辑的语义分界（版本兼容契约）：
/// - `kind = SetClassAccess` + `access`（旧字段，保留）：整体重建 access 位——
///   旧工程/旧调用点的既有语义照旧重放；access 里的 0x20000 是 Deprecated
///   属性不是 flag，映射为属性追加（历史版本静默丢弃，禁止再丢）。
/// - `kind = ChangeClassAccess` + `setFlags`/`clearFlags`（新 op，新语义）：
///   增量编辑——set 置位、clear 清位、未提及位一律保留（ACC_SUPER 等
///   不可表达 flag 不再丢失）；"deprecated" 名字映射为 Deprecated 属性增删。
///   未知 flag 名报错并列出 class 目标合法 flag 全集。
struct ClassMutation final
{
    ca::str::Utf8String            classUri;
    ClassMutationKind              kind = ClassMutationKind::SetClassAccess;
    MemberSelector                 member;
    ca::i32                        access           = 0;
    /// @brief ChangeClassAccess：置位 flag 名单（JVM 小写助记，如 "public"）。
    std::vector<ca::str::Utf8String> setFlags;
    /// @brief ChangeClassAccess：清位 flag 名单；两组都未提及的位保留原值。
    std::vector<ca::str::Utf8String> clearFlags;
    ca::i32                        instructionIndex = -1;
    bool                           insertAfter      = false;
    ca::i32                        localIndex       = -1;
    ca::str::Utf8String            localName;
    ca::str::Utf8String            replacementLocalName;
    AnnotationTarget               annotationTarget  = AnnotationTarget::Class;
    bool                           annotationVisible = true;
    ca::str::Utf8String            annotationDescriptor;
    InstructionSpec                instruction;
    ca::str::Utf8String            signature;
    std::vector<ca::str::Utf8String> exceptions;
    bool                           hasFieldValue = false;
    ConstantSpec                   fieldValue;
    std::vector<LabelBindingSpec>  existingLabels;
    std::vector<InstructionSpec>   initialInstructions;
    std::vector<TryCatchSpec>      tryCatchBlocks;
    std::vector<LocalVariableSpec> localVariables;

    /// @brief 深拷贝（候选事务要对请求改写字段时的复制面；逐字段 clone）。
    ClassMutation clone() const
    {
        return {classUri.clone(),
                kind,
                member.clone(),
                access,
                cloneList(setFlags),
                cloneList(clearFlags),
                instructionIndex,
                insertAfter,
                localIndex,
                localName.clone(),
                replacementLocalName.clone(),
                annotationTarget,
                annotationVisible,
                annotationDescriptor.clone(),
                instruction.clone(),
                signature.clone(),
                cloneList(exceptions),
                hasFieldValue,
                fieldValue.clone(),
                cloneList(existingLabels),
                cloneList(initialInstructions),
                cloneList(tryCatchBlocks),
                cloneList(localVariables)};
    }
};

/// @brief 候选 class 修改的底层结果；Workspace 将其包装为统一 MutationResult。
struct ClassMutationOutcome final
{
    WorkspaceErrorInfo            status;
    std::vector<jasm::Diagnostic> diagnostics;
};

/// @brief 在一个候选 ResourceContainer 中执行完整 Jasm 或结构化 class 修改。
///
/// 本服务不持有 revision、历史或原始快照。调用方必须在隔离的候选容器上使用它，并仅在
/// `status` 成功后提交候选容器。完整 Jasm 修改委托 ResourceJasmService；结构化修改
/// 按libjbc 会话改写（骨架元数据直改 backing，方法体经 promote/CodeDom/lower 或
/// 整体重建），产物统一经 spi::sanitizeClassFile 与 Jasm 验证后再写回。
///
/// COMPUTE_FRAMES 式 StackMapTable 重算在 libjbc 写回面无对应物（pass-contract §9），
/// 布局改变的产物沿用原属性的 relocation 语义（lower 链路），整方法重建的产物
/// 不携带重建的 StackMapTable。
///
/// 与 mbase 版的差异：构造不再挂 GlobalAnalysisManager——mjbase ResourceJasmService
/// 每次按容器即时重建层次图，服务不持任何跨调用缓存。
class ClassMutationService final
{
public:
    explicit ClassMutationService(model::ResourceContainer& container)
        : container_(&container)
    {}

    /// @brief 汇编完整 Jasm 并事务替换候选容器中的目标 class。
    ///        validationMode 同时控制重建管线自带的字节码校验门：off 时重建
    ///        后不再做 validate（汇编本身仍必须成功）。
    jasm::Result<jasm::JvmJasmClass> applyJasm(const ca::str::Utf8StringRef& uri,
                                               ca::str::Utf16StringRef       sourceName,
                                               ca::str::Utf16StringRef       source,
                                               jasm::ValidationMode          validationMode
                                                   = jasm::ValidationMode::Relaxed);

    /// @brief 应用一项结构化修改；失败时不会替换目标 ClassResource 的字节。
    ///        validationMode 控制写回后的整类校验门（off 同样跳过）。分档
    ///        （#73）：不触碰方法体的旗标/成员表面编辑（访问位、成员表增删、
    ///        注解、局部名）跳过整类门——全类校验会被类内先在坏帧无差别拒掉；
    ///        触碰方法体的 mutation（换体/指令编辑/新建方法体）档位照常生效，
    ///        与 jasm 重建路径同档位语义。门失败时同档位重验先在字节，先在
    ///        字节同样不过门则错误信息注明归因（先在问题，非本次编辑引入）。
    ClassMutationOutcome apply(const ClassMutation& mutation,
                               jasm::ValidationMode validationMode
                                   = jasm::ValidationMode::Relaxed);

private:
    model::ResourceContainer* container_;
};

}   // namespace mjt::workspace
