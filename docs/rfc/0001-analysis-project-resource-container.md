# RFC 0001：分析工程 + 资源容器双层模型

- 状态：**草案待评审**（本文所有"方案"小节均为草案，未定案；"现状"小节均带代码证据）
- 作者：mjt 架构重设计
- 日期：2026-10-23
- 关联：luiox/morpher#1092（分析工程）、luiox/morpher#1096（实体口径 v1 冻结）、mjbase 私有仓 `doc_ai/analysis-project-store-design.md`（R45 评审通过稿）

---

## 1. 背景与动机

mjt 当前的分析载体是 **进程内 workspace**：MCP 会话里 `workspace_open` 打开一个 JAR，构造 `mjt::JarWorkspace`（继承 `mjbase::workspace::WorkspaceBase`，`mjt/src/shared/jar_workspace.hpp:12-16`），全部状态（原始快照、工作副本、undo/redo 栈、savepoint）只活在内存里，随 `workspace_close` 或进程退出蒸发（`mjt/mcp/src/tool/session.cpp:218-238`、`mjbase/mjbase/src/mjbase/workspace/workspace.hpp:347-352`）。

这个模型的三个结构性问题：

1. **无跨会话持久**。批注、编辑台账、xref/decompile 缓存全部随进程消失；同一个 JAR 的重复分析成本每次从头付。
2. **编辑语义绑死单容器**。savepoint/undo/redo 是整容器内存克隆（`workspace.hpp:350-352`、`mjbase/mjbase/src/mjbase/workspace/workspace.cpp:3942`），既贵（每个 savepoint 一份全量字节）又无法 provenance（没有"哪次编辑改了哪个条目、前后字节是什么"的台账）。
3. **MCP 工具面碎片化**。`contract.cpp:toolsList()` 实测注册 **74 个工具**（`mjt/mcp/src/tool/contract.cpp:345-745`，74 个 `result.push_back`），大量同族工具只是选择器不同（5 个 search、6 个 patch 变体、5 个 xref 变体），把"快速问答"与"批量组合"混在同一面上。

目标形态：**资源容器（不可变侧，管字节）+ 分析工程（可持久化侧，管分析数据）**双层模型，对标 Ghidra project / IDA .i64 / CodeQL db；同时 MCP 工具面收敛到约 10 个粗粒度入口，细粒度能力走已存在的嵌入式 Python SDK（`mjt --script`，`mjt-py/src/pysdk_embed.hpp:3`）。

---

## 2. 既有依据：这不是从零设计

调研确认，"分析工程 + resource container"在 mjbase 侧已有成型的规划与部分落地，本 RFC 的职责是**把既有设计接到 mjt 消费面上**，不另起炉灶。

### 2.1 已落地的实体层：`mjbase::project::AnalysisProject`（schema v1）

`mjbase/mjbase/src/mjbase/project/analysis_project.hpp`（module-map 记为"analysis_project（schema v1）"，`mjbase/mjbase/doc_ai/module-map.md:35`）：

- kind 串 `"mjt-analysis-project"`、版本 1，不识别版本整文件拒载（`analysis_project.hpp:48-51`）；
- `inputs[]`：被分析对象登记，`{path, fingerprint, role}`，role ∈ `Primary|Support`（`:54-58,70-75`）——**"一个工程挂 N 个容器"的登记结构已存在**；
- `annotations`：四级锚定（class / method+owner+name+desc / field / instruction+index），id 单调不复用（`:60-95`）——笔记/标注层已存在；
- `shadow_sidecar`：以引用（path+fingerprint）而非内嵌方式挂接别名表（`:97-102`）；
- 严格 schema：未知字段/重复字段整文件拒载；确定性写出（parse(write(parse(x))) 恒等，`:45-47,208-212`）。

### 2.2 已落地的存储引擎：`mjbase::project::ProjectStore`

`mjbase/mjbase/src/mjbase/project/project_store.hpp`，设计源为私有仓 `doc_ai/analysis-project-store-design.md`（R45 评审通过稿，`:17-18`），对应 #1092 分解 2。已经提供：

- **bundle 磁盘布局**：目录内 `project.json`（实体事实源，#1096 冻结 v1）+ `store.json`（引擎清单）+ `store.lock`（单写者锁）+ `sections/`（section 内容文件）+ `tmp/`（事务暂存，同卷 MUST），kind `"mjt-project-store"`、版本 1（`:47-62`）；
- **section 模型**：`sections[]` 条目含 name（语法 `^[a-z0-9][a-z0-9._-]{0,63}$`，`:204-210`）、format（Json/Jsonl/Opaque，`:142-147`）、`section_version`（生产者私有版本，读者拒读更新值，`:169`）、`content_hash`（`"sha256:"+64hex`，`:170`）、`byte_size`（截断/追加双检出，`:171`）、`profile`（对内容有影响的构建选项串，`:174-178`）、`depends[]` `{path, fingerprint}`（对输入的依赖，`:149-155`）、`produced_by`（生产者自述串，`:180`）——**缓存失效键的全部字段已在 schema 里**；
- **原子写与事务**：单 section 原子写（staged `tmp/txn-<pid>-<mono>/` + 单次换名 + Windows 撞读者重试 3×50ms，`:294-311`）；store.json 读-改-写事务（墓碑先删文件后提清单、CP-1 降级态拒绝换清单，`:366-398`）；
- **恢复扫描**：`scanBundleRecovery` 只读全量盘点，产出 Ok/Damaged/Orphan 分类账；孤儿两档处置（清单可载 → 首次 flush 后 prune；清单不可载 → 永久保留零自动 prune）（`:421-497`）；
- **单写者锁**：`acquireStoreLock` 独占创建 + PID fail-closed 探测（证明不了死不得动锁）+ 死锁接管恰好一次重试 + `stealLock` 人工抢占（`:570-666`）。

