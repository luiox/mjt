-- ── 共享库（供 MCP 核心与 unittest 共用） ─────────────────────
target("mjt_shared")
    set_kind("static")
    set_group("mjt")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

    add_deps("mjbase")
    add_packages("libjhasm")

    add_includedirs("src", {public = true})

    add_files("src/shared/*.cpp")
    -- workspace 引擎（自 mjbase workspace 模块移交，ns mjt::workspace）：编辑
    -- 台账/类变更/引用·摘要扫描，MCP 工具面与 pysdk 绑定共用的底座件。
    add_files("src/workspace/**.cpp")
    add_files("src/shared/translations/*.lang", {rules = "libca.i18n.embed-lang"})

target("mjt_mcp_core")
    set_kind("static")
    set_group("mjt")

    if is_plat("windows") then
        add_cxflags("/utf-8", {tools = "cl"})
    end

    add_deps("mjt_shared", "mjbase")
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
    if has_config("with_pysdk") then
        -- --script 宿主接线：嵌入解释器静态库（纯 CPython API）。
        -- MJT_WITH_PYSDK 供 main.cpp 分派编译期分支；mjt_py_embed 只挂头文件
        -- 路径不链 python314——exe 对 Python 零加载期依赖（纯 MCP/用法模式
        -- 在无 Python 的机器上可起，绿色部署前提），CPython 入口经
        -- pysdk_embed.cpp 的运行时绑定解析（/DELAYLOAD 不可行：PyExc_* 等
        -- 数据导出 delayimp 不覆盖，LNK1194 实证）。
        -- Python.h 的 pyconfig.h 会 pragma comment(lib,"python314.lib") 内嵌
        -- /DEFAULTLIB 指令，nodefaultlib 显式抵消（无符号引用，安全）。
        if is_plat("windows") then
            add_ldflags("/nodefaultlib:python314.lib", {force = true})
        end
        add_deps("mjt_py_embed")
        add_defines("MJT_WITH_PYSDK=1")
    end
    -- GNU ld 单遍从左到右扫描归档：binary 必须显式给全静态闭包（消费者在前、
    -- 被依赖在后），否则 auto 排序把 libca 包归档排到 mjt_shared 前，链接期
    -- 报 ca::process 等符号 undefined（Linux CI 实证）。闭包序与 unittest 同款。
     mjt_binary_link_closure("mjt_mcp_core", "mjt_shared", "mjbase")
    mjt_binary_link_closure("libjhasm", "libjclassfile", "libjstr", "libjbc", "libjzip")
    add_files("mcp/src/main.cpp")

    set_rundir("$(projectdir)")

if has_config("with_tests") then
    target("mjt_workspace_unittest")
        set_kind("binary")
        set_group("mjt/test")
        set_default(false)

        add_deps("mjt_shared", "test_helper")
        add_packages("gtest")
        -- xmake 3.x 不把静态 target 依赖传进 binary 链接：显式给全闭包，按
        -- 「消费者在前、被依赖在后」排序（与 mjt_mcp_unittest 同款）。
         mjt_binary_link_closure("mjt_shared", "mjbase")
        mjt_binary_link_closure("libjhasm", "libjclassfile", "libjstr", "libjbc", "libjzip", "test_helper", "libjdk_finder")
        add_files("test/workspace/*.cpp")
        set_rundir("$(projectdir)")

        if is_plat("windows") then
            add_cxflags("/utf-8", {tools = "cl"})
        end
        if is_plat("linux") then
            add_syslinks("pthread")
        end

    target("mjt_mcp_unittest")
        set_kind("binary")
        set_group("mjt/test")
        set_default(false)

        add_deps("mjt_mcp_core", "mjt_shared", "test_helper")
        add_packages("gtest")
        -- xmake 3.x 不把静态 target 依赖传进 binary 链接：显式给全闭包，按
        -- 「消费者在前、被依赖在后」排序，另含 libmcp 及其 http/net/io 链。
         mjt_binary_link_closure("mjt_mcp_core", "mjt_shared", "mjbase")
        mjt_binary_link_closure("libjhasm", "libjclassfile", "libjstr", "libjbc", "libjzip", "test_helper", "libjdk_finder")
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

