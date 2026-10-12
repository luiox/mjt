#include "decompiler_support.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <system_error>
#include <utility>

#include "limits.hpp"

namespace mjt::mcp::detail {

namespace fs = std::filesystem;

using mjt::base::workspace::IWorkspace;

ScopedTemporaryDirectory::ScopedTemporaryDirectory(fs::path path)
    : path_(std::move(path))
{}

ScopedTemporaryDirectory::~ScopedTemporaryDirectory()
{
    std::error_code error;
    fs::remove_all(path_, error);
}

const fs::path& ScopedTemporaryDirectory::path() const noexcept
{
    return path_;
}

ca::StatusOr<fs::path> createDecompilerTemporaryDirectory()
{
    static std::atomic<ca::u64> sequence{0};
    std::error_code             error;
    const auto root = fs::temp_directory_path(error);
    if (error)
        return ca::Err(ca::ErrStatus(ca::StatusCode::INTERNAL,
                                     "无法确定临时目录: " + error.message()));

    for (ca::usize attempt = 0; attempt < 16; ++attempt) {
        const auto name = "mjt-mcp-decompile-" +
                          std::to_string(std::chrono::steady_clock::now()
                                             .time_since_epoch()
                                             .count()) +
                          "-" + std::to_string(sequence.fetch_add(1));
        const auto path = root / name;
        error.clear();
        if (fs::create_directory(path, error)) return ca::Ok(path);
        if (error && error != std::errc::file_exists) {
            return ca::Err(ca::ErrStatus(ca::StatusCode::PERMISSION_DENIED,
                                         "无法创建反编译临时目录: " + error.message()));
        }
    }
    return ca::Err(ca::ErrStatus(ca::StatusCode::ALREADY_EXISTS,
                                 "无法创建唯一的反编译临时目录"));
}

bool isSafeRelativePath(const fs::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name()) return false;
    for (const auto& component : path) {
        if (component == "..") return false;
    }
    return true;
}

ca::Status validateDecompilerSettings(const DecompilerSessionSettings& settings)
{
    auto kind = parseDecompilerKind(settings.backend);
    if (kind.is_err()) return kind.unwrap_err();
    if (settings.timeoutMilliseconds <= 0 ||
        settings.timeoutMilliseconds > kMaximumDecompilerTimeoutMilliseconds) {
        return ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                             "timeout_ms must be between 1 and " +
                                 std::to_string(kMaximumDecompilerTimeoutMilliseconds));
    }
    if (settings.maxMemoryMiB <= 0 || settings.maxMemoryMiB > kMaximumDecompilerMemoryMiB) {
        return ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                             "max_memory_mib must be between 1 and " +
                                 std::to_string(kMaximumDecompilerMemoryMiB));
    }
    if (settings.maxSourceBytes == 0 || settings.maxSourceBytes > kMaximumDecompilerSourceBytes) {
        return ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                             "max_source_bytes must be between 1 and " +
                                 std::to_string(kMaximumDecompilerSourceBytes));
    }
    return ca::OkStatus();
}

ca::Status parseDecompilerSettings(const Json& arguments,
                                   const DecompilerSessionSettings& current,
                                   DecompilerSessionSettings& output)
{
    output = current;
    output.backend = arguments.value("backend", output.backend);
    output.cfrJar = arguments.value("cfr_jar", output.cfrJar);
    output.vineflowerJar = arguments.value("vineflower_jar", output.vineflowerJar);
    output.javaProgram = arguments.value("java", output.javaProgram);
    output.timeoutMilliseconds = arguments.value("timeout_ms", output.timeoutMilliseconds);
    output.maxMemoryMiB = arguments.value("max_memory_mib", output.maxMemoryMiB);
    output.maxSourceBytes = arguments.value("max_source_bytes", output.maxSourceBytes);
    return validateDecompilerSettings(output);
}

