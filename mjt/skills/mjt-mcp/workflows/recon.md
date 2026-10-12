# 侦察工作流：概览与信号

配合[侦察 skill](../specialists/recon/SKILL.md)与[共享契约](common.md)使用。

## 输入与范围

接受一个已打开的 `workspace_id`，或协调者刚拿到的 `workspace_open` 结果。
记录问题与结果预算（要看多少类、翻多少页）。能力摸底（`pass_describe`、
`decompiler_list`）不需要任何类数据。侦察产出只支撑派发，不支撑结论。

## 流程与检查

1. **形态**。`workspace_info` 确认 revision 与 exportable；`class_list`
   翻页统计类总量与包分布，`workspace_resources` 区分主类资源与非类资源。
   检查：类名/包名的语义密度——大面积 `a.b.c` 或机械后缀记为第一信号。
   `class_list` 有载荷字节预算，大 jar 必须翻页，首页不是全量。
2. **结构采样**。按包抽取代表类跑 `class_get`，看注解、签名、方法规模；
   体量大的 jar 同时看常量池形态。检查：样本覆盖了每个主要包，不是只看了
   入口类。
3. **字符串与数字**。`search_strings` 找 URL、反射调用、可疑长串；
   `search_numbers` 找高重复数值常量（解码 key 候选，可带 `opcode` 收窄）。
   检查：每个查询的 `total` 与 `next_cursor` 都被处置——翻完或在交接里
   写明"扫描了前 N 条"。
4. **指令形态**。对可疑类跑 `search_instructions`，关注 tableswitch 密度、
   `ldc` 与算术/异或的组合、invokedynamic 出现位置。只计数与定位，
   不展开语义分析——那是指令分析师的工作。
5. **命名槽**。符号大面积无语义时向协调者建议 `shadow_generate`；sidecar
   写入后用 `raw_to_shadow` 抽查解析正确性。检查：占位名跨类重号时必须
   `shadow_to_raw` 确认，不得直接引用。
6. **交接**。按共享契约交回概览与候选信号清单，每条信号带定位与建议的
   下一步角色。交出的是"信号 + 定位"，不是下结论。

## 覆盖与恢复

返回条数等于有效 limit 时可能被截断：提高 limit 或收窄查询词是有效动作，
重复同参数查询不是翻页。空结果只支持"该查询无命中"。`class_bytes`/
`resource_read` 按字节分页，大资源要么读完要么声明只读了前缀。

发现疑似加壳或 native 入口形态（资源里混非 class 载荷、入口类极小、方法
体大面积 native）时：直接上报请求升级，标注"疑似 L5+"，不自行深挖。

## 完成

概览足以支撑派发，或覆盖缺口已显式声明，即为 complete；主要包未采样或
关键检索未走完即为 partial。entropy 高、命名机械这类信号以 candidate
元数据交接，不翻译成"已加密"之类的断言。
