add_rules("plugin.compile_commands.autoupdate", {outputdir = "."})

set_project("mjt")
set_version("0.1.0")
set_xmakever("2.9.3")
set_allowedplats("windows", "linux", "macosx")
set_defaultplat("windows")
set_defaultarchs("x64")

set_languages("c99", "cxx17")
add_rules("mode.debug", "mode.release")
set_warnings("all")

-- 全线静态 CRT（/MT，debug 档自动映射 /MTd）：注入型 agent 与宿主 JVM 捆绑的
-- 旧 CRT 混链是 LNK2038/AV 根因，静态链接后不再依赖宿主进程的 CRT 解析。
-- xrepo 包按项目 runtime 自动以 MT 口味重装。Linux/macOS 不受此设置影响。
if is_plat("windows") then
    set_runtimes("MT")
end

-- 日志门面编译期级别=Trace：CA_LOG_DEBUG/TRACE 一律编入，可见性由 Logger
-- 运行期级别与各调用点既有门控负责。
add_defines("CA_COMPILE_LOG_LEVEL=0")

function mjt_artifact_dir(line)
    return path.join("$(builddir)", "artifacts", "$(plat)", "$(arch)", "$(mode)", line)
end

function mjt_set_artifact_dir(line)
    set_targetdir(mjt_artifact_dir(line))
end

-- 测试/工具 binary 的静态库链接闭包（add_links + linux 闭包组）。
-- GNU ld 单遍从左到右扫描归档，闭包列表必须消费库在前、被依赖库在后；
-- MSVC link.exe 对库顺序不敏感。声明序即闭包序（xmake 3.0.9 实证）。
function mjt_binary_link_closure(...)
    local libs = {...}
    local unpack = table.unpack or unpack
    add_links(unpack(libs))
    if is_plat("linux") then
        add_linkgroups(unpack(libs))
    end
end

-- mjbase 单测闭包（与 mjbase 仓同名实现；过渡期本仓不编 mjbase_unittest，
-- 但 mjbase/mjbase/xmake.lua 在 with_tests 下会引用它）。
function mjbase_binary_link_closure(...)
    local libs = {...}
    local unpack = table.unpack or unpack
    add_links(unpack(libs))
    if is_plat("linux") then
        add_linkgroups(unpack(libs))
    end
end

-- 禁用 RTTI：dynamic_cast 替换为 typed::cast<T>()。
if is_plat("windows") then
    add_cxflags("/GR-", {tools = "cl"})
    add_cxflags("/EHa", {tools = "cl"})
    add_cxflags("/wd4541", {tools = "cl"})
    -- 关掉 xmake 默认追加的 /EHsc：与 /EHa 同传会触发 MSVC D9025。
    set_exceptions("none")
else
    add_cxxflags("-fno-rtti")
end

if is_plat("windows") then
    set_toolchains("msvc")
end

if is_host("windows") then
    set_plat("windows")
    set_toolchains("msvc")
end

-- 构建开关：必须最先 include，后续用 has_config("with_xxx") 判断。
-- 命令行用下划线形式：xmake f --with_tests=y 等。
includes("xmake/options.lua")

-- Windows CLI exe 的 activeCodePage=UTF-8 manifest 规则（GBK ACP 下中文 argv
-- 修复）：规则仅声明，binary target 经 add_rules 显式接入。
includes("xmake/win_utf8_manifest.lua")

-- AddressSanitizer：MSVC 下必须在全局作用域开启，让所有 target（含静态库）
-- 用同一套插桩，否则链接期 LNK2038。不要与 gtest targets 组合。
option("with_asan")
    set_default(false)
    set_showmenu(true)
    set_description("Build with AddressSanitizer (global).")
option_end()

if has_config("with_asan") then
    set_optimize("none")
    set_symbols("debug")
    if is_plat("windows") then
        add_cxflags("/fsanitize=address", {tools = "cl"})
        add_defines("_DISABLE_VECTOR_ANNOTATION", "_DISABLE_STRING_ANNOTATION")
        add_ldflags("/INCREMENTAL:NO", {tools = "link"})
    else
        add_cxflags("-fsanitize=address", "-fno-omit-frame-pointer")
        add_ldflags("-fsanitize=address")
    end
end

-- gtest：仅在启用测试时通过 xrepo 拉取。
if has_config("with_tests") then
    add_requires("gtest")
end