### 2.3 已落地的容器层：`mjbase::model`

`mjbase/mjbase/src/mjbase/model/`（module-map 批 2，libarchive 后端保留，`module-map.md:33`）：`IResource`（URI + 类型化载荷：Class/Manifest/Jar/Unknown，`resource_model.hpp:39-47,111-122`）+ `IResourceContainer`（unordered_map + Utf8StringArena，重复条目 first-wins 对齐 JVM 类加载并记录 `duplicateResourceUris`，issue #962，`resource_container.hpp:9-11,32-110`）。

### 2.4 公私边界与批次现状

- `mjbase/doc_ai/migration-boundary.md`：keys/容器/映射/指令工程/引用索引/pass 契约属公开仓；project 批 1 已落（`migration-boundary.md:60`）、workspace 批 4 已落（`:63`）。
- mjt 经 `mjbase_root` option 源码级依赖 mjbase（`mjt/xmake.lua:175-181`，默认兄弟目录 `../mjbase`）。

### 2.5 结论

双层模型的**下半层（工程文件格式、存储事务、锁、容器）在 mjbase 已定案并落地**；缺的是 mjt 侧三件事：① workspace 会话与工程 bundle 的接线（挂载/flush）；② 编辑事务从内存克隆重建模为 overlay + 台账；③ MCP 工具面收敛。本 RFC 全部术语沿用既有命名：工程（AnalysisProject）、bundle、section、容器（ResourceContainer）、条目（IResource/resource）。

---

## 3. 术语与模型（沿用 + 扩展）

| 术语 | 定义 | 既有出处 |
|---|---|---|
| **分析工程**（project） | 可持久化侧实体：容器登记 + 批注 + 编辑台账 + 缓存，落盘为一个 bundle 目录 | `analysis_project.hpp:119-124` |
| **bundle** | 工程的磁盘形态：`project.json + store.json + store.lock + sections/ + tmp/` | `project_store.hpp:56-62` |
| **资源容器**（container） | 不可变侧字节集合：JAR/归档的条目按 URI 索引，first-wins | `resource_container.hpp:32-53` |
| **条目身份**（entry identity） | `(容器 digest, 条目 URI)` 二元组；容器 digest = 内容指纹 | 本文定义（fingerprint 概念已存在于 `workspace.cpp:898-914`、`analysis_project.hpp:73`） |
| **primary / library / runtime 容器** | 工程内容器的角色：主分析对象 / 依赖库 / 运行时（JDK） | v1 只有 `Primary|Support`（`analysis_project.hpp:54-58`），本文提议 v2 扩展（草案待评审） |
| **派生容器**（derived container） | 对某容器施加编辑链后物化的字节集合；原容器永不回写 | 对齐 workspace 既有"原始快照不可变 + 工作副本 + export 写新文件"（`workspace.hpp:56-57,298`、`workspace.cpp:4037-4049`） |
| **pending-edit overlay** | 已应用到内存工作副本、尚未定稿（commit 到台账）的条目级编辑集合；工程级而非会话级 | 本文重建模（现状是整容器克隆，见 §6） |
| **编辑台账**（edit ledger） | append-only 的事务记录：每次已提交编辑的前后条目 digest 与来源 | 本文新增（现状无持久台账） |
| **缓存失效键** | `depends[](容器 path+fingerprint) + produced_by(引擎) + section_version + profile` | **既有**：`project_store.hpp:149-155,169-180` |

**digest 口径决定（草案待评审）**：现状两套指纹并存——workspace 容器/支撑 JAR 用 `fnv1a64:16hex`（`mjbase/mjbase/src/mjbase/workspace/workspace.cpp:887-914`），project store 的 section 校验用 `sha256:64hex`（`project_store.hpp:170,290`）。本 RFC 提议：**容器与条目身份统一升 sha256**（与 store 对齐、满足 provenance 与跨工程复用），fnv1a64 前缀兼容读（旧登记仍可识别，遇即提示重算）。

---

## 4. 磁盘格式草案（草案待评审）

### 4.1 工程目录布局

基座直接复用 ProjectStore bundle（§2.2），不发明新格式：

```
samples/app.jar.mjt-project/        # bundle 根（目录后缀是 CLI 约定，引擎不校验，
                                    #   project_store.hpp:56-57）
├── project.json                    # 实体事实源：容器登记(role 扩展) + 批注；
│                                   #   #1096 冻结 v1 读者仍可读（零破坏边界，
│                                   #   project_store.hpp:38-41）
├── store.json                      # 引擎清单：section 登记表（既有 schema）
├── store.lock                      # 单写者锁 {pid, created_at, owner}
├── sections/                       # 分析数据（全部经 writeSectionBytes 原子写 +
│   │                               #   sha256/size 双登记 + 恢复扫描）
│   ├── class-index-<d8>.jsonl      # 类/成员索引（键含容器 digest 前缀 d8）
│   ├── xref-<d8>-<profile8>.jsonl  # xref 缓存（失效键见 §7）
│   ├── decompile-<d8>-<engine>.bin # 反编译缓存（opaque，键含引擎与 options）
│   ├── edit-ledger.jsonl           # 编辑台账（append-only jsonl，见 §6）
│   ├── overlay-head.json           # pending-edit overlay 头（当前未定稿编辑集指针）
│   └── notes.jsonl                 # 工具批注流水（人工批注仍在 project.json）
├── derived/                        # 派生容器物化产物（export 落点，v2 起作为
│                                   #   可再挂载容器登记回 project.json）
└── tmp/                            # txn-<pid>-<mono> 暂存（既有恢复扫描覆盖）
```

