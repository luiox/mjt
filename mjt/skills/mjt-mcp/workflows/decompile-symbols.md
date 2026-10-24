# 符号修复工作流：改名、mapping 与反编译迭代

配合[符号修复 skill](../specialists/decompile-symbols/SKILL.md)与
[共享契约](common.md)使用。方法论骨架见仓库文档
`doc/recover-source-from-jar-guide.md`（四阶段还原）。

## 铁律

改名与 mapping 在反编译之前完成。顺序颠倒会把符号修复变成对反编译文本的
文本级补丁——不进 mapping、不进字节码，下次重建全部丢失。反编译产物永远
是 `reconstruction` 级，backend 与版本随行，与字节码证据冲突时以字节码
为准。

## 输入与范围

接受目标类/符号清单、命名参考（哪里来的语义线索）与授权边界（是否允许
写操作、是否允许导出）。一轮只改一层：符号修复与形态反制（优化管线等）
不混在同一次提交，否则归因困难。

## 门 1：配置

`project_info` 的 decompilers 面确认 backend 可用；不可用时先修环境
（`decompile` 的 `decompiler_jar`/`java`/`timeout_ms`/`max_memory_mib`
参数按调用注入），修不了就降级为 Jasm 阅读并在交接里声明。检查：
backend 选择与版本记进任务，后续所有产物可归因。

## 门 2：修符号

逐符号：`edit_apply`(kind=`rename`, `dry_run: true`) 拿 `mappings[]`、
`overrides[]`、`impact`（前后引用数、byte diff、风险）→ 审 override
组件 → 去掉 dry_run 提交。

- `requires_override_review` 为真时逐条确认：父类/接口/实现一致命名，
  `cascade_overrides` 的取舍有理由。
- `policy` 缺省 bytecode-only；要动 reflection/metadata 显式声明并记录。
- 传播由服务端完成；返回的 `affected_classes`/`affected_references` 是
  传播结果的权威记录，归档进交接——不手工同步引用，也不复核对服务端
  传播面的信任。
- 已有 mapping 源时走 `edit_apply`(kind=`mapping`)，与逐符号 rename
  二选一为主，避免同符号两路冲突。

检查：rename 未过 override 审查不提交；每次提交带 `expected_revision`，
成功后刷新 revision。

## 门 3：反编译

`decompile`（class 选择或三元组）反编译目标类；大类用 `async: true` 拿
`job_id`，`decompile`(action=`poll`) 轮询到终态（succeeded/failed/
cancelled），完成后 `decompile`(action=`release`) 释放。

job 规则：取消走 `decompile`(action=`cancel`)，只对 queued/running 生效，
进入 committing 不可取消；超时与失败不产生任何写回（无导出、revision
不变）；job 按 `project_id` 归属隔离，跨工程引用 job_id 按不存在处理——
**job 运行期间不并行提交其他变更**，提交会撞 stale 拒绝。

检查：产物带 backend/版本与告警；读取失败区分"超时"
（DEADLINE_EXCEEDED 侧车）与"反编译错误"。

## 门 4：迭代

读产物 → 分类错误：语法伪影（record 折叠、语法糖）、未还原符号、残留
混淆形态。处理：符号问题回门 2；指令级确认派指令分析师；文本后处理规则
（语法糖还原、record 伪影幂等修复）按 `doc/recover-source-from-jar-guide.md`
对应章节执行 → 再反编译验证。

失败排查顺序：`project_info` 的 backend 可用性 → 超时/内存上限 → 类形态
（未剥 CFF、nest 元数据残留）→ 派指令分析师。原参数重跑不超过一次。

## 交付

最终源码 + mapping/重命名记录（含传播面）+ 失败区域与残余风险清单。
按"反编译了哪些类、成功多少、失败分布"记账。需要落盘时导出走协调者批准
的 `project_export`（只写此前不存在的新路径）；交付文本永远标
`reconstruction` 级。

## 完成

目标类全部得到源码且错误分类处理完毕即为 complete；部分类反编译失败但
失败原因与后续路径已写明即为 partial。可读性改善不是正确性证明——
最终验收以字节码证据与（如有）可构建性检查为准。
