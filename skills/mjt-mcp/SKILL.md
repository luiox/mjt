---
name: mjt-mcp
description: >
  mjt-mcp（JVM 字节码分析/改写 MCP 服务器）的使用技能包：11 个粗粒度工具覆盖
  工程会话、类查询、统一检索、交叉引用、CFR/Vineflower 反编译、候选事务式
  字节码编辑、台账回滚与派生 JAR 导出。凡是要摸清陌生 JAR 的结构与调用关系、
  追方法/字段引用、还原源码，或对字节码做可预览、可回滚的修改，都加载本包；
  再按会话性质取用 analysis.md（只读分析）或 editing.md（字节码改写）。
---

# mjt-mcp 使用技能包

mjt 是 JVM 字节码工具集的 MCP 服务器：stdio 传输（也支持 HTTP），进程名 `mjt-mcp`，`xmake run mjt-mcp` 启动后与模型客户端完成 MCP 握手。服务器侧注册 11 个粗粒度工具，schema 以运行中服务的 `tools/list` 为权威；本文按源码（`mjt/mcp/src/tool/contract.cpp` 的 `toolsList()`）核对，若升级后出现出入以 `tools/list` 实测为准。分析全部本地执行，不访问被分析产物的网络。

## 何时用这个技能

- 摸底陌生 JAR：类结构、成员签名、常量池字符串/数值、资源清单、MANIFEST。
- 追引用与层次：方法/字段/类的引用站点与调用边、继承闭包、override 组件。
- 源码还原：CFR / Vineflower 反编译单个类或单个方法，长任务走异步作业。
- 受控改写：结构化变更、Jasm / unified diff 补丁、类字节整替、优化管线、改名、mapping 应用；全部候选事务语义，可预览、可回滚、可导出。
- 多容器分析：library/runtime 角色挂载依赖与运行时，类解析按 primary > library > runtime 定序，同名条目 first-wins 且遮蔽记录透明上报。
- 跨会话持久：编辑台账、书签与分析缓存落盘工程 bundle，重开同一 `project_dir` 即恢复。

不适用：Android/DEX 产物；不加载、不执行目标 JAR 的业务代码。

## 11 工具面速览

| 工具 | 一句话能力 | 关键参数 |
| --- | --- | --- |
| project_open | 打开 JAR 建持久分析工程，取 bundle 单写者锁 | `path`（必填）；`project_dir`（省略 = 临时工程，close 即删）；`project_id`；`timeout_ms` |
| project_close | flush 台账与分析缓存后关闭会话、释放锁 | `project_id` |
| project_info | 工程总览：容器角色/指纹、revision、台账深度与压缩健康、可用反编译后端、锁持有者 | `project_id` |
| container_mount | 挂载/卸载支撑容器并登记进 project.json | `action`=mount/detach；`role`=library/runtime/support（primary 拒收）；mount 带 `path`，detach 带 `support_id` |
| class_query | 单类/资源精确查询：清单、结构摘要、完整元数据、原始字节、Jasm、方法详情、MANIFEST | `kind`（必填）；`class`/`uri`/`owner`+`name`(+`descriptor`)；分页 |
| search | 统一检索：类、成员、字符串、数值、指令、资源、shadow 显示名双向解析 | `kind`（必填）；`query`+`match_mode`+各 kind 专属键；未知键拒绝 |
| xref_query | 引用图查询：引用站点/调用边、继承关系、override 组件 | `kind`（必填）；`target`/`direction`/`resolved`/`class`/`owner`+`name`+`descriptor`/`role` |
| decompile | 单类或单方法反编译（CFR/Vineflower/jcdc 实验后端），异步作业 poll/cancel/release | `class` 或 `owner`+`name`+`descriptor`；`action`（缺省 decompile）；`async`；`decompiler`/`decompiler_jar`/`java`/`timeout_ms` |
| edit_apply | 唯一编辑入口：structured、Jasm 及其 diff、类字节整替、优化管线、改名、mapping | `kind`（必填）；`expected_revision`/`dry_run`/`label`/`validation`；各 kind 载荷 |
| edit_history | 台账操作：书签、恢复、单类回退、undo/redo、压缩、列览、已改类清单 | `action`（必填）；`name`/`class_uri`/`keep_recent_bookmarks`/`keep_undo_depth` |
| project_export | 把当前编辑状态物化为派生 JAR（原子写，永不触碰主容器） | `path`（必填） |

除 project_open 外，所有工具的第一个必填参数都是 `project_id`（project_open 的返回值）。

## 通用约定