要点：

- **一切分析数据皆 section**：沿用 format/section_version/content_hash/record_count/profile/depends 既有字段（`project_store.hpp:157-183`），不新增平行机制。
- **project.json 与 store.json 分治不变**（`project_store.hpp:38-41`）：实体（容器/批注）只在 project.json；引擎私有数据只在 store.json。
- **批注分两档**：人工批注沿用 project.json annotations（四级锚定已定）；工具高频批注（如批量标记）走 notes.jsonl section，避免人工实体文件被高频重写。

### 4.2 格式版本与迁移策略

沿用既有纪律并补两点：

1. **project.json**：v1 顶层字段只增不删不改义、读者忽略未知顶层字段（`analysis_project.hpp:45-47` 为字段严格版；store.json 侧已是"顶层忽略/条目拒载"双轨，`project_store.hpp:249-258`）。v2 新增 role 枚举值与容器挂载结构时**升 version 并保留 v1 读者可读性**（v1 读者遇到新 role 值应按 Support 降级或拒载——需在 #1096 侧定案）。
2. **store.json**：条目字段严格（未知字段拒载），加字段必须升主版本（`project_store.hpp:159-162`）；不识别的 section format 按 opaque 收容保留不解释（`:250-258`）——这就是**前向兼容机制**：新版引擎写的未知 section，旧版引擎照常开工程、只是不读这些段。
3. **迁移工具**：提供一次性 `project migrate`（或 SDK 函数）做 v(n)→v(n+1)；拒绝自动降级写（对齐"降级孤儿态不得换新清单"的保守哲学，`project_store.hpp:120-127`）。

### 4.3 并发与锁

- **同机多进程开同一工程**：直接采用既有 `store.lock` 单写者锁——独占创建、PID fail-closed 探测、死锁接管、`stealLock` 人工抢占（`project_store.hpp:627-666`）。锁内容 `{"pid","created_at","owner"}` 支撑"database is locked by …"式报错。mjt 仓 AGENTS.md 的 `/tmp` 目录锁是**构建错峰**纪律（多 agent 并行开发），与工程锁是两回事，不得混用：工程锁管数据一致性，目录锁管构建资源。
- **读者**：无锁短句柄读 section（先 size 后 hash 校验，读时校验替代持锁，`project_store.hpp:319-334`）。
- **MCP 服务器与 SDK 脚本并发**：v1 约束"一个工程同一时刻至多一个写者进程"；MCP 服务器与 `mjt --script` 同时写同一工程被 store.lock 拒绝，属预期行为。只读旁路（如脚本读缓存）不需要锁。

---

## 5. 容器抽象（草案待评审）

### 5.1 v1：单 JAR + 多容器登记就绪

- **挂载**：`project_open(path, role=primary)` 打开主 JAR（对齐现状 `workspace_open`，`session.cpp:218-238`）；`container_mount(path, role=library|runtime)` 追加。登记写 project.json `inputs[]`，fingerprint sha256。
- **不可变红线**：原容器永不回写。现状已隐含此纪律（`original_` 快照 + `exportJar` 只写新文件，`workspace.hpp:56-57,347`、`workspace.cpp:4037-4049`），本 RFC 把它升格为显式契约：**一切编辑产出到派生容器**。
- **支撑 JAR 语义迁移**：现状 `addSupportingJar` 返回稳定 support ID + fingerprint、重复路径或内容被拒（`workspace.hpp:70-76`）、移除时失效依赖 classpath 的缓存（`:74-76`）——映射为 `container_mount(role=library)`，support ID 即登记 path，fingerprint 即登记 fingerprint。

### 5.2 迁移到多容器挂载（阶段 4）

v1 的 xref/层次分析已有"工作副本 + supporting 合并只读容器"先例（`workspace.hpp:321-323` `analysisScanContainer`：主容器 URI 优先，同 URI 支撑资源跳过）。多容器化 = 把这个合并视图从"1 主 + N 支撑"推广为"角色化容器栈"：解析顺序 primary > library（按挂载序） > runtime。

### 5.3 复合容器扩展位（v2 预留，本 RFC 只定寻址方案）

条目寻址统一为**挂载链 URI**：

```
<containerId>!/entry/path                 # 单容器（v1 恒此形态）
<containerId>!/WEB-INF/lib/a.jar!/com/x/A.class   # 复合：bang 链逐层下钻
```

- `containerId` = project.json 内登记的容器序号或 digest 前 8 位（登记期分配，工程内唯一）；
- 嵌套成员（WAR 内 JAR、fat jar 嵌套 jar、JMOD、JIMAGE）在 `inputs[]` 条目增加 `parent` 字段构成挂载树，嵌套字节作为派生容器缓存于 `sections/`（opaque），父容器 digest 变化即失效；
- **multi-release JAR**：条目解析按 JVM 规则归并（`META-INF/versions/N/` 版本桶，实际类取 ≤ 目标版本的最高 N），索引 section 记录归并结果；编辑寻址必须带版本桶限定符，防止跨版本误伤（与 dev_plan P0-6 "Multi-Release 条目版本分组"挂账同题，`mjbase/mjbase/dev_plan.md:25-26`）；
- v1 接口留位方式：`IResourceContainer` 不改（URI 是 opaque 键，`resource_container.hpp:36-48`），复合寻址只发生在工程层的索引/缓存键生成与挂载树登记，底层容器实现逐段懒加载即可。

