-- Windows CLI exe 统一嵌 activeCodePage=UTF-8 + longPathAware manifest（D36/#1110）。
--
-- 缺陷（D31 minzi 样本实测，work/minzi_addon_report/report.md 异常①）：
--   ANSI main(argc, argv) 的 exe 在 GBK ACP（中文 Windows 默认 936）下，CRT
--   按 ACP 解码命令行，而 jar 内符号名是 UTF-8 —— 中文类名传给 -c/--class
--   永不命中（字节口径错位），且部分命令零命中时静默无提示。
--   activeCodePage=UTF-8 manifest 让进程 ACP 切到 UTF-8，argv 以 UTF-8 交付，
--   与仓内 UTF-8 符号名同口径；零代码改动即愈（D31 用 SDK mt.exe 后嵌已实测）。
-- #1110 补充：追加 <longPathAware>true</longPathAware>（SMI/2016 命名空间），
--   声明进程可感知长路径；注册表 LongPathsEnabled=1 时 Win32 宽字符 API 放开
--   260 限制，与 mjt/src/shared/long_path.hpp 的 \\?\ 扩展前缀链路双保险
--   （未开注册表的机器忽略该设置，无害；activeCodePage 语义原样保留）。
--
-- 用法（仅 binary target，规则内部自守卫平台/种类）：
--   add_rules("mjt.win_utf8_manifest")
--
-- 适用面：ANSI 入口的 exe——mjt-mcp。宽字符入口（wmain）天然免疫 ACP 口径
--   问题，无需接入本规则。
--
-- 生效前提：activeCodePage 仅
--   Windows 10 1903 (build 18362)+ 识别；更早系统忽略该设置、回退改动前行为
--   （无害）。
--
-- 实现：/MANIFESTINPUT 让 link.exe 把本清单与链接器默认生成的清单合并后按
--   RT_MANIFEST/1 嵌入（VS2015+），无 post-link mt.exe 步骤。改 manifest 后
--   只重链不重编（链接器输入变化即触发重链）。

rule("mjt.win_utf8_manifest")
    on_load(function (target)
        if not is_plat("windows") then return end
        if target:kind() ~= "binary" then return end
        local manifest = path.join(os.projectdir(), "xmake", "win_utf8.manifest")
        assert(os.isfile(manifest), "utf8 argv manifest missing: " .. manifest)
        target:add("ldflags", "/MANIFEST:EMBED", "/MANIFESTINPUT:" .. manifest, {force = true})
    end)
