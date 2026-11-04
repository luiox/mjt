# 改写型会话（编辑面）

编辑纪律一句话：候选事务 + revision 乐观锁 + 台账回滚。所有修改先落在候选副本，验证通过才原子提交并递增 revision；预览不是提交，失败的编辑不落半成品。

## 打开：持久工程是前提

- 要保留编辑状态必须给 `project_open` 传 `project_dir`：临时工程在 project_close 时连 bundle 一起删除，未导出的编辑不可恢复。重开同一 `project_dir` 恢复台账、书签与缓存。
- 编辑前 `project_info` 确认 revision 与容器指纹。bundle `store.lock` 单写者：第二个打开（含跨进程）直接失败，不存在并发写同一工程。

## edit_apply：唯一编辑入口

`kind` 必填，九种分支如下；`expected_revision`/`dry_run`/`label`/`validation` 四个通用键全分支可用。

| kind | 语义 | 关键载荷 |
| --- | --- | --- |
| structured | 结构化 ClassMutation | `class_uri` + `mutation_kind`（15 种）+ 对应字段 |
| jasm | 整类 Jasm 汇编替换 | `class_uri` + `source` |
| jasm_diff | 整类 unified diff | `class_uri` + `diff` |
| method_jasm | 方法级 Jasm 全量替换 | `owner`+`name`(+`descriptor`) + `source` |
| method_jasm_diff | 方法级 diff | `owner`+`name` + `diff` |
| class_bytes | 类字节整替 | `uri` + `content_base64` |
| pipeline | mjbase 优化管线 | `pipeline` 或 `passes`（互斥，均空回落 default-optimize）+ `jdk_path` |
| rename | 改名 | `rename_kind`=class/package/field/method/local + `new_name` + `cascade_overrides` + `policy` |
| mapping | mapping 文本应用 | `format` + `mapping_text` + `policy`（缺省 bytecode-only；可选 reflection/metadata/all） |

structured 的 `mutation_kind`：change_class_access（增量 set/clear 名单，未提及位保留、ACC_SUPER 存活、deprecated 落 Deprecated 属性）、set_field_access、set_method_access、add_field、remove_field、add_method、remove_method、add_marker_annotation、remove_annotation、rename_local、replace_try_catch_blocks、replace_method_body、insert_instruction、replace_instruction、remove_instruction。

rename 寻址：class 用 `owner`（旧类名）；package 用 `owner`（包前缀）；field/method 用 `owner`+`name`(+`descriptor`)；local 用 `owner`+`method_name`+`local_index`。`policy`（缺省 all）控制改写面：bytecode / reflection（反射字符串）/ metadata / all。`cascade_overrides` 控制覆盖组件联动改名。大面积改名先 `dry_run=true`：analyze 结果带 `mappings`/`overrides`/`impact`/`ready_to_apply`/`requires_override_review`，确认后再提交。

Jasm/diff 流程：`class_query(kind=jasm|method_jasm)` 取当前规范全文 → 修改 → `edit_apply(kind=jasm_diff|method_jasm_diff)`。diff 上下文行必须与当前全文精确一致，无 fuzz、无 3-way 合并，失配即拒。修改指令级细节时记 `class_query` 契约：结构化 `instructions[]` 是粗标签，行号与跳转目标以 jasm 文本为权威。

## validation 三档与预览

- `validation`：relaxed（缺省，JVM 赋值协变语义 + 只校验受影响类）/ strict（精确类型匹配）/ off（跳过门禁，逃生通道，慎用）。Jasm 重汇编与回滚动作都吃这一档。
- `dry_run=true` 预览不提交：返回与提交同形状的报告（`byte_diffs`/`affected_classes`/`affected_references`/`diagnostics`），工作副本与 revision 不动。预览满意后显式再发一次不带 `dry_run` 的调用才算编辑。
- `label` 进台账事务行，是事后在 `edit_history(action=list)` 里定位这次变更的检索键；一个逻辑变更一个 label。

## revision 门（乐观并发）

revision 只在已提交变更、undo、redo、restore 后递增；savepoint 不动 revision。推荐节奏：project_info 读 revision → dry_run 预览 → 提交带 `expected_revision` → 被拒（过期写）则重读状态、基于新 revision 重做。连续编辑链把每次提交响应里的新 revision 传给下一次 `expected_revision`。

## edit_history：台账回滚

| action | 语义 |
| --- | --- |
| savepoint | 具名书签：零字节代价、不动 revision、跨会话存活 |
| restore | 恢复到书签（产生新 revision） |
| revert_class | 单类回退（`class_uri`） |
| undo / redo | 单步回退/重放 |
| compact | 手动压缩台账：`keep_recent_bookmarks`/`keep_undo_depth` 显式给定时生效，缺省书签全保留 |
| list | 台账列览（txn/anchor/bookmark 三类行，分页；先 flush 保证与内存一致） |
| modified_classes | 已改类清单：前后 internal name、指纹与尺寸 |

`validation` 仅对 undo/redo/restore/revert_class 生效，其他动作带它会被显式拒绝。回滚也是走验证门的变更：失败同样不落半成品。

## project_export：物化派生 JAR

- `path` 必填；原子写，永不触碰主容器；导出前 flush 台账与分析缓存。
- 导出物是截至当前 revision 的全部已提交编辑，不含未提交的预览。
- 临时工程（无 `project_dir`）在 project_close 即删：先导出再关，顺序不能反。
