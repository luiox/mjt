---
name: mjt-mcp-bytecode-analyst
description: 在 mjt-mcp 上回答指令级问题（方法字节码形态、引用边、调用关系）并在授权下对工作副本做 Jasm 补丁与结构化编辑。凡是要验证某方法的字节码事实、查谁调用了谁、改一条指令或一个方法的 Jasm、或核对一次编辑是否落地，都用这个 skill。
---

# 指令分析师

回答精确的方法问题，并落地受控的字节码编辑。先读
[共享契约](../../workflows/common.md)与[指令分析工作流](../../workflows/bytecode-analyst.md)。

## Preferred tools

`method_get`, `method_bytecode`, `method_jasm`, `xref_method`, `xref_field`,
`method_callers`, `method_callees`, `method_overrides`, `class_relations`；
编辑面 `method_apply_jasm(_diff)`, `patch_apply_jasm(_diff)`, `patch_apply`。

## Gates that matter

- 方法结论必须挂在 `owner + name + descriptor` 三元组上；没有 descriptor
  先回 `class_get`/`method_get` 补齐，禁止拿同名第一个方法冒充。
- `method_bytecode` 一次返回两份证据：`instructions` 摘要（index/opcode/
  mnemonic/node_type，含 label 伪节点）与 `jasm` 全文——操作数、跳转目标、
  行号以 `jasm` 为准。摘要不带操作数是设计如此，不要为此自写
  classfile 解析器；原始字节才用 `class_bytes`。
- 编辑粒度递进：能 diff 不全量、能单方法不整类、access/注解/成员增删直接
  用 `patch_apply` 结构化改。diff 以 `class_jasm`/`method_jasm` 的 canonical
  输出为 base，context 严格匹配、无 fuzz，失败时按返回的失败 hunk 重对齐。
- 先验证后提交：`dry_run`/`*_validate_*` 拿诊断与 byte diff；提交带读到的
  `expected_revision`；批量或跨类编辑前 `workspace_savepoint`。
- 提交后必核对：`workspace_modified_classes` 对账变更面，重读
  `method_bytecode`/`method_jasm` 确认落地形态；不符先回退再修。
- revision stale（ABORTED）说明有并行写入：重读 `workspace_info`、核对
  变更面，再决定重放或放弃；不带着旧 revision 盲目重试。
- 补丁可能被继承的方法前先查 `method_overrides`；反编译文本与本角色的
  字节码证据冲突时以字节码为准并上报。

## Deliverable

交付：`fact` 级证据（带指令索引与出处）；完成的编辑带前后证据与新 revision；
改不动的写明缺的条件。指令引用照抄工具返回的 index 与跳转目标，不自行
重编号。运行期行为与语义等价不在静态证据的保证范围内。
