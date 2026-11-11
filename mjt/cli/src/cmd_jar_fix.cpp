/// @file        cmd_jar_fix.cpp
/// @brief       `mjt jar fix` 命令核心：jar_ops 扫描诊断 + applyEdit 修复
///              （EditCommand::JarRepair 单入口）+ exportArtifact 落盘。
///
/// 修复策略（v1 固定，与 workspace JarRepairRequest 两旗标对齐）：移除无法
/// 解析/校验的 class 条目（invalid_class / invalid_class_entry）与已知损坏
/// 输出标记条目（corrupt_output_marker）；其余问题保留并披露。输出 jar 走
/// 覆盖写：旧输出在导出前一刻删除（exportJar 只接受此前不存在的输出），
/// 打不开输入或修复失败时不删——上一份有效产物得以保留。
#include "commands.hpp"

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "shared/i18n_errors.hpp"
#include "shared/output_sink.hpp"
#include "workspace/edit_command.hpp"
#include "workspace/jar_ops.hpp"
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
    // 输入输出同路径在这里挡下（usage 校验归 main，此处兜底防自毁）。
    std::error_code canonicalEc;
    const auto inCanonical  = std::filesystem::weakly_canonical(inJar, canonicalEc);
    const auto outCanonical = std::filesystem::weakly_canonical(outJar, canonicalEc);
    if (!canonicalEc && inCanonical == outCanonical) {
        workspace::WorkspaceErrorInfo info;
        info.code    = workspace::WorkspaceError::InvalidArgument;
        info.message = shared::trfStr("mjt.cli.err.fix_same_path", {outJar});
        return ca::Err(std::move(info));
    }

    workspace::WorkspaceBase ws;
    auto opened = ws.open(refOf(inJar));
    if (opened.is_err()) return ca::Err(std::move(opened).unwrap_err());

    workspace::JarRepairRequest repair;
    repair.removeInvalidClassEntries   = true;
    repair.removeCorruptOutputMarkers  = true;

    // 报告面 = jar_ops 扫描自由函数（无状态只读）；修复事务走 applyEdit
    // 单入口（候选删除 → 门禁 → 台账落账一条管道）。扫描计数在事务前取定
    // ——commit 会换底工作副本容器，applyEdit 后旧容器指针悬垂不可复用。
    const auto* working = ws.workingContainer();
    ca::usize   classCount = 0;
    const auto  issues =
        working != nullptr ? workspace::scanJarRepairIssues(*working, repair, classCount)
                           : std::vector<workspace::JarRepairIssue>{};
    const ca::usize scannedEntries = working != nullptr ? working->size() : 0;
    if (workspace::hasSelectedRepairIssues(issues)) {
        workspace::MutationRequest request;
        const auto applied = workspace::applyEdit(
            ws, workspace::EditCommand{workspace::EditCommand::JarRepair{repair}}, request);
        if (applied.status.is_err()) return ca::Err(std::move(applied.status));
    }

    FixReport report;
    report.scannedEntries = scannedEntries;
    report.scannedClasses = classCount;
    report.issues.reserve(issues.size());
    for (const auto& issue : issues) {
        report.issues.push_back({issue.uri.to_std_string(), issue.kind.to_std_string(),
                                 issue.message.to_std_string(), issue.selectedForRemoval});
    }
    std::sort(report.issues.begin(), report.issues.end(),
              [](const FixIssue& left, const FixIssue& right) {
                  if (left.uri != right.uri) return left.uri < right.uri;
                  return left.kind < right.kind;
              });

    // 覆盖写（exportArtifact 契约是「此前不存在的输出」，删除失败按运行
    // 失败处理）：删旧输出推迟到导出前一刻——输入打不开或修复失败时上一份
    // 有效产物原样保留。
    std::error_code fsEc;
    std::filesystem::remove(outJar, fsEc);
    if (std::filesystem::exists(outJar)) {
        workspace::WorkspaceErrorInfo info;
        info.code    = workspace::WorkspaceError::PermissionDenied;
        info.message = shared::trfStr("mjt.cli.err.fix_output_busy", {outJar});
        return ca::Err(std::move(info));
    }

    auto exported = ws.exportArtifact(refOf(outJar));
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
