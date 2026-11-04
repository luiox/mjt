/// @file        main.cpp
/// @brief       `mjt` CLI 入口：ca::opt 命令树（info / mapping / jar fix /
///              decompile）+ 全局 --version/--help + 退出码映射。
///
/// 退出码约定（README「CLI usage」同文）：0 成功（含 --help）/ 1 用法错误
/// （参数缺失、文件不存在、格式名不识别、类选择器无命中）/ 2 运行失败
/// （打开/解析/修复/导出/反编译失败）。--help 经 ca::opt 的 HelpRequested
/// 通道返回 0。
///
/// 文案：i18n 表 = mjt_shared（共享层错误文案）+ 本 CLI（mjt.cli.*），语言
/// 由 MJT_LANG 选择（未设置或未注册时落 en_US）。
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "libca/opt/opt.hpp"

#include "i18n_mjt_shared_translations.generated.hpp"
#include "i18n_mjt_cli_core_translations.generated.hpp"
#include "libca/i18n/i18n.hpp"

#include "mjbase/log/log_init.hpp"

#include "commands.hpp"
#include "shared/file_io.hpp"
#include "shared/i18n_errors.hpp"
#include "shared/output_sink.hpp"

namespace {

/// 进程级 i18n 装配：共享层表 + CLI 表；MJT_LANG 优先，缺省 en_US
/// （zh_CN 为仓内 fallback 源语言，MJT_LANG=zh_CN 可显式切换）。
void initI18n()
{
    ca::i18n::init({kI18nTableSet_mjt_shared, kI18nTableSet_mjt_cli_core}, {"MJT_LANG"});
    const char* env = std::getenv("MJT_LANG");
    const char* lang = (env != nullptr && ca::i18n::set_lang(env)) ? env : "en_US";
    ca::i18n::set_lang(lang);
}

/// 用法错误输出：stderr 一行 + 退出码 1。
int usageFailure(const std::string& message)
{
    mjt::err().puts("mjt: " + message);
    return mjt::cli::kExitUsage;
}

/// 运行失败输出：stderr 一行 + 退出码 2。
int runFailure(const std::string& message)
{
    mjt::err().puts("mjt: " + message);
    return mjt::cli::kExitFailure;
}

/// 输入文件存在性属用法校验：不存在 → 退出码 1。
bool requireInputFile(const std::string& path)
{
    if (path.empty()) return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

/// `mjt info -i <jar> [--json]`。
int runInfo(const ca::opt::ParseResult& args)
{
    const std::string input = args.get("input");
    if (!requireInputFile(input))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.input_missing", {input}));

    auto report = mjt::cli::collectInfo(input);
    if (report.is_err())
        return runFailure(std::move(report).unwrap_err().message);

    if (args.has("json")) mjt::cli::renderInfoJson(std::move(report).unwrap(), mjt::out());
    else                  mjt::cli::renderInfoText(std::move(report).unwrap(), mjt::out());
    return mjt::cli::kExitOk;
}

/// `mjt mapping -i <in> -o <out> --from <fmt> --to <fmt> [--remap-jar <jar>
///  --remap-out <jar> --remap-policy <p>]`。
int runMapping(const ca::opt::ParseResult& args)
{
    const std::string input  = args.get("input");
    const std::string output = args.get("output");
    const std::string from   = args.get("from");
    const std::string to     = args.get("to");
    if (!requireInputFile(input))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.input_missing", {input}));
    if (output.empty()) return usageFailure(mjt::shared::trfStr("mjt.cli.err.output_missing"));

    std::string mappingText;
    if (!mjt::readTextFile(input, mappingText))
        return runFailure(mjt::shared::trfStr("mjt.cli.err.mapping_read_failed", {input}));

    auto converted = mjt::cli::convertMappingText(mappingText, from, to);
    if (converted.is_err()) {
        const auto error = std::move(converted).unwrap_err();
        if (error.kind == mjt::cli::MappingError::Kind::UnknownFormat)
            return usageFailure(error.message);
        return runFailure(error.message);
    }

    {
        std::ofstream out(output, std::ios::binary);
        if (!out) return runFailure(mjt::shared::trfStr("mjt.cli.err.mapping_write_failed", {output}));
        out << converted.unwrap();
        out.close();
        if (!out) return runFailure(mjt::shared::trfStr("mjt.cli.err.mapping_write_failed", {output}));
    }

    // 可选：同一份 mapping（--from 形态）应用到 jar，产出重映射副本。
    if (args.has("remap-jar")) {
        mjt::cli::RemapJarRequest remap;
        remap.mappingPath = input;
        remap.format      = mjt::cli::resolveMappingFormatName(from).unwrap();
        remap.jarPath     = args.get("remap-jar");
        remap.outputPath  = args.get("remap-out");
        remap.policy      = args.get("remap-policy");
        if (remap.outputPath.empty()) {
            // 缺省输出 = 同目录 <stem>-remapped.jar。
            const auto jarPath     = std::filesystem::path(remap.jarPath);
            const auto defaultPath = jarPath.parent_path() /
                                     (jarPath.stem().string() + "-remapped.jar");
            remap.outputPath = defaultPath.string();
        }
        if (!requireInputFile(remap.jarPath))
            return usageFailure(mjt::shared::trfStr("mjt.cli.err.input_missing", {remap.jarPath}));
        auto remapped = mjt::cli::remapJarWithMapping(remap);
        if (remapped.is_err()) {
            const auto error = std::move(remapped).unwrap_err();
            return error.kind == mjt::cli::MappingError::Kind::UnknownFormat
                       ? usageFailure(error.message)
                       : runFailure(error.message);
        }
        mjt::out().puts(mjt::shared::trfStr("mjt.cli.mapping.remap_done",
                                       {remap.jarPath, remap.outputPath,
                                        std::move(remapped).unwrap() ? "1" : "0"}));
    }
    return mjt::cli::kExitOk;
}

/// `mjt jar fix -i <in> -o <out>`。
int runJarFix(const ca::opt::ParseResult& args)
{
    const std::string input  = args.get("input");
    const std::string output = args.get("output");
    if (!requireInputFile(input))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.input_missing", {input}));
    if (output.empty()) return usageFailure(mjt::shared::trfStr("mjt.cli.err.output_missing"));

    auto report = mjt::cli::fixJar(input, output);
    if (report.is_err())
        return runFailure(std::move(report).unwrap_err().message);
    mjt::cli::renderFixReport(std::move(report).unwrap(), output, mjt::out());
    return mjt::cli::kExitOk;
}

/// --timeout-ms / --max-memory-mib 的整数旗标解析：非数字或 ≤ 0 → 用法错误。
/// @return 成功时写入 @p value。
bool parsePositiveInteger(const std::string& raw, const char* flag, ca::i64& value)
{
    if (!raw.empty()) {
        std::size_t consumed = 0;
        long long   parsed   = 0;
        try {
            parsed = std::stoll(raw, &consumed);
        }
        catch (const std::exception&) {
            parsed = -1;
        }
        if (consumed == raw.size() && parsed > 0) {
            value = static_cast<ca::i64>(parsed);
            return true;
        }
    }
    usageFailure(mjt::shared::trfStr("mjt.cli.decompile.err.invalid_value",
                                     {flag, raw.empty() ? std::string{"(empty)"} : raw}));
    return false;
}

/// decompile 错误 → 退出码分支（UnknownBackend / InvalidArgument /
/// ClassNotFound = 用法 1；Runtime = 运行 2）。
int decompileFailure(mjt::cli::DecompileError error)
{
    const bool usage = error.kind != mjt::cli::DecompileError::Kind::Runtime;
    return usage ? usageFailure(std::move(error.message))
                 : runFailure(std::move(error.message));
}

/// 输出路径是否 .java（大小写不敏感；单类 -o 校验）。
bool isJavaOutputPath(const std::string& path)
{
    const std::filesystem::path parsed = std::filesystem::u8path(path);
    if (parsed.extension().string().size() != 5) return false;
    std::string lowered = parsed.extension().string();
    for (auto& character : lowered)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return lowered == ".java";
}

/// `mjt decompile -i <jar> -c <internalName> [-o <file.java>]
///  | -i <jar> -o <outdir|.zip> --all [--decompiler cfr|vineflower]
///  [--decompiler-jar <jar>] [--timeout-ms <n>] [--max-memory-mib <n>]`。
///
/// 模式：-c 在场 = 单类（短安全名方案，敌意类名免疫；-o 缺省打印 stdout）；
/// -o 在场 = 批量（--all 确认整 jar 成本后执行）。退出码：0 成功（批量预检
/// 跳过属部分成功，清单走 stderr）/ 1 用法 / 2 运行。
int runDecompile(const ca::opt::ParseResult& args)
{
    const std::string input     = args.get("input");
    const std::string className = args.get("class");
    const std::string output    = args.get("output");
    if (!requireInputFile(input))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.input_missing", {input}));

    mjt::cli::DecompileCliOptions options;
    if (args.has("decompiler")) {
        auto kind = mjt::cli::parseDecompilerBackendName(args.get("decompiler"));
        if (kind.is_err()) return decompileFailure(std::move(kind).unwrap_err());
        options.kind = kind.unwrap();
        //backendName 归一为稳定 ID（渲染与断言口径一致）。
        options.backendName = mjt::decompilerKindId(options.kind);
    }
    options.decompilerJar = args.get("decompiler-jar");
    if (args.has("timeout-ms") &&
        !parsePositiveInteger(args.get("timeout-ms"), "--timeout-ms", options.timeoutMs))
        return mjt::cli::kExitUsage;
    if (args.has("max-memory-mib") &&
        !parsePositiveInteger(args.get("max-memory-mib"), "--max-memory-mib", options.maxMemoryMiB))
        return mjt::cli::kExitUsage;

    //单类模式：-c 在场优先（短安全名方案，敌意类名免疫；-o 缺省 stdout）。
    if (args.has("class")) {
        if (!output.empty() && !isJavaOutputPath(output))
            return usageFailure(mjt::shared::trfStr("mjt.cli.decompile.err.single_output_java"));
        auto report = mjt::cli::decompileSingleClass(input, className, output, options);
        if (report.is_err()) return decompileFailure(std::move(report).unwrap_err());
        const auto value = std::move(report).unwrap();
        if (output.empty()) {
            //stdout 模式只落源码（重定向取文件语义，不混入摘要）。
            mjt::out().write(value.source);
        }
        else {
            mjt::cli::renderSingleDecompileSummary(value, mjt::out());
        }
        return mjt::cli::kExitOk;
    }

    //批量模式：-o 必填 + --all 显式确认整 jar 成本（缺席时给类数提示）。
    if (output.empty())
        return usageFailure(mjt::shared::trfStr("mjt.cli.decompile.err.need_mode"));
    if (!args.has("all")) {
        auto counted = mjt::cli::countJarClassEntries(input);
        const std::string count =
            counted.is_ok() ? std::to_string(counted.unwrap()) : std::string{"?"};
        return usageFailure(mjt::shared::trfStr("mjt.cli.decompile.err.need_all", {count}));
    }
    auto report = mjt::cli::decompileJarBatch(input, output, options);
    if (report.is_err()) return decompileFailure(std::move(report).unwrap_err());
    const auto value = std::move(report).unwrap();
    mjt::cli::renderBatchDecompileSummary(value, mjt::out());
    //敌意名预检跳过清单走 stderr（部分成功语义：退出码仍 0）。
    mjt::cli::renderSkippedEntries(value, mjt::err());
    return mjt::cli::kExitOk;
}

/// 命令树：根（--version）→ info / mapping / jar → fix / decompile。
ca::opt::Command buildCommandLine()
{
    ca::opt::Command root;
    root.name = "mjt";
    root.help = mjt::shared::trfStr("mjt.cli.root.help");
    ca::opt::Arg version;
    version.name       = "version";
    version.kind       = ca::opt::OptKind::Flag;
    version.help       = mjt::shared::trfStr("mjt.cli.root.version_help");
    root.args.push_back(version);

    ca::opt::Arg input;
    input.name     = "input";
    input.aliases  = {"-i"};
    input.kind     = ca::opt::OptKind::String;
    input.metavar  = "<path>";
    input.help     = mjt::shared::trfStr("mjt.cli.opt.input");
    input.required = true;

    ca::opt::Arg output;
    output.name     = "output";
    output.aliases  = {"-o"};
    output.kind     = ca::opt::OptKind::String;
    output.metavar  = "<path>";
    output.help     = mjt::shared::trfStr("mjt.cli.opt.output");
    output.required = true;

    ca::opt::Command info;
    info.name       = "info";
    info.help       = mjt::shared::trfStr("mjt.cli.info.help");
    ca::opt::Arg infoInput = input;
    info.args.push_back(infoInput);
    ca::opt::Arg json;
    json.name = "json";
    json.kind = ca::opt::OptKind::Flag;
    json.help = mjt::shared::trfStr("mjt.cli.info.json_help");
    info.args.push_back(json);
    root.subcommands.push_back(std::move(info));

    ca::opt::Command mapping;
    mapping.name = "mapping";
    mapping.help = mjt::shared::trfStr("mjt.cli.mapping.help");
    ca::opt::Arg mappingInput = input;
    mapping.args.push_back(mappingInput);
    ca::opt::Arg mappingOutput = output;
    mapping.args.push_back(mappingOutput);
    ca::opt::Arg from;
    from.name     = "from";
    from.kind     = ca::opt::OptKind::String;
    from.metavar  = "<fmt>";
    from.help     = mjt::shared::trfStr("mjt.cli.mapping.from_help");
    from.required = true;
    mapping.args.push_back(from);
    ca::opt::Arg to;
    to.name     = "to";
    to.kind     = ca::opt::OptKind::String;
    to.metavar  = "<fmt>";
    to.help     = mjt::shared::trfStr("mjt.cli.mapping.to_help");
    to.required = true;
    mapping.args.push_back(to);
    ca::opt::Arg remapJar;
    remapJar.name    = "remap-jar";
    remapJar.kind    = ca::opt::OptKind::String;
    remapJar.metavar = "<jar>";
    remapJar.help    = mjt::shared::trfStr("mjt.cli.mapping.remap_jar_help");
    mapping.args.push_back(remapJar);
    ca::opt::Arg remapOut;
    remapOut.name    = "remap-out";
    remapOut.kind    = ca::opt::OptKind::String;
    remapOut.metavar = "<jar>";
    remapOut.help    = mjt::shared::trfStr("mjt.cli.mapping.remap_out_help");
    mapping.args.push_back(remapOut);
    ca::opt::Arg remapPolicy;
    remapPolicy.name        = "remap-policy";
    remapPolicy.kind        = ca::opt::OptKind::String;
    remapPolicy.metavar     = "<policy>";
    remapPolicy.help        = mjt::shared::trfStr("mjt.cli.mapping.remap_policy_help");
    remapPolicy.default_value = "bytecode-only";
    mapping.args.push_back(remapPolicy);
    root.subcommands.push_back(std::move(mapping));

    ca::opt::Command jar;
    jar.name = "jar";
    jar.help = mjt::shared::trfStr("mjt.cli.jar.help");
    ca::opt::Command fix;
    fix.name = "fix";
    fix.help = mjt::shared::trfStr("mjt.cli.fix.help");
    ca::opt::Arg fixInput = input;
    fix.args.push_back(fixInput);
    ca::opt::Arg fixOutput = output;
    fix.args.push_back(fixOutput);
    jar.subcommands.push_back(std::move(fix));
    root.subcommands.push_back(std::move(jar));

    ca::opt::Command decompile;
    decompile.name = "decompile";
    decompile.help = mjt::shared::trfStr("mjt.cli.decompile.help");
    ca::opt::Arg decompileInput = input;
    decompileInput.required     = true;
    decompile.args.push_back(decompileInput);
    ca::opt::Arg classArg;
    classArg.name    = "class";
    classArg.aliases = {"-c"};
    classArg.kind    = ca::opt::OptKind::String;
    classArg.metavar = "<internalName>";
    classArg.help    = mjt::shared::trfStr("mjt.cli.decompile.class_help");
    decompile.args.push_back(classArg);
    ca::opt::Arg decompileOutput = output;
    decompileOutput.required     = false;
    decompile.args.push_back(decompileOutput);
    ca::opt::Arg all;
    all.name = "all";
    all.kind = ca::opt::OptKind::Flag;
    all.help = mjt::shared::trfStr("mjt.cli.decompile.all_help");
    decompile.args.push_back(all);
    ca::opt::Arg decompiler;
    decompiler.name    = "decompiler";
    decompiler.kind    = ca::opt::OptKind::String;
    decompiler.metavar = "<name>";
    decompiler.help    = mjt::shared::trfStr("mjt.cli.decompile.backend_help");
    decompiler.default_value = "cfr";
    decompile.args.push_back(decompiler);
    ca::opt::Arg decompilerJar;
    decompilerJar.name    = "decompiler-jar";
    decompilerJar.kind    = ca::opt::OptKind::String;
    decompilerJar.metavar = "<jar>";
    decompilerJar.help    = mjt::shared::trfStr("mjt.cli.decompile.jar_help");
    decompile.args.push_back(decompilerJar);
    ca::opt::Arg timeoutMs;
    timeoutMs.name    = "timeout-ms";
    timeoutMs.kind    = ca::opt::OptKind::String;
    timeoutMs.metavar = "<n>";
    timeoutMs.help    = mjt::shared::trfStr("mjt.cli.decompile.timeout_help");
    decompile.args.push_back(timeoutMs);
    ca::opt::Arg maxMemoryMiB;
    maxMemoryMiB.name    = "max-memory-mib";
    maxMemoryMiB.kind    = ca::opt::OptKind::String;
    maxMemoryMiB.metavar = "<n>";
    maxMemoryMiB.help    = mjt::shared::trfStr("mjt.cli.decompile.memory_help");
    decompile.args.push_back(maxMemoryMiB);
    root.subcommands.push_back(std::move(decompile));

    return root;
}

}   // namespace

int main(int argc, char* argv[])
{
    initI18n();
    mjbase::log::initDefaultLogger();

    ca::opt::Parser parser{buildCommandLine()};
    const auto parsed = parser.parse(argc, argv);
    if (parsed.is_err()) {
        const auto err = std::move(parsed).unwrap_err();
        if (err.category == ca::opt::ParseErrorCategory::HelpRequested) {
            std::cout << err.message;
            return mjt::cli::kExitOk;
        }
        return usageFailure(err.message);
    }
    const auto& args = parsed.unwrap();

    if (args.has("version")) {
        mjt::out().puts(std::string("mjt ") + MJT_VERSION);
        return mjt::cli::kExitOk;
    }

    const auto& path = args.subcommand_path();
    if (path.empty()) {
        std::cerr << ca::opt::help_text(parser.root());
        return mjt::cli::kExitUsage;
    }
    if (path.size() == 1 && path.front() == "info") return runInfo(args);
    if (path.size() == 1 && path.front() == "mapping") return runMapping(args);
    if (path.size() == 1 && path.front() == "decompile") return runDecompile(args);
    if (path.size() == 2 && path.front() == "jar" && path[1] == "fix") return runJarFix(args);

    std::cerr << ca::opt::help_text(parser.root());
    return mjt::cli::kExitUsage;
}
