# mjt-py SDK 现状盘点与 MCP 工具覆盖矩阵

> 审计快照：2026-10-23，基于 mjt commit 612ea79（origin/main 最新）。
> 只读源码审计，为「SDK 优先、MCP 面收敛」RFC 提供输入；未运行构建。

---

## ① 总览结论

**SDK 成熟度一句话**：mjt-py 是一个"字节码编辑原语面已相当完备、但产品化程度很低"的 SDK——指令级/成员级/改名/shadow/批量改名/单 pass 的编辑与查询原语齐全且测试充分（33 个 C++ facade 直测 + 16 阶段 Python 全链自检），但它没有宿主接线（`mjt --script` 在本仓不存在）、没有事务/版本/undo 语义、没有反编译与 Jasm 文本面、没有资源上限与取消、CI 完全不构建它；以当前形态直接承载"MCP 面收敛为少量粗粒度入口"还差一层运行时与五族引擎能力绑定。

**工具数勘误**：任务描述按单行 grep 估为 61 个 MCP 工具；实际 `contract.cpp` 注册 **74 个**（13 个定义跨行，单行 grep 漏计，含 `decompiler_settings_set`、`analysis_workflow`、`class_bytes`、`method_get`、`search_strings`、`workspace_open` 及 4 个 method_*_jasm_diff/validate 族等）。本矩阵按 74 个全覆盖。

---

## ② 覆盖矩阵（74 个 MCP 工具 ↔ mjt-py SDK）

分档口径：

- **已覆盖**：SDK 有直接等位物（同一引擎面，参数形态可不同）。
- **部分**：SDK 有相关面或可用既有原语拼装，但有实质缺口（表中说明差什么）。
- **缺失**：SDK 无等位物，且拼装需要新增引擎绑定（反编译/Jasm/事务/注解/LVT/进程编排等），纯脚本拼装不可行或成本失真。

### 2.1 已覆盖（11 个）

| MCP 工具 | SDK 等位物 | 备注 |
|---|---|---|
| workspace_open | `mjt.open_jar` | 句柄 vs workspace_id；SDK 无 kMaximumRetainedWorkspaces=16 上限 |
| workspace_close | `mjt.close_container` | |
| workspace_resources | `mjt.list_entries` | 全量含非类条目；MCP 有分页、SDK 全量返回 |
| resource_read | `mjt.get_entry_bytes` | MCP 分块 base64；SDK 一次全量 |
| class_list | `mjt.class_names` + `class_info` | |
| class_bytes | `mjt.class_bytes` | MCP 有 limit 分块与 fingerprint；SDK 全量、无指纹 |
| method_get | `mjt.method_info` | |
| shadow_generate | `mjt.shadow_generate` | 同一 mjbase::renamer 机械占位名（幂等）；MCP 另写 sidecar 文件、SDK 落会话表 |
| xref_method | `mjt.util.call_sites(owner, name, desc)` | 全罐方法调用点精确过滤，语义同；无分页 |
| method_callers | `mjt.util.call_sites` | 同上 |
| workspace_export | `mjt.save` / `mjt.export_jar` | 两阶段落盘纪律同款；未触碰类字节原样导出 |

### 2.2 部分（30 个）

