#include "pysdk_embed.hpp"

// pysdk_embed.cpp — 嵌入解释器实现（纯 CPython API）。
//
// 契约（mjt-py/doc_ai/mjt-py-sdk-spec.md §4）：
//   - python home：MJT_PYTHON_HOME env > 编译期 MJT_PYTHON_HOME_DEFAULT；
//   - sys.path 前插：exe 目录（mjt.pyd / mjt_util.py 缺位点）；
//   - 预载 import mjt 并注入脚本 globals；
//   - sys.argv = [script, *scriptArgs]；异常打 traceback，退出码 1；
//   - utf8_mode=1（对齐全仓 UTF-8 契约，D36）；
//   - 退出码口径：0 成功；1 脚本异常/语法错误/协作层上限（KeyboardInterrupt
//     族 traceback）；2 用法与环境错误；3 资源上限硬退出（watchdog）。

#include <Python.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "pysdk_limits.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace mjt::pysdk {
namespace {

std::string envOr(const char* name, const std::string& fallback)
{
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : fallback;
}

/// 窄串路径 → python 字符串字面量（forward slash 规范化，规避 raw-string 尾反斜杠）。
std::string pyPathLiteral(const std::string& p)
{
    std::string out;
    for (char c : p) {
        if (c == '\\') out += '/';
        else if (c == '\'') out += "\\'";
        else out += c;
    }
    return out;
}

void addSysPath(const std::string& dir)
{
    if (dir.empty()) return;
    const std::string code =
        "import sys; sys.path.insert(0, r'" + pyPathLiteral(dir) + "')\n";
    PyRun_SimpleString(code.c_str());
}

// ── 资源上限（协作层装点；状态与硬层 watchdog 在 pysdk_limits） ───────────

std::uint64_t g_outputCap  = 0;    ///< 输出字节上限（0=不限）
PyObject*     g_outputCell = nullptr;   ///< __main__._mjt_cap_cell（list[int] 计数格）

/// 解释器 trace 钩子：逐 Python 行事件查 deadline 与输出计数。协作层兜两类
/// 逃逸：纯 Python 死循环（facade 检查点够不着）与吞掉 writer 异常继续空转
/// 的脚本。只在配置了上限时装（无上限零开销）；只在主线程跑脚本，无需
/// 线程局部。
int traceFunc(PyObject* /*obj*/, PyFrameObject* /*frame*/, int what, PyObject* /*arg*/)
{
    if (what != PyTrace_LINE) return 0;
    if (deadlineRemainingMs() == 0) {
        PyErr_SetString(PyExc_KeyboardInterrupt, "script deadline exceeded");
        return -1;
    }
    if (g_outputCell != nullptr) {
        PyObject* value = PyList_GetItem(g_outputCell, 0);
        const long long written = value ? PyLong_AsLongLong(value) : -1;
        if (written >= 0 && static_cast<std::uint64_t>(written) > g_outputCap) {
            PyErr_SetString(PyExc_KeyboardInterrupt, "script output limit exceeded");
            return -1;
        }
    }
    return 0;
}

/// 输出上限装点：stdout/stderr 换装计数 writer。超限在 write 内即抛
/// KeyboardInterrupt 族；计数格留在 __main__，trace 钩子按格复核（脚本吞
/// 异常时仍有下一行事件兜底）。
void installOutputCap(std::uint64_t cap)
{
    const std::string capLit = std::to_string(cap);
    const std::string bootstrap =
        "import sys\n"
        "_mjt_cap_cell = [0]\n"
        "class _mjt_CappedWriter:\n"
        "    def __init__(self, sink):\n"
        "        self._sink = sink\n"
        "    def write(self, s):\n"
        "        _mjt_cap_cell[0] += len(s.encode('utf-8', 'replace'))\n"
        "        if _mjt_cap_cell[0] > " + capLit + ":\n"
        "            raise KeyboardInterrupt('script output limit exceeded: '\n"
        "                + str(_mjt_cap_cell[0]) + ' > " + capLit + " bytes')\n"
        "        return self._sink.write(s)\n"
        "    def flush(self):\n"
        "        return self._sink.flush()\n"
        "    def isatty(self):\n"
        "        return getattr(self._sink, 'isatty', lambda: False)()\n"
        "    def __getattr__(self, name):\n"
        "        return getattr(self._sink, name)\n"
        "sys.stdout = _mjt_CappedWriter(sys.stdout)\n"
        "sys.stderr = _mjt_CappedWriter(sys.stderr)\n";
    PyRun_SimpleString(bootstrap.c_str());
    PyObject* main = PyImport_AddModule("__main__");
    g_outputCell = main ? PyObject_GetAttrString(main, "_mjt_cap_cell") : nullptr;
    g_outputCap  = cap;
}

/// trace 装点：deadline 或输出上限任一在位才装。
void installTraceIfNeeded()
{
    if (g_outputCap == 0 && deadlineRemainingMs() < 0) return;
    PyEval_SetTrace(traceFunc, Py_None);
}

/// 硬层宽限：协作层在 deadline 时刻中断脚本后，进程还要走 traceback 打印
/// 与解释器收尾——watchdog 若紧贴 deadline 轮询会跟自己的干净退出竞速
/// （实测退出码在 1/3 间漂）。硬层改在 deadline + 宽限到点，协作层正常
/// 工作时恒有数个量级的余量先退出；只有真卡死的进程才会吃满宽限。
constexpr std::int64_t kScriptHardExitGraceMs = 5000;

}   // namespace

