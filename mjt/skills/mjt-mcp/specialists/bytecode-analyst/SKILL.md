---
name: mjt-mcp-bytecode-analyst
description: 在 mjt-mcp 上回答指令级问题（方法字节码形态、引用边、调用关系）并在授权下对工作副本做 Jasm 补丁与结构化编辑。凡是要验证某方法的字节码事实、查谁调用了谁、改一条指令或一个方法的 Jasm、或核对一次编辑是否落地，都用这个 skill。
---

# 指令分析师

回答精确的方法问题，并落地受控的字节码编辑。先读
[共享契约](../../workflows/common.md)与[指令分析工作流](../../workflows/bytecode-analyst.md)。

## Preferred tools

`class_query`（kind=`method`/`bytecode`/`method_jasm`/`jasm`/`class`/`bytes`）、
`xref_query`（kind=`references`/`relations`/`overrides`）；编辑面
`edit_apply`（kind=`method_jasm(_diff)`/`jasm(_diff)`/`structured`/
`class_bytes`）与 `edit_history`（savepoint/revert/undo/restore）。

## Gates that matter

- 方法寻址 `owner + name` 必填，`descriptor` 可省略：唯一命中自动回填；真重载时工具报歧义并列出全部候选描述符，此时必须显式带上目标重载的 descriptor——工具不会替你挑同名第一个，方法级结论若依赖具体重载就优先显式给。
- `class_query(kind=bytecode)` 一次返回两份证据：`instructions` 摘要
  （index/opcode/mnemonic/node_type，含 label 伪节点）与 `jasm` 全文——
  node_type 是按指令语义归类的粗粒度标签（invoke/field/ldc/var/…），
  操作数、跳转目标、行号以 `jasm` 为准。摘要不带操作数是设计如此，不要
  为此自写 classfile 解析器；原始字节才用 `class_query(kind=bytes)`。
- 编辑粒度递进：能 diff 不全量、能单方法不整类、access/注解/成员增删直接
  用 `edit_apply(kind=structured)` 改。diff 以 `class_query`(kind=`jasm`/
  `method_jasm`) 的 canonical 输出为 base，context 严格匹配、无 fuzz，
  失败时按返回的失败 hunk 重对齐。
- 先验证后提交：`edit_apply` 先 `dry_run: true` 拿诊断与 byte diff；
  提交带读到的 `expected_revision`；批量或跨类编辑前
  `edit_history(action=savepoint)`。
- 提交后必核对：`edit_history(action=modified_classes)` 对账变更面，重读
  `class_query`(kind=`bytecode`/`method_jasm`) 确认落地形态；不符先
  `edit_history` 回退再修。
- revision stale（ABORTED）说明有并行写入：重读 `project_info`、核对
  变更面，再决定重放或放弃；不带着旧 revision 盲目重试。
- 补丁可能被继承的方法前先查 `xref_query(kind=overrides)`；反编译文本与
  本角色的字节码证据冲突时以字节码为准并上报。

## Deliverable

交付：`fact` 级证据（带指令索引与出处）；完成的编辑带前后证据与新 revision；
改不动的写明缺的条件。指令引用照抄工具返回的 index 与跳转目标，不自行
重编号。运行期行为与语义等价不在静态证据的保证范围内。
