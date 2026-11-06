/// @file        cmd_decompile.cpp
/// @brief       `mjt decompile` 命令核心：单类（短安全名 + classpath 上下文）
///              与批量（敌意名预检 + 字节级快照暂存）两条链路。
///
/// #63 修法落点：单类模式把目标类字节写临时目录短安全名（Target.class），
/// 后端以单文件输入 + classpath 指向原 jar 保跨类型上下文——类名从不落盘，
/// 短名方案对「单段超 255 UTF-16 单元」与含非法字符的敌意名天然免疫，且
/// 整 jar 不进批量，敌意类无法级联毒化（vineflower rc=1 连累全 jar 的形态
/// 在结构性上不再可达）。批量模式在把 jar 交给后端前做敌意名预检，无法落
/// 盘的条目经字节级快照暂存剔除并在 stderr 披露 N 项 + 指路单类模式 / jasm
/// 文本面。
#include "commands.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "libca/str/utf16_string.hpp"
#include "libca/str/utf8_string.hpp"

#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/jar/jar_writer.hpp"

#include "shared/decompiler.hpp"
#include "shared/i18n_errors.hpp"
#include "shared/output_sink.hpp"
#include "shared/text_codec.hpp"
#include "workspace/workspace.hpp"

namespace mjt::cli {
namespace {

namespace fs = std::filesystem;

/// Windows 单段文件名上限（UTF-16 单元）。
constexpr ca::usize kMaximumSegmentUtf16Units = 255;

/// 披露面条目名截断上限（字节；敌意名可上万字节，原始字节全量展示无意义）。
constexpr std::size_t kSkippedDisplayLimit = 96;

/// Utf8String/UTF-8 std::string → 借用视图（与 info / jar fix 同款构造）。
ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// 一次性临时目录（进程临时目录 + 单调序号；析构整体清理）。
class TempDirGuard
{
public:
    TempDirGuard()
    {
        static std::atomic<ca::u64> sequence{0};
        const auto id =
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(sequence.fetch_add(1));
        std::error_code ec;
        path_ = fs::temp_directory_path(ec) / ("mjt-cli-decompile-" + id);
        if (!ec) fs::create_directories(path_, ec);
        valid_ = !ec;
    }
    ~TempDirGuard()
    {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDirGuard(const TempDirGuard&)            = delete;
    TempDirGuard& operator=(const TempDirGuard&) = delete;

    bool            valid() const { return valid_; }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
    bool     valid_ = false;
};

/// DecompileError 便捷构造（i18n 渲染）。
DecompileError decompileError(DecompileError::Kind kind, const char* key,
                              std::initializer_list<std::string_view> args = {})
{
    DecompileError error;
    error.kind    = kind;
    error.message = shared::trfStr(key, args);
    return error;
}

/// workspace 错误 → decompile 错误（openJar / readClassBytes 失败通道；
/// NotFound = 选择器无命中，归用法面，其余归运行面）。
DecompileError mapWorkspaceError(const workspace::WorkspaceErrorInfo& info,
                                 const char* notFoundKey, const char* runtimeKey,
                                 const std::string& selector)
{
    if (info.code == workspace::WorkspaceError::NotFound)
        return decompileError(DecompileError::Kind::ClassNotFound, notFoundKey, {selector});
    DecompileError error;
    error.kind    = DecompileError::Kind::Runtime;
    error.message = shared::trfStr(runtimeKey, {info.message});
    return error;
}

/// 条目名分段：'/' 与 '\\' 都是落盘分隔符（Windows 上含 '\\' 的条目名经
/// u8path 构造会成为子路径，按分段处理才是真实落盘形态）。
std::vector<std::string> splitEntrySegments(const std::string& canonical)
{
    std::vector<std::string> segments;
    std::string              current;
    for (const char character : canonical) {
        if (character == '/' || character == '\\') {
            segments.push_back(std::move(current));
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    segments.push_back(std::move(current));
    return segments;
}

/// 段内 UTF-16 单元数（规范化后的输入恒为合法 UTF-8，转换不失败）。
ca::usize utf16UnitCount(const std::string& segment)
{
    return ca::str::Utf16String::from_utf8_string(refOf(segment)).size();
}

/// 单段文件系统合法性检查；不安全时 reason 落稳定名（首个命中即返回）。
EntryNameSafety classifySegment(const std::string& segment)
{
    if (segment.empty()) return {false, "empty_segment"};
    if (segment == "." || segment == "..") return {false, "parent_or_self"};
    if (utf16UnitCount(segment) > kMaximumSegmentUtf16Units)
        return {false, "segment_too_long"};
    for (const unsigned char character : segment) {
        //文件系统非法字符：Windows 保留集 + 控制字符（POSIX 仅禁 '/' 与空
        //字节，但跨平台 CLI 按最严集预检，落盘面只判 Windows 形态）。
        if (character == '<' || character == '>' || character == ':' || character == '"' ||
            character == '|' || character == '?' || character == '*' || character < 0x20)
            return {false, "illegal_character"};
    }
    //Windows 落盘剥离尾点/尾空格：这类名字无法物化（Win32 CreateFile 语义）。
    if (segment.back() == '.' || segment.back() == ' ')
        return {false, "trailing_dot_or_space"};
    return {true, {}};
}

/// 条目是否 class 形态（与 mjbase JarUtil::isClassFile 容器分类口径同源）：
/// 后缀大小写敏感；OS2.9 形 `.class/` 尾斜杠条目按类计——容器导入侧剥尾
/// 斜杠登记为 ClassResource，计数侧不对齐会让 strings 的 skipped 对账出
/// 负数（进扫描不进总数）。
bool isClassEntryName(const std::string& canonical)
{
    constexpr std::string_view kSuffix      = ".class";
    constexpr std::string_view kSuffixSlash = ".class/";
    const auto endsWith = [&canonical](std::string_view suffix) {
        return canonical.size() >= suffix.size() &&
               canonical.compare(canonical.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    return endsWith(kSuffixSlash) || endsWith(kSuffix);
}

/// 输出扩展名是否 .zip（大小写不敏感，与共享层 input_ext 口径一致）。
bool isZipOutputPath(const std::string& outputPath)
{
    const auto path = fs::u8path(outputPath);
    auto       ext  = path.extension().u8string();
    for (auto& character : ext)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return ext == ".zip";
}

/// 共享层 DecompileRequest 组装（单类与批量公共字段）。
DecompileRequest buildRequest(const DecompileCliOptions& options)
{
    DecompileRequest request;
    request.kind          = options.kind;
    request.decompilerJar = options.decompilerJar;
    request.timeout       = std::chrono::milliseconds(options.timeoutMs);
    request.maxMemoryMiB  = static_cast<ca::i32>(options.maxMemoryMiB);
    return request;
}

/// 共享层 Status → decompile 错误（后端执行失败通道；消息已 i18n 渲染）。
DecompileError mapStatusError(const ca::Status& status)
{
    DecompileError error;
    error.kind    = DecompileError::Kind::Runtime;
    error.message = status.message();
    return error;
}

/// 读整个文件为文本（二进制安全；读不到按 Runtime 报错）。
ca::Result<std::string, DecompileError> readWholeFile(const fs::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return ca::Err(decompileError(DecompileError::Kind::Runtime,
                                      "mjt.cli.decompile.err.readback_failed",
                                      {path.u8string()}));
    std::string content;
    char        chunk[64 * 1024];
    while (input.read(chunk, static_cast<std::streamsize>(sizeof(chunk))) ||
           input.gcount() > 0)
        content.append(chunk, static_cast<std::size_t>(input.gcount()));
    if (input.bad())
        return ca::Err(decompileError(DecompileError::Kind::Runtime,
                                      "mjt.cli.decompile.err.readback_failed",
                                      {path.u8string()}));
    return ca::Ok(std::move(content));
}

/// 字节级快照暂存：JvmJarReader 原始字节直通读 → JarWriter 逐条目写回，
/// 剔除 @p excludedRawNames 命中（键 = 条目名原始字节）的条目，其余零漂移
/// 拷贝。条目名不注入宽容解码（读侧原始字节 / 写侧原样回写，双侧零解码
/// 对齐）。返回交给后端的条目数。
ca::Result<ca::usize, DecompileError> stageJarWithoutEntries(
    const std::string& jarPath, const fs::path& stagedJar,
    const std::vector<std::string>& excludedRawNames)
{
    struct StagedEntry
    {
        std::string         name;
        std::vector<ca::u8> bytes;
    };
    std::vector<StagedEntry> kept;
    {
        mjbase::jar::JvmJarReader reader(refOf(jarPath));
        auto collected = reader.read([&](const ca::str::Utf8String& name,
                                         const std::vector<ca::u8>& bytes) {
            const std::string rawName = name.to_std_string();
            if (std::find(excludedRawNames.begin(), excludedRawNames.end(), rawName) !=
                excludedRawNames.end())
                return;
            kept.push_back({rawName, bytes});
        });
        if (collected.is_err())
            return ca::Err(decompileError(DecompileError::Kind::Runtime,
                                          "mjt.cli.decompile.err.stage_failed", {jarPath}));
    }
    mjbase::jar::JarWriter writer(refOf(stagedJar.u8string()));
    auto written = writer.write([&](mjbase::jar::IEntryWriter& entryWriter) {
        for (auto& entry : kept)
            entryWriter.writeEntry(
                ca::str::Utf8StringRef::from_data(
                    reinterpret_cast<const ca::u8*>(entry.name.data()), entry.name.size()),
                std::move(entry.bytes));
    });
    if (written.is_err())
        return ca::Err(decompileError(DecompileError::Kind::Runtime,
                                      "mjt.cli.decompile.err.stage_failed", {jarPath}));
    return ca::Ok(kept.size());
}

}   // namespace

ca::Result<DecompilerKind, DecompileError> parseDecompilerBackendName(const std::string& name)
{
    std::string lowered;
    lowered.reserve(name.size());
    for (const char character : name)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    if (lowered == "cfr") return ca::Ok(DecompilerKind::Cfr);
    if (lowered == "vineflower") return ca::Ok(DecompilerKind::Vineflower);
    if (lowered == "jcdc") return ca::Ok(DecompilerKind::Jcdc);
    return ca::Err(decompileError(DecompileError::Kind::UnknownBackend,
                                  "mjt.shared.err.unknown_decompiler", {name}));
}

EntryNameSafety classifyEntryNameForDisk(const std::string& rawEntryName)
{
    //条目名原始字节 → 规范 UTF-8（与 archiveEntryRelativePath 单一规范路径
    //同源）：合法 UTF-8 逐字节原样、GBK 恢复、残字节 U+FFFD 兜底。
    const std::string canonical = shared::text::toCanonicalUtf8(rawEntryName);
    //目录条目（'/' 结尾，jar 写出器目录补全产物）不是文件落盘对象：后端只按
    //目录创建，不进预检、不计跳过。
    if (!canonical.empty() && (canonical.back() == '/' || canonical.back() == '\\'))
        return {true, {}};
    for (const auto& segment : splitEntrySegments(canonical)) {
        auto verdict = classifySegment(segment);
        if (!verdict.safe) return verdict;
    }
    return {true, {}};
}

ca::Result<ca::usize, DecompileError> countJarClassEntries(const std::string& jarPath)
{
    //仅 central directory 元数据（不解压条目内容）；名称统计与批量预检同一
    //canonical 口径。
    auto listed = mjbase::jar::listJarEntries(refOf(jarPath));
    if (listed.is_err())
        return ca::Err(decompileError(DecompileError::Kind::Runtime,
                                      "mjt.cli.decompile.err.open_failed", {jarPath}));
    ca::usize count = 0;
    for (const auto& entry : std::move(listed).unwrap()) {
        if (isClassEntryName(shared::text::toCanonicalUtf8(entry.name.to_std_string()))) ++count;
    }
    return ca::Ok(count);
}

ca::Result<SingleDecompileReport, DecompileError> decompileSingleClass(
    const std::string& jarPath, const std::string& internalName,
    const std::string& outputJavaPath, const DecompileCliOptions& options)
{
    TempDirGuard temporary;
    if (!temporary.valid())
        return ca::Err(decompileError(DecompileError::Kind::Runtime,
                                      "mjt.cli.decompile.err.temp_failed"));

    //目标类字节：workspace readClassBytes（URI / 内名双解析语义与产品其余面
    //同源——精确 URI 命中、字节级 this_class 内名命中、Multi-Release 确定序）。
    workspace::WorkspaceBase ws;
    auto opened = ws.openJar(refOf(jarPath));
    if (opened.is_err())
        return ca::Err(mapWorkspaceError(std::move(opened).unwrap_err(),
                                         "mjt.cli.decompile.err.class_not_found",
                                         "mjt.cli.decompile.err.open_failed", jarPath));
    auto classBytes = ws.readClassBytes(refOf(internalName));
    if (classBytes.is_err())
        return ca::Err(mapWorkspaceError(std::move(classBytes).unwrap_err(),
                                         "mjt.cli.decompile.err.class_not_found",
                                         "mjt.cli.decompile.err.open_failed", internalName));
    const auto bytesValue = std::move(classBytes).unwrap();

    //唯一物化路径 = 短安全名：敌意名（超长 / 非法字符）从不进文件系统。
    const auto stagedClass = temporary.path() / "Target.class";
    {
        std::ofstream out(stagedClass, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytesValue.bytes.data()),
                  static_cast<std::streamsize>(bytesValue.bytes.size()));
        out.close();
        if (!out)
            return ca::Err(decompileError(DecompileError::Kind::Runtime,
                                          "mjt.cli.decompile.err.stage_failed", {jarPath}));
    }

    //输出：-o 直传共享层（.java 校验归 main 用法面）；stdout 模式落临时固定
    //名读回。classpath = 原 jar 直传后端（零拷贝跨类型上下文）。
    const bool stdoutMode = outputJavaPath.empty();
    DecompileRequest request = buildRequest(options);
    request.inputPath        = stagedClass.u8string();
    request.outputPath = stdoutMode ? (temporary.path() / "Target.java").u8string()
                                    : outputJavaPath;
    request.classPath  = jarPath;
    //后端暂存区不嵌套在本 guard 目录下（tempRoot 留空 = 系统临时目录顶层）：
    //CFR 对超限类名自截断的产物名仍可长达 255 单元，双层临时目录嵌套会把
    //产物路径推过 MAX_PATH（读回 copy_file 报 PATH_NOT_FOUND）；共享层自带
    //RAII 清理，单层嵌套 + 短 guard 名把路径预算留给产物名。
    request.tempRoot   = {};
    auto decompiled    = decompile(request);
    if (decompiled.is_err()) return ca::Err(mapStatusError(std::move(decompiled).unwrap_err()));
    auto result = std::move(decompiled).unwrap();

    SingleDecompileReport report;
    report.internalName  = internalName;
    report.backend       = decompilerKindId(options.kind);
    report.decompilerJar = std::move(result.decompilerJar);
    if (stdoutMode) {
        auto source = readWholeFile(temporary.path() / "Target.java");
        if (source.is_err()) return ca::Err(std::move(source).unwrap_err());
        report.source = std::move(source).unwrap();
    }
    return ca::Ok(std::move(report));
}

ca::Result<BatchDecompileReport, DecompileError> decompileJarBatch(
    const std::string& jarPath, const std::string& outputPath,
    const DecompileCliOptions& options)
{
    TempDirGuard temporary;
    if (!temporary.valid())
        return ca::Err(decompileError(DecompileError::Kind::Runtime,
                                      "mjt.cli.decompile.err.temp_failed"));

    //zip 输出仅 vineflower（共享层契约；CFR 无归档输出能力）——提前到用法面。
    if (isZipOutputPath(outputPath) && options.kind != DecompilerKind::Vineflower)
        return ca::Err(decompileError(DecompileError::Kind::InvalidArgument,
                                      "mjt.shared.err.zip_output_vineflower_only",
                                      {decompilerKindId(options.kind)}));

    //敌意名预检（批量落盘路径的唯一闸门）：条目名经 central directory 元数据
    //列举（不解压），canonical 口径与落盘构造（archiveEntryRelativePath）同源。
    auto listed = mjbase::jar::listJarEntries(refOf(jarPath));
    if (listed.is_err())
        return ca::Err(decompileError(DecompileError::Kind::Runtime,
                                      "mjt.cli.decompile.err.open_failed", {jarPath}));
    const auto entries = std::move(listed).unwrap();

    BatchDecompileReport report;
    report.zipOutput  = isZipOutputPath(outputPath);
    report.outputPath = outputPath;
    std::vector<std::string> excludedRawNames;
    for (const auto& entry : entries) {
        const std::string rawName   = entry.name.to_std_string();
        const std::string canonical = shared::text::toCanonicalUtf8(rawName);
        if (isClassEntryName(canonical)) ++report.classEntries;
        const auto verdict = classifyEntryNameForDisk(rawName);
        if (verdict.safe) continue;
        excludedRawNames.push_back(rawName);
        //披露名截断到可读长度（敌意名可上万字节；原始字节展示无意义）。走
        //共享层 UTF-8 边界回退口径：纯字节切可落进多字节序列中间产出非法
        //UTF-8，渲染期经 Utf8StringBuilder 软失败会把该条披露吞成空行。
        report.skipped.push_back({truncateUtf8(canonical, kSkippedDisplayLimit),
                                  verdict.reason});
    }
    std::sort(report.skipped.begin(), report.skipped.end(),
              [](const SkippedEntry& left, const SkippedEntry& right) {
                  return left.entryName < right.entryName;
              });

    //后端输入：无敌意名时原 jar 直跑（零暂存成本）；有则字节级快照剔除。
    std::string backendInput = jarPath;
    if (!excludedRawNames.empty()) {
        const auto stagedJar = temporary.path() / "staged-input.jar";
        auto       staged    = stageJarWithoutEntries(jarPath, stagedJar, excludedRawNames);
        if (staged.is_err()) return ca::Err(std::move(staged).unwrap_err());
        report.stagedEntries = staged.unwrap();
        backendInput         = stagedJar.u8string();
    }
    else {
        report.stagedEntries = entries.size();
    }

    DecompileRequest request = buildRequest(options);
    request.inputPath        = backendInput;
    request.outputPath       = outputPath;
    request.tempRoot         = temporary.path().u8string();
    auto decompiled          = decompile(request);
    if (decompiled.is_err()) return ca::Err(mapStatusError(std::move(decompiled).unwrap_err()));
    report.decompilerJar = std::move(decompiled).unwrap().decompilerJar;
    report.backend       = decompilerKindId(options.kind);
    return ca::Ok(std::move(report));
}

void renderSingleDecompileSummary(const SingleDecompileReport& report, OutputSink& sink)
{
    sink.puts(shared::trfStr("mjt.cli.decompile.single_done", {report.internalName}));
    sink.puts(shared::trfStr("mjt.cli.decompile.backend_line",
                             {report.backend, report.decompilerJar}));
}

void renderBatchDecompileSummary(const BatchDecompileReport& report, OutputSink& sink)
{
    sink.puts(shared::trfStr("mjt.cli.decompile.backend_line",
                             {report.backend, report.decompilerJar}));
    sink.puts(shared::trfStr("mjt.cli.decompile.batch_classes",
                             {std::to_string(report.classEntries)}));
    sink.puts(shared::trfStr(report.zipOutput ? "mjt.cli.decompile.batch_done_zip"
                                             : "mjt.cli.decompile.batch_done_dir",
                             {report.outputPath}));
}

void renderSkippedEntries(const BatchDecompileReport& report, OutputSink& sink)
{
    if (report.skipped.empty()) return;
    sink.puts(shared::trfStr("mjt.cli.decompile.batch_skipped",
                             {std::to_string(report.skipped.size())}));
    for (const auto& skipped : report.skipped)
        sink.puts(shared::trfStr("mjt.cli.decompile.batch_skipped_reason",
                                 {skipped.entryName, skipped.reason}));
    sink.puts(shared::trfStr("mjt.cli.decompile.batch_skipped_hint"));
}

}   // namespace mjt::cli
