/// @file        dex_domain.hpp
/// @brief       dex 域：apk/裸 dex 输入的类事实、引用、常量与文本视图聚合器。
///
/// libdex DOM（每 dex 文件一实例）之上的纯读事实层：apk 输入经 libjzip
/// 提取根级 *.dex 条目，裸 dex 直接装载。类事实（internal_name/super/
/// interfaces/members）、引用扫描（语义逐条对齐 reference_scan 的 JVM 扫描——
/// dex 描述符语法与 JVM 同形）、常量事实（string_ids/静态初值/const 字面量）、
/// 层次闭包（dex DOM 反向索引）与 smali 文本视图都从这里出。
///
/// 身份语法裁决：dex 的类型描述符（"Lcom/foo;Bar;"）、方法 proto（"(II)V"）
/// 与 JVM 内部名/描述符语法同形，本层零转换直通——analysis.db 的类事实/
/// refs/常量面按同一身份语法键控，灌库零漂移（见 docs/apk-dex-support-plan.md §4）。
///
/// 容错纪律与 JVM 扫描同款 fail-open：单类/单方法解码失败（libdex Result
/// Err）跳过该类/方法，不致命。注解类型引用未随迁。
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "libca/str/utf8_string.hpp"

#include "constant_scan.hpp"
#include "reference_scan.hpp"
#include "workspace_types.hpp"

namespace dex::core {
class DexFile;   ///< libdex DOM（dex_file.hpp；此处仅前置声明）
}

namespace mjbase::jar {
class IJarReader;   ///< jar 读取接口（jar_reader.hpp；此处仅前置声明）
}

namespace mjt::workspace {

/// @brief dex 域聚合器（WorkspaceBase 的 dex 分派后端；可空 = 非 dex 输入）。
class DexDomain
{
public:
    ~DexDomain();   ///< 析构 out-of-line：unique_ptr<DexFile> 的删除器需要 DexFile 完整类型

    /// @brief apk 容器装载：逐条目提取根级 *.dex 条目（libjzip 解压 → readDex）。
    /// @param reader 已就位的 jar 读取器（与容器导入共用一次遍历）。
    /// @return 无根级 dex 条目 = InvalidArgument（非 apk 输入）。
    static WorkspaceResult<std::unique_ptr<DexDomain>> loadFromJarReader(
        mjbase::jar::IJarReader& reader);

    /// @brief 裸 dex 装载：单 dex 文件字节。
    /// @param uri 该 dex 在资源面的条目 URI（裸输入 = 文件路径，与 openClass 口径一致）。
    static WorkspaceResult<std::unique_ptr<DexDomain>> loadFromDexBytes(
        const std::vector<ca::u8>& bytes, const std::string& uri);

    /// @brief dex 版本号（首个 dex 的 magic 版本，35..41；类事实 version 列同源）。
    ca::u32 dexVersion() const noexcept { return dexVersion_; }

    /// @brief dex 文件条目 URI 清单（装载序）。
    const std::vector<std::string>& dexUris() const noexcept { return dexUris_; }
    const std::vector<ca::str::Utf8String>& dexLoadErrors() const noexcept { return dexLoadErrors_; }

    /// @brief 类总数（跨 dex 文件的 class_def 总数）。
    ca::usize classCount() const noexcept { return classOrder_.size(); }
    const std::vector<std::string>& classNames() const noexcept { return classOrder_; }

    /// @brief internal name 是否在 dex 域内。
    bool containsClass(const std::string& internalName) const;

    //-- 类事实面 ------------------------------------------------------------

    WorkspaceResult<std::vector<ClassSummary>> listClassSummaries() const;
    WorkspaceResult<std::vector<ClassInfo>> listClasses(bool includeMethodBodies = true) const;
    /// @brief URI 或 internal name 查询 class 详情（URI = dex 条目 URI 时只匹配
    ///        单类 dex；优先内名）。
    WorkspaceResult<ClassInfo> classInfo(const std::string& uriOrInternalName) const;
    WorkspaceResult<MethodInfo> methodInfo(const std::string& owner, const std::string& name,
                                           const std::string& descriptor) const;
    WorkspaceResult<FieldInfo> fieldInfo(const std::string& owner, const std::string& name,
                                         const std::string& descriptor) const;
    WorkspaceResult<MethodBytecode> methodBytecode(const std::string& owner,
                                                   const std::string& name,
                                                   const std::string& descriptor) const;
    WorkspaceResult<MethodInstructions> methodInstructions(const std::string& owner,
                                                           const std::string& name,
                                                           const std::string& descriptor,
                                                           bool resolve) const;

    /// @brief dex 五张 ID 表投影常量池视图（tag = StringId/TypeId/ProtoId/
    ///        FieldId/MethodId；resolve=true 时引用类条目填 owner.name:desc 文本）。
    WorkspaceResult<std::vector<ConstantPoolEntryInfo>> constantPool(
        const std::string& uriOrInternalName, bool resolve) const;

    //-- 引用面（语义对齐 reference_scan 的 JVM 扫描）--------------------------

    /// @brief 有调用点或方法句柄表时，查询需要保留分析库 schema 无法表达的引用种类。
    bool hasDynamicReferenceTables() const noexcept;

