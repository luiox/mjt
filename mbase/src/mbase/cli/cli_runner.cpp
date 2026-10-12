#include "mbase/cli/cli_runner.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>

#include "jbytecode/jbytecode.hpp"
#include "libca/fs/file_util.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mbase/i18n.hpp"
#include "mbase/jar/jar_util.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/model/resource_helper.hpp"
#include "mbase/model/resource_io.hpp"
#include "mbase/model/resource_model.hpp"
#include "mbase/spi/pass_manager.hpp"
#include "mbase/spi/progress_sink.hpp"
#include "mbase/util/class_name_util.hpp"

namespace mjt::base {

namespace {

using ca::fs::FileUtil;

//判断输入是 jar、class 还是其它
enum class InputKind
{
    Jar,
    Class,
    Unsupported
};

void setFailure(CliRunReport& report, CliFailureKind kind, std::string message)
{
    report.failure        = kind;
    report.failureMessage = std::move(message);
}

bool verifyOutput(const CliOptions& options, InputKind kind)
{
    ResourceContainer check;
    if (kind == InputKind::Jar) {
        ResourceHelper::importFromJar(check, options.output);
    }
    else if (kind == InputKind::Class) {
        ResourceHelper::importFromClassFile(check, options.output);
    }
    else {
        return false;
    }
    bool hasClass = false;
    for (const auto& [uri, resource] : check.getResources()) {
        if (!resource) {
            fprintf(stderr, "[verify] null resource for %s\n", uri.c_str());
            return false;
        }
        if (resource->getType() != ResourceType::Class) {
            //病态 jar 可能含 0 字节 / 非 class 内容但以 .class 命名的 entry
            //（如被混淆器改坏的 module-info.class）。此时按内容分拣已是
            //UnknownResource，说明其字节本身不构成可解析 class，跳过即可。
            continue;
        }

        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(resource.get());
        if (!classResource || classResource->get().empty()) {
            fprintf(stderr, "[verify] empty class bytes for %s\n", uri.c_str());
            return false;
        }

        ca::str::Utf8StringArena classArena;
        jbytecode::tree::ClassNode classNode;
        const auto&               bytes = classResource->get();
        try {
            jbytecode::core::ClassReader(
                classArena, bytes.data(), static_cast<ca::i32>(bytes.size()))
                .accept(&classNode, 0);
        }
        catch (const std::exception& e) {
            fprintf(stderr, "[verify] parse exception for %s: %s\n", uri.c_str(), e.what());
            return false;
        }
        if (classNode.name.is_empty()) {
            fprintf(stderr, "[verify] empty class name for %s\n", uri.c_str());
            return false;
        }
        hasClass = true;
    }
    return hasClass;
}

bool validateOutputTarget(const CliOptions& options, CliRunReport& report)
{
    if (!FileUtil::exists(options.output)) return true;
    setFailure(report, CliFailureKind::InvalidArguments, tr("mbase.cli.runner.err.output_exists"));
    fprintf(stderr, "Error: refusing to overwrite existing output %s\n", options.output.c_str());
    return false;
}

std::string createStagedOutput(const std::string& output)
{
    //output 持 argv 值（UTF-8 契约）：显式 u8path（u8path 清扫）；base/回传
    //串一律 generic_u8string 保持 UTF-8 字节域——原 generic_string 按 ACP 再编码，
    //非 UTF-8 ACP 进程里 UTF-8 路径会被转 mojibake 且 create_file（libca UTF-8
    //契约）二次解码放大。
    const std::filesystem::path destination = std::filesystem::u8path(output);
    const auto parent = destination.has_parent_path() ? destination.parent_path()
                                                       : std::filesystem::path(".");
    const auto base = destination.filename().generic_u8string();
    static std::atomic<ca::u64> sequence{0};
    const auto timestamp = static_cast<ca::u64>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (ca::u32 attempt = 0; attempt < 128; ++attempt) {
        const auto suffix = std::to_string(timestamp) + "." +
                            std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        const auto candidate =
            parent / std::filesystem::u8path(base + ".mjt-stage." + suffix);
        if (FileUtil::create_file(candidate.generic_u8string()))
            return candidate.generic_u8string();
    }
    return {};
}

bool publishStagedOutput(std::string& stagedOutput, const CliOptions& options, CliRunReport& report)
{
    if (FileUtil::move(stagedOutput, options.output, false)) {
        stagedOutput.clear();
        return true;
    }
    setFailure(report, CliFailureKind::Write, tr("mbase.cli.runner.err.publish_failed"));
    report.failed    = report.processed;
    report.processed = 0;
    fprintf(stderr, "Error: failed to publish %s\n", options.output.c_str());
    return false;
}

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

}   //namespace

//把 targetMethod（"name" 或 "nameI"）解析成方法过滤谓词。
MethodPredicate buildMethodPredicate(const std::string& targetMethod)
{
    if (targetMethod.empty()) return nullptr;

    //拆 "name(desc)"：左括号前是 name，括号内（含到结尾）是 desc
    std::string name;
    std::string desc;
    auto        paren = targetMethod.find('(');
    if (paren == std::string::npos) {
        name = targetMethod;   //纯名字，desc 任意
    }
    else {
        name = targetMethod.substr(0, paren);
        desc = targetMethod.substr(paren);   //含 '('
    }

    return [name, desc](const jbytecode::tree::MethodNode& m) -> bool {
        if (m.name != name.c_str()) return false;           //name 精确匹配
        if (!desc.empty() && !(m.desc == desc.c_str())) {   //desc 给了则精确匹配
            return false;
        }
        return true;
    };
}

CliRunReport runCli(const CliOptions& options, const ClassProcessor& processor)
{
    CliRunReport report;

    if (options.input.empty() || options.output.empty()) {
        setFailure(report, CliFailureKind::InvalidArguments, tr("mbase.cli.runner.err.empty_path"));
        return report;   //调用方负责校验必填项并报错
    }
    if (!validateOutputTarget(options, report)) return report;

    ResourceContainer container;

    const auto kind = detectInputKind(options.input);

    try {
        if (kind == InputKind::Jar) {
            ResourceHelper::importFromJar(container, options.input);
        }
        else if (kind == InputKind::Class) {
            ResourceHelper::importFromClassFile(container, options.input);
        }
        else {
            setFailure(report, CliFailureKind::UnsupportedInput, tr("mbase.cli.runner.err.unsupported_input"));
            fprintf(stderr,
                    "Error: unsupported input type for %s (expected .jar or .class)\n",
                    options.input.c_str());
            return report;   //不支持的输入类型
        }
    }
    catch (const std::exception& e) {
        setFailure(report, CliFailureKind::Read, e.what());
        fprintf(stderr, "Error: failed to read %s: %s\n", options.input.c_str(), e.what());
        return report;
    }

    //遍历所有 ClassResource，对每个跑 processor
    //注意：ca::core::tag_cast::dyn_cast 在 MSVC 下对非 const 指针会误推为 const 返回，
    //这里已用 getType 保证类型安全，直接用 ca::core::tag_cast::cast 拿到非 const 指针。
    auto&       resources = container.getResources();
    std::string singleClassUri;
    for (auto& [uri, resPtr] : resources) {
        IResource* raw = resPtr.get();
        if (raw->getType() != ResourceType::Class) continue;
        //-c 过滤：只处理匹配 targetClass 的类
        if (!classUriMatches(uri, options.targetClass)) continue;
        auto* cls = ca::core::tag_cast::cast<ClassResource>(raw);

        auto bytes  = cls->get();
        auto result = processor(uri, bytes);
        if (result.ok) {
            cls->set(std::move(bytes));
            report.processed++;
        }
        else {
            setFailure(report, CliFailureKind::Processing, result.message);
            fprintf(stderr, "  FAIL: %s - %s\n", uri.c_str(), result.message.c_str());
            report.failed++;
        }
        singleClassUri = uri;
    }

    if (report.processed == 0 && report.failed == 0) {
        if (!options.targetClass.empty()) {
            setFailure(report, CliFailureKind::NoMatch, tr("mbase.cli.runner.err.no_class_match"));
            fprintf(stderr,
                    "Error: no class files matched --class '%s' in %s\n",
                    options.targetClass.c_str(),
                    options.input.c_str());
        }
        else {
            setFailure(report, CliFailureKind::NoClass, tr("mbase.cli.runner.err.no_class"));
            fprintf(stderr, "Error: no class files found in %s\n", options.input.c_str());
        }
        return report;
    }
    if (report.failed != 0) return report;

    std::string stagedOutput = createStagedOutput(options.output);
    if (stagedOutput.empty()) {
        setFailure(report, CliFailureKind::Write, tr("mbase.cli.runner.err.temp_failed"));
        report.failed    = report.processed;
        report.processed = 0;
        return report;
    }
    CliOptions stagedOptions = options;
    stagedOptions.output = stagedOutput;

    //写出
    try {
        if (kind == InputKind::Jar) {
            ResourceHelper::exportToJar(container, stagedOutput);
        }
        else {
            ResourceHelper::exportToClassFile(container, stagedOutput, singleClassUri);
        }
    }
    catch (const std::exception& e) {
        FileUtil::remove(stagedOutput);
        fprintf(stderr, "Error: failed to write %s: %s\n", options.output.c_str(), e.what());
        setFailure(report, CliFailureKind::Write, e.what());
        report.failed    = report.processed;   //全部视为失败
        report.processed = 0;
        return report;
    }

    try {
        if (!verifyOutput(stagedOptions, kind)) {
            FileUtil::remove(stagedOutput);
            setFailure(report, CliFailureKind::Verification, tr("mbase.cli.runner.err.verify_failed"));
            report.failed    = report.processed;
            report.processed = 0;
            return report;
        }
    }
    catch (const std::exception& e) {
        FileUtil::remove(stagedOutput);
        setFailure(report, CliFailureKind::Verification, e.what());
        report.failed    = report.processed;
        report.processed = 0;
        fprintf(stderr, "Error: failed to verify %s: %s\n", options.output.c_str(), e.what());
        return report;
    }
    if (!publishStagedOutput(stagedOutput, options, report)) {
        FileUtil::remove(stagedOutput);
        return report;
    }

    printf("Done: %d classes processed, %d failed -> %s\n",
           report.processed,
           report.failed,
           options.output.c_str());
    return report;
}

//── 全局流水线重载 ──

CliRunReport runCli(const CliOptions& options, GlobalPassManager& gpm, ProgressSink* progress,
                    jbytecode::core::ICommonSuperClassProvider* typeResolver)
{
    CliRunReport report;

    if (options.input.empty() || options.output.empty()) {
        setFailure(report, CliFailureKind::InvalidArguments, tr("mbase.cli.runner.err.empty_path"));
        return report;
    }
    if (!validateOutputTarget(options, report)) return report;

    ResourceContainer container;
    const auto        kind = detectInputKind(options.input);

    //loading 阶段进度（issue ）：import 时逐 entry 刷新。
    //lastLoading 记录最终 entry 状态，finishPhase 时用于显示满进度。
    ProgressSink::State lastLoading;
    if (progress) progress->start();
    try {
        if (kind == InputKind::Jar) {
            if (progress) {
                ResourceHelper::importFromJar(
                    container,
                    options.input,
                    [progress, &lastLoading](int index, int total, const std::string& name) {
                        ProgressSink::State s;
                        s.phase       = ProgressSink::Phase::Loading;
                        s.total       = total;
                        s.index       = index;
                        s.currentName = escapeClassName(ca::str::Utf8StringRef::from_data(
                            reinterpret_cast<const ca::u8*>(name.data()), name.size()));
                        lastLoading   = s;   //留作 finish 的满进度态
                        progress->update(std::move(s));
                    });
            }
            else {
                ResourceHelper::importFromJar(container, options.input);
            }
        }
        else if (kind == InputKind::Class) {
            ResourceHelper::importFromClassFile(container, options.input);
        }
        else {
            setFailure(report, CliFailureKind::UnsupportedInput, tr("mbase.cli.runner.err.unsupported_input"));
            fprintf(stderr,
                    "Error: unsupported input type for %s (expected .jar or .class)\n",
                    options.input.c_str());
            return report;
        }
    }
    catch (const std::exception& e) {
        setFailure(report, CliFailureKind::Read, e.what());
        fprintf(stderr, "Error: failed to read %s: %s\n", options.input.c_str(), e.what());
        return report;
    }

    if (progress) {
        //强制刷成 100% 进度条 + 换行 + "loading done in Xs"。
        lastLoading.index = lastLoading.total;   //满进度
        progress->finishPhase(lastLoading, "loading");
        progress->start();   //重置计时，进入 pipeline 阶段
    }

    if (container.getResources().empty()) {
        setFailure(report, CliFailureKind::NoClass, tr("mbase.cli.runner.err.no_resource_or_class"));
        fprintf(stderr, "Error: no resources found in %s\n", options.input.c_str());
        return report;
    }

    //-c 过滤只限定 pipeline 的处理集合，不能从原 container 删除资源；
    //否则导出的 jar 会丢掉未匹配 class。过滤时构造临时 class-only 容器，
    //pipeline 跑完后再把匹配类字节写回原 container。
    ResourceContainer        filteredContainer;
    std::vector<std::string> selectedClassUris;
    if (!options.targetClass.empty()) {
        for (const auto& [uri, resPtr] : container.getResources()) {
            if (resPtr->getType() != ResourceType::Class ||
                !classUriMatches(uri, options.targetClass)) {
                continue;
            }
            auto* cls = ca::core::tag_cast::cast<ClassResource>(resPtr.get());
            selectedClassUris.push_back(uri);
            filteredContainer.addResource(
                uri, std::make_unique<ClassResource>(cls->getLocation(), cls->get()));
        }
    }

    ResourceContainer& pipelineContainer =
        options.targetClass.empty() ? container : filteredContainer;

    //跑全局流水线（内部创建 ClassAM + MethodAM，经 Proxy 暴露给下级 pass）。
    //pipeline 阶段的 class 进度由 gpm 内部的 GlobalToClassPassAdaptor 经 progress 驱动
    //（progress 在 buildNestedPipeline 时已注入 adaptor）。
    AsmGlobalContext globalContext;
    try {
        //classWriterFlags 与 resolver 一致：有 JDK resolver 时启用 COMPUTE_FRAMES
        //（final writer 与 pass 内部 writeback 都能重算 StackMapTable）。
        const int classWriterFlags =
            typeResolver != nullptr ? jbytecode::core::ClassWriter::COMPUTE_FRAMES : 0;
        runJarPipeline(pipelineContainer, gpm, globalContext, classWriterFlags, typeResolver);
    }
    catch (const std::exception& e) {
        setFailure(report, CliFailureKind::Processing, e.what());
        fprintf(stderr, "Error: pipeline failed: %s\n", e.what());
        return report;
    }

    if (!options.targetClass.empty()) {
        for (const auto& uri : selectedClassUris) {
            auto* source = ca::core::tag_cast::cast<ClassResource>(filteredContainer.getResource(uri));
            auto* target = ca::core::tag_cast::cast<ClassResource>(container.getResource(uri));
            if (source && target) target->set(source->get());
        }
    }

    //统计所有 ClassResource 为 processed（pipeline 阶段的最终总数也来自此）。
    std::string singleClassUri;
    for (auto& [uri, resPtr] : pipelineContainer.getResources()) {
        if (resPtr->getType() != ResourceType::Class) continue;
        report.processed++;
        singleClassUri = uri;
    }

    if (progress) {
        //pipeline 满进度：index=total=已处理的 class 数。
        ProgressSink::State finalPipeline;
        finalPipeline.phase = ProgressSink::Phase::Pipeline;
        finalPipeline.total = report.processed;
        finalPipeline.index = report.processed;
        progress->finishPhase(finalPipeline, "pipeline");
    }

    if (report.processed == 0) {
        if (!options.targetClass.empty()) {
            setFailure(report, CliFailureKind::NoMatch, tr("mbase.cli.runner.err.no_class_match"));
            fprintf(stderr,
                    "Error: no class files matched --class '%s' in %s\n",
                    options.targetClass.c_str(),
                    options.input.c_str());
        }
        else {
            setFailure(report, CliFailureKind::NoClass, tr("mbase.cli.runner.err.no_class"));
            fprintf(stderr, "Error: no class files found in %s\n", options.input.c_str());
        }
        return report;
    }

    std::string stagedOutput = createStagedOutput(options.output);
    if (stagedOutput.empty()) {
        setFailure(report, CliFailureKind::Write, tr("mbase.cli.runner.err.temp_failed"));
        report.failed    = report.processed;
        report.processed = 0;
        return report;
    }
    CliOptions stagedOptions = options;
    stagedOptions.output = stagedOutput;

    //写出
    try {
        if (kind == InputKind::Jar) {
            ResourceHelper::exportToJar(container, stagedOutput);
        }
        else {
            ResourceHelper::exportToClassFile(container, stagedOutput, singleClassUri);
        }
    }
    catch (const std::exception& e) {
        FileUtil::remove(stagedOutput);
        fprintf(stderr, "Error: failed to write %s: %s\n", options.output.c_str(), e.what());
        setFailure(report, CliFailureKind::Write, e.what());
        report.failed    = report.processed;
        report.processed = 0;
        return report;
    }

    try {
        if (!verifyOutput(stagedOptions, kind)) {
            FileUtil::remove(stagedOutput);
            setFailure(report, CliFailureKind::Verification, tr("mbase.cli.runner.err.verify_failed"));
            report.failed    = report.processed;
            report.processed = 0;
            return report;
        }
    }
    catch (const std::exception& e) {
        FileUtil::remove(stagedOutput);
        setFailure(report, CliFailureKind::Verification, e.what());
        report.failed    = report.processed;
        report.processed = 0;
        fprintf(stderr, "Error: failed to verify %s: %s\n", options.output.c_str(), e.what());
        return report;
    }
    if (!publishStagedOutput(stagedOutput, options, report)) {
        FileUtil::remove(stagedOutput);
        return report;
    }

    printf("Done: %d classes processed, %d failed -> %s\n",
           report.processed,
           report.failed,
           options.output.c_str());
    return report;
}

}   //namespace mjt::base
