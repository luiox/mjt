/// @file        cmd_reflect.cpp
/// @brief       `mjt reflect` 命令核心：反射 sink 调用点绑定分析（纯文本 / JSON 两种渲染）。
///
/// 基于 workspace 打开容器与 mjbase ReflectionBindingAnalyzer（0.0.31 起 build 免 arena，键驻留内化为分析器自有 KeyPool）。输出契约：`# ` 摘要三行、绑定行 `[state] api ... <- site`；--json 时摘要走 stderr，stdout 为纯 JSON 数组。
#include "commands.hpp"

#include <cstdio>
#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "mjbase/model/resource_model.hpp"
#include "mjbase/reference/reflection_binding_analyzer.hpp"

#include "shared/i18n_errors.hpp"
#include "shared/json_util.hpp"
#include "shared/output_sink.hpp"
#include "workspace/workspace.hpp"
#include "workspace/workspace_types.hpp"

namespace mjt::cli {
namespace {

/// Utf8String/UTF-8 std::string → 借用视图（info / strings 命令同此构造）。
ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// ReflectError 便捷构造（i18n 渲染）。
ReflectError reflectError(const char* key, std::initializer_list<std::string_view> args = {})
{
    ReflectError error;
    error.kind    = ReflectError::Kind::Runtime;
    error.message = shared::trfStr(key, args);
    return error;
}

/// 空串 → "-"（绑定行里未知字段不空位）。
std::string dashIfEmpty(const std::string& value)
{
    return value.empty() ? "-" : value;
}

/// mjbase 绑定 → CLI 行数据（string_view 驻留于分析器 KeyPool，此处拷贝）。
ReflectBindingRow toRow(const mjbase::reference::ReflectionBinding& binding)
{
    namespace ref = mjbase::reference;
    ReflectBindingRow row;
    row.state          = ref::reflectionBindingStateLabel(binding.state);
    row.api            = ref::reflectionApiKindLabel(binding.api);
    row.reason         = ref::reflectionPendingReasonLabel(binding.reason);
    row.className      = std::string(binding.className);
    row.memberName     = std::string(binding.memberName);
    row.paramTypeCount = binding.paramTypeCount;
    row.resolved       = binding.state == ref::ReflectionBindingState::Resolved;
    row.targetOwner    = std::string(binding.target.owner);
    row.targetName     = std::string(binding.target.name);
    row.targetDesc     = std::string(binding.target.desc);
    row.siteClass      = std::string(binding.site.sourceClass);
    row.siteMethod     = std::string(binding.site.sourceMethod);
    row.siteDesc       = std::string(binding.site.sourceDesc);
    row.siteOpcode     = binding.site.opcode;
    row.siteInsn       = binding.site.insnIndex;
    return row;
}

}   // namespace

ca::Result<ReflectReport, ReflectError> collectReflect(const std::string& jarPath,
                                                       bool unresolvedOnly,
                                                       const std::string& classFilter)
{
    workspace::WorkspaceBase ws;
    auto opened = ws.open(refOf(jarPath));
    if (opened.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(opened).unwrap_err();
        return ca::Err(reflectError("mjt.cmd.reflect.err.jar_open_failed", {info.message}));
    }

    auto* container = ws.workingContainer();

    // 分析器全量构建（try/catch 兜底：单类损坏由分析器跳过，此处只防构建路径自身的意外异常）。
    mjbase::reference::ReflectionBindingAnalyzer analyzer;
    try {
        analyzer.build(*container);
    }
    catch (const std::exception& e) {
        return ca::Err(reflectError("mjt.cmd.reflect.err.build_failed", {e.what()}));
    }
    catch (...) {
        return ca::Err(reflectError("mjt.cmd.reflect.err.build_failed", {"unknown"}));
    }

    // 类计数（容器 class 形态资源数；畸形字节条目容器分类为非 class，不计数）。
    ca::usize classCount = 0;
    container->visitResources([&classCount](std::string_view, const mjbase::model::IResource& res) {
        if (res.getType() == mjbase::model::ResourceType::Class) ++classCount;
    });

    ReflectReport report;
    report.jarPath     = jarPath;
    report.classFilter = classFilter;
    report.classCount  = classCount;

    const auto analyzed = analyzer.summarize();
    report.summary.total       = analyzed.total;
    report.summary.resolved    = analyzed.resolved;
    report.summary.pending     = analyzed.pending;
    report.summary.unsupported = analyzed.unsupported;
    report.summary.ratePercent = analyzed.resolvedRatePercent();
    for (std::size_t kind = 0; kind < analyzed.byKind.size(); ++kind) {
        if (analyzed.byKind[kind] == 0) continue;
        report.summary.apiDistribution.emplace_back(
            std::string(mjbase::reference::reflectionApiKindLabel(
                static_cast<mjbase::reference::ReflectionApiKind>(kind))),
            analyzed.byKind[kind]);
    }

    for (const auto& binding : analyzer.bindings()) {
        if (unresolvedOnly && binding.state != mjbase::reference::ReflectionBindingState::Pending)
            continue;
        if (!classFilter.empty() && binding.site.sourceClass != classFilter) continue;
        report.bindings.push_back(toRow(binding));
    }
    return ca::Ok(std::move(report));
}

void renderReflectSummary(const ReflectReport& report, OutputSink& sink)
{
    char rate[32];
    std::snprintf(rate, sizeof(rate), "%.1f", report.summary.ratePercent);
    sink.printf("# %s\n",
                shared::trfStr("mjt.cmd.reflect.out.scanned",
                               {report.jarPath, std::to_string(report.classCount)})
                    .c_str());
    sink.printf("# %s\n",
                shared::trfStr("mjt.cmd.reflect.out.sink_summary",
                               {std::to_string(report.summary.total),
                                std::to_string(report.summary.resolved),
                                std::to_string(report.summary.pending),
                                std::to_string(report.summary.unsupported), rate})
                    .c_str());

    // API 分布（枚举序，label=count 逗号连接）。
    std::string dist;
    for (const auto& [label, count] : report.summary.apiDistribution) {
        if (!dist.empty()) dist += ", ";
        dist += label + "=" + std::to_string(count);
    }
    sink.printf("# %s\n",
                shared::trfStr("mjt.cmd.reflect.out.api_dist", {dist}).c_str());
}

void renderReflectText(const ReflectReport& report, OutputSink& sink)
{
    renderReflectSummary(report, sink);

    if (report.bindings.empty()) sink.puts(shared::trfStr("mjt.cmd.reflect.out.none"));

    for (const auto& row : report.bindings) {
        // 目标列：Resolved 时 owner.name+desc（无括号拼接），否则 "-"；reason 列空标签同 "-"。
        const std::string target =
            row.resolved ? row.targetOwner + "." + row.targetName + row.targetDesc
                         : std::string("-");
        const std::string reason = row.reason.empty() ? std::string("-") : row.reason;
        sink.printf(
            "[%s] %-22s class=%s member=%s params=%d reason=%s target=%s <- %s.%s%s insn=%d\n",
            row.state.c_str(), row.api.c_str(), dashIfEmpty(row.className).c_str(),
            dashIfEmpty(row.memberName).c_str(), row.paramTypeCount, reason.c_str(),
            target.c_str(), row.siteClass.c_str(), row.siteMethod.c_str(), row.siteDesc.c_str(),
            row.siteInsn);
    }
}

void renderReflectJson(const ReflectReport& report, OutputSink& sink)
{
    {
        json::JsonArrayWriter arr(sink);
        for (const auto& row : report.bindings) {
            auto& out = arr.next();
            out.printf(
                "  {\"state\":\"%s\",\"api\":\"%s\",\"reason\":\"%s\",\"class\":\"%s\","
                "\"member\":\"%s\",\"paramTypes\":%d,"
                "\"target\":{\"owner\":\"%s\",\"name\":\"%s\",\"desc\":\"%s\"},"
                "\"site\":{\"class\":\"%s\",\"method\":\"%s\",\"desc\":\"%s\",\"opcode\":%d,"
                "\"insn\":%d}}",
                row.state.c_str(), row.api.c_str(), row.reason.c_str(),
                json::escape(row.className).c_str(), json::escape(row.memberName).c_str(),
                row.paramTypeCount,
                row.resolved ? json::escape(row.targetOwner).c_str() : "",
                row.resolved ? json::escape(row.targetName).c_str() : "",
                row.resolved ? json::escape(row.targetDesc).c_str() : "",
                json::escape(row.siteClass).c_str(), json::escape(row.siteMethod).c_str(),
                json::escape(row.siteDesc).c_str(), row.siteOpcode, row.siteInsn);
        }
    }
    sink.printf("\n");
}

}   // namespace mjt::cli
