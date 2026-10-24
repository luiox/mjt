---
name: mjt-mcp-decompile-symbols
description: 在 mjt-mcp 上维护符号系统并迭代反编译：rename/mapping 修复符号、CFR/Vineflower 产出源码、按错误分类迭代到可交付。凡是要给无语义符号起名、导入 mapping、反编译类或方法、或处理反编译失败迭代，都用这个 skill。
---

# 符号修复

把混淆符号变成可持续维护的 mapping，把目标类变成可交付的重建源码。先读
[共享契约](../../workflows/common.md)与[符号修复工作流](../../workflows/decompile-symbols.md)。

## Preferred tools

`edit_apply`（kind=`rename`/`mapping`，dry_run 预览）、`decompile`
（同步或 `async: true`；action=`poll`/`cancel`/`release` 管理 job）、
`project_info`（backend 面与版本）。

## Decisions that matter

- 改名与 mapping 在反编译之前完成。顺序颠倒会把符号修复变成对反编译文本
  的文本级补丁，既不进 mapping 也不进字节码，下次重建全部丢失。
- 重命名的引用传播由服务端完成：`edit_apply(kind=rename, dry_run: true)`
  的影响面报告与提交后返回的 `affected_classes`/`affected_references` 是
  传播结果的权威记录，角色只决策"改什么、改成什么"，不手工同步引用。
- rename 前必审 `overrides[]` 与 `requires_override_review`：父类/接口/实现
  要一致命名，`cascade_overrides` 的取舍逐条确认。`policy` 缺省
  bytecode-only，要动 reflection/metadata 必须显式声明并记录。
- 反编译 backend 与超时/内存/源码上限按 `decompile` 调用参数注入，不落
  服务端会话态；backend 可用性与版本查 `project_info` 的 decompilers 面。
- async 反编译 job 按 `project_id` 归属隔离，期间不并行提交其他变更——
  提交撞 stale 拒绝。进入 committing 后不可取消。
- 产物永远是 `reconstruction` 级：backend 与版本随行；与字节码证据冲突时
  以字节码为准。一轮只改一层，符号修复与形态反制不混在同一次提交。

## Deliverable

交付：最终源码 + mapping/重命名记录（含传播面）+ 失败区域与残余风险清单，
按"反编译了哪些类、成功多少、失败分布"记账。反编译失败的排查顺序：
`project_info` 的 backend 可用性 → 超时/内存上限 → 类形态（未剥 CFF、
nest 元数据残留）→ 派指令分析师；原参数重跑不超过一次。
