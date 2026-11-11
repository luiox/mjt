# workspace 移除与重构 · 交接文档

- 状态：**交接中**（2026-11-10）；W2–W5 已全部落地（见 §4 完成度矩阵 = 终态）
- 阅读口径：§0 / §2.3 / §5 / §7 是交接时点的快照（当时 W2–W4 尚未动代码），
  与 §4 完成度矩阵不一致时**以 §4 为准**；§3.2 的 libjbc 链接阻塞已由
  0.4.27 抬版解除（见该节尾注）。
- 代码基线：`mjt` HEAD `f80450c`；上游 `mjbase` HEAD `80cf053`（本仓经 luiox-repo 包词条消费 `mjbase 0.0.31`）
- 工作区：W0 / W1 已落地**但未提交**（`git status` 见 §2.3），W2–W4 未动代码
- 用户裁决：**统一 `FacadeSession`；不做兼容层、不设别名期；按波次直接推进**

---

## 0. 一句话现状

删掉了 `IWorkspace`（59 纯虚）和 `JarWorkspace` 空壳（W0）；把 workspace 的 `supporting_` 与 `ProjectSession::mountedContainers` 合成一张会话级 `ContainerRegistry`，并消除了挂载时的重复容器导入（W1）；`xmake -r mjt_mcp_core` 强制重建通过。**还差 W2（编辑单入口 + 格式中立）、W3（查询面 DB-first 收尾）、W4（FacadeSession 统一）、W5（契约/文档/测试收尾）**；`mjt_mcp_unittest` 可执行文件链接被上游 `libjbc` 的 CRT 符号阻塞，属环境问题，不是代码错误。

---

## 1. 问题与裁决（接手前先读）

用户指出的结构问题，代码证据均已核实：

| 问题 | 证据 |
|---|---|
| 接口数量爆炸且不稳定 | `i_workspace.hpp` 59 个纯虚；该文件仅 5 次提交，但 #58 全量签名改 `Utf8StringRef`、#87 塞 `analysisDb()` 默认实现、#98 一次删掉 5 个搜索方法；`replaceClassBytes(...)` 这类签名随分析需求被反复冲击 |
| 接口毫无必要 | 唯一实现 `WorkspaceBase`；`JarWorkspace` 是 16 行空壳；无 mock / 无第二实现；CLI 8 个 `cmd_*.cpp`、pysdk、`ProjectSession` 全持具体类；`bindLedgerStore/flushLedger/compactLedger/bindAnalysisDb` 不在接口里；`openJar(path, decoder)` 是非虚重载 |
| jar 假设焊死 | `ArtifactKind{None,Class,Jar}`、`openJar/openClass/addSupporting*/removeSupportingJar/diffJar/analyzeJarRepair/applyJarRepair/exportJar/exportClass/commitWorkflowCandidateJar/manifestAttributes`、`JarDiffResult/JarRepairRequest/WorkflowCandidateCommitRequest`；底层其实已有格式中立的 `mjbase::model::MountedContainerSet`（只认 `IResourceContainer*` + role + id，带 `!/` 复合寻址扩展位） |
| 层层嵌套 | `mjbase/mjbase/dev_plan.md:921-929` 裁决「workspace 概念不留在基座、66 虚方法绑死 class/JAR、dex/apk 无插入位」，但处置是「12 源文件整体移交 mjt，正文零改动，消费面随迁不受影响」——欠账转移而非解决；随后 RFC 0001 P1–P3 落地 bundle / `EditLedgerStore` / `AnalysisCacheStore` / `AnalysisDb` / `containerStack`，旧 `WorkspaceBase` 未拆，形成查询双轨与挂载双轨 |

目标形态（后续波次都以此为准）：

```
ProjectSession（唯一组合根，只持状态）
├── 容器层  importer/exporter registry → IResourceContainer（格式中立）
├── 事实层  AnalysisDb（唯一查询入口；新增分析 = 事实表 + ingest + 查询）
├── 事务层  apply(EditCommand, MutationRequest) 单入口 / EditLedgerStore（undo/redo/savepoint）
└── 算子层  无状态自由函数 + kind→handler 注册表（新增分析只加一行注册）
```

