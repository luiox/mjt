/// @file        commands.hpp
/// @brief       `mjt` CLI 命令核心面：info / mapping / jar fix / decompile /
///              strings / xref 六个命令的纯逻辑入口与报告结构（main 与 gtest
///              共链）。
///
/// 分层契约：本模块只做「打开制品 → 调 workspace/mjbase 既有面 → 产出可
/// 断言的报告结构」，命令行解析（ca::opt）与退出码映射归 main.cpp；渲染
/// （文本/JSON）为纯函数，测试直接断言结构与渲染输出。
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "libca/core/result.hpp"
#include "shared/decompiler.hpp"
#include "workspace/workspace_types.hpp"

namespace mjt {

class OutputSink;   // output_sink.hpp（渲染实现内引入，接口层只前向声明）

}   // namespace mjt

namespace mjt::cli {

/// @brief 退出码约定（README「CLI usage」同文）：0 成功 / 1 用法错误 /
///        2 运行失败。
inline constexpr int kExitOk      = 0;
inline constexpr int kExitUsage   = 1;
inline constexpr int kExitFailure = 2;

// ── info ───────────────────────────────────────────────────────────

/// @brief 一次 jar 概览的普查结果（数据面；渲染与断言共用）。
///
/// 计数与分布一律来自 census 投影（AnalysisDb.censusContainer /
/// censusClasses），字节码版本分布来自 summary_scan 骨架（listClassSummaries，
/// 损坏类按跳过计——不计入版本分布，也不计入类数以外的失败）。各分布只含
/// 非零项，顺序确定：类形态按稳定名字典序；条目形态按 census 投影声明序；
/// 版本按 major 升序。
struct InfoReport
{
    std::string jarPath;   ///< 输入 jar 路径（原样回显）
    ca::i64     entries    = 0; ///< 活跃条目数
    ca::i64     classes    = 0; ///< 类数（关联活跃条目）
    /// @brief 类形态分布（class_kind 稳定名 → 计数，名字典序）。
    std::vector<std::pair<std::string, ca::i64>> classKinds;
    /// @brief 条目路径形态分布（entry_shape facet 稳定名 → 计数，投影声明序）。
    std::vector<std::pair<std::string, ca::i64>> entryShapes;
    /// @brief 字节码版本分布（class file major → 计数，major 升序）。
    std::vector<std::pair<ca::i32, ca::i64>>     versions;
    bool hasManifest = false; ///< 容器内是否有可解析的 MANIFEST.MF
    // ── 批三新增：成员普查 / native 计数 / 来源分布（census 投影同源） ──
    ca::i64 methods       = 0; ///< 方法总数（censusContainer.methods）
    ca::i64 fields        = 0; ///< 字段总数（censusContainer.fields）
    ca::i64 nativeMethods = 0; ///< ACC_NATIVE 方法数（censusContainer.nativeMethods）
    /// @brief 方法可见性分布（public/private/protected/package_private 声明序；
    ///        censusMembers 逐成员 access 按位归类，只含非零项）。
    std::vector<std::pair<std::string, ca::i64>> methodVisibility;
    /// @brief 字段可见性分布（同 methodVisibility 桶序）。
    std::vector<std::pair<std::string, ca::i64>> fieldVisibility;
    /// @brief 类来源分布（library_origin：jdk/known/unknown，LibraryOrigin 枚举
    ///        序；censusClasses 投影层派生列，只含非零项）。
    std::vector<std::pair<std::string, ca::i64>> origins;
};

/// @brief 成员 access 旗标 → 可见性桶名（JVMS：三显式位之外 = 包私有）。
inline std::string memberVisibilityName(ca::i32 access)
{
    if (access & 0x0001) return "public";     // ACC_PUBLIC
    if (access & 0x0002) return "private";    // ACC_PRIVATE
    if (access & 0x0004) return "protected";  // ACC_PROTECTED
    return "package_private";
}

/// @brief 采集 jar 概览：workspace 打开 → 分析库整库导入 → census 投影 +
///        summary_scan 骨架。分析库落在进程临时目录的唯一文件，返回前清理。
/// @note 打不开 jar / 导入或投影失败走错误通道（退出码归 main 定为 2）。
ca::Result<InfoReport, workspace::WorkspaceErrorInfo> collectInfo(const std::string& jarPath);

/// @brief 渲染 info 人类可读文本（i18n 标签 + 确定序数据行）。
void renderInfoText(const InfoReport& report, OutputSink& sink);

/// @brief 渲染 info JSON（键稳定英文；字符串值经 json::escape 转义）。
void renderInfoJson(const InfoReport& report, OutputSink& sink);

// ── mapping ────────────────────────────────────────────────────────

/// @brief mapping 命令错误：类别定退出码分支，message 为已渲染文案。
struct MappingError
{
    enum class Kind
    {
        UnknownFormat, ///< 格式名不识别（用法错误 → 退出码 1）
        ParseFailed,   ///< 输入 mapping 文本解析失败（运行失败 → 退出码 2）
        FormatFailed,  ///< 目标格式生成失败（运行失败 → 退出码 2）
        Io,            ///< mapping 文件或重映射 jar 读写失败（运行失败 → 退出码 2）
    };
    Kind        kind    = Kind::Io;
    std::string message;
};

/// @brief 解析格式名：大小写不敏感地匹配注册表标准格式名，唯一命中返回
///        注册名原形（如 "srg" → "SRG"、"tiny-v2" → "Tiny-V2"）。
///        未注册或命中多个时返回 UnknownFormat。
ca::Result<std::string, MappingError> resolveMappingFormatName(const std::string& name);

/// @brief mapping 文本互转：@p fromName 解析 → @p toName 生成（两端均为
///        注册表注册名；mjbase 统一中间表示承载语义）。
/// @note 输入文本按字节原样交给解析器（行式格式的宽容语义在 mjbase 侧）。
ca::Result<std::string, MappingError> convertMappingText(const std::string& mappingText,
                                                         const std::string& fromName,
                                                         const std::string& toName);

/// @brief 应用 mapping 重映射 jar 的请求（mapping 文本互转的伴随操作）。
struct RemapJarRequest
{
    std::string jarPath;      ///< 输入 jar
    std::string outputPath;   ///< 重映射输出 jar（必须事先不存在）
    std::string mappingPath;  ///< mapping 文件（即 mapping 命令的 -i 输入）
    std::string format;       ///< mapping 格式注册名（resolveMappingFormatName 产物）
    std::string policy;       ///< bytecode-only（缺省）/ reflection / metadata / all
};

/// @brief 应用 mapping 重映射 jar（shared 层 remapJar 直连；mjbase remapper）。
/// @return 成功时 changed = 是否至少修改了一处。
ca::Result<bool, MappingError> remapJarWithMapping(const RemapJarRequest& request);

// ── jar fix ────────────────────────────────────────────────────────

/// @brief 单条修复问题：workspace 诊断原样 + 本次处理结果。
struct FixIssue
{
    std::string uri;      ///< 条目路径
    std::string kind;     ///< 问题类别稳定名（invalid_class / invalid_class_entry /
                          ///  corrupt_output_marker）
    std::string message;  ///< 诊断说明（workspace 扫描器原文）
    bool        removed = false; ///< true = 本次已移除；false = 保留
};

/// @brief 一次 jar 修复的报告（修了什么 / 跳过什么）。
struct FixReport
{
    ca::usize scannedEntries = 0; ///< 扫描的容器条目数
    ca::usize scannedClasses = 0; ///< 扫描的 class 形态条目数
    std::vector<FixIssue> issues; ///< 按 (uri, kind) 稳定序
};

/// @brief 诊断 + 修复一个 jar 并导出：workspace applyEdit（EditCommand::
///        JarRepair，移除无效 class 条目与损坏输出标记条目）→ exportArtifact
///        落盘 @p outJar。
/// @note @p outJar 已存在时在导出前删除（exportArtifact 只接受此前不存在的路径；
///        删除推迟到修复成功之后，坏输入不毁已有产物）；删除失败、打不开
///        jar、修复提交或导出失败走错误通道（退出码 2）。
ca::Result<FixReport, workspace::WorkspaceErrorInfo> fixJar(const std::string& inJar,
                                                            const std::string& outJar);

/// @brief 渲染 jar fix 人类可读报告（i18n 标签；问题按 (uri, kind) 序）。
void renderFixReport(const FixReport& report, const std::string& outJar, OutputSink& sink);

// ── decompile ──────────────────────────────────────────────────────

/// @brief decompile 命令错误：类别定退出码分支，message 为已渲染文案。
///
/// UnknownBackend / InvalidArgument / ClassNotFound 是用户可纠正的输入问题
/// （→ 退出码 1）；Runtime 是后端执行 / IO / 环境问题（→ 退出码 2）。
struct DecompileError
{
    enum class Kind
    {
        UnknownBackend, ///< --decompiler 名不识别（用法错误 → 退出码 1）
        InvalidArgument, ///< 旗标值 / 输出扩展名等用法问题（→ 退出码 1）
        ClassNotFound,  ///< -c 选择器在 jar 内无命中（→ 退出码 1）
        Runtime,        ///< 后端执行 / 暂存 / 落盘失败（→ 退出码 2）
    };
    Kind        kind    = Kind::Runtime;
    std::string message;
};

/// @brief 单类 / 批量反编译的公共选项（CLI 旗标直传；--timeout-ms /
///        --max-memory-mib / --decompiler-jar 与 MCP decompile 契约字段对齐）。
struct DecompileCliOptions
{
    DecompilerKind kind         = DecompilerKind::Cfr; ///< 后端类型
    std::string    backendName  = "cfr";  ///< 稳定后端 ID（"cfr" / "vineflower" / "jcdc"）
    std::string    decompilerJar;         ///< 显式反编译器 JAR；空 = 按发现序解析
    ca::i64        timeoutMs    = 120000; ///< 子进程超时（毫秒）
    ca::i64        maxMemoryMiB = 1024;   ///< 后端 JVM 最大堆（MiB）
};

/// @brief 单类反编译报告：读回的源码与后端元数据（结构断言共用）。
struct SingleDecompileReport
{
    std::string internalName;    ///< 请求的类内部名（原样回显）
    std::string backend;         ///< 稳定后端 ID
    std::string decompilerJar;   ///< 实际使用的反编译器 JAR
    std::string source;          ///< 读回的 Java 源码（仅 stdout 模式填充；
                                 ///  -o 模式由共享层直接落盘，不驻内存）
};

/// @brief 批量反编译的单条跳过记录（敌意名预检产物）。
struct SkippedEntry
{
    std::string entryName;  ///< 条目名（规范 UTF-8 形，超长截断展示）
    std::string reason;     ///< 稳定原因名（segment_too_long / illegal_character /
                            ///  trailing_dot_or_space / parent_or_self / empty_segment）
};

/// @brief 批量反编译报告：后端元数据 + 预检跳过清单（渲染与断言共用）。
struct BatchDecompileReport
{
    std::string              backend;         ///< 稳定后端 ID
    std::string              decompilerJar;   ///< 实际使用的反编译器 JAR
    std::string              outputPath;      ///< 输出目录或 .zip（原样回显）
    bool                     zipOutput = false; ///< true = 输出是 vineflower 归档
    ca::usize                classEntries = 0; ///< jar 内 class 条目总数
    ca::usize                stagedEntries = 0; ///< 交给后端的条目数（原 jar 直跑 =
                                                ///  全部条目；暂存剔除 = 剔除后余量）
    std::vector<SkippedEntry> skipped;      ///< 无法落盘的条目（按名字典序）
};

/// @brief 解析 --decompiler 旗标值：ASCII 小写归一后匹配 cfr / vineflower / jcdc。
///        未知名返回 UnknownBackend（退出码 1）。
ca::Result<DecompilerKind, DecompileError> parseDecompilerBackendName(const std::string& name);

/// @brief 敌意名预检：条目名（原始字节）能否在本机文件系统落盘。
///
/// 规范化（合法 UTF-8 原样 / GBK → UTF-8 / 残字节 U+FFFD）后按 '/' 与 '\\'
/// 分段，任一段命中以下之一即不安全：空段；"." / ".."；UTF-16 单元数
/// 超 255；含文件系统非法字符（< > : " | ? * 与控制字符）；以 '.' 或 ' '
/// 结尾（Windows 落盘时被剥离，文件无法存在）。单类模式不经本检查（短
/// 安全名方案天然免疫）；批量模式据此跳过并披露。
/// @return safe = 是否可落盘；reason = 不安全时的稳定原因名（i18n key 后缀）。
struct EntryNameSafety
{
    bool        safe   = true;
    std::string reason;
};
EntryNameSafety classifyEntryNameForDisk(const std::string& rawEntryName);

/// @brief 统计 jar 内 class 条目数（后缀 ".class" 大小写不敏感；仅 central
///        directory 元数据，不解压）。批量模式 --all 缺席提示带出类数。
ca::Result<ca::usize, DecompileError> countJarClassEntries(const std::string& jarPath);

/// @brief 单类反编译：目标类字节写临时目录短安全名（Target.class），后端以
///        单文件输入 + classpath 指向原 jar 保跨类型上下文，产出读回。
///
/// 敌意名免疫（#63 A/B 根治）：类名从不落盘——短安全名是唯一物化路径；
/// 整 jar 不跑批量，敌意类无法级联毒化。
/// @param jarPath       输入 jar（同时作为 classpath 上下文直传后端）
/// @param internalName  类内部名（slash 形；workspace readClassBytes 解析）
/// @param outputJavaPath 输出 .java 路径；空 = 读入内存走 stdout（此时输出
///                        落在临时目录，返回前清理）
/// @param options       后端 / 超时 / 内存选项
ca::Result<SingleDecompileReport, DecompileError> decompileSingleClass(
    const std::string& jarPath, const std::string& internalName,
    const std::string& outputJavaPath, const DecompileCliOptions& options);

/// @brief 批量反编译：敌意名预检 → （有敌意名时）字节级快照暂存 jar（剔除
///        无法落盘条目，其余条目零漂移拷贝）→ 后端原生批量（outdir 落盘 /
///        vineflower 归档输出）。
/// @param jarPath    输入 jar
/// @param outputPath 输出目录或 .zip（.zip 仅 vineflower）
/// @param options    后端 / 超时 / 内存选项
ca::Result<BatchDecompileReport, DecompileError> decompileJarBatch(
    const std::string& jarPath, const std::string& outputPath,
    const DecompileCliOptions& options);

/// @brief 渲染单类反编译 stdout 摘要（仅 -o 模式；stdout 源码模式不落摘要）。
void renderSingleDecompileSummary(const SingleDecompileReport& report, OutputSink& sink);

/// @brief 渲染批量反编译 stdout 摘要（后端 / 类数 / 落点）。
void renderBatchDecompileSummary(const BatchDecompileReport& report, OutputSink& sink);

/// @brief 渲染批量预检跳过清单到 stderr（N 项 + 单类模式 / jasm 指路）。
void renderSkippedEntries(const BatchDecompileReport& report, OutputSink& sink);

// ── strings ────────────────────────────────────────────────────────

/// @brief strings 命令错误：类别定退出码分支，message 为已渲染文案。
struct StringsError
{
    enum class Kind
    {
        InvalidArgument, ///< --min-len 等旗标值非法（用法错误 → 退出码 1）
        ClassNotFound,   ///< --class 选择器在 jar 内无命中（→ 退出码 1）
        Runtime,         ///< jar 打开 / 读取失败（→ 退出码 2）
    };
    Kind        kind    = Kind::Runtime;
    std::string message;
};

/// @brief 一条去重后的字符串值：jar 级出现次数 + 常量池种类计数。
struct StringsValue
{
    ca::i64 count = 0;  ///< 出现次数（常量池条目数，跨类累加）
    ca::i64 utf8 = 0;   ///< 其中裸 CONSTANT_Utf8 条目数
    ca::i64 string = 0; ///< 其中 CONSTANT_String（字面量）条目数
    ca::i64 cls = 0;    ///< 其中 CONSTANT_Class（类名引用）条目数
    std::string value;  ///< 字符串值（规范 UTF-8）
};

/// @brief 一次常量池字符串抽取的报告（计数面 + 去重列表；渲染与断言共用）。
struct StringsReport
{
    std::string jarPath;      ///< 输入 jar 路径（原样回显）
    std::string classFilter;  ///< --class 内部名；空 = 整 jar
    ca::i64     classesScanned = 0; ///< 实际扫描的 class（可解析骨架）
    ca::i64     classesSkipped = 0; ///< jar 内 class 条目 − 可解析类（畸形跳过）
    ca::i64     occurrences    = 0; ///< 过滤后抽取条目总数（= Σ count）
    ca::i64     uniqueValues   = 0; ///< 去重值数（= values.size()）
    /// @brief 去重值列表：count 降序、value 升序（全序，可 diff）。
    std::vector<StringsValue> values;
};

/// @brief 抽取常量池字符串：shared 层 StringExtractor 逐类抽取（Utf8 / String /
///        Class 三种条目），--min-len 按规范 UTF-8 字节长度过滤，值跨类去重。
/// @param jarPath     输入 jar
/// @param classFilter --class 内部名（slash 形）；空 = 整 jar。类经
///                    listClassSummaries 骨架匹配，无命中返回 ClassNotFound
/// @param minimumLen  最短长度（字节），< 1 返回 InvalidArgument
/// @note 契约：一个字符串字面量同时产出 CONSTANT_String 与其背书
///       CONSTANT_Utf8 两个条目（kind 计数披露口径）；畸形 class 静默跳过并
///       计入 classesSkipped。
ca::Result<StringsReport, StringsError> collectStrings(const std::string& jarPath,
                                                       const std::string& classFilter,
                                                       ca::i64 minimumLen);

/// @brief 渲染 strings 人类可读报告（i18n 头 + count 值列表；值经控制字符
///        转义保证单行）。
void renderStringsText(const StringsReport& report, OutputSink& sink);

/// @brief 渲染 strings JSON（键稳定英文；值经 json::escape 转义）。
void renderStringsJson(const StringsReport& report, OutputSink& sink);

// ── xref ───────────────────────────────────────────────────────────

/// @brief xref 命令错误：类别定退出码分支，message 为已渲染文案。
struct XrefError
{
    enum class Kind
    {
        InvalidArgument, ///< --direction / --limit 等旗标值非法（用法 → 退出码 1）
        ClassNotFound,   ///< -c 类在 jar 内无命中（→ 退出码 1）
        Runtime,         ///< jar 打开 / 扫描失败（→ 退出码 2）
    };
    Kind        kind    = Kind::Runtime;
    std::string message;
};

/// @brief 一次 xref 查询的报告（输出有界；渲染与断言共用）。
struct XrefReport
{
    std::string jarPath;      ///< 输入 jar 路径（原样回显）
    std::string className;    ///< -c 类内部名（原样回显）
    std::string member;       ///< -m 成员选择器原样（空 = 类级）
    std::string direction;    ///< "to" / "from" / "both"
    ca::i64     limit    = 200; ///< --limit（生效值）
    ca::i64     total    = 0;   ///< 截断前匹配总数
    bool        truncated = false; ///< true = total > limit，references 只含前 limit 条
    /// @brief 引用事实（workspace sortReferences 稳定序后截断；kind =
    ///        method / field / type，target = owner.name(desc) 或类型名）。
    std::vector<workspace::ReferenceInfo> references;
};

/// @brief 类/成员反查：workspace ReferenceScan 全量索引 → to（编码目标命中）/
///        from（该类方法体发出）方向的引用事实。
///
/// 语义（reference_scan.cpp 面）：to = 类型引用目标命中 + 方法/字段引用 owner
/// 命中（成员选择器在场时收缩到 name[/descriptor] 精确目标）；from = 该类全部
/// 方法发出的方法/字段/类型引用（成员选择器过滤 sourceMethod[/Descriptor]）。
/// both = 两向并集（自引用去重）。类经 listClassSummaries 骨架匹配，无命中
/// 返回 ClassNotFound；输出必经 --limit 截断（混淆 jar 反查可达百万行）。
ca::Result<XrefReport, XrefError> collectXref(const std::string& jarPath,
                                              const std::string& className,
                                              const std::string& member,
                                              const std::string& direction, ca::i64 limit);

/// @brief 渲染 xref 人类可读报告（头部计数 + 截断提示 + 引用行）。
void renderXrefText(const XrefReport& report, OutputSink& sink);

/// @brief 渲染 xref JSON（键稳定英文；文本字段经 json::escape 转义）。
void renderXrefJson(const XrefReport& report, OutputSink& sink);

// ── classes ────────────────────────────────────────────────────────

/// @brief classes 命令错误：类别定退出码分支，message 为已渲染文案。
struct ClassesError
{
    enum class Kind
    {
        InvalidArgument, ///< --kind / --origin / --limit 值非法（用法 → 退出码 1）
        Runtime,         ///< jar 打开 / 分析库失败（→ 退出码 2）
    };
    Kind        kind    = Kind::Runtime;
    std::string message;
};

/// @brief 一行类清单：内部名 + 形态 + 来源（censusClasses 投影行的 CLI 切面）。
struct ClassesRow
{
    std::string internalName; ///< 类内部名（slash 形）
    std::string kind;         ///< classKindName 稳定名（class / interface / …）
    std::string origin;       ///< libraryOriginName 稳定名（jdk / known / unknown）
};

/// @brief 一次类清单查询的报告（渲染与断言共用；-o 落盘路径见
///        collectClasses）。
struct ClassesReport
{
    std::string jarPath;      ///< 输入 jar 路径（原样回显）
    std::string kindFilter;   ///< --kind 归一值（小写；空 = 不过滤）
    std::string originFilter; ///< --origin 归一值（小写；空 = 不过滤）
    ca::i64     classesInJar = 0; ///< jar 内类总数（过滤前口径，对账列）
    ca::i64     total        = 0; ///< 过滤后总行数
    ca::i64     limit        = 10000; ///< --limit（生效值；stdout 防呆上限，
                                          ///  -o 全量路径归一为清单行数）
    bool        truncated    = false; ///< true = total > limit，classes 只含前 limit 行
    /// @brief 类清单行（censusClasses 库序 = internal_name BINARY 升序）。
    std::vector<ClassesRow> classes;
};

/// @brief 类清单：census 投影（censusClasses，kind / origin 为投影层派生列）
///        全行过滤 + --limit 截断。
///
/// 过滤项组合语义：--kind 与 --origin 都给 = 交集；两者都缺 = 全量。
/// --kind / --origin 值大小写不敏感，未知名返回 InvalidArgument；--limit < 1
/// 返回 InvalidArgument。stdout 列表经 --limit 截断（缺省 10000 防呆——
/// 更大的 jar 无界输出是事故）；CLI 的 -o 落盘路径以无界上限调入取完整
/// 清单（main.cpp runClasses）。
ca::Result<ClassesReport, ClassesError> collectClasses(const std::string& jarPath,
                                                       const std::string& kindFilter,
                                                       const std::string& originFilter,
                                                       ca::i64 limit);

/// @brief 渲染 classes 人类可读报告（头部计数 + 截断提示 + 行列表）。
void renderClassesText(const ClassesReport& report, OutputSink& sink);

/// @brief 渲染 classes JSON（键稳定英文；文本字段经 json::escape 转义）。
void renderClassesJson(const ClassesReport& report, OutputSink& sink);

// ── disasm ─────────────────────────────────────────────────────────

/// @brief disasm 命令错误：类别定退出码分支，message 为已渲染文案。
struct DisasmError
{
    enum class Kind
    {
        ClassNotFound, ///< -c 类在 jar 内无命中（→ 退出码 1）
        Runtime,       ///< jar 打开 / 反汇编失败（→ 退出码 2）
    };
    Kind        kind    = Kind::Runtime;
    std::string message;
};

/// @brief 单类 jasm 反汇编报告（渲染与断言共用）。
struct DisasmReport
{
    std::string jarPath;      ///< 输入 jar 路径（原样回显）
    std::string internalName; ///< 请求的类内部名（原样回显）
    std::string jasm;         ///< 反汇编文本（UTF-8；stdout 模式原样直出）
};

/// @brief 单类 jasm 文本：workspace getJasm（MCP class_query kind=jasm 同源
///        能力）读面，类选择器经 classInfo（URI / 内名双解析）定位。
/// @note 类不存在返回 ClassNotFound（退出码 1）；类字节可解析但反汇编失败
///       返回 Runtime（退出码 2）。纯文本面，不依赖反编译器。
ca::Result<DisasmReport, DisasmError> collectDisasm(const std::string& jarPath,
                                                    const std::string& internalName);

/// @brief 渲染 disasm 文本（jasm 全文原样直出，不混头部——重定向取文件语义）。
void renderDisasmText(const DisasmReport& report, OutputSink& sink);

// ── subs ───────────────────────────────────────────────────────────

/// @brief subs 命令错误：类别定退出码分支，message 为已渲染文案。
struct SubsError
{
    enum class Kind
    {
        ClassNotFound, ///< -c 类在 jar 内无命中（→ 退出码 1）
        Runtime,       ///< jar 打开 / 解析失败（→ 退出码 2）
    };
    Kind        kind    = Kind::Runtime;
    std::string message;
};

/// @brief 一行成员清单：kind + 名字 + 描述符 + access 旗标 + native 标记。
struct SubsMember
{
    std::string kind;        ///< "field" / "method"（censusMembers kind 小写 token）
    std::string name;
    std::string descriptor;
    ca::i32     access = 0;  ///< 原始访问旗标（JVMS 位掩码，十进制渲染）
    bool        native = false; ///< 方法 ACC_NATIVE 派生；字段恒 false
};

/// @brief 单类成员清单报告（渲染与断言共用）。
struct SubsReport
{
    std::string jarPath;      ///< 输入 jar 路径（原样回显）
    std::string internalName; ///< 请求类的内部名（classInfo 解析后的规范内名；
                              ///  URI 形选择器会被解析，与请求原文可不同）
    std::string kind;         ///< 类形态稳定名（classifyClassKind，census 同源派生）
    ca::i64     fields  = 0;  ///< 字段数（= members 中 kind == field 行数）
    ca::i64     methods = 0;  ///< 方法数（= members 中 kind == method 行数）
    /// @brief 成员行：字段在前（censusMembers kind 编码序），各自保持
    ///        classfile 声明序。
    std::vector<SubsMember> members;
};

/// @brief 单类成员清单：workspace classInfo 读面（字段/方法名、描述符、access
///        旗标、native 派生）+ 类形态 classifyClassKind 派生。
///
/// 单类查询走 classInfo 直读（声明序保留），不走 censusMembers（那需要整
/// jar 分析库导入，单类成本不成比例）；类形态与 classes 命令同源同果。
/// 类不存在返回 ClassNotFound（退出码 1）。纯文本面，不依赖反编译器。
ca::Result<SubsReport, SubsError> collectSubs(const std::string& jarPath,
                                              const std::string& internalName);

/// @brief 渲染 subs 人类可读报告（头部计数 + 成员行列表）。
void renderSubsText(const SubsReport& report, OutputSink& sink);

/// @brief 渲染 subs JSON（键稳定英文；文本字段经 json::escape 转义）。
void renderSubsJson(const SubsReport& report, OutputSink& sink);

// ── hexdump ────────────────────────────────────────────────────────

/// @brief hexdump 命令错误：类别定退出码分支，message 为已渲染文案。
struct HexdumpError
{
    enum class Kind
    {
        InvalidArgument, ///< --offset/--length/--width 值非法或缺 <class>（用法 → 退出码 1）
        ClassNotFound,   ///< <class> 选择器在 jar 内无命中（→ 退出码 1）
        Runtime,         ///< jar 打开 / 类字节读取失败（→ 退出码 2）
    };
    Kind        kind    = Kind::Runtime;
    std::string message;
};

/// @brief hexdump 渲染窗口选项（CLI 旗标直传；collect 校验 + 渲染共用输入）。
struct HexdumpOptions
{
    ca::i64 offset    = 0;    ///< 起始偏移（字节）
    ca::i64 length    = -1;   ///< 显示字节数；-1 = 到类字节末尾（哨兵值）
    ca::i64 width     = 16;   ///< 每行字节数
    bool    showAscii = true; ///< false = --no-ascii（隐藏右侧 ASCII 列）
};

/// @brief 单类原始字节报告（渲染与断言共用）。
struct HexdumpReport
{
    std::string jarPath;       ///< 输入 jar 路径（原样回显）
    std::string classUri;      ///< 命中的资源 URI（容器条目 URI，含 .class 后缀）
    std::string internalName;  ///< 命中类的内部名
    std::vector<ca::u8> bytes; ///< 类原始字节
};

/// @brief 采集单类原始字节：workspace 打开 → listClassSummaries 类选择 → readClassBytes 读取类原始字节。
///
/// 类选择：URI / 内名精确命中优先；无精确命中取首个包含选择器的子串候选（listClassSummaries 内名字典序，多命中属歧义输入，取确定性序，单命中语义不变）。旗标防御：负 offset 会负索引字节向量；width <= 0 让渲染步进不推进；length < -1 与 -1（整个 class）哨兵冲突，一律 InvalidArgument（退出码 1）。
ca::Result<HexdumpReport, HexdumpError> collectHexdump(const std::string& jarPath,
                                                       const std::string& classSelector,
                                                       const HexdumpOptions& options);

/// @brief 渲染 xxd 风格十六进制转储。
///
/// 行形：`%08zx` 偏移 + 两空格，每字节 `%02x `，半行宽处补一空格分隔；短行按每缺一字节三空格补齐（半行宽处再多一空格）；ASCII 列 ` |...|` 以空格补到行宽。偏移越过类字节末尾时只输出越界提示行（调用方保持退出码 0）。
void renderHexdumpText(const HexdumpReport& report, const HexdumpOptions& options,
                       OutputSink& sink);

// ── eval ───────────────────────────────────────────────────────────

/// @brief eval 命令错误：类别定退出码分支，message 为已渲染文案。
struct EvalError
{
    enum class Kind
    {
        InvalidVarSpec,   ///< --vars 绑定串非法（用法 → 退出码 1）
        MissingExpression, ///< 缺 <expression> 位置参数（用法 → 退出码 1）
        ParseFailed,      ///< 表达式解析失败（用法 → 退出码 1）
        EvalFailed,       ///< 求值失败（深度超限等，非 unbound；运行 → 退出码 2）
    };
    Kind        kind    = Kind::EvalFailed;
    std::string message;
};

/// @brief 一次表达式求值的结果（渲染与断言共用）。
///
/// evaluated = true 时 value 为数值结果；false 时 symbolic 承载含未绑定变量表达式的 S-表达式形（unbound 不算失败，退出码 0）。
struct EvalOutcome
{
    bool        evaluated = false;
    ca::i64     value     = 0; ///< evaluated 时的数值结果
    std::string symbolic;     ///< !evaluated 时的 S-表达式
};

/// @brief 求值/化简表达式：--vars 绑定解析 → parseExpression →（可选）simplify → eval。unbound（未绑定变量）走 symbolic 输出通道（成功）；其余求值错误（深度超限等）返回 EvalFailed（退出码 2）。
/// @param exprStr   表达式文本（中缀；十进制/十六进制字面量与变量）
/// @param varSpec   "k=v,k2=v2" 形绑定串；空 = 无绑定
/// @param doSimplify 求值前先过化简管线（折叠常量、应用恒等式）
ca::Result<EvalOutcome, EvalError> evaluateExpression(const std::string& exprStr,
                                                      const std::string& varSpec,
                                                      bool doSimplify);

/// @brief 渲染求值结果：数值一行（%lld）或 S-表达式一行。
void renderEvalOutcome(const EvalOutcome& outcome, OutputSink& sink);

/// @brief 渲染 eval 详细用法（算子表与示例；i18n 键 mjt.cmd.eval.out.usage）。缺表达式错误后的伴随输出。
void renderEvalUsage(OutputSink& sink);

// ── reflect ────────────────────────────────────────────────────────

/// @brief reflect 命令错误：类别定退出码分支，message 为已渲染文案。
struct ReflectError
{
    enum class Kind
    {
        Runtime, ///< jar 打开 / 分析器构建失败（→ 退出码 2）
    };
    Kind        kind    = Kind::Runtime;
    std::string message;
};

/// @brief 一行反射 sink 绑定（mjbase ReflectionBindingAnalyzer 输出转成 std::string 的副本；collect 拷贝驻留，与分析器寿命解耦）。
struct ReflectBindingRow
{
    std::string state;      ///< 绑定状态稳定名（resolved / pending / unsupported）
    std::string api;        ///< API 分类短标签（"Class.forName" 形态）
    std::string reason;     ///< Pending 原因稳定名（非 Pending 为空串）
    std::string className;  ///< 解析出的目标类名（未知为空）
    std::string memberName; ///< 解析出的成员名（未知为空）
    ca::i32     paramTypeCount = -1; ///< Class[] 槽可判定的参数类型个数
    bool        resolved       = false; ///< true = 目标已在容器内验证存在
    std::string targetOwner;    ///< Resolved 时的目标成员（否则空）
    std::string targetName;
    std::string targetDesc;
    std::string siteClass;  ///< 调用点位置（来源类 / 方法 / 描述符）
    std::string siteMethod;
    std::string siteDesc;
    ca::i32     siteOpcode = -1;
    ca::i32     siteInsn   = -1;
};

/// @brief 汇总计量（analyzer summarize 结果；渲染与断言共用）。
struct ReflectSummary
{
    ca::usize total       = 0; ///< sink 调用点总数
    ca::usize resolved    = 0;
    ca::usize pending     = 0;
    ca::usize unsupported = 0;
    double    ratePercent = 0.0; ///< resolved 比例（0-100；无 sink 为 0）
    /// @brief API 分布（枚举序，只含非零项）：API 标签 → 计数。
    std::vector<std::pair<std::string, ca::usize>> apiDistribution;
};

/// @brief 一次反射绑定分析的报告（渲染与断言共用）。
struct ReflectReport
{
    std::string       jarPath;    ///< 输入 jar 路径（原样回显）
    std::string       classFilter; ///< --bindings-of 归一后内部名；空 = 不过滤
    ca::usize         classCount = 0; ///< 容器内 class 形态资源数（摘要行用）
    ReflectSummary    summary;
    std::vector<ReflectBindingRow> bindings; ///< 过滤后绑定（扫描顺序）
};

/// @brief 反射 sink 调用点绑定分析：workspace 打开 → 容器类计数 → mjbase ReflectionBindingAnalyzer 全量构建（无法判定的绑定按 Pending 呈现，不误报）→ --unresolved / --bindings-of 过滤。
/// @param classFilter 来源类过滤（归一后内部名精确匹配；空 = 不过滤）
/// @note jar 打开与构建失败都走 Runtime（退出码 2）；类选择器归一是用法校验（归 main）。
ca::Result<ReflectReport, ReflectError> collectReflect(const std::string& jarPath,
                                                       bool unresolvedOnly,
                                                       const std::string& classFilter);

/// @brief 渲染 `# ` 摘要三行（扫描类数 / 三态计数与 resolved 比例 / API 分布）。--json 时调用方把它指到 stderr（stdout 保持纯 JSON 流）。
void renderReflectSummary(const ReflectReport& report, OutputSink& sink);

/// @brief 渲染人类可读全文（摘要 + 绑定行 `[state] api ... <- site insn=N`；无匹配时输出 none 提示行）。
void renderReflectText(const ReflectReport& report, OutputSink& sink);

/// @brief 渲染 JSON 数组（键稳定英文；文本字段经 json::escape 转义；仅数组本体，摘要行走 stderr 由 main 分工）。
void renderReflectJson(const ReflectReport& report, OutputSink& sink);

}   // namespace mjt::cli
