# 指令分析工作流：证据与受控编辑

配合[指令分析 skill](../specialists/bytecode-analyst/SKILL.md)与
[共享契约](common.md)使用。工具 schema 以 `tools/list` 为权威。

## 输入与范围

接受三元组或类级范围、要回答的问题、以及（编辑任务时）授权边界。
只读问题到证据为止；编辑任务必须带明确的变更描述——"看看能不能改"
不是编辑授权。

## 门 1：立事实

三元组定位 → `class_query`(kind=`method`) 确认存在与元数据 →
`class_query`(kind=`bytecode`) 拿现状（响应同时含指令摘要与 `jasm` 全文；
操作数、跳转目标、行号以 `jasm` 为准，抽象方法摘要为空属正常）。需要
单独的规范化文本时才调 `class_query`(kind=`method_jasm`)。需要引用面时
`xref_query`(kind=`references`, `direction=to|from`)；虚调用语义用
`direction=from`（`resolved: true` 时结果注明是解析后目标，不是唯一
可能）；改动可能影响继承前查 `xref_query`(kind=`overrides`)，层级问题查
`xref_query`(kind=`relations`)。

检查：每条事实带指令索引与出处；引用返回的 index 与跳转目标，
不自行重编号。

## 门 2：定方案

把"要改什么"落成精确编辑：目标方法、目标指令/行、预期形态。选粒度
（全部走 `edit_apply`，`kind` 选择器分派；验证与提交是同一工具的
`dry_run` 两态）：

| 粒度 | kind | 验证 | 提交 |
| --- | --- | --- | --- |
| 单方法局部 diff | `method_jasm_diff` | `dry_run: true` | 去掉 dry_run |
| 单方法整体替换 | `method_jasm` | `dry_run: true` | 去掉 dry_run |
| 整类局部 diff | `jasm_diff` | `dry_run: true` | 去掉 dry_run |
| 整类 Jasm | `jasm` | `dry_run: true` | 去掉 dry_run |
| access/注解/成员/指令结构化小改 | `structured` | `dry_run: true` | 去掉 dry_run |
| 外部整类字节替换 | `class_bytes` | —（服务端校验） | 直接提交 |
| rename / mapping / 优化管线 | `rename` / `mapping` / `pipeline` | `dry_run: true` | 去掉 dry_run |

检查：能 diff 不全量、能单方法不整类；debug/局部名编辑依赖既有表，
表缺失或锚点歧义时停下，不发明条目。批量或跨类编辑前
`edit_history`(action=`savepoint`) 打台账书签（命名快照，不动 revision）。

## 门 3：验证

`dry_run: true` 拿诊断与 byte diff。diff 以 `class_query`(kind=`jasm`/
`method_jasm`) 的 canonical 输出为 base，context 严格匹配、无 fuzz；
失败时按返回的失败 hunk 与期望行重对齐后重试一次。检查：byte diff 与
预期形态逐条对得上；对不上就停下修方案——"先提交再看看"不存在。

## 门 4：提交与核对

带读到的 `expected_revision` 提交。成功后三件事：

1. 刷新登记的 revision；
2. `edit_history`(action=`modified_classes`) 对账变更面与预期一致；
3. 重读 `class_query`(kind=`bytecode`/`method_jasm`) 确认落地形态。

不符则回退——单类 `edit_history`(action=`revert_class`，恢复到打开时
原始字节，产生新 revision)、单步 `edit_history`(action=`undo`)、
回书签 `edit_history`(action=`restore`)——修正后重走门 3。

## 恢复

- stale（ABORTED）：有并行写入。重读 `project_info`，核对变更面，
  决定重放或放弃；不带旧 revision 盲试。
- 验证失败：诊断里找原因（栈形、label、描述符）；改方案而不是绕过校验。
- diff 反复失配：重新拉取当前 `class_query`(kind=`jasm`) 作 base——
  旧文本属于旧 revision。
- 需要源码语义才能继续时派符号修复角色，拿到结论回来继续；不阻塞在
  猜测上。

## 完成

只读任务：事实齐、问题可答即为 complete，答不了写明缺的证据。编辑任务：
落地形态核对通过才算 complete；核对未过但已提交即为 partial 并附回退
建议。运行期链接与语义等价永远不在声明范围内。
