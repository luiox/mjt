/// @file        commands.hpp
/// @brief       `mjt` CLI 命令核心面：info / mapping / jar fix / decompile
///              四个命令的纯逻辑入口与报告结构（main 与 gtest 共链）。
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
};

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

/// @brief 诊断 + 修复一个 jar 并导出：workspace applyJarRepair（移除无效
///        class 条目与损坏输出标记条目）→ exportJar 落盘 @p outJar。
/// @note @p outJar 若已存在会被先删除（exportJar 只接受此前不存在的路径）；
///        打不开 jar、修复提交或导出失败走错误通道（退出码 2）。
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
    std::string    backendName  = "cfr";  ///< 稳定后端 ID（"cfr" / "vineflower"）
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
    ca::usize                stagedEntries = 0; ///< 交给后端的条目数（原 jar 直跑 = classEntries）
    std::vector<SkippedEntry> skipped;      ///< 无法落盘的条目（按名字典序）
};

/// @brief 解析 --decompiler 旗标值：ASCII 小写归一后匹配 cfr / vineflower。
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

}   // namespace mjt::cli
