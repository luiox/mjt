-- lib* 包的 unittest 消费区：库包化（luiox-repo 直连包）后，各库的测试目标
-- 集中于此——消费包本体 + 本仓 test_helper（JDK/路径测试基建仍在 mjt 侧）。
if has_config("with_tests") then

target("libjdk_finder_unittest")
    set_kind("binary")
    set_group("unittest")
    set_default(false)

    add_deps("test_helper")
    add_packages("libjdk_finder", "libca")
    -- xmake 3.x 不把静态 target 依赖传进 binary 链接：显式给全闭包，按
    -- 「消费者在前、被依赖在后」排序（Linux CI 与 Windows 同一口径）。
    -- test_helper 依赖 libca_test（包链接项提供）；libjdk_finder 已包化，
    -- 保留在闭包里维持 GNU ld 消费者在前顺序。
     mjt_binary_link_closure("test_helper", "libjdk_finder")
    add_packages("gtest")
    add_files("jdk_finder/main.cpp", "jdk_finder/*_test.cpp")
    add_includedirs("jdk_finder")
    set_rundir("$(projectdir)")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
    if is_plat("linux") then
        add_syslinks("pthread")
    end

target("libjhasm_unittest")
    set_kind("binary")
    set_group("unittest")
    set_default(false)

    add_deps("test_helper")
    add_packages("libjhasm", "libjbc", "libjstr", "libca")
    -- libjhasm 0.3.2 后端换 libjbc（B4）：libjblw/libjbytecode 出链；套件的
    -- 产物回读验证直连 libjbc readClass。libjhasm/libjbc/libjdk_finder 已
    -- 包化，保留在闭包里维持 GNU ld 消费者在前。
     mjt_binary_link_closure("libjhasm", "libjbc", "libjstr", "test_helper", "libjdk_finder")
    add_packages("gtest")
    add_files("jhasm/main.cpp", "jhasm/*_test.cpp")
    add_includedirs("jhasm")
    set_rundir("$(projectdir)")
    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end
    if is_plat("linux") then
        add_syslinks("pthread")
    end

end
