#include "service.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "decompiler_support.hpp"
#include "job.hpp"
#include "json.hpp"
#include "limits.hpp"
#include "parser.hpp"
#include "shared/jar_remapper.hpp"
#include "shared/status_convert.hpp"
#include "tool/shadow_slot.hpp"

namespace mjt::mcp {

using namespace detail;
using mjbase::workspace::MutationRequest;
using mjbase::workspace::PipelineRequest;
using mjbase::workspace::WorkflowCandidateCommitRequest;
using mjbase::workspace::WorkflowReferenceQuery;
using mjbase::workspace::WorkflowRequest;
using mjbase::workspace::IWorkspace;

namespace fs = std::filesystem;

namespace {

struct WorkflowJobSpec final
{
    std::vector<WorkflowReferenceQuery> referenceQueries;
    std::optional<WorkspaceRemapRequest> remap;
    bool                                  runPipeline = false;
    PipelineRequest                       pipeline;
    std::vector<std::string>              decompileClasses;
    bool                                  decompileBatch = false;
    DecompilerOptions                     decompiler;
    std::string                           exportPath;
    MutationRequest                       mutation;
};

void appendWorkflowSources(Json& payload,
                           const std::vector<DecompiledSource>& sources,
                           bool batch)
{
    if (sources.empty()) return;
    if (!batch) {
        payload["decompiled_source"] = decompiledSourceJson(sources.front(), "class");
        return;
    }
    Json values = Json::array();
    for (const auto& source : sources) values.push_back(decompiledSourceJson(source, "class"));
    payload["decompiled_sources"] = std::move(values);
}


}   //namespace

std::optional<Json> McpToolService::callDecompilerTool(
    const std::string& name, const Json& arguments, IWorkspace* current)
{
    if (name == "class_source" || name == "decompiler_decompile" || name == "method_source" ||
        name == "decompiler_compare") {
        DecompilerOptions options;
        {
            //decompilerSettings_ 由 session 工具（decompiler_settings_set）在状态锁下
            //修改；M1 修复后执行体不再持有状态锁，此处短持状态锁读取默认值。
            std::lock_guard<std::mutex> lock(mutex_);
            const auto optionsStatus = parseDecompilerOptions(arguments, decompilerSettings_, options);
            if (optionsStatus.is_err()) return toolError("invalid decompiler options", &optionsStatus);
        }

        //T2 name 槽：class/owner/name 同查询面口径——Ref 全量解析，
        //raw 全量核表（反编译目标必须是 jar 内已定义类，核表零漂移）。
        std::string className;
        Json        slotError;
        if (name == "method_source") {
            const auto slots = slot::loadWorkspaceShadowTable(*current);
            std::string owner;
            std::string member;
            std::string descriptor;
            if (!slot::resolveMethodSlots(slots, arguments, true, owner, member, descriptor, slotError))
                return slotError;
            auto methodInfo = current->methodInfo(owner, member, descriptor);
            if (methodInfo.is_err()) {
                const auto err = shared::toCaStatus(methodInfo.unwrap_err());
                return toolError("get method source failed", &err);
            }
            className = owner;
        }
        else if (!slot::resolveClassArgument(*current, arguments, "class", true, className, slotError)) {
            return slotError;
        }
        if (className.empty()) return toolError("class is required");

        if (arguments.value("async", false)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (jobs_.size() >= kMaximumRetainedJobs)
                    return toolError("too many retained jobs; release completed jobs before starting another");
            }
            auto classInfo = current->classInfo(className);
            if (classInfo.is_err()) {
                const auto err = shared::toCaStatus(classInfo.unwrap_err());
                return toolError("get decompile class failed", &err);
            }
            const auto internalName = classInfo.unwrap().internalName;

            auto temporary = createDecompilerTemporaryDirectory();
            if (temporary.is_err()) { const auto err = temporary.unwrap_err(); return toolError("create decompiler snapshot failed", &err); }
            const auto snapshotDirectory = std::move(temporary).unwrap();
            const auto snapshot = snapshotDirectory / "workspace.jar";
            const auto exportStatus = current->exportJar(snapshot.string());
            if (exportStatus.is_err()) {
                std::error_code cleanupError;
                fs::remove_all(snapshotDirectory, cleanupError);
                const auto err = shared::toCaStatus(exportStatus.unwrap_err());
                return toolError("snapshot decompiler workspace failed", &err);
            }

            auto job = std::make_shared<JobState>();
            job->workspaceId    = arguments.value("workspace_id", std::string{});
            job->kind           = name;
            job->sourceRevision = current->revision();
            //decompiler_compare 的 schema（decompilerRuntime）不含 async 且
            //additionalProperties=false，走不到这里；async job 只服务
            //class_source / decompiler_decompile / method_source，保持单一同步路径。
            {
                //计数器与 jobs_ 在状态锁保护下（M1 修复后执行体不再持有状态锁，
                //此处短持）。
                std::lock_guard<std::mutex> lock(mutex_);
                job->id = nextJobId();
                jobs_.emplace(job->id, job);
            }

            try {
                job->worker = std::thread([job,
                                           snapshotDirectory,
                                           snapshot,
                                           internalName,
                                           options] {
                    ScopedTemporaryDirectory directory(snapshotDirectory);
                    const auto finish = [&](const std::string& state, Json result, bool isError) {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->state         = state;
                        job->result        = std::move(result);
                        job->resultIsError = isError;
                    };
                    try {
                        const auto cancelled = [&] {
                            return ca::ErrStatus(ca::StatusCode::CANCELLED,
                                                 "decompile job cancelled");
                        };
                        {
                            std::lock_guard<std::mutex> lock(job->mutex);
                            if (job->cancellationRequested.load()) {
                                const auto status = cancelled();
                                job->state = "cancelled";
                                job->result = {{"error", "decompiler job cancelled"},
                                               {"status", statusJson(status)}};
                                job->resultIsError = true;
                                return;
                            }
                            job->state = "running";
                        }
                        const auto cancellationRequested = [job] {
                            return job->cancellationRequested.load();
                        };
                        const auto fail = [&](const std::string& operation, const ca::Status& status) {
                            //N2 async 超时侧车：DEADLINE_EXCEEDED（反编译子进程超时，
                            //全仓唯一产出点 mjt/src/shared/decompiler.cpp）与同步路径
                            //normalizeToolError 同口径补 partial 元数据，job_get 消费者
                            //可据此区分「超时失败」与「即时业务错误」。仅超时路径写入，
                            //其余失败与取消零变化；received/total/cursor 对超时无意义，
                            //按 P2-3 侧车 schema 同口径省略。
                            Json payload = {{"error", operation}, {"status", statusJson(status)}};
                            if (status.code() == ca::StatusCode::DEADLINE_EXCEEDED) {
                                payload["partial"] = true;
                                payload["reason"]  = "timeout";
                            }
                            finish(status.code() == ca::StatusCode::CANCELLED ? "cancelled" : "failed",
                                   std::move(payload),
                                   true);
                        };

                        auto source = decompileSnapshotClass(snapshot,
                                                             internalName,
                                                             options,
                                                             cancellationRequested);
                        if (source.is_err()) {
                            fail("decompile class failed", source.unwrap_err());
                            return;
                        }
                        if (cancellationRequested()) {
                            const auto status = cancelled();
                            fail("decompiler job cancelled", status);
                            return;
                        }
                        finish("succeeded", decompiledSourceJson(source.unwrap(), "class"), false);
                    }
                    catch (const std::exception& exception) {
                        finish("failed", {{"error", exception.what()}}, true);
                    }
                    catch (...) {
                        finish("failed", {{"error", "unknown decompiler job failure"}}, true);
                    }
                });
            }
            catch (const std::exception& exception) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    jobs_.erase(job->id);
                }
                std::error_code cleanupError;
                fs::remove_all(snapshotDirectory, cleanupError);
                return toolError("start decompiler job failed: " + std::string(exception.what()));
            }
            return toolResult(jobStateJson(job));
        }

        if (name == "decompiler_compare") {
            auto cfrOptions = options;
            cfrOptions.kind = DecompilerKind::Cfr;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!arguments.contains("decompiler_jar"))
                    cfrOptions.decompilerJar = decompilerSettings_.cfrJar;
            }
            auto cfr = decompileWorkspaceClass(*current, className, cfrOptions);
            if (cfr.is_err()) { const auto err = cfr.unwrap_err(); return toolError("decompile with CFR failed", &err); }

            auto vineflowerOptions = options;
            vineflowerOptions.kind = DecompilerKind::Vineflower;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!arguments.contains("decompiler_jar"))
                    vineflowerOptions.decompilerJar = decompilerSettings_.vineflowerJar;
            }
            auto vineflower = decompileWorkspaceClass(*current, className, vineflowerOptions);
            if (vineflower.is_err())
                { const auto err = vineflower.unwrap_err(); return toolError("decompile with Vineflower failed", &err); }
            return toolResult({{"internal_name", cfr.unwrap().internalName},
                               {"sources", Json::array({decompiledSourceJson(cfr.unwrap(), "class"),
                                                        decompiledSourceJson(vineflower.unwrap(), "class")})}});
        }

        auto source = decompileWorkspaceClass(*current, className, options);
        if (source.is_err()) { const auto err = source.unwrap_err(); return toolError("decompile class failed", &err); }
        return toolResult(decompiledSourceJson(source.unwrap(), "class"));
    }

    if (name == "analysis_workflow") {
        WorkflowRequest workflow;
        std::string     parseError;
        if (!parseWorkflowReferences(arguments, workflow.referenceQueries, parseError))
            return toolError(parseError);
        workflow.exportPath = arguments.value("export_path", std::string{});

        const bool hasPipeline = arguments.contains("pipeline") || arguments.contains("passes") ||
                                 arguments.contains("jdk_path");
        workflow.runPipeline = arguments.value("run_pipeline", hasPipeline);
        if (!workflow.runPipeline && hasPipeline)
            return toolError("run_pipeline must be true when pipeline, passes or jdk_path is supplied");
        if (workflow.runPipeline && !parsePipelineRequest(arguments, workflow.pipeline, parseError))
            return toolError(parseError);

        const auto mappingFormat = arguments.value("mapping_format", std::string{});
        const auto mappingText = arguments.value("mapping_text", std::string{});
        const bool hasMapping = !mappingFormat.empty() || !mappingText.empty() ||
                                arguments.contains("mapping_policy");
        WorkspaceRemapRequest remap;
        mjbase::asm_remapper::MemberRemapPlan plan;
        mjbase::asm_remapper::RemapPolicy     policy;
        if (hasMapping) {
            if (mappingFormat.empty() || mappingText.empty())
                return toolError("mapping_format and mapping_text are both required for a workflow remap");
            remap.format      = mappingFormat;
            remap.mappingText = mappingText;
            remap.policy      = arguments.value("mapping_policy", std::string{"bytecode-only"});
            const auto status = buildWorkspaceRemapPlan(*current, remap, plan, policy);
            if (status.is_err()) {
                const auto err = shared::toCaStatus(status);
                return toolError("build workflow remap plan failed", &err);
            }
            workflow.remapPlan = &plan;
            workflow.remapPolicy = policy;
        }

        std::vector<std::string> decompileClasses;
        bool                     decompileBatch = false;
        if (!parseWorkflowDecompileClasses(arguments,
                                           decompileClasses,
                                           decompileBatch,
                                           parseError)) {
            return toolError(parseError);
        }
        const bool asyncWorkflow = arguments.value("async", false);
        DecompilerOptions options;
        if (!decompileClasses.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto optionsStatus = parseDecompilerOptions(arguments, decompilerSettings_, options);
            if (optionsStatus.is_err())
                return toolError("invalid workflow decompiler options", &optionsStatus);
        }

        if (asyncWorkflow) {
            if (decompileClasses.empty())
                return toolError("async workflow requires decompile_class or decompile_classes");
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (jobs_.size() >= kMaximumRetainedJobs)
                    return toolError("too many retained jobs; release completed jobs before starting another");
            }

            WorkflowJobSpec spec;
            spec.referenceQueries = workflow.referenceQueries;
            spec.runPipeline      = workflow.runPipeline;
            spec.pipeline         = workflow.pipeline;
            spec.decompileClasses = decompileClasses;
            spec.decompileBatch   = decompileBatch;
            spec.decompiler       = options;
            spec.exportPath       = workflow.exportPath;
            spec.mutation         = mutationRequest(arguments);
            if (hasMapping) spec.remap = remap;
            if (spec.mutation.expectedRevision != mjbase::workspace::kAnyRevision &&
                spec.mutation.expectedRevision != current->revision()) {
                const auto result = current->runWorkflow(workflow, spec.mutation);
                return toolResult(workflowJson(result), result.mutation.status.is_err());
            }

            auto temporary = createDecompilerTemporaryDirectory();
            if (temporary.is_err())
                { const auto err = temporary.unwrap_err(); return toolError("create workflow job snapshot failed", &err); }
            const auto snapshotDirectory = std::move(temporary).unwrap();
            const auto snapshot = snapshotDirectory / "input.jar";
            const auto snapshotStatus = current->exportJar(snapshot.string());
            if (snapshotStatus.is_err()) {
                std::error_code cleanupError;
                fs::remove_all(snapshotDirectory, cleanupError);
                const auto err = shared::toCaStatus(snapshotStatus.unwrap_err());
                return toolError("snapshot workflow workspace failed", &err);
            }

            std::vector<fs::path> supportingSnapshots;
            for (const auto& supporting : current->supportingResources()) {
                const auto copy = snapshotDirectory /
                                  ("support-" + std::to_string(supportingSnapshots.size()) + ".jar");
                std::error_code copyError;
                fs::copy_file(supporting.path, copy, fs::copy_options::none, copyError);
                if (copyError) {
                    std::error_code cleanupError;
                    fs::remove_all(snapshotDirectory, cleanupError);
                    return toolError("snapshot workflow supporting JAR failed: " + copyError.message());
                }
                supportingSnapshots.push_back(copy);
            }

            auto job = std::make_shared<JobState>();
            job->workspaceId    = arguments.value("workspace_id", std::string{});
            job->kind           = "analysis_workflow";
            job->sourceRevision = current->revision();
            {
                std::lock_guard<std::mutex> lock(mutex_);
                job->id = nextJobId();
                jobs_.emplace(job->id, job);
            }

            try {
                job->worker = std::thread([this,
                                           job,
                                           snapshotDirectory,
                                           snapshot,
                                           supportingSnapshots,
                                           spec = std::move(spec)]() mutable {
                    ScopedTemporaryDirectory directory(snapshotDirectory);
                    const auto finish = [&](const std::string& state, Json result, bool isError) {
                        std::lock_guard<std::mutex> lock(job->mutex);
                        job->state         = state;
                        job->result        = std::move(result);
                        job->resultIsError = isError;
                    };
                    const auto cancelled = [] {
                        return ca::ErrStatus(ca::StatusCode::CANCELLED, "workflow job cancelled");
                    };
                    const auto finishCancelled = [&] {
                        const auto status = cancelled();
                        finish("cancelled",
                               {{"error", "workflow job cancelled"}, {"status", statusJson(status)}},
                               true);
                    };
                    try {
                        {
                            std::lock_guard<std::mutex> lock(job->mutex);
                            if (job->cancellationRequested.load()) {
                                const auto status = cancelled();
                                job->state = "cancelled";
                                job->result = {{"error", "workflow job cancelled"},
                                               {"status", statusJson(status)}};
                                job->resultIsError = true;
                                return;
                            }
                            job->state = "running";
                        }

                        mjt::JarWorkspace detached;
                        const auto openStatus = detached.openJar(snapshot.string());
                        if (openStatus.is_err()) {
                            finish("failed",
                                   {{"error", "open workflow job snapshot failed"},
                                    {"status", statusJson(shared::toCaStatus(openStatus.unwrap_err()))}},
                                   true);
                            return;
                        }
                        for (const auto& supporting : supportingSnapshots) {
                            const auto supportStatus = detached.addSupportingJar(supporting.string());
                            if (supportStatus.is_err()) {
                                finish("failed",
                                       {{"error", "add workflow job supporting JAR failed"},
                                        {"status", statusJson(shared::toCaStatus(supportStatus.unwrap_err()))}},
                                       true);
                                return;
                            }
                        }
                        if (job->cancellationRequested.load()) {
                            finishCancelled();
                            return;
                        }

                        WorkflowRequest candidateWorkflow;
                        candidateWorkflow.referenceQueries = spec.referenceQueries;
                        candidateWorkflow.runPipeline      = spec.runPipeline;
                        candidateWorkflow.pipeline         = spec.pipeline;
                        mjbase::asm_remapper::MemberRemapPlan candidatePlan;
                        mjbase::asm_remapper::RemapPolicy     candidatePolicy;
                        if (spec.remap.has_value()) {
                            const auto remapStatus = buildWorkspaceRemapPlan(
                                detached, *spec.remap, candidatePlan, candidatePolicy);
                            if (remapStatus.is_err()) {
                                finish("failed",
                                       {{"error", "build workflow remap plan failed"},
                                        {"status", statusJson(shared::toCaStatus(remapStatus))}},
                                       true);
                                return;
                            }
                            candidateWorkflow.remapPlan   = &candidatePlan;
                            candidateWorkflow.remapPolicy = candidatePolicy;
                        }

                        std::vector<DecompiledSource> sources;
                        candidateWorkflow.candidateJarStageName = "decompile";
                        candidateWorkflow.candidateJarConsumer =
                            [job, &sources, classes = spec.decompileClasses, options = spec.decompiler](
                                const std::string& candidateJar) -> mjbase::workspace::WorkspaceErrorInfo {
                                for (const auto& className : classes) {
                                    if (job->cancellationRequested.load()) {
                                        return {mjbase::workspace::WorkspaceError::Aborted,
                                                "workflow job cancelled"};
                                    }
                                    auto source = decompileSnapshotClass(
                                        candidateJar,
                                        className,
                                        options,
                                        [job] { return job->cancellationRequested.load(); });
                                    if (source.is_err()) return shared::fromCaStatus(source.unwrap_err());
                                    sources.push_back(std::move(source).unwrap());
                                }
                                return {};
                            };
                        const auto candidateJar = directory.path() / "candidate.jar";
                        candidateWorkflow.exportPath = candidateJar.string();
                        MutationRequest detachedRequest;
                        detachedRequest.expectedRevision = 0;
                        detachedRequest.label            = spec.mutation.label;
                        auto report = detached.runWorkflow(candidateWorkflow, detachedRequest);
                        report.exportPath.clear();
                        report.exported = false;
                        report.stages.erase(
                            std::remove_if(report.stages.begin(),
                                           report.stages.end(),
                                           [](const auto& stage) {
                                               return stage.name == "export" ||
                                                      stage.name == "export_preview";
                                           }),
                            report.stages.end());
                        if (report.mutation.status.is_err()) {
                            auto payload = workflowJson(report);
                            appendWorkflowSources(payload, sources, spec.decompileBatch);
                            //候选通道的取消以 Aborted 承载（detached workspace 上
                            //revision 恒匹配，不会产生其他 Aborted 错误）。
                            const auto state =
                                report.mutation.status.code == mjbase::workspace::WorkspaceError::Aborted
                                    ? "cancelled"
                                    : "failed";
                            finish(state, std::move(payload), true);
                            return;
                        }
                        if (job->cancellationRequested.load()) {
                            finishCancelled();
                            return;
                        }

                        bool promoteCancelled = false;
                        {
                            std::lock_guard<std::mutex> lock(job->mutex);
                            if (job->cancellationRequested.load()) promoteCancelled = true;
                            else job->state = "committing";
                        }
                        if (promoteCancelled) {
                            finishCancelled();
                            return;
                        }

                        ca::Status commitStatus = ca::OkStatus();
                        bool       attemptedCommit = false;
                        {
                            //commit 会写回主 workspace，必须与同一 session 的工具执行体
                            //串行：按 executionMutex_ → mutex_ 的全局锁顺序获取（M1 修复）。
                            std::lock_guard<std::mutex> executionLock(executionMutex_);
                            std::lock_guard<std::mutex> serviceLock(mutex_);
                            if (job->cancellationRequested.load()) {
                                commitStatus = cancelled();
                            }
                            else {
                                const auto currentWorkspace = workspaces_.find(job->workspaceId);
                                if (currentWorkspace == workspaces_.end()) {
                                    commitStatus = ca::ErrStatus(ca::StatusCode::NOT_FOUND,
                                                                 "workflow job workspace was closed");
                                }
                                else {
                                    WorkflowCandidateCommitRequest commitRequest;
                                    commitRequest.candidateJarPath = candidateJar.string();
                                    commitRequest.exportPath       = spec.exportPath;
                                    attemptedCommit = true;
                                    auto commitMutation = spec.mutation;
                                    commitMutation.expectedRevision = job->sourceRevision;
                                    auto committed =
                                        currentWorkspace->second->commitWorkflowCandidateJar(
                                            commitRequest,
                                            commitMutation);
                                    commitStatus      = shared::toCaStatus(committed.mutation.status);
                                    report.mutation   = std::move(committed.mutation);
                                    report.exportPath = std::move(committed.exportPath);
                                    report.exported   = committed.exported;
                                }
                            }
                        }
                        if (commitStatus.is_err()) {
                            if (!attemptedCommit) {
                                report.mutation.status    = shared::fromCaStatus(commitStatus);
                                report.mutation.revision  = job->sourceRevision;
                                report.mutation.committed = false;
                            }
                            report.stages.push_back({"commit", shared::fromCaStatus(commitStatus)});
                            auto payload = workflowJson(report);
                            appendWorkflowSources(payload, sources, spec.decompileBatch);
                            const auto state = commitStatus.code() == ca::StatusCode::CANCELLED
                                                   ? "cancelled"
                                                   : "failed";
                            finish(state, std::move(payload), true);
                            return;
                        }

                        report.stages.push_back(
                            {spec.mutation.dryRun ? "commit_preview" : "commit",
                             mjbase::workspace::WorkspaceErrorInfo{}});
                        auto payload = workflowJson(report);
                        appendWorkflowSources(payload, sources, spec.decompileBatch);
                        finish("succeeded", std::move(payload), false);
                    }
                    catch (const std::exception& exception) {
                        finish("failed", {{"error", exception.what()}}, true);
                    }
                    catch (...) {
                        finish("failed", {{"error", "unknown workflow job failure"}}, true);
                    }
                });
            }
            catch (const std::exception& exception) {
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    jobs_.erase(job->id);
                }
                std::error_code cleanupError;
                fs::remove_all(snapshotDirectory, cleanupError);
                return toolError("start workflow job failed: " + std::string(exception.what()));
            }
            return toolResult(jobStateJson(job));
        }

        std::vector<DecompiledSource> workflowSources;
        if (!decompileClasses.empty()) {
            workflow.candidateJarStageName = "decompile";
            workflow.candidateJarConsumer =
                [decompileClasses, options, &workflowSources](
                    const std::string& candidateJar) -> mjbase::workspace::WorkspaceErrorInfo {
                for (const auto& className : decompileClasses) {
                    auto source = decompileSnapshotClass(candidateJar, className, options);
                    if (source.is_err()) return shared::fromCaStatus(source.unwrap_err());
                    workflowSources.push_back(std::move(source).unwrap());
                }
                return {};
            };
        }

        const auto result = current->runWorkflow(workflow, mutationRequest(arguments));
        auto payload = workflowJson(result);
        appendWorkflowSources(payload, workflowSources, decompileBatch);
        return toolResult(std::move(payload), result.mutation.status.is_err());
    }
    return std::nullopt;
}

}   //namespace mjt::mcp
