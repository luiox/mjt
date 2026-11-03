/// @file        analysis_ingest.hpp
/// @brief       工作区 → 分析数据库的整库导入桥。
///
/// 职责边界：workspace 负责从 class 字节解码（ClassInfo + ReferenceScan），
/// mjbase::project::AnalysisDb 负责持久化与查询。本桥把两者接起来：
/// - 主容器条目身份（jar 内文件身份：path/kind/size/sha256/regular）写入
///   `ensureInput` + `ingestEntries`。条目遍历走容器面（`listResources` +
///   `readResource`），内容锚为精确字节 SHA-256；输入行的内容锚是容器
///   canonical 序列化（analysis-project-schema-v2 §1.6 口径）的 SHA-256。
///   形态判定（regular 旗标）是本桥的最小口径，见实现内注释。
/// - 主容器全部 class 的头事实与成员清单写入 `ingestClasses`；class 条目
///   通过 `DbClassInput::entryId` 与 `classes.entry_id` 关联。
/// - 主容器 class 发出的直接引用（方法/字段/类型）写入 `replaceReferences`；
/// - supporting JAR 只作为引用目标出现，其自身 class 与条目均不登记（支持
///   库可能包含整个 JDK，不属于工程身份面）。
///
/// 写入顺序：producer 注册（key=`mjt.workspace.ingest`，trusted，幂等）→
/// full_import 批开启 → 条目身份 → 类事实 → 引用 → 批次收尾（计数从
/// AnalysisIngestStats 回填）。批次是逻辑写入运行不是事务：各写入口显式挂
/// batchId，中途失败留下 finished_at 为空的残批（合法审计状态，不 close）。
/// 条目身份 → 类事实 → 引用各步各自单事务；中途失败时前面的步已提交，引用
/// 仍是旧集合。整库重建可安全重跑：输入按 path 复用 input_id，条目按
/// (input_id, path, sha256) 匹配——同内容重导入 id 不漂，消失条目进墓碑不删
/// 行；类身份 upsert 不改 ID，引用是整体替换；每次重导入各落一个新批次。
#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/core/result.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/project/analysis_db.hpp"
#include "workspace_types.hpp"

namespace mjt::workspace {

class WorkspaceBase;

/// @brief 导入失败来源。
enum class AnalysisIngestErrorKind : ca::u8
{
    Workspace, ///< 工作区读取/扫描失败
    Database,  ///< 分析数据库写入失败
};

/// @brief 导入诊断（message 为 std::string：Result 错误通道要求可拷贝）。
struct AnalysisIngestError
{
    AnalysisIngestErrorKind kind = AnalysisIngestErrorKind::Database;
    std::string             message;
};

/// @brief 一次整库导入的规模统计。
struct AnalysisIngestStats
{
    ca::usize classes    = 0;
    ca::usize members    = 0;
    ca::usize references = 0;
    /// @brief 主容器登记的条目数（ingestEntries 的 total）。
    ca::usize entries    = 0;
    /// @brief 本次新分配 entry_id 的条目数；同内容重导入应为 0。
    ca::usize createdEntries    = 0;
    /// @brief 本次标记消失（墓碑）的既有条目数。
    ca::usize tombstonedEntries = 0;
};

/// @brief 把工作区主容器的类/成员/直接引用整库写入分析数据库。
///
/// 要求 @p db 已以读写方式打开；本函数不创建/关闭数据库。
ca::Result<AnalysisIngestStats, AnalysisIngestError> ingestIntoAnalysisDb(
    const WorkspaceBase& workspace, mjbase::project::AnalysisDb& db);

}   // namespace mjt::workspace
