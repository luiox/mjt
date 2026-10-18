#pragma once

//long_path.hpp
//
//jar extract 落盘长路径安全写原语（P0 硬崩修复面）。
//
//缺陷（外部实战报告 ，本仓 baseline mjt.exe 实测复现，见
//work/report_1110_crash.md）：activeCodePage=UTF-8 manifest 进程内，窄字节
//stdio（fopen 系）对多组件目标路径存在 MAX_PATH 界的 UCRT fail-fast 硬崩
//（0xC0000409 / STATUS_STACK_BUFFER_OVERRUN），实测边界 258 字符成功 /
//259 字符起进程硬崩零输出；超长单组件（如 800 字符条目名）则不崩，表现为
//fopen 失败 + 逐条目错误刷屏。std::filesystem 的 create_directories 内部
//走宽字符链路不受影响（与报告"崩点在 fopen/fwrite 区"一致）。
//
//修复口径：
//落盘统一走宽字符（_wopen → CreateFileW 链路），绕开窄字节 CRT 转换层；
//路径先经 GetFullPathNameW 归一化为绝对路径（正斜杠转反斜杠、消 . 与
//..），总长 ≥ MAX_PATH 时加 \\?\ 扩展长度前缀（UNC 走 \\?\UNC\）。
//\\?\ 段不做任何归一化，因此前缀必须加在归一化后的规范绝对路径上。
//短路径（< MAX_PATH）不加前缀，行为与原先逐字节等价（含尾部点/空格
//组件等 Win32 归一化特例——加前缀会改变这类路径的语义）。
//
//\\?\ 转换边界用例（详见 report §边界清单，unittest 同名用例覆盖）：
//- 已带 \\?\ / \\?\UNC\ 前缀 → 原样返回，不二次加前缀；
//- 相对路径 → GetFullPathNameW 按 cwd 展开为绝对路径；
//- 正斜杠 → 统一转反斜杠（\\?\ 不接受正斜杠、不做归一化）；
//- UNC（\\server\share\...）→ \\?\UNC\server\share\...（#4 未测钉：本机
//无 UNC 服务器，环境不可得，映射分支仅代码走查）；
//- 总长 < MAX_PATH → 不加前缀（短路径逐字节等价口径）；
//- 单组件 > 255（NTFS 上限）→ 预检直接判 ComponentTooLong，不给底层
//模糊错误；
//- 总长 > 32767（NT 内部上限）→ 预检直接判 PathTooLong。
//
//纯 header-only；非 Windows 平台退化为 fopen / fs::create_directories 直通
//（Linux CI 同靶编译，长路径不是 POSIX 的问题面）。

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>

#include "libca/str/utf8_util.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cwctype>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>

//windows.h 的 VOID 宏（#define VOID void）会打坏后续 JVM 字节码头的类型
//常量（jbytecode::core::Type::VOID → "static constexpr int void"）。本头的
//消费 TU（cmd_jar 等）在包含本头之后还要包含 mjbase/jbytecode 头，故在此
//恢复裸名；本头不再包含其它 Windows 头，无回漂风险。
#ifdef VOID
#undef VOID
#endif
#endif

