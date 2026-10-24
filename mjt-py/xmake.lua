-- ── mjt-py 组件（with_pysdk；开源基座面）────────────────────────────────
-- 独立顶层 SDK 组件（与 mjt 平级，消费 mjbase 基座）：
--   mjt_py_embed          静态库：--script 嵌入解释器（纯 CPython API，无 pybind11），
--                         由宿主 exe 消费；
--   mjt_pysdk             mjt.pyd：pybind11 裸 binding，基座面 + mjt/shared 反编译
--                         执行器（decompiler_compare 双后端复用 mjt/src/shared，
--                         与 mjt-mcp 同一实现）。方法级 pass 桥属私有侧管线消费
--                         面，不在开源基座实现；Python 类型锚（MethodPass/
--                         PreservedAnalyses/changed()/preserved_all()）保持脚本
--                         面形状，私有侧适配器按同款锚接自家管线；
--   mjt_pysdk_unittest    组件测试（C++ facade gtest + 内嵌解释器跑 Python 自测；
--                         exe 兼任 fake 反编译器 java 替身）。
-- util 层 mjt_util.py（纯 Python）随 pyd 同目录部署。
if has_config("with_pysdk") then
    local pyHome = get_config("python_home")
    if pyHome == nil or pyHome == "" then
        pyHome = os.getenv("MJT_PYTHON_HOME") or "D:/sdk/python/Python314"
    end
    --烧进字符串字面量的路径必须归一为正斜杠：反斜杠里的 \3（八进制）、\x…
    --（十六进制）会被 C++ 编译器当转义序列吃掉（CI 的 hostedtoolcache
    --Python\3.14.9\x64 实证）。Windows API 对正斜杠路径同接受。
    pyHome = pyHome:gsub("\\", "/")
    local pybind11Root = os.getenv("MORPHER_PYBIND11_ROOT") or "D:/sdk/cpp/pybind11-3.1.0"
    -- mjt shared 反编译执行器（shared/decompiler.hpp 的 include 根）。
    local mjtSharedSrc = path.join(os.scriptdir(), "..", "mjt", "src")

    target("mjt_py_embed")
        set_kind("static")
        set_group("mjt-py")
        set_default(false)

        add_includedirs("src", {public = true})
        -- 只挂头文件路径，不链 python314：exe 对 Python 零加载期依赖（纯
        -- MCP/用法模式在无 Python 的机器上可起，绿色部署前提）。CPython 入口
        -- 经 pysdk_embed.cpp 的运行时绑定（GetProcAddress）解析；unittest/
        -- embedtest 宿主如需直连可自行挂 libs（当前同样走运行时绑定）。
        add_includedirs(path.join(pyHome, "include"))
        add_defines('MJT_PYTHON_HOME_DEFAULT="' .. pyHome .. '"')
        add_files("src/pysdk_embed.cpp", "src/pysdk_limits.cpp")

        if is_plat("windows") then
            add_cxflags("/utf-8", {tools = "cl"})
        end

    target("mjt_pysdk")
        set_kind("shared")
        set_group("mjt-py")
        set_basename("mjt")
        set_extension(".pyd")
        set_default(false)

        add_deps("mjbase")
        -- 包面全部经 mjbase public 包传导（libjbc/libjzip/libca/libjhasm/...
        -- 与 mjbase 同版本）：本仓内联 libjbc 包是旧组件版（libjstr 0.0.1 缺
        -- MUtf8StringRef 双参构造），显式 add_packages 会把旧 lib dir 排进
        -- 链接序，与 mjbase 包的 0.0.2 撞版本（LNK2019 实证）。消费方式与
        -- mjt-mcp binary 同款：只挂 deps + 显式闭包。
        add_includedirs("src")
        add_includedirs(mjtSharedSrc)
        add_includedirs(path.join(pybind11Root, "include"))
        add_includedirs(path.join(pyHome, "include"))
        add_linkdirs(path.join(pyHome, "libs"))
        add_links("python314")

        if is_plat("windows") then
            add_cxflags("/utf-8", {tools = "cl"})
        end
        add_files("src/pysdk_jar.cpp", "src/pysdk_facade.cpp", "src/pysdk_jasm.cpp",
                  "src/pysdk_limits.cpp", "src/bind_core.cpp", "src/bind_facade.cpp")
        add_files(path.join(mjtSharedSrc, "shared", "decompiler.cpp"),
                  path.join(mjtSharedSrc, "shared", "jar_remapper.cpp"))

        -- xmake 3.x 不把静态 target 依赖传进 shared 链接：显式给全闭包，按
        -- 「消费者在前、被依赖在后」排序（MSVC 对序不敏感，此序为 GNU ld 兼容
        -- 口径；链面与 mjt-mcp binary 同款，mcp 编排专有件除外）。
        mjt_binary_link_closure("mjbase", "libjhasm", "libjblw",
                                "libjbytecode_util", "libjbytecode_commons",
                                "libjbytecode_analysis", "libjbytecode_tree",
                                "libjbytecode_core", "libgarena", "libjclassfile",
                                "libjstr", "libjbc", "libjzip")

        -- util 层随 pyd 同目录部署（--script / 外部 import mjt 都从 pyd 旁加载）。
        after_build(function (target)
            os.cp(path.join(os.scriptdir(), "src", "mjt_util.py"), target:targetdir())
        end)

    if has_config("with_tests") then
        target("mjt_pysdk_unittest")
            set_kind("binary")
            set_group("mjt-py/test")
            set_default(false)

            add_deps("mjbase")
            add_packages("gtest")
            add_includedirs("src")
            add_includedirs(mjtSharedSrc)
            add_includedirs(path.join(pybind11Root, "include"))
            add_includedirs(path.join(pyHome, "include"))
            add_linkdirs(path.join(pyHome, "libs"))
            add_links("python314")
            add_files("src/pysdk_jar.cpp", "src/pysdk_facade.cpp", "src/pysdk_jasm.cpp",
                      "src/pysdk_embed.cpp", "src/pysdk_limits.cpp",
                      "src/mjt_pysdk_unittest.cpp", "src/mjt_pysdk_facade_test.cpp")
            add_files(path.join(mjtSharedSrc, "shared", "decompiler.cpp"),
                      path.join(mjtSharedSrc, "shared", "jar_remapper.cpp"))
            mjt_binary_link_closure("mjbase", "libjhasm", "libjblw",
                                    "libjbytecode_util", "libjbytecode_commons",
                                    "libjbytecode_analysis", "libjbytecode_tree",
                                    "libjbytecode_core", "libgarena", "libjclassfile",
                                    "libjstr", "libjbc", "libjzip")
            set_rundir("$(projectdir)")
            add_defines('MJT_PYTHON_HOME_DEFAULT="' .. pyHome .. '"')

            if is_plat("windows") then
                add_cxflags("/utf-8", {tools = "cl"})
            end
            if is_plat("linux") then
                add_syslinks("pthread", "dl", "util")
            end
        end

    -- 嵌入宿主进程级测试：--script 的退出码契约含 watchdog 硬退出（_Exit(3)）
    -- 与逐场景独立解释器生命周期，必须整进程观测——单进程 gtest 进不来。
    -- 无 gtest：plain main，无参时逐场景 spawn 自身并断言退出码。
    target("mjt_pysdk_embedtest")
        set_kind("binary")
        set_group("mjt-py/test")
        set_default(false)

        add_deps("mjbase")
        add_includedirs("src")
        add_includedirs(path.join(pyHome, "include"))
        add_linkdirs(path.join(pyHome, "libs"))
        add_links("python314")
        add_files("src/pysdk_embed.cpp", "src/pysdk_limits.cpp", "src/mjt_pysdk_embedtest.cpp")
        add_defines('MJT_PYTHON_HOME_DEFAULT="' .. pyHome .. '"')
        set_rundir("$(projectdir)")

        if is_plat("windows") then
            add_cxflags("/utf-8", {tools = "cl"})
        end
        if is_plat("linux") then
            add_syslinks("pthread", "dl", "util")
        end
end