**核心不变量：新增分析不再修改任何公共类头文件。**

---

## 2. 已完成（文件级清单）

### 2.1 W0 — 删除接口与空壳（任务 #4，已完成）

- 删除 `mjt/src/workspace/i_workspace.hpp`（306 行）、`mjt/src/shared/jar_workspace.hpp`（18 行）。
- `mjt/src/workspace/workspace.hpp`：`WorkspaceBase` 去 `: public IWorkspace`、去 62 处 `override`；补显式 include `class_mutation_service.hpp` + `mjbase/model/container_stack.hpp`。
- 15 个文件 `IWorkspace*` / `&` / `using` / 前置声明改 `WorkspaceBase`：`mcp/src/tool/{query,mutation,service,session,json,decompiler,decompiler_support,shadow_slot}.{cpp,hpp}`、`src/shared/jar_remapper.{cpp,hpp}`、`src/workspace/workspace_types.hpp`。
- `ProjectSession::workspace` 改 `unique_ptr<mjt::workspace::WorkspaceBase>`；`openProjectSession` 直接实例化。
- 删掉 `mjt/test/workspace/workspace_test.cpp` 的假多态 TEST（原 1596–1643）。
- 同步 `docs/audit/pysdk-coverage.md`、`docs/rfc/0001-analysis-project-resource-container.md` 的失真描述。

### 2.2 W1 — 容器登记归一（任务 #6，已完成）

- 新增 `mjt/src/workspace/container_registry.hpp`：`ContainerRecord`（`id` / `supportId` / `path` / `fingerprint` / `role` / `roleLabel` / `shared_ptr<ResourceContainer> container`）与 `ContainerRegistry`（`records / nextSupportId / clear / findBy* / attach / detachBySupportId`）。
- `WorkspaceBase`：删 `SupportingResource` 结构、`supporting_` 向量、`nextSupportingId_`；改为 `ownedRegistry_` + 借用 `registry_`（新增 `bindContainerRegistry`）；`openJar/openClass` 调 `registry_->clear()`；`addSupportingRole / removeSupportingJar / supportingResources / analysisScanContainer / supportingClassIds / analysisGraph` 全部读写同一张表（字段改 `supportId` / `roleLabel`）。
- `ProjectSession`：删嵌套 `MountedContainerRecord` 与 `mountedContainers` 向量，改 `using MountedContainerRecord = mjt::workspace::ContainerRecord` + `ContainerRegistry containerRegistry`；`project_session.cpp` 在 `bindAnalysisDb` 旁调 `bindContainerRegistry(&session->containerRegistry)`；`findMountedContainer` 遍历 registry。
- `mutation.cpp` `container_mount(library/runtime)`：不再调 `current->addSupportingContainer`（消除第二次 `importFromJar`），直接登记已导入容器（`nextSupportId()` + `roleLabel`），补 fingerprint 查重；detach 改 `findBySupportId` / `detachBySupportId`，删掉原 `mountedContainers.erase`；`support` 遗留分支仍走 `addSupportingJar`（委托同一 registry）。
- `query.cpp` 的支撑工具分支不用改，经 `WorkspaceBase` 委托落到同一 registry。

> **注意（与任务 #6 标题的偏差）**：任务 #6 标题写了「含上游 mjbase 定版」，**实际落地没有改上游任何代码**——用仓内 `ContainerRegistry` + 现成的 `MountedContainerSet` API 绕开了。只有在后续决定「让 `MountedContainerSet` 本身成为唯一登记面（给它补 path/fingerprint 字段）」时，才需要上游定版 + luiox-repo 追加包词条 + 本仓 bump `add_requires`。

### 2.3 当前工作区状态

