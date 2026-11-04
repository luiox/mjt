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
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "census_support.hpp"
#include "mjbase/project/class_kind.hpp"
#include "mjbase/project/library_origin.hpp"
#include "shared/json_util.hpp"
#include "shared/output_sink.hpp"
#include "workspace/workspace.hpp"

namespace mjt::cli {
namespace {

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

/// 计数表 → 分布：按 @p order 的稳定桶序输出，只含非零项（分布不依赖 map
/// 迭代序，桶序即呈现契约）。
template <typename T>
std::vector<std::pair<std::string, T>> orderedBuckets(
    const std::map<std::string, T>& counts, std::initializer_list<const char*> order)
{
    std::vector<std::pair<std::string, T>> result;
    for (const char* name : order) {
        const auto it = counts.find(name);
        if (it == counts.end() || it->second == 0) continue;
        result.emplace_back(name, it->second);
    }
    return result;
}

/// 可见性分布固定桶序（JVMS 命名；包私有 = 三显式位全空）。
constexpr std::initializer_list<const char*> kVisibilityOrder = {
    "public", "private", "protected", "package_private"};

/// 来源分布固定桶序（LibraryOrigin 枚举序；mjbase libraryOriginName 同文）。
constexpr std::initializer_list<const char*> kOriginOrder = {"jdk", "known", "unknown"};

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

    // 成员普查总量：容器投影计数列（与 censusMembers 逐行同源同口径）。
    report.methods       = censusValue.methods;
    report.fields        = censusValue.fields;
    report.nativeMethods = censusValue.nativeMethods;

    // class_kind 分布：投影行已带派生形态（classifyClassKind），这里只归并计数。
    std::map<std::string, ca::i64> kinds;
    std::map<std::string, ca::i64> origins;
    for (const auto& row : std::move(classRows).unwrap()) {
        ++kinds[std::string(mjbase::project::classKindName(row.kind))];
        ++origins[std::string(mjbase::project::libraryOriginName(row.origin))];
    }
    for (const auto& [kind, count] : kinds) report.classKinds.emplace_back(kind, count);
    sortByName(report.classKinds);
    report.origins = orderedBuckets(origins, kOriginOrder);

    // 成员可见性分布：censusMembers 逐成员 access 按位归类（JVMS 三显式位；
    // 之外 = 包私有）。行序复合键，无聚合价值，逐行扫一遍即线性。
    auto memberRows = mapMemberRowsError(db.censusMembers(id));
    if (memberRows.is_err()) return ca::Err(std::move(memberRows).unwrap_err());
    {
        std::map<std::string, ca::i64> methodVis;
        std::map<std::string, ca::i64> fieldVis;
        for (const auto& row : std::move(memberRows).unwrap()) {
            auto& target = row.kind == mjbase::project::DbMemberKind::Method ? methodVis
                                                                            : fieldVis;
            ++target[memberVisibilityName(row.access)];
        }
        report.methodVisibility = orderedBuckets(methodVis, kVisibilityOrder);
        report.fieldVisibility  = orderedBuckets(fieldVis, kVisibilityOrder);
    }

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
    sink.puts(shared::trfStr("mjt.cli.info.members",
                             {std::to_string(report.methods), std::to_string(report.fields),
                              std::to_string(report.nativeMethods)}));
    if (!report.classKinds.empty()) {
        sink.puts(shared::trfStr("mjt.cli.info.class_kinds"));
        for (const auto& [kind, count] : report.classKinds)
            sink.puts("  " + kind + ": " + std::to_string(count));
    }
    if (!report.methodVisibility.empty()) {
        sink.puts(shared::trfStr("mjt.cli.info.method_visibility"));
        for (const auto& [bucket, count] : report.methodVisibility)
            sink.puts("  " + bucket + ": " + std::to_string(count));
    }
    if (!report.fieldVisibility.empty()) {
        sink.puts(shared::trfStr("mjt.cli.info.field_visibility"));
        for (const auto& [bucket, count] : report.fieldVisibility)
            sink.puts("  " + bucket + ": " + std::to_string(count));
    }
    if (!report.origins.empty()) {
        sink.puts(shared::trfStr("mjt.cli.info.origins"));
        for (const auto& [origin, count] : report.origins)
            sink.puts("  " + origin + ": " + std::to_string(count));
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
    out += "  \"methods\": " + std::to_string(report.methods) + ",\n";
    out += "  \"fields\": " + std::to_string(report.fields) + ",\n";
    out += "  \"native_methods\": " + std::to_string(report.nativeMethods) + ",\n";
    out += "  \"class_kinds\": " + objectField(report.classKinds) + ",\n";
    out += "  \"method_visibility\": " + objectField(report.methodVisibility) + ",\n";
    out += "  \"field_visibility\": " + objectField(report.fieldVisibility) + ",\n";
    out += "  \"origins\": " + objectField(report.origins) + ",\n";
    out += "  \"entry_shapes\": " + objectField(report.entryShapes) + ",\n";
    out += "  \"bytecode_versions\": " + versions + ",\n";
    out += std::string("  \"has_manifest\": ") + (report.hasManifest ? "true" : "false") + "\n";
    out += "}";
    sink.puts(out);
}

}   // namespace mjt::cli