    /// @brief 全域引用扫描（fail-open：单类/单方法解码失败跳过）。
    ReferenceScan buildReferenceScan() const;
    WorkspaceResult<std::vector<ReferenceInfo>> xrefsToMethod(const std::string& owner,
                                                              const std::string& name,
                                                              const std::string& descriptor) const;
    WorkspaceResult<std::vector<ReferenceInfo>> xrefsToField(const std::string& owner,
                                                             const std::string& name,
                                                             const std::string& descriptor) const;
    WorkspaceResult<std::vector<ReferenceInfo>> xrefsToClass(const std::string& internalName) const;
    /// @brief 直接调用、引导方法和未解析的动态调用点；resolved=true 展开虚调用。
    WorkspaceResult<std::vector<ReferenceInfo>> calleesOfMethod(const std::string& owner,
                                                                const std::string& name,
                                                                const std::string& descriptor,
                                                                bool resolved) const;

    //-- 层次面 ---------------------------------------------------------------

    WorkspaceResult<ClassRelations> classRelations(const std::string& internalName) const;
    /// @brief super 链同名同描述符声明（primary = 查询 owner 自身声明；
    ///        域外超类截断）。
    WorkspaceResult<std::vector<MethodOverrideInfo>> overridesOfMethod(
        const std::string& owner, const std::string& name, const std::string& descriptor) const;

    //-- 常量事实面（ingest 桥喂库形状）----------------------------------------

    /// @brief Visit each decodable string_ids entry once per loaded DEX unit.
    ///        The UTF-8 view is borrowed for the duration of the callback.
    void visitStringTableValues(const std::function<void(std::string_view)>& visit) const;

    /// @brief 类的字符串切片（引用集口径：const-string 值 + 静态字符串初值 +
    ///        自身/成员名字与描述符，facet 预分同 constantUtf8Facet）。
    WorkspaceResult<std::vector<mjbase::project::DbStringRecord>> cpStringRecords(
        const std::string& internalName) const;
    /// @brief 类级数值切片（静态初值 encoded_array；member_id=0 哨兵口径）。
    WorkspaceResult<std::vector<mjbase::project::DbNumberRecord>> cpNumberRecords(
        const std::string& internalName) const;
    /// @brief 逐方法数值切片（const 家族字面量；有方法体的方法恒产出切片）。
    WorkspaceResult<std::vector<MethodNumberSlice>> methodNumberSlices(
        const std::string& internalName) const;

    //-- 文本面 ----------------------------------------------------------------

    /// @brief 单类 smali 反汇编文本（libsmali disassembleClass）。
    WorkspaceResult<std::string> smaliText(const std::string& uriOrInternalName) const;
    /// @brief Assemble a class replacement into a copy of its containing DEX unit.
    WorkspaceResult<std::vector<ca::u8>> assembleSmali(const std::string& internalName,
                                                       std::string_view source) const;
    /// @brief 指令文本搜索（smali 形操作数行渲染；SearchOptions 过滤）。
    WorkspaceResult<std::vector<SearchHit>> searchInstructions(const std::string& query,
                                                               const SearchOptions& options) const;

private:
    /// @brief 类寻址槽：dex 文件序号 + class_def 序号。
    struct Slot
    {
        ca::u32 dexIndex     = 0;
        ca::u32 classDefIndex = 0;
    };

    /// @brief URI 或 internal name → 类槽（优先内名；URI 仅匹配单类 dex）。
    const Slot* findClass(const std::string& uriOrInternalName) const;
    /// @brief 候选类是否（传递）继承祖先类（域内 super 链；域外/环截断）。
    bool isSubclassInDomain(const std::string& candidate, const std::string& ancestor) const;
    /// @brief 类的 smali 文本（内部面：按槽寻址）。
    WorkspaceResult<std::string> smaliTextAt(const Slot& slot) const;
    WorkspaceErrorInfo domainError(WorkspaceError code, const std::string& operation,
                                   const std::string& detail) const;
    WorkspaceErrorInfo notFound(const std::string& operation,
                                const std::string& target) const;

    /// libdex DOM（每 dex 文件一实例）。mutable：const 查询路径内的懒解码
    /// 取值（protoParameters/interfaces/classData/staticValues/smali 反汇编）
    /// 按需填缓存——与 WorkspaceBase 的 lvt 懒算旗标 mutable 同型。
    mutable std::vector<std::unique_ptr<dex::core::DexFile>> dexes_;
    std::vector<std::string>                                 dexUris_;
    std::vector<ca::str::Utf8String>                         dexLoadErrors_;
    /// internal name → 类槽（装载时建）。
    std::unordered_map<std::string, Slot> byName_;
    /// 类名清单（跨 dex 文件的 class_def 序；listClasses 行序锚）。
    std::vector<std::string> classOrder_;
    ca::u32 dexVersion_ = 35;
};

/// @brief 输入嗅探：是否 dex 族制品（apk 或裸 dex）。
///
/// 剥离 BOM 后读文件头：`dex\n` magic → 裸 dex；`PK\x03\x04` 且扩展名 .apk
/// （大小写不敏感）→ apk。返回 0 = 非 dex 族（走 jar 流程）。
enum class DexArtifactForm : ca::u8
{
    None = 0,
    Apk,
    Dex,
};
DexArtifactForm probeDexArtifact(const std::string& path);

/// @brief dex 输入形态文本（"apk"/"dex"；错误报告与 project_info 用）。
const char* to_string(DexArtifactForm form) noexcept;

}   // namespace mjt::workspace
