#include "pysdk_embed.hpp"

// pysdk_embed.cpp — 嵌入解释器实现（纯 CPython API）。
//
// 契约（mjt-py/doc_ai/mjt-py-sdk-spec.md §4）：
//   - python home：MJT_PYTHON_HOME env > 编译期 MJT_PYTHON_HOME_DEFAULT；
//   - sys.path 前插：exe 目录（mjt.pyd / mjt_util.py 缺点位）；
//   - 预载 import mjt 并注入脚本 globals；
//   - sys.argv = [script, *scriptArgs]；异常打 traceback，退出码 1；
//   - utf8_mode=1（对齐全仓 UTF-8 契约，D36）。

#include <Python.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

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

    // Windows：先把 python314.dll 拉进进程，再初始化（utf8_mode=1 + home）。
    if (!pythonPreInit()) return 2;
    if (!pythonInit()) {
        std::fprintf(stderr, "mjt: interpreter init failed (check MJT_PYTHON_HOME)\n");
        return 2;
    }

    // exe 目录（mjt.pyd / mjt_util.py 缺位点）+ 追加路径。
    pythonAddExePath();

    // 预载 mjt（IDAPython 形态：模块对脚本开箱即用）。
    PyObject* mjtModule = PyImport_ImportModule("mjt");
    if (!mjtModule) {
        std::fprintf(stderr, "mjt: failed to import mjt (mjt.pyd not on sys.path?)\n");
        PyErr_Print();
        Py_FinalizeEx();
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

    // 执行脚本：globals 预注入 mjt 与 __name__="__main__"。
    FILE* fp = nullptr;
#ifdef _WIN32
    errno_t ferr = fopen_s(&fp, options.scriptPath.c_str(), "rb");
    if (ferr != 0) fp = nullptr;
#else
    fp = std::fopen(options.scriptPath.c_str(), "rb");
#endif
    if (!fp) {
        std::fprintf(stderr, "mjt: cannot open script: %s\n", options.scriptPath.c_str());
        Py_DECREF(mjtModule);
        Py_FinalizeEx();
        return 2;
    }

    PyObject* globals = PyDict_New();
    PyDict_SetItemString(globals, "__name__", PyUnicode_FromString("__main__"));
    PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());
    PyDict_SetItemString(globals, "mjt", mjtModule);

    PyObject* result = PyRun_File(fp, options.scriptPath.c_str(), Py_file_input, globals,
                                  globals);
    std::fclose(fp);
    if (!result) {
        PyErr_Print();
    }

    if (options.interactive) {
        printf("mjt interactive REPL (mjt preloaded; Ctrl+Z / exit() to leave)\n");
        PyRun_InteractiveLoop(stdin, "<mjt-repl>");
    }

    Py_XDECREF(globals);
    Py_XDECREF(mjtModule);

    pythonFinalize();
    return result ? 0 : 1;   // 0=脚本成功；1=脚本抛异常
}

}   // namespace mjt::pysdk
