/// @file        main.cpp
/// @brief       `mjt` CLI 入口：ca::opt 命令树（info / mapping / jar fix /
///              decompile / strings / xref / classes / disasm / subs / hexdump）+ 全局 --version/--help + 退出码映射。
///
/// 退出码约定（README「CLI usage」同文）：0 成功（含 --help）/ 1 用法错误
/// （参数缺失、文件不存在、格式名不识别、类选择器无命中、旗标值非法）/
/// 2 运行失败（打开/解析/修复/导出/反编译/抽取/扫描/反汇编失败）。--help 经
/// ca::opt 的 HelpRequested 通道返回 0。
///
/// 文案：i18n 表 = mjt_shared（共享层错误文案）+ 本 CLI（mjt.cli.*），语言
/// 由 MJT_LANG 选择（未设置或未注册时落 en_US）。
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>

#include "libca/opt/opt.hpp"
#include "libca/str/string_util.hpp"

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

/// 输入与输出是否同一文件（weakly_canonical 归一比较，fixJar 核心兜底同款
/// 口径）。归一失败按「不同」放行，交由后续写路径报错。
bool isSamePath(const std::string& left, const std::string& right)
{
    std::error_code ec;
    const auto leftCanonical  = std::filesystem::weakly_canonical(left, ec);
    const auto rightCanonical = std::filesystem::weakly_canonical(right, ec);
    return !ec && leftCanonical == rightCanonical;
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
    // 同路径防自毁（usage 校验归 main）：mapping 文本走裸 ofstream 落盘，
    // 原地覆盖即数据丢失。
    if (isSamePath(input, output))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.mapping_same_path", {output}));

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
        if (isSamePath(remap.jarPath, remap.outputPath))
            return usageFailure(
                mjt::shared::trfStr("mjt.cli.err.mapping_same_path", {remap.outputPath}));
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
    // 同路径防自毁（usage 校验归 main，fixJar 核心内另有兜底）。
    if (isSamePath(input, output))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.fix_same_path", {output}));

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
    usageFailure(mjt::shared::trfStr("mjt.cli.err.invalid_value",
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
    const std::string lowered = ca::str::StringUtil::to_lower_case(parsed.extension().string());
    return lowered == ".java";
}

/// `mjt decompile -i <jar> -c <internalName> [-o <file.java>]
///  | -i <jar> -o <outdir|.zip> --all [--decompiler cfr|vineflower|jcdc]
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
    //共享层字段是 i32：超上界的值经 static_cast 回绕成小值/负值（脚本误传
    //字节数等），按用法错误拒绝而不是溢出后落运行失败通道。
    if (options.maxMemoryMiB > std::numeric_limits<ca::i32>::max())
        return usageFailure(mjt::shared::trfStr(
            "mjt.cli.err.invalid_value", {"--max-memory-mib", args.get("max-memory-mib")}));

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

/// `mjt strings -i <jar> [--class <internalName>] [--min-len N] [--json] [-o <file>]`。
int runStrings(const ca::opt::ParseResult& args)
{
    const std::string input     = args.get("input");
    const std::string className = args.get("class");
    const std::string output    = args.get("output");
    if (!requireInputFile(input))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.input_missing", {input}));

    ca::i64 minimumLen = 1;
    if (args.has("min-len") &&
        !parsePositiveInteger(args.get("min-len"), "--min-len", minimumLen))
        return mjt::cli::kExitUsage;

    auto report = mjt::cli::collectStrings(input, className, minimumLen);
    if (report.is_err()) {
        const auto error = std::move(report).unwrap_err();
        const bool usage = error.kind != mjt::cli::StringsError::Kind::Runtime;
        return usage ? usageFailure(error.message) : runFailure(error.message);
    }
    const auto value = std::move(report).unwrap();

    // -o 重定向主输出（文本/JSON 同形落盘），stdout 只留摘要行。
    if (!output.empty()) {
        std::FILE* file = std::fopen(output.c_str(), "wb");
        if (file == nullptr)
            return runFailure(mjt::shared::trfStr("mjt.cli.strings.err.write_failed", {output}));
        {
            mjt::OutputSink sink(file);
            if (args.has("json")) mjt::cli::renderStringsJson(value, sink);
            else                  mjt::cli::renderStringsText(value, sink);
        }
        std::fclose(file);
        mjt::out().puts(mjt::shared::trfStr("mjt.cli.strings.written", {output}));
        return mjt::cli::kExitOk;
    }
    if (args.has("json")) mjt::cli::renderStringsJson(value, mjt::out());
    else                  mjt::cli::renderStringsText(value, mjt::out());
    return mjt::cli::kExitOk;
}

/// `mjt xref -i <jar> -c <internalName> [-m <member>] [--direction to|from|both]
///  [--limit N] [--json]`。
int runXref(const ca::opt::ParseResult& args)
{
    const std::string input     = args.get("input");
    const std::string className = args.get("class");
    const std::string member    = args.get("member");
    if (!requireInputFile(input))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.input_missing", {input}));

    ca::i64 limit = 200;
    if (args.has("limit") && !parsePositiveInteger(args.get("limit"), "--limit", limit))
        return mjt::cli::kExitUsage;

    auto report = mjt::cli::collectXref(input, className, member, args.get("direction"), limit);
    if (report.is_err()) {
        const auto error = std::move(report).unwrap_err();
        const bool usage = error.kind != mjt::cli::XrefError::Kind::Runtime;
        return usage ? usageFailure(error.message) : runFailure(error.message);
    }
    const auto value = std::move(report).unwrap();
    if (args.has("json")) mjt::cli::renderXrefJson(value, mjt::out());
    else                  mjt::cli::renderXrefText(value, mjt::out());
    return mjt::cli::kExitOk;
}

/// `mjt classes -i <jar> [--kind <k>] [--origin <o>] [--limit N] [--json]
///  [-o <file>]`。
int runClasses(const ca::opt::ParseResult& args)
{
    const std::string input  = args.get("input");
    const std::string output = args.get("output");
    if (!requireInputFile(input))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.input_missing", {input}));

    ca::i64 limit = 10000;
    if (args.has("limit") && !parsePositiveInteger(args.get("limit"), "--limit", limit))
        return mjt::cli::kExitUsage;

    // --limit 防呆只作用于 stdout 列表；-o 落盘完整清单（README 同文）——以
    // 无界上限调入，limit 字段归一为清单行数（truncated 恒 false）。
    const bool toFile = !output.empty();
    auto report =
        mjt::cli::collectClasses(input, args.get("kind"), args.get("origin"),
                                 toFile ? std::numeric_limits<ca::i64>::max() : limit);
    if (report.is_err()) {
        const auto error = std::move(report).unwrap_err();
        const bool usage = error.kind != mjt::cli::ClassesError::Kind::Runtime;
        return usage ? usageFailure(error.message) : runFailure(error.message);
    }
    auto value = std::move(report).unwrap();
    if (toFile) value.limit = static_cast<ca::i64>(value.classes.size());

    // -o 重定向主输出（文本/JSON 同形落盘），stdout 只留摘要行。
    if (toFile) {
        std::FILE* file = std::fopen(output.c_str(), "wb");
        if (file == nullptr)
            return runFailure(mjt::shared::trfStr("mjt.cli.classes.err.write_failed", {output}));
        {
            mjt::OutputSink sink(file);
            if (args.has("json")) mjt::cli::renderClassesJson(value, sink);
            else                  mjt::cli::renderClassesText(value, sink);
        }
        std::fclose(file);
        mjt::out().puts(mjt::shared::trfStr("mjt.cli.classes.written", {output}));
        return mjt::cli::kExitOk;
    }
    if (args.has("json")) mjt::cli::renderClassesJson(value, mjt::out());
    else                  mjt::cli::renderClassesText(value, mjt::out());
    return mjt::cli::kExitOk;
}

/// disasm / subs 错误 → 退出码分支（ClassNotFound = 用法 1；Runtime = 运行 2）。
template <typename Error>
int classSelectorFailure(Error error)
{
    const bool usage = error.kind != Error::Kind::Runtime;
    return usage ? usageFailure(std::move(error.message)) : runFailure(std::move(error.message));
}

/// `mjt disasm -i <jar> -c <internalName> [-o <file>]`。
int runDisasm(const ca::opt::ParseResult& args)
{
    const std::string input     = args.get("input");
    const std::string className = args.get("class");
    const std::string output    = args.get("output");
    if (!requireInputFile(input))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.input_missing", {input}));

    auto report = mjt::cli::collectDisasm(input, className);
    if (report.is_err())
        return classSelectorFailure(std::move(report).unwrap_err());
    const auto value = std::move(report).unwrap();

    // -o 落盘 jasm 全文，stdout 只留 written 行（strings -o 同款分工）。
    if (!output.empty()) {
        std::FILE* file = std::fopen(output.c_str(), "wb");
        if (file == nullptr)
            return runFailure(mjt::shared::trfStr("mjt.cli.disasm.err.write_failed", {output}));
        {
            mjt::OutputSink sink(file);
            mjt::cli::renderDisasmText(value, sink);
        }
        std::fclose(file);
        mjt::out().puts(mjt::shared::trfStr("mjt.cli.disasm.written", {output}));
        return mjt::cli::kExitOk;
    }
    mjt::cli::renderDisasmText(value, mjt::out());
    return mjt::cli::kExitOk;
}

/// `mjt subs -i <jar> -c <internalName> [--json]`。
int runSubs(const ca::opt::ParseResult& args)
{
    const std::string input     = args.get("input");
    const std::string className = args.get("class");
    if (!requireInputFile(input))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.input_missing", {input}));

    auto report = mjt::cli::collectSubs(input, className);
    if (report.is_err())
        return classSelectorFailure(std::move(report).unwrap_err());
    const auto value = std::move(report).unwrap();
    if (args.has("json")) mjt::cli::renderSubsJson(value, mjt::out());
    else                  mjt::cli::renderSubsText(value, mjt::out());
    return mjt::cli::kExitOk;
}

/// `mjt hexdump -i <jar> <class> [--offset N] [--length N] [--width N] [--no-ascii]`。
///
/// <class> 为位置参数（URI / 内名精确命中优先，无精确命中退子串候选）。旗标值非法 / 类选择器无命中 = 用法 1；jar 打开 / 类字节读取失败 = 运行 2。
int runHexdump(const ca::opt::ParseResult& args)
{
    const std::string input = args.get("input");
    if (!requireInputFile(input))
        return usageFailure(mjt::shared::trfStr("mjt.cli.err.input_missing", {input}));

    mjt::cli::HexdumpOptions options;
    options.offset    = static_cast<ca::i64>(args.get_int("offset", 0));
    options.length    = static_cast<ca::i64>(args.get_int("length", -1));
    options.width     = static_cast<ca::i64>(args.get_int("width", 16));
    options.showAscii = !args.has("no-ascii");

    const auto& positionals = args.positionals();
    const std::string className = positionals.empty() ? std::string{} : positionals.front();

    auto report = mjt::cli::collectHexdump(input, className, options);
    if (report.is_err()) {
        const auto error = std::move(report).unwrap_err();
        const bool usage = error.kind != mjt::cli::HexdumpError::Kind::Runtime;
        return usage ? usageFailure(error.message) : runFailure(error.message);
    }
    mjt::cli::renderHexdumpText(std::move(report).unwrap(), options, mjt::out());
    return mjt::cli::kExitOk;
}

/// 命令树：根（--version）→ info / mapping / jar → fix / decompile / strings /
/// xref / classes / disasm / subs / hexdump。
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

    // strings：常量池字符串抽取（-o 可选；--class 过滤单类，--min-len 过滤短串）。
    ca::opt::Command strings;
    strings.name = "strings";
    strings.help = mjt::shared::trfStr("mjt.cli.strings.help");
    ca::opt::Arg stringsInput = input;
    strings.args.push_back(stringsInput);
    ca::opt::Arg stringsClass = classArg;
    stringsClass.name = "class";
    stringsClass.help = mjt::shared::trfStr("mjt.cli.strings.class_help");
    strings.args.push_back(stringsClass);
    ca::opt::Arg minLen;
    minLen.name          = "min-len";
    minLen.kind          = ca::opt::OptKind::String;
    minLen.metavar       = "<n>";
    minLen.help          = mjt::shared::trfStr("mjt.cli.strings.min_len_help");
    minLen.default_value = "1";
    strings.args.push_back(minLen);
    ca::opt::Arg stringsJson;
    stringsJson.name = "json";
    stringsJson.kind = ca::opt::OptKind::Flag;
    stringsJson.help = mjt::shared::trfStr("mjt.cli.strings.json_help");
    strings.args.push_back(stringsJson);
    ca::opt::Arg stringsOutput = output;
    stringsOutput.required     = false;
    stringsOutput.help         = mjt::shared::trfStr("mjt.cli.strings.output_help");
    strings.args.push_back(stringsOutput);
    root.subcommands.push_back(std::move(strings));

    // xref：类/成员反查（--direction 缺省 to；--limit 缺省 200，输出必有界）。
    ca::opt::Command xref;
    xref.name = "xref";
    xref.help = mjt::shared::trfStr("mjt.cli.xref.help");
    ca::opt::Arg xrefInput = input;
    xref.args.push_back(xrefInput);
    ca::opt::Arg xrefClass = classArg;
    xrefClass.name    = "class";
    xrefClass.help    = mjt::shared::trfStr("mjt.cli.xref.class_help");
    xrefClass.required = true;
    xref.args.push_back(xrefClass);
    ca::opt::Arg member;
    member.name    = "member";
    member.aliases = {"-m"};
    member.kind    = ca::opt::OptKind::String;
    member.metavar = "<member>";
    member.help    = mjt::shared::trfStr("mjt.cli.xref.member_help");
    xref.args.push_back(member);
    ca::opt::Arg direction;
    direction.name          = "direction";
    direction.kind          = ca::opt::OptKind::String;
    direction.metavar       = "<to|from|both>";
    direction.help          = mjt::shared::trfStr("mjt.cli.xref.direction_help");
    direction.default_value = "to";
    xref.args.push_back(direction);
    ca::opt::Arg limit;
    limit.name          = "limit";
    limit.kind          = ca::opt::OptKind::String;
    limit.metavar       = "<n>";
    limit.help          = mjt::shared::trfStr("mjt.cli.xref.limit_help");
    limit.default_value = "200";
    xref.args.push_back(limit);
    ca::opt::Arg xrefJson;
    xrefJson.name = "json";
    xrefJson.kind = ca::opt::OptKind::Flag;
    xrefJson.help = mjt::shared::trfStr("mjt.cli.xref.json_help");
    xref.args.push_back(xrefJson);
    root.subcommands.push_back(std::move(xref));

    // classes：类清单（census 投影；--kind/--origin 过滤，--limit 防呆上限，
    // -o 可选落盘）。
    ca::opt::Command classes;
    classes.name = "classes";
    classes.help = mjt::shared::trfStr("mjt.cli.classes.help");
    ca::opt::Arg classesInput = input;
    classes.args.push_back(classesInput);
    ca::opt::Arg kind;
    kind.name    = "kind";
    kind.kind    = ca::opt::OptKind::String;
    kind.metavar = "<class|interface|enum|record|annotation|module>";
    kind.help    = mjt::shared::trfStr("mjt.cli.classes.kind_help");
    classes.args.push_back(kind);
    ca::opt::Arg origin;
    origin.name    = "origin";
    origin.kind    = ca::opt::OptKind::String;
    origin.metavar = "<jdk|known|unknown>";
    origin.help    = mjt::shared::trfStr("mjt.cli.classes.origin_help");
    classes.args.push_back(origin);
    ca::opt::Arg classesLimit;
    classesLimit.name          = "limit";
    classesLimit.kind          = ca::opt::OptKind::String;
    classesLimit.metavar       = "<n>";
    classesLimit.help          = mjt::shared::trfStr("mjt.cli.classes.limit_help");
    classesLimit.default_value = "10000";
    classes.args.push_back(classesLimit);
    ca::opt::Arg classesJson;
    classesJson.name = "json";
    classesJson.kind = ca::opt::OptKind::Flag;
    classesJson.help = mjt::shared::trfStr("mjt.cli.classes.json_help");
    classes.args.push_back(classesJson);
    ca::opt::Arg classesOutput = output;
    classesOutput.required     = false;
    classesOutput.help         = mjt::shared::trfStr("mjt.cli.classes.output_help");
    classes.args.push_back(classesOutput);
    root.subcommands.push_back(std::move(classes));

    // disasm：单类 jasm 文本（-o 可选；类不存在退出码 1）。
    ca::opt::Command disasm;
    disasm.name = "disasm";
    disasm.help = mjt::shared::trfStr("mjt.cli.disasm.help");
    ca::opt::Arg disasmInput = input;
    disasm.args.push_back(disasmInput);
    ca::opt::Arg disasmClass = classArg;
    disasmClass.name    = "class";
    disasmClass.help    = mjt::shared::trfStr("mjt.cli.disasm.class_help");
    disasmClass.required = true;
    disasm.args.push_back(disasmClass);
    ca::opt::Arg disasmOutput = output;
    disasmOutput.required     = false;
    disasmOutput.help         = mjt::shared::trfStr("mjt.cli.disasm.output_help");
    disasm.args.push_back(disasmOutput);
    root.subcommands.push_back(std::move(disasm));

    // subs：单类成员清单（字段/方法名、描述符、access 旗标、native 标记）。
    ca::opt::Command subs;
    subs.name = "subs";
    subs.help = mjt::shared::trfStr("mjt.cli.subs.help");
    ca::opt::Arg subsInput = input;
    subs.args.push_back(subsInput);
    ca::opt::Arg subsClass = classArg;
    subsClass.name    = "class";
    subsClass.help    = mjt::shared::trfStr("mjt.cli.subs.class_help");
    subsClass.required = true;
    subs.args.push_back(subsClass);
    ca::opt::Arg subsJson;
    subsJson.name = "json";
    subsJson.kind = ca::opt::OptKind::Flag;
    subsJson.help = mjt::shared::trfStr("mjt.cli.subs.json_help");
    subs.args.push_back(subsJson);
    root.subcommands.push_back(std::move(subs));

    // hexdump：单类原始字节十六进制转储（<class> 位置参数；--offset / --length / --width 窗口，--no-ascii 隐藏 ASCII 列）。旗标文案复用共享表 mjt.cmd.hexdump.*（与 MCP 共用同一份文案，避免两处漂移）。
    ca::opt::Command hexdump;
    hexdump.name = "hexdump";
    hexdump.help = mjt::shared::trfStr("mjt.cli.hexdump.help");
    ca::opt::Arg hexdumpInput = input;
    hexdump.args.push_back(hexdumpInput);
    ca::opt::Arg hexdumpClass;
    hexdumpClass.name    = "class";
    hexdumpClass.kind    = ca::opt::OptKind::Positional;
    hexdumpClass.metavar = "<class>";
    hexdumpClass.help    = mjt::shared::trfStr("mjt.cmd.hexdump.param.class.desc");
    hexdump.args.push_back(hexdumpClass);
    ca::opt::Arg offset;
    offset.name          = "offset";
    offset.kind          = ca::opt::OptKind::Int;
    offset.metavar       = "<n>";
    offset.help          = mjt::shared::trfStr("mjt.cmd.hexdump.param.offset.desc");
    offset.default_value = "0";
    hexdump.args.push_back(offset);
    ca::opt::Arg length;
    length.name          = "length";
    length.kind          = ca::opt::OptKind::Int;
    length.metavar       = "<n>";
    length.help          = mjt::shared::trfStr("mjt.cmd.hexdump.param.length.desc");
    length.default_value = "-1";
    hexdump.args.push_back(length);
    ca::opt::Arg width;
    width.name          = "width";
    width.kind          = ca::opt::OptKind::Int;
    width.metavar       = "<n>";
    width.help          = mjt::shared::trfStr("mjt.cmd.hexdump.param.width.desc");
    width.default_value = "16";
    hexdump.args.push_back(width);
    ca::opt::Arg noAscii;
    noAscii.name = "no-ascii";
    noAscii.kind = ca::opt::OptKind::Flag;
    noAscii.help = mjt::shared::trfStr("mjt.cmd.hexdump.param.no_ascii.desc");
    hexdump.args.push_back(noAscii);
    root.subcommands.push_back(std::move(hexdump));

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
    if (path.size() == 1 && path.front() == "strings") return runStrings(args);
    if (path.size() == 1 && path.front() == "xref") return runXref(args);
    if (path.size() == 1 && path.front() == "classes") return runClasses(args);
    if (path.size() == 1 && path.front() == "disasm") return runDisasm(args);
    if (path.size() == 1 && path.front() == "subs") return runSubs(args);
    if (path.size() == 1 && path.front() == "hexdump") return runHexdump(args);
    if (path.size() == 2 && path.front() == "jar" && path[1] == "fix") return runJarFix(args);

    std::cerr << ca::opt::help_text(parser.root());
    return mjt::cli::kExitUsage;
}