| MCP 工具 | SDK 现有面 | 差什么 |
|---|---|---|
| pass_describe | `mjt.passes()` | 只列 pass 名；无 description/scope、无 pipeline 清单 |
| analysis_workflow | `run_pass` + token 改名 + `verify_container` + `export_jar` 可手工拼段 | 无原子事务/候选副本/candidate-jar 提交语义；decompile 段无 |
| resource_manifest | `get_entry_bytes`（MANIFEST 现值字节） | 无主属性解析面（key-value） |
| resource_strings | `get_entry_bytes` 非类资源字节 | 可打印字符串提取未引擎化（脚本可拼，trivial） |
| search_classes | `mjt.util.classes_where`（glob name/not_name/where） | 无 contains/regex/match_mode、无成员外资源 URI 搜索 |
| search_members | `mjt.util.methods_where`（glob/static/abstract/native/has_handlers） | 无 owner/descriptor/signature 维度、无 regex、无字段独立过滤 |
| search_strings | `mjt.util.string_census`（ldc 字符串直方图） | 无 query 匹配引擎面；非类资源字符串不在面内 |
| search_numbers | 指令原语可遍历 | 无引擎级数值字面量普查（ldc/iconst/bipush 手拼） |
| search_instructions | `insns` + `insn_detail` 全遍历 | 无 mnemonic/opcode 引擎级搜索；全罐扫描靠脚本循环 |
| class_get | `class_info` + `methods` + `fields` | **注解读写完全缺失**（annotations 不在任何 SDK API 里） |
| class_fidelity_check | `write_back(compute_frames=False)` 往返 + `verify_container` | 无"字节恒等断言"封装（脚本可比 bytes，无结构摘要对拍） |
| method_bytecode | `insns` + `insn_detail` | 指令摘要有；normalized Jasm 源文本无 |
| shadow_to_raw | `mjt.shadow_entries` 全表 | 无点查 API（脚本自建反查表） |
| raw_to_shadow | 同上 | 同上 |
| pass_run | `mjt.run_pass(h, name, jdk)` | 只收单 pass；无 passes 数组组合（分析管理器不共享）、无 dry_run/expected_revision/label 事务参数 |
| opt_preview | `mjt.util.dry_run`（类级分叉 + 结构闸 + COMPUTE_FRAMES + cleanliness diff） | 工作区级候选副本与 byte-diff 无；只支持单类粒度 |
| opt_apply | `run_pass` 直改容器 | 无原子提交/revision/回滚 |
| jar_fix_analyze | `verify_container`（mismatch/malformed/dep_missing 清单） | 无"修复移除预览"（remove_invalid_class_entries 语义） |
| jar_fix_apply | `remove_entry`（按 verify 结果脚本裁决） | 同上 |
| xref_field | 遍历指令 kind=="field" 可拼 | 无引擎引用扫描绑定 |
| xref_class | 遍历 type/field/method/ldc 指令可拼 | 同上；无 metadata 级引用 |
| method_callees | 遍历方法体 method 指令可拼；resolved 版可借 `class_hierarchy` | 无 resolved 调用边引擎面 |
| class_relations | `mjt.class_hierarchy`（super 链 + 接口闭包 + 沿途成员集） | **无反向子类/实现者查询**（谁继承我）；容器外父类不入闭包 |
| method_overrides | `class_hierarchy` 沿途成员集手工比对 | 无 override 组件查询 |
| patch_replace_class_bytes | `mjt.set_class_bytes` | 无 validate+原子提交语义（坏字节进容器后延迟到 parse 才炸） |
| patch_apply | emit_*/insn_set_*/add_/remove_/try_catch 原语可覆盖多数 kind | **add/remove_annotation、rename_local（LVT）无法达**；无 dry_run/revision |
| rename_analyze | `rename_apply_suggestions`（引擎全量裁决 + 被拒流水，应用型） | 无 plan-only 分析模式绑定（RenamerEngine planOnly 未出） |
| rename_apply | `rename_begin`/`token_rename_*`/`rename_commit` | **无 local 变量改名**（RenameKind::local）；无 cascade_overrides 显式开关 |
| mapping_analyze | `mapping_dump`/`mapping_counts`（仅导出） | mapping 文本导入解析（Enigma/Tiny/ProGuard/SRG/Simple/R8 解析器在 mjbase 已有）未绑定 |
| mapping_apply | token 面可逐条收计划 | 同上：7 种格式文本→plan 的引擎导入缺，脚本自写解析成本高 |

### 2.3 缺失（33 个）

