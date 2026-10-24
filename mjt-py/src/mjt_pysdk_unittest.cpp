// mjt_pysdk_unittest.cpp — mjt-py SDK 绑定层单测。
//
// 结构：C++ facade 直测（mjt_pysdk_facade_test.cpp）+ 嵌入解释器跑
// pysdk_selftest.py（函数面全链断言）。python 初始化一次，全部测试共享。
//
// 前置：mjt_pysdk（mjt.pyd）已随 deps 构建并拷贝 mjt_util.py 到同目录；
// rundir = $(projectdir)（MJT_UTIL_DIR 回退源 mjt-py/src）。
//
// exe 兼任 fake 反编译器 java 替身（先例：mjt_mcp_unittest 的
// runFakeDecompilerProcess）：以 `<本exe> ... -jar fake-pysdk-*.jar <输入>
// --outputdir <目录>` 再入时，逐 jar 内 class 条目写出占位 .java 并退出，
// 供 decompiler_compare 在无真实 CFR/Vineflower 的环境实跑双后端。

#include <gtest/gtest.h>

#include <Python.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"

#include "pysdk_embed.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

/// fake 反编译器分派：识别 `-jar fake-pysdk-*.jar` 调用形；非本替身调用
/// 返回 -1（回落正常 gtest main）。
int runFakeDecompiler(int argc, char** argv)
{
    int jarIndex = -1;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "-jar") {
            jarIndex = index + 1;
            break;
        }
    }
    if (jarIndex < 0) return -1;
    const auto jarName = std::filesystem::path(argv[jarIndex]).filename().string();
    if (jarName != "fake-pysdk-decompiler.jar" && jarName != "fake-pysdk-vineflower.jar") {
        return -1;
    }

    // mjt decompile 的 JAR 命令形（双后端）：CFR = `... -jar <jar> <input>
    // --outputdir <dir>`；Vineflower = `... -jar <jar> <input> <dir>`（无旗标）。
    if (jarIndex + 2 >= argc) return 7;
    const std::filesystem::path input = argv[jarIndex + 1];
    std::filesystem::path output;
    if (jarIndex + 3 < argc && std::string(argv[jarIndex + 2]) == "--outputdir") {
        output = argv[jarIndex + 3];
    }
    else {
        output = argv[jarIndex + 2];
    }
    if (input.empty() || output.empty()) return 7;

    mjbase::model::ResourceContainer container;
    auto imported = mjbase::model::ResourceHelper::importFromJar(container, input.u8string());
    if (imported.is_err()) return 7;
    std::error_code ec;
    std::filesystem::create_directories(output, ec);
    if (ec) return 7;
    container.visitResources([&](std::string_view uri,
                                 const mjbase::model::IResource& resource) {
        const auto* cls =
            ca::core::tag_cast::dyn_cast<const mjbase::model::ClassResource>(&resource);
        if (cls == nullptr) return;
        // getLocation() = jar 条目名（含 .class 后缀）；反编译源路径按内部名。
        std::string internal(cls->getLocation());
        constexpr char kSuffix[] = ".class";
        if (internal.size() > sizeof(kSuffix) - 1 &&
            internal.compare(internal.size() - (sizeof(kSuffix) - 1),
                             sizeof(kSuffix) - 1, kSuffix) == 0) {
            internal.resize(internal.size() - (sizeof(kSuffix) - 1));
        }
        if (internal.find("..") != std::string::npos) return;
        const auto target = output / std::filesystem::u8path(internal + ".java");
        std::filesystem::create_directories(target.parent_path(), ec);
        const auto slash   = internal.rfind('/');
        const std::string simple =
            slash == std::string::npos ? internal : internal.substr(slash + 1);
        std::ofstream source(target, std::ios::binary | std::ios::trunc);
        source << "// fake pysdk decompiled source\npublic class " << simple
               << " {\n  public static int value() { return 42; }\n}\n";
    });
    std::cout << "fake pysdk decompiler ok\n";
    return 0;
}

}   // namespace

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
    // fake 反编译器替身分派先行（mjt_mcp_unittest 同款）：命中即以替身退出。
    const auto fakeResult = runFakeDecompiler(argc, argv);
    if (fakeResult >= 0) return fakeResult;

    ::testing::InitGoogleTest(&argc, argv);

    // fake java 替身的再入路径 = 本 exe 全路径（selftest 经环境变量取用）。
    // 必须在解释器初始化前落 CRT 环境：os.environ 在 import os（init 期间）
    // 时对 CRT environ 做快照，之后再 _putenv_s 不可见。
#ifdef _WIN32
    {
        char exeBuf[MAX_PATH] = {0};
        if (GetModuleFileNameA(nullptr, exeBuf, MAX_PATH) > 0) {
            _putenv_s("MJT_PYSDK_FAKE_JAVA", exeBuf);
        }
    }
#endif

    // Windows：先拉 python314.dll 进进程，再初始化解释器（utf8_mode=1 + home）。
    if (!mjt::pysdk::pythonPreInit()) return 2;
    if (!mjt::pysdk::pythonInit()) {
        std::fprintf(stderr, "pysdk unittest: interpreter init failed\n");
        return 2;
    }

    // mjt.pyd / mjt_util.py 搜索点：exe 目录 + 仓内 mjt-py 源目录（回退）。
    // 源目录定位：MJT_PYSDK_SRC 显式 > exe 目录逐级上溯找 mjt-py/src >
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
