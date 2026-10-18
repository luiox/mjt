-- ── mjt-py 组件（with_pysdk；开源基座面）────────────────────────────────
-- 独立顶层 SDK 组件（与 mjt 平级，消费 mjbase 基座）：
--   mjt_py_embed          静态库：--script 嵌入解释器（纯 CPython API，无 pybind11），
--                         由宿主 exe 消费；
--   mjt_pysdk             mjt.pyd：pybind11 裸 binding，纯基座面——全部 TU 只碰
--                         mjbase/libjbytecode/libca。方法级 pass 桥与 run_pipeline
--                         属私有侧管线消费面，不在开源基座实现；Python 类型锚
--                         （MethodPass/PreservedAnalyses/changed()/preserved_all()）
--                         保持脚本面形状，私有侧适配器按同款锚接自家管线；
--   mjt_pysdk_unittest    组件测试（C++ facade gtest + 内嵌解释器跑 Python 自测）。
-- util 层 mjt_util.py（纯 Python）随 pyd 同目录部署。
if has_config("with_pysdk") then
    local pyHome = get_config("python_home")
    if pyHome == nil or pyHome == "" then
        pyHome = os.getenv("MJT_PYTHON_HOME") or "D:/sdk/python/Python314"
    end
    local pybind11Root = os.getenv("MORPHER_PYBIND11_ROOT") or "D:/sdk/cpp/pybind11-3.1.0"

    target("mjt_py_embed")
        set_kind("static")
        set_group("mjt-py")
        set_default(false)

        add_includedirs("src", {public = true})
        add_includedirs(path.join(pyHome, "include"))
        add_linkdirs(path.join(pyHome, "libs"))
        add_links("python314")
        add_defines('MJT_PYTHON_HOME_DEFAULT="' .. pyHome .. '"')
        add_files("src/pysdk_embed.cpp")

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
        add_includedirs(path.join(pybind11Root, "include"))
        add_includedirs(path.join(pyHome, "include"))
        add_linkdirs(path.join(pyHome, "libs"))
        add_links("python314")

        if is_plat("windows") then
            add_cxflags("/utf-8", {tools = "cl"})
        end
        add_files("src/pysdk_jar.cpp", "src/pysdk_facade.cpp",
                  "src/bind_core.cpp", "src/bind_facade.cpp")

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
            add_includedirs(path.join(pybind11Root, "include"))
            add_includedirs(path.join(pyHome, "include"))
            add_linkdirs(path.join(pyHome, "libs"))
            add_links("python314")
            add_files("src/pysdk_jar.cpp", "src/pysdk_facade.cpp", "src/pysdk_embed.cpp",
                      "src/mjt_pysdk_unittest.cpp", "src/mjt_pysdk_facade_test.cpp")
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
end