| 工具族 | 工具 | 缺口说明 |
|---|---|---|
| 反编译（6） | class_source、method_source、decompiler_decompile、decompiler_compare、decompiler_list、decompiler_settings_get/set（共 7 个函数） | SDK 零反编译概念。MCP 侧是外部 JVM 子进程编排（CFR/Vineflower jar、timeout_ms/max_memory_mib/max_source_bytes 上限、后台 job 取消），SDK 无任何进程编排面 |
| 事务/版本（7） | workspace_info、workspace_savepoint、workspace_restore、workspace_revert_class、workspace_undo、workspace_redo、workspace_modified_classes | SDK 无 revision 计数、无原始快照、无 undo/redo/savepoint；树脏纪律是内部实现不外显 |
| Jasm（8） | class_jasm、method_jasm、patch_validate_jasm、patch_apply_jasm、patch_validate_jasm_diff、patch_apply_jasm_diff、method_validate_jasm、method_apply_jasm、method_validate_jasm_diff、method_apply_jasm_diff | libjhasm 汇编/反汇编完全未绑定。SDK 只有 insn_detail dict 形态，无 Jasm 文本 |
| 后台 job（3） | job_get、job_cancel、job_release | SDK 无后台执行/取消/释放机制 |
| supporting JAR（3） | workspace_add_support、workspace_list_support、workspace_remove_support | SDK 无依赖罐附加（run_pass 的 jdk 参数只在管线内建层次图，不进会话） |
| 其他（6） | analysis_evaluate（mjbase::expr 表达式面未绑定）、jar_diff（引擎 diff 面 MCP 侧独有）、shadow_prune（SDK generate 幂等但无过期清理） | |

---

## ③ SDK 独有能力（MCP 面没有的）

**mjt.util（mjt_util.py，纯 Python 二次封装层，随 pyd 部署）**：

- `classes_where` / `methods_where`：glob/谓词惰性过滤（name/not_name/name_in/static/abstract/native/has_handlers/where callable）。
- `HandlerRef` + `identity_rethrows`：恒等 rethrow 假 handler 判据（混淆器三形态保守子集：裸 athrow / invokestatic 恒等包装 / store 桥；非 catch-all、无 jump 入体、无 fallthrough、签名门+方法体验证、跨组重叠门）与键寻址删除；`allow_overlap` 链式成组模式。
- `GuardRef` + `guard_patterns`：恒假谓词（iconst_0+ifne / iconst_1+ifeq）普查与保守改写（载荷→pop、分支删除）。
- 解密器路径：`decryptor_sites`（启发式站点普查）/ `residual_sites`（残留台账）/ `call_sites`（定点普查，str 键解密器正式出路）/ `fold_to_ldc`（解密点回填明文 ldc，实参同摘、fail-closed、原子拒绝）/ `fold_call_sites` / `fold_decryptor_sites`。
- `clone_method`：同类方法克隆（逐指令 emit 重建 + try 行 label 映射；switch/indy/multianewarray/long/float/double ldc 如实拒绝）。
- `string_census`（全罐字符串直方图）、`cleanliness`（清洁度指标差分）。
- `dry_run`（容器分叉 + 结构闸 + COMPUTE_FRAMES 写出验证 + cleanliness diff + `DryRunDraft.apply()`）、`save_verified`（落盘后重开逐类解析复核）。
- `is_obfuscated_name` / `semantic_name`（改名助手，策略可换 LLM/词典）。

**C++ 绑定层独有**：

- `verify_container`：entry 探针 + 解析闸 + 容器外非 JDK 依赖清点（MCP 面无完全等位物；jar_fix_analyze 相近但语义不同）。
- `duplicate_names` / first-wins 索引与落选者晋升（#962）。
- `sync_entries`：类改名条目落账迁移清单（write_back/export 边界自动调，也可显式调）。
- 逐指令编辑原语族：`emit_insn/var/iinc/type/field/method_call/ldc/jump/switch/label` + `insn_set_*` + `remove_insn`（墓地 fail-closed）——比 MCP patch_apply 的结构化指令树粒度更细、增量式。
- `shadow_load`/`shadow_dump`/`shadow_entries`：会话内 shadow 表整体读写（MCP 面只写 sidecar 文件）。
- `run_pass` + `RunStats`/`last_run_report`：单 pass 运行 + 逐槽位统计（processed/changed/failed/durationMs）——MCP pass_run 不返回等价统计。
- `class_hierarchy` 的"沿途声明成员集"形态（super 链 + 接口闭包 + methods/fields 四元组列表）。
- `write_demo_jar` 合成样本、`opcodes`/`acc` 常量面、`MethodPass`/`changed()`/`preserved_all()` 类型锚（Python pass 形状，适配器实现属私有侧管线面）。

