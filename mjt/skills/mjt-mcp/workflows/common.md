# 共享角色契约

这是角色间的交接契约，不是 MCP 请求 schema。工具参数永远以 `tools/list`
为准；本文约束任何角色都必须遵守的寻址、证据、授权与汇报规则。协调者委托
时把本文与所选角色的 skill 和 workflow 一起交给执行者：文件可读时给路径，
不可读时内联全文——执行者拿不到指令时应先索取，而不是发明一套替代工作流。

## 任务输入

委托一个角色时提供：用户目标、交付物、精确范围、工程身份、已有证据、
授权边界与停止条件。范围按对象选择：方法范围用类/方法/描述符三元组，
发现范围用查询词加结果预算。未知字段可以为 null，由接收角色先用只读工具
补齐。

```json
{
  "goal": "判断该类的控制流是否被扁平化并给出证据",
  "deliverable": "指令级证据与结论，不改工作副本",
  "project_id": null,
  "revision": null,
  "scope": {
    "class": "com/example/Gui",
    "method": { "owner": null, "name": null, "descriptor": null }
  },
  "authorization": { "mutate": false, "export": false },
  "evidence": [],
  "stopCondition": "给出结论或指明缺失的证据"
}
```

## 工程身份与寻址

- MCP 面共 11 个粗工具：`project_open`、`project_close`、`project_info`、
  `container_mount`、`class_query`、`search`、`xref_query`、`decompile`、
  `edit_apply`、`edit_history`、`project_export`。除 `project_open` 外全部
  以 `project_id` 寻址（`project_open` 返回它）。工具内部用 kind/mode/action
  选择器分派，参数形态以 `tools/list` 为权威。
- `project_id` 与 `revision` 成对登记。revision 只在已提交变更、undo、
  redo、restore 后递增；savepoint 不递增。提交任何变更必须携带读到的最新
  revision（`expected_revision`）——不携带等于接受任意过期写入，stale 拒绝
  说明有并行写入，先重读再决定，不盲目重试。
- 类选择器接受 JVM internal name 或精确 resource URI。
- `container_mount` 的 `role` 支持 `support`（遗留引用面）、`library`、
  `runtime`（进角色化挂载栈，解析序 primary > library > runtime；
  `primary` 由 project_open 绑定、挂载即拒）。挂载响应透传新增 `shadowed`
  副本记录，卸载响应透传 `surfaced`（解析序重建后浮现的被遮蔽副本）。
  `search` 与 `xref_query` 接受 `role` 过滤：library/runtime 按挂载栈寻址，
  命中 URI 形如 `<containerId>!/entry`。
- 方法寻址 `owner + name` 必填，`descriptor` 可省略：名字唯一时自动回填；重载歧义时工具报错并附全部候选描述符，必须显式选择目标重载——禁止拿同名第一个方法冒充，方法级结论若依赖具体重载就优先显式给 descriptor。
- shadow 命名槽活跃时（primary 容器旁存在 `<jar>.mjt-shadow` sidecar，
  由 SDK 的 `mjt shadow -i <jar>` 生成），`search(kind=shadow)` 做
  shadow 显示名与 raw 名的双向解析；占位名跨类可能重号，命中多条时逐条
  核对后取精确 raw 名。
- 指令索引是工作副本的原始位置，引用证据照抄工具返回的 index、label 与
  跳转目标，不自行重编号；变更提交后旧 index 属于旧来源。

## 证据与覆盖

| 级别 | 含义 | 限制条件 |
| --- | --- | --- |
| `fact` | 工具响应中的直接观察（指令、元数据、引用边、字节） | 结论不得超出观察面 |
| `candidate` | 启发式信号或位置假设（检索命中、分诊判断） | 附待确认项与确认路径 |
| `reconstruction` | 反编译文本与语法重建 | 附 backend/版本与告警；与字节码冲突以字节码为准 |
| `unknown` | 证据无法裁决 | 说明缺什么、下一步用什么工具补 |

覆盖三分开汇报：扫描了多少（scanned）、返回了多少（returned）、实际审阅
了多少（reviewed）。检索分页 `page_size` 默认 50、上限 1000（与 `limit`
互斥），截断页附 `total`/`next_cursor` 侧车；`fields` 可投影行键，未知键
直接报错。返回条数等于有效页大小时可能被截断，cursor 是
会话内游标而非跨 revision 书签。引用证据保留出处：工具名、参数、工程
身份、三元组、指令索引。"查询无命中"是扫描级事实，不是"不存在"的证明；
扫描完成不等于页面完整。

## 授权与效果

只读侦察与分诊不调用 `edit_apply`/`edit_history` 写动作与
`project_export`，即使技术上可行。变更一律先发生在服务端工作副本：
验证失败、revision 不匹配、取消或导出失败都不落盘——`edit_apply` 先
`dry_run: true` 拿诊断与 byte diff，再带 `expected_revision` 提交；
`project_export` 只接受此前不存在的输出路径且失败先于提交。
分析与预览不构成 apply 授权；授权一次只覆盖声明过的变更范围，换目标类
或换操作类型要重新确认。全程不加载、不执行目标 JAR 的业务代码。

## 结果交接

委托执行用结构化 JSON；直接回答可用散文但保留同等要素。

```json
{
  "role": "bytecode-analyst",
  "status": "complete",
  "project_id": null,
  "revision": null,
  "scope": { "class": null, "method": null },
  "tools_used": [],
  "claims": [
    { "level": "fact", "claim": null, "evidence": [] }
  ],
  "coverage": { "requested": null, "scanned": null, "returned": null, "reviewed": null, "truncated": null },
  "artifacts": [],
  "limitations": [],
  "next_action": { "role": null, "reason": null }
}
```

`complete` 表示交付物与检查完成，允许内含显式 `unknown`；`partial` 表示有
有用产出但覆盖或检查有缺口；`blocked` 表示当前输入、工具与授权下没有下一
步，必须写明缺的条件。跳过的步骤永远不算通过的检查。

## 恢复与停止

- 参数校验错误：按错误信息与 schema 修正后重试一次；不改参数的重放没有
  意义。
- 成员定位失败：descriptor 歧义错误自带候选列表，直接挑目标重载；名字不存在则回 `class_query(kind=class)` 拿权威成员表，禁止枚举猜测 descriptor。
- revision stale：重读 `project_info` 拿新 revision 并核对变更面，再决定
  重放或放弃。
- 反编译不可用：查 `project_info` 的 decompilers 面与环境配置；修不了就
  降级为 Jasm/字节码证据，把缺口写进 limitations。
- 有界尝试：同参数失败调用不重复；两条替代路径都失败就以 `blocked` 退出，
  把已确认的部分先交接回来。