ca::Status parseDecompilerOptions(const Json& arguments,
                                  const DecompilerSessionSettings& settings,
                                  DecompilerOptions& options)
{
    const auto name = arguments.value("decompiler", settings.backend);
    auto kind = parseDecompilerKind(name);
    if (kind.is_err()) return kind.unwrap_err();

    DecompilerSessionSettings effective = settings;
    effective.backend = name;
    effective.javaProgram = arguments.value("java", effective.javaProgram);
    effective.timeoutMilliseconds = arguments.value("timeout_ms", effective.timeoutMilliseconds);
    effective.maxMemoryMiB = arguments.value("max_memory_mib", effective.maxMemoryMiB);
    effective.maxSourceBytes = arguments.value("max_source_bytes", effective.maxSourceBytes);
    auto validated = validateDecompilerSettings(effective);
    if (validated.is_err()) return validated;

    options.kind          = kind.unwrap();
    options.decompilerJar = arguments.value(
        "decompiler_jar",
        options.kind == DecompilerKind::Cfr ? settings.cfrJar : settings.vineflowerJar);
    options.javaProgram   = effective.javaProgram;
    options.timeout       = std::chrono::milliseconds(effective.timeoutMilliseconds);
    options.maxMemoryMiB  = effective.maxMemoryMiB;
    options.maxSourceBytes = effective.maxSourceBytes;
    return ca::OkStatus();
}

Json decompilerSettingsJson(const DecompilerSessionSettings& settings)
{
    return {{"backend", settings.backend},
            {"cfr_jar", settings.cfrJar},
            {"vineflower_jar", settings.vineflowerJar},
            {"java", settings.javaProgram},
            {"timeout_ms", settings.timeoutMilliseconds},
            {"max_memory_mib", settings.maxMemoryMiB},
            {"max_source_bytes", settings.maxSourceBytes}};
}

Json decompilerAvailabilityJson(const DecompilerAvailability& availability,
                                const DecompilerSessionSettings& settings)
{
    return {{"id", availability.id},
            {"name", availability.name},
            {"active", availability.id == settings.backend},
            {"environment", availability.environmentVariable},
            {"decompiler_jar", availability.decompilerJar},
            {"java", availability.javaProgram},
            {"artifact_available", availability.artifactAvailable},
            {"java_available", availability.javaAvailable},
            {"available", availability.artifactAvailable && availability.javaAvailable},
            {"version", ""},
            {"unavailable_reason", availability.unavailableReason}};
}

Json decompilerAvailabilityList(const DecompilerSessionSettings& settings)
{
    Json backends = Json::array();
    backends.push_back(decompilerAvailabilityJson(
        inspectDecompiler(DecompilerKind::Cfr, settings.cfrJar, settings.javaProgram), settings));
    backends.push_back(decompilerAvailabilityJson(
        inspectDecompiler(DecompilerKind::Vineflower,
                          settings.vineflowerJar,
                          settings.javaProgram),
        settings));
    return backends;
}

ca::StatusOr<std::string> readDecompilerSource(const fs::path& path, ca::usize maximumBytes)
{
    //不做"先 stat 后读"：stat 与读取之间文件可被替换或增长（TOCTOU），size 校验
    //拦不住读取时的实际内容。改为按块读取，累计字节数超过 maximumBytes 立即失败，
    //超限判定与读到的数据来自同一数据流。
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return ca::Err(ca::ErrStatus(ca::StatusCode::NOT_FOUND,
                                     "无法打开反编译源码: " + path.string()));

    constexpr ca::usize kReadChunkBytes = 64 * 1024;
    std::string         content;
    content.reserve(std::min(maximumBytes, kReadChunkBytes));
    char chunk[kReadChunkBytes];
    while (true) {
        input.read(chunk, sizeof(chunk));
        if (input.bad()) {
            return ca::Err(ca::ErrStatus(ca::StatusCode::INTERNAL,
                                         "无法读取反编译源码: " + path.string()));
        }
        const auto got = static_cast<ca::usize>(input.gcount());
        if (got > 0) {
            if (content.size() + got > maximumBytes) {
                return ca::Err(ca::ErrStatus(ca::StatusCode::RESOURCE_EXHAUSTED,
                                             "反编译源码超过返回上限 " +
                                                 std::to_string(maximumBytes) + " bytes"));
            }
            content.append(chunk, got);
        }
        if (input.eof()) break;
        if (input.fail()) {
            return ca::Err(ca::ErrStatus(ca::StatusCode::INTERNAL,
                                         "无法读取反编译源码: " + path.string()));
        }
    }
    return ca::Ok(std::move(content));
}