---

## ④ 架构事实

### 4.1 脚本如何获得"工作区/会话"状态

- **同进程、进程内静态注册表**。`mjt.pyd` 是 pybind11 扩展模块；一切 C++ 侧状态归 `FacadeSession`（ResourceContainer + name↔URI 索引 + 树缓存/脏纪律 + uid 映射 + rename token + shadow 表），由进程内单例 `FacadeRegistry`（`mjt-py/src/pysdk_facade.hpp:124-140`）按 u64 句柄管理。
- **pysdk_jar/pysdk_facade 直接进程内调用引擎**，不起子进程：`facadeImportJar` 调 `mjbase::model::ResourceHelper::importFromJar`（`pysdk_facade.cpp:161-169`）；改名提交调 `mjbase::asm_remapper::applyResourceRemapPlan`（`pysdk_facade.cpp:1718`）。
- **嵌入解释器路径**：`pysdk_embed.cpp:127-207` `runEmbeddedScript` = 预载 python314.dll（Windows）→ `Py_InitializeFromConfig`（utf8_mode=1 + home）→ exe 目录/MJT_PYSDK_PATH 插 sys.path → `import mjt` 预注入脚本 globals → `PyRun_File`。异常打 traceback，退出码 0/1/2。
- **资源上限与取消：脚本路径上完全缺失**。mjt-py 全部源码无 timeout/deadline/cancel/内存上限/输出上限任何机制（grep 证据：全目录零命中）。对照组，MCP 侧有 kMaximumRetainedWorkspaces=16 / kMaximumRetainedJobs=64（`mjt/mcp/src/tool/limits.hpp:22,27`）、反编译 timeout/内存/源码体积上限（`contract.cpp:290-300`）与 job 取消（`session.cpp:181-217`）。脚本若死循环或吃爆内存，宿主进程只能整体陪葬。

### 4.2 MCP 工具与 SDK 是否同一套底层实现

- **共享底座 = mjbase::model::ResourceContainer 数据模型 + mjbase 各引擎组件**。两侧都消费：ResourceContainer（数据）、mjbase::spi pass 注册表与管线（SDK `run_pass` 用 `defaultPassRegistry` + `runContainerPipeline`，`bind_core.cpp:137-226`；MCP 经 `WorkspaceBase::runPipeline`）、asm_remapper（`applyResourceRemapPlan` 两侧同用）、mjbase::renamer（ShadowAliasTable/RenamerEngine 两侧同用：`pysdk_facade.hpp:114` / `mutation.cpp:519`）、mapping 格式注册表。
- **但事务编排层是两套代码**：
  - MCP：74 个工具全部经 `IWorkspace`（65 个虚方法，`mjbase/src/mjbase/workspace/i_workspace.hpp`）→ `WorkspaceBase`（`workspace.hpp:39-358`，内持 original_/working_ 双容器 + undo_/redo_/savepoints_ 快照栈 + revision + MutationRequest 原子提交/dry-run）。mjt 侧 `JarWorkspace` 是空壳继承（`mjt/src/shared/jar_workspace.hpp:12-16`）。
  - SDK：`FacadeSession` 直接持单个 ResourceContainer，自管树缓存/脏纪律/uid 映射/rename token，**没有** revision/undo/redo/savepoint/dry-run 事务机制；MCP 侧也没有 SDK 的树缓存与 rename token。
