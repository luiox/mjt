#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

TEST(McpServerTest, CommitsAsyncCandidateWorkflowAfterSingleClassDecompile)
{
    const auto temporaryBefore = workflowTemporaryArtifacts();
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto fakeDecompiler = test::getTempOutPath("fake-mcp-decompiler.jar");
    std::ofstream(fakeDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");
    const auto supporting = test::getTempOutPath("mjt_mcp_workflow_supporting.jar");
    std::error_code supportingError;
    std::filesystem::remove(supporting, supportingError);
    std::filesystem::copy_file(fixture, supporting, supportingError);
    ASSERT_FALSE(supportingError);
    const auto addedSupport = call(server,
                                   "workspace_add_support",
                                   {{"workspace_id", workspaceId},
                                    {"path", supporting.string()}});
    ASSERT_FALSE(addedSupport.at("result").at("isError"));
    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/WorkflowSubject\n";

    const auto started = call(
        server,
        "analysis_workflow",
        {{"workspace_id", workspaceId},
         {"mapping_format", "Tiny-V2"},
         {"mapping_text", mapping},
         {"mapping_policy", "all"},
         {"decompile_class", "mcp/WorkflowSubject"},
         {"decompiler_jar", fakeDecompiler.string()},
         {"java", fakeJava.string()},
         {"expected_revision", 0},
         {"async", true}});
    ASSERT_FALSE(started.at("result").at("isError"));
    supportingError.clear();
    std::filesystem::remove(supporting, supportingError);
    ASSERT_FALSE(supportingError);
    const auto jobId = payload(started).at("job_id").get<std::string>();
    const auto completed = waitForTerminalJob(server, jobId);
    ASSERT_FALSE(completed.at("result").at("isError"));
    ASSERT_EQ("succeeded", payload(completed).at("state"));
    ASSERT_FALSE(payload(completed).at("result_is_error"));
    const auto& report = payload(completed).at("result");
    EXPECT_TRUE(report.at("mutation").at("committed"));
    EXPECT_EQ(1, report.at("mutation").at("revision"));
    EXPECT_FALSE(report.at("exported"));
    EXPECT_EQ("mcp/WorkflowSubject", report.at("decompiled_source").at("internal_name"));
    EXPECT_NE(std::string::npos,
              report.at("decompiled_source").at("source").get<std::string>().find("WorkflowSubject"));
    EXPECT_EQ(std::string::npos, report.dump().find("candidate.jar"));

    const auto remapped = call(server,
                               "class_get",
                               {{"workspace_id", workspaceId},
                                {"class", "mcp/WorkflowSubject"}});
    ASSERT_FALSE(remapped.at("result").at("isError"));
    releaseJob(server, jobId);
    expectNoNewWorkflowTemporaryArtifacts(temporaryBefore);
}

TEST(McpServerTest, CommitsAsyncCandidateWorkflowAndAtomicallyExportsBatchResult)
{
    const auto temporaryBefore = workflowTemporaryArtifacts();
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto fakeDecompiler = test::getTempOutPath("fake-mcp-decompiler.jar");
    std::ofstream(fakeDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");
    const auto output = test::getTempOutPath("mjt_mcp_async_workflow_output.jar");
    std::error_code outputError;
    std::filesystem::remove(output, outputError);
    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/WorkflowSubject\n";

    const auto started = call(
        server,
        "analysis_workflow",
        {{"workspace_id", workspaceId},
         {"mapping_format", "Tiny-V2"},
         {"mapping_text", mapping},
         {"mapping_policy", "all"},
         {"decompile_classes", Json::array({"mcp/WorkflowSubject", "mcp/Second"})},
         {"decompiler_jar", fakeDecompiler.string()},
         {"java", fakeJava.string()},
         {"export_path", output.string()},
         {"expected_revision", 0},
         {"async", true}});
    ASSERT_FALSE(started.at("result").at("isError"));
    const auto jobId = payload(started).at("job_id").get<std::string>();
    const auto completed = waitForTerminalJob(server, jobId);
    ASSERT_FALSE(completed.at("result").at("isError"));
    ASSERT_EQ("succeeded", payload(completed).at("state"));
    ASSERT_FALSE(payload(completed).at("result_is_error"));
    const auto& report = payload(completed).at("result");
    EXPECT_TRUE(report.at("mutation").at("committed"));
    EXPECT_EQ(1, report.at("mutation").at("revision"));
    EXPECT_TRUE(report.at("exported"));
    ASSERT_EQ(2U, report.at("decompiled_sources").size());
    EXPECT_EQ("mcp/WorkflowSubject", report.at("decompiled_sources").at(0).at("internal_name"));
    EXPECT_EQ("mcp/Second", report.at("decompiled_sources").at(1).at("internal_name"));
    EXPECT_EQ(std::filesystem::absolute(output).string(), report.at("export_path"));
    EXPECT_TRUE(std::filesystem::exists(output));
    EXPECT_EQ(std::string::npos, report.dump().find("candidate.jar"));
    EXPECT_EQ(std::string::npos, report.dump().find("mjt-mcp-decompile-"));

    const auto exportedWorkspace = call(server,
                                        "workspace_open",
                                        {{"path", output.string()},
                                         {"workspace_id", "exported-workflow"}});
    ASSERT_FALSE(exportedWorkspace.at("result").at("isError"));
    const auto exportedClass = call(server,
                                    "class_get",
                                    {{"workspace_id", "exported-workflow"},
                                     {"class", "mcp/WorkflowSubject"}});
    ASSERT_FALSE(exportedClass.at("result").at("isError"));
    releaseJob(server, jobId);
    expectNoNewWorkflowTemporaryArtifacts(temporaryBefore);
}

TEST(McpServerTest, PreviewsAsyncCandidateWorkflowWithoutCommitOrExport)
{
    const auto temporaryBefore = workflowTemporaryArtifacts();
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto fakeDecompiler = test::getTempOutPath("fake-mcp-decompiler.jar");
    std::ofstream(fakeDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");
    const auto output = test::getTempOutPath("mjt_mcp_async_workflow_preview.jar");
    std::error_code outputError;
    std::filesystem::remove(output, outputError);
    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/WorkflowSubject\n";

    const auto started = call(
        server,
        "analysis_workflow",
        {{"workspace_id", workspaceId},
         {"mapping_format", "Tiny-V2"},
         {"mapping_text", mapping},
         {"mapping_policy", "all"},
         {"decompile_class", "mcp/WorkflowSubject"},
         {"decompiler_jar", fakeDecompiler.string()},
         {"java", fakeJava.string()},
         {"export_path", output.string()},
         {"dry_run", true},
         {"expected_revision", 0},
         {"async", true}});
    ASSERT_FALSE(started.at("result").at("isError"));
    const auto jobId = payload(started).at("job_id").get<std::string>();
    const auto completed = waitForTerminalJob(server, jobId);
    ASSERT_FALSE(completed.at("result").at("isError"));
    ASSERT_EQ("succeeded", payload(completed).at("state"));
    const auto& report = payload(completed).at("result");
    EXPECT_FALSE(report.at("mutation").at("committed"));
    EXPECT_EQ(0, report.at("mutation").at("revision"));
    EXPECT_FALSE(report.at("exported"));
    EXPECT_FALSE(std::filesystem::exists(output));

    const auto original = call(server,
                               "class_get",
                               {{"workspace_id", workspaceId}, {"class", "mcp/Subject"}});
    ASSERT_FALSE(original.at("result").at("isError"));
    const auto remapped = call(server,
                               "class_get",
                               {{"workspace_id", workspaceId},
                                {"class", "mcp/WorkflowSubject"}});
    EXPECT_TRUE(remapped.at("result").at("isError"));
    releaseJob(server, jobId);
    expectNoNewWorkflowTemporaryArtifacts(temporaryBefore);
}

TEST(McpServerTest, CancelsImmediateAndRunningAsyncCandidateWorkflowsWithoutCommit)
{
    const auto temporaryBefore = workflowTemporaryArtifacts();
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto slowDecompiler = test::getTempOutPath("fake-mcp-decompiler-slow.jar");
    std::ofstream(slowDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");
    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/WorkflowSubject\n";
    const auto startJob = [&](const std::filesystem::path& output) {
        std::error_code outputError;
        std::filesystem::remove(output, outputError);
        return call(server,
                    "analysis_workflow",
                    {{"workspace_id", workspaceId},
                     {"mapping_format", "Tiny-V2"},
                     {"mapping_text", mapping},
                     {"mapping_policy", "all"},
                     {"decompile_class", "mcp/WorkflowSubject"},
                     {"decompiler_jar", slowDecompiler.string()},
                     {"java", fakeJava.string()},
                     {"export_path", output.string()},
                     {"async", true}});
    };

    const auto immediateOutput = test::getTempOutPath("mjt_mcp_cancelled_queued_workflow.jar");
    const auto immediateStarted = startJob(immediateOutput);
    ASSERT_FALSE(immediateStarted.at("result").at("isError"));
    const auto immediateJobId = payload(immediateStarted).at("job_id").get<std::string>();
    const auto immediateCancelled = call(server, "job_cancel", {{"job_id", immediateJobId}});
    ASSERT_FALSE(immediateCancelled.at("result").at("isError"));
    EXPECT_TRUE(payload(immediateCancelled).at("cancel_requested"));
    const auto immediateCompleted = waitForTerminalJob(server, immediateJobId);
    ASSERT_EQ("cancelled", payload(immediateCompleted).at("state"));
    EXPECT_TRUE(payload(immediateCompleted).at("result_is_error"));
    EXPECT_FALSE(std::filesystem::exists(immediateOutput));
    releaseJob(server, immediateJobId);

    const auto runningOutput = test::getTempOutPath("mjt_mcp_cancelled_running_workflow.jar");
    const auto runningStarted = startJob(runningOutput);
    ASSERT_FALSE(runningStarted.at("result").at("isError"));
    const auto runningJobId = payload(runningStarted).at("job_id").get<std::string>();
    const auto running = waitForJobState(server, runningJobId, "running");
    ASSERT_FALSE(running.at("result").at("isError"));
    ASSERT_EQ("running", payload(running).at("state"));
    const auto runningCancelled = call(server, "job_cancel", {{"job_id", runningJobId}});
    ASSERT_FALSE(runningCancelled.at("result").at("isError"));
    EXPECT_TRUE(payload(runningCancelled).at("cancel_requested"));
    const auto runningCompleted = waitForTerminalJob(server, runningJobId);
    ASSERT_EQ("cancelled", payload(runningCompleted).at("state"));
    EXPECT_TRUE(payload(runningCompleted).at("result_is_error"));
    EXPECT_FALSE(std::filesystem::exists(runningOutput));
    releaseJob(server, runningJobId);

    const auto info = call(server, "workspace_info", {{"workspace_id", workspaceId}});
    ASSERT_FALSE(info.at("result").at("isError"));
    EXPECT_EQ(0, payload(info).at("revision"));
    const auto original = call(server,
                               "class_get",
                               {{"workspace_id", workspaceId}, {"class", "mcp/Subject"}});
    ASSERT_FALSE(original.at("result").at("isError"));
    const auto remapped = call(server,
                               "class_get",
                               {{"workspace_id", workspaceId},
                                {"class", "mcp/WorkflowSubject"}});
    EXPECT_TRUE(remapped.at("result").at("isError"));
    expectNoNewWorkflowTemporaryArtifacts(temporaryBefore);
}

TEST(McpServerTest, TimesOutAsyncCandidateWorkflowWithoutCommitOrArtifacts)
{
    const auto temporaryBefore = workflowTemporaryArtifacts();
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto slowDecompiler = test::getTempOutPath("fake-mcp-decompiler-slow.jar");
    std::ofstream(slowDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");
    const auto output = test::getTempOutPath("mjt_mcp_timed_out_workflow.jar");
    std::error_code outputError;
    std::filesystem::remove(output, outputError);
    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/WorkflowSubject\n";

    const auto started = call(
        server,
        "analysis_workflow",
        {{"workspace_id", workspaceId},
         {"mapping_format", "Tiny-V2"},
         {"mapping_text", mapping},
         {"mapping_policy", "all"},
         {"decompile_class", "mcp/WorkflowSubject"},
         {"decompiler_jar", slowDecompiler.string()},
         {"java", fakeJava.string()},
         {"timeout_ms", 25},
         {"export_path", output.string()},
         {"async", true}});
    ASSERT_FALSE(started.at("result").at("isError"));
    const auto jobId = payload(started).at("job_id").get<std::string>();
    const auto completed = waitForTerminalJob(server, jobId);
    ASSERT_FALSE(completed.at("result").at("isError"));
    ASSERT_EQ("failed", payload(completed).at("state"));
    EXPECT_TRUE(payload(completed).at("result_is_error"));
    EXPECT_FALSE(payload(completed).at("result").at("mutation").at("committed"));
    EXPECT_FALSE(std::filesystem::exists(output));
    releaseJob(server, jobId);

    const auto info = call(server, "workspace_info", {{"workspace_id", workspaceId}});
    ASSERT_FALSE(info.at("result").at("isError"));
    EXPECT_EQ(0, payload(info).at("revision"));
    expectNoNewWorkflowTemporaryArtifacts(temporaryBefore);
}

TEST(McpServerTest, RejectsFailedCandidateDecompilerWithoutCommitOrArtifacts)
{
    const auto temporaryBefore = workflowTemporaryArtifacts();
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto failedDecompiler = test::getTempOutPath("fake-mcp-decompiler-fail.jar");
    std::ofstream(failedDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");
    const auto output = test::getTempOutPath("mjt_mcp_failed_workflow.jar");
    std::error_code outputError;
    std::filesystem::remove(output, outputError);

    const auto missingClasses = call(server,
                                     "analysis_workflow",
                                     {{"workspace_id", workspaceId}, {"async", true}});
    ASSERT_TRUE(missingClasses.at("result").at("isError"));
    EXPECT_EQ("async workflow requires decompile_class or decompile_classes",
              payload(missingClasses).at("error"));

    const auto started = call(
        server,
        "analysis_workflow",
        {{"workspace_id", workspaceId},
         {"decompile_class", "mcp/Subject"},
         {"decompiler_jar", failedDecompiler.string()},
         {"java", fakeJava.string()},
         {"export_path", output.string()},
         {"async", true}});
    ASSERT_FALSE(started.at("result").at("isError"));
    const auto jobId = payload(started).at("job_id").get<std::string>();
    const auto completed = waitForTerminalJob(server, jobId);
    ASSERT_FALSE(completed.at("result").at("isError"));
    ASSERT_EQ("failed", payload(completed).at("state"));
    EXPECT_TRUE(payload(completed).at("result_is_error"));
    EXPECT_FALSE(payload(completed).at("result").at("mutation").at("committed"));
    EXPECT_FALSE(std::filesystem::exists(output));
    releaseJob(server, jobId);

    const auto info = call(server, "workspace_info", {{"workspace_id", workspaceId}});
    ASSERT_FALSE(info.at("result").at("isError"));
    EXPECT_EQ(0, payload(info).at("revision"));
    expectNoNewWorkflowTemporaryArtifacts(temporaryBefore);
}

TEST(McpServerTest, RejectsAsyncCandidateWorkflowWhenRevisionBecomesStale)
{
    const auto temporaryBefore = workflowTemporaryArtifacts();
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto slowDecompiler = test::getTempOutPath("fake-mcp-decompiler-slow.jar");
    std::ofstream(slowDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");
    const auto output = test::getTempOutPath("mjt_mcp_stale_workflow_output.jar");
    std::error_code outputError;
    std::filesystem::remove(output, outputError);
    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/WorkflowSubject\n";

    const auto started = call(
        server,
        "analysis_workflow",
        {{"workspace_id", workspaceId},
         {"mapping_format", "Tiny-V2"},
         {"mapping_text", mapping},
         {"mapping_policy", "all"},
         {"decompile_class", "mcp/WorkflowSubject"},
         {"decompiler_jar", slowDecompiler.string()},
         {"java", fakeJava.string()},
         {"export_path", output.string()},
         {"expected_revision", 0},
         {"async", true}});
    ASSERT_FALSE(started.at("result").at("isError"));
    const auto jobId = payload(started).at("job_id").get<std::string>();

    const auto changed = call(server,
                              "patch_apply_jasm",
                              {{"workspace_id", workspaceId},
                               {"uri", "mcp/Subject.class"},
                               {"source_name", "Subject.jasm"},
                               {"source", classSource("3")},
                               {"expected_revision", 0}});
    ASSERT_FALSE(changed.at("result").at("isError"));
    EXPECT_EQ(1, payload(changed).at("revision"));

    const auto completed = waitForTerminalJob(server, jobId);
    ASSERT_FALSE(completed.at("result").at("isError"));
    ASSERT_EQ("failed", payload(completed).at("state"));
    EXPECT_TRUE(payload(completed).at("result_is_error"));
    EXPECT_EQ(static_cast<int>(ca::StatusCode::ABORTED),
              payload(completed).at("result").at("mutation").at("status").at("code"));
    EXPECT_FALSE(payload(completed).at("result").at("mutation").at("committed"));
    EXPECT_FALSE(std::filesystem::exists(output));

    const auto original = call(server,
                               "class_get",
                               {{"workspace_id", workspaceId}, {"class", "mcp/Subject"}});
    ASSERT_FALSE(original.at("result").at("isError"));
    const auto remapped = call(server,
                               "class_get",
                               {{"workspace_id", workspaceId},
                                {"class", "mcp/WorkflowSubject"}});
    EXPECT_TRUE(remapped.at("result").at("isError"));
    releaseJob(server, jobId);
    expectNoNewWorkflowTemporaryArtifacts(temporaryBefore);
}

TEST(McpServerTest, RejectsAsyncCandidateWorkflowAfterWorkspaceClose)
{
    const auto temporaryBefore = workflowTemporaryArtifacts();
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto slowDecompiler = test::getTempOutPath("fake-mcp-decompiler-slow.jar");
    std::ofstream(slowDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");
    const auto output = test::getTempOutPath("mjt_mcp_closed_workflow_output.jar");
    std::error_code outputError;
    std::filesystem::remove(output, outputError);

    const auto started = call(server,
                              "analysis_workflow",
                              {{"workspace_id", workspaceId},
                               {"decompile_class", "mcp/Subject"},
                               {"decompiler_jar", slowDecompiler.string()},
                               {"java", fakeJava.string()},
                               {"export_path", output.string()},
                               {"async", true}});
    ASSERT_FALSE(started.at("result").at("isError"));
    const auto jobId = payload(started).at("job_id").get<std::string>();
    const auto running = waitForJobState(server, jobId, "running");
    ASSERT_EQ("running", payload(running).at("state"));
    const auto closed = call(server, "workspace_close", {{"workspace_id", workspaceId}});
    ASSERT_FALSE(closed.at("result").at("isError"));

    const auto completed = waitForTerminalJob(server, jobId);
    ASSERT_FALSE(completed.at("result").at("isError"));
    ASSERT_EQ("failed", payload(completed).at("state"));
    EXPECT_TRUE(payload(completed).at("result_is_error"));
    EXPECT_EQ(static_cast<int>(ca::StatusCode::NOT_FOUND),
              payload(completed).at("result").at("mutation").at("status").at("code"));
    EXPECT_FALSE(payload(completed).at("result").at("mutation").at("committed"));
    EXPECT_FALSE(std::filesystem::exists(output));
    releaseJob(server, jobId);
    expectNoNewWorkflowTemporaryArtifacts(temporaryBefore);
}

TEST(McpServerTest, RejectsAsyncWorkflowExportFailureBeforeCommit)
{
    const auto temporaryBefore = workflowTemporaryArtifacts();
    McpSessionHarness server;
    const auto fixture = writeFixture();
    const auto opened = call(server, "workspace_open", {{"path", fixture.string()}});
    ASSERT_FALSE(opened.at("result").at("isError"));
    const auto workspaceId = payload(opened).at("workspace_id").get<std::string>();
    const auto fakeDecompiler = test::getTempOutPath("fake-mcp-decompiler.jar");
    std::ofstream(fakeDecompiler, std::ios::binary | std::ios::trunc) << "fake";
    const auto fakeJava = test::getBuildTargetPath("mjt_mcp_unittest");
    const auto output = test::getTempOutPath("mjt_mcp_existing_workflow_output.jar");
    std::ofstream(output, std::ios::binary | std::ios::trunc) << "existing";
    const std::string mapping = "tiny\t2\t0\tintermediary\tnamed\n"
                                "c\tmcp/Subject\tmcp/WorkflowSubject\n";

    const auto started = call(
        server,
        "analysis_workflow",
        {{"workspace_id", workspaceId},
         {"mapping_format", "Tiny-V2"},
         {"mapping_text", mapping},
         {"mapping_policy", "all"},
         {"decompile_class", "mcp/WorkflowSubject"},
         {"decompiler_jar", fakeDecompiler.string()},
         {"java", fakeJava.string()},
         {"export_path", output.string()},
         {"expected_revision", 0},
         {"async", true}});
    ASSERT_FALSE(started.at("result").at("isError"));
    const auto jobId = payload(started).at("job_id").get<std::string>();
    const auto completed = waitForTerminalJob(server, jobId);
    ASSERT_FALSE(completed.at("result").at("isError"));
    ASSERT_EQ("failed", payload(completed).at("state"));
    const auto& report = payload(completed).at("result");
    EXPECT_TRUE(payload(completed).at("result_is_error"));
    EXPECT_FALSE(report.at("mutation").at("committed"));
    EXPECT_FALSE(report.at("exported"));
    EXPECT_EQ(std::string::npos, report.dump().find("candidate.jar"));
    EXPECT_TRUE(std::filesystem::exists(output));
    releaseJob(server, jobId);

    const auto info = call(server, "workspace_info", {{"workspace_id", workspaceId}});
    ASSERT_FALSE(info.at("result").at("isError"));
    EXPECT_EQ(0, payload(info).at("revision"));
    const auto original = call(server,
                               "class_get",
                               {{"workspace_id", workspaceId}, {"class", "mcp/Subject"}});
    ASSERT_FALSE(original.at("result").at("isError"));
    expectNoNewWorkflowTemporaryArtifacts(temporaryBefore);
}

}   //namespace
}   //namespace mjt::mcp