```
 M 23 个已跟踪文件（+179 / -518）
 D mjt/src/workspace/i_workspace.hpp
 D mjt/src/shared/jar_workspace.hpp
?? mjt/src/workspace/container_registry.hpp        ← W1 新增，需随提交一起 add
?? .workbuddy-ai/                                  ← 会话记忆，勿提交
?? 16                                              ← 0 字节来历不明文件，勿动
?? mjt/test/workspace/*.cpp（6 个）                 ← 他人在写的陈旧产物，见 §6.4
```

### 2.4 验证记录

| 命令 | 结果 |
|---|---|
| `xmake -r mjt_shared` | 通过（280s） |
| `xmake -r mjt_mcp_core` | W0 后通过（315s）；W1 后再次强制重建通过（257s） |
| `xmake build mjt_shared` / `mjt_mcp_core`（增量） | 1 秒假 ok（obj 时间戳 10:23 早于源码 11:35 却未重编），**不可信** |
| `xmake build mjt_mcp_unittest` | 链接失败，见 §3.2 |

---

## 3. 环境与构建（接手必读）

### 3.1 当前配置

- C 盘：用户已清理，16 GB 空闲（此前 0 GB，曾导致 `LNK1180` 磁盘空间不足）；D 盘约 9.8 GB。
- `xmake f -c --with_tests=y` 已执行成功；`xmake.conf` 中 `with_tests = true`、`with_pysdk = true`。
- 全局 `http.proxy = http://127.0.0.1:10809` **仍在**（未改用户配置）。拉 git / 重新配置时只对单次命令屏蔽：

  ```sh
  env -u HTTP_PROXY -u HTTPS_PROXY -u ALL_PROXY -u http_proxy -u https_proxy -u all_proxy \
      GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=http.proxy GIT_CONFIG_VALUE_0= \
      xmake f -c --with_tests=y
  ```

- 上游包：`xmake.lua:232 add_requires("mjbase 0.0.31")` 走 luiox-repo 中心包仓；改本地 `D:\WorkSpace\mjbase` 源码**不会**进入本仓构建，必须上游 main 定版 → 追加包词条（只追加不修改）→ 本仓 bump 版本号。

### 3.2 未解决的构建阻塞（任务 #5）

`mjt_mcp_unittest` 链接报：

```
libjbc.lib(code_dom.cpp.obj) : error LNK2019: 无法解析的外部符号 __imp__wassert
..\mjt\build\windows\x64\release\mjt_mcp_unittest.exe : fatal error LNK1120
```

- `libjbc 0.4.26` 包实例 `manifest.txt` 写 `runtimes = "MT"`，obj 却引用动态 CRT 的 `__imp__wassert`；三个包实例的 `libjbc.lib` 内容一致（均 21,138,986 字节），**不是选错实例**。
- 曾因 C 盘满 + 包实例被 xmake 标成 `.md-unlinked` 缺 `lib/libjbc.lib`（`LNK1181`），已用「复制到正常实例目录名」恢复；重新配置后不再报该错。
- 恢复路径二选一：① 上游用一致 CRT 重编 `libjbc` 包；② 在链接闭包显式提供 `ucrt.lib`。**不要改项目链接配置去掩盖它**，也不要动用户 C 盘其它文件。
- **后续（终态注记）**：已走路径①——上游以一致 CRT 重编发布 `libjbc 0.4.27`
  （commit 锁 `0c54fc2`），本仓 bump 后 `mjt_mcp_unittest` 链接恢复，157/158
  通过（1 skip = CFR 环境门）。本节仅存档排障过程。
- 解决后单独执行（构建与验证不并成一条命令）：

  ```sh
  xmake build mjt_mcp_unittest
  xmake run mjt_mcp_unittest --gtest_brief=1
  ```

### 3.3 AGENTS.md 纪律（已实证）

