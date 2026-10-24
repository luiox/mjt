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
