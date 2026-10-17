#include "service.hpp"

#include <limits>
#include <memory>
#include <mutex>
#include <utility>

#include "mjbase/expr/expr_eval.hpp"
#include "mjbase/expr/expr_parser.hpp"
#include "mjbase/expr/expr_print.hpp"
#include "mjbase/expr/expr_simplify.hpp"
#include "decompiler_support.hpp"
#include "job.hpp"
#include "json.hpp"
#include "limits.hpp"

namespace mjt::mcp {

using namespace detail;
using mjt::base::workspace::IWorkspace;

namespace {

Json evaluateExpression(const Json& arguments, std::string& error)
{
    const auto text = arguments.value("expression", std::string{});
    if (text.empty()) {
        error = "expression is required";
        return Json();
    }

    bool simplify = false;
    if (arguments.contains("simplify")) {
        const auto& value = arguments.at("simplify");
        if (!value.is_boolean()) {
            error = "simplify must be a boolean";
            return Json();
        }
        simplify = value.get<bool>();
    }

    std::string parseError;
    auto expression = mjbase::expr::parseExpression(text, &parseError);
    if (!expression) {
        error = "cannot parse expression: " + parseError;
        return Json();
    }
    if (simplify) expression = mjbase::expr::simplify(expression).simplified;

    mjbase::expr::VarBindings bindings;
    if (arguments.contains("bindings")) {
        const auto& values = arguments.at("bindings");
        if (!values.is_object()) {
            error = "bindings must be an object of integer values";
            return Json();
        }
        for (const auto& [name, value] : values.items()) {
            if (!value.is_number_integer() && !value.is_number_unsigned()) {
                error = "binding '" + name + "' must be an integer";
                return Json();
            }
            if (value.is_number_unsigned()) {
                const auto number = value.get<ca::u64>();
                if (number > static_cast<ca::u64>(std::numeric_limits<ca::i64>::max())) {
                    error = "binding '" + name + "' exceeds the signed 64-bit range";
                    return Json();
                }
                bindings.emplace(name, mjbase::expr::ExprValue(static_cast<ca::i64>(number)));
            }
            else {
                bindings.emplace(name, mjbase::expr::ExprValue(value.get<ca::i64>()));
            }
        }
    }

    ca::u32 maximumDepth = 1024;
    if (arguments.contains("max_depth")) {
        const auto& value = arguments.at("max_depth");
        if (!value.is_number_integer() && !value.is_number_unsigned()) {
            error = "max_depth must be an integer";
            return Json();
        }
        if (value.is_number_unsigned()) {
            const auto number = value.get<ca::u64>();
            if (number == 0 || number > 4096) {
                error = "max_depth must be between 1 and 4096";
                return Json();
            }
            maximumDepth = static_cast<ca::u32>(number);
        }
        else {
            const auto number = value.get<ca::i64>();
            if (number <= 0 || number > 4096) {
                error = "max_depth must be between 1 and 4096";
                return Json();
            }
            maximumDepth = static_cast<ca::u32>(number);
        }
    }
    auto result = mjbase::expr::eval(*expression, bindings, maximumDepth);
    Json output = {{"expression", text},
                   {"normalized", mjbase::expr::toSExpr(*expression)},
                   {"simplified", simplify},
                   {"evaluated", result.ok}};
    if (result.ok)
        output["value"] = result.value.rawValue();
    else
        output["error"] = result.error;
    return output;
}

}   //namespace

Json jobStateJson(const std::shared_ptr<JobState>& job)
{
    std::lock_guard<std::mutex> lock(job->mutex);
    Json result = {{"job_id", job->id},
                   {"workspace_id", job->workspaceId},
                   {"kind", job->kind},
                   {"source_revision", job->sourceRevision},
                   {"state", job->state},
                   {"cancel_requested", job->cancellationRequested.load()}};
    if (!job->result.is_null()) {
        result["result"] = job->result;
        result["result_is_error"] = job->resultIsError;
    }
    return result;
}

std::optional<Json> McpToolService::callSessionTool(const std::string& name, const Json& arguments)
{
    if (name == "decompiler_list") {
        return toolResult({{"settings", decompilerSettingsJson(decompilerSettings_)},
                           {"backends", decompilerAvailabilityList(decompilerSettings_)}});
    }
    if (name == "decompiler_settings_get") {
        return toolResult({{"settings", decompilerSettingsJson(decompilerSettings_)}});
    }
    if (name == "decompiler_settings_set") {
        DecompilerSessionSettings updated;
        const auto status = parseDecompilerSettings(arguments, decompilerSettings_, updated);
        if (status.is_err()) return toolError("invalid decompiler settings", &status);
        decompilerSettings_ = std::move(updated);
        return toolResult({{"settings", decompilerSettingsJson(decompilerSettings_)},
                           {"backends", decompilerAvailabilityList(decompilerSettings_)}});
    }
    if (name == "pass_describe") {
        const auto requestedName = arguments.value("name", std::string{});
        const auto requestedKind = arguments.value("kind", std::string{});
        Json passes = Json::array();
        Json pipelines = Json::array();
        if (requestedKind.empty() || requestedKind == "pass") {
            for (const auto& pass : mjt::JarWorkspace::availablePasses()) {
                if (!requestedName.empty() && pass.name != requestedName) continue;
                passes.push_back(
                    {{"name", pass.name}, {"description", pass.description}, {"scope", pass.scope}});
            }
        }
        if (requestedKind.empty() || requestedKind == "pipeline") {
            for (const auto& pipeline : mjt::JarWorkspace::availablePipelines()) {
                if (!requestedName.empty() && pipeline.name != requestedName) continue;
                pipelines.push_back({{"name", pipeline.name}, {"description", pipeline.description}});
            }
        }
        if ((!requestedName.empty() && passes.empty() && pipelines.empty()) ||
            (!requestedKind.empty() && requestedKind != "pass" && requestedKind != "pipeline")) {
            const auto status = ca::ErrStatus(
                ca::StatusCode::NOT_FOUND,
                "registered pass or pipeline not found: " + requestedName);
            return toolError("registered pass or pipeline not found: " + requestedName, &status);
        }
        return toolResult({{"passes", std::move(passes)}, {"pipelines", std::move(pipelines)}});
    }
    if (name == "analysis_evaluate") {
        std::string error;
        auto result = evaluateExpression(arguments, error);
        if (!error.empty()) return toolError(error);
        return toolResult(std::move(result));
    }
    if (name == "job_get" || name == "job_cancel" || name == "job_release") {
        const auto id = arguments.value("job_id", std::string{});
        if (id.empty()) return toolError("job_id is required");
        //注：本函数由 call 对 session 工具只持 mutex_（状态锁）调用（见
        //service.cpp call），因此 jobs_ 的 find/erase 是线程安全的。
        const auto found = jobs_.find(id);
        if (found == jobs_.end()) {
            const auto status = ca::ErrStatus(ca::StatusCode::NOT_FOUND,
                                              "job not found: " + id);
            return toolError("job not found: " + id, &status);
        }
        const auto job = found->second;
        if (name == "job_get") return toolResult(jobStateJson(job));

        std::unique_lock<std::mutex> lock(job->mutex);
        const bool terminal = job->state == "succeeded" || job->state == "failed" ||
                              job->state == "cancelled";
        if (name == "job_cancel") {
            if (job->state == "committing")
                return toolError("job is committing and can no longer be cancelled");
            if (!terminal) job->cancellationRequested.store(true);
            return toolResult({{"job_id", job->id},
                               {"state", job->state},
                               {"cancel_requested", job->cancellationRequested.load()},
                               {"terminal", terminal}});
        }
        if (!terminal) return toolError("running job must be cancelled or completed before release");
        const auto released = job->id;
        lock.unlock();
        //join 在持有 mutex_ 的 call 上下文内进行：worker 的 commit 阶段需要按
        //executionMutex_ → mutex_ 顺序取锁（见 service.hpp 锁模型），此处持状态锁等待
        //join 会与 commit 竞争。但 release 仅在 job 已 terminal（worker 的 finish 已
        //写完结果并即将返回）时才走到这里，join 立即返回，实际不阻塞。
        if (job->worker.joinable()) job->worker.join();
        jobs_.erase(found);
        return toolResult({{"job_id", released}, {"released", true}});
    }
    if (name == "workspace_open") {
        const auto path = arguments.value("path", std::string{});
        if (path.empty()) return toolError("path is required");
        //与 kMaximumRetainedJobs 对齐的资源上限：workspace 常驻内存（整个
        //ResourceContainer），不设上限会让长会话无界增长。
        if (workspaces_.size() >= detail::kMaximumRetainedWorkspaces)
            return toolError("too many open workspaces (limit " +
                             std::to_string(detail::kMaximumRetainedWorkspaces) +
                             "); close one first");
        auto instance = std::make_unique<mjt::JarWorkspace>();
        const auto status = instance->openJar(path);
        if (status.is_err()) return toolError("open workspace failed", &status);
        auto id = arguments.value("workspace_id", std::string{});
        if (id.empty()) id = nextWorkspaceId();
        if (workspaces_.find(id) != workspaces_.end()) return toolError("workspace already exists: " + id);
        workspaces_.emplace(id, std::move(instance));
        return toolResult({{"workspace_id", id}, {"revision", 0}});
    }
    if (name == "workspace_close") {
        const auto id = arguments.value("workspace_id", std::string{});
        if (workspaces_.erase(id) == 0) {
            const auto status = ca::ErrStatus(ca::StatusCode::NOT_FOUND,
                                              "workspace not found: " + id);
            return toolError("workspace not found: " + id, &status);
        }
        return toolResult({{"workspace_id", id}, {"closed", true}});
    }
    return std::nullopt;
}

}   //namespace mjt::mcp