- **delete-then-rebuild**：内容哈希增量会跳过「回到旧状态」的文件，信测试结果前必须 `rm <exe> && xmake -r <target>`；`xmake build` 的 1 秒 ok 不作数。
- 构建与验证分开；一次一个 target；多进程/多 agent 并行开发用 `/tmp` 目录锁错峰。
- 曾出现一个 `cl.exe` 编译 `mbase/src/mbase/asm/insn/asm_expr_bridge.cpp` 挂死 32 分钟（CPU 0.09），已 kill；若再次出现，用进程检查确认后只终止卡死的子进程，不要重跑整条 `-r` 链。

---

## 4. 全计划完成度矩阵

| 波次 | 目标 | 状态 | 还差什么 | 验收门 |
|---|---|---|---|---|
| **W0** | 删 `IWorkspace` / `JarWorkspace` | ✅ 已完成 | 单测可执行文件回归（被 §3.2 阻塞） | `mjt_shared`/`mjt_mcp_core` 强制重建通过 ✅ |
| **W1** | 组合根归一、消除双轨 | ✅ 已完成 | 若坚持让 `MountedContainerSet` 成为唯一登记面，需上游定版（见 §2.2 注意） | `xmake -r mjt_mcp_core` 通过 ✅；`container_mount` 挂载/卸载回归待测 |
| **W2** | `apply(EditCommand, …)` 单入口、格式中立 | ✅ 已完成 | — | 编辑族全量回归 + 导出保真 golden ✅ |
| **W3** | 查询面全量 DB-first | ✅ 已完成 | —（overridesOfMethod 留引擎，裁决随提交注记） | DB-first 等价断言 + `mjt_mcp_unittest` ✅ |
| **W4** | 统一 `FacadeSession` | ✅ 已完成 | —（条目族/改名搬移/remap/Jasm 写边界全收敛 `applyEdit`；建议引擎整容器重写为注明的唯一例外） | `--with_pysdk=y` 全量单测 67/67 + SDK selftest 25 阶段 ✅ |
| **W5** | 契约/文档/测试收尾 | ✅ 已完成（除测试归属） | 6 个未跟踪 `mjt/test/workspace/*.cpp` 归属待作者确认（不删文件） | 全量回归 + CHANGELOG ✅ |

---

## 5. 剩余波次（逐项可接手）

### 5.1 W2 — 单入口编辑与格式中立（任务 #7）

现状证据：

- `workspace.hpp` 仍是 god object：编辑族成对方法 `validateJasm/applyJasm`（219/224）、`validateClassBytes/applyClassBytes`（229/234）、`validateClassMutation/applyClassMutation`（239/243）、`replaceClassBytes`（253）、`analyzeRemap/applyRemap`（261/265）、`analyzeRename/applyRename`（270/273）、`validatePipeline/applyPipeline`（277/281）、`runWorkflow`（285）；jar 族 `commitWorkflowCandidateJar`（290）、`diffJar`（295）、`analyzeJarRepair/applyJarRepair`（298/302）、`exportJar/exportClass`（356/359）、`manifestAttributes`（170）。
- `ArtifactKind{None,Class,Jar}` 在 `workspace_types.hpp:95`；jar DTO 在 `workspace_types.hpp:738/754/772`。
- MCP 工具面**已收敛到 11 个粗工具**（`contract.cpp:170 toolsList()`，11 处 `result.push_back`）：`project_open / project_close / project_info / container_mount / class_query / search / xref_query / decompile / edit_apply / edit_history / project_export`。但 `query.cpp` / `mutation.cpp` 内部仍是长 `if (name == ...)` 链，且保留旧分支名（如 `workspace_add_support`）。

动作：