---

## 6. 事务重建模：overlay + 台账（草案待评审）

### 6.1 现状证据

| 现状 | 证据 |
|---|---|
| commit 把编辑前整容器快照压入 `undo_`，清空 `redo_` | `workspace.cpp:2723-2724` |
| savepoint = `savepoints_[name] = cloneWorking()` 整容器克隆 | `workspace.cpp:3942` |
| undo 弹栈恢复整容器 | `workspace.cpp:3976-3982` |
| 打开新主资源/导入时清空 undo/redo/savepoint | `workspace.cpp:1173-1175,1241-1243` |
| 乐观并发控制：`MutationRequest{expectedRevision, dryRun, label}` | `workspace_types.hpp:108-113` |
| revision 只在提交/undo/redo/restore 后递增 | `workspace.hpp:90-91` |
| 导出 = 工作副本原子写全新文件 | `workspace.cpp:4037-4049` |
| 两阶段提交（候选验证 + `ClassResourcePendingCommit` 落盘） | `workspace.hpp:28-31` |

问题：savepoint/undo 栈是全量字节克隆（N 个 savepoint = N 份容器），无 provenance，不持久，换主资源即蒸发。

### 6.2 新模型

一切编辑表达为**台账事务**（ledger transaction），容器状态 = `原容器字节 ⊕ 台账重放`：

```
edit-ledger.jsonl 每行一个事务：
{ "txn": 42, "parent": 41, "label": "rename Foo->Bar",
  "base_container": "sha256:…",            # 编辑基线容器 digest
  "entries": [
    { "uri": "com/x/A.class",
      "before": "sha256:…", "after": "sha256:…",   # 条目级 digest 链
      "op": "replace",                       # replace|add|remove
      "payload": {"kind":"overlay", "ref":"overlay-42-0"}   # 字节在 overlay section
    } ],
  "produced_by": "mjt edit_apply rename", "created_at": "…" }
```

- **savepoint**：从"整容器克隆"降为**台账书签**（命名指针指向某 txn）。restore = 从书签重放到工作副本。零字节代价。
- **undo/redo**：台账头指针回退/前进一个事务；字节按需从 overlay section 取（before/after 双侧 digest 都可寻址）。不再维护平行克隆栈。
- **revert_class**：`class_query` 式定位该条目的最后 touch 事务，反向应用单条目（before 字节恢复）并记新事务——仍是台账操作，不是隐秘旁路。
- **pending-edit overlay**：MCP/SDK 的 `dry_run`（`workspace_types.hpp:111`）与未提交编辑驻留内存 overlay；**flush（工程关闭/显式 save）时**经 `writeSectionBytes` 原子落 `overlay-*` section + 台账行（manifest last 序，`project_store.hpp:336-398`）。崩溃窗口由既有恢复扫描兜底（Ok/Damaged/Orphan 三态 + 永不自动 prune 红线，`project_store.hpp:421-497`）。
- **revision**：内存 `revision_` 语义不变（乐观并发控制不折腾调用方），flush 时与台账 txn 号对齐。
- **provenance**：`entries[].before/after` digest 链 + `base_container` digest 使每个导出条目可追溯到"原容器第 X 字节态 + 事务链"；`derived/` 物化容器可在 manifest 写工程来源（可选扩展）。
- **导出**：`project_export` = 从（基线容器 ⊕ 重放到头的台账）物化 JAR 到 `derived/`，沿既有 `exportContainerToJar` 原子写路径（`workspace.cpp:4045-4047`）。

### 6.3 与 mjbase 边界

台账/overlay 的**引擎**（ledger 读写、重放、书签）进 mjbase（project 模块扩展 + workspace 消费），符合 migration-boundary"通用能力只出现一次"纪律（`migration-boundary.md:24-25`）；MCP 工具面与 CLI 留 mjt。现有 `WorkspaceBase` 公开方法签名不动（undo/redo/restoreSavepoint 语义不变，实现换底），上层零改动过渡。

### 6.4 台账压缩（compaction）（草案待评审）

台账只追加会使 jsonl 与 overlay section 无界膨胀，重放与 undo 随之变慢。压缩把基线推进到台账内的某个事务，重写基线字节并截断台账，对外语义不变。

触发分两类。(1) 手动：`edit_history(action=compact)` 与 SDK `compact_ledger()`，可带保留参数（保留最近 N 个书签、保留 undo 深度）；显式允许丢弃更早书签，结果中列明被丢弃者。(2) 自动：flush 与 savepoint 时机检查阈值（事务条数、台账字节数、距上次压缩的事务距离），超限才执行，不阻塞交互路径；阈值入工程配置，可整体关闭自动压缩。

自动压缩的不变量红线：保留点不早于最老存活书签，任何书签不因自动压缩失效；undo 窗口深度不因自动压缩缩短；导出物的 digest 可追溯性不变——基线推进以"新基线 digest + 被压缩区间记录"补链。

压缩步骤：选定保留点（默认当前头）→ 从"基线 ⊕ 重放到保留点"物化新基线 section（沿 writeSectionBytes 原子写）→ manifest 原子切换基线引用并截断台账至保留窗口 → 释放旧 overlay 与旧基线 section。先写后切换，崩溃任意一步旧基线与旧台账仍完整，由既有恢复扫描兜底。

---

## 7. 缓存与失效键（草案待评审）

缓存失效键**完全沿用 store.json 既有字段**（§2.2），不自造：

