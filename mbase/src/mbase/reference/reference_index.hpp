///@defgroup reference_index 引用索引模块
///
///面向全程序 optimize/recover pass 的基础分析。回答一个窄问题：哪些扫描过的 class
///和方法体直接引用给定的 method、field 或 type？它刻意比调用图更小——只记录字节码
///中的编码事实，后续分析再结合继承图、方法覆盖、字段覆盖、副作用模型或 classpath
///数据。
///
///查询面同时提供"谁引用了此 method/field"的精确成员视图和"此类引用/owner 了什么"
///的类级/owner 级视图。
///
///## 扫描范围
///
///索引扫描 `ClassResource` 方法体并记录：
///
///- 声明的 class（含直接超类和接口名）
///- 声明的 method 和 field
///- 来自 `MethodInsnNode` 的方法调用
///- 来自 `FieldInsnNode` 的字段访问
///- 显式类型指令（`new`、`checkcast`、`instanceof`、`anewarray`）
///- 嵌入方法描述符、字段访问描述符和字段声明描述符中的对象类型
///- `LDC` class 常量（object 和 array 类型）
///- `multianewarray` 描述符
///- method/field owner 作为类型引用
///- 注解和签名中的元数据类型引用
///- 结构 class 元数据（`OuterClass`、nest host/members、permitted subclasses、
///inner classes、record component 描述符）
///- class-string 反射使用、常量反射成员查找、method handle、lambda bootstrap 目标
///- 通过 operand-stack 和局部变量别名的常量 `Class`/`Method`/`Constructor`/`Field`
///对象的方法局部传播
///- 确认的 `Method.invoke`、`Class.newInstance`、`Constructor.newInstance` 和
///`Field.get/set` 执行/访问点（含基本类型字段访问器变体）
///
///Owner-as-type 条目对 class 可达性很重要。即使在考虑虚分派或常量池引用之前，
///对 `B.callee` 的调用或对 `B.value` 的读取就能使 class `B` 保持被引用状态。
///
///Class-header、结构元数据和字段声明类型条目使用无源方法和指令索引的类级引用点，
///出现在 `typeReferencesFromClass` 而非 `typeReferencesFrom`。
///
///## 反射事实语义
///
///常量成员查找和实际反射执行是分离的事实：
///
///- `Reflection` 类型表示成员对象被查找过，即使对象从未被执行或访问。
///- `ReflectionExecution` 仅在方法局部数据流帧证明 `Method.invoke`、
///`Class.newInstance` 或 `Constructor.newInstance` 收到已知反射对象时发出。
///- `ReflectionAccess` 仅在证明的 `Field.get/set` 接收者时发出（含 getInt/setLong
///等基本类型变体）。
///- 相等反射值在控制流合并时存活；冲突值合并为 Unknown，索引不从瞬时路径猜测目标。
///
///CallGraph 使用带解析描述符的 `ReflectionExecution` 事实，不将仅查找事实转为调用边。
///
///## 非目标
///
///索引不解析：
///
///- 虚/接口分派
///- 跨方法传递的反射对象
///- 存储在字段或通用数组/集合中的值
///- 非常量反射数据流
///
///反射 method/constructor 描述符在常量 `Class[]` 参数数组匹配扫描声明或方法声明
///包含唯一 owner/name 匹配时填充。支持的数组形状包括内联 `DUP`/`AASTORE` 构造和
///使用 `ANEWARRAY` 创建、`ASTORE` 存储、常量 `ALOAD`/`AASTORE` 序列填充的直线
///局部数组。对象、数组和包装器 `TYPE` 基本类型参数被识别。缺失或重复索引、局部
///覆盖和进入构造区域的控制流仍不解析。数组别名、分支合并、字段、跨过程传播和
///通用容器属于后续分析。索引也不判定引用是否存活——那些决策属于后续分析。
///
///格式错误的 class 被跳过，使基础分析在容错 CLI 工作流中可用。
///
///## 分析集成
///
///`ReferenceIndexAnalysis` 为 `GlobalAnalysisManager` 包装扫描。全局 pass 可
///作为常规 analysis 结果请求它，而不是自行重新扫描 container。
///
///预期分层：
///
///1. `ReferenceIndex`：直接编码引用
///2. `CallGraph`：方法声明节点和直接调用边
///3. `ClassHierarchy` / `MethodHierarchy` / `FieldHierarchy`：继承和覆盖关系
///4. Pass 特定策略：未使用成员删除、内联资格、访问收紧、重命名安全性、报告
///
///## 验证覆盖
///
///`ReferenceIndexTest` 构建双 class fixture 并验证：
///
///- `B.callee` 的调用者
///- `B.value` 的使用者
///- 通过显式类型指令和 member owner 对 B 的类型引用
///- 声明 class 查找（含直接超类和接口元数据）
///- 声明 method/field 查找
///- method/field/type 引用的源方法视图
///- 批量全局 pass 查询的源 class 和目标 owner 视图
///- 描述符、字段声明、class-header、LDC class 常量类型引用
///- 注解/签名/结构元数据、反射字符串、反射成员查找、method handle、lambda bootstrap 目标
///- 通过局部别名的 method/constructor/field 反射执行、控制流合并的 fail-closed 行为
///- `GlobalAnalysisManager` 通过 `ReferenceIndexAnalysis` 集成