namespace mjt::longpath {

///Win32 传统 MAX_PATH 界（含结尾 NUL 的路径缓冲上限）。
inline constexpr std::size_t kMaxPath = 260;
///NTFS/Win32 单组件长度上限（UTF-16 计）。
inline constexpr std::size_t kMaxComponent = 255;
///NT 内部路径上限（\\?\ 前缀形态下的总长上限）。
inline constexpr std::size_t kNtInternalLimit = 32767;

///错误文案里条目名/路径的显示截断预算（UTF-8 码点数）。
inline constexpr std::size_t kErrorDisplayChars = 64;

///UTF-8 首字节 → 序列字节长度（非法字节按 1 兜底，保证遍历必然前进）。
inline std::size_t utf8SequenceLength(unsigned char lead) noexcept
{
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

///报错显示用截断：超过 @p maxChars 个 UTF-8 码点时取前 maxChars 个码点 +
///"…(len=全长码点数)"。长名错误刷屏（缺陷②）的降噪原语。
inline std::string truncateForDisplay(std::string_view s, std::size_t maxChars = kErrorDisplayChars)
{
    //逐序列步进（非法字节按 1 兜底，遍历必然前进、不劈开合法序列）：
    //total 统计全长码点数；cutBytes 记第 maxChars+1 个码点的起始字节。
    std::size_t bytes    = 0;
    std::size_t total    = 0;
    std::size_t cutBytes = std::string_view::npos;
    while (bytes < s.size()) {
        const std::size_t step =
            std::min(utf8SequenceLength(static_cast<unsigned char>(s[bytes])), s.size() - bytes);
        if (total == maxChars) cutBytes = bytes;
        bytes += step;
        ++total;
    }
    if (cutBytes == std::string_view::npos) return std::string(s);   //未超限
    return std::string(s.substr(0, cutBytes)) + "\xE2\x80\xA6(len=" + std::to_string(total) + ")";
}

#if defined(_WIN32)

namespace detail {

///窄字节 fs::path → 宽字符串。转换口径与 std::filesystem 一致（native
///narrow = ACP；mjt exe 已嵌 UTF-8 manifest，与程序内其它路径解释同源）。
inline std::wstring toWide(const std::filesystem::path& p)
{
    std::wstring wide = p.wstring();
    std::replace(wide.begin(), wide.end(), L'/', L'\\');
    return wide;
}

///GetFullPathNameW 归一化：相对→绝对（按 cwd）、消 . 与 ..、正斜杠→反斜杠。
///失败时原样返回输入（后续打开步骤给出底层错误）。纯字符串操作，不做
///文件系统探测。
inline std::wstring toAbsoluteWide(const std::wstring& wide)
{
    DWORD need = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (need == 0) return wide;
    std::wstring abs(static_cast<std::size_t>(need), L'\0');
    DWORD written = GetFullPathNameW(wide.c_str(), need, abs.data(), nullptr);
    if (written == 0 || written >= need) return wide;
    abs.resize(written);
    return abs;
}

inline bool hasExtendedPrefix(const std::wstring& w)
{
    return w.rfind(L"\\\\?\\UNC\\", 0) == 0 || w.rfind(L"\\\\?\\", 0) == 0;
}

///已带扩展前缀 → 原样；UNC（\\server\share\...）→ \\?\UNC\server\share\...；
///盘符/根相对绝对路径 → \\?\...。仅对归一化后的绝对路径合法（\\?\ 段
///不做归一化，见头注）。
inline std::wstring withExtendedPrefix(const std::wstring& abs)
{
    if (hasExtendedPrefix(abs)) return abs;
    if (abs.rfind(L"\\\\", 0) == 0 && abs.size() >= 2 && abs[2] != L'?')
        return L"\\\\?\\UNC\\" + abs.substr(2);
    return L"\\\\?\\" + abs;
}

///UTF-16 → UTF-8（诊断文案固定编码，不随 ACP 漂移）。
inline std::string wideToUtf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}

///GetLastError → "win32=<code>(<FormatMessage 文案>)"（无消息时仅码值）。
inline std::string lastWin32ErrorText()
{
    const DWORD err = GetLastError();
    if (err == 0) return {};
    std::string out = "win32=" + std::to_string(err);
    LPWSTR    msg   = nullptr;    const DWORD n   = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                               reinterpret_cast<LPWSTR>(&msg), 0, nullptr);
    if (n > 0 && msg != nullptr) {
        std::wstring text(msg, n);
        LocalFree(msg);
        while (!text.empty() &&
               (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
            text.pop_back();
        out += "(" + wideToUtf8(text) + ")";
    }
    return out;
}

inline std::string errnoCode()
{
    return "errno=" + std::to_string(errno);
}

inline std::string errnoText()
{
#if defined(_WIN32)
    char buf[128];
    buf[0] = '\0';
    strerror_s(buf, sizeof(buf), errno);
    return errnoCode() + "(" + buf + ")";
#else
    return errnoCode() + "(" + std::strerror(errno) + ")";
#endif
}

///组件超长预检：归一化绝对路径上任一组件 > 255 → 返回该组件（宽）。
///"C:" 盘符段不计。纯字符串操作。
inline bool findOverlongComponent(const std::wstring& abs, std::wstring* outComponent)
{
    std::size_t start = 0;
    while (start <= abs.size()) {
        std::size_t end = abs.find(L'\\', start);
        if (end == std::wstring::npos) end = abs.size();
        const std::size_t len     = end - start;
        const bool        isDrive = len == 2 && start + 1 < abs.size() && abs[start + 1] == L':';
        if (len > kMaxComponent && !isDrive) {
            if (outComponent != nullptr) *outComponent = abs.substr(start, len);
            return true;
        }
        start = end + 1;
    }
    return false;
}

}   //namespace detail

///目标路径是否超限：绝对化后总长 ≥ MAX_PATH，或任一组件 > 255。
///--flat 模式据此决定是否改用安全名落盘（修复⑤）。纯字符串判定，
///不做文件系统探测。
inline bool isOverLimit(const std::filesystem::path& path)
{
    const std::wstring wide = detail::toAbsoluteWide(detail::toWide(path));
    if (wide.size() + 1 >= kMaxPath) return true;
    return detail::findOverlongComponent(wide, nullptr);
}

///打开结果：file 为 nullptr 时 kind/code/detail 给出可诊断原因（修复③）。
///code 为稳定错误签名（不含路径/名字等可变部分），供 --recursive 同形错误
///聚合去重（修复⑤）作 shape 键；detail 为人类可读全文。
struct OpenResult
{
    enum class Kind
    {
        Ok,
        ComponentTooLong,   ///单组件 > 255（NTFS 上限），写前预检拦截
        PathTooLong,        ///总长 > 32767（NT 内部上限），写前预检拦截
        OpenFailed,         ///底层打开失败（detail 含 errno + win32 文案）
    };

    FILE*       file   = nullptr;
    Kind        kind   = Kind::Ok;
    std::string code;
    std::string detail;
};

///长路径安全写打开（"wb" 语义：不存在则建，存在则截断）。
///Windows 上经 GetFullPathNameW 归一化 + 按需 \\?\ 前缀 + _wopen，绕开
///窄字节 stdio 的 MAX_PATH fail-fast（缺陷①机理）。
inline OpenResult openForWrite(const std::filesystem::path& path)
{
    OpenResult result;
    const std::wstring abs = detail::toAbsoluteWide(detail::toWide(path));

    std::wstring overlong;
    if (detail::findOverlongComponent(abs, &overlong)) {
        result.kind   = OpenResult::Kind::ComponentTooLong;
        result.code   = "component_too_long";
        result.detail = "component " + std::to_string(overlong.size()) + " chars > " +
                        std::to_string(kMaxComponent) + ": " +
                        truncateForDisplay(detail::wideToUtf8(overlong));
        return result;
    }

    const std::wstring osPath = detail::withExtendedPrefix(abs);
    if (osPath.size() + 1 > kNtInternalLimit) {
        result.kind   = OpenResult::Kind::PathTooLong;
        result.code   = "path_too_long";
        result.detail = "path " + std::to_string(osPath.size()) + " chars > " +
                        std::to_string(kNtInternalLimit);
        return result;
    }

    //_wopen 失败即取码：CreateFileW 的 LastError 与 CRT errno 同时上报，
    //避免窄字节 fopen 失败后只剩布尔信号的不可诊断局面。
    const int fd =
        _wopen(osPath.c_str(), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
    if (fd < 0) {
        result.kind = OpenResult::Kind::OpenFailed;
        result.code = detail::errnoCode();
        result.detail = detail::errnoText();
        const std::string win32 = detail::lastWin32ErrorText();
        if (!win32.empty()) {
            result.code += "," + win32.substr(0, win32.find('('));
            result.detail += "; " + win32;
        }
        return result;
    }

    FILE* f = _fdopen(fd, "wb");
    if (f == nullptr) {
        result.kind   = OpenResult::Kind::OpenFailed;
        result.code   = detail::errnoCode() + ",_fdopen";
        result.detail = detail::errnoText() + " (_fdopen)";
        _close(fd);
        return result;
    }
    result.file = f;
    return result;
}

///fwrite 短写等写中失败的诊断串（调用方 fclose 后仍可安全取 errno）。
inline std::string writeFailureDetail()
{
    return detail::errnoText();
}

inline std::string writeFailureCode()
{
    return detail::errnoCode();
}

///长路径安全 create_directories。短路径（< MAX_PATH）走 std::filesystem
///既有链路（逐字节等价口径）；长路径逐级 CreateDirectoryW（\\?\ 形态）。
///失败时返回 false 且 diag 给底层原因。
inline bool createDirectories(const std::filesystem::path& dir, std::string& diag)
{
    diag.clear();
    if (dir.empty()) return true;

    const std::wstring abs = detail::toAbsoluteWide(detail::toWide(dir));

    std::wstring overlong;
    if (detail::findOverlongComponent(abs, &overlong)) {
        diag = "parent component " + std::to_string(overlong.size()) + " chars > " +
               std::to_string(kMaxComponent) + ": " +
               truncateForDisplay(detail::wideToUtf8(overlong));
        return false;
    }

    if (abs.size() + 1 < kMaxPath) {
        //短路径：维持 std::filesystem 既有行为（含错误语义）。
        std::error_code ec;
        if (std::filesystem::create_directories(dir, ec)) return true;
        if (!ec) return true;
        if (std::filesystem::is_directory(dir, ec)) return true;   //已存在不算失败
        diag = ec.message();
        return false;
    }

    //长路径：逐级 CreateDirectoryW。\\?\ 前缀形态要求规范绝对路径。
    const std::wstring osPath = detail::withExtendedPrefix(abs);
    if (osPath.size() + 1 > kNtInternalLimit) {
        diag = "path " + std::to_string(osPath.size()) + " chars > " +
               std::to_string(kNtInternalLimit);
        return false;
    }

    //根定位（注意 osPath 带 \\?\ 前缀时，盘符/UNC 段从前缀之后算起）：
    //盘符形态跳过 "\\?\C:\"（盘根不可创建）；UNC 形态跳过
    //"\\?\UNC\server\share"（server/share 由系统管理）。
    std::wstring cur;
    std::size_t  pos  = 0;
    std::size_t  skip = 0;
    if (osPath.rfind(L"\\\\?\\UNC\\", 0) == 0)
        skip = 8;
    else if (osPath.rfind(L"\\\\?\\", 0) == 0) skip = 4;

    if (skip + 2 < osPath.size() && std::iswalpha(osPath[skip]) != 0 && osPath[skip + 1] == L':')
    {
        cur = osPath.substr(0, skip + 3);   //"C:\" 或 "\\?\C:\"
        pos = skip + 3;
    }
    else if (skip == 8) {
        const std::size_t first = osPath.find(L'\\', skip);   //server 之后
        if (first == std::wstring::npos) return true;
        const std::size_t second = osPath.find(L'\\', first + 1);
        if (second == std::wstring::npos) return true;   //仅 \\?\UNC\server\share 本身
        cur = osPath.substr(0, second + 1);
        pos = second + 1;
    }
    else {
        cur = osPath.substr(0, skip);   //根相对兜底（如 \\?\dir\...）
        pos = skip;
    }

    while (pos < osPath.size()) {
        std::size_t end = osPath.find(L'\\', pos);
        if (end == std::wstring::npos) end = osPath.size();
        if (end > pos) {
            if (!cur.empty() && cur.back() != L'\\') cur += L'\\';
            cur += osPath.substr(pos, end - pos);
            if (!CreateDirectoryW(cur.c_str(), nullptr)) {
                const DWORD err = GetLastError();
                //已存在（ALREADY_EXISTS）不算失败；对 C:\Users 这类已存在但
                //拒绝创建调用（ACCESS_DENIED）的保护目录，属性核实为目录后
                //同样放行——逐级创建必须容忍"既有祖先"。
                bool exists = (err == ERROR_ALREADY_EXISTS);
                if (!exists) {
                    const DWORD attr = GetFileAttributesW(cur.c_str());
                    exists = attr != INVALID_FILE_ATTRIBUTES &&
                             (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
                }
                if (!exists) {
                    diag = detail::lastWin32ErrorText();
                    if (diag.empty()) diag = detail::errnoText();
                    return false;
                }
            }
        }
        pos = end + 1;
    }

    //终态校验：目标必须已是目录。
    const DWORD attr = GetFileAttributesW(osPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        diag = detail::lastWin32ErrorText();
        if (diag.empty()) diag = "final path is not a directory";
        return false;
    }
    return true;
}

///长路径安全 remove_all（清场/删除面，与 openForWrite/createDirectories 同源
///宽字符链路）：归一化 + 按需 \\?\ 前缀后交 std::filesystem::remove_all——
///MSVC fs 对 \\?\ 形态直通宽字符删除 API（FindFirstFileExW/CreateFileW/
///RemoveDirectoryW），不受窄路径形态的 MAX_PATH 界约束（同机理：
///无 manifest 进程里窄路径 remove_all 对 ≥260 长路径恰好静默 no-op）。
///路径不存在不算失败（fs::remove_all 语义）；失败经 ec 上报，调用方不许
///静默吞掉（unittest 清场即消费方）。
inline bool removeAll(const std::filesystem::path& path, std::error_code& ec)
{
    ec.clear();
    if (path.empty()) return true;
    const std::wstring abs    = detail::toAbsoluteWide(detail::toWide(path));
    const std::wstring osPath = detail::withExtendedPrefix(abs);
    std::filesystem::remove_all(std::filesystem::path(osPath), ec);
    return !ec;
}

#else   //!defined(_WIN32)

///目标路径是否超限。非 Windows 平台无 MAX_PATH 界，恒 false。
inline bool isOverLimit(const std::filesystem::path&)
{
    return false;
}

///POSIX 直通：保持窄字节 stdio，失败给 errno 诊断。
struct OpenResult
{
    enum class Kind
    {
        Ok,
        ComponentTooLong,
        PathTooLong,
        OpenFailed,
    };

    FILE*       file   = nullptr;
    Kind        kind   = Kind::Ok;
    std::string code;
    std::string detail;
};

inline OpenResult openForWrite(const std::filesystem::path& path)
{
    OpenResult result;
    result.file = fopen(path.generic_string().c_str(), "wb");
    if (result.file == nullptr) {
        result.kind   = OpenResult::Kind::OpenFailed;
        result.code   = "errno=" + std::to_string(errno);
        result.detail = result.code + "(" + std::strerror(errno) + ")";
    }
    return result;
}

inline std::string writeFailureDetail()
{
    return "errno=" + std::to_string(errno) + "(" + std::strerror(errno) + ")";
}

inline std::string writeFailureCode()
{
    return "errno=" + std::to_string(errno);
}

inline bool createDirectories(const std::filesystem::path& dir, std::string& diag)
{
    diag.clear();
    if (dir.empty()) return true;
    std::error_code ec;
    if (std::filesystem::create_directories(dir, ec)) return true;
    if (!ec) return true;
    if (std::filesystem::is_directory(dir)) return true;
    diag = ec.message();
    return false;
}

///POSIX 直通：无 MAX_PATH 界，fs::remove_all 即安全；失败经 ec 上报。
inline bool removeAll(const std::filesystem::path& path, std::error_code& ec)
{
    ec.clear();
    if (path.empty()) return true;
    std::filesystem::remove_all(path, ec);
    return !ec;
}

#endif   //!defined(_WIN32)

}   //namespace mjt::longpath