1. 定义 `EditCommand`（tagged union：ClassBytes / Jasm / JasmDiff / ClassMutation / Rename / RemapPlan / Pipeline / JarRepair / Workflow…）与 `MutationRequest` 组合；落 `apply(Session&, const EditCommand&, const MutationRequest&) -> MutationResult` 单入口，把 dryRun / expectedRevision / 验证门 / ledger append 收进一条管道。
2. 删除所有 `validate*/apply*` 成对方法，工具层改 `kind→handler` 注册表（新增编辑形态 = 加一个变体 + 一个 handler + 一行注册，不改公共类头）。
3. jar 专属面下沉 `jar_ops/`：`diffJar / *JarRepair / commitWorkflowCandidateJar / manifestAttributes / exportJar / exportClass` 变成自由函数服务；主会话只认 `IResourceContainer` 与 URI。
4. `open` / `export` 改 importer/exporter registry（jar / class / 未来 apk / dex），用 `mjbase::model::IResourceImporter`/`IResourceExporter` 扩展位；删除 `ArtifactKind`，改 `ContainerFormat` + capability 位（hasManifest 等）。
5. 顺手删掉 `WorkspaceBase::addSupportingContainer`（W1 后已无生产消费方，只剩未跟踪测试在用）与 `addSupportingRole` 的 jar 命名，不设兼容 wrapper。

验收：编辑族全量回归、`#74` 导出保真 golden（条目集一致 / MANIFEST 未编辑逐字节复制）、`xmake -r mjt_mcp_core`、新增 dex/apk importer stub 跑通 open→ingest→query→edit→export。

风险：importer registry 若需要 mjbase 侧新增接口，须先走 §3.1 的上游定版流程；`MutationRequest` 默认值 `= {}` 在旧签名上是编译期绑定，切单入口后要显式传，别留隐式兼容。

### 5.2 W3 — 查询面 DB-first 收尾（任务 #8）

已切到 `session.analysisDb->` 的族（`query.cpp:660/730/811/848/903`）：`searchClassesPaged / searchMembersPaged / searchStringsPaged / searchResourceStringsPaged / searchNumbers`。**刻意保留**的引擎活扫描：`query.cpp:940 current->searchInstructions`（千万行级事实面，DB 反而慢）。

仍走 `current->` 的约 25 处，需逐族切到 `AnalysisDb` 类型化 API（`analysis_db.hpp` 1712 行 / 82 个 `Result` 方法 / 11 个分组，schema v13）：

- `query.cpp`：517 `listResources`、540 `readResource`、576 `manifestAttributes`、1066 `listClassSummaries`、1083/1131 `classInfo`、1093 `readClassBytes`、1135 `getJasm`、1168 `methodInfo`、1174 `methodBytecode`、1191/1201 `getMethodJasm`。
- `mutation.cpp`：585/928 `classInfo`、695 `xrefsToMethod`、699 `xrefsToField`、703 `xrefsToClass`、705 `calleesOfMethod`、789 `classRelations`、823 `overridesOfMethod`、867/934 `getJasm`、905 `getMethodJasm`。
- `decompiler.cpp`：222 `methodInfo`、248/400 `classInfo`。
- 注：部分「精确单类读取 / Jasm」没有 DB 等价面，正确做法是保留为**无状态算子自由函数**（输入容器字节 + URI，输出 DTO），从 `WorkspaceBase` 方法变成 `analysis_ops/`；不要为了让 `current->` 消失而硬塞进 DB。

切换期用双读等价断言（旧引擎结果 vs DB 结果，行序 / supporting 标注 / shadow 显示名零漂移），收口沿用 `workspace.cpp` 的 `finalizeReferenceRows`（`sortReferences` + `annotateSupportingSources`，现读 `ContainerRegistry`）。删除 `workspace.cpp` 内对应查询实现。

### 5.3 W4 — 统一 `FacadeSession`（任务 #9，用户已裁决）

