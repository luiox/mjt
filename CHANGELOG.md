# Changelog

本文件记录 mjt-mcp 使用者可见的行为变化，遵循 Keep a Changelog 的分节
习惯，版本号以实际发布为准。

## Unreleased

### Breaking changes

- MCP 工具面收敛为 11 个粗粒度工具：`project_open`、`project_close`、
  `project_info`、`container_mount`、`class_query`、`search`、`xref_query`、
  `decompile`、`edit_apply`、`edit_history`、`project_export`。旧细粒度
  工具名一次性退役，无别名过渡期；调用未注册名返回 `unknown tool`。
  迁移对应关系：class 查询族 → `class_query` 的 kind 选择器；检索族 →
  `search` 的 kind 选择器（含 shadow 双向查询）；xref 族 →
  `xref_query`；validate/apply 对 → `edit_apply` 的 `dry_run` 两态；
  `job_get`/`job_cancel`/`job_release` → `decompile` 的 action 选择器；
  savepoint/restore/revert/undo/redo/compact → `edit_history`。
- 会话寻址键 `workspace_id` 更名 `project_id`；`project_open` 打开或创建
  工程 bundle，不再有游离的内存工作区。
- `analysis_workflow`、`class_fidelity_check`、`decompiler_decompile`、
  `decompiler_compare`、`jar_diff`、`jar_fix_*`、`shadow_generate`、
  `shadow_prune`、`decompiler_settings_*`、`pass_run`、`pass_describe`、
  `opt_preview`/`opt_apply` 自 MCP 面退役：对应能力转入 SDK（后续卡片），
  服务端不再提供 MCP 等价物。反编译参数（backend、`decompiler_jar`、
  `java`、`timeout_ms`、`max_memory_mib`、`max_source_bytes`）改为
  `decompile` 按调用注入，不再有会话级 settings。
- `container_mount` 本轮只支持 support 角色的 mount/detach；role=primary
  由 `project_open` 承担，library/runtime 角色留待后续波次。

### Added

- `project_open` / `class_query` / `search` / `xref_query` 增可选
  `timeout_ms`（缺省 180000，上限 300000，与 `decompile` 的超时上限对齐）。
  超时返回结构化错误（`reason=timeout` + `elapsed_ms`），执行体转后台收尾，
  stdio 通道与已打开工程会话保持可用；`project_open` 超时错误携带预订的
  `project_id`，后台打开完成后该会话照常可寻址。
- 查询与反编译超时的诊断面（#71）：超时错误不再静默挂 deadline——
  `class_query` / `search` / `xref_query` / `project_open` 的超时 payload 附
  `hints` 数组，按工具与 kind 给收敛/分页/粒度指引（宽查询收敛、
  `page_size` + `cursor` 切片、大类 jasm 换 `method_jasm` 单方法粒度与
  pysdk 侧批量导出通道）；`decompile` 反编译子进程超时的失败面（同步与
  async job 两个出口）附通道指引（`async=true` + `job_get` 轮询、方法级
  粒度、pysdk 通道）。仅超时路径写入，成功路径零变化。

### Fixed

- `project_export` 导出保真（#74）：MANIFEST 无编辑时产物逐字节复制源条目
  （LF 行尾、续行折行、主属性段外尾随内容原样，不再被 CRLF 重序列化改写）；
  移除「检测到 `META-INF/versions/` 条目就自动注入 `Multi-Release: true`」
  的推断——源 jar 里处于惰性状态的版本化覆盖不再被导出激活；产物条目集
  与工作副本一致，不再为文件条目合成无内容目录占位。源 jar 无 MANIFEST
  条目且含 JEP 238 版本化类（≥9）时维持补最小 MANIFEST 的收口契约，其余
  无 MANIFEST 形态产物照旧不带该条目；导出不再原地改写会话内工作副本的
  MANIFEST 资源。
- `edit_apply` 结构化编辑的写回校验门按 mutation 种类分档：不触碰方法体的
  旗标/成员表面编辑（访问位、成员表增删、注解、局部名）不再被整类
  rebuild-validate 拒绝——类内先在坏帧此前会让这类编辑在 `validation`
  默认档下完全不可用（#73）。触碰方法体的编辑（换体/指令编辑/新建方法体）
  维持整类门；门失败时同档位重验先在字节，先在字节同样不过门的错误信息
  注明归因（先在问题，非本次编辑引入）。`validation=off` 直通与 byte_diffs
  行为不变。
