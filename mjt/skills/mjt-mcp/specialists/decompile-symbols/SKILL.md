---
name: mjt-mcp-decompile-symbols
description: 在 mjt-mcp 上维护符号系统并迭代反编译：rename/mapping 修复符号、CFR/Vineflower 产出源码、按错误分类迭代到可交付。凡是要给无语义符号起名、导入 mapping、反编译类或方法、比较双 backend 输出、或处理反编译失败迭代，都用这个 skill。
---

# 符号修复

把混淆符号变成可持续维护的 mapping，把目标类变成可交付的重建源码。先读
[共享契约](../../workflows/common.md)与[符号修复工作流](../../workflows/decompile-symbols.md)。

## Preferred tools

`rename_analyze`, `rename_apply`, `mapping_analyze`, `mapping_apply`,
`class_source`/`method_source`（async）, `decompiler_compare`,
`decompiler_settings_set`, `job_get`, `job_release`。

## Decisions that matter

- 改名与 mapping 在反编译之前完成。顺序颠倒会把符号修复变成对反编译文本
  的文本级补丁，既不进 mapping 也不进字节码，下次重建全部丢失。
- 重命名的引用传播由服务端完成：`rename_analyze` 的影响面报告与
  `rename_apply` 返回的 `affected_classes`/`affected_references` 是传播结果
  的权威记录，角色只决策"改什么、改成什么"，不手工同步引用。
- rename 前必审 `overrides[]` 与 `requires_override_review`：父类/接口/实现
  要一致命名，`cascade_overrides` 的取舍逐条确认。`policy` 缺省
  bytecode-only，要动 reflection/metadata 必须显式声明并记录。
- async 反编译 job 期间不并行提交其他变更——组合事务对发起时 revision 做
  stale 校验，期间被改则 ABORTED。进入 committing 后不可取消。
- 产物永远是 `reconstruction` 级：backend 与版本随行；与字节码证据冲突时
  以字节码为准。一轮只改一层，符号修复与形态反制不混在同一次提交。

## Deliverable

交付：最终源码 + mapping/重命名记录（含传播面）+ 失败区域与残余风险清单，
按"反编译了哪些类、成功多少、失败分布"记账。反编译失败的排查顺序：
`decompiler_list` 可用性 → 超时/内存上限 → 类形态（未剥 CFF、nest 元数据
残留）→ 派指令分析师；原参数重跑不超过一次。