- 现状：`mjt-py/src/pysdk_facade.hpp:114 class FacadeSession`，`:121 std::unique_ptr<mjbase::model::ResourceContainer> container`，自管树缓存 / 脏纪律 / uid / rename token；`pysdk_facade.cpp` 内 `container` 直接引用 **54 处**（另有 `bind_facade.cpp` / `pysdk_jasm.cpp` 写点）；SDK 侧**没有** revision / dry_run / undo / redo / savepoint / ledger（`docs/audit/pysdk-coverage.md` §4.2 / §5 明确这是 MCP 收敛硬前置）。
- 动作：让 `FacadeSession` 换持同一 `Session`/`EditEngine`（W2 的单入口），写边界统一走 `apply(EditCommand, …)`；删 SDK 自管脏纪律与重复事务代码；`StaleRefError`（fail-closed）与 `mjt_util.py` 契约保持。
- 验收：`xmake f --with_pysdk=y --with_tests=y`、`xmake -r mjt_pysdk_unittest`、`xmake run mjt_pysdk_unittest --gtest_brief=1`、`pysdk_selftest.py` 25 阶段；需 Python 3.14 + pybind11 环境（`MJT_PYTHON_HOME` / `MORPHER_PYBIND11_ROOT`），CI 目前**不构建** `mjt.pyd`（`.github/workflows/ci.yml` 只有 `--with_tests=y` 两个 job）。
- 同时按 RFC 0001 §8 处理「同版本删除旧工具名」：外部 11 个粗工具已就位，删的是内部旧分支名与 SDK 侧遗留入口，不设别名期。

### 5.4 W5 — 契约、文档与测试收尾

- `docs/rfc/0001-analysis-project-resource-container.md` 仍标「草案待评审」，但 P1–P3 已落地（状态行已加 W0 注记）；需按实际落地结果更新结论、§10 阶段表、附录 A 证据索引。
- `docs/audit/pysdk-coverage.md` §4.2 / §5 已部分同步；W4 完成后复核。
- `AGENTS.md`（仓库约定）、`CHANGELOG.md`（破坏性变更：删接口、工具名、schema/契约变化）同步。
- `mjt/test/workspace/` 6 个**未跟踪**测试 cpp 是陈旧/在写产物：`workspace_test.cpp` 仍调用 #98 已退役的 `searchClasses/searchMembers/searchStrings/searchNumbers`（790–884、978–984），`workspace_supporting_role_test.cpp` 仍调 `addSupportingContainer`（93/111/132，W2 计划删除）。`mjt/xmake.lua:146 add_files("test/workspace/*.cpp")` 会把它们编进 `mjt_workspace_unittest`，**该目标当前必然编译失败**。接手前先与作者确认归属：要么按 DB-first / registry 重写，要么从 glob 移除；不要直接删文件。
- 根目录 0 字节文件 `16`：来历不明，勿动、勿提交。

---

## 6. 阻塞与前置依赖（按优先级）

1. **libjbc CRT 符号**（任务 #5）：阻塞 `mjt_mcp_unittest` 运行，与 W0/W1 代码无关；见 §3.2。
2. **上游 `mjbase` 定版**：仅当 W2 的 importer/exporter registry 需要 mjbase 侧新接口，或决定扩展 `MountedContainerSet` 成为唯一登记面时才触发；W1 已用仓内 `ContainerRegistry` 绕开，不是当前硬前置。
3. **pysdk 构建环境**：Python 3.14 / pybind11 / `mjt --script` 宿主接线（audit §4.3 指出宿主目前只被 unittest 消费，CI 不覆盖），W4 验收前必须准备好。
4. **未跟踪测试文件归属**：见 §5.4，影响 `mjt_workspace_unittest` 目标。

---

## 7. 任务台账与建议顺序

| 任务 ID | 内容 | 状态 |
|---|---|---|
| #1–#3 | 调研与方案 | ✅ |
| #4 | W0 删除接口与空壳 | ✅ |
| #5 | 解除构建环境阻塞并完成 W0 回归 | ⬜ 阻塞中（§3.2） |
| #6 | W1 组合根归一 | ✅（标题含上游定版，实际未改上游，见 §2.2） |
| #7 | W2 单入口编辑与格式中立 | ⬜ |
| #8 | W3 查询面 DB-first 收尾 | ⬜ |
| #9 | W4 统一 FacadeSession | ⬜（用户已裁决） |

建议顺序：**先解 #5**（否则所有波次只能做编译期验证）→ **W2**（给 W3/W4 提供稳定扩展点）→ **W3** → **W4** → **W5**。W2 与 W3 可以并行开发不同文件，但按 AGENTS.md 用 `/tmp` 目录锁错峰构建。