#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mbase/model/resource_container.hpp"

namespace mjt::base {

class AliasAnalysis;

///@brief Stable identifier for a JVM method reference.
///
///The strings are internal JVM names/descriptors copied out of the parsed
///bytecode tree. They are independent of the temporary ClassNode arena used
///while building the index, so query results remain valid after build.
struct MethodRefKey
{
    std::string owner;
    std::string name;
    std::string desc;
};

///@brief Stable identifier for a JVM field reference.
struct FieldRefKey
{
    std::string owner;
    std::string name;
    std::string desc;
};

///@brief Location of a reference inside one method body.
struct ReferenceSite
{
    std::string sourceClass;
    std::string sourceMethod;
    std::string sourceDesc;
    ca::i32     opcode    = -1;
    ca::i32     insnIndex = -1;
};

///@brief Why a method reference was recorded.
enum class MethodReferenceKind
{
    Direct,
    MethodHandle,
    Lambda,
    ///@brief Constant reflection lookup diagnostic. target.desc may be empty
    ///when the descriptor was not resolved from the reflection argument array.
    ///Lookup facts do not represent execution and are excluded from CallGraph.
    Reflection,
    ///@brief Confirmed execution through Method.invoke or Constructor.newInstance.
    ReflectionExecution
};

///@brief Why a field reference was recorded.
enum class FieldReferenceKind
{
    Direct,
    MethodHandle,
    ///@brief Constant reflection lookup diagnostic. target.desc may be empty
    ///when the field descriptor was not resolved from declarations or metadata.
    Reflection,
    ///@brief Confirmed access through Field.get/set or a primitive variant.
    ReflectionAccess
};

///@brief Method reference discovered from bytecode or a supported indirect mechanism.
///
///This is not a resolved call graph edge: virtual/interface dispatch is left to
///later MethodHierarchy/ClassHierarchy-aware analyses. The target is exactly
///the owner/name/descriptor encoded in the bytecode instruction.
struct MethodReference
{
    ReferenceSite site;
    MethodRefKey  target;
    ///@brief Parsed argument descriptor for constant reflection lookups.
    ///
    ///This
    ///is intentionally separate from target.desc. Reflection facts expose
    ///a full JVM method
    ///descriptor only after build can match the argument
    ///list against scanned
    ///declarations.
    std::string         reflectionArgDesc;
    bool                isInterface = false;
    MethodReferenceKind kind        = MethodReferenceKind::Direct;
};

///@brief Field reference discovered from bytecode or a supported indirect mechanism.
struct FieldReference
{
    ReferenceSite      site;
    FieldRefKey        target;
    FieldReferenceKind kind = FieldReferenceKind::Direct;
};

///@brief Why a class/type name was recorded as referenced.
enum class TypeReferenceKind
{
    ClassSuper,
    ClassInterface,
    MethodOwner,
    FieldOwner,
    MethodDescriptor,
    FieldDescriptor,
    FieldDeclarationDescriptor,
    TypeInstruction,
    MultiANewArray,
    ClassConstant,
    Annotation,
    Signature,
    StructuralMetadata,
    ReflectionString,
    MethodHandleOwner
};

///@brief Direct type reference discovered from bytecode instructions.
///
///Method/field owners are also recorded as type references because class-level
///passes such as unused-class removal need to know that `Owner.member` keeps
///`Owner` reachable even before a full call graph exists.
struct TypeReference
{
    ReferenceSite     site;
    std::string       targetType;
    TypeReferenceKind kind = TypeReferenceKind::TypeInstruction;
};

///@brief Class declared by a scanned class resource.
///
///ReferenceIndex records only the direct class identity, access flags, direct
///superclass, and directly implemented interfaces. Transitive hierarchy
///closure belongs to hierarchy analyses layered above this direct-reference
///index.
struct ClassDeclaration
{
    std::string              name;
    std::string              superName;
    std::vector<std::string> interfaces;
    ca::i32                  access = 0;
};

///@brief Method declared by a scanned class resource.
///
///Declaration records let call-graph and reachability analyses distinguish
///project-local methods from external targets that only appear in bytecode call
///instructions.
struct MethodDeclaration
{
    MethodRefKey method;
    ca::i32      access = 0;
};

///@brief Field declared by a scanned class resource.
struct FieldDeclaration
{
    FieldRefKey field;
    ca::i32     access = 0;
};

///@brief Minimal direct-reference index for P3 whole-program analyses.
///
///ReferenceIndex scans ClassResource bytecode and records direct references
///from method bodies
///to methods, fields, and types. It also records conservative
///metadata facts such as
///annotations, signatures, structural class metadata,
///class-string reflection uses, method
///handles, lambdas, constant
///Class.getDeclared* / Class.get* member lookups, and confirmed
///reflective
///execution through Method/Constructor/Field objects propagated within a
///method.
///Reflection member descriptors are filled when a constant
///`Class[]` argument list matches a
///scanned method declaration, or when the
///scanned declarations contain a unique owner/name
///match. It deliberately
///does not resolve inheritance, virtual dispatch, interprocedural
///reflection
///aliases, or conflicting control-flow values. Those are higher-level analyses
///
///layered on top of this direct index.
///Build policy: malformed classes are skipped
///conservatively so a single bad resource does not make the whole analysis unavailable.
class ReferenceIndex
{
public:
    ///@brief Rebuild the index from all class resources in a container.
    ///
    ///Existing results are discarded before scanning starts. The arena is used
    ///only by the bytecode reader while decoding class trees; every exposed
    ///query result owns stable std::string copies and may outlive those
    ///temporary ClassNode instances.
    ///
    ///@param container Resource set to scan. Non-class resources are ignored.
    ///@param arena Shared UTF-8 arena used by libjbytecode readers.
    void build(ResourceContainer& container, ca::str::Utf8StringArena& arena);

