#pragma once

//opt_dry_run：`mjt opt --dry-run` 的零落盘试运行执行器（issue ）。
//
//红线（任务书口径）：
//- 全程 stdout、零落盘——不写 staged tmp、不做 verify、不发布输出；
//- -o 在 dry-run 下不要求；「输出已存在则拒绝」检查由调用方（cmd_opt）跳过；
//- pass 管线行为零改动——与实跑共用同一 PipelineBuilder 构建路径，计数经
//mbase PassRunStats 只读装饰器采集（run 参数/返回值/异常一律透传）。
//
//三档输出（CLI 形态：单一子命令级旗标 `--dry-run <mode>`，见 cmd_opt）：
//json 机器档：固定键序单文档 JSON（schema "mjt.opt.dry-run/1"），退出码与
//实跑一致（report.ok ? 0 : 5），stdout 除该文档外无其他输出；
//counts 人读档：pass 级摘要行 + 变更类清单；
//diff jasm-diff 档：仅对字节级变更类，前后各自经
//jasm::JvmJasmService::disassemble（disasm 命令同一渲染层）渲染为
//规范化 jasm 后做行级 unified diff；差异行全局预算
//--dry-run-diff-limit（缺省 200），超限截断并显式标注。

#include <cstddef>
#include <string>

#include "jbytecode/jbytecode.hpp"
#include "mbase/cli/cli_runner.hpp"
#include "mbase/pass/pass_registry.hpp"

namespace mjt::opt_dry_run {

///三档输出档位。
enum class OutputMode
{
    Counts,    ///< 人读摘要档
    Json,      ///< 机器档：固定键序 JSON 单文档
    JasmDiff,  ///< jasm-diff 档：仅变更类，有界输出
};

///diff 档全局差异行预算缺省值（--dry-run-diff-limit 可覆盖；报告口径）。
inline constexpr std::size_t kDefaultDiffLimit = 200;

///dry-run 请求参数（均由 cmd_opt 从已解析的 CLI 面装配）。
struct DryRunRequest
{
    std::string input;         ///< 输入 jar/class（必填，cmd_opt 已校验）
    std::string targetClass;   ///< -c internal name 子串；空 = 不过滤
    std::string targetMethod;  ///< -m 原始 spec（仅用于 JSON "method" 字段回显）
    std::string runMode;       ///< 选择器描述（"pipeline:<name>" / "passes:a,b"）
    OutputMode  mode      = OutputMode::Counts;
    std::size_t diffLimit = kDefaultDiffLimit;  ///< diff 档差异行全局预算（≥1）
};

///@brief dry-run 主入口：import → 字节快照 → 管线（带只读计数）→ 对比 → 渲染 stdout。
///
///骨架与 mbase runCli 全局流水线路径逐段对齐（import / -c 过滤 / runJarPipeline /
///processed 计数 / 同文案同类别失败面），仅截去写出段（staged tmp、verify、发布、
///"Done:" 行）。返回的 CliRunReport 与实跑同构，退出码由调用方沿用
///report.ok ? 0 : 5。
///
///@param request 请求参数（见上）
///@param builder 已配置的流水线构建器（与实跑同一构建路径）
///@param predicate 方法过滤谓词（CLI -m 解析产物；可为 nullptr）
///@param writerFlags 与实跑一致（--jdk 时 COMPUTE_MAXS | COMPUTE_FRAMES）
///@param typeResolver 与实跑一致（--jdk 的类层次解析器；可为 nullptr）
mjt::base::CliRunReport run(const DryRunRequest& request,
                                const mjt::base::PipelineBuilder& builder,
                                mjt::base::MethodPredicate predicate, int writerFlags,
                                jbytecode::core::ICommonSuperClassProvider* typeResolver);

}   //namespace mjt::opt_dry_run
