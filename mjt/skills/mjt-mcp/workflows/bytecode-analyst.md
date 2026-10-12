# 指令分析工作流：证据与受控编辑

配合[指令分析 skill](../specialists/bytecode-analyst/SKILL.md)与
[共享契约](common.md)使用。工具 schema 以 `tools/list` 为权威。

## 输入与范围

接受三元组或类级范围、要回答的问题、以及（编辑任务时）授权边界。
只读问题到证据为止；编辑任务必须带明确的变更描述——"看看能不能改"
不是编辑授权。

## 门 1：立事实

三元组定位 → `method_get` 确认存在与元数据 → `method_bytecode` 拿现状
（响应同时含指令摘要与 `jasm` 全文；操作数、跳转目标、行号以 `jasm`
为准，抽象方法摘要为空属正常）。需要单独的规范化文本时才调
`method_jasm`。需要引用面时 `xref_method`/`xref_field`/
`method_callers`；虚调用语义用 `method_callees`（`resolved: true`，结果
注明是解析后目标，不是唯一可能）；改动可能影响继承前查 `method_overrides`，
层级问题查 `class_relations`。

检查：每条事实带指令索引与出处；引用返回的 index 与跳转目标，
不自行重编号。有界求值 `analysis_evaluate` 的结果只作 candidate 佐证。

## 门 2：定方案

把"要改什么"落成精确编辑：目标方法、目标指令/行、预期形态。选粒度：

| 粒度 | 验证 | 提交 |
| --- | --- | --- |
| 单方法局部 diff | `method_validate_jasm_diff` | `method_apply_jasm_diff` |
| 单方法整体替换 | `method_validate_jasm` | `method_apply_jasm` |
| 整类局部 diff | `patch_validate_jasm_diff` | `patch_apply_jasm_diff` |
| 整类 Jasm | `patch_validate_jasm` | `patch_apply_jasm` |
| access/注解/成员/指令结构化小改 | —（`patch_apply` 自带 dry_run） | `patch_apply` |
| 外部整类字节替换 | —（服务端校验） | `patch_replace_class_bytes` |

检查：能 diff 不全量、能单方法不整类；debug/局部名编辑依赖既有表，
表缺失或锚点歧义时停下，不发明条目。批量或跨类编辑前
`workspace_savepoint`（命名快照，不动 revision）。

## 门 3：验证

`dry_run`/`*_validate_*` 拿诊断与 byte diff。diff 以 `class_jasm`/
`method_jasm` 的 canonical 输出为 base，context 严格匹配、无 fuzz；
失败时按返回的失败 hunk 与期望行重对齐后重试一次。检查：byte diff 与
预期形态逐条对得上；对不上就停下修方案——"先提交再看看"不存在。

## 门 4：提交与核对

带读到的 `expected_revision` 提交。成功后三件事：

1. 刷新登记的 revision；
2. `workspace_modified_classes` 对账变更面与预期一致；
3. 重读 `method_bytecode`/`method_jasm` 确认落地形态。

不符则回退——单类 `workspace_revert_class`（恢复到打开时原始字节，产生
新 revision）、单步 `workspace_undo`、回快照 `workspace_restore`——修正
后重走门 3。

## 恢复

- stale（ABORTED）：有并行写入。重读 `workspace_info`，核对变更面，
  决定重放或放弃；不带旧 revision 盲试。
- 验证失败：诊断里找原因（栈形、label、描述符）；改方案而不是绕过校验。
- diff 反复失配：重新拉取当前 `class_jasm` 作 base——旧文本属于旧
  revision。
- 需要源码语义才能继续时派符号修复角色，拿到结论回来继续；不阻塞在
  猜测上。

## 完成

只读任务：事实齐、问题可答即为 complete，答不了写明缺的证据。编辑任务：
落地形态核对通过才算 complete；核对未过但已提交即为 partial 并附回退
建议。运行期链接与语义等价永远不在声明范围内。