- fail-loud schema：所有工具 `additionalProperties: false`；class_query/search/xref_query 按 kind 分支校验，分支不支持的键直接报 `unknown parameter … expected: …`，绝不静默忽略。选择器类参数（class/owner/name/descriptor/uri/signature 等）超 65535 字节即拒（JVM 常量池 UTF8 上限）。
- 分页与投影：`cursor`（不透明十进制串）与 `offset` 互斥，`limit` 与 `page_size` 互斥（缺省 50，上限 1000）；`fields` 投影按该结果种类的静态键集校验，未知键拒绝。
- 寻址：类名一律 JVM internal name（`a/b/C` 形态），也接受精确类资源 URI（`a/b/C.class`）。成员按 `owner`+`name`(+`descriptor`) 寻址；descriptor 省略时按成员名在宿主类内唯一解析回填，真重载报歧义并列出全部候选描述符，名字不存在报缺失。
- 业务错误走 `isError=true` 的结构化文档：`error`/`status`/`status_code`/`operation`/`project_id`/`revision`/`diagnostics`。这不是传输故障，按文案修正参数重发即可。
- Jasm：`class_query(kind=jasm|method_jasm)` 返回规范化全文，也是 `edit_apply` 的 diff 基线；指令的结构化 `instructions[]` 节点类型是粗粒度语义标签，操作数、跳转目标与行号以 jasm 文本为权威。方言细节见 [../../../docs/jasm-syntax.md](../../../docs/jasm-syntax.md)。
- 字节码证据只从本工具面取：`javap`、自写 classfile 解析器等旁路工具一律不用——它们的输出形态与工作副本指令索引对不上，取证后无法回指补丁位置，结论也不可复核。

## 典型工作流

1. `project_open(path)` 拿 `project_id`；要跨会话保留分析与编辑状态就给 `project_dir`（不给是临时工程，project_close 即删 bundle）。
2. 依赖库/JDK 参与解析时 `container_mount(role=library|runtime, path=…)`；响应透传 shadowed/surfaced 遮蔽变化。
3. 只读面：`project_info` 看健康 → `class_query`/`search`/`xref_query`/`decompile` 取证据。
4. 改写面：`edit_apply(dry_run=true)` 预览 → 确认后去掉 `dry_run` 提交，提交带 `expected_revision` 防过期写。
5. 出问题：`edit_history` 的 undo/redo/revert_class/restore 回滚；阶段成果先 savepoint 打书签。
6. `project_export(path)` 物化派生 JAR，`project_close` 收尾。

census（全量普查）不在 11 工具面内：jar 级类普查导出（`class_census.tsv`/`member_census.tsv`/`container_census.tsv`）与字符串直方图、调用点普查等只读分析面在 pysdk 批量路径上，用法见 [analysis.md](analysis.md) 的 census 一节。

## 错误与并发注意

- fail-loud 是契约：宁要结构化报错不要静默错数据。报错先读 `error` 与 `diagnostics` 文案，修正参数重发，不要原样重试。
- 超时面：project_open / class_query / search / xref_query 接受 `timeout_ms`（缺省 180000，上限 300000）。超时返回结构化错误并带 `partial=true`/`reason=timeout`；操作转孤儿继续执行至自然完成，stdio 通道、会话与其余工具不受阻塞。
- 大容器超时豁免面：宽检索（`search` 全库宽词）与大类 jasm/反编译全量输出在大容器上可能挂满 deadline，但单类粒度的 `class_query`、`xref_query`、`edit_apply` 不受影响——宽检索改用包前缀限定或收窄查询词分页推进，大类证据按类拆小取；超时不构成启用旁路工具（javap、自写解析器）的理由。
- 单工程互斥：工程 bundle 的 `store.lock` 是单写者锁，同一 `project_dir` 的第二个 project_open 失败（含跨进程）；同进程内工作区工具经执行锁串行，长工具不阻塞 project_close 与反编译作业轮询。
- 容量上限：同时最多 16 个打开工程，异步反编译作业最多 64 个；超限先 close 再开。
- close/export 前自动 flush pending 台账与分析缓存；flush 失败不关闭（数据安全优先于会话回收）。

## pysdk 批量替代路径

需要循环批处理（全罐普查、逐类清洗、批量折叠/改名建议）时改走 `mjt-mcp --script <script.py>` 嵌入 Python SDK（构建需 `--with_pysdk=y`）：`mjt.*` 句柄式门面 + `mjt.util` 普查助手，编辑原语齐全但无事务/revision 语义；deadline 与输出上限见 `mjt-py/doc_ai/`。

## 角色文档

| 会话性质 | 文档 | 取用时机 |
| --- | --- | --- |
| 分析型（只读） | [analysis.md](analysis.md) | 任务不改字节码：查询/检索/引用/反编译的分支细节、分页口径、census 与 pysdk 批量分析 |
| 改写型 | [editing.md](editing.md) | 任务要改字节码并产出 JAR：edit_apply 九种 kind、validation 三档、revision 门、台账回滚、导出 |

两个文档都只依赖同一份 `tools/list`，不引入权限或进程边界；一个会话按性质选一份加载即可。