```
失效键 = depends[]（参与容器的 path+fingerprint 全集）
       + produced_by（引擎自述串，如 "mjt xref/3"）
       + section_version（消费者私有版本）
       + profile（影响内容的 options，如反编译参数、keep 规则集 hash）
```

- xref 缓存：`depends = [primary, libraries...]`；任一容器 digest 变化或编辑台账推进 → 下次读失效重算（或按 §6.2 ChangeSet 局部重建——mjbase ReferenceIndex 已有 `applyChangeSet` 局部重建面，`dev_plan.md:247-254`）。
- decompile 缓存：键 = 条目 digest（非容器 digest，条目级粒度）+ 反编译引擎 + options profile；条目未变即命中，编辑一个类只失效该类。
- 缓存 section 损坏 = Damaged（同名重写即恢复，`project_store.hpp:410-414`），缓存永不值得为它破坏红线。

---

## 8. MCP 工具收敛（74 → 11 粗粒度，同版本切换）（草案待评审）

实测 `contract.cpp:toolsList()` 注册 74 个工具（74 个 `result.push_back`，`contract.cpp:345-745`；任务书口径"约 61"以实测为准）。收敛原则：

- MCP 面只服务**快速问答**（单类/单方法/单引用的即席查询与单点编辑）；
- **批量与组合走 SDK 脚本**（`mjt --script`）；
- **不新增 `mjt_run_script` 类 MCP 工具**（脚本入口是 CLI，不是 MCP）；
- 同族工具合并为"一个粗工具 + kind/mode 选择器"；验证/应用合并为 `dry_run` 一面（现状 `patch_validate_*`/`patch_apply_*` 六对、`opt_preview`/`opt_apply`、`jar_fix_analyze`/`jar_fix_apply`、`rename_analyze`/`rename_apply`、`mapping_analyze`/`mapping_apply` 全部是同一调用的 dryRun 开关，见 `mutation.cpp:33-79,176-250,374-458`）。

### 8.1 收敛后的粗粒度工具（11 个，取代全部旧工具名）

| # | 工具 | 职责（一句话） |
|---|---|---|
| 1 | `project_open` | 打开/创建分析工程并挂载 primary 容器（JAR/class 文件；可带工程目录参数） |
| 2 | `project_close` | flush pending overlay 并关闭工程（释放 store.lock） |
| 3 | `project_info` | 工程总览：容器及角色/digest、revision、台账深度、缓存健康、可用反编译后端、store.lock 持有者 |
| 4 | `container_mount` | 挂载/卸载 library/runtime 容器（登记 project.json，返回 digest） |
| 5 | `class_query` | 精确查询一个类的详情：结构摘要/完整元数据/字节/Jasm/常量池/方法与字段详情/MANIFEST（kind 选择器） |
| 6 | `search` | 五族搜索合一：classes/members/strings/numbers/instructions + shadow 显示名双向解析（kind 选择器） |
| 7 | `xref_query` | 引用图查询：指向/发出引用、调用点、层次关系、override 组件（direction/kind 选择器） |
| 8 | `decompile` | 反编译一个类或定位一个方法（后端可选；异步返回 job_id） |
| 9 | `edit_apply` | 一切编辑唯一入口：结构化 ClassMutation/Jasm/diff/字节替换/rename/mapping/pipeline/jar 修复，`dry_run` 预览 + `expected_revision` 乐观控制 + label |
| 10 | `edit_history` | 事务台账操作与查询：undo/redo/restore/savepoint/revert_class/modified_classes/台账列览 |
| 11 | `project_export` | 把当前编辑态物化为派生容器写 `derived/`（原子、不碰原容器） |

### 8.2 74 个现有工具逐个去向

去向类别：**保留**（收敛为粗工具本体）/ **折叠**（成为某粗工具的 mode/响应字段）/ **SDK**（批量或组合场景，MCP 面删除）/ **删除**（无消费价值）。按评审决定，本次收敛**同版本切换、不设别名期**：旧工具名在同一发布内删除，仓内消费方（skills 包、测试、文档）随同一变更集迁移，避免双轨语义漂移。

**会话与作业层（9 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `workspace_open`（contract.cpp:390） | 保留 | → `project_open`（新增工程目录/角色参数；不带工程参数时自动建临时工程，兼容旧行为） |
| `workspace_close`（:399） | 保留 | → `project_close` |
| `workspace_info`（:400） | 折叠 | → `project_info` |
| `workspace_add_support`（:404） | 保留 | → `container_mount` |
| `workspace_list_support`（:406） | 折叠 | → `project_info` 容器列表 |
| `workspace_remove_support`（:408） | 折叠 | → `container_mount(action=detach)` |
| `job_get`（:379） | 折叠 | → `decompile` 作业面：`job_id` + `action=poll`；异步反编译轮询不需要独立工具 |
| `job_cancel`（:381） | 折叠 | → `decompile`，`action=cancel` |
| `job_release`（:383） | 折叠 | → `decompile`，`action=release` |

**搜索与资源（8 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `search_classes`（:431） | 保留 | → `search(kind=classes)`（同分支实现 `query.cpp:131-132`） |
| `search_members`（:433） | 保留 | → `search(kind=members)` |
| `search_strings`（:442） | 保留 | → `search(kind=strings)` |
| `search_numbers`（:447） | 保留 | → `search(kind=numbers)` |
| `search_instructions`（:458） | 保留 | → `search(kind=instructions)` |
| `shadow_to_raw`（:513） | 折叠 | → `search(kind=shadow)`（双向同分支 `query.cpp:272,317`） |
| `raw_to_shadow`（:517） | 折叠 | 同上 |
| `workspace_resources`（:410） | 折叠 | → `search(kind=resources)`（资源枚举本就是分页列表） |

