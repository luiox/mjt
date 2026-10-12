-- libjblw: C++ rewrite of dev.xdark.blw.

target("libjblw")
    set_kind("static")
    set_group("libs")

     add_packages("libjbytecode", {public = true})
    add_includedirs("src", {public = true})
    add_files("src/jblw/**.cpp")
    add_headerfiles("src/jblw/**.hpp")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

if has_config("with_tests") then
target("libjblw_unittest")
    set_kind("binary")
    set_group("libs/test")
    add_packages("libca")
    set_default(false)
    add_deps("libjblw", "test_helper")
    -- xmake 3.x 不把静态 target 依赖传进 binary 链接：显式给全闭包，按
    -- 「消费者在前、被依赖在后」排序（Linux CI 与 Windows 同一口径）。
    -- libjblw 依赖 libjbytecode 聚合库（其实现分散在 core/tree/analysis/commons/util
    -- 子库），聚合库本身只有版本符号，必须逐个补齐子库；test_helper 自身依赖
    -- libca_test/libjdk_finder，一并补入。
     mjt_binary_link_closure("libjblw", "libjbytecode_util", "libjbytecode_commons", "libjbytecode_analysis", "libjbytecode_tree", "libjbytecode_core", "libgarena", "libjstr", "test_helper", "libjdk_finder")
    add_packages("gtest")
    add_files("test/main.cpp")
    add_files("test/*_test.cpp")
    add_includedirs("src", "test")
    set_rundir("$(projectdir)")
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
end