std::string pythonHome()
{
#ifdef MJT_PYTHON_HOME_DEFAULT
    return envOr("MJT_PYTHON_HOME", MJT_PYTHON_HOME_DEFAULT);
#else
    return envOr("MJT_PYTHON_HOME", "");
#endif
}

std::string exeDir()
{
#ifdef _WIN32
    char buf[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH) > 0) {
        return std::filesystem::path(buf).parent_path().u8string();
    }
    return {};
#else
    return {};
#endif
}

bool pythonPreInit()
{
#ifdef _WIN32
    const std::string dll = (std::filesystem::path(pythonHome()) / "python314.dll").u8string();
    if (!LoadLibraryExA(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH)) {
        std::fprintf(stderr, "mjt: cannot load %s (set MJT_PYTHON_HOME)\n", dll.c_str());
        return false;
    }
#endif
    return true;
}

void pythonAddExePath()
{
    addSysPath(exeDir());
    if (const char* extra = std::getenv("MJT_PYSDK_PATH")) {
        std::string list = extra;
        std::size_t pos  = 0;
        while (pos < list.size()) {
            const auto sep = list.find(';', pos);
            addSysPath(list.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos));
            if (sep == std::string::npos) break;
            pos = sep + 1;
        }
    }
}

bool pythonInit()
{
    // utf8_mode 挂在 pre-config（3.14 头契约）；全仓 UTF-8 契约（D36）。
    PyPreConfig pre;
    PyPreConfig_InitPythonConfig(&pre);
    pre.utf8_mode = 1;
    if (PyStatus_Exception(Py_PreInitialize(&pre))) return false;

    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    const std::string home = pythonHome();
    if (!home.empty()) {
        if (PyStatus_Exception(PyConfig_SetBytesString(&config, &config.home, home.c_str()))) {
            PyConfig_Clear(&config);
            return false;
        }
    }
    const PyStatus st = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    return !PyStatus_Exception(st);
}

int pythonFinalize() { return Py_FinalizeEx(); }