- **若 MCP 收敛，SDK 侧缺的共享层入口**（IWorkspace 有而 facade 无的族）：事务族（MutationRequest expected_revision/dry_run/label、savepoint/undo/redo/restore/revert、modifiedClasses）、supporting JAR 族、search 五族引擎级搜索、xref/callees/classRelations/overridesOfMethod 分析族（analysisGraph）、Jasm 族（libjhasm）、反编译编排族、jar diff/repair 族、mapping 文本导入族、注解族（add/remove_marker_annotation）、LVT/local 改名、analysis_evaluate（mjbase::expr）。

### 4.3 mjt.pyd 的构建/发布形态

- **门控**：根 `xmake.lua` 末尾 `if has_config("with_pysdk") then includes("mjt-py")`；选项在 `xmake/options.lua:19-22`，**默认 false**。注意：任务侧说的"MJT_WITH_PYSDK=1" 实际是 `pysdk_embed.hpp:6` 注释里的旧名，本仓真实门控是 xmake 选项 `--with_pysdk=y`，全仓无 MJT_WITH_PYSDK 编译定义。
- **三个 target**（`mjt-py/xmake.lua`）：`mjt_py_embed`（静态库，纯 CPython API，set_default(false)）；`mjt_pysdk`（`set_basename("mjt")` + `set_extension(".pyd")` 的 shared，链 python314 + pybind11 include，mjbase 全静态闭包）；`mjt_pysdk_unittest`（C++ facade gtest + 嵌入跑 pysdk_selftest.py，自定义 main 先 init Python）。
- **python314.dll 依赖**：Windows 上 `pythonPreInit` 用 `LoadLibraryExA(LOAD_WITH_ALTERED_SEARCH_PATH)` 从 home 预载（`pysdk_embed.cpp:76-86`）；链接期 `add_links("python314")` 指向 `<pyHome>/libs`。
- **MJT_PYTHON_HOME 解析顺序**：`--python_home=` xmake 配置 > `MJT_PYTHON_HOME` env > 硬编码缺省 `D:/sdk/python/Python314`（`mjt-py/xmake.lua:13-15`）；该值同时以 `MJT_PYTHON_HOME_DEFAULT` 编译期宏注入 embed 库（:28,95）。pybind11 经 `MORPHER_PYBIND11_ROOT` env，缺省 `D:/sdk/cpp/pybind11-3.1.0`（:17）。`pythonHome()` 运行期再按 env 覆盖编译期缺省（`pysdk_embed.cpp:54-61`）。
- **CI 不构建它**：`.github/workflows/ci.yml` 两个 job（linux/windows）只配 `--with_tests=y --mjbase_root=mjbase-src`，只建 `mjt-mcp` + `mjt_mcp_unittest`；无 `--with_pysdk`、无 Python 3.14/pybind11 准备步骤。**mjt.pyd 无 CI 覆盖，纯本机构建物，也不进任何发布产物目录。**
- **宿主未接线（关键事实）**：`mjt --script foo.py` 的宿主形态在本仓不存在——`mjt-mcp` 的 main（`mjt/mcp/src/main.cpp:26-79`）只认 --stdio/--http/--host/--port/--token/--origin；`runEmbeddedScript` 的唯一消费方是 `mjt_pysdk_unittest`。`mjt_py_embed` 静态库 set_default(false) 且无任何 target 依赖它。SDK 当前只能通过"外部 CPython import mjt.pyd"或测试进程触达。

### 4.4 脚本 DX 现状