---

## 8. 接手第一步（可直接执行）

```sh
# 1) 确认工作区没被别人改动
git -C D:/WorkSpace/mjt status --short
git -C D:/WorkSpace/mjt rev-parse --short HEAD      # 期望 f80450c

# 2) 先恢复单测链接（§3.2），再跑 W0/W1 回归
xmake f -c --with_tests=y
xmake build mjt_mcp_unittest
xmake run mjt_mcp_unittest --gtest_brief=1

# 3) 进入 W2：先定义 EditCommand，再把 edit_apply 的 if 链换成 handler 注册表
#    起点文件：mjt/src/workspace/workspace_types.hpp（MutationRequest 附近）
#             mjt/src/workspace/workspace.hpp:219-302（待删的编辑/jar 方法族）
#             mjt/mcp/src/tool/mutation.cpp（edit_apply 分发）
#             mjt/mcp/src/tool/contract.cpp:686（edit_apply 契约）
```

提交前：`git status` 确认只提交源码与文档，把 `.workbuddy-ai/`、`16`、未跟踪测试 cpp 排除；提交信息用约定式前缀并带模块作用域（`refactor(workspace): …`、`feat(mcp): …`）。

---

## 9. 约定速查（AGENTS.md）

- Doxygen：`///` 行注释；`/** */` 禁止。
- No RTTI：`dynamic_cast` / `typeid` 禁止，用 `typed::cast<T>` 家族。
- 类型：`i32` / `u32` / `usize` / `Result<T,E>`；错误走 `Result` 值语义。
- 字符串：`Utf8String` 家族；API 借用视图 `std::string_view`，不新增 `std::string` 存储（字节边界除外）。
- Includes：模块内 `"file.hpp"`，跨模块 `"module/path/file.hpp"`，禁 `../`。
- 测试输出 `<project_root>/test/`，基线 `test_resource/`；不提交测试产物与临时文件。
- 构建门：`xmake f --with_tests=y` → `rm <exe> && xmake -r <target>`；构建与验证分开。
- 加依赖前先确认消费方；无消费方的模块不进仓。
- 用户裁决：**不堆兼容层、不设别名期、同版本删除旧名**。

---

## 附录 A · 关键证据索引

| 断言 | 位置 |
|---|---|
| 上游 workspace 退役裁决 | `mjbase/mjbase/dev_plan.md:921-929` |
| 双层模型与 11 粗工具规划 | `docs/rfc/0001-analysis-project-resource-container.md` §1 / §8 / §10 |
| SDK 无事务面、54 处 container | `docs/audit/pysdk-coverage.md` §4.2 / §5；`mjt-py/src/pysdk_facade.hpp:114/121` |
| 包版本与上游节奏 | `xmake.lua:232`；`AGENTS.md`「mjbase 基座」 |
| 测试 target glob | `mjt/xmake.lua:146` |
| 工具面 11 个粗工具 | `mjt/mcp/src/tool/contract.cpp:170` + 11 处 `result.push_back` |
| 已切 DB 的查询族 | `mjt/mcp/src/tool/query.cpp:660/730/811/848/903` |
| 仍走 `current->` 的查询族 | `query.cpp:517/540/576/1066/1083/1093/1131/1135/1168/1174/1191/1201`；`mutation.cpp:585/695/699/703/705/789/823/867/905/928/934`；`decompiler.cpp:222/248/400` |
| 分析库能力面 | `mjbase/mjbase/src/mjbase/project/analysis_db.hpp`（schema v13，82 个 `Result` 方法，11 个 `//--` 分组） |
| 格式中立容器栈 | `mjbase/mjbase/src/mjbase/model/container_stack.hpp`（`MountedContainerSet` / `ContainerRole` / `!/` 扩展位） |
| W1 登记表 | `mjt/src/workspace/container_registry.hpp` |