int runEmbeddedScript(const EmbedOptions& options)
{
    if (options.scriptPath.empty()) {
        std::fprintf(stderr, "mjt: --script requires a .py path\n");
        return 2;
    }
    std::error_code ec;
    if (!std::filesystem::exists(options.scriptPath, ec)) {
        std::fprintf(stderr, "mjt: script not found: %s\n", options.scriptPath.c_str());
        return 2;
    }

    // 收口 guard：trace 卸载 / 解释器 finalize / watchdog 停 / deadline 清，
    // 全部出口只走这一份。finalize 收进 guard 是为了让「协作层异常 → 返回
    // 码」路径与错误路径共享同一收尾序（先卸 trace 再 finalize）。
    struct LifecycleGuard
    {
        bool pyUp = false;
        ~LifecycleGuard()
        {
            if (pyUp) {
                PyEval_SetTrace(nullptr, nullptr);
                Py_CLEAR(g_outputCell);
                Py_FinalizeEx();
            }
            g_outputCap = 0;
            stopDeadlineWatchdog();
            setDeadlineMs(0);
        }
    } guard;

    // deadline 先于解释器装好：协作层时点 = deadline（trace/检查点中断），
    // 硬层时点 = deadline + 宽限（卡死进程的 _Exit(3)，见宽限常量注）。
    if (options.deadlineMs > 0) {
        setDeadlineMs(options.deadlineMs);
        startDeadlineWatchdog(options.deadlineMs + kScriptHardExitGraceMs, &hardExitHook);
    }

    // Windows：先把 python314.dll 拉进进程，再初始化（utf8_mode=1 + home）。
    if (!pythonPreInit()) return 2;
    if (!pythonInit()) {
        std::fprintf(stderr, "mjt: interpreter init failed (check MJT_PYTHON_HOME)\n");
        return 2;
    }
    guard.pyUp = true;

    // 输出上限装点：换装计数 writer（须在脚本任何输出之前）。
    if (options.maxOutputBytes > 0) installOutputCap(options.maxOutputBytes);

    // exe 目录（mjt.pyd / mjt_util.py 缺位点）+ 追加路径。
    pythonAddExePath();

    // 预载 mjt（IDAPython 形态：模块对脚本开箱即用）。
    PyObject* mjtModule = PyImport_ImportModule("mjt");
    if (!mjtModule) {
        std::fprintf(stderr, "mjt: failed to import mjt (mjt.pyd not on sys.path?)\n");
        PyErr_Print();
        return 2;
    }

    // sys.argv。
    {
        PyObject* argvList = PyList_New(static_cast<Py_ssize_t>(options.scriptArgs.size() + 1));
        PyList_SetItem(argvList, 0, PyUnicode_FromString(options.scriptPath.c_str()));
        for (std::size_t i = 0; i < options.scriptArgs.size(); ++i) {
            PyList_SetItem(argvList, static_cast<Py_ssize_t>(i + 1),
                           PyUnicode_FromString(options.scriptArgs[i].c_str()));
        }
        PySys_SetObject("argv", argvList);
        Py_XDECREF(argvList);
    }

    installTraceIfNeeded();

    // 执行脚本：globals 预注入 mjt 与 __name__="__main__"。
    // 读字节 + Py_CompileString + PyEval_EvalCode，不走 PyRun_File 的 FILE*：
    // 宿主以 /MT 静态 CRT 构建而 python314.dll 是动态 CRT，FILE* 跨 CRT
    // 边界的布局解读会让解释器 abort（实测 fastfail 0xC0000409，静默退出）。
    std::FILE* fp = nullptr;
#ifdef _WIN32
    const errno_t ferr = fopen_s(&fp, options.scriptPath.c_str(), "rb");
    if (ferr != 0) fp = nullptr;
#else
    fp = std::fopen(options.scriptPath.c_str(), "rb");
#endif
    if (!fp) {
        std::fprintf(stderr, "mjt: cannot open script: %s\n", options.scriptPath.c_str());
        Py_DECREF(mjtModule);
        return 2;
    }
    std::string source;
    {
        char buf[8192];
        std::size_t got = 0;
        while ((got = std::fread(buf, 1, sizeof(buf), fp)) > 0) source.append(buf, got);
        std::fclose(fp);
    }
    // UTF-8 BOM：Py_CompileString 按 utf8 解码，BOM 必须先剥（PyRun_File 会吃掉它）。
    if (source.size() >= 3 && static_cast<unsigned char>(source[0]) == 0xEF &&
        static_cast<unsigned char>(source[1]) == 0xBB &&
        static_cast<unsigned char>(source[2]) == 0xBF) {
        source.erase(0, 3);
    }

    PyObject* globals = PyDict_New();
    PyDict_SetItemString(globals, "__name__", PyUnicode_FromString("__main__"));
    PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
    PyDict_SetItemString(globals, "mjt", mjtModule);

    // 语法错误与运行期异常同一呈现面：traceback + 退出码 1。
    PyObject* code = Py_CompileString(source.c_str(), options.scriptPath.c_str(), Py_file_input);
    if (!code) {
        PyErr_Print();
        Py_DECREF(globals);
        Py_DECREF(mjtModule);
        return 1;
    }
    PyObject* result = PyEval_EvalCode(code, globals, globals);
    Py_DECREF(code);
    if (!result) {
        PyErr_Print();
    }

    if (options.interactive) {
        // REPL 走 code.interact（纯 Python）：stdin FILE* 同样不能跨 CRT 传给
        // PyRun_InteractiveOne；以脚本 globals 为命名空间执行，local 即脚本面。
        // trace 仍挂在主线程：deadline 覆盖 REPL 会话。
        const char* repl =
            "import code\n"
            "code.interact(banner='mjt interactive REPL (mjt preloaded; exit() to leave)',\n"
            "              exitmsg='', local=globals())\n";
        PyObject* replResult =
            PyRun_String(repl, Py_file_input, globals, globals);
        if (!replResult) PyErr_Print();
        Py_XDECREF(replResult);
    }

    const int exitCode = result ? 0 : 1;   // 0=脚本成功；1=脚本抛异常（含协作层上限）
    Py_XDECREF(result);
    Py_DECREF(globals);
    Py_DECREF(mjtModule);
    return exitCode;
}

}   // namespace mjt::pysdk