- **错误呈报 = 异常，不是返回码**。C++ `Result` 值语义在 facade 层转抛异常；pybind11 转 Python 异常：`StaleRefError` 经 `py::register_exception` 注册为**KeyError 子类**（`bind_facade.cpp:74-76`，含 translator 补刀说明——只建类型不装 translator 会让旧键 fail-closed 全变 SystemError）；其余 `std::runtime_error`→RuntimeError、`py::value_error`→ValueError。脚本可 `except KeyError/StaleRefError` 兜底。进程退出码仅 embed 层 0/1/2。
- **help/文档**：每个 `m.def` 都带 docstring（`help(mjt)` 可读），`m.doc()` 有模块一句话说明；无独立文档生成流程。代码注释引用的契约文件 `mjt-py/doc_ai/mjt-py-sdk-spec.md` **不在仓内**（mjt-py/ 下只有 src/ 与 xmake.lua），契约已失联。
- **mjt_util.py 的定位 = 正式 SDK 层的一部分，而非示例库**：模块 init 时被 exec 成 `mjt.util` 子模块（`bind_core.cpp:232-271`，pyd 旁自动部署、MJT_UTIL_DIR 回退），头注明确"定案（spec v3 §4）：清单沿 v2，改基 v3 平函数面"。但两点削弱其正式性：判据是"移植语义，非引擎强制门"（安全类工具 dry_run/save_verified 是"可选调用"）；`clone_method`/`identity_rethrows` 等在纯 Python 逐指令循环，无引擎加速。它同时承担 util 库与回归测试载荷（pysdk_selftest.py 16 阶段大量复用其函数）。

---

## ⑤ SDK 补缺清单（收敛 MCP 面前必须补齐，按优先级排序）

1. **事务/版本面（最高优先）**：把 IWorkspace 的 revision、expected_revision、dry-run 候选、undo/redo/savepoint/restore/revert、modifiedClasses 移植为 FacadeSession 之上的薄层（或干脆让 FacadeSession 换持 WorkspaceBase）。没有它，收敛后 AI 每一次编辑都是不可回滚的单发——patch/rename/mapping 全族都依赖这个安全网。这是"MCP 收敛"的硬前置。
2. **Jasm 汇编面**：绑定 libjhasm（class_jasm/method_jasm/get/apply + diff 变体等位物，共 8~10 个 MCP 工具的来源）。文本级汇编是 LLM 最顺手的编辑形态；指令原语面（emit_*/insn_set_*）对 AI 太低层，80 行 Python 才能等价一个 method_apply_jasm。
3. **反编译面**：class_source/method_source 等位物。需要把 MCP 侧 CFR/Vineflower 子进程编排 + timeout/内存/输出上限 +（可选）后台 job 下沉为共享层；或明确定"SDK 不带反编译、脚本自调 java"的边界——但必须先定，不能悬着。
4. **分析族绑定**：xref_field/xref_class/method_callees（resolved）/class_relations 反向（子类/实现者）/method_overrides。引擎已在 IWorkspace/analysisGraph 里，缺的只是 facade 绑定；配合 util.call_sites 可覆盖全部 xref 工具。
5. **运行形态**：宿主 `--script` 接线（把 mjt_py_embed 接进 mjt-mcp main 或独立 mjt CLI）+ **脚本路径资源上限（超时/内存/输出上限）与取消透传**（现状为零，见 4.1）+ CI 构建 mjt.pyd（Python 3.14/pybind11 环境准备 + pysdk unittest 进 CI 门）。当前连 `mjt --script` 都跑不起来，"SDK 优先"没有运行载体。
6. **注解 + LVT/local 面**：annotations 读写（patch_apply 的 annotation kind、class_get 的 annotations）、rename_local/LVT 表读写。小而硬的引擎缺口。
7. **search 与 mapping 导入**：引擎级 search_strings/numbers/instructions 绑定（现靠脚本全罐循环）；mapping 文本导入（mjbase mapping_format_registry 的解析器已存在，缺 facade 入口）。
8. **supporting JAR**：依赖罐附加面，否则 class_hierarchy 闭包止于单罐，xref/relations 分析在多罐工程上是错的。

---

### 附：审计方法注记

- 工具清单提取：`contract.cpp` 中 `tool(`/`workspaceTool(` 注册调用，多行定义已并拾，共 74 个（2.1+2.2+2.3 = 11+30+33）。
- 行号引用基于 commit 612ea79 工作区；mjbase 侧引用位于兄弟仓 `D:\WorkSpace\mjbase`（mjt 经 `--mjbase_root` 源码级依赖，CI 钉 ref f5cf955）。
- 未运行 xmake 构建；全部结论来自源码静态阅读。