    ///@brief Restore the index from already-materialized primary tables.
    ///
    ///Used by the persistent cache loader to skip bytecode parsing. The six
    ///primary tables are moved in verbatim, then all derived lookup maps are
    ///rebuilt to match the post-build state. The caller must guarantee the
    ///tables originate from a trusted writer (validated magic/version/etc);
    ///restore does not re-validate table invariants beyond what the derived
    ///map rebuild requires.
    ///
    ///Type references whose `site.sourceMethod` is empty are treated as
    ///class-scoped (the same convention as `makeClassSite`), so they populate
    ///`typeRefsBySourceClass_` but not `typeRefsBySource_` — matching the
    ///original `appendTypeReference(ref, methodScoped=false)` behavior.
    void restore(std::vector<ClassDeclaration>  classDecls,
                 std::vector<MethodDeclaration> methodDecls,
                 std::vector<FieldDeclaration>  fieldDecls,
                 std::vector<MethodReference>   methodRefs,
                 std::vector<FieldReference>    fieldRefs,
                 std::vector<TypeReference>     typeRefs);

    ///@brief Remove all collected references and target lookup tables.
    void clear();

    ///@brief Return all direct call instructions targeting a method key.
    ///
    ///The key is matched exactly against the owner/name/descriptor encoded in
    ///MethodInsnNode. No inheritance, bridge, interface, or virtual dispatch
    ///expansion is applied here.
    const std::vector<MethodReference>& methodReferencesTo(const std::string& owner,
                                                           const std::string& name,
                                                           const std::string& desc) const;

