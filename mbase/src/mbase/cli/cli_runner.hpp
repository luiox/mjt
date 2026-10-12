#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "jbytecode/core/frame.hpp"   //ICommonSuperClassProvider
#include "mbase/spi/asm_pass.hpp"   //GlobalPassManager, AsmGlobalContext, MethodPredicate

namespace mjt::base {

//前向声明：runCli 可选注入进度 sink（issue ）。
class ProgressSink;

///@brief CLI 通用参数（-i / -o / --jdk / -c / -m）。
///
///input/output 为必填，jdkPath 为空表示不启用 COMPUTE_FRAMES。
///
///targetClass（-c）非空时只限制 pipeline 的处理集合：匹配类会被处理，
///未匹配 class 和非 class 资源仍会保留并写入输出 JAR。targetMethod（-m）
///非空时只处理匹配方法，类本身仍参与读写 roundtrip。
struct CliOptions
{
    std::string input;
    std::string output;
    std::string jdkPath;
    std::string targetClass;       //-c: internal name 子串匹配（org/example/Demo）
    std::string targetMethod;      //-m: "name" 或 "nameI"
    bool        quiet   = false;   //--quiet: 完全静默，仅最终总结行
    bool        verbose = false;   //--verbose: 非 TTY 下也输出换行式阶段日志
};

///@brief 单个 class 的处理结果。
struct ClassProcessResult
{
    bool        ok = true;
    std::string message;
};

///@brief class 字节处理器：输入 URI + class 字节，原地修改字节并返回处理状态。
///
///@param uri 资源 URI（jar 内路径或 class 文件路径）
///@param bytes class 字节（引用，处理器直接原地改写）
///@return 处理结果（ok=false 时计入失败计数，message 用于日志）
using ClassProcessor =
    std::function<ClassProcessResult(const std::string& uri, std::vector<ca::u8>& bytes)>;

///CLI 运行失败的稳定分类，供命令行和发布 smoke test 使用。
enum class CliFailureKind
{
    None,
    InvalidArguments,
    UnsupportedInput,
    Read,
    NoClass,
    NoMatch,
    Processing,
    Write,
    Verification,
};

///@brief 整体运行报告。
///
///`failure` 和 `failureMessage` 是稳定的机器可读失败契约；调用方不得只依据 stderr
///文本判断失败原因。成功写出后 runner 会重新读取输出，并确认每个 class 都能重新解析；
///JAR 中后缀为 `.class` 但未被识别为 class 资源的条目同样会导致 `Verification`。
struct CliRunReport
{
    ca::i32 processed = 0;   //成功处理的 class 数
    ca::i32 failed    = 0;   //失败的 class 数
    CliFailureKind failure = CliFailureKind::None;
    std::string    failureMessage;
    //成功 = 处理了至少一个 class 且无失败（空输入/不支持的类型/全部失败都算失败）
    bool ok() const { return processed > 0 && failed == 0; }
};

///@brief CLI 公共骨架：import → 对每个 ClassResource 跑 processor → export。
///
///消除 mjt-optimizer / mjt-pass 等工具重复的「解析参数 → 读资源 → 转换 →
///写出」流程。文件 IO 走 ca::fs::FileUtil / ResourceHelper，不手写 ifstream。输出先写入
///同目录临时文件，逐个重新解析 class 验证后才发布到此前不存在的目标路径。
///
///输入类型由 JarUtil::isJarFile / isClassFile 自动分发：
///- jar：importFromJar → 遍历所有 ClassResource → exportToJar
///- class：importFromClassFile → 处理 → exportToClassFile
///
///@param options CLI 参数
///@param processor class 字节处理器
///@return 运行报告；输入非法（空路径/不支持的类型/无 class）时 report.ok=false
///且 processed=0，调用方据此返回非零退出码。
CliRunReport runCli(const CliOptions& options, const ClassProcessor& processor);

///@brief CLI 公共骨架（全局流水线重载）：import → runJarPipeline → export。
///
///与 per-class 版本的区别：processor 被 GlobalPassManager 替代。
///整个 jar 的 class 先导入 ResourceContainer，再跑全局 pipeline，最后导出。
///arena 由内部 AsmGlobalContext 持有并贯穿整个作业。
///
///Class filtering deliberately does not delete unmatched resources from the
///exported container. When `options.targetClass` is set, the runner builds a
///temporary class-only processing container, runs the pipeline there, and copies
///changed bytes back to the original container before export.
///
///@param options CLI 参数
///@param gpm 全局流水线（由 PipelineBuilder.buildNestedPipeline 构建）
///@param progress 可选进度 sink（issue ）；nullptr 无进度反馈
///@param typeResolver 可选 CommonSuperClass 解析器（--jdk 时传入）。注入到
///GlobalAnalysisManager，供改写控制流的 pass（如
///ControlflowJumpPass）在 writeback 时用 COMPUTE_FRAMES
///重算 StackMapTable。nullptr 表示无 JDK resolver。
///@return 运行报告
CliRunReport runCli(const CliOptions& options, GlobalPassManager& gpm,
                    ProgressSink*                               progress      = nullptr,
                    jbytecode::core::ICommonSuperClassProvider* typeResolver = nullptr);

///@brief 把 targetMethod（"name" 或 "nameI"）解析成方法过滤谓词。
///
///- "name" → name 精确匹配，descriptor 任意
///- "nameI" → name 精确匹配 AND descriptor 精确匹配
///- "" → 返回 nullptr（不过滤）
///
///供 cmdOpt 在 buildNestedPipeline(...predicate) 时使用。
MethodPredicate buildMethodPredicate(const std::string& targetMethod);

}   //namespace mjt::base
