---
name: mjt-mcp
description: >
  mjt-mcp 的三角色 agent skill 包：对 JAR 做事务式静态分析与受控编辑——
  侦察汇总、指令级证据与 Jasm 补丁、符号修复与 CFR/Vineflower 反编译迭代。
  凡是要分析 JAR、查方法字节码证据、改字节码或还原源码，都加载本包并按
  角色路由取用。
---

# mjt-mcp 角色包

本包把同一个 MCP 工具注册表（11 个粗粒度工具，schema 以运行中服务的
`tools/list` 为权威）分成三个角色视角。角色不是独立进程或权限系统；协调、
派发与授权裁决由使用方提示词承担，包内不含协调层。

先读[共享契约](workflows/common.md)——寻址、证据分级、授权与交接对所有
角色生效；再按当前问题加载一个角色的 skill 与 workflow，不要一次全部加载。

## 角色路由

| 角色 | 使用时机 | Skill | Workflow |
| --- | --- | --- | --- |
| 侦察汇总 | 任务第一站：概览、检索、候选信号 | [specialists/recon](specialists/recon/SKILL.md) | [workflows/recon.md](workflows/recon.md) |
| 指令分析师 | 指令级证据、Jasm 补丁与结构化编辑 | [specialists/bytecode-analyst](specialists/bytecode-analyst/SKILL.md) | [workflows/bytecode-analyst.md](workflows/bytecode-analyst.md) |
| 符号修复 | rename/mapping、反编译迭代 | [specialists/decompile-symbols](specialists/decompile-symbols/SKILL.md) | [workflows/decompile-symbols.md](workflows/decompile-symbols.md) |

主工作流（侦察 → 分析/符号修复 → 导出）由使用方提示词编排；
本包只约定每个角色内部的门与纪律。

## 边界

mjt-mcp 是静态分析与受验证的工作副本编辑工具：不加载、不执行目标 JAR
的业务代码；不支持 Android/DEX；所有变更走候选事务，分析与预览不构成
apply 授权。
