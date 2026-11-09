add_rules("plugin.compile_commands.autoupdate", {outputdir = "."})

set_project("mjt")
-- 工具版本单源：set_version 与 mjt_shared 的 MJT_VERSION 宏（producer 注册表
-- toolFingerprint 自报，见 workspace/analysis_ingest）共用同一字面量。
MJT_VERSION = "2.3.0"
set_version(MJT_VERSION)
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

-- zlib：libjzip 传递需要；nlohmann_json：MCP 协议载荷。
add_requires("zlib")
add_requires("nlohmann_json 3.12.0")

-- sqlite3：mjbase 基座 analysis_db（project 模块）的存储引擎。mjbase 已切
-- luiox-repo 包消费，本仓根仍显式 require 钉版本/配置（静态 3.53.0 单实例），
-- 链接闭包在 mjt/xmake.lua 的 mjt_binary_link_closure 里显式点名。
add_requires("sqlite3 3.53.0", {configs = {shared = false}})

-- 中心包仓直连包（无仓内副本、无 phony 垫片，消费点直接 add_packages）。
add_repositories("luiox-repo https://github.com/luiox/luiox-repo.git")
-- libca 版本/模块双单源：顶层与全部嵌套路径一律归并到同一 (版本, 模块集)。
-- 历史上家族包内声明的 libca 依赖无版本约束且是小模块超集（core,str,log,
-- crypto,zip），放任自解析会多出实例；消费方目标同名包去重后落到无 json 的
-- 实例时，链接期 LNK2019 ca::json（D33 同族根因）。现 luiox-repo 各包定义
-- 已全线自钉与本行逐字一致的 17 模块超集（版本裸名或兼容下界，由消费图
-- 顶层 require 决议），单源靠包定义自钉维持。升级 libca 只改下面两行。
-- 0.0.16 = 与 mjbase main 及其包词条（0.0.5+）消费版本对齐，消两仓 skew。
LUIOX_LIBCA_VERSION = "0.0.16"
LUIOX_LIBCA_MODULES = "core,crypto,str,fs,env,io,json,net,http,process,opt,thread,log,zip,resources,i18n,test"
add_requires("libca " .. LUIOX_LIBCA_VERSION, {configs = {modules = LUIOX_LIBCA_MODULES}})
-- test 模块必须显式点名：test_helper 调用 ca::test::top_project_path()，
-- 缺模块时链接期 undefined reference；包 on_load 按依赖闭包展开排序链接项。
-- libjstr/libjzip/libjhasm 与 mjbase main 根 require 清单字面对齐（mjbase
-- 包词条按同清单声明，消费图版本不一致会实例分裂）。mjbase 包词条对
-- libjstr/libjzip 为裸名 add_deps：实测不吃消费图顶层钉版（不合并、按包仓
-- 最新浮动），故本行钉版必须逐字跟随其浮动目标（= luiox-repo 包仓最新
-- 词条），落后即双实例分裂——mjt#78（根钉 0.0.3 + 传递浮动 0.0.5，同进程
-- 两份取档，0 字节 STORED 条目回垃圾字节）即此病。升级只在 mjbase 包词条
-- 浮动目标上移时同步跟版，两行一起改、不得单边。
add_requires("libmcp 0.1.0")
add_requires("libjstr 0.0.4")
add_requires("libjzip 0.0.5")
add_requires("libjclassfile 0.0.2")
-- libjbc 走项目内联包定义（git 直连 + commit 锁，≥ mjbase 门控下界 0.4.22）：
-- luiox-repo 的发布版落后于 mjbase 需要的面，且内联定义不吃 requireconfs，
-- 其嵌套 libca 由定义内的 add_deps 钉到上面的单源版本。
includes("xmake/packages/libjbc.lua")
add_requires("libjbc 0.4.23")
add_requires("libjstd 0.0.1")
add_requires("libjdk_finder 0.1.1")
add_requires("libjhasm 0.3.3")
-- 嵌套 libca 不再 requireconf 钉定：libmcp/libjstr/libjzip/libjclassfile/
-- libjstd/libjdk_finder/libjhasm 的包定义 add_deps 已自钉与本仓逐字一致的
-- modules 超集（libmcp 为 >=0.0.7 兼容下界，其余裸名走消费图版本决议），
-- requireconf 重复声明无信息量；包定义漂移（modules 或版本约束偏离本行
-- 超集）时再按路径补钉。
-- i18n/resources 构建规则内嵌（.lang add_files 规则，包安装产物不带规则）。
includes("xmake/libca_rules.lua")

