#pragma once

#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/status.hpp"
#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mbase/jasm/jvm_jasm_service.hpp"
#include "mbase/asm/remapper/resource_remapper.hpp"

///@file workspace_types.hpp
///@brief Workspace 接口层共享的数据传输类型（DTO）。
///
///@details
///无业务语义的纯数据结构 + 枚举 + 常量：IWorkspace 接口签名、WorkspaceBase
///实现与各产品 Workspace 共同使用。项目特有类型放项目内，
///不进本头。

namespace mjt::base::workspace {
///@brief 工作区当前承载的主制品种类。
enum class ArtifactKind : ca::u8
{
    None,
    Class,
    Jar,
};

///@brief 未提供 expectedRevision 时使用的显式哨兵值。
constexpr ca::u64 kAnyRevision = static_cast<ca::u64>(-1);

///@brief 单次原始 class bytes 替换允许的服务端硬上限。
constexpr ca::usize kMaximumClassReplacementSize = 16U * 1024U * 1024U;

///@brief 读写操作使用的乐观并发控制选项。
struct MutationRequest final
{
    ca::u64 expectedRevision = kAnyRevision;
    bool    dryRun           = false;
    std::string label;
};

///@brief 一个变更字节区间，长度分别描述变更前后内容。
struct ByteRange final
{
    ca::usize offset       = 0;
    ca::usize beforeLength = 0;
    ca::usize afterLength  = 0;
};

///@brief 单个资源的紧凑字节级 diff 摘要。
struct ByteDiff final
{
    std::string            uri;
    ca::usize              beforeSize = 0;
    ca::usize              afterSize  = 0;
    std::vector<ByteRange> ranges;
};

///@brief 修改 class 后重新索引到的直接引用事实。
struct ReferenceImpact final
{
    std::string kind;
    std::string target;
};

///@brief 所有修改操作共用的稳定结果协议。
struct MutationResult final
{
    ca::Status                         status = ca::OkStatus();
    ca::u64                            revision = 0;
    bool                               committed = false;
    bool                               exportable = false;
    std::vector<jasm::Diagnostic>      diagnostics;
    std::vector<ByteDiff>              byteDiffs;
    std::vector<std::string>           affectedClasses;
    std::vector<ReferenceImpact>       affectedReferences;
};

///@brief JAR 内资源的稳定摘要。
struct ResourceSummary final
{
    std::string uri;
    std::string type;
    ca::usize   size = 0;
};

///@brief 资源的原始字节读取结果。
struct ResourceContent final
{
    ResourceSummary      summary;
    std::vector<ca::u8> bytes;
};

///@brief 一个 class 资源的原始字节和稳定内容标识。
///
///internalName 在 class header 损坏时为空；调用方仍可通过精确资源 URI 读取原始字节用于诊断。
struct ClassBytesInfo final
{
    std::string         resourceUri;
    std::string         internalName;
    std::string         fingerprint;
    std::vector<ca::u8> bytes;
};

///@brief 当前工作副本中一个相对原始 JAR 已变化的 class 摘要。
struct ModifiedClassInfo final
{
    std::string resourceUri;
    std::string originalInternalName;
    std::string currentInternalName;
    std::string originalFingerprint;
    std::string currentFingerprint;
    ca::usize   originalSize = 0;
    ca::usize   currentSize = 0;
};

///@brief 注解元素的完整值树。
struct AnnotationElement final
{
    std::string                    name;
    char                           tag = 's';
    std::string                    value;
    std::string                    annotationDescriptor;
    std::vector<AnnotationElement> children;
};

///@brief 一个运行时可见或不可见注解。
struct AnnotationInfo final
{
    std::string                    descriptor;
    bool                           visible = false;
    std::vector<AnnotationElement> elements;
};

///@brief 局部变量调试表条目。
struct LocalVariableInfo final
{
    std::string name;
    std::string descriptor;
    std::string signature;
    ca::i32     index = 0;
};

///@brief 方法字节码中的源码行号映射。
struct LineNumberInfo final
{
    ca::i32 instructionIndex = -1;
    ca::i32 line             = -1;
};

///@brief 一条方法字节码指令的稳定摘要。
struct InstructionInfo final
{
    ca::i32      index  = -1;
    ca::i32      opcode = -1;
    std::string  mnemonic;
    std::string  nodeType;
};

///@brief switch 指令的一个跳转分支（key 与绝对目标 PC）。
struct SwitchCaseInfo final
{
    ca::i32 key      = 0;
    ca::i32 targetPc = 0;
};

///@brief 一条字节码指令的完整渲染信息（基于 jclassfile 容错后端，保留 CP 索引与字节 PC）。
///@details 供 mjt info/disasm 消费，消除其直接遍历 jclassfile 指令树的需求。pc 为该
///指令在方法 code 数组中的字节偏移；cpIndex 为该指令引用的常量池条目索引（无引用为 -1）；
///operandText 为助记符后的操作数文本（如 "#5"/"3, 1"/"#5 dims=2"）；operandComment 为
///resolve 后的人类可读注释（owner.name:desc / "str" / 字面值，已过 IdentifierUtil 可读化）；
///switchDefaultPc>=0 时该指令是 switch，switchCases 列出各分支。
struct InstructionDetailInfo final
{
    ca::i32                          pc             = -1;
    ca::i32                          opcode         = -1;
    std::string                      mnemonic;
    ca::i32                          cpIndex        = -1;
    std::string                      operandText;
    std::string                      operandComment;
    bool                             isSwitch       = false;
    ca::i32                          switchDefaultPc = -1;
    std::vector<SwitchCaseInfo>      switchCases;
};

///@brief 一个方法的完整指令渲染（基于 jclassfile 后端）。
struct MethodInstructions final
{
    ca::i32                              maxStack      = 0;
    ca::i32                              maxLocals     = 0;
    std::vector<InstructionDetailInfo>   instructions;
};

///@brief 常量池条目的稳定摘要。
///@details tag 为标签名（"Utf8"/"Integer"/"MethodRef" 等，与 JVM 规范常量名对齐）；
///text 为条目值文本：非引用类条目（Utf8/Integer/Float/Long/Double/String/Class/
///MethodHandle/MethodType/Module/Package）总是填充；引用类条目（FieldRef/MethodRef/
///InterfaceMethodRef/NameAndType/InvokeDynamic/Dynamic）仅在 resolve=true 时填充解析
///后的 owner.name:desc 文本，否则为空。malformed 标记损坏/占位条目。text 未经过
///IdentifierUtil 可读化，由调用方按需施加。
struct ConstantPoolEntryInfo final
{
    ca::usize   index = 0;
    std::string tag;
    std::string text;
    bool        malformed = false;
};
///@details text 为中性渲染文本（无类型后缀、字符串不加引号），调用方按需补后缀/
///引号。数值用 %d/%lld/%g 渲染，字符串保留原文（未经 IdentifierUtil 可读化），
///与 ConstantValue 属性语义一致。
struct ConstantValueInfo final
{
    ///@brief 常量值类型，决定 text 的量纲与调用方应补的后缀。
    enum class Kind
    {
        Integer,
        Long,
        Float,
        Double,
        String
    };
    Kind        kind = Kind::Integer;
    std::string text;
};

///@brief 字段的结构化元数据。
struct FieldInfo final
{
    std::string                          name;
    std::string                          descriptor;
    std::string                          signature;
    ca::i32                              access = 0;
    std::vector<AnnotationInfo>          annotations;
    ///ConstantValue 属性；无该属性则为 nullopt。由 mbase 从 jbytecode FieldNode.value
    ///序列化，消除上层（mjt info/xref）各自解析 jclassfile 常量池的重复实现。
    std::optional<ConstantValueInfo> constantValue;
};

///@brief 方法的结构化元数据。
struct MethodInfo final
{
    std::string                    name;
    std::string                    descriptor;
    std::string                    signature;
    ca::i32                        access = 0;
    ca::i32                        maxStack = 0;
    ca::i32                        maxLocals = 0;
    ca::usize                      instructionCount = 0;
    ca::usize                      tryCatchCount = 0;
    std::vector<std::string>       exceptions;
    std::vector<LocalVariableInfo> localVariables;
    std::vector<LineNumberInfo>     lineNumbers;
    std::vector<AnnotationInfo>    annotations;
};

///@brief 精确选择的方法及其字节码指令摘要。
struct MethodBytecode final
{
    MethodInfo                  method;
    std::vector<InstructionInfo> instructions;
};

///@brief class 列表使用的轻量结构摘要。
///@details 不保留 signature、annotation、成员详情或方法体；详细查询使用 ClassInfo。
struct ClassSummary final
{
    std::string resourceUri;
    std::string internalName;
    std::string superName;
    ca::i32     access         = 0;
    ca::i32     version        = 0;
    ca::usize   interfaceCount = 0;
    ca::usize   fieldCount     = 0;
    ca::usize   methodCount    = 0;
};

///@brief class 的结构化元数据。
struct ClassInfo final
{
    std::string                 resourceUri;
    std::string                 internalName;
    std::string                 superName;
    std::string                 signature;
    ca::i32                     access = 0;
    ca::i32                     version = 0;
    std::vector<std::string>    interfaces;
    std::vector<FieldInfo>      fields;
    std::vector<MethodInfo>     methods;
    std::vector<std::string>    innerClasses;
    std::vector<AnnotationInfo> annotations;
};

///@brief MANIFEST 主属性的稳定键值条目。
struct ManifestAttribute final
{
    std::string key;
    std::string value;
};

///@brief 非 class 资源中提取出的可打印字符串。
struct ResourceStringInfo final
{
    std::string uri;
    ca::usize   offset = 0;
    std::string value;
};

///@brief 一个可分页传输的结构化搜索命中。
///
///kind 表示命中来源，例如 class、field、method、constant_pool_string、resource_string、
///constant_pool_number、instruction 或 instruction_number。instructionIndex 只在方法指令
///命中时有效；其余命中为 -1。
struct SearchHit final
{
    std::string resourceUri;
    std::string kind;
    std::string owner;
    std::string name;
    std::string descriptor;
    ca::i32     instructionIndex = -1;
    std::string value;
};

///@brief 文本搜索的匹配模式。
enum class SearchMatchMode : ca::u8
{
    Contains,
    Exact,
    Regex,
};

///@brief Workspace 五类搜索共用的过滤条件。
///
///owner 和 packagePrefix 使用 JVM internal name；memberKind 为空或 field/method；opcode 为 -1 时
///不过滤。并非每类搜索都使用全部字段，例如 class 搜索忽略 memberKind/opcode。
struct SearchOptions final
{
    SearchMatchMode matchMode = SearchMatchMode::Contains;
    bool            caseSensitive = false;
    std::string     owner;
    std::string     packagePrefix;
    std::string     memberKind;
    ca::i32         opcode = -1;
};

///@brief 方法覆盖组件中的一个声明。
struct MethodOverrideInfo final
{
    std::string owner;
    std::string name;
    std::string descriptor;
    bool        primary = false;
    bool        supporting = false;
    std::string supportId;
};

///@brief 支持库的只读描述。
struct SupportingResourceInfo final
{
    std::string id;
    std::string path;
    std::string fingerprint;
    ca::usize   resourceCount = 0;
    ca::usize   classCount = 0;
};

///@brief 精确 xref 查询返回的引用位置和目标。
struct ReferenceInfo final
{
    std::string sourceClass;
    std::string sourceMethod;
    std::string sourceDescriptor;
    ca::i32     instructionIndex = -1;
    std::string target;
    std::string kind;
    bool        supporting = false;
    std::string supportId;
};

///@brief 继承关系结果中一个 class 的来源。
struct ClassRelationSource final
{
    std::string internalName;
    bool        supporting = false;
    std::string supportId;
};

///@brief 一个 class 的继承、接口和后代闭包。
struct ClassRelations final
{
    std::vector<std::string> superclasses;
    std::vector<std::string> interfaces;
    std::vector<std::string> directSubclasses;
    std::vector<std::string> allSubclasses;
    std::vector<std::string> implementors;
    std::vector<ClassRelationSource> sources;
};

///@brief 影响分析中的一项预期或已应用映射。
struct RenameMapping final
{
    std::string kind;
    std::string before;
    std::string after;
};

///@brief 工作区重命名操作的目标种类。
enum class RenameKind : ca::u8
{
    Class,
    Package,
    Field,
    Method,
    Local,
};

///@brief 一个 class/package/member/local 重命名请求。
struct RenameRequest final
{
    RenameKind   kind = RenameKind::Class;
    std::string  owner;
    std::string  methodName;
    std::string  name;
    std::string  descriptor;
    std::string  newName;
    ca::i32      localIndex = -1;
    bool         cascadeOverrides = false;
    RemapPolicy  policy = RemapPolicy::all();
};

///@brief 对候选映射变更的前后影响和验证结果。
struct ImpactAnalysis final
{
    ca::Status                         status = ca::OkStatus();
    bool                               exportable = false;
    std::vector<ReferenceImpact>       beforeReferences;
    std::vector<ReferenceImpact>       afterReferences;
    std::vector<ByteDiff>              byteDiffs;
    std::vector<jasm::Diagnostic>      diagnostics;
    std::vector<std::string>           risks;
};

///@brief 重命名预览，包括方法 override 组件和可供审阅的映射表。
struct RenameAnalysis final
{
    ca::Status                         status = ca::OkStatus();
    bool                               readyToApply = false;
    bool                               requiresOverrideReview = false;
    std::vector<RenameMapping>         mappings;
    std::vector<MethodOverrideInfo>    overrideComponent;
    ImpactAnalysis                     impact;
};

///@brief 一个可由工作区执行的已注册 pass 摘要。
struct PassDescription final
{
    std::string name;
    std::string description;
    std::string scope;
};

///@brief 一个可由工作区执行的已注册 pipeline 摘要。
struct PipelineDescription final
{
    std::string name;
    std::string description;
};

///@brief 在工作区候选副本上执行 pass 或 pipeline 的请求。
///
///passNames 与 pipeline 互斥；两者均为空时使用 `default-optimize`。jdkPath 非空时会
///扫描 JDK 类路径并以 COMPUTE_MAXS | COMPUTE_FRAMES 写回候选 class。
struct PipelineRequest final
{
    std::vector<std::string> passNames;
    std::string              pipeline;
    std::string              jdkPath;
};

///@brief 一个工作区与外部 JAR 的全资源字节比较结果。
struct JarDiffResult final
{
    ca::Status              status = ca::OkStatus();
    std::string             referencePath;
    ca::usize               workspaceOnly = 0;
    ca::usize               referenceOnly = 0;
    ca::usize               modified = 0;
    std::vector<ByteDiff>   byteDiffs;
};

///@brief 一项可由 JAR 修复事务处理的资源问题。
struct JarRepairIssue final
{
    std::string uri;
    std::string kind;
    std::string message;
    ca::usize   size = 0;
    bool        selectedForRemoval = false;
};

///@brief JAR 修复的显式、可审阅删除策略。
struct JarRepairRequest final
{
    bool removeInvalidClassEntries = false;
    bool removeCorruptOutputMarkers = false;
};

///@brief JAR 修复扫描结果及候选事务预览或提交结果。
struct JarRepairResult final
{
    MutationResult              mutation;
    ca::usize                   resourceCount = 0;
    ca::usize                   classCount = 0;
    std::vector<JarRepairIssue> issues;
};

///@brief 工作流中要在候选副本前后执行的精确引用查询种类。
enum class WorkflowReferenceKind : ca::u8
{
    Class,
    Field,
    Method,
};

///@brief 一个可重放工作流的精确 xref 查询。
struct WorkflowReferenceQuery final
{
    WorkflowReferenceKind kind = WorkflowReferenceKind::Class;
    std::string           owner;
    std::string           name;
    std::string           descriptor;
};

///@brief 一个 xref 查询在工作流输入和候选输出中的稳定结果。
struct WorkflowReferenceReport final
{
    WorkflowReferenceQuery       query;
    std::vector<ReferenceInfo>   before;
    std::vector<ReferenceInfo>   after;
};

///@brief 工作流中一个已执行阶段的状态。
struct WorkflowStageResult final
{
    std::string name;
    ca::Status  status = ca::OkStatus();
};

///@brief 一次工作区候选事务的组合请求。
///
///remapPlan 为调用期间借用的已解析计划，必须在 runWorkflow 返回前保持有效。若设置
///runPipeline，mapping 和 pipeline 都在同一候选副本中运行；任一阶段失败都不会提交工作区。
struct WorkflowRequest final
{
    const MemberRemapPlan*                 remapPlan = nullptr;
    RemapPolicy                            remapPolicy = RemapPolicy::bytecodeOnly();
    bool                                   runPipeline = false;
    PipelineRequest                        pipeline;
    std::vector<WorkflowReferenceQuery>    referenceQueries;
    ///@brief 非空时将已验证的候选副本原子导出到此前不存在的 JAR 路径。
    ///@note dryRun 只校验导出目标，不创建输出文件。
    std::string                            exportPath;
    ///@brief 在验证完成后接收一次性候选 JAR 快照的只读阶段。
    ///@note 回调返回前必须完成读取，快照会立刻删除；返回错误时不导出或提交 Workspace。
    std::function<ca::Status(const std::string&)> candidateJarConsumer;
    std::string                            candidateJarStageName;
};

///@brief 可重放工作流的机器可读报告。
///
///inputFingerprint 和 candidateFingerprint 为按资源 URI 和字节内容计算的稳定 FNV-1a-64 摘要。
///mutation 复用所有 Workspace 编辑的 revision、diagnostics 和 byte diff 契约；dryRun 时仅
///返回候选报告，成功提交时最多增加一个 revision。
struct WorkflowResult final
{
    MutationResult                         mutation;
    std::string                            inputFingerprint;
    std::string                            candidateFingerprint;
    std::string                            exportPath;
    bool                                   exported = false;
    std::vector<WorkflowStageResult>       stages;
    std::vector<WorkflowReferenceReport>   references;
};

///@brief 已完成工作流候选 JAR 的内部提交请求。
///
///candidateJarPath 必须由上层事务私有持有，不能直接暴露给不可信客户端。exportPath 非空时会在
///commit 前从同一候选原子导出；commit 失败会删除该导出。dryRun 只验证候选和导出目标。
struct WorkflowCandidateCommitRequest final
{
    std::string candidateJarPath;
    std::string exportPath;
};

///@brief 工作流候选 JAR 导入、导出和提交的统一结果。
struct WorkflowCandidateCommitResult final
{
    MutationResult mutation;
    std::string    exportPath;
    bool           exported = false;
};

}   //namespace mjt::base::workspace