    ///@brief Return all direct field access instructions targeting a field key.
    ///
    ///The key is matched exactly against FieldInsnNode owner/name/descriptor.
    ///Field hierarchy and shadowing policy are intentionally left to later
    ///analyses built on top of the direct index.
    const std::vector<FieldReference>& fieldReferencesTo(const std::string& owner,
                                                         const std::string& name,
                                                         const std::string& desc) const;

    ///@brief Return all direct type references matching an internal JVM name.
    ///
    ///Results include explicit type instructions plus method/field owner
    ///entries. Array descriptors are preserved as encoded for multianewarray
    ///instructions because descriptor normalization belongs to higher layers.
    const std::vector<TypeReference>& typeReferencesTo(const std::string& targetType) const;

    ///@brief Return direct method calls emitted by one source method.
    ///
    ///This is the forward view paired with methodReferencesTo. It is useful
    ///for call graph construction and local "what does this method touch?"
    ///reports without scanning the global reference list.
    const std::vector<MethodReference>& methodReferencesFrom(const std::string& owner,
                                                             const std::string& name,
                                                             const std::string& desc) const;

    ///@brief Return direct field accesses emitted by one source method.
    const std::vector<FieldReference>& fieldReferencesFrom(const std::string& owner,
                                                           const std::string& name,
                                                           const std::string& desc) const;

    ///@brief Return direct type references emitted by one source method.
    const std::vector<TypeReference>& typeReferencesFrom(const std::string& owner,
                                                         const std::string& name,
                                                         const std::string& desc) const;

    ///@brief Return direct method calls emitted by any method in a source class.
    ///
    ///This class-level forward view is intended for global passes that need to
    ///invalidate or inspect all outgoing references from one class without
    ///iterating over every method-level source key.
    const std::vector<MethodReference>& methodReferencesFromClass(const std::string& owner) const;

    ///@brief Return direct field accesses emitted by any method in a source class.
    const std::vector<FieldReference>& fieldReferencesFromClass(const std::string& owner) const;

    ///@brief Return direct type references associated with a source class.
    ///
    ///Results include method-body references plus class declaration facts such
    ///as superclass, interfaces, and field declaration descriptor types.
    const std::vector<TypeReference>& typeReferencesFromClass(const std::string& owner) const;

    ///@brief Return all direct call instructions whose encoded target owner matches owner.
    const std::vector<MethodReference>& methodReferencesToOwner(const std::string& owner) const;

    ///@brief Return all direct field access instructions whose encoded target owner matches owner.
    const std::vector<FieldReference>& fieldReferencesToOwner(const std::string& owner) const;

    ///@brief Return a declared method by exact owner/name/descriptor.
    const MethodDeclaration* declaredMethod(const std::string& owner, const std::string& name,
                                            const std::string& desc) const;

    ///@brief Return a declared field by exact owner/name/descriptor.
    const FieldDeclaration* declaredField(const std::string& owner, const std::string& name,
                                          const std::string& desc) const;

    ///@brief Return all methods declared by a scanned class.
    const std::vector<MethodDeclaration>& declaredMethodsIn(const std::string& owner) const;

    ///@brief Return all fields declared by a scanned class.
    const std::vector<FieldDeclaration>& declaredFieldsIn(const std::string& owner) const;

    ///@brief Return a declared class by exact internal JVM name.
    const ClassDeclaration* declaredClass(const std::string& owner) const;