ca::StatusOr<DecompiledSource> decompileSnapshotClass(
    const fs::path& snapshot, const std::string& internalName, const DecompilerOptions& options,
    const std::function<bool()>& cancellationRequested)
{
    //internalName 为类内部名（jar 符号表 UTF-8 家族，MCP JSON 面同域）：
    //显式 u8path（u8path 清扫），与反编译产物树的 UTF-8 落盘名对齐，不再经
    //ACP 隐式窄转码。
    const auto sourceRelative = fs::u8path(internalName + ".java");
    if (!isSafeRelativePath(sourceRelative)) {
        return ca::Err(ca::ErrStatus(ca::StatusCode::INVALID_ARGUMENT,
                                     "class name cannot be used as a source path: " + internalName));
    }

    auto temporary = createDecompilerTemporaryDirectory();
    if (temporary.is_err()) return ca::Err(temporary.unwrap_err());
    ScopedTemporaryDirectory directory(std::move(temporary).unwrap());

    const auto sourceRoot = directory.path() / "sources";
    DecompileRequest request;
    request.inputPath     = snapshot.string();
    request.outputPath    = sourceRoot.string();
    request.kind          = options.kind;
    request.decompilerJar = options.decompilerJar;
    request.javaProgram   = options.javaProgram;
    request.timeout       = options.timeout;
    request.maxMemoryMiB  = options.maxMemoryMiB;
    request.tempRoot      = directory.path().string();
    request.cancellationRequested = cancellationRequested;
    auto decompiled = decompile(request);
    if (decompiled.is_err()) return ca::Err(decompiled.unwrap_err());

    const auto sourcePath = sourceRoot / sourceRelative;
    std::error_code error;
    if (!fs::is_regular_file(sourcePath, error) || error) {
        return ca::Err(ca::ErrStatus(ca::StatusCode::NOT_FOUND,
                                     "反编译器未生成目标 class 的源码: " + internalName));
    }
    auto source = readDecompilerSource(sourcePath, options.maxSourceBytes);
    if (source.is_err()) return ca::Err(source.unwrap_err());

    return ca::Ok(DecompiledSource{internalName,
                                   options.kind == DecompilerKind::Cfr ? "cfr" : "vineflower",
                                   std::move(decompiled).unwrap().decompilerJar,
                                   std::move(source).unwrap()});
}

ca::StatusOr<DecompiledSource> decompileWorkspaceClass(IWorkspace& workspace,
                                                        const std::string& className,
                                                        const DecompilerOptions& options)
{
    auto info = workspace.classInfo(className);
    if (info.is_err()) return ca::Err(info.unwrap_err());
    const auto internalName = info.unwrap().internalName;

    auto temporary = createDecompilerTemporaryDirectory();
    if (temporary.is_err()) return ca::Err(temporary.unwrap_err());
    ScopedTemporaryDirectory directory(std::move(temporary).unwrap());

    const auto snapshot = directory.path() / "workspace.jar";
    auto exportStatus = workspace.exportJar(snapshot.string());
    if (exportStatus.is_err()) return ca::Err(std::move(exportStatus));
    return decompileSnapshotClass(snapshot, internalName, options);
}

Json decompiledSourceJson(const DecompiledSource& result, const char* sourceScope)
{
    return {{"internal_name", result.internalName},
            {"backend", result.backend},
            {"decompiler_jar", result.decompilerJar},
            {"source_scope", sourceScope},
            {"source", result.source}};
}

}   //namespace mjt::mcp::detail