-- mjbase 基座：luiox-repo 包消费（git 直连 + commit 锁）。包词条按源仓根
-- require 清单声明依赖；本仓 require 清单与其字面对齐（PR-A），消费图实例
-- 单一。兄弟目录源码级依赖接线（--mjbase_root / CI checkout）已随切换拆除；
-- mjbase 侧开发走包词条发版节奏。
-- 0.0.8 = analysis-db v3 条目身份层（B1：inputs/entries + classes.entry_id）；
-- 0.0.9 = LVT 显示名策略判定核心（B3：analysis/lvt_display_policy）；
-- 0.0.10 = analysis-db 出口遮蔽层（B2：suspect 扫描 + mask_rules 表 +
-- 信任级别 meta + recordMaskRuleHits 记账 + mask_evaluator 求值器核心）+
-- 性能调优（B6：打开面 PRAGMA + refs_source_class 索引 + 按源反查两步走）；
-- 0.0.11 = census 普查投影（B4：censusClasses/censusContainer 查询面 +
-- 0.0.12 = entry_shape 补 Overlong/WhitespaceEdge 两 facet + mapping 转义
-- 增补平面补全（#136）；census 超长单元格折叠随 #50 消费。
-- 0.0.13 = 按源成员引用切片替换（G1）+ 库前缀分类核心（L1）+ 引用与层次
-- 闭包分页变体（G4）+ censusClasses origin 派生列（#139/#140/#141/#142）；
-- 本行须待 luiox-repo 追加对应词条后方可配置。
-- 0.0.14 = refs 目标侧 id 化（schema v5：双可空外键 + 整目标二元化 + 双分支查询，#149，旧 v4 库 fail-loud 拒绝属预期，临时库重导入即可）。
-- 0.0.15 = member 普查投影面（DbMemberCensusRow + censusMembers(inputId) 复合键行序，#150）。
-- 0.0.16 = producer 注册表批 1-3（schema v6：producers/batches/batch_scopes +
-- renames 行级 producer_id，写入口批次参数化，#151；旧 v5 库 fail-loud 属预期，
-- 临时库重建即愈）。
-- 0.0.17 = method_versions 方法版本指纹表（schema v8：32 字节 BLOB +
-- fp_version 算法版本列 + is_current 部分唯一索引当前指针，#154；旧 v7 库
-- fail-loud 属预期，临时库重建即愈）。
-- 0.0.18 = jar 导入同名重复条目改末现保留（对齐 JVM 类加载口径，PR #160）+
-- asm/remapper 注解递归畸形输入边界守卫（#158）；词条面无新增依赖。
-- 0.0.19 = 全程序方法可达闭包入分析库（schema v9：closure_batches/
-- closure_members + 查询面 project/reachability_closure，PR #162）；词条面
-- 无新增依赖。
-- 0.0.20 = 真改名身份迁移（AnalysisDb::migrateIdentities：单事务批量改写
-- classes.internal_name / members.name+descriptor，class_id / member_id 与
-- renames 覆盖行零触碰，undo 反向规格对称回滚，PR #198）。
-- 0.0.21 = 台账事务身份迁移注记（LedgerTransaction::identity 可空旁车：
-- 真改名旧值→新值语义随事务原子落账，undo/redo/restore 反推库迁移的反向/
-- 前向规格，PR #200）+ MemberRemapPlan 枚举面（classMappings/fieldRules/
-- methodRules，applyRemap 漏斗推导注记的输入）——shadow 落分析库（DB 为
-- renames 覆盖层唯一真源、sidecar 退役为 open 期一次性导入源）的 undo/redo
-- 持久化前提；词条面无新增依赖。
-- 0.0.22 = jasm assemble 保留原帧档 FrameMode（PR #197，mjbase#190：
-- FrameMode{Regenerate,PreserveOriginal} + ResourceJasmService::assemble
-- 第 5 参缺省兼容——v49 杠杆跳过 smt_build 帧序列化层（unnamed slot/
-- returnAddress 形可救，数据流分析失败两档同败）+ fail-closed pc 对齐门；
-- pysdk apply 族 frame 档消费本档）；词条面无新增依赖。
-- 0.0.23 = 台账头位持久化——头指针行 row=head（PR #202）：LedgerRow 增
-- Head 行型承载 undo 态头位（head 落后于链尾），闭合「undo 作为末动作落盘、
-- 重开静默复活到链尾」的格式缺陷；条件写出保常态字节不变；persistedHead_
-- 头位脏跟踪使 undo-only 会话 flush 落换代。open 期身份自愈（#88）的 undo
-- 方向对账前提——undo 后反迁移失败的库行名分歧，重开须工作副本停在 undo
-- 位而非复活改名态，阶段二路径对账才有靶子。
-- 0.0.24 = analysis_db 类文本搜索面（PR #203）：searchClassesPaged——活跃
-- 锚类跨输入并集、internal_name/path 双面、contains/prefix/exact（LOWER
-- ASCII 折叠与扫描面同口径 + GLOB/= 字节档）、owner/packagePrefix 作用域、
-- (path, internal_name) BINARY 行序、页附 total。消费方 = search
-- kind=classes DB-first 接线（干净会话走库，编辑会话回落引擎扫描）。
-- 0.0.25 = analysis_db 类头引用切片替换（PR #204）：replaceReferencesFromClass
-- ——按类头切片替换引用行（super/interfaces 等 Type 引用），batchId 锚与
-- ingestClasses 同 (2, class_id)。消费方 = 编辑增量同步 analysis_edit_sync
-- 的逐类引用切片写入（commit/undo/redo 后类事实/条目锚/引用切片追平工作
-- 副本，search kind=classes DB-first 解除干净会话守卫）。
add_requires("mjbase 0.0.26")

includes("test_helper")
includes("mjt")
includes("unittest")
-- mjt-py（with_pysdk 门控；mjbase 基座消费方，须在 mjbase includes 之后）。
if has_config("with_pysdk") then
    includes("mjt-py")
end