    ///@brief Return every declared class in scan order.
    const std::vector<ClassDeclaration>& classDeclarations() const { return classDecls_; }

    ///@brief Return every direct method reference in scan order.
    const std::vector<MethodReference>& methodReferences() const { return methodRefs_; }

    ///@brief Return every direct field reference in scan order.
    const std::vector<FieldReference>& fieldReferences() const { return fieldRefs_; }

    ///@brief Return every direct type reference in scan order.
    const std::vector<TypeReference>& typeReferences() const { return typeRefs_; }

    ///@brief Return every declared method in scan order.
    const std::vector<MethodDeclaration>& methodDeclarations() const { return methodDecls_; }

    ///@brief Return every declared field in scan order.
    const std::vector<FieldDeclaration>& fieldDeclarations() const { return fieldDecls_; }

    ///@brief Number of direct method reference instructions collected.
    ca::usize methodReferenceCount() const { return methodRefs_.size(); }

    ///@brief Number of direct field reference instructions collected.
    ca::usize fieldReferenceCount() const { return fieldRefs_.size(); }

    ///@brief Number of direct type reference records collected.
    ca::usize typeReferenceCount() const { return typeRefs_.size(); }

    ///@brief Number of declared methods collected.
    ca::usize methodDeclarationCount() const { return methodDecls_.size(); }

    ///@brief Number of declared fields collected.
    ca::usize fieldDeclarationCount() const { return fieldDecls_.size(); }

    ///@brief Number of declared classes collected.
    ca::usize classDeclarationCount() const { return classDecls_.size(); }

private:
    void indexClassBytes(const std::vector<ca::u8>& bytes, ca::str::Utf8StringArena& arena,
                         const AliasAnalysis* aliases = nullptr);
    void appendMethodReference(const MethodReference& ref);
    void appendFieldReference(const FieldReference& ref);
    void appendTypeReference(const TypeReference& ref, bool methodScoped);
    void resolveReflectionMemberDescriptors();
    void rebuildReferenceLookups();

    std::vector<ClassDeclaration>                                   classDecls_;
    std::vector<MethodDeclaration>                                  methodDecls_;
    std::vector<FieldDeclaration>                                   fieldDecls_;
    std::vector<MethodReference>                                    methodRefs_;
    std::vector<FieldReference>                                     fieldRefs_;
    std::vector<TypeReference>                                      typeRefs_;
    std::unordered_map<std::string, std::size_t>                    classDeclsByKey_;
    std::unordered_map<std::string, std::size_t>                    methodDeclsByKey_;
    std::unordered_map<std::string, std::size_t>                    fieldDeclsByKey_;
    std::unordered_map<std::string, std::vector<MethodDeclaration>> methodDeclsByOwner_;
    std::unordered_map<std::string, std::vector<FieldDeclaration>>  fieldDeclsByOwner_;
    std::unordered_map<std::string, std::vector<MethodReference>>   methodRefsByTarget_;
    std::unordered_map<std::string, std::vector<FieldReference>>    fieldRefsByTarget_;
    std::unordered_map<std::string, std::vector<TypeReference>>     typeRefsByTarget_;
    std::unordered_map<std::string, std::vector<MethodReference>>   methodRefsByTargetOwner_;
    std::unordered_map<std::string, std::vector<FieldReference>>    fieldRefsByTargetOwner_;
    std::unordered_map<std::string, std::vector<MethodReference>>   methodRefsBySource_;
    std::unordered_map<std::string, std::vector<FieldReference>>    fieldRefsBySource_;
    std::unordered_map<std::string, std::vector<TypeReference>>     typeRefsBySource_;
    std::unordered_map<std::string, std::vector<MethodReference>>   methodRefsBySourceClass_;
    std::unordered_map<std::string, std::vector<FieldReference>>    fieldRefsBySourceClass_;
    std::unordered_map<std::string, std::vector<TypeReference>>     typeRefsBySourceClass_;
};

}   //namespace mjt::base
