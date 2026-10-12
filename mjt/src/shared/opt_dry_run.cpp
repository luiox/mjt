#include "shared/opt_dry_run.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "libca/i18n/i18n.hpp"

#include "mbase/i18n.hpp"
#include "mbase/jar/jar_util.hpp"
#include "mbase/jasm/jvm_jasm_service.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/model/resource_helper.hpp"
#include "mbase/model/resource_model.hpp"
#include "mbase/spi/asm_pass.hpp"
#include "mbase/spi/pass_manager.hpp"

#include "shared/json_util.hpp"

using namespace mjt::base;

namespace mjt::opt_dry_run {

namespace {

//── 输入识别（与 mbase cli_runner 同口径的只读镜像）──────────────────

enum class InputKind
{
    Jar,
    Class,
    Unsupported
};

InputKind detectInputKind(const std::string& path)
{
    if (JarUtil::isJarFile(path)) return InputKind::Jar;
    if (JarUtil::isClassFile(path)) return InputKind::Class;
    return InputKind::Unsupported;
}

//uri 形如 "org/example/Demo.class"，targetClass 形如 "org/example/Demo"。
//子串匹配（去 .class 后缀更稳），空 target 表示不过滤（返回 true）。
bool classUriMatches(const std::string& uri, const std::string& targetClass)
{
    if (targetClass.empty()) return true;
    std::string       base   = uri;
    const std::string suffix = ".class";
    if (base.size() > suffix.size() &&
        base.compare(base.size() - suffix.size(), suffix.size(), suffix) == 0) {
        base = base.substr(0, base.size() - suffix.size());
    }
    return base.find(targetClass) != std::string::npos;
}

const char* failureKindName(CliFailureKind kind)
{
    switch (kind) {
    case CliFailureKind::None:             return "None";
    case CliFailureKind::InvalidArguments: return "InvalidArguments";
    case CliFailureKind::UnsupportedInput: return "UnsupportedInput";
    case CliFailureKind::Read:             return "Read";
    case CliFailureKind::NoClass:          return "NoClass";
    case CliFailureKind::NoMatch:          return "NoMatch";
    case CliFailureKind::Processing:       return "Processing";
    case CliFailureKind::Write:            return "Write";
    case CliFailureKind::Verification:     return "Verification";
    }
    return "Unknown";
}

const char* outputModeName(OutputMode mode)
{
    switch (mode) {
    case OutputMode::Json:     return "json";
    case OutputMode::JasmDiff: return "diff";
    case OutputMode::Counts:   break;
    }
    return "counts";
}

//── 行级 diff（前后缀修剪 + 中心 LCS，超限退化为整段替换）────────────

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t              start = 0;
    while (true) {
        const auto nl = text.find('\n', start);
        if (nl == std::string::npos) {
            if (start < text.size()) lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

struct DiffLine
{
    char        kind;   //' ' / '-' / '+'
    std::string text;
};

//中心区 LCS（suffix 表前向回溯）；单元数超限（防御超大方法体的 O(n·m) 表）
//时退化为先删后插的整段替换——逐行语义仍正确，规模有界。
std::vector<DiffLine> midDiff(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    std::vector<DiffLine> out;
    const std::size_t     n = a.size();
    const std::size_t     m = b.size();
    if (n == 0 && m == 0) return out;

    if (n == 0 || m == 0 || (n + 1) * (m + 1) > 4000000) {
        for (const auto& line : a) out.push_back({'-', line});
        for (const auto& line : b) out.push_back({'+', line});
        return out;
    }

    //lcs[i][j] = LCS(a[i..], b[j..])，行主序 (n+1)×(m+1) 表。
    std::vector<ca::u32> lcs((n + 1) * (m + 1), 0);
    auto at = [&](std::size_t i, std::size_t j) { return lcs[i * (m + 1) + j]; };
    for (std::size_t i = n; i-- > 0;) {
        for (std::size_t j = m; j-- > 0;) {
            lcs[i * (m + 1) + j] =
                a[i] == b[j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
        }
    }
    std::size_t i = 0;
    std::size_t j = 0;
    while (i < n && j < m) {
        if (a[i] == b[j]) {
            out.push_back({' ', a[i]});
            ++i;
            ++j;
        }
        else if (at(i + 1, j) >= at(i, j + 1)) {
            out.push_back({'-', a[i]});
            ++i;
        }
        else {
            out.push_back({'+', b[j]});
            ++j;
        }
    }
    for (; i < n; ++i) out.push_back({'-', a[i]});
    for (; j < m; ++j) out.push_back({'+', b[j]});
    return out;
}

std::vector<DiffLine> lineDiff(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    std::size_t pre = 0;
    while (pre < a.size() && pre < b.size() && a[pre] == b[pre]) ++pre;
    std::size_t suf = 0;
    while (suf < a.size() - pre && suf < b.size() - pre &&
           a[a.size() - 1 - suf] == b[b.size() - 1 - suf]) {
        ++suf;
    }

    std::vector<DiffLine> out;
    for (std::size_t k = 0; k < pre; ++k) out.push_back({' ', a[k]});
    const std::vector<std::string> midA(a.begin() + static_cast<std::ptrdiff_t>(pre),
                                        a.end() - static_cast<std::ptrdiff_t>(suf));
    const std::vector<std::string> midB(b.begin() + static_cast<std::ptrdiff_t>(pre),
                                        b.end() - static_cast<std::ptrdiff_t>(suf));
    for (auto& line : midDiff(midA, midB)) out.push_back(std::move(line));
    for (std::size_t k = a.size() - suf; k < a.size(); ++k) out.push_back({' ', a[k]});
    return out;
}

struct DiffHunk
{
    std::size_t           oldStart = 0;   //1-based；纯插入段取前一行号（顶端插入为 0）
    std::size_t           oldCount = 0;
    std::size_t           newStart = 0;
    std::size_t           newCount = 0;
    std::vector<DiffLine> lines;
};

//按 unified-diff 语义聚合 ops：非相等行成岛，两侧各扩 context 行相等上下文，
//相邻窗口重叠则合并。行号采用 GNU/git 约定（空段起点 = 前一行号，可为 0）。
std::vector<DiffHunk> buildHunks(const std::vector<DiffLine>& ops, std::size_t context)
{
    //收集非相等 op 的岛（op 索引区间）。
    std::vector<std::pair<std::size_t, std::size_t>> islands;
    for (std::size_t k = 0; k < ops.size();) {
        if (ops[k].kind == ' ') {
            ++k;
            continue;
        }
        const std::size_t begin = k;
        while (k < ops.size() && ops[k].kind != ' ') ++k;
        islands.push_back({begin, k});
    }
    if (islands.empty()) return {};

    //岛扩上下文后合并重叠窗口。
    std::vector<std::pair<std::size_t, std::size_t>> windows;
    for (const auto& [begin, end] : islands) {
        const std::size_t wBegin = begin > context ? begin - context : 0;
        const std::size_t wEnd   = std::min(ops.size(), end + context);
        if (!windows.empty() && wBegin <= windows.back().second) {
            windows.back().second = std::max(windows.back().second, wEnd);
        }
        else {
            windows.push_back({wBegin, wEnd});
        }
    }

    std::vector<DiffHunk> hunks;
    for (const auto& [begin, end] : windows) {
        //窗口起点处旧/新两侧已消费的行数（ops 与行号一一对应，线性重扫）。
        std::size_t preOld = 0;
        std::size_t preNew = 0;
        for (std::size_t k = 0; k < begin; ++k) {
            if (ops[k].kind != '+') ++preOld;
            if (ops[k].kind != '-') ++preNew;
        }
        bool hasOld = false;
        bool hasNew = false;
        DiffHunk hunk;
        for (std::size_t k = begin; k < end; ++k) {
            hunk.lines.push_back(ops[k]);
            if (ops[k].kind != '+') {
                ++hunk.oldCount;
                hasOld = true;
            }
            if (ops[k].kind != '-') {
                ++hunk.newCount;
                hasNew = true;
            }
        }
        hunk.oldStart = hasOld ? preOld + 1 : preOld;
        hunk.newStart = hasNew ? preNew + 1 : preNew;
        hunks.push_back(std::move(hunk));
    }
    return hunks;
}

std::string hunkHeader(const DiffHunk& hunk)
{
    std::string header = "@@ -";
    header += std::to_string(hunk.oldStart);
    if (hunk.oldCount != 1) {
        header += ',';
        header += std::to_string(hunk.oldCount);
    }
    header += " +";
    header += std::to_string(hunk.newStart);
    if (hunk.newCount != 1) {
        header += ',';
        header += std::to_string(hunk.newCount);
    }
    header += " @@";
    return header;
}

//── 渲染辅助 ────────────────────────────────────────────────

///反汇编为规范化 jasm 文本（disasm 命令同一渲染层）。
///ok=false 时 message 带首条诊断，供渲染失败行使用。
std::string jasmText(const std::vector<ca::u8>& bytes, jasm::JvmJasmService& service, bool& ok,
                     std::string& message)
{
    ok       = false;
    auto result = service.disassemble(bytes);
    for (const auto& diagnostic : result.diagnostics) {
        if (message.empty()) {
            message = diagnostic.message.to_utf8_string().to_std_string();
        }
    }
    if (!result.isOk()) return {};
    ok = true;
    return result.value->source.to_utf8_string().to_std_string();
}

std::string durationField(double ms)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.3f", ms);
    return buf;
}

void renderCounts(const CliRunReport&             report,
                  const PassRunStats&             stats,
                  const std::vector<std::string>& changedClasses,
                  const DryRunRequest&            request)
{
    cli::out().printf("%s\n",
                      ca::i18n::trf("mjt.cmd.opt.out.dryrun_header",
                                    {request.input.c_str(), request.runMode.c_str()})
                          .c_str());
    for (const auto& slot : stats.slots) {
        const std::string name     = slot.name.empty() ? std::string("(unnamed)") : slot.name;
        const std::string duration = durationField(slot.durationMs);
        cli::out().printf("%s\n",
                          ca::i18n::trf("mjt.cmd.opt.out.dryrun_pass_line",
                                        {name.c_str(),
                                         slot.scope != nullptr ? slot.scope : "?",
                                         std::to_string(slot.processed).c_str(),
                                         std::to_string(slot.changed).c_str(),
                                         std::to_string(slot.failed).c_str(),
                                         duration.c_str()})
                              .c_str());
    }
    cli::out().printf("%s\n",
                      ca::i18n::trf("mjt.cmd.opt.out.dryrun_classes_line",
                                    {std::to_string(report.processed).c_str(),
                                     std::to_string(changedClasses.size()).c_str(),
                                     std::to_string(report.failed).c_str()})
                          .c_str());
    if (changedClasses.empty()) {
        cli::out().printf("%s\n", ca::i18n::tr("mjt.cmd.opt.out.dryrun_no_changes"));
        return;
    }
    cli::out().printf("%s\n",
                      ca::i18n::trf("mjt.cmd.opt.out.dryrun_changed_header",
                                    {std::to_string(changedClasses.size()).c_str()})
                          .c_str());
    for (const auto& uri : changedClasses) {
        cli::out().printf("    %s\n", uri.c_str());
    }
}

std::string jsonEscapeWrap(const std::string& value)
{
    return "\"" + mjt::json::escape(value) + "\"";
}

void renderJson(const CliRunReport&             report,
                const PassRunStats&             stats,
                const std::vector<std::string>& changedClasses,
                const DryRunRequest&            request)
{
    //固定键序 + 固定形状（可选信息用空串/空数组/None 表示），机器消费按键取值。
    std::string out;
    out += "{\n";
    out += "  \"tool\": \"mjt opt\",\n";
    out += "  \"schema\": \"mjt.opt.dry-run/1\",\n";
    out += "  \"mode\": " + jsonEscapeWrap(outputModeName(request.mode)) + ",\n";
    out += "  \"input\": " + jsonEscapeWrap(request.input) + ",\n";
    out += "  \"runMode\": " + jsonEscapeWrap(request.runMode) + ",\n";
    out += "  \"class\": " + jsonEscapeWrap(request.targetClass) + ",\n";
    out += "  \"method\": " + jsonEscapeWrap(request.targetMethod) + ",\n";

    const std::size_t processed = static_cast<std::size_t>(report.processed);
    const std::size_t unchanged = changedClasses.size() > processed ? 0 : processed - changedClasses.size();
    out += "  \"summary\": {\"processed\": " + std::to_string(report.processed) +
           ", \"changed\": " + std::to_string(changedClasses.size()) +
           ", \"unchanged\": " + std::to_string(unchanged) +
           ", \"failed\": " + std::to_string(report.failed) +
           ", \"ok\": " + (report.ok() ? "true" : "false") + "},\n";

    out += "  \"passes\": [";
    for (std::size_t k = 0; k < stats.slots.size(); ++k) {
        const auto& slot = stats.slots[k];
        out += k == 0 ? "\n" : ",\n";
        out += "    {\"name\": " + jsonEscapeWrap(slot.name) +
               ", \"scope\": " + jsonEscapeWrap(slot.scope != nullptr ? slot.scope : "?") +
               ", \"index\": " + std::to_string(k) +
               ", \"processed\": " + std::to_string(slot.processed) +
               ", \"changed\": " + std::to_string(slot.changed) +
               ", \"failed\": " + std::to_string(slot.failed) +
               ", \"durationMs\": " + durationField(slot.durationMs) + "}";
    }
    out += stats.slots.empty() ? "],\n" : "\n  ],\n";

    out += "  \"changedClasses\": [";
    for (std::size_t k = 0; k < changedClasses.size(); ++k) {
        out += k == 0 ? "\n" : ",\n";
        out += "    " + jsonEscapeWrap(changedClasses[k]);
    }
    out += changedClasses.empty() ? "],\n" : "\n  ],\n";

    out += "  \"failure\": {\"kind\": \"" + std::string(failureKindName(report.failure)) +
           "\", \"message\": " + jsonEscapeWrap(report.failureMessage) + "}\n";
    out += "}";

    cli::out().printf("%s\n", out.c_str());
}

void renderDiff(const CliRunReport&                          report,
                const PassRunStats&                          stats,
                const std::vector<std::string>&              changedClasses,
                const DryRunRequest&                         request,
                const std::map<std::string, std::vector<ca::u8>>& beforeBytes,
                const ResourceContainer&                     pipelineContainer)
{
    cli::out().printf("%s\n",
                      ca::i18n::trf("mjt.cmd.opt.out.dryrun_diff_header",
                                    {request.input.c_str(),
                                     request.runMode.c_str(),
                                     std::to_string(changedClasses.size()).c_str(),
                                     std::to_string(report.processed).c_str()})
                          .c_str());

    //有界契约：模式头 1 行 + 差异体 ≤ diffLimit 行 + 截断/跳过标注 ≤ 2 行。
    std::size_t budget   = request.diffLimit;
    std::size_t skipped  = 0;
    bool        truncated = false;
    auto emit = [&](const std::string& line) {
        if (budget == 0) return false;
        cli::out().printf("%s\n", line.c_str());
        --budget;
        return true;
    };

    jasm::JvmJasmService service;
    for (const auto& uri : changedClasses) {
        if (budget == 0) {
            ++skipped;
            continue;
        }
        auto        it    = beforeBytes.find(uri);
        const auto* res   = pipelineContainer.getResource(uri);
        const auto* cls   = res ? ca::core::tag_cast::cast<ClassResource>(res) : nullptr;
        if (it == beforeBytes.end() || cls == nullptr) {
            ++skipped;
            continue;
        }

        if (!emit("--- a/" + uri)) {
            truncated = true;
            break;
        }
        if (!emit("+++ b/" + uri)) {
            truncated = true;
            break;
        }

        bool        okBefore = false;
        bool        okAfter  = false;
        std::string diagBefore;
        std::string diagAfter;
        const std::string textBefore = jasmText(it->second, service, okBefore, diagBefore);
        const std::string textAfter  = jasmText(cls->get(), service, okAfter, diagAfter);
        if (!okBefore || !okAfter) {
            const std::string& diag = okBefore ? diagAfter : diagBefore;
            if (!emit(ca::i18n::trf("mjt.cmd.opt.out.dryrun_diff_render_failed",
                                    {uri.c_str(), diag.c_str()})
                          .c_str())) {
                truncated = true;
                break;
            }
            continue;
        }
        if (textBefore == textAfter) {
            if (!emit(std::string("  ") +
                      ca::i18n::tr("mjt.cmd.opt.out.dryrun_diff_text_identical"))) {
                truncated = true;
                break;
            }
            continue;
        }

        const auto ops   = lineDiff(splitLines(textBefore), splitLines(textAfter));
        const auto hunks = buildHunks(ops, 2);
        bool       stop  = false;
        for (const auto& hunk : hunks) {
            if (!emit(hunkHeader(hunk))) {
                stop = true;
                break;
            }
            for (const auto& line : hunk.lines) {
                std::string rendered;
                rendered += line.kind;
                rendered += line.text;
                if (!emit(rendered)) {
                    stop = true;
                    break;
                }
            }
            if (stop) break;
        }
        if (stop) {
            truncated = true;
            break;
        }
    }

    if (truncated) {
        cli::out().printf("%s\n",
                          ca::i18n::trf("mjt.cmd.opt.out.dryrun_diff_truncated",
                                        {std::to_string(request.diffLimit).c_str()})
                              .c_str());
    }
    if (skipped > 0) {
        cli::out().printf("%s\n",
                          ca::i18n::trf("mjt.cmd.opt.out.dryrun_diff_skipped",
                                        {std::to_string(skipped).c_str()})
                              .c_str());
    }
}

}   //namespace

mjt::base::CliRunReport run(const DryRunRequest& request,
                                const mjt::base::PipelineBuilder& builder,
                                MethodPredicate predicate, int writerFlags,
                                jbytecode::core::ICommonSuperClassProvider* typeResolver)
{
    CliRunReport report;

    //与 runCli 同口径：空输入路径 = InvalidArguments（调用方已先行校验，双保险）。
    if (request.input.empty()) {
        report.failure        = CliFailureKind::InvalidArguments;
        report.failureMessage = ca::i18n::tr("mbase.cli.runner.err.empty_path");
        if (request.mode == OutputMode::Json) renderJson(report, PassRunStats{}, {}, request);
        return report;
    }

    ResourceContainer container;
    const auto        kind = detectInputKind(request.input);

    try {
        if (kind == InputKind::Jar) {
            ResourceHelper::importFromJar(container, request.input);
        }
        else if (kind == InputKind::Class) {
            ResourceHelper::importFromClassFile(container, request.input);
        }
        else {
            report.failure        = CliFailureKind::UnsupportedInput;
            report.failureMessage = ca::i18n::tr("mbase.cli.runner.err.unsupported_input");
            std::fprintf(stderr,
                         "Error: unsupported input type for %s (expected .jar or .class)\n",
                         request.input.c_str());
            if (request.mode == OutputMode::Json) renderJson(report, PassRunStats{}, {}, request);
            return report;
        }
    }
    catch (const std::exception& e) {
        report.failure        = CliFailureKind::Read;
        report.failureMessage = e.what();
        std::fprintf(stderr, "Error: failed to read %s: %s\n", request.input.c_str(), e.what());
        if (request.mode == OutputMode::Json) renderJson(report, PassRunStats{}, {}, request);
        return report;
    }

    if (container.getResources().empty()) {
        report.failure        = CliFailureKind::NoClass;
        report.failureMessage = ca::i18n::tr("mbase.cli.runner.err.no_resource_or_class");
        std::fprintf(stderr, "Error: no resources found in %s\n", request.input.c_str());
        if (request.mode == OutputMode::Json) renderJson(report, PassRunStats{}, {}, request);
        return report;
    }

    //-c 过滤只限定管线的处理集合（与 runCli 同构的临时 class-only 容器）。
    //dry-run 不导出，故无需像 runCli 那样把字节写回原 container（无写出消费方）。
    ResourceContainer        filteredContainer;
    std::vector<std::string> selectedClassUris;
    if (!request.targetClass.empty()) {
        for (const auto& [uri, resPtr] : container.getResources()) {
            if (resPtr->getType() != ResourceType::Class ||
                !classUriMatches(uri, request.targetClass)) {
                continue;
            }
            auto* cls = ca::core::tag_cast::cast<ClassResource>(resPtr.get());
            selectedClassUris.push_back(uri);
            filteredContainer.addResource(
                uri, std::make_unique<ClassResource>(cls->getLocation(), cls->get()));
        }
    }
    ResourceContainer& pipelineContainer =
        request.targetClass.empty() ? container : filteredContainer;

    //字节快照（管线前）：变更类 = 管线后字节不等。仅内存驻留，零落盘。
    std::map<std::string, std::vector<ca::u8>> beforeBytes;
    for (const auto& [uri, resPtr] : pipelineContainer.getResources()) {
        if (resPtr->getType() != ResourceType::Class) continue;
        beforeBytes[uri] = ca::core::tag_cast::cast<ClassResource>(resPtr.get())->get();
    }

    //管线（与实跑同一构建路径；PassRunStats 只读采集，指针为空等价零包装）。
    PassRunStats stats;
    auto nestedPipeline = builder.buildNestedPipeline(writerFlags, typeResolver, predicate,
                                                      /*progress=*/nullptr, {}, &stats);
    AsmGlobalContext globalContext;
    try {
        const int classWriterFlags =
            typeResolver != nullptr ? jbytecode::core::ClassWriter::COMPUTE_FRAMES : 0;
        runJarPipeline(pipelineContainer, nestedPipeline, globalContext, classWriterFlags,
                       typeResolver);
    }
    catch (const std::exception& e) {
        report.failure        = CliFailureKind::Processing;
        report.failureMessage = e.what();
        std::fprintf(stderr, "Error: pipeline failed: %s\n", e.what());
        if (request.mode == OutputMode::Json) renderJson(report, stats, {}, request);
        return report;
    }

    //processed 计数与变更类清单（字节级对比；输出按 URI 字典序稳定化）。
    std::vector<std::string> changedClasses;
    for (const auto& [uri, resPtr] : pipelineContainer.getResources()) {
        if (resPtr->getType() != ResourceType::Class) continue;
        report.processed++;
        auto it = beforeBytes.find(uri);
        if (it == beforeBytes.end()) continue;
        const auto* cls = ca::core::tag_cast::cast<ClassResource>(resPtr.get());
        if (cls->get() != it->second) changedClasses.push_back(uri);
    }
    std::sort(changedClasses.begin(), changedClasses.end());

    if (report.processed == 0) {
        if (!request.targetClass.empty()) {
            report.failure        = CliFailureKind::NoMatch;
            report.failureMessage = ca::i18n::tr("mbase.cli.runner.err.no_class_match");
            std::fprintf(stderr,
                         "Error: no class files matched --class '%s' in %s\n",
                         request.targetClass.c_str(),
                         request.input.c_str());
        }
        else {
            report.failure        = CliFailureKind::NoClass;
            report.failureMessage = ca::i18n::tr("mbase.cli.runner.err.no_class");
            std::fprintf(stderr, "Error: no class files found in %s\n", request.input.c_str());
        }
        if (request.mode == OutputMode::Json) renderJson(report, stats, {}, request);
        return report;
    }

    switch (request.mode) {
    case OutputMode::Json:
        renderJson(report, stats, changedClasses, request);
        break;
    case OutputMode::Counts:
        renderCounts(report, stats, changedClasses, request);
        break;
    case OutputMode::JasmDiff:
        renderDiff(report, stats, changedClasses, request, beforeBytes, pipelineContainer);
        break;
    }
    return report;
}

}   //namespace mjt::opt_dry_run
