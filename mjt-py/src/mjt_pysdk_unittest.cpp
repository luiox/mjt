// mjt_pysdk_unittest.cpp — mjt-py SDK 绑定层单测。
//
// 结构：C++ facade 直测（mjt_pysdk_facade_test.cpp）+ 嵌入解释器跑
// pysdk_selftest.py（函数面全链断言）。python 初始化一次，全部测试共享。
//
// 前置：mjt_pysdk（mjt.pyd）已随 deps 构建并拷贝 mjt_util.py 到同目录；
// rundir = $(projectdir)（MJT_UTIL_DIR 回退源 mjt-py/src）。

#include <gtest/gtest.h>

#include <Python.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "pysdk_embed.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

bool pyRunOK(const std::string& code)
{
    PyObject* main = PyImport_AddModule("__main__");
    PyObject* dict = PyModule_GetDict(main);
    PyObject* r    = PyRun_String(code.c_str(), Py_file_input, dict, dict);
    if (!r) {
        PyErr_Print();
        return false;
    }
    Py_DECREF(r);
    return true;
}

/// 嵌入式跑 selftest，返回失败清单（python list[str] → vector<string>）。
std::vector<std::string> runSelftest()
{
    PyObject* module = PyImport_AddModule("__main__");
    PyObject* dict   = PyModule_GetDict(module);
    PyObject* r      = PyRun_String(
        "import pysdk_selftest\n"
        "fails = pysdk_selftest.run_all()\n",
        Py_file_input, dict, dict);
    if (!r) {
        PyErr_Print();
        return {"selftest: python execution failed"};
    }
    Py_DECREF(r);
    PyObject* fails = PyDict_GetItemString(dict, "fails");
    std::vector<std::string> out;
    if (fails && PyList_Check(fails)) {
        for (Py_ssize_t i = 0; i < PyList_Size(fails); ++i) {
            PyObject* item = PyList_GetItem(fails, i);
            out.push_back(PyUnicode_AsUTF8(item));
        }
    }
    return out;
}

}   // namespace

// ── 绑定层（嵌入 python，跑 selftest 全链） ──────────────────────────────

TEST(PysdkBinding, SelftestAllGreen)
{
    auto fails = runSelftest();
    for (const auto& f : fails) {
        ADD_FAILURE() << f;
    }
}

#ifndef PYSDK_UNITTEST_NO_MAIN

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);

    // Windows：先拉 python314.dll 进进程，再初始化解释器（utf8_mode=1 + home）。
    if (!mjt::pysdk::pythonPreInit()) return 2;
    if (!mjt::pysdk::pythonInit()) {
        std::fprintf(stderr, "pysdk unittest: interpreter init failed\n");
        return 2;
    }

    // mjt.pyd / mjt_util.py 搜索点：exe 目录 + 仓内 mjt-py 源目录（回退）。
    // 源目录定位（P2 修）：MJT_PYSDK_SRC 显式 > exe 目录逐级上溯找 mjt-py/src >
    // cwd 兜底——不再依赖运行时 cwd 恰为仓根。
    mjt::pysdk::pythonAddExePath();
    std::filesystem::path repoPysdk;
    if (const char* env = std::getenv("MJT_PYSDK_SRC")) {
        repoPysdk = env;
    }
    else {
        const auto exeDirPath = std::filesystem::path(mjt::pysdk::exeDir());
        for (auto dir = exeDirPath; !dir.empty(); dir = dir.parent_path()) {
            const auto cand = dir / "mjt-py" / "src";
            std::error_code ec;
            if (std::filesystem::exists(cand / "mjt_util.py", ec)) {
                repoPysdk = cand;
                break;
            }
        }
    }
    if (repoPysdk.empty()) {
        repoPysdk = std::filesystem::current_path() / "mjt-py" / "src";
    }
    _putenv_s("MJT_UTIL_DIR", repoPysdk.u8string().c_str());
    {
        const std::string code =
            "import sys\n"
            "sys.path.insert(0, r'" + mjt::pysdk::exeDir() + "')\n"
            "sys.path.insert(0, r'" + repoPysdk.u8string() + "')\n";
        PyRun_SimpleString(code.c_str());
    }

    const int rc = RUN_ALL_TESTS();

    mjt::pysdk::pythonFinalize();
    return rc;
}

#endif   // PYSDK_UNITTEST_NO_MAIN
