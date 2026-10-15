-- ── 共享库（供 MCP 核心与 unittest 共用） ─────────────────────
target("mjt_shared")
    set_kind("static")
    set_group("mjt")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

    add_deps("mbase")
    add_packages("libjhasm")

    add_includedirs("src", {public = true})

    add_files("src/shared/*.cpp")
    add_files("src/shared/translations/*.lang", {rules = "libca.i18n.embed-lang"})

target("mjt_mcp_core")
    set_kind("static")
    set_group("mjt")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

    add_deps("mbase", "mjt_shared")
    add_packages("libmcp", {public = true})
    add_packages("libca")
    add_packages("libjbc")
    add_packages("nlohmann_json", {public = true})
    add_includedirs("mcp/src", {public = true})
    add_files("mcp/src/mcp_server.cpp")
    add_files("mcp/src/tool/*.cpp")

target("mjt-mcp")
    set_kind("binary")
    set_group("mjt")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

    -- Windows exe 的 activeCodePage=UTF-8 manifest（GBK ACP 下中文 argv
    -- 修复）：规则仅声明，binary target 经 add_rules 显式接入。
    add_rules("mjt.win_utf8_manifest")

    add_deps("mjt_mcp_core", "mjt_shared")
    -- GNU ld 单遍从左到右扫描归档：binary 必须显式给全静态闭包（消费者在前、
    -- 被依赖在后），否则 auto 排序把 libca 包归档排到 mjt_shared 前，链接期
    -- 报 ca::process 等符号 undefined（Linux CI 实证）。闭包序与 unittest 同款。
     mjt_binary_link_closure("mjt_mcp_core", "mjt_shared", "mbase", "libjhasm", "libjblw", "libjbytecode_util", "libjbytecode_commons", "libjbytecode_analysis", "libjbytecode_tree", "libjbytecode_core", "libgarena", "libjclassfile", "libjstr", "libjbc", "libjzip")
    add_files("mcp/src/main.cpp")

    set_rundir("$(projectdir)")

if has_config("with_tests") then
    target("mjt_mcp_unittest")
        set_kind("binary")
        set_group("mjt/test")
        set_default(false)

        add_deps("mjt_mcp_core", "mjt_shared", "test_helper")
        add_packages("gtest")
        -- xmake 3.x 不把静态 target 依赖传进 binary 链接：显式给全闭包，按
        -- 「消费者在前、被依赖在后」排序，另含 libmcp 及其 http/net/io 链。
         mjt_binary_link_closure("mjt_mcp_core", "mjt_shared", "mbase", "libjhasm", "libjblw", "libjbytecode_util", "libjbytecode_commons", "libjbytecode_analysis", "libjbytecode_tree", "libjbytecode_core", "libgarena", "libjclassfile", "libjstr", "libjbc", "libjzip", "test_helper", "libjdk_finder")
        -- 注：libca_i18n 为 headeronly target（无 .lib 产物），只入 add_deps 不入 add_links。
        add_includedirs("mcp/src")
        add_files("mcp/test/*.cpp")
        set_rundir("$(projectdir)")

        if is_plat("windows") then
            add_cxflags("/utf-8", {tools = "cl"})
        end
        if is_plat("linux") then
            add_syslinks("pthread")
        end
end

