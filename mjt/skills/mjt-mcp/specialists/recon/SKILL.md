---
name: mjt-mcp-recon
description: 在 mjt-mcp 上对 JAR 做只读侦察：建立类目与资源概览、扫描字符串/数字/指令形态、收集候选特征交给下一步分析。凡是要先摸底一个陌生的 JAR、清点类与资源、圈定值得细看的方法或类，或任务开头的广度优先侦察，都用这个 skill。
---

# 侦察汇总

把一个打开的工作区变成结构化概览和一份候选信号清单。先读
[共享契约](../../workflows/common.md)与[侦察工作流](../../workflows/recon.md)。

## Preferred tools

`workspace_info`, `workspace_resources`, `class_list`, `search_classes`,
`search_strings`, `search_numbers`, `search_instructions`, `class_get`；
`shadow_generate` 仅在授权后使用。

## Decisions that matter

- 所有检索都分页：limit 默认 100、上限 1000，截断页带 `total` 与
  `next_cursor`。返回条数等于有效 limit 时可能被截断——要么翻页到底，
  要么声明覆盖比例；重复同参数查询不是翻页。
- cursor 只是本次会话的翻页游标，不是跨 revision 的书签；工作区发生变更后
  重新查询。
- "查询无命中"只支持"该查询无命中"，不支持"行为不存在"的结论；单个损坏
  class 不会中断其余类的搜索。
- `shadow_generate`/`shadow_prune` 只写 sidecar 文件、永不改 jar；机械占位名
  跨类可能重号，引用前用 `shadow_to_raw` 解析确认。
- 信号只回答"值得看"，不回答"是什么"。家族判断与置信归分诊角色。

## Deliverable

交付：全局概览（规模/包分布/资源/命名语义密度）+ 候选信号清单，每条信号带
定位（类、三元组、指令索引或检索式）与建议的下一步角色；覆盖按扫描了/
返回了/审阅了三分口径汇报。停止条件：概览足以支撑派发，或覆盖缺口已声明。
发现疑似加壳或 native 入口形态直接上报请求升级，不要自行深挖。
