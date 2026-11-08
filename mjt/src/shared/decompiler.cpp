#include "shared/decompiler.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "libca/process/subprocess.hpp"
#include "libca/thread/thread.hpp"
#include "libca/zip/file.hpp"

#include "shared/jar_remapper.hpp"
#include "shared/i18n_errors.hpp"
#include "shared/text_codec.hpp"

#if defined(_WIN32)
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

#include "libca/str/utf16_string.hpp"

namespace mjt {
namespace {

namespace fs = std::filesystem;

struct ProcessResult
{
    ca::i32     exitCode = -1;
    std::string stdoutText;
    std::string stderrText;
};

std::string environment(const char* name)
{
#if defined(_WIN32)
    char*  value  = nullptr;
    size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) return {};
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string{} : std::string(value);
#endif
}

bool isRegularFile(const fs::path& path)
{
    std::error_code error;
    return fs::is_regular_file(path, error);
}

bool isProgramAvailable(const std::string& program)
{
    if (program.empty()) return false;
    //program 持用户指定路径（--java argv，UTF-8 交付契约；u8path 清扫），
    //显式 u8path 与 条目名边界同款，不经 ACP 隐式窄转码。
    const fs::path path = fs::u8path(program);
    if (path.has_parent_path() || path.has_root_name()) return isRegularFile(path);

    const auto searchPath = environment("PATH");
    if (searchPath.empty()) return false;
#if defined(_WIN32)
    constexpr char kSeparator = ';';
#else
    constexpr char kSeparator = ':';
#endif
    std::size_t start = 0;
    while (start <= searchPath.size()) {
        const auto end = searchPath.find(kSeparator, start);
        const auto directory = searchPath.substr(start,
                                                 end == std::string::npos
                                                     ? std::string::npos
                                                     : end - start);
        if (!directory.empty()) {
            //directory 来自环境变量 PATH（本机 ANSI 字节，native 口径自洽），
            //保持隐式窄构造；program 是用户值（UTF-8 契约），已按 u8path 构造。
            auto candidate = fs::path(directory) / program;
            if (isRegularFile(candidate)) return true;
#if defined(_WIN32)
            if (candidate.extension().empty()) {
                candidate += ".exe";
                if (isRegularFile(candidate)) return true;
            }
#endif
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return false;
}

std::string decompilerName(DecompilerKind kind)
{
    //jcdc 按上游品牌小写书写；CFR / Vineflower 沿用既有展示名。
    switch (kind) {
        case DecompilerKind::Cfr: return "CFR";
        case DecompilerKind::Vineflower: return "Vineflower";
        case DecompilerKind::Jcdc: return "jcdc";
    }
    return "CFR";
}

//反编译器产物发现：五层 fallback，优先级从高到低。
//1. --decompiler-jar 显式参数
//2. MJT_CFR_JAR / MJT_VINEFLOWER_JAR / MJT_JCDC_EXE 环境变量（按 kind 选择）
//3. MJT_DECOMPILER_HOME 下的候选文件名（cfr 单候选；vineflower 两个候选，
//因发布件有 fat 与非 fat 两种命名；jcdc 是可执行文件，按平台取 jcdc.exe /
//jcdc——带版本的发布件文件名由第 4 层扫描兜住）
//4. mjt(.exe) 同级的 package 布局（mjt-mcp 发布包 / 开发树 artifact 目录）：
//<exe_dir>/package 与 <exe_dir>/../package 按「前缀 + .jar」探测（jcdc 按
//「前缀 jcdc + 可执行件规则」）——发布件
//文件名带版本（cfr-0.152.jar / vineflower-1.12.0.jar / jcdc-v0.1.4-…），固定名候选不中，
//故做目录扫描；按文件名字典序取首个命中保证确定性（-sources/-javadoc
//附属 jar 除外，N5 守卫）
//5. 都没命中时 jar 为空，probed 带全部已探测候选（调用方拼进报错文案）

//反编译器 → 显式环境变量名（发现序第 2 层与「产物未找到」报错文案同一来源）。
const char* decompilerEnvironmentVariable(DecompilerKind kind)
{
    switch (kind) {
        case DecompilerKind::Cfr: return "MJT_CFR_JAR";
        case DecompilerKind::Vineflower: return "MJT_VINEFLOWER_JAR";
        case DecompilerKind::Jcdc: return "MJT_JCDC_EXE";
    }
    return "MJT_CFR_JAR";
}

//MJT_DECOMPILER_HOME 固定名候选（发现序第 3 层）。jcdc 是可执行文件：Windows
//发布件带 .exe，其余平台发布件是无扩展名二进制；固定名假定用户已把发布件
//落成 jcdc(.exe)，带版本文件名由第 4 层目录扫描兜住。
std::vector<std::string> decompilerHomeCandidates(DecompilerKind kind)
{
    switch (kind) {
        case DecompilerKind::Cfr: return {"cfr.jar"};
        case DecompilerKind::Vineflower: return {"vineflower.jar", "vineflower-fat.jar"};
        case DecompilerKind::Jcdc: break;
    }
#if defined(_WIN32)
    return {"jcdc.exe"};
#else
    return {"jcdc"};
#endif
}

std::string asciiLower(std::string text)
{
    for (auto& character : text) {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return text;
}

//单目录内按「前缀 + .jar」探测（ASCII 小写比较，文件名字典序取首个命中）。
//N5 守卫：-sources.jar / -javadoc.jar 是源码/文档附属 jar，不是可运行的反编译器；
//前缀扫描对它们同样命中（如 vineflower-sources.jar）且 '-' 字典序靠前时会误选，
//故显式排除；目录内只剩附属件时不命中（不兜回误选）。
std::string probeDecompilerJarDirectory(const fs::path& directory, DecompilerKind kind)
{
    static constexpr const char* kExcludedSuffixes[] = {"-sources.jar", "-javadoc.jar"};
    //Jcdc 不消费 JAR 探测（走 probeDecompilerExecutableDirectory）；防御性早退，
    //避免可执行件扫描口径被误判成 .jar（kind 走错分支时宁可不命中）。
    if (kind == DecompilerKind::Jcdc) return {};
    std::error_code error;
    if (!fs::is_directory(directory, error) || error) return {};
    const std::string prefix = kind == DecompilerKind::Cfr ? "cfr" : "vineflower";
    std::vector<std::string> hits;
    for (fs::directory_iterator it(directory,
                                   fs::directory_options::skip_permission_denied,
                                   error),
         end;
         it != end && !error;
         it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const auto name = it->path().filename().string();
        const auto lower = asciiLower(name);
        if (!(lower.size() >= prefix.size() + 4 && lower.rfind(prefix, 0) == 0 &&
              lower.compare(lower.size() - 4, 4, ".jar") == 0)) {
            continue;
        }
        bool excluded = false;
        for (const auto* suffix : kExcludedSuffixes) {
            const auto length = std::char_traits<char>::length(suffix);
            if (lower.size() >= length &&
                lower.compare(lower.size() - length, length, suffix) == 0) {
                excluded = true;
                break;
            }
        }
        if (excluded) continue;
        hits.push_back(name);
    }
    if (hits.empty()) return {};
    std::sort(hits.begin(), hits.end());
    //..\package 形态的目录归一（报错文案与命中路径可读，且与用户视角一致）。
    return (directory / hits.front()).lexically_normal().string();
}

//jcdc 可执行文件单目录探测（与 probeDecompilerJarDirectory 同口径：前缀 + ASCII
//小写比较、文件名字典序取首个命中）。
//规则按平台分：Windows 发布件是 .exe（jcdc-v<version>-windows-*.exe），必须
//后缀 .exe（同前缀的 .exe.sha256 等旁文件天然出局）；非 Windows 发布件是无
//扩展名二进制（jcdc-v<version>-linux-amd64），做不了扩展名要求，改为排除
//可能同前缀的校验和/文档/归档旁文件。
std::string probeDecompilerExecutableDirectory(const fs::path& directory, DecompilerKind kind)
{
    if (kind != DecompilerKind::Jcdc) return {};
    static constexpr const char* kExcludedSuffixes[] = {".jar",  ".md",    ".txt",
                                                        ".sha256", ".sig", ".asc",
                                                        ".zip",   ".json",  ".toml",
                                                        ".yml",   ".yaml"};
    std::error_code error;
    if (!fs::is_directory(directory, error) || error) return {};
    std::vector<std::string> hits;
    for (fs::directory_iterator it(directory,
                                   fs::directory_options::skip_permission_denied,
                                   error),
         end;
         it != end && !error;
         it.increment(error)) {
        if (!it->is_regular_file(error)) continue;
        const auto name = it->path().filename().string();
        const auto lower = asciiLower(name);
        //前缀 "jcdc" 且至少还有一字符（纯 "jcdc" 不是带版本的发布件文件名，
        //归第 3 层固定名候选）。
        if (lower.size() <= 4 || lower.rfind("jcdc", 0) != 0) continue;
        bool excluded = false;
        for (const auto* suffix : kExcludedSuffixes) {
            const auto length = std::char_traits<char>::length(suffix);
            if (lower.size() >= length &&
                lower.compare(lower.size() - length, length, suffix) == 0) {
                excluded = true;
                break;
            }
        }
        if (excluded) continue;
#if defined(_WIN32)
        //Windows 发布件恒为 .exe：同前缀无 .exe 后缀的文件（文档、裸二进制、
        //`.exe.bak` 等）一律出局。
        if (lower.size() < 8 || lower.compare(lower.size() - 4, 4, ".exe") != 0) continue;
#endif
        hits.push_back(name);
    }
    if (hits.empty()) return {};
    std::sort(hits.begin(), hits.end());
    return (directory / hits.front()).lexically_normal().string();
}

//当前进程可执行文件绝对路径。libca 的 env 模块（executable_path）未进全仓
//modules 链接闭包，这里本地实现同款查询；失败返回空。
std::string currentExecutablePath()
{
#if defined(_WIN32)
    std::wstring wide(MAX_PATH, L'\0');
    for (;;) {
        const auto size = GetModuleFileNameW(nullptr, wide.data(),
                                             static_cast<DWORD>(wide.size()));
        if (size == 0) return {};
        if (size < wide.size()) {
            wide.resize(size);
            break;
        }
        wide.resize(wide.size() * 2);
    }
    try {
        const auto utf8 = ca::str::Utf16String(
            reinterpret_cast<const ca::u16*>(wide.data()), wide.size());
        return utf8.to_utf8_string().to_std_string();
    }
    catch (const std::exception&) {
        return {};
    }
#elif defined(__linux__)
    std::string link(256, '\0');
    while (true) {
        const auto written = ::readlink("/proc/self/exe", link.data(), link.size());
        if (written < 0) return {};
        if (static_cast<std::size_t>(written) < link.size()) {
            link.resize(static_cast<std::size_t>(written));
            break;
        }
        link.resize(link.size() * 2);
    }
    return link;
#else
    return {};
#endif
}

}   //namespace (anonymous)

//discoverDecompilerJarDetailed 为 decompiler.hpp 导出面（mjt_unittest 注入
//exePath 直测探测层），必须外部链接；其余 helper 留在上方匿名命名空间。

DecompilerJarDiscovery discoverDecompilerJarDetailed(const DecompileRequest& request,
                                                     const std::string& executablePath)
{
    DecompilerJarDiscovery discovery;
    if (!request.decompilerJar.empty()) {
        discovery.jar = request.decompilerJar;
        discovery.probed.push_back(request.decompilerJar);
        return discovery;
    }

    const char* variable = decompilerEnvironmentVariable(request.kind);
    auto explicitPath = environment(variable);
    if (!explicitPath.empty()) {
        discovery.jar = explicitPath;
        discovery.probed.push_back(explicitPath);
        return discovery;
    }

    auto home = environment("MJT_DECOMPILER_HOME");
    const std::vector<std::string> candidates = decompilerHomeCandidates(request.kind);
    if (!home.empty()) {
        for (const auto& candidate : candidates) {
            auto path = fs::path(home) / candidate;
            discovery.probed.push_back(path.string());
            if (discovery.jar.empty() && isRegularFile(path)) discovery.jar = path.string();
        }
        if (!discovery.jar.empty()) return discovery;
    }

    const auto exe = executablePath.empty() ? currentExecutablePath() : executablePath;
    if (!exe.empty()) {
        //exe 为宽字符 API（GetModuleFileNameW）→ UTF-8 显式转换产物（或 unittest
        //注入的 UTF-8 路径），u8path 语义与之同源；隐式窄构造在非 UTF-8 ACP 进程
        //会把非 ASCII 安装目录转成 mojibake，导致 package 探测永不命中。
        const auto exeDirectory = fs::u8path(exe).parent_path();
        for (const auto& packageDirectory :
             {exeDirectory / "package", exeDirectory / ".." / "package"}) {
            const auto normalized = packageDirectory.lexically_normal();
            discovery.probed.push_back(normalized.string());
            if (discovery.jar.empty())
                discovery.jar = request.kind == DecompilerKind::Jcdc
                                    ? probeDecompilerExecutableDirectory(normalized, request.kind)
                                    : probeDecompilerJarDirectory(normalized, request.kind);
        }
        if (!discovery.jar.empty()) return discovery;
    }

    //兜底（既有行为）：home 层首个候选路径，让不可用展示与报错仍有具体路径
    //可指；home 为空时该值同为空。
    if (!home.empty()) discovery.jar = (fs::path(home) / candidates.front()).string();
    return discovery;
}

std::string truncateUtf8(const std::string& text, std::size_t limit)
{
    if (text.size() <= limit) return text;
    //不得在多字节序列中间截断：从 limit 位置向前回退到 UTF-8 首字节。
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
    return text.substr(0, cut) + "…";
}

namespace {

std::string discoverDecompilerJar(const DecompileRequest& request)
{
    return discoverDecompilerJarDetailed(request).jar;
}

//Java 可执行程序发现：四层 fallback，优先级从高到低。
//1. --java 显式参数
//2. MJT_JAVA 环境变量
//3. JAVA_HOME/bin/java（[.exe]）（仅当文件存在时采用，避免 JAVA_HOME 指向 JRE 等）
//4. PATH 上的 "java"（交给系统查找）
std::string discoverJava(const DecompileRequest& request)
{
    if (!request.javaProgram.empty()) return request.javaProgram;
    auto java = environment("MJT_JAVA");
    if (!java.empty()) return java;

    auto javaHome = environment("JAVA_HOME");
    if (!javaHome.empty()) {
#if defined(_WIN32)
        auto candidate = fs::path(javaHome) / "bin" / "java.exe";
#else
        auto candidate = fs::path(javaHome) / "bin" / "java";
#endif
        if (isRegularFile(candidate)) return candidate.string();
    }
    return "java";
}

fs::path createTempDirectory(const DecompileRequest& request)
{
    static std::atomic<ca::u64> sequence{0};
    //tempRoot 持 argv --temp-dir（UTF-8 契约；中文用户名系统临时目录同形），
    //显式 u8path；缺省分支 fs::temp_directory_path 本就是宽字符精确面。
    fs::path                    root =
        request.tempRoot.empty() ? fs::temp_directory_path() : fs::u8path(request.tempRoot);
    fs::create_directories(root);
    const auto id = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                    "-" + std::to_string(sequence.fetch_add(1));
    auto path = root / ("mjt-decompile-" + id);
    fs::create_directories(path);
    return path;
}

class TempDirectory
{
public:
    explicit TempDirectory(fs::path path)
        : path_(std::move(path))
    {}
    ~TempDirectory()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

ca::StatusOr<ProcessResult> runProcess(const std::string&              program,
                                       const std::vector<std::string>& arguments,
                                       std::chrono::milliseconds       timeout,
                                       const std::function<bool()>&    cancellationRequested)
{
    ca::process::Command command(program);
    command.args(arguments)
        .stdin(ca::process::Stdio::null())
        .stdout(ca::process::Stdio::piped())
        .stderr(ca::process::Stdio::piped());

    auto spawned = command.spawn();
    if (spawned.is_err()) return ca::Err(spawned.unwrap_err());

    auto child  = std::move(spawned).unwrap();
    auto output = child.take_stdout();
    auto error  = child.take_stderr();
    if (!output.has_value() || !error.has_value()) {
        (void)child.kill();
        (void)child.wait();
        return ca::Err(shared::errStatus(ca::StatusCode::INTERNAL, "mjt.shared.err.pipe"));
    }

    std::string stdoutText;
    std::string stderrText;
    ca::Status  stdoutStatus = ca::OkStatus();
    ca::Status  stderrStatus = ca::OkStatus();
    //不能先 wait 再读管道：反编译器输出超过管道容量时，子进程会等待 host 读取，
    //host 又在等待子进程退出，双方形成死锁。两个 reader 必须与超时等待并行运行；
    //超时或 wait 失败时先终止并回收子进程，让管道关闭后再 join reader。
    auto stdoutStarted = ca::thread::Thread::start([&] {
        auto result = output->read_to_end();
        if (result.is_err())
            stdoutStatus = result.unwrap_err();
        else
            stdoutText = std::move(result).unwrap();
    });
    if (stdoutStarted.is_err()) {
        (void)child.kill();
        (void)child.wait();
        return ca::Err(stdoutStarted.unwrap_err());
    }
    auto stdoutReader = std::move(stdoutStarted).unwrap();

    auto stderrStarted = ca::thread::Thread::start([&] {
        auto result = error->read_to_end();
        if (result.is_err())
            stderrStatus = result.unwrap_err();
        else
            stderrText = std::move(result).unwrap();
    });
    if (stderrStarted.is_err()) {
        //stderr 无人读取时子进程可能阻塞在满管道上；必须先结束子进程并关闭管道，
        //再等待已经启动的 stdout reader，不能依赖 Thread 析构时直接 join。
        (void)child.kill();
        (void)child.wait();
        (void)stdoutReader.join();
        return ca::Err(stderrStarted.unwrap_err());
    }
    auto stderrReader = std::move(stderrStarted).unwrap();

    //wait_for 返回三态：ok+has_value=正常退出（拿到 exit code）；ok+无 value=仍在运行；
    //err=系统错误。以短周期等待同时观察取消请求；超时、取消或错误都先 kill 再 wait，
    //确保子进程不残留、管道能排空，否则 reader 线程 join 会死锁。
    constexpr auto kCancellationPollInterval = std::chrono::milliseconds(50);
    auto           remaining                 = timeout;
    std::optional<ca::process::ExitStatus> exitStatus;
    ca::Status                         waitStatus = ca::OkStatus();
    bool                               timedOut   = false;
    bool                               cancelled  = false;
    while (!exitStatus.has_value() && !timedOut && !cancelled && waitStatus.is_ok()) {
        if (cancellationRequested && cancellationRequested()) {
            cancelled = true;
            break;
        }
        const auto waitSlice = std::min(remaining, kCancellationPollInterval);
        auto waited = child.wait_for(waitSlice);
        if (waited.is_err()) {
            waitStatus = waited.unwrap_err();
            break;
        }
        if (waited.unwrap().has_value()) {
            exitStatus = waited.unwrap();
            break;
        }
        remaining -= waitSlice;
        timedOut = remaining <= std::chrono::milliseconds::zero();
    }
    if (timedOut || cancelled || waitStatus.is_err()) {
        (void)child.kill();
        (void)child.wait();
    }
    auto stdoutJoin = stdoutReader.join();
    auto stderrJoin = stderrReader.join();

    if (waitStatus.is_err()) return ca::Err(std::move(waitStatus));
    if (cancelled)
        return ca::Err(shared::errStatus(ca::StatusCode::CANCELLED, "mjt.shared.err.cancelled"));
    if (timedOut) {
        return ca::Err(
            shared::errStatus(ca::StatusCode::DEADLINE_EXCEEDED, "mjt.shared.err.timeout",
                              {std::to_string(timeout.count())}));
    }
    if (stdoutJoin.is_err()) return ca::Err(std::move(stdoutJoin));
    if (stderrJoin.is_err()) return ca::Err(std::move(stderrJoin));
    if (stdoutStatus.is_err()) return ca::Err(std::move(stdoutStatus));
    if (stderrStatus.is_err()) return ca::Err(std::move(stderrStatus));

    ProcessResult result;
    result.exitCode   = exitStatus->code;
    result.stdoutText = std::move(stdoutText);
    result.stderrText = std::move(stderrText);
    return ca::Ok(std::move(result));
}

std::vector<fs::path> findJavaFiles(const fs::path& root)
{
    std::vector<fs::path> files;
    std::error_code       error;
    for (fs::recursive_directory_iterator it(root, error), end; it != end && !error;
         it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == ".java") {
            files.push_back(it->path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

//── 子进程失败诊断（反馈 bug1：vineflower 静默 rc=1）──────────────────
//
//java 子进程在中文 Windows 上以 GBK 输出本地化异常文案（JDK17 重定向 stderr
//的默认字符集跟随平台）。原始 GBK 字节拼进错误消息后，CLI 端经
//ca::i18n::trf 渲染时 Utf8StringBuilder::build_or_empty 对非法 UTF-8 软失败
//成空串——用户只看到一个换行、退出码 1，即"静默失败"。因此诊断文本必须先
//经 text::toCanonicalUtf8 规范化（合法 UTF-8 原样、GBK 恢复、残字节 U+FFFD），
//再做根因摘要与可行动提示。

constexpr std::size_t kCauseSummaryLimit = 400;    //根因行截断上限（字节）
constexpr std::size_t kOffenderLimit     = 96;     //肇事文件名截断上限（字节）

///取诊断文本中优先级最高的根因行：JVM 堆栈的真实根因是最后一个
///"Caused by: " 行；没有堆栈形态时退回首个非空行。
std::string extractRootCauseLine(const std::string& diagnostic)
{
    std::size_t causeBegin = std::string::npos;
    for (std::size_t pos = diagnostic.find("Caused by: "); pos != std::string::npos;
         pos              = diagnostic.find("Caused by: ", pos + 1)) {
        causeBegin = pos;
    }
    if (causeBegin == std::string::npos) {
        //首个非空行。
        std::size_t start = diagnostic.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return {};
        std::size_t end = diagnostic.find_first_of("\r\n", start);
        if (end == std::string::npos) end = diagnostic.size();
        return diagnostic.substr(start, end - start);
    }
    std::size_t end = diagnostic.find_first_of("\r\n", causeBegin);
    if (end == std::string::npos) end = diagnostic.size();
    return diagnostic.substr(causeBegin, end - causeBegin);
}

///从 FileSystemException 行提取肇事文件名（"java.nio.file.FileSystemException:
///<path>: <reason>" 的 <path> 基名），供错误文案点名；提取失败返回空。
std::string extractOffenderName(const std::string& diagnostic)
{
    constexpr const char* kMarker = "FileSystemException: ";
    const auto            marker  = diagnostic.find(kMarker);
    if (marker == std::string::npos) return {};
    const std::size_t start = marker + std::strlen(kMarker);
    const std::size_t end   = diagnostic.find(": ", start);
    if (end == std::string::npos) return {};
    std::string path = diagnostic.substr(start, end - start);
    //去掉 sz>0 场景的引号形态（部分 JDK 变体带引号包裹路径）。
    if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
        path = path.substr(1, path.size() - 2);
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string::npos) path = path.substr(slash + 1);
    return path;
}

///组装非零退出的失败文案：根因摘要 + （检出长名写入失败时的）zip 输出绕过
///提示。前提：diagnostic 已规范化为合法 UTF-8，文案不再被 trf 软失败吞掉。
std::string composeExecFailure(DecompilerKind       kind,
                               ca::i32              exitCode,
                               const std::string&   stdoutText,
                               const std::string&   stderrText,
                               const std::string&   inputName)
{
    const std::string& raw      = stderrText.empty() ? stdoutText : stderrText;
    const std::string  diagnostic = shared::text::toCanonicalUtf8(raw);

    std::string message = shared::trfStr("mjt.shared.err.exec_failed",
                                         {decompilerName(kind), std::to_string(exitCode)});
    auto        cause   = extractRootCauseLine(diagnostic);
    if (!cause.empty()) message += ": " + truncateUtf8(cause, kCauseSummaryLimit);

    if (diagnostic.find("java.nio.file.FileSystemException") != std::string::npos) {
        //Vineflower 写单个 .java 产物失败（Files.newOutputStream 一族）：超长
        //类名在 Windows 路径限制下必然命中。文案点名肇事文件并给出 zip 输出
        //绕过（反编译器对归档输出不做逐文件落盘，天然免疫路径限制）；深处
        //根治（\\?\ 前缀 + 长名改写）跟踪 。
        auto offender = extractOffenderName(diagnostic);
        if (offender.empty()) offender = inputName;
        message += "\n" + shared::trfStr("mjt.shared.err.decompile_long_name_hint",
                                         {truncateUtf8(offender, kOffenderLimit)});
    }
    return message;
}

ca::Status normalizeClassOutput(const fs::path& generatedRoot, const fs::path& input,
                                const fs::path& requestedOutput,
                                const std::string& backendSummary)
{
    auto files = findJavaFiles(generatedRoot);
    if (files.empty()) {
        std::string message = shared::trfStr("mjt.shared.err.no_java_output");
        if (!backendSummary.empty()) message += "\n" + backendSummary;
        return ca::ErrStatus(ca::StatusCode::NOT_FOUND, message);
    }

    auto       selected     = files.begin();
    const auto expectedStem = input.stem().string();
    auto       match        = std::find_if(files.begin(), files.end(), [&](const fs::path& path) {
        return path.stem().string() == expectedStem;
    });
    if (match != files.end()) selected = match;

    std::error_code error;
    if (!requestedOutput.parent_path().empty()) {
        fs::create_directories(requestedOutput.parent_path(), error);
        if (error) {
            return shared::errStatus(ca::StatusCode::PERMISSION_DENIED,
                                     "mjt.shared.err.mkdir_output", {error.message()});
        }
    }
    fs::copy_file(*selected, requestedOutput, fs::copy_options::overwrite_existing, error);
    if (error) {
        return shared::errStatus(ca::StatusCode::INTERNAL, "mjt.shared.err.write_result",
                                 {error.message()});
    }
    return ca::OkStatus();
}

}   //namespace (anonymous)

//路径穿越防护：Vineflower 会把 JAR 输入的反编译结果重新打包成同名源码 JAR，
//解包时必须拒绝绝对路径、根相对路径（如 "/abs.txt"）、带盘符/root name 的路径，
//以及任何 .. 段，否则恶意或损坏的归档条目可能写到目标目录之外（zip-slip）。
//这是安全关键检查，不能为了兼容性放宽。该 helper 同时被 mjt jar extract 等
//解包路径复用，故置于外层命名空间并经 decompiler.hpp 导出。
bool isSafeArchivePath(const fs::path& path)
{
    //注意不能只查 is_absolute：Windows 语义下它要求 root_name（如 "C:"），
    //"/abs.txt" 这类仅带 root_directory 的根相对路径会被漏判，且 outDir / uri
    //拼接时 operator/ 以其替换结果路径，直接落到盘根逃逸输出目录。
    //has_root_path = has_root_name || has_root_directory，POSIX 上
    //is_absolute 与 has_root_directory 等价，语义收紧不改变 POSIX 判定。
    if (path.empty() || path.has_root_path()) return false;
    for (const auto& component : path) {
        if (component == "..") return false;
    }
    return true;
}

//条目名 → 相对路径唯一构造点（decompiler.hpp 导出面；u8path 清扫）。
//toCanonicalUtf8 对合法 UTF-8 逐字节原样（零漂移），残字节 U+FFFD 兜底不抛，
//之后 u8path 的输入恒为合法 UTF-8——ACP 无关，与 条目名门同口径。
fs::path archiveEntryRelativePath(const std::string& entryName)
{
    return fs::u8path(shared::text::toCanonicalUtf8(entryName));
}

namespace {
ca::Status normalizeVineflowerJarOutput(const fs::path& generatedRoot,
                                        const fs::path& decompilerInput,
                                        const std::string& backendSummary)
{
    const auto archivePath = generatedRoot / decompilerInput.filename();
    if (!isRegularFile(archivePath)) return ca::OkStatus();

    //libca 窄串路径参数全库按 UTF-8 契约（FileUtil/PathUtil 头注口径），
    //显式 u8string 交付；archivePath 由 u8path 系构造，u8string 即原始字节。
    ca::zip::ZipFile archive(archivePath.u8string());
    ca::usize    extracted = 0;
    for (const auto& name : archive.entries()) {
        //条目名原始字节 → 相对路径：显式 UTF-8 语义构造（u8path 清扫），不再
        //fs::path(name) 经 ACP 隐式窄转码把合法 UTF-8 类名转 mojibake。
        const fs::path relative = archiveEntryRelativePath(name);
        if (relative.extension() != ".java" || !isSafeArchivePath(relative)) continue;

        //libca 0.0.9：ZipFile::read 错误通道由异常改为 Result<T, ZipErrorInfo>。
        //沿用本函数既有体例：失败经 errStatus 构造 ca::Status 向上传播，与既有
        //解包失败同口径（INTERNAL + unpack_vineflower；ZipErrorInfo.message 已含
        //条目名等动态详情），不新增 i18n key。
        auto            bytesResult = archive.read(name);
        if (bytesResult.is_err()) {
            return shared::errStatus(ca::StatusCode::INTERNAL, "mjt.shared.err.unpack_vineflower",
                                     {bytesResult.unwrap_err().message});
        }
        const auto&     bytes       = bytesResult.unwrap();
        auto            destination = generatedRoot / relative;
        std::error_code error;
        fs::create_directories(destination.parent_path(), error);
        if (error) {
            return shared::errStatus(ca::StatusCode::PERMISSION_DENIED,
                                     "mjt.shared.err.mkdir_vineflower", {error.message()});
        }
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            return shared::errStatus(ca::StatusCode::INTERNAL,
                                 "mjt.shared.err.unpack_vineflower", {name});
        }
        ++extracted;
    }
    archive.close();
    std::error_code error;
    fs::remove(archivePath, error);
    if (extracted == 0) {
        std::string message = shared::trfStr("mjt.shared.err.vineflower_no_java");
        if (!backendSummary.empty()) message += "\n" + backendSummary;
        return ca::ErrStatus(ca::StatusCode::NOT_FOUND, message);
    }
    return ca::OkStatus();
}

ca::Status copyJavaTree(const fs::path& generatedRoot, const fs::path& requestedOutput,
                        const std::string& backendSummary)
{
    auto files = findJavaFiles(generatedRoot);
    if (files.empty()) {
        std::string message = shared::trfStr("mjt.shared.err.no_java_output");
        if (!backendSummary.empty()) message += "\n" + backendSummary;
        return ca::ErrStatus(ca::StatusCode::NOT_FOUND, message);
    }

    for (const auto& file : files) {
        std::error_code error;
        auto            relative = fs::relative(file, generatedRoot, error);
        if (error || !isSafeArchivePath(relative)) {
            return shared::errStatus(ca::StatusCode::INTERNAL, "mjt.shared.err.relpath");
        }
        auto destination = requestedOutput / relative;
        fs::create_directories(destination.parent_path(), error);
        if (error) {
            return shared::errStatus(ca::StatusCode::PERMISSION_DENIED,
                                     "mjt.shared.err.mkdir_sources", {error.message()});
        }
        fs::copy_file(file, destination, fs::copy_options::overwrite_existing, error);
        if (error) {
            return shared::errStatus(ca::StatusCode::INTERNAL, "mjt.shared.err.write_sources",
                                     {error.message()});
        }
    }
    return ca::OkStatus();
}

//── 反编译器 JAR 自报版本提取（decompilerJarVersion 的实现件）──────────────

///@brief MANIFEST 读取上限：超出部分不参与版本解析（防超大条目拖慢发现）。
constexpr std::size_t kManifestMaximumBytes = 64 * 1024;
///@brief 版本串上限（含超长伪造值的截断兜底）。
constexpr std::size_t kVersionMaximumBytes = 64;

///@brief JAR MANIFEST 折行展开：规范按 72 字节折行，续行以单空格/制表开头。
std::vector<std::string> unfoldManifestLines(const std::string& raw)
{
    std::vector<std::string> lines;
    std::size_t              start = 0;
    while (start <= raw.size()) {
        const auto end  = raw.find('\n', start);
        auto       line = raw.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && (line.front() == ' ' || line.front() == '\t') && !lines.empty())
            lines.back() += line.substr(1);
        else
            lines.push_back(std::move(line));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

///@brief 去首尾空白并截断到版本串上限；全空白返回空串。
std::string trimVersionValue(const std::string& value)
{
    const auto isBlank = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t begin  = 0;
    std::size_t end    = value.size();
    while (begin < end && isBlank(value[begin])) ++begin;
    while (end > begin && isBlank(value[end - 1])) --end;
    const auto length = std::min(end - begin, kVersionMaximumBytes);
    return value.substr(begin, length);
}

///@brief 按 MANIFEST 键名取值；键匹配忽略冒号前空白，值取冒号后首个非空格起。
std::string manifestAttribute(const std::vector<std::string>& lines, const std::string& key)
{
    for (const auto& line : lines) {
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::size_t nameEnd = colon;
        while (nameEnd > 0 && (line[nameEnd - 1] == ' ' || line[nameEnd - 1] == '\t')) --nameEnd;
        if (line.compare(0, nameEnd, key) != 0) continue;
        std::size_t valueBegin = colon + 1;
        while (valueBegin < line.size() && (line[valueBegin] == ' ' || line[valueBegin] == '\t'))
            ++valueBegin;
        return trimVersionValue(line.substr(valueBegin));
    }
    return {};
}

///@brief JAR 文件名回退：`name-<version>.jar` 取首个以数字开头段起的余串。
///
///`vineflower-1.12.0.jar` → `1.12.0`；`fake-mcp-decompiler.jar` 无数字开头段
///→ 空串（词尾不冒充版本）。
std::string versionFromJarFileName(const fs::path& jarPath)
{
    const auto stem = jarPath.stem().u8string();
    std::size_t segment = 0;
    while (segment < stem.size()) {
        const auto next = stem.find('-', segment);
        const auto head = stem[segment];
        if (head >= '0' && head <= '9')
            return trimVersionValue(stem.substr(segment));
        if (next == std::string::npos) break;
        segment = next + 1;
    }
    return {};
}

std::string versionFromJarFileName(const std::string& jarPath)
{
    if (jarPath.empty()) return {};
    //jarPath 持 discovery 结果（UTF-8 交付契约），显式 u8path 清扫。
    return versionFromJarFileName(fs::u8path(jarPath));
}

///@brief 可执行文件名版本回退（jcdc 等原生件无 MANIFEST 可读）。
///
///`-` 分段，首个「数字开头段」起取余串；`v` 前缀段（发布件
///`jcdc-v0.1.4-windows-amd64.exe` 的 `v0.1.4`）剥掉 `v` 后同样成立，余串连
///平台段一并保留（版本串随制品自述，宁全不截——与 JAR 文件名回退口径一致）。
///无数字开头段（如固定名 `jcdc.exe`）→ 空串（词尾不冒充版本，交由调用方
///以产物路径兜底展示）。
std::string versionFromExecutableFileName(const std::string& executablePath)
{
    if (executablePath.empty()) return {};
    //executablePath 持 discovery 结果（UTF-8 交付契约），显式 u8path 清扫。
    const auto stem = fs::u8path(executablePath).stem().u8string();
    std::size_t segment = 0;
    while (segment < stem.size()) {
        std::size_t head = segment;
        if ((stem[head] == 'v' || stem[head] == 'V') && head + 1 < stem.size() &&
            stem[head + 1] >= '0' && stem[head + 1] <= '9') {
            ++head;
        }
        if (stem[head] >= '0' && stem[head] <= '9')
            return trimVersionValue(stem.substr(head));
        const auto next = stem.find('-', segment);
        if (next == std::string::npos) break;
        segment = next + 1;
    }
    return {};
}

///@brief jcdc `-cp` 实参归一（Windows 盘符绕行）。
///
///jcdc 上游把 classpath 固定按 `:` 切分（帮助文本亦写 colon-separated），
///Windows 盘符绝对路径 `C:\foo\bar.jar` 会被切成 `C` 并以 "unsupported
///source" 硬失败（rc=2，整请求炸）。故：不含冒号的路径原样透传（POSIX 绝对/
///相对路径、已是相对路径的形态都走这条）；含冒号（Windows 盘符）的改写成
///相对当前目录的路径（同盘可达时无冒号）；改写后仍含冒号（跨盘）或改写失败
///返回空串——调用方丢弃 -cp，宁缺跨类型上下文不炸请求。子进程继承本进程
///cwd（libca Command 无 cwd 覆盖面），相对路径解析口径与本函数一致。
std::string jcdcClasspathArgument(const std::string& classPath)
{
    if (classPath.find(':') == std::string::npos) return classPath;
    std::error_code error;
    const auto       base = fs::current_path(error);
    if (error) return {};
    const auto relative = fs::relative(fs::u8path(classPath), base, error);
    if (error || relative.empty()) return {};
    const auto text = relative.u8string();
    return text.find(':') == std::string::npos ? text : std::string{};
}

}   //namespace

std::string decompilerJarVersion(const std::string& jarPath)
{
    if (jarPath.empty()) return {};
    try {
        ca::zip::ZipFile archive;
        //需要 Result 语义（构造重载保留异常），先默认构造再 open。
        if (archive.open(jarPath).is_err()) return versionFromJarFileName(jarPath);

        const ca::zip::ZipEntry* manifestEntry = archive.get_entry("META-INF/MANIFEST.MF");
        if (manifestEntry == nullptr) {
            //少数归档条目名大小写/分隔有出入时按序扫一遍。
            for (const auto& name : archive.entries()) {
                std::string lowered = name;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
                    return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
                });
                if (lowered == "meta-inf/manifest.mf") {
                    manifestEntry = archive.get_entry(name);
                    break;
                }
            }
        }
        if (manifestEntry == nullptr) return versionFromJarFileName(jarPath);

        auto bytesResult = archive.read(*manifestEntry);
        if (bytesResult.is_err()) return versionFromJarFileName(jarPath);
        const auto& bytes = bytesResult.unwrap();
        const auto  length = std::min(bytes.size(), kManifestMaximumBytes);
        const std::string raw(reinterpret_cast<const char*>(bytes.data()), length);

        const auto lines    = unfoldManifestLines(raw);
        auto       version  = manifestAttribute(lines, "Implementation-Version");
        if (version.empty()) version = manifestAttribute(lines, "Specification-Version");
        if (!version.empty()) return version;
    } catch (...) {
        //版本是尽力而为的展示字段：解析失败回退文件名，绝不影响可用性判定。
    }
    return versionFromJarFileName(jarPath);
}

ca::StatusOr<DecompilerKind> parseDecompilerKind(const std::string& name)
{
    if (name == "cfr") return ca::Ok(DecompilerKind::Cfr);
    if (name == "vineflower") return ca::Ok(DecompilerKind::Vineflower);
    if (name == "jcdc") return ca::Ok(DecompilerKind::Jcdc);
    return ca::Err(shared::errStatus(ca::StatusCode::INVALID_ARGUMENT,
                                         "mjt.shared.err.unknown_decompiler", {name}));
}

const char* decompilerKindId(DecompilerKind kind) noexcept
{
    switch (kind) {
        case DecompilerKind::Cfr: return "cfr";
        case DecompilerKind::Vineflower: return "vineflower";
        case DecompilerKind::Jcdc: return "jcdc";
    }
    return "cfr";
}

DecompilerAvailability inspectDecompiler(DecompilerKind kind,
                                         const std::string& decompilerJar,
                                         const std::string& javaProgram)
{
    DecompileRequest request;
    request.kind          = kind;
    request.decompilerJar = decompilerJar;
    request.javaProgram   = javaProgram;

    DecompilerAvailability result;
    result.kind                = kind;
    result.id                  = decompilerKindId(kind);
    result.name                = decompilerName(kind);
    result.environmentVariable = decompilerEnvironmentVariable(kind);
    //jcdc 是原生可执行文件（不经 JVM 启动）：Java 不是它的运行前提，仅作环境
    //信息展示，不参与 available 判定与不可用原因拼装。
    result.requiresJava        = kind != DecompilerKind::Jcdc;
    result.decompilerJar       = discoverDecompilerJar(request);
    result.javaProgram         = discoverJava(request);
    result.artifactAvailable   = !result.decompilerJar.empty() && isRegularFile(result.decompilerJar);
    result.javaAvailable       = isProgramAvailable(result.javaProgram);
    //来源层（P9）：与 discoverDecompilerJarDetailed 的优先级逐步对齐——请求
    //显式路径 > 后端环境变量 > MJT_DECOMPILER_HOME 命中候选名 > package 目录
    //兜底。HOME 设了但候选都没落盘时发现会继续走 package，来源如实报
    //package_dir。
    if (!decompilerJar.empty()) {
        result.discoverySource = "explicit";
    }
    else {
        const auto envJar = environment(result.environmentVariable.c_str());
        bool       envHomeHit = false;
        if (envJar.empty()) {
            const auto home = environment("MJT_DECOMPILER_HOME");
            if (!home.empty()) {
                const auto candidates = decompilerHomeCandidates(kind);
                for (const auto& candidate : candidates) {
                    if (isRegularFile(fs::path(home) / candidate)) {
                        envHomeHit = true;
                        break;
                    }
                }
            }
        }
        result.discoverySource = !envJar.empty() || envHomeHit ? "env" : "package_dir";
    }
    //版本是产物自报的展示字段：仅在产物确实存在时读取，尽力而为（JAR 走
    //MANIFEST、jcdc 走可执行文件名、失败返空），不影响
    //artifactAvailable/javaAvailable 判定。
    if (result.artifactAvailable) {
        result.version = kind == DecompilerKind::Jcdc
                             ? versionFromExecutableFileName(result.decompilerJar)
                             : decompilerJarVersion(result.decompilerJar);
    }
    if (!result.artifactAvailable) {
        result.unavailableReason =
            result.name + (kind == DecompilerKind::Jcdc ? " executable was not found"
                                                        : " JAR was not found");
    }
    if (result.requiresJava && !result.javaAvailable) {
        if (!result.unavailableReason.empty()) result.unavailableReason += "; ";
        result.unavailableReason += "Java executable was not found";
    }
    return result;
}

ca::StatusOr<DecompileResult> decompile(const DecompileRequest& request)
{
    namespace fs = std::filesystem;
    //输入/输出路径持 argv 值（UTF-8 交付契约；mjt-mcp 面为 ASCII 临时目录
    //产物，同样在 UTF-8 契约内）——显式 u8path，与 条目名边界同款，不经
    //ACP 隐式窄转码。
    const fs::path input = fs::u8path(request.inputPath);
    const fs::path output = fs::u8path(request.outputPath);
    if (request.inputPath.empty() || request.outputPath.empty()) {
        return ca::Err(shared::errStatus(ca::StatusCode::INVALID_ARGUMENT, "mjt.shared.err.missing_io"));
    }
    if (!isRegularFile(input)) {
        return ca::Err(shared::errStatus(ca::StatusCode::NOT_FOUND, "mjt.shared.err.input_missing",
                                         {request.inputPath}));
    }
    //扩展名大小写不敏感（与 jar remap 的 JarUtil::isJarFile 口径一致，.JAR/.Class 可用）。
    auto equalsNoCase = [](const std::string& s, const char* lower) {
        if (s.size() != std::strlen(lower)) return false;
        for (std::size_t i = 0; i < s.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(s[i])) != lower[i]) return false;
        }
        return true;
    };
    const auto inputExt  = input.extension().string();
    const auto outputExt = output.extension().string();
    const bool classInput = equalsNoCase(inputExt, ".class");
    const bool jarInput   = equalsNoCase(inputExt, ".jar");
    if (!classInput && !jarInput) {
        return ca::Err(
            shared::errStatus(ca::StatusCode::INVALID_ARGUMENT, "mjt.shared.err.input_kind"));
    }
    if (classInput && !equalsNoCase(outputExt, ".java")) {
        return ca::Err(
            shared::errStatus(ca::StatusCode::INVALID_ARGUMENT, "mjt.shared.err.output_java"));
    }
    //zip 输出模式（反馈 bug1）：JAR 输入 + vineflower 时把用户请求的
    //.zip 路径直接交给反编译器写归档——归档内条目名不受文件系统路径限制，
    //是超长类名样本的官方绕过（错误文案亦指向此处）。class 输入的 .zip 已被
    //上面的 output_java 检查拒绝；CFR 无归档输出能力，显式拒绝而非静默落成
    //同名目录。
    const bool zipOutput = jarInput && equalsNoCase(outputExt, ".zip");
    if (zipOutput && request.kind != DecompilerKind::Vineflower) {
        return ca::Err(shared::errStatus(ca::StatusCode::INVALID_ARGUMENT,
                                         "mjt.shared.err.zip_output_vineflower_only",
                                         {decompilerName(request.kind)}));
    }
    if (!request.mappingPath.empty() && !jarInput) {
        return ca::Err(
            shared::errStatus(ca::StatusCode::INVALID_ARGUMENT, "mjt.shared.err.premap_jar_only"));
    }
    if (!request.mappingPath.empty() && request.mappingFormat.empty()) {
        return ca::Err(shared::errStatus(ca::StatusCode::INVALID_ARGUMENT,
                                         "mjt.shared.err.mapping_format_required"));
    }
    if (request.timeout.count() <= 0 || request.maxMemoryMiB <= 0) {
        return ca::Err(
            shared::errStatus(ca::StatusCode::INVALID_ARGUMENT, "mjt.shared.err.positive_limits"));
    }

    auto discovery = discoverDecompilerJarDetailed(request);
    if (discovery.jar.empty() || !isRegularFile(discovery.jar)) {
        const char* variable = decompilerEnvironmentVariable(request.kind);
        auto message =
            shared::trfStr(request.kind == DecompilerKind::Jcdc
                               ? "mjt.shared.err.decompiler_exe_missing"
                               : "mjt.shared.err.decompiler_jar_missing",
                           {decompilerName(request.kind), variable});
        //报错文案列出全部已探测候选（按序），区分「没配」与「配了但文件不在」。
        if (!discovery.probed.empty()) {
            std::string probed;
            for (const auto& candidate : discovery.probed) {
                if (!probed.empty()) probed += "; ";
                probed += candidate;
            }
            message += "\n" +
                       shared::trfStr("mjt.shared.err.decompiler_jar_probed", {probed});
        }
        return ca::Err(ca::ErrStatus(ca::StatusCode::NOT_FOUND, message));
    }
    const auto decompilerJar = discovery.jar;

    try {
        TempDirectory temporary(createTempDirectory(request));
        fs::path      actualInput = input;
        if (!request.mappingPath.empty()) {
            actualInput   = temporary.path() / "remapped-input.jar";
            auto remapped = remapJar({input.u8string(),
                                      actualInput.u8string(),
                                      request.mappingPath,
                                      request.mappingFormat,
                                      request.mappingPolicy});
            if (remapped.is_err()) return ca::Err(remapped.unwrap_err());
        }

        //外部工具始终写入独立临时目录。这样非零退出和超时不会在用户目录留下半成品，
        //也避免 Vineflower 的同名源码 JAR 覆盖或删除位于输出目录中的原输入 JAR。
        //zip 输出模式例外：归档路径由用户显式指定、直接作为反编译器的输出目标，
        //不经临时目录（成功即交付物；失败时清除半成品归档）。
        fs::path        generatedRoot = temporary.path() / "sources";
        std::error_code error;
        if (zipOutput) {
            if (!output.parent_path().empty()) {
                fs::create_directories(output.parent_path(), error);
                if (error) {
                    return ca::Err(shared::errStatus(ca::StatusCode::PERMISSION_DENIED,
                                                     "mjt.shared.err.mkdir_output",
                                                     {error.message()}));
                }
            }
        }
        else {
            fs::create_directories(generatedRoot, error);
            if (error) {
                return ca::Err(shared::errStatus(ca::StatusCode::PERMISSION_DENIED,
                                                 "mjt.shared.err.mkdir_output",
                                                 {error.message()}));
            }
        }

        std::vector<std::string> arguments;
        const bool jcdcInput = request.kind == DecompilerKind::Jcdc;
        if (jcdcInput) {
            //jcdc 是原生可执行文件：不经 JVM 启动，不消费 -D/-Xmax/-Xmx，
            //maxMemoryMiB 对它无意义（请求层校验仍要求正值，保持两后端同一条
            //输入纪律）。argv 形态 = 选项先行 + 输入（与 --help 示例一致，
            //实测选项后置亦可，此处取示例形）。
            arguments.push_back("-o");
            //zip 输出已被上面 zip_output 检查拒（非 vineflower），generatedRoot
            //恒为已创建目录 → jcdc 按输出根写包结构树（单 class 只写目标类一个
            //文件、jar 写整树，与 CFR 口径一致）。
            arguments.push_back(generatedRoot.u8string());
            //恒传 --synthetic：jcdc 默认隐藏 synthetic/bridge 成员（仅类头留
            //计数注释），与 CFR/Vineflower 全量成员口径不齐——mjt#62 的头号
            //发现就是该静默面，产品面按全量对齐（口径已确认）。
            arguments.push_back("--synthetic");
            //单 class 跨类型上下文：-cp（上游按 ':' 分隔、不认 Windows 盘符，
            //见 jcdcClasspathArgument；返回空则丢弃该参数宁缺不炸）。
            if (classInput && !request.classPath.empty()) {
                const auto classpath = jcdcClasspathArgument(request.classPath);
                if (!classpath.empty()) {
                    arguments.push_back("-cp");
                    arguments.push_back(classpath);
                }
            }
            arguments.push_back(actualInput.u8string());
        }
        else {
            //T3 stderr 根治参数化实验：显式声明子进程编码为 UTF-8（JDK17 重定向
            //stdio 默认跟随平台，中文 Windows 为 GBK，诊断文案需 text_codec GBK
            //恢复）。-Dfile.encoding 管 System.getProperty/文件 API 默认，
            //-Dsun.{stdout,stderr}.encoding 管 System.out/err 重定向流编码。
            //验证门槛：真实 cfr-0.152.jar / vineflower-1.12.0.jar 成功场景产物
            //与不加参数逐字节一致（见 work/report_mcp_sweep1.md §T3）。
            arguments.push_back("-Dfile.encoding=UTF-8");
            arguments.push_back("-Dsun.stdout.encoding=UTF-8");
            arguments.push_back("-Dsun.stderr.encoding=UTF-8");
            arguments.push_back("-Xmx" + std::to_string(request.maxMemoryMiB) + "m");
            arguments.push_back("-jar");
            arguments.push_back(decompilerJar);
            if (request.kind == DecompilerKind::Cfr) {
                //子进程 argv 按 libca 全库 UTF-8 窄串纪律交付：u8string 与 契约
                //同源（UTF-8 ACP 进程里 .string 同值，零漂移；GBK ACP 进程里不再
                //ACP 再编码）。
                arguments.push_back(actualInput.u8string());
                arguments.push_back("--outputdir");
                arguments.push_back(generatedRoot.u8string());
                //单 class 输入的跨类型上下文：--extraclasspath 指向原 jar。CFR 的
                //选项值取下一 token，与 --outputdir 同区置于输入之后。
                if (classInput && !request.classPath.empty()) {
                    arguments.push_back("--extraclasspath");
                    arguments.push_back(request.classPath);
                }
            }
            else {
                //vineflower 选项必须在 source 之前（--<option>=<value> 单 token
                //等号形）：--add-external 挂原 jar 保单类反编译的跨类型上下文。
                if (classInput && !request.classPath.empty())
                    arguments.push_back("--add-external=" + request.classPath);
                arguments.push_back(actualInput.u8string());
                arguments.push_back(zipOutput ? output.u8string() : generatedRoot.u8string());
            }
        }

        //jcdc 直接以发现的可执行文件为程序；JVM 后端走 discoverJava。
        const std::string program = jcdcInput ? decompilerJar : discoverJava(request);
        auto process = runProcess(program,
                                  arguments,
                                  request.timeout,
                                  request.cancellationRequested);
        if (process.is_err()) {
            if (zipOutput) {
                std::error_code cleanup;
                fs::remove(output, cleanup);
            }
            return ca::Err(process.unwrap_err());
        }
        auto processResult = std::move(process).unwrap();
        if (processResult.exitCode != 0) {
            if (zipOutput) {
                std::error_code cleanup;
                fs::remove(output, cleanup);
            }
            return ca::Err(ca::ErrStatus(
                ca::StatusCode::INTERNAL,
                composeExecFailure(request.kind,
                                   processResult.exitCode,
                                   processResult.stdoutText,
                                   processResult.stderrText,
                                   input.filename().string())));
        }

        if (zipOutput) {
            //归档即交付物：反编译器已直接写用户请求的 .zip，不做解包/目录归一。
            return ca::Ok(DecompileResult{decompilerJar,
                                          std::move(processResult.stdoutText),
                                          std::move(processResult.stderrText)});
        }

        //后端正常退出但产物缺席（rc=0 空输出/目标类被后端内部跳过）时，错误
        //消息附 stderr 根因摘要；分诊「输入触发后端内部异常」还是「工具侧问题」。
        const auto cause =
            composeBackendSummary(processResult.stderrText, processResult.stdoutText);
        std::string backendSummary;
        if (!cause.empty()) backendSummary = decompilerName(request.kind) + ": " + cause;

        if (classInput) {
            auto status = normalizeClassOutput(generatedRoot, input, output, backendSummary);
            if (status.is_err()) return ca::Err(std::move(status));
        }
        else {
            if (request.kind == DecompilerKind::Vineflower) {
                auto status =
                    normalizeVineflowerJarOutput(generatedRoot, actualInput, backendSummary);
                if (status.is_err()) return ca::Err(std::move(status));
            }
            auto status = copyJavaTree(generatedRoot, output, backendSummary);
            if (status.is_err()) return ca::Err(std::move(status));
        }

        return ca::Ok(DecompileResult{decompilerJar,
                                      std::move(processResult.stdoutText),
                                      std::move(processResult.stderrText)});
    }
    catch (const std::exception& exception) {
        return ca::Err(shared::errStatus(ca::StatusCode::INTERNAL,
                                         "mjt.shared.err.orchestration_exception",
                                         {exception.what()}));
    }
    //非 std 异常兜底（对齐 remapJar）：zip/JVM 库抛出未知异常时返回 INTERNAL
    //状态，而不是让进程 terminate。
    catch (...) {
        return ca::Err(shared::errStatus(ca::StatusCode::INTERNAL, "mjt.shared.err.orchestration_unknown"));
    }
}

std::string composeBackendSummary(const std::string& stderrText, const std::string& stdoutText)
{
    //GBK 规范化与 exec 失败路径同源：子进程在中文 Windows 上以 GBK 输出本地化
    //异常文案，原始字节拼进错误消息会被 CLI 渲染软失败吞掉（历史"静默失败"）。
    const std::string& raw        = stderrText.empty() ? stdoutText : stderrText;
    const std::string  diagnostic = shared::text::toCanonicalUtf8(raw);
    return truncateUtf8(extractRootCauseLine(diagnostic), kCauseSummaryLimit);
}

}   //namespace mjt
