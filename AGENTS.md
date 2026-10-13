# AGENTS.md

`mjt` 是 JVM 字节码工具单仓：`mjt/` 是产品线（MCP 服务器），`mbase/`、`libs/`、
`test_helper/` 是共享层。

## 命令

```sh
xmake f --with_tests=y        # 开测试
xmake build <target>          # 一次一个 target
xmake run <target> --gtest_brief=1
```

目标：`mjt-mcp`（产品）、`mjt_mcp_core` / `mjt_shared`（库）、`mjt_mcp_unittest`
（需 `--with_tests=y`）。

外部依赖只有 luiox-repo 中心包仓直连包（libca / libmcp / libjstr /
libjbytecode / libjzip / libgarena / libjclassfile + xrepo 的 gtest /
libarchive / zlib / nlohmann_json），不引入仓内副本、不做 phony 垫片；
各包嵌套的 libca 必须用 `add_requireconfs` 钉回同一 `LUIOX_LIBCA_VERSION`。

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

## mbase 分叉治理

本仓 `mbase/`（253 文件）是 morpher 主仓 `mbase/` 的开源分叉：air/sir/info 已剜除，
命名空间整体重写为 `mjt::base`。治理规则：

- **分叉身份**：本仓 `mbase/` 是旧栈（libjbytecode 底座）的唯一活跃维护面；公开产品
  的 mbase bug 就地在本仓修复。
- **上游关系**：morpher 主仓的 `mbase/` 是冻结快照（mjbase 重写战役的对拍 oracle
  基线），不接收回流；禁止往 morpher 的 mbase 打补丁。
- **修复流向（单向）**：本仓修完后，同一修复回流到 morpher 侧的 **mjbase**（重写线），
  提交注记 `mjt <hash> 同源`。
- **外部 PR**：触碰 `mbase/` 时接受修复、拒绝功能性新增（旧栈不加功能）；结构性变更
  应去 morpher 上游提案。
- **寿命**：mjbase 迁移完成并经 luiox-repo 打包后，本目录整体删除、切换为消费 mjbase
  包，届时闭包缩为 `mcp/skills/src`。
- **从 morpher 同步修复**：① 取 morpher 侧修复 diff；② 内容移植到本仓 `mbase/` 对应
  文件；③ 命名空间重写 `morpher::base` → `mjt::base`（含 namespace 声明与限定名）；
  ④ 跳过涉及已剜除模块（air/sir/info）的改动，include 按本仓布局调整；⑤ 按构建门
  流程重跑受影响单测。

## 提交

- 不提交测试产物与临时文件；提交前 `git status`。
- 提交信息用约定式前缀并带模块作用域：`feat(mcp): …`、`fix(mbase): …`。
