# AGENTS.md

`mjt` 是 JVM 字节码工具单仓：`mjt/` 是产品线（MCP 服务器），`mjt-py/` 是嵌入
Python SDK 组件（`--with_pysdk` 门控），`test_helper/` 是共享测试层。字节码基座
是 mjbase（独立仓，luiox-repo 包词条消费，见「mjbase 基座」）。无仓内共享层：外部
依赖经 luiox-repo 中心包仓直连（libjbc 走项目内联包定义）。

## 命令

```sh
xmake f --with_tests=y        # 开测试
xmake build <target>          # 一次一个 target
xmake run <target> --gtest_brief=1
```

目标：`mjt-mcp`（产品）、`mjt`（CLI）、`mjt_mcp_core` / `mjt_cli_core` /
`mjt_shared`（库）、`mjt_mcp_unittest` / `mjt_cli_unittest`（需
`--with_tests=y`）、`mjt_pysdk_unittest`（需 `--with_pysdk=y` 与
`--with_tests=y`）。

外部依赖只有 luiox-repo 中心包仓直连包（libca / libmcp / libjstr /
libjzip / libgarena / libjclassfile / libjstd / libjdk_finder /
libjhasm + xrepo 的 gtest / zlib / nlohmann_json；
libjbc 走项目内联包定义，git 直连 + commit 锁），不引入仓内副本、不做 phony
垫片；各包嵌套的 libca 必须用 `add_requireconfs` 钉回同一 `LUIOX_LIBCA_VERSION`。
libjbytecode / libjblw 已全退役（B7 摘 libjblw，B8 8.7 摘 libjbytecode）。

## 约定

- **Doxygen**：`///` 行注释；`/** */` 禁止。
- **No RTTI**：`dynamic_cast`/`typeid` 禁止，用 `typed::cast<T>` 家族。
- **Types**：`i32`/`u32`/`usize`/`Result<T,E>`；错误走 `Result` 值语义。
- **Strings**：`Utf8String` 家族；API 借用视图用 `std::string_view`，不新增
  `std::string` 存储（字节边界除外）。
- **Includes**：模块内 `"file.hpp"`，跨模块 `"module/path/file.hpp"`，禁 `../`。
- **测试输出**：`<project_root>/test/`（test-helper API），基线在 `test_resource/`。
- **构建门**：delete-then-rebuild——内容哈希增量会跳过"回到旧状态"的文件，
  信测试结果前先 `rm <exe> && xmake -r <target>`；构建与验证不并成一条命令。
- 多进程/多 agent 并行开发时用 `/tmp` 目录锁错峰构建。

## 加依赖前的判断

改动前先确认消费方是否真的需要。若某个库/模块只有一个测试或工具在用，
它通常应该被删掉而不是留着。**新增依赖必须说明它服务于哪个在用的消费方**；
没有消费方的模块不进仓。

## mjbase 基座

字节码基座是 **mjbase**（独立仓），本仓经 luiox-repo 包词条消费
（`add_requires("mjbase 0.0.31")`，git 直连 + commit 锁；0.0.7 起含
analysis_db/sqlite3 面；0.0.31 门控 libjbc>=0.4.22，本仓统一 0.4.27
单实例）：

- **依赖对齐**：包词条按 mjbase 源仓根 require 清单声明依赖；本仓根的
  libca/libjstr/libjzip/libjhasm 钉版必须与其字面对齐，版本不一致会在消费
  图裂出双实例（链接期 LNK2019 同族根因）。
- **升级节奏**：mjbase main 定版后在 luiox-repo 追加包词条（只追加不修改），
  本仓再 bump `add_requires` 版本号；不再有兄弟目录源码级直连。

## 提交

- 不提交测试产物与临时文件；提交前 `git status`。
- 提交信息用约定式前缀并带模块作用域：`feat(mcp): …`、`fix(mjt-py): …`。
