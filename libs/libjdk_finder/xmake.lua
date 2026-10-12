-- libjdk_finder — JDK 与原生工具链（MSVC/gcc/clang）探测
--
-- 零外部依赖（仅 std + libca）。供需要定位 JDK JNI 头与 C 编译器的产品功能
-- （mj2x-cli --compile）和测试侧共用，避免探测逻辑在产品代码与测试代码各写一份。
target("libjdk_finder")
    set_kind("static")
    set_group("libs")

    -- jdk_finder.hpp include libca/core/datatype.hpp；public 供 test_helper 等消费方传递。
    add_packages("libca", {public = true})
    add_files("src/jdk_finder/*.cpp")
    add_headerfiles("src/jdk_finder/**.hpp")
    add_includedirs("src", {public = true})

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

-- gtest 面随 with_tests 门控：默认配置不定义本 target、不拉 gtest（D15）。
if has_config("with_tests") then
target("libjdk_finder_unittest")
    set_kind("binary")
    set_group("libs/test")
    add_packages("libca")
    set_default(false)

    add_deps("libjdk_finder", "test_helper")
    -- xmake 3.x 不把静态 target 依赖传进 binary 链接：显式给全闭包，按
    -- 「消费者在前、被依赖在后」排序（Linux CI 与 Windows 同一口径）。
    -- test_helper 依赖 libjdk_finder 与 libca_test，二者均依赖 libca_core。
     mjt_binary_link_closure("test_helper", "libjdk_finder")
    add_packages("gtest")
    add_files("unittest/main.cpp")
    add_files("unittest/*_test.cpp")
    add_includedirs("src")
    set_rundir("$(projectdir)")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
end
