# 侦察工作流：概览与信号

配合[侦察 skill](../specialists/recon/SKILL.md)与[共享契约](common.md)使用。

## 输入与范围

接受一个已打开的 `project_id`，或协调者刚拿到的 `project_open` 结果。
记录问题与结果预算（要看多少类、翻多少页）。能力摸底
（`project_info` 的容器、revision 与 decompilers 面）不需要任何类数据。
侦察产出只支撑派发，不支撑结论。

## 流程与检查

1. **形态**。`project_info` 确认 revision 与容器清单；`class_query`
   (kind=`list`) 翻页统计类总量与包分布，`search`(kind=`resources`) 区分
   主类资源与非类资源。检查：类名/包名的语义密度——大面积 `a.b.c` 或
   机械后缀记为第一信号。`class_query(kind=list)` 有载荷字节预算，大 jar
   必须翻页，首页不是全量。
2. **结构采样**。按包抽取代表类跑 `class_query`(kind=`class`)，看注解、
   签名、方法规模；体量大的 jar 同时看常量池形态。检查：样本覆盖了每个
   主要包，不是只看了入口类。
3. **字符串与数字**。`search`(kind=`strings`) 找 URL、反射调用、可疑长串；
   `search`(kind=`numbers`) 找高重复数值常量（解码 key 候选，可带
   `opcode` 收窄）。检查：每个查询的 `total` 与 `next_cursor` 都被处置
   ——翻完或在交接里写明"扫描了前 N 条"。
4. **指令形态**。对可疑类跑 `search`(kind=`instructions`)，关注
   tableswitch 密度、`ldc` 与算术/异或的组合、invokedynamic 出现位置。
   只计数与定位，不展开语义分析——那是指令分析师的工作。
5. **命名槽**。符号大面积无语义时向协调者建议用 SDK 生成 shadow sidecar
   （`mjt shadow -i <jar>`，MCP 面不写 sidecar）；sidecar 就位后用
   `search`(kind=`shadow`) 抽查双向解析正确性。检查：占位名跨类重号时
   必须逐条核对方向与归属，不得直接引用。
6. **交接**。按共享契约交回概览与候选信号清单，每条信号带定位与建议的
   下一步角色。交出的是"信号 + 定位"，不是下结论。

## 覆盖与恢复

返回条数等于有效页大小时可能被截断：提高 `page_size` 或收窄查询词是有效
动作，重复同参数查询不是翻页。空结果只支持"该查询无命中"。
`class_query`(kind=`bytes`/`resource_bytes`) 按字节分页，大资源要么读完
要么声明只读了前缀。

发现疑似加壳或 native 入口形态（资源里混非 class 载荷、入口类极小、方法
体大面积 native）时：直接上报请求升级，标注"疑似 L5+"，不自行深挖。

## 完成

概览足以支撑派发，或覆盖缺口已显式声明，即为 complete；主要包未采样或
关键检索未走完即为 partial。entropy 高、命名机械这类信号以 candidate
元数据交接，不翻译成"已加密"之类的断言。