**资源读取（3 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `resource_read`（:412） | 折叠 | → `class_query(kind=resource_bytes)`（URI 寻址，含分页） |
| `resource_manifest`（:417） | 折叠 | → `class_query(kind=manifest)` |
| `resource_strings`（:418） | 折叠 | → `search(kind=strings)` 已覆盖非类资源可打印串（工具描述即此语义，`contract.cpp:444-445`） |

**类与方法查询（8 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `class_list`（:464） | 折叠 | → `class_query(kind=list)`（或 search 的 resources/classes 模式） |
| `class_get`（:465） | 保留 | → `class_query(kind=class)` |
| `class_bytes`（:469） | 折叠 | → `class_query(kind=bytes)` |
| `class_fidelity_check`（:478） | 删除 | libjbc 包试运营期保真探针（`contract.cpp:477-483`），验收职能已由管线 golden 门承担；SDK 保留自检函数 |
| `class_jasm`（:484） | 折叠 | → `class_query(kind=jasm)` |
| `method_get`（:488） | 折叠 | → `class_query(kind=method)`（同分支 `query.cpp:223`） |
| `method_bytecode`（:490） | 折叠 | → `class_query(kind=bytecode)` |
| `method_jasm`（:492） | 折叠 | → `class_query(kind=method_jasm)` |

**反编译（4 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `class_source`（:540） | 保留 | → `decompile` |
| `method_source`（:544） | 折叠 | → `decompile(selector=method)`（同分支 `decompiler.cpp:66-67`） |
| `decompiler_decompile`（:548） | 删除 | `class_source` 的历史别名（同一分支，`decompiler.cpp:66`） |
| `decompiler_compare`（:552） | SDK | 双后端对比是组合/批量场景 |

**反编译器配置与杂项（3 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `decompiler_list`（:347） | 折叠 | → `project_info` 返回后端可用性 |
| `decompiler_settings_get`（:350） | SDK | 会话级默认值改工程级配置（project.json 扩展或 store profile）；MCP 面删 |
| `decompiler_settings_set`（:353） | SDK | 同上 |

**pass 与表达式（3 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `pass_describe`（:368） | SDK | pass/pipeline 发现面；`edit_apply` 错误消息自带已注册名单兜底 |
| `analysis_evaluate`（:371） | SDK | 实验性表达式求值（`session.cpp:25-111`），非问答面 |
| `analysis_workflow`（:385） | SDK | 多段组合（mapping+pipeline+xref+decompile+export）正是 SDK 脚本的一句话；迁移期暂留薄别名兜底存量用法 |

**jar 对比与修复（3 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `jar_diff`（:567） | SDK | 全容器对比是批量报告 |
| `jar_fix_analyze`（:575） | SDK | 修复扫描+应用是批量维护（同分支 `mutation.cpp:70-79`） |
| `jar_fix_apply`（:578） | SDK | 同上 |

**引用图（7 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `xref_method`（:581） | 保留 | → `xref_query(target=method, direction=to)` |
| `method_callers`（:583） | 折叠 | 与 xref_method 同族（同分支 `mutation.cpp:81-85`） |
| `xref_field`（:585） | 折叠 | → `xref_query(target=field, direction=to)` |
| `xref_class`（:587） | 折叠 | → `xref_query(target=class, direction=to)` |
| `method_callees`（:591） | 折叠 | → `xref_query(direction=from, resolved)` |
| `class_relations`（:596） | 折叠 | → `xref_query(kind=relations)` |
| `method_overrides`（:600） | 折叠 | → `xref_query(kind=overrides)` |

**编辑（12 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `patch_validate_jasm`（:608） | 折叠 | → `edit_apply(kind=jasm, dry_run=true)` |
| `patch_apply_jasm`（:610） | 保留 | → `edit_apply(kind=jasm)` |
| `patch_validate_jasm_diff`（:620） | 折叠 | → `edit_apply(kind=jasm_diff, dry_run=true)` |
| `patch_apply_jasm_diff`（:624） | 折叠 | → `edit_apply(kind=jasm_diff)` |
| `method_validate_jasm`（:638） | 折叠 | → `edit_apply(kind=method_jasm, dry_run=true)` |
| `method_apply_jasm`（:642） | 折叠 | → `edit_apply(kind=method_jasm)` |
| `method_validate_jasm_diff`（:656） | 折叠 | → `edit_apply(kind=method_jasm_diff, dry_run=true)` |
| `method_apply_jasm_diff`（:660） | 折叠 | → `edit_apply(kind=method_jasm_diff)` |
| `patch_replace_class_bytes`（:670） | 折叠 | → `edit_apply(kind=class_bytes)` |
| `patch_apply`（:703） | 折叠 | → `edit_apply(kind=structured)`（15 种 ClassMutation 原样承载） |
| `pass_run`（:557） | 折叠 | → `edit_apply(kind=pipeline)`（同分支 `mutation.cpp:33`） |
| `opt_preview`（:561）/`opt_apply`（:564） | 折叠 | → `edit_apply(kind=pipeline, dry_run)` 同面（同分支） |

**重命名与映射（4 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `rename_analyze`（:715） | 折叠 | → `edit_apply(kind=rename, dry_run=true)` |
| `rename_apply`（:717） | 折叠 | → `edit_apply(kind=rename)` |
| `mapping_analyze`（:725） | 折叠 | → `edit_apply(kind=mapping, dry_run=true)` |
| `mapping_apply`（:728） | 折叠 | → `edit_apply(kind=mapping)` |

