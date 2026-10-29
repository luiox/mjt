#pragma once

#include <string>

#include "libca/core/status.hpp"
#include "workspace/i_workspace.hpp"

namespace mjt {

///@brief JAR 重映射请求，供 mapping 与 decompile 命令复用。
struct JarRemapRequest
{
    ///@brief 输入 JAR。
    std::string inputPath;
    ///@brief 输出 JAR。
    std::string outputPath;
    ///@brief mapping 文件。
    std::string mappingPath;
    ///@brief mapping 格式名。
    std::string format;
    ///@brief 重映射策略：bytecode-only、reflection、metadata 或 all。
    std::string policy = "bytecode-only";
};

///@brief 解析 mapping 并重写整个 JAR。
///@return 成功时返回是否至少修改了一处；格式、文件或 class 写回失败时返回状态。
ca::StatusOr<bool> remapJar(const JarRemapRequest& request);

///@brief 在现有 Workspace 的候选副本中解析并分析一份 mapping 文本。
///@note 解析、class/package/member plan 构建与 CLI JAR remap 共用同一实现。
struct WorkspaceRemapRequest final
{
    std::string format;
    std::string mappingText;
    std::string policy = "bytecode-only";
};

///@brief 将 mapping 文本解析为可供单一 Workspace 候选事务复用的 remap 计划。
///@note 输出计划必须在使用它的 Workspace 调用返回前保持有效。
mjt::workspace::WorkspaceErrorInfo buildWorkspaceRemapPlan(
    mjt::workspace::IWorkspace& workspace, const WorkspaceRemapRequest& request,
    mjbase::asm_remapper::MemberRemapPlan& plan, mjbase::asm_remapper::RemapPolicy& policy);

///@brief 返回 mapping 应用后的引用影响、验证诊断和字节 diff，不提交工作区。
mjt::workspace::ImpactAnalysis analyzeWorkspaceRemap(
    mjt::workspace::IWorkspace& workspace, const WorkspaceRemapRequest& request);

///@brief 原子应用 mapping 文本；dryRun 时返回与分析一致的 MutationResult。
mjt::workspace::MutationResult applyWorkspaceRemap(
    mjt::workspace::IWorkspace& workspace, const WorkspaceRemapRequest& request,
    const mjt::workspace::MutationRequest& mutation = {});

}   //namespace mjt
