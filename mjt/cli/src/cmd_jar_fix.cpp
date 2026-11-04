/// @file        cmd_jar_fix.cpp
/// @brief       `mjt jar fix` 命令核心：analyzeJarRepair 诊断 + applyJarRepair
///              修复（workspace 既有事务面直连）+ exportJar 落盘。
///
/// 修复策略（v1 固定，与 workspace JarRepairRequest 两旗标对齐）：移除无法
/// 解析/校验的 class 条目（invalid_class / invalid_class_entry）与已知损坏
/// 输出标记条目（corrupt_output_marker）；其余问题保留并披露。输出 jar 必须
/// 是新路径——已存在时先删除（exportJar 只接受此前不存在的输出）。
#include "commands.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "shared/i18n_errors.hpp"
#include "shared/output_sink.hpp"
#include "workspace/workspace.hpp"

namespace mjt::cli {
namespace {

ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

}   // namespace

ca::Result<FixReport, workspace::WorkspaceErrorInfo> fixJar(const std::string& inJar,
                                                            const std::string& outJar)
{
    // exportJar 契约是「此前不存在的输出」：CLI 语义取覆盖写（先删旧输出，
    // 删除失败按运行失败处理）。输入输出同路径在这里挡下（usage 校验归
    // main，此处兜底防自毁）。
    std::error_code canonicalEc;
    const auto inCanonical  = std::filesystem::weakly_canonical(inJar, canonicalEc);
    const auto outCanonical = std::filesystem::weakly_canonical(outJar, canonicalEc);
    if (!canonicalEc && inCanonical == outCanonical) {
        workspace::WorkspaceErrorInfo info;
        info.code    = workspace::WorkspaceError::InvalidArgument;
        info.message = shared::trfStr("mjt.cli.err.fix_same_path", {outJar});
        return ca::Err(std::move(info));
    }
    std::error_code fsEc;
    std::filesystem::remove(outJar, fsEc);
    if (std::filesystem::exists(outJar)) {
        workspace::WorkspaceErrorInfo info;
        info.code    = workspace::WorkspaceError::PermissionDenied;
        info.message = shared::trfStr("mjt.cli.err.fix_output_busy", {outJar});
        return ca::Err(std::move(info));
    }

    workspace::WorkspaceBase ws;
    auto opened = ws.openJar(refOf(inJar));
    if (opened.is_err()) return ca::Err(std::move(opened).unwrap_err());

    workspace::JarRepairRequest repair;
    repair.removeInvalidClassEntries   = true;
    repair.removeCorruptOutputMarkers  = true;
    auto applied = ws.applyJarRepair(repair);
    if (applied.mutation.status.is_err())
        return ca::Err(std::move(applied.mutation.status));

    FixReport report;
    report.scannedEntries = applied.resourceCount;
    report.scannedClasses = applied.classCount;
    report.issues.reserve(applied.issues.size());
    for (const auto& issue : applied.issues) {
        report.issues.push_back({issue.uri.to_std_string(), issue.kind.to_std_string(),
                                 issue.message.to_std_string(), issue.selectedForRemoval});
    }
    std::sort(report.issues.begin(), report.issues.end(),
              [](const FixIssue& left, const FixIssue& right) {
                  if (left.uri != right.uri) return left.uri < right.uri;
                  return left.kind < right.kind;
              });

    auto exported = ws.exportJar(refOf(outJar));
    if (exported.is_err()) return ca::Err(std::move(exported).unwrap_err());
    return ca::Ok(std::move(report));
}

void renderFixReport(const FixReport& report, const std::string& outJar, OutputSink& sink)
{
    sink.puts(shared::trfStr("mjt.cli.fix.scanned",
                             {std::to_string(report.scannedEntries),
                              std::to_string(report.scannedClasses)}));
    if (report.issues.empty()) {
        sink.puts(shared::trfStr("mjt.cli.fix.no_issues"));
    }
    else {
        sink.puts(shared::trfStr("mjt.cli.fix.issues", {std::to_string(report.issues.size())}));
        for (const auto& issue : report.issues) {
            const char* verbKey = issue.removed ? "mjt.cli.fix.removed" : "mjt.cli.fix.kept";
            const std::string reasonKey = "mjt.cli.fix.reason." + issue.kind;
            sink.puts(shared::trfStr(verbKey,
                                     {issue.uri, issue.kind, shared::trfStr(reasonKey.c_str())}));
        }
    }
    sink.puts(shared::trfStr("mjt.cli.fix.done", {outJar}));
}

}   // namespace mjt::cli