**shadow 管理（2 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `shadow_generate`（:530） | SDK | 批量生成写 sidecar（`mutation.cpp:490-534`） |
| `shadow_prune`（:535） | SDK | 批量清理 |

**事务（6 个）**

| 现工具 | 去向 | 说明 |
|---|---|---|
| `workspace_savepoint`（:731） | 折叠 | → `edit_history(action=savepoint)`（实现降为台账书签，§6.2） |
| `workspace_restore`（:733） | 折叠 | → `edit_history(action=restore)`（同分支 `mutation.cpp:466-479`） |
| `workspace_revert_class`（:735） | 折叠 | → `edit_history(action=revert_class)` |
| `workspace_undo`（:740） | 保留 | → `edit_history(action=undo)` |
| `workspace_redo`（:741） | 折叠 | → `edit_history(action=redo)` |
| `workspace_export`（:742） | 保留 | → `project_export` |

统计：保留/收敛为本体 11；折叠 50；SDK 化 12；删除 2（`decompiler_decompile`、`class_fidelity_check`；另有分析口径差一约数以 contract.cpp 实测为准）。无别名期，旧名同版本删除。

---

## 9. SDK 优先策略：粗工具与 SDK 的边界（草案待评审）

**同一进程内引擎层，两个入口面**：

```
            ┌────────────────────────────────────────────┐
MCP (libmcp)│  粗工具 11 个：问答 + 单点编辑 + 工程生命周期    │
            └───────────────┬────────────────────────────┘
                            │  同一进程内引擎层
        ┌───────────────────┴──────────────────────┐
        │ 工程层：挂载/台账/overlay/缓存（mjbase project + workspace 扩展） │
        ├──────────────────────────────────────────┤
        │ 分析层：索引/xref/decompile/pass 管线（mjbase 现有模块）        │
        └───────────────────┬──────────────────────┘
            ┌───────────────┴────────────────────────────┐
mjt --script│  Python SDK（FacadeSession 全粒度面，IDAPython 形态）│
            └────────────────────────────────────────────┘
```

- **边界划分规则**：能表达为"单目标 + 单意图"的进 MCP 面（查一个类、改一个方法、undo 一次）；需要循环、过滤、批量、多段编排的进 SDK（`mjt_util.py` 已有 `classes_where/methods_where` 批量谓词面，`mjt-py/src/mjt_util.py:64-79`）。
- **SDK 现状与对齐**：SDK 面是 `FacadeSession`（容器 + 树缓存 + rename token + shadow 表，`mjt-py/src/pysdk_facade.hpp:71-122`）。注意 SDK 树载体目前是旧栈 `jbytecode::tree::ClassNode`（`:51-57`），与 MCP 侧 libjbc 新栈并存——**工程层接口只定义在容器字节面**（entry URI + digest + ledger），两栈共享工程层而互不穿透，这是双栈收敛前的稳定缝。
- **不做** `mjt_run_script` MCP 工具：脚本走 CLI 进程（天然隔离、天然持有 store.lock 排队语义），MCP 会话内嵌脚本执行会把"批量"又引回 MCP 面。
- 工具收敛砍掉的批量能力（shadow_generate/jar_diff/analysis_workflow 等）在 SDK 侧补齐等价函数后 MCP 面才可删（双轨期见 §11）。

---

## 10. 分阶段落地路线（每阶段独立合并、可回归验证）

| 阶段 | 内容 | 交付与回归门 | 依赖 |
|---|---|---|---|
| **P0 对齐**（mjbase） | project.json v2 草案定稿（role 扩展 primary/library/runtime + 挂载树 parent 字段 + 容器 digest sha256 口径）；#1096 侧裁决 v1 读者遇 v2 的行为 | mjbase 单测 + 文档；mjt 零改动 | 无 |
| **P1 工程接线**（mjt） | `ProjectSession`：MCP 会话持有工程 bundle；`workspace_open` 挂 `--project` 参数（缺省内存态兼容旧轨）；容器登记/指纹落 project.json；store.lock 接入 | 旧 74 工具行为零变化（回归 = 现有 mjt_mcp_unittest 全绿）+ 新开合测试 | P0 |
| **P2 台账与 overlay**（mjbase+ mjt） | §6 重建模：ledger section 读写 + 重放引擎（mjbase project 扩展）；WorkspaceBase undo/redo/savepoint 换底（签名不变）；flush 挂点（close/export 前）；台账压缩引擎（§6.4，手动 + 自动阈值） | mjbase 事务单测（含崩溃注入：staged 残留/Damaged/孤儿档）+ 压缩不变量测试（书签存活/undo 深度/digest 补链）+ mjt 事务回归 | P1 |
| **P3 缓存落盘**（mjt） | xref 与 decompile 缓存写 sections/（§7 失效键）；条目级 decompile 命中 | 二次打开同工程命中率断言 + 容器变更失效断言 | P2（键定义可先冻结并行开发） |
| **P4 工具收敛**（mjt） | 11 个粗工具落地（新 schema + 分发）；SDK 补齐 12 个 SDK 化能力的等价函数；**同版本删除全部旧工具名**并迁移仓内消费方（skills 包、测试、文档），不设别名期 | 新工具单测 + 旧名删除后的全量回归；SDK selftest 扩面；changelog 声明破坏性变更 | P1（不依赖 P2/P3，可提前） |
| **P5 多容器**（mjt+ mjbase） | `container_mount` 开放 library/runtime 多容器 + 复合寻址扩展位（§5.3）；v2 role 字段启用 | 工具数 = 11；多容器 xref 归并测试 | P2 P3 P4 |

