/// @file        commands.hpp
/// @brief       `mjt` CLI 命令核心面：info / mapping / jar fix 三个命令的
///              纯逻辑入口与报告结构（main 与 gtest 共链）。
///
/// 分层契约：本模块只做「打开制品 → 调 workspace/mjbase 既有面 → 产出可
/// 断言的报告结构」，命令行解析（ca::opt）与退出码映射归 main.cpp；渲染
/// （文本/JSON）为纯函数，测试直接断言结构与渲染输出。
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "libca/core/result.hpp"
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

}   // namespace mjt::cli
