-- libdex 包定义（项目内联定义，git 直连 + commit 锁）
--
-- 为什么项目内联：libdex（github.com/luiox/libdex，DEX 单文件读写库：
-- 单 DexFile lazy DOM + 保真写回 + smali 互转）在 luiox-repo 中心包仓尚无
-- 词条。先例同 libjbc：add_urls 指向 git 仓库 + add_versions 第二参数锁
-- commit，xmake 用系统 git 克隆并 checkout 到锁定 commit，随后在本包沙箱
-- 内独立构建安装（不进消费工程的 target 图）。待 luiox-repo 追加 libdex
-- 词条（只追加不修改）后，本文件退役、消费方改走中心包词条。
--
-- 安装面：libdex 工程共 7 个 target——静态库 libdex / libsmali 两个是
-- 交付物；dexbuild/dexperf/smalict/libdex_leak_check 均 set_default(false)
-- 不进默认构建；libdex_unittest 由 with_tests 门控（install 传
-- with_tests=false，gtest 不入沙箱 require 图）。默认构建即两库，无需
-- on_install 布局修补：headerfiles 模式 src/(dex/*.hpp) 与
-- src/(smali/**.hpp) 的括号组保留目录前缀，产物 include/{dex,smali}/
-- 天然与公开头的 #include "dex/..."、#include "smali/..." 对齐。
--
-- 链接说明：双库（links = libsmali + libdex，依赖者在前、被依赖在后——
-- GNU ld 单遍扫描下 libsmali 对 libdex 的引用须由其后出现的 libdex 归档
-- 解析；MSVC link.exe 对顺序不敏感）。libdex 工程自身 xmake.lua 已声明
-- libca/libjstr 依赖，包沙箱内自解析；下方 add_deps 是给消费方的传递
-- 声明（另一回事，不可省）。libca 版本/模块取 root xmake.lua 的
-- LUIOX_LIBCA_VERSION/LUIOX_LIBCA_MODULES 家族统一超集（同一消费图内
-- 各包的 libca (版本, 模块集) 必须完全一致——实例身份 = (版本, configs
-- 哈希)，任何偏差都会多出一个 libca 实例：链接闭包多条 -libpath，同名
-- 符号双归档，解析次序脆弱）。
--
-- 构建口径传导：/MT 全线静态 CRT 由消费工程 set_runtimes("MT") 经
-- package.tools.xmake 传入沙箱（同族包同款）；/GR- 无 RTTI 与 /utf-8
-- 由 libdex 工程自身 xmake.lua 在沙箱内自带（同 mjt 全局口径）；本文件
-- on_load 补 /utf-8（windows）是给消费方编译期用的——公开头含 UTF-8
-- 中文注释，GBK ACP 的机器上 on_test 探针与消费方编译单元必须按 UTF-8
-- 读源，否则注释吞行致语法错（机制与 libjbc.lua 全同，见其文件头）。
--
-- 许可：MIT（源仓 set_license("MIT")）。

package("libdex")
    set_homepage("https://github.com/luiox/libdex")
    set_description("DEX single-file read/write library: lazy DexFile DOM with fidelity write-back and smali bridge (git direct package)")
    set_license("MIT")

    add_urls("https://github.com/luiox/libdex.git")

    -- 语义版本名 + 锁定 commit。版本条目只追加、不修改。
    -- 0.0.1 = libdex 自身 set_version("0.0.1")（尚无 tag，锁 main 头）。
    add_versions("0.0.1", "a5fae5a3c788d8d85bf32bd21633a84a9525927a") -- #23 main 头（docs 拆篇）  2026-11-11

    -- 嵌套 libca 直接钉到本仓单源版本：项目内联包定义不吃 deps.lua 的
    -- **.libca requireconfs（实测，同 libjbc），裸声明会解析到包仓最新版，
    -- 与项目实例（LUIOX_LIBCA_VERSION）形成双版本 libca——D33 同族隐患。
    -- 版本/模块常量取 root xmake.lua 全局（本文件在常量定义之后 include，
    -- 作用域可见）；configs 必须与项目级 add_requires 完全一致，否则同一
    -- 消费图内多出一份 libca 实例（链接闭包多条 -libpath）。
    add_deps("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})
    -- libjstr（MUTF-8 容器）与 zlib：与源仓根 require 清单字面对齐
    -- （libdex xmake.lua add_requires("libjstr 0.0.4") + zlib——libca_zip
    -- 的消费方自带 zlib，libca 包不传递声明）。本仓根已逐字 require 同版
    -- 本，消费图单实例。
    add_deps("libjstr 0.0.4")
    add_deps("zlib")

    on_load(function (package)
        -- 双库交付：依赖者在前、被依赖在后（见文件头链接说明）。
        package:add("links", "libsmali")
        package:add("links", "libdex")
        -- 语言标准传导：公开头使用 C++17（同 libjstr/libjbc 包口径）。
        if package:is_plat("windows") then
            package:add("cxxflags", "/std:c++17")
            -- 公开头含 UTF-8 中文注释，消费方编译期（含本包 on_test 探针）
            -- 必须按 UTF-8 读源，GBK ACP 下否则注释吞行致语法错（见文件头
            -- 说明）。
            package:add("cxxflags", "/utf-8")
        else
            package:add("cxxflags", "-std=c++17")
        end
    end)

    on_install("windows", "linux", "macosx", function (package)
        -- 包安装只装两库本体：dexbuild/dexperf/smalict/leak_check 均为
        -- set_default(false) 的开发态目标，unittest 由 with_tests=false
        -- 摘除（gtest 不入沙箱）。
        import("package.tools.xmake").install(package, {with_tests = false})
    end)

    on_test(function (package)
        assert(package:has_cxxincludes("dex/dex.hpp", {configs = {languages = "cxx17"}}))
        -- 链接级自检：dex::get_lib_version 强制解析 libdex 的符号链
        -- （缺库即在此暴露）。
        assert(package:check_cxxsnippets({test = [[
            #include "dex/dex.hpp"
            int main(int argc, char** argv) {
                return dex::get_lib_version() != nullptr && argc >= 0 ? 0 : 1;
            }
        ]]}, {configs = {languages = "cxx17"}}))
    end)
package_end()