-- libarchive：mbase ZipUtil 的跨平台解压后端。
-- 本地覆盖包（xmake/repo/packages/l/libarchive/xmake.lua）规避官方包在
-- Linux 漏传 POSIX_REGEX_LIB=NONE 导致的编译失败。
add_repositories("mjt-repo xmake/repo")
add_requires("libarchive 3.8.8", {configs = {openssl3 = false}})
add_requires("zlib")
add_requires("nlohmann_json 3.12.0")

-- 中心包仓直连包（无仓内副本、无 phony 垫片，消费点直接 add_packages）。
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")
-- libca 版本/模块双单源：顶层与全部嵌套路径一律归并到同一 (版本, 模块集)。
-- 家族包内声明的 libca 依赖无版本约束且是小模块超集（core,str,log,crypto,zip），
-- 放任自解析会多出实例；消费方目标同名包去重后落到无 json 的实例时，
-- 链接期 LNK2019 ca::json（D33 同族根因）。升级 libca 只改下面两行。
LUIOX_LIBCA_VERSION = "0.0.13"
LUIOX_LIBCA_MODULES = "core,crypto,str,fs,io,json,net,http,process,opt,thread,log,zip,resources,i18n,test"
add_requires("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})
-- test 模块必须显式点名：test_helper 调用 ca::test::top_project_path()，
-- 缺模块时链接期 undefined reference；包 on_load 按依赖闭包展开排序链接项。
-- luiox 家族直连包消费 0.0.2（libca 0.0.14 对齐版）。
add_requires("libmcp 0.1.0")
add_requires("libjstr 0.0.2")
add_requires("libjbytecode 0.0.2")
add_requires("libjzip 0.0.2")
add_requires("libgarena 0.0.2")
add_requires("libjclassfile 0.0.2")
-- libjbc 走项目内联包定义（git 直连 + commit 锁，与 mjbase 同版本）：
-- luiox-repo 的发布版落后于 mjbase 需要的面，且内联定义不吃 requireconfs，
-- 其嵌套 libca 由定义内的 add_deps 钉到上面的单源版本。
includes("xmake/packages/libjbc.lua")
add_requires("libjbc 0.4.15")
add_requires("libjstd 0.0.1")
add_requires("libjdk_finder 0.0.1")
add_requires("libjblw 0.0.1")
add_requires("libjhasm 0.0.1")
-- 嵌套 libca 全路径钉定，缺一条就多一个实例。
add_requireconfs("libmcp.libca",                 {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjstr.libca",                {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjbytecode.libca",           {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjbytecode.libjstr.libca",   {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjbytecode.libgarena.libca", {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjzip.libca",                {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libgarena.libca",              {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjclassfile.libca",          {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjclassfile.libjstr.libca",  {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjstd.libca",                {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjdk_finder.libca",          {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjblw.libca",                        {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjblw.libjbytecode.libca",           {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjblw.libjbytecode.libjstr.libca",   {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjblw.libjbytecode.libgarena.libca", {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libca",                       {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libjblw.libca",               {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libjbytecode.libca",          {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libjbytecode.libjstr.libca",  {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libjbytecode.libgarena.libca",{version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
add_requireconfs("libjhasm.libjblw.libjbytecode.libca",  {version = LUIOX_LIBCA_VERSION, configs = {modules = LUIOX_LIBCA_MODULES}})
-- i18n/resources 构建规则内嵌（.lang add_files 规则，包安装产物不带规则）。
includes("xmake/libca_rules.lua")

-- 过渡形态：mjbase 以源码级依赖接入（默认兄弟目录 ../mjbase），供 mcp 与
-- mjt-py 从 mbase 切到 mjbase 的本地验证使用。mjbase 打包进 luiox-repo 后
-- 改为 add_requires("mjbase") 消费，本段随之删除。
--
-- 必须在 includes("mjt") 之前：mjt 的 target 要在配置期就能引用 mjbase，
-- 且需要下面的 MJBASE_ENABLED 判断共存链接。
option("mjbase_root")
    set_default("../mjbase")
    set_showmenu(true)
    set_description("Path to the mjbase source tree (transitional source-level dep).")
option_end()

local mjbase_root = get_config("mjbase_root")
MJBASE_ENABLED = mjbase_root and mjbase_root ~= ""
                 and os.isdir(path.join(os.projectdir(), mjbase_root))
if MJBASE_ENABLED then
    includes(path.join(mjbase_root, "mjbase"))
end

includes("test_helper")
includes("mbase")
includes("mjt")
includes("unittest")
