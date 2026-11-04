# 分析型会话（只读面）

只读任务（摸结构、找证据、追引用、还原源码）全程不碰 edit_apply / edit_history / project_export。本文展开四个查询工具的分支细节与只读分析的边界。

## 打开与总览

- `project_open`：`path` 必填。只读分析给 `project_dir` 的唯一收益是分析缓存跨会话复用；不给则落临时工程，`project_close` 时 bundle 整体删除。大 jar 打开可能分钟级，给 `timeout_ms`；超时错误会携带预订的 `project_id`，孤儿完成后会话照常可用。
- `project_info`：会话健康检查第一站——容器角色与指纹、revision、台账深度与压缩健康、可用反编译后端、锁持有者。
- `container_mount`：陌生 JAR 常带依赖库与运行时形态。`role=library|runtime` 挂载后进入角色化容器栈（解析定序 primary > library > runtime），同名类条目 first-wins，被遮蔽副本在 mount/detach 响应里以 `shadowed`/`surfaced` 逐对上报。`role=support` 是遗留角色（不进角色栈），`primary` 由 project_open 绑定、挂载拒收。detach 按 `support_id`（project_info 里的稳定容器 id）。

## class_query：单点精确查询

`kind` 必填，分支选择器如下。

| kind | 返回 | 选择器 |
| --- | --- | --- |
| list | 全容器类结构摘要清单（分页） | 仅分页键 |
| class | 结构摘要 + 完整元数据 | `class` |
| bytes | 类原始字节（base64 分块） | `class` + `limit`（块上限 1 MiB） |
| jasm | 规范化 Jasm 全文 | `class` |
| resource_bytes | 任意资源字节（分块） | `uri` + `limit` |
| manifest | 解析后的 MANIFEST | 无 |
| method | 方法声明详情 | `owner`+`name`(+`descriptor`) |
| bytecode | 方法指令结构化列表 | `owner`+`name` |
| method_jasm | 方法 Jasm 声明段 | `owner`+`name` |

`cursor`/`offset`/`limit` 作用于 list（分页）与 bytes/resource_bytes（分块续读）；`page_size`/`fields` 仅 list。`instructions[]` 的节点类型是粗标签，操作数、跳转目标与行号以 jasm 文本为权威——指令级证据链先 `bytecode` 定位、再 `method_jasm` 对文本。

## search：统一检索

`kind` 必填，每个 kind 有独立键白名单，未知键按 kind 拒绝。

| kind | 用途 | 支持键 |
| --- | --- | --- |
| classes | 类名检索 | query/match_mode/case_sensitive/package_prefix/role |
| members | 成员检索 | classes 键 + owner/member_kind |
| strings | 字符串常量与常量池 UTF8 | members 键 + string_kind |
| numbers | 数值常量检索 | query/case_sensitive/owner/package_prefix/opcode/role |
| instructions | 指令检索（结构化过滤） | members 键 + opcode + mnemonic/operand/cst（三者为结构化过滤，与 query 互斥） |
| resources | 资源 URI 检索 | query/match_mode/case_sensitive/include_classes/role |
| shadow | shadow 显示名双向解析（无分页） | name/owner/direction/symbol_kind/file |

`match_mode`：contains（缺省）/ exact / prefix / regex；`query` 是文本或 ECMAScript 正则。`string_kind` 把常量池 UTF8 按内容形状分类：string_literal（ldc 常量与非类资源的可打印字符串）/ utf8_classref / utf8_descriptor / utf8_name。`role` 过滤把检索限定到指定角色容器。

shadow 面：sidecar 缺省是主容器旁的 `<jar>.mjt-shadow`。11 工具面只解析不生成——sidecar 由 pysdk（`mjt.shadow_generate_file` / `shadow_load`）或上游流程产出；sidecar 在位时，成员/类选择器与引用 handle 也接受 shadow 显示名。

## xref_query：引用图

| kind | 返回 | 选择器 |
| --- | --- | --- |
| references | 引用站点/调用边（分页） | `target`=method/field/class + `direction`/`resolved`/`role` |
| relations | 单类继承关系 | `class` |
| overrides | 单方法 override 组件 | `owner`+`name`+`descriptor` |

direction=to 列引用站点（target=method/field/class）；direction=from 仅 target=method，列调用边，`resolved=true` 解析到声明成员。descriptor 省略时按成员名回填；名字存在但描述符不匹配时，响应带 `diagnostics` 列出宿主类内全部真实描述符——先读诊断再下结论，不要拿空结果当「无引用」。`role` 过滤引用站点的容器归属（缺省全角色）。

## decompile：源码还原

- 同步：定位目标二选一——`class`（整类）或 `owner`+`name`+`descriptor`（该方法所在的类）；后端 `decompiler`=cfr/vineflower，`decompiler_jar`/`java` 缺省走本机发现；`timeout_ms`/`max_memory_mib`/`max_source_bytes` 是子进程防护上限。可用后端清单先看 project_info。
- 异步：`async=true` 返回 `job_id`；`action=poll` 取状态与结果、`cancel` 请求取消、`release` 回收（仅终态可 release，committing 不可 cancel）。缓存命中也走 poll 契约取结果。

## census 与批量只读面（pysdk）

MCP 11 工具面没有 census 工具。普查与批处理走 `mjt-mcp --script <script.py>`（构建需 `--with_pysdk=y`，签名在 `mjt.*` 绑定函数 `__doc__` 首行）：

- `mjt.class_census(path, out_dir)`：jar 全量类普查文件面导出——`class_census.tsv`（一行一实体、列序版本化、行序按内部名字节序）+ `container_census.tsv` 汇总 + `member_census.tsv`；数据来自分析库投影，输入 jar 只读，输出目录缺省 `<jar>.census`。
- `mjt.util` 只读普查助手：`string_census`（ldc 字符串直方图）、`call_sites`（按被调方坐标全罐普查调用点）、`decryptor_sites`/`residual_sites`（解密器形态站点启发式普查）、`guard_patterns`（恒假谓词分支）、`identity_rethrows`（恒等 rethrow 假 handler 组）、`native_table`（native 方法底表）、`class_hierarchy`（继承闭包 + 沿途声明成员集）、`verify_container`（容器完整性体检）、`jar_fix_analyze`（jar 修复只读扫描）。
- 脚本面没有事务/revision/savepoint 语义；deadline 与输出上限双层兜底（协作层 `mjt.DeadlineExceeded` + 硬层退出码 3），口径见 [../../../mjt-py/doc_ai/pysdk-script-limits.md](../../../mjt-py/doc_ai/pysdk-script-limits.md)。分析结论可回灌 MCP 会话继续做点查询。

## 只读会话纪律

- 空结果先排除寻址问题：descriptor 诊断、role 过滤、match_mode 大小写，再下「无命中」结论。
- 大 jar 上全量类元数据查询是分钟级常态：给 `timeout_ms`、用 `fields` 投影与分页收窄，不要一把梭全量。
- 反编译前确认后端可用（project_info）；子进程超时与会话超时是两层，别混用。
