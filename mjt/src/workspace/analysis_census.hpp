/// @file        analysis_census.hpp
/// @brief       census 普查导出出口：AnalysisDb 投影 → 确定性 TSV 落盘。
///
/// 位置契约（#39，对齐 #3 结论）：SDK/CLI 工具面，不进 MCP 热路径。本出口
/// 只做投影消费与渲染——数据一律来自 AnalysisDb 查询面（#121 §5 B4：库是
/// 事实源，导出 schema 是投影，不重新扫描容器），判定核心（路径形态、类
/// 形态）在 mjbase（entry_shape / class_kind）。
///
/// 确定性契约（#39 设计要点 2，B4 验收）：
/// - 表头与列序版本化（列集/列序/值渲染规则变更递增版本行）；
/// - 行序 = 库序（censusClasses 的 internal_name BINARY 升序；汇总面按
///   固定 metric 序 + 分布列稳定序）；
/// - 文本列统一 `mjbase::mapping::IdentifierUtil::escapeUnicode` 转义
///   （\uXXXX 大写，与 rename mapping 格式同一套转义，全链路不做两套），
///   转义后不含制表/换行/空白，天然 TSV 安全；
/// - 超长单元格折叠（v2 起）：转义后超 kCensusCellFoldBytes 的单元格
///   渲染为 `#folded:len=<原始字节数>:sha256=<原始字节摘要>` 确定形。
///   空格名混淆 jar 实测 21845 字节名转义成 131KB 单行，diff 基线不可用；
///   折叠形保身份（len + 原始字节 SHA-256）丢字面，全名仍在库内事实层。
/// - 字节面：UTF-8 无 BOM、LF 行尾、binary 写出（无 CRT 文本模式翻译），
///   不含时间戳——同输入两次导出逐字节一致。
///
/// 汇总面（container_census.tsv）是简报补充的最小可用面（#39 首期只定
/// class_census.tsv），metric 集后续可追加：消费方按 `# container-census vN`
/// 版本行分支，行内不依赖全集。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"

#include "mjbase/project/analysis_db.hpp"

namespace mjt::workspace {

/// @brief class_census.tsv schema 版本（表头首行外显 `# class-census v<N>`）。
///        v2 = 超长单元格折叠（#folded 确定形，见文件头契约）。
inline constexpr ca::u32 kClassCensusSchemaVersion = 2;

/// @brief container_census.tsv schema 版本（`# container-census v<N>`）。
///        v2 与 class 面同步引入折叠。
inline constexpr ca::u32 kContainerCensusSchemaVersion = 2;

/// @brief 单元格折叠阈值：转义后字节数超过此值的单元格渲染为
///        `#folded:len=<原始字节数>:sha256=<原始字节摘要>`。
inline constexpr ca::usize kCensusCellFoldBytes = 4096;

/// @brief census 导出失败来源。
enum class CensusExportErrorKind : ca::u8
{
    Database, ///< 分析数据库查询失败
    Invalid,  ///< 输入未登记等调用方错误
    Io,       ///< 输出目录/文件写出失败
};

/// @brief census 导出诊断（message 为 std::string：Result 错误通道要求可拷贝）。
struct CensusExportError
{
    CensusExportErrorKind kind = CensusExportErrorKind::Database;
    std::string           message;
};

/// @brief 一次 census 导出的两表文本（不落盘；测试与自定义出口可直取）。
struct CensusExportText
{
    std::string classCensusTsv;     ///< class_census.tsv 内容（含版本化表头）
    std::string containerCensusTsv; ///< container_census.tsv 内容
};

/// @brief 一次 census 导出的落盘位置。
struct CensusExportPaths
{
    std::string classCensus;     ///< class_census.tsv 路径（directory 拼接）
    std::string containerCensus; ///< container_census.tsv 路径
};

/// @brief 渲染 class census TSV（纯函数面，#39 列契约）。
///
/// 行序按传入序渲染（库序 = internal_name 升序，不重排）；列 =
/// internal_name / kind / access / parent / iface_count / method_count /
/// field_count / native_method_count / byte_size / zip_entry。kind 取
/// 投影行派生形态；access 为旗标摘要（JVMS 位序命名，未知名按 0x hex）；
/// zip_entry 取 regular/irregular。
std::string renderClassCensusTsv(
    const std::vector<mjbase::project::DbClassCensusRow>& rows);

/// @brief 渲染 container census TSV（纯函数面，简报补充汇总面）。
///
/// metric 列固定序（容器身份 → 条目面 → kind 分布 → 形态分布 → 事实面），
/// 分布行只含非零 facet，kind 按名升序、形态按枚举声明序。
std::string renderContainerCensusTsv(const mjbase::project::DbContainerCensus& summary);

/// @brief 一次普查导出：投影（censusClasses + censusContainer）+ 两表渲染。
///
/// 连接未打开或 @p inputId 未登记 → `Invalid`；查询失败 → `Database`。
ca::Result<CensusExportText, CensusExportError> renderCensus(
    const mjbase::project::AnalysisDb& db, ca::i64 inputId);

/// @brief 一次普查导出并落盘：directory 下写 class_census.tsv 与
///        container_census.tsv（目录不存在则创建；binary + LF，见文件头
///        确定性契约）。
ca::Result<CensusExportPaths, CensusExportError> exportCensus(
    const mjbase::project::AnalysisDb& db, ca::i64 inputId, const std::string& directory);

/// @brief 按容器来源路径解析 input_id（导入桥以 workspace.sourcePath() 为
///        inputs.path 身份键登记）。未登记 → `Invalid`。
ca::Result<ca::i64, CensusExportError> resolveCensusInput(
    const mjbase::project::AnalysisDb& db, const ca::str::Utf8StringRef& sourcePath);

}   // namespace mjt::workspace
