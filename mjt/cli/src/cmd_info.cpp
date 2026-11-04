/// @file        cmd_info.cpp
/// @brief       `mjt info` 命令核心：jar 概览（条目/类/class_kind/entry_shape/
///              字节码版本/manifest）。
///
/// 数据源接线：census 投影（censusContainer + censusClasses，事实源是经
/// ingestIntoAnalysisDb 落库的 AnalysisDb）+ summary_scan 骨架
/// （listClassSummaries）+ manifest 查询。分析库是进程临时目录的一次性
/// 文件，collectInfo 返回前删除（含 -wal/-shm）。
#include "commands.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#define MJT_CLI_GETPID _getpid
#else
#include <unistd.h>
#define MJT_CLI_GETPID getpid
#endif

#include "mjbase/project/analysis_db.hpp"
#include "mjbase/project/class_kind.hpp"
#include "shared/i18n_errors.hpp"
#include "shared/json_util.hpp"
#include "shared/output_sink.hpp"
#include "workspace/analysis_census.hpp"
#include "workspace/analysis_ingest.hpp"
#include "workspace/workspace.hpp"

namespace mjt::cli {
namespace {

/// Utf8String/UTF-8 std::string → 借用视图（与 workspace 测试同款构造）。
ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// info 用的一次性分析库文件路径：临时目录 + 进程 id + 单调序号，进程内唯一。
std::string tempDbPath()
{
    static unsigned long long sequence = 0;
    const auto path = std::filesystem::temp_directory_path() /
                      ("mjt-info-" + std::to_string(MJT_CLI_GETPID()) + "-" +
                       std::to_string(++sequence) + ".mjdb");
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return path.string();
}

/// RAII：分析库一次性文件清理（主文件 + WAL/SHM 伴生，失败静默）。
class TempDbGuard
{
public:
    explicit TempDbGuard(std::string path)
        : path_(std::move(path))
    {}
    ~TempDbGuard()
    {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
        std::filesystem::remove(path_ + "-wal", ec);
        std::filesystem::remove(path_ + "-shm", ec);
    }
    TempDbGuard(const TempDbGuard&)            = delete;
    TempDbGuard& operator=(const TempDbGuard&) = delete;
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

/// AnalysisDb 错误（Utf8String message）→ workspace 错误值（i18n 模板承载）。
workspace::WorkspaceErrorInfo dbError(const mjbase::project::AnalysisDbError& cause)
{
    workspace::WorkspaceErrorInfo info;
    info.code    = workspace::WorkspaceError::Internal;
    info.message = shared::trfStr("mjt.cli.err.info_db", {cause.message.to_std_string()});
    return info;
}

/// 导入桥错误 → workspace 错误值（message 原样透传，已是呈现面文案）。
workspace::WorkspaceErrorInfo ingestError(const workspace::AnalysisIngestError& cause)
{
    workspace::WorkspaceErrorInfo info;
    info.code    = workspace::WorkspaceError::Internal;
    info.message = cause.message;
    return info;
}

/// census 投影错误 → workspace 错误值（message 原样透传）。
workspace::WorkspaceErrorInfo censusError(const workspace::CensusExportError& cause)
{
    workspace::WorkspaceErrorInfo info;
    info.code    = workspace::WorkspaceError::Internal;
    info.message = cause.message;
    return info;
}

/// AnalysisDb 查询/导入 Result 的错误通道换型（AnalysisDbError → 错误值）。
ca::Result<mjbase::project::DbContainerCensus, workspace::WorkspaceErrorInfo>
mapCensusError(ca::Result<mjbase::project::DbContainerCensus, mjbase::project::AnalysisDbError>&&
                   value)
{
    if (value.is_err()) return ca::Err(dbError(std::move(value).unwrap_err()));
    return ca::Ok(std::move(value).unwrap());
}

ca::Result<std::vector<mjbase::project::DbClassCensusRow>, workspace::WorkspaceErrorInfo>
mapClassRowsError(
    ca::Result<std::vector<mjbase::project::DbClassCensusRow>, mjbase::project::AnalysisDbError>&&
        value)
{
    if (value.is_err()) return ca::Err(dbError(std::move(value).unwrap_err()));
    return ca::Ok(std::move(value).unwrap());
}

/// major → "java N" 短标签（major ≥ 45 且 ≤ 200 才可判；越界值不贴标签）。
std::string javaVersionLabel(ca::i32 major)
{
    if (major >= 45 && major <= 200) return "java " + std::to_string(major - 44);
    return "";
}

/// 名字→计数分布的确定序：名字升序。
template <typename T>
void sortByName(std::vector<std::pair<std::string, T>>& distribution)
{
    std::sort(distribution.begin(), distribution.end(),
              [](const auto& left, const auto& right) { return left.first < right.first; });
}

}   // namespace

ca::Result<InfoReport, workspace::WorkspaceErrorInfo> collectInfo(const std::string& jarPath)
{
    workspace::WorkspaceBase ws;
    auto opened = ws.openJar(refOf(jarPath));
    if (opened.is_err()) return ca::Err(std::move(opened).unwrap_err());

    TempDbGuard guard{tempDbPath()};
    auto dbResult = mjbase::project::AnalysisDb::open(refOf(guard.path()));
    if (dbResult.is_err()) return ca::Err(dbError(std::move(dbResult).unwrap_err()));
    auto db = std::move(dbResult).unwrap();

    auto ingested = workspace::ingestIntoAnalysisDb(ws, db);
    if (ingested.is_err()) return ca::Err(ingestError(std::move(ingested).unwrap_err()));
    auto inputId = workspace::resolveCensusInput(db, ws.sourcePath());
    if (inputId.is_err()) return ca::Err(censusError(std::move(inputId).unwrap_err()));
    const ca::i64 id = inputId.unwrap();

    auto census = mapCensusError(db.censusContainer(id));
    if (census.is_err()) return ca::Err(std::move(census).unwrap_err());
    // DbContainerCensus 携 move-only Utf8String：unwrap 走移动重载，一次取值。
    auto censusValue = std::move(census).unwrap();
    auto classRows = mapClassRowsError(db.censusClasses(id));
    if (classRows.is_err()) return ca::Err(std::move(classRows).unwrap_err());

    InfoReport report;
    report.jarPath = jarPath;
    report.entries = censusValue.entriesActive;
    report.classes = censusValue.classes;
    report.entryShapes.reserve(censusValue.entryShapes.size());
    for (const auto& facet : censusValue.entryShapes)
        report.entryShapes.emplace_back(facet.facet.to_std_string(), facet.count);

    // class_kind 分布：投影行已带派生形态（classifyClassKind），这里只归并计数。
    std::map<std::string, ca::i64> kinds;
    for (const auto& row : std::move(classRows).unwrap())
        ++kinds[std::string(mjbase::project::classKindName(row.kind))];
    for (const auto& [kind, count] : kinds) report.classKinds.emplace_back(kind, count);
    sortByName(report.classKinds);

    // 字节码版本分布：summary_scan 骨架；损坏类被跳过（不进分布，不判失败）。
    auto summaries = ws.listClassSummaries();
    if (summaries.is_err()) return ca::Err(std::move(summaries).unwrap_err());
    {
        std::map<ca::i32, ca::i64> versions;
        for (const auto& summary : std::move(summaries).unwrap())
            ++versions[summary.version];
        report.versions.reserve(versions.size());
        for (const auto& [major, count] : versions) report.versions.emplace_back(major, count);
    }

    report.hasManifest = ws.manifestAttributes().is_ok();
    return ca::Ok(std::move(report));
}

void renderInfoText(const InfoReport& report, OutputSink& sink)
{
    sink.puts(shared::trfStr("mjt.cli.info.title", {report.jarPath}));
    sink.puts(shared::trfStr("mjt.cli.info.entries", {std::to_string(report.entries)}));
    sink.puts(shared::trfStr("mjt.cli.info.classes", {std::to_string(report.classes)}));
    if (!report.classKinds.empty()) {
        sink.puts(shared::trfStr("mjt.cli.info.class_kinds"));
        for (const auto& [kind, count] : report.classKinds)
            sink.puts("  " + kind + ": " + std::to_string(count));
    }
    if (!report.entryShapes.empty()) {
        sink.puts(shared::trfStr("mjt.cli.info.entry_shapes"));
        for (const auto& [shape, count] : report.entryShapes)
            sink.puts("  " + shape + ": " + std::to_string(count));
    }
    if (!report.versions.empty()) {
        sink.puts(shared::trfStr("mjt.cli.info.bytecode_versions"));
        for (const auto& [major, count] : report.versions) {
            std::string line = "  " + std::to_string(major);
            const auto  label = javaVersionLabel(major);
            if (!label.empty()) line += " (" + label + ")";
            line += ": " + std::to_string(count);
            sink.puts(line);
        }
    }
    sink.puts(shared::trfStr(
        "mjt.cli.info.manifest",
        {shared::trfStr(report.hasManifest ? "mjt.cli.info.manifest_yes"
                                           : "mjt.cli.info.manifest_no")}));
}

void renderInfoJson(const InfoReport& report, OutputSink& sink)
{
    /// 名字→计数分布的 JSON 对象形（键经 json::escape 转义）。
    auto objectField = [](const std::vector<std::pair<std::string, ca::i64>>& distribution) {
        std::string out = "{";
        for (std::size_t index = 0; index < distribution.size(); ++index) {
            if (index != 0) out += ", ";
            out += "\"" + json::escape(distribution[index].first) +
                   "\": " + std::to_string(distribution[index].second);
        }
        out += "}";
        return out;
    };

    std::string versions = "{";
    for (std::size_t index = 0; index < report.versions.size(); ++index) {
        if (index != 0) versions += ", ";
        versions += "\"" + std::to_string(report.versions[index].first) +
                    "\": " + std::to_string(report.versions[index].second);
    }
    versions += "}";

    std::string out = "{\n";
    out += "  \"jar\": \"" + json::escape(report.jarPath) + "\",\n";
    out += "  \"entries\": " + std::to_string(report.entries) + ",\n";
    out += "  \"classes\": " + std::to_string(report.classes) + ",\n";
    out += "  \"class_kinds\": " + objectField(report.classKinds) + ",\n";
    out += "  \"entry_shapes\": " + objectField(report.entryShapes) + ",\n";
    out += "  \"bytecode_versions\": " + versions + ",\n";
    out += std::string("  \"has_manifest\": ") + (report.hasManifest ? "true" : "false") + "\n";
    out += "}";
    sink.puts(out);
}

}   // namespace mjt::cli
