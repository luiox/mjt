/// @file        cmd_classes.cpp
/// @brief       `mjt classes` 命令核心：类清单（census 投影 + kind/origin
///              过滤 + --limit 防呆截断）。
///
/// 地基：census 投影（censusClasses，class_kind 与 library_origin 为投影层
/// 派生列，与 info 同源同果）。行序 = 库序（internal_name BINARY 升序，跨
/// 平台确定）；过滤项组合 = 交集；输出必经 --limit 截断（缺省 10000），截断
/// 时提示按 total 放大或加过滤项。纯文本面，不依赖反编译器。
#include "commands.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "census_support.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "mjbase/project/class_kind.hpp"
#include "mjbase/project/library_origin.hpp"
#include "shared/i18n_errors.hpp"
#include "shared/json_util.hpp"
#include "shared/output_sink.hpp"
#include "workspace/workspace.hpp"

namespace mjt::cli {
namespace {

/// ClassesError 便捷构造（i18n 渲染）。
ClassesError classesError(ClassesError::Kind kind, const char* key,
                          std::initializer_list<std::string_view> args = {})
{
    ClassesError error;
    error.kind    = kind;
    error.message = shared::trfStr(key, args);
    return error;
}

/// ASCII 小写归一（xref 方向名同款口径：--kind / --origin 值大小写不敏感）。
std::string asciiLower(const std::string& value)
{
    std::string out = value;
    for (auto& character : out)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return out;
}

/// --kind 合法值集合（classKindName 稳定名；与 census kind 列同文）。
bool isKnownClassKind(const std::string& name)
{
    static constexpr const char* kKinds[] = {"class",    "interface", "enum",
                                             "record",   "annotation", "module"};
    for (const char* kind : kKinds)
        if (name == kind) return true;
    return false;
}

/// --origin 合法值集合（libraryOriginName 稳定名；与 census origin 列同文）。
bool isKnownOrigin(const std::string& name)
{
    static constexpr const char* kOrigins[] = {"jdk", "known", "unknown"};
    for (const char* origin : kOrigins)
        if (name == origin) return true;
    return false;
}

}   // namespace

ca::Result<ClassesReport, ClassesError> collectClasses(const std::string& jarPath,
                                                       const std::string& kindFilter,
                                                       const std::string& originFilter,
                                                       ca::i64 limit)
{
    const std::string kind = asciiLower(kindFilter);
    const std::string origin = asciiLower(originFilter);
    if (!kind.empty() && !isKnownClassKind(kind))
        return ca::Err(classesError(ClassesError::Kind::InvalidArgument,
                                    "mjt.cli.classes.err.invalid_kind", {kindFilter}));
    if (!origin.empty() && !isKnownOrigin(origin))
        return ca::Err(classesError(ClassesError::Kind::InvalidArgument,
                                    "mjt.cli.classes.err.invalid_origin", {originFilter}));
    if (limit < 1)
        return ca::Err(classesError(ClassesError::Kind::InvalidArgument,
                                    "mjt.cli.classes.err.invalid_limit",
                                    {std::to_string(limit)}));

    workspace::WorkspaceBase ws;
    auto opened = ws.openJar(refOf(jarPath));
    if (opened.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(opened).unwrap_err();
        return ca::Err(classesError(ClassesError::Kind::Runtime, "mjt.cli.classes.err.open_failed",
                                    {info.message}));
    }

    TempDbGuard guard{tempDbPath()};
    auto dbResult = mjbase::project::AnalysisDb::open(refOf(guard.path()));
    if (dbResult.is_err()) return ca::Err(classesError(ClassesError::Kind::Runtime,
                                                       "mjt.cli.classes.err.open_failed",
                                                       {dbError(std::move(dbResult).unwrap_err())
                                                            .message}));
    auto db = std::move(dbResult).unwrap();

    auto ingested = workspace::ingestIntoAnalysisDb(ws, db);
    if (ingested.is_err()) {
        workspace::WorkspaceErrorInfo info = ingestError(std::move(ingested).unwrap_err());
        return ca::Err(classesError(ClassesError::Kind::Runtime, "mjt.cli.classes.err.open_failed",
                                    {info.message}));
    }
    auto inputId = workspace::resolveCensusInput(db, ws.sourcePath());
    if (inputId.is_err()) {
        workspace::WorkspaceErrorInfo info = censusError(std::move(inputId).unwrap_err());
        return ca::Err(classesError(ClassesError::Kind::Runtime, "mjt.cli.classes.err.open_failed",
                                    {info.message}));
    }

    auto rows = mapClassRowsError(db.censusClasses(inputId.unwrap()));
    if (rows.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(rows).unwrap_err();
        return ca::Err(classesError(ClassesError::Kind::Runtime, "mjt.cli.classes.err.open_failed",
                                    {info.message}));
    }
    // DbClassCensusRow 携 move-only Utf8String：unwrap 走移动重载，一次取值。
    auto censusRows = std::move(rows).unwrap();

    ClassesReport report;
    report.jarPath      = jarPath;
    report.kindFilter   = kind;
    report.originFilter = origin;
    report.limit        = limit;
    report.classesInJar = static_cast<ca::i64>(censusRows.size());

    // 过滤 + 截断：库序即输出序（internal_name 升序，投影层定死）。
    report.classes.reserve(censusRows.size());
    for (auto& row : censusRows) {
        const auto rowKind   = mjbase::project::classKindName(row.kind);
        const auto rowOrigin = mjbase::project::libraryOriginName(row.origin);
        if (!kind.empty() && kind != rowKind) continue;
        if (!origin.empty() && origin != rowOrigin) continue;
        ++report.total;
        if (report.total <= limit)
            report.classes.push_back({row.internalName.to_std_string(), std::string(rowKind),
                                      std::string(rowOrigin)});
    }
    report.truncated = report.total > limit;
    return ca::Ok(std::move(report));
}

void renderClassesText(const ClassesReport& report, OutputSink& sink)
{
    sink.puts(shared::trfStr("mjt.cli.classes.title", {report.jarPath}));
    if (!report.kindFilter.empty())
        sink.puts(shared::trfStr("mjt.cli.classes.kind_filter", {report.kindFilter}));
    if (!report.originFilter.empty())
        sink.puts(shared::trfStr("mjt.cli.classes.origin_filter", {report.originFilter}));
    // 命中数恒带 jar 类总数（过滤前口径）：过滤效果一目了然。
    sink.puts(shared::trfStr("mjt.cli.classes.matched",
                             {std::to_string(report.total),
                              std::to_string(report.classesInJar),
                              std::to_string(static_cast<ca::i64>(report.classes.size()))}));
    if (report.truncated)
        // 截断提示带全量数：按 total 放大 --limit 或加 --kind/--origin 过滤。
        sink.puts(shared::trfStr("mjt.cli.classes.truncated", {std::to_string(report.total)}));
    for (const auto& row : report.classes)
        sink.puts("  " + row.internalName + " " + row.kind + " " + row.origin);
}

void renderClassesJson(const ClassesReport& report, OutputSink& sink)
{
    std::string out = "{\n";
    out += "  \"jar\": \"" + json::escape(report.jarPath) + "\",\n";
    if (!report.kindFilter.empty())
        out += "  \"kind\": \"" + json::escape(report.kindFilter) + "\",\n";
    if (!report.originFilter.empty())
        out += "  \"origin\": \"" + json::escape(report.originFilter) + "\",\n";
    out += "  \"classes_in_jar\": " + std::to_string(report.classesInJar) + ",\n";
    out += "  \"total\": " + std::to_string(report.total) + ",\n";
    out += "  \"limit\": " + std::to_string(report.limit) + ",\n";
    out += std::string("  \"truncated\": ") + (report.truncated ? "true" : "false") + ",\n";
    out += "  \"classes\": [";
    for (std::size_t index = 0; index < report.classes.size(); ++index) {
        const auto& row = report.classes[index];
        if (index != 0) out += ", ";
        out += "\n    {\"name\": \"" + json::escape(row.internalName) +
               "\", \"kind\": \"" + json::escape(row.kind) +
               "\", \"origin\": \"" + json::escape(row.origin) + "\"}";
    }
    if (!report.classes.empty()) out += "\n  ";
    out += "]\n}";
    sink.puts(out);
}

}   // namespace mjt::cli