---

## 11. 风险清单

1. **磁盘格式演进**：store.json 条目严格拒载（`project_store.hpp:159-162`）意味着任何 section 元数据扩展都要升版本；缓冲垫是 opaque 收容（`:250-258`）与"顶层只增"规则。缓解：P0 先冻结 v2 全字段再动代码；section 内容（jsonl 行级）自描述 version 字段，行级演进不触清单版本。
2. **mjbase 上游边界**：台账/overlay 引擎、role 扩展、多容器合并视图进 mjbase（公开仓，需过 migration-boundary 红线复查——无家族名、消费方驱动）；MCP 面、CLI、SDK facade 留 mjt。风险是 mjt 侧为求快把工程语义写进 mjt shared，造成后来双实现。缓解：P2 的 mjbase 卡先落，mjt 接线卡后落（migration-boundary §三 的切片纪律，`migration-boundary.md:34-48`）。
3. **迁移期双轨**：按评审决定**不设双轨版本**——旧工具名与新粗工具不在同一发布内并存，`workspace_open` 无工程参数时落在临时工程（内存态旧栈实现随收敛删除）。风险收窄为单次发布内的行为切换：savepoint 跨 flush 存活等语义变化在 changelog 逐条声明；仓内 skills 包与测试随同一变更集迁移，评审期即验证新语义。
4. **digest 口径切换**：fnv1a64（64 位，`workspace.cpp:887`）→ sha256。风险：外部已持久化的旧指纹（用户脚本、sidecar 引用）失配。缓解：双前缀兼容读 + 命中旧前缀时提示重算；不静默降级。
5. **锁与并发体验**：单写者锁使"MCP 服务器 + SDK 脚本同时开一个工程"变成第二者报错。这是设计意图（IDA 同款），但要防误读为缺陷；PID 复用是已知残余风险，`stealLock` 人工兜底（`project_store.hpp:565-566`）。
6. **Windows 换名重试**：高频 flush 下 3×50ms 重试耗尽 = Io（`project_store.hpp:298-300`）；反编译缓存写入频繁时考虑合并 flush（批量 section 提交一次清单事务）。
7. **SDK 双栈**：SDK 树载体仍在旧栈（`pysdk_facade.hpp:51-57`），工程层若不钉死在容器字节面，会被拖入双栈接口分裂。缓解：§9 的"字节面缝"写入 SDK 契约。
8. **工具收敛的生态破坏**：同版本删除旧工具名是破坏性变更。外部若有依赖旧工具名的提示词/脚本，需随版本升级一次性迁移；`decompiler_decompile`/`class_fidelity_check` 直接删除需在 changelog 声明。
9. **AGENTS.md 时滞**：mjt AGENTS.md 的 mbase 分叉治理节已过时（仓内 `mbase/` 只剩 `test_resource/`）；RFC 落地波次应同步修订 AGENTS.md，防止后来者按旧治理规则行事。

---

## 附录 A：现状证据索引（文件：行号速查）

| 断言 | 证据 |
|---|---|
| mjt 源码级依赖 mjbase | `mjt/xmake.lua:175-181` |
| JarWorkspace 无扩展语义直继承 | `mjt/src/shared/jar_workspace.hpp:12-16` |
| workspace 状态全内存（original/working/support/undo/redo/savepoint） | `mjbase .../workspace/workspace.hpp:347-352` |
| savepoint = 整容器克隆 | `workspace.cpp:3942` |
| commit 压 before 快照入 undo 栈 | `workspace.cpp:2723-2724` |
| MutationRequest{expectedRevision,dryRun,label} | `workspace_types.hpp:108-113` |
| 容器指纹 fnv1a64 | `workspace.cpp:887-914` |
| export 原子写新文件 | `workspace.cpp:4037-4049` |
| 工程实体 schema v1（inputs/role/annotations/sidecar） | `mjbase .../project/analysis_project.hpp:48-102` |
| bundle 布局与 section 字段（缓存失效键） | `mjbase .../project/project_store.hpp:47-62,142-183` |
| 单写者锁/PID 探测/stealLock | `project_store.hpp:570-666` |
| 恢复扫描与孤儿两档 | `project_store.hpp:421-497` |
| 容器 first-wins + 重复记录 | `mjbase .../model/resource_container.hpp:9-11,92-101` |
| MCP 74 工具注册 | `mjt/mcp/src/tool/contract.cpp:345-745` |
| 会话工具持锁分发（open/close 内存态） | `mjt/mcp/src/tool/session.cpp:131-249` |
| workspace 上限 16 / job 上限 64 | `mjt/mcp/src/tool/limits.hpp:22,27` |
| 同族工具同分支（search/xref/patch/txn） | `query.cpp:131-144,223,272`；`mutation.cpp:33-85,176-250,374-479`；`decompiler.cpp:66-67` |
| `mjt --script` 嵌入解释器 | `mjt-py/src/pysdk_embed.hpp:3`；`pysdk_embed.cpp:129-190` |
| SDK FacadeSession 旧栈树载体 | `mjt-py/src/pysdk_facade.hpp:71-122` |
| project/model 已落 mjbase 批次 | `mjbase/mjbase/doc_ai/module-map.md:35,33`；`mjbase/doc_ai/migration-boundary.md:60,63` |
| multi-release 挂账（P0-6） | `mjbase/mjbase/dev_plan.md:25-26` |
