# APK/DEX 输入支持（原型）——实施计划

本文记录 PR #100 的只读原型设计和后续接口。原型基于 PR #99 的
ContainerIoRegistry 格式注册表。

## 1. 背景与目标

mjt 的主制品打开面（`WorkspaceBase::open`）按扩展名路由 ContainerIoRegistry 格式
注册表（jar / classfile）。本计划追加 apk / dex 两个格式行，查询面尽可能走分析
数据库（DB-first）：search 四档、xref references、层次闭包在 dex 输入上直接可用。

**基座事实**（调研已核实）：

- libdex（独立仓 `luiox/libdex`，Under Construction）：DEX 单文件读写全能力——
  035–041 全版本、全结构建模（annotations/debug info/call_site/method_handle）、
  insn 层 `decodeInsns`（`Insn.index/indexKind/literal` 已按 IndexKind 归一）、
  smali 反/汇编（`disassembleClass` 单类文本）、Result 值语义与 mjt 仓规同族。
  **APK 容器/多 dex 是其明文 non-goal**，归上层。
- libjzip（已在依赖闭包）：`ZipIO::readJvm → getNameFilteredCentralDirectories →
  decompressLocalFile` 提取 classes*.dex；`JvmZipReader` 的 EOCD 链容忍签名块前缀
  数据。**apk 签名块语义全链路无人负责**——写回 apk 若重排条目会使 v2+ 签名失效，
  本原型不提供 apk 导出。
- mjbase analysis_db（0.0.31，schema v13）：`inputs/entries` 容器中立；类事实/refs/
  常量面全部按 JVM 内部名/描述符语法键控——**dex 描述符语法与 JVM 同形，零 schema
  改动直接灌库**；层次闭包递归 CTE 对 dex 同样成立。
- mjbase 有明文裁决「容器形态归下游自理」——apk/dex 容器落点在 mjt，基座零改动。

## 2. MVP 边界

做（只读分析原型）：

- `project_open` / CLI 接受 `.apk` / `.dex` 输入（扩展名路由 + importer 内 magic
  前置定性：zip 头 / `dex\n0XX\0`）。
- DB-first 查询面全量覆盖 dex：search classes/members/strings/numbers/resources、
  xref references、层次闭包（classRelations 走 dex DOM，超集语义不变）。
- dex 类的文本视图：smali（libsmali `disassembleClass`），`class_query kind=jasm`
  对 dex 类返回 `format:"smali"` 文本。
- 引擎残余面按 dex 事实实现（WorkspaceBase 仍保留的成员方法前置分支）：
  `constantPool(→dex ID 表)/referenceScan/xrefsTo*/calleesOfMethod/classRelations/
  overridesOfMethod/searchInstructions/methodInstructions/fieldInfo`；类事实面
  （listClasses/classInfo 已退役为 analysis_ops 自由函数）在调用侧分支——ingest
  桥与 MCP class_list/class_get 走 `dexDomain()`。
- 常量事实面：const-string → cp_strings；const 字面量 + 静态初值 → number_literals。

原型范围之外：

- dex 编辑（smali apply / edit_apply 对 dex 会话明确报错）、decompile 对 dex 类报错。
- apk 导出（`project_export` 对 dex 会话报错——避免产出破坏 v2+ 签名的 zip）。
- AXML 资源串（AndroidManifest.xml 二进制解析）、pysdk apk/dex 入口、
  jar+apk 混合挂载（`container_mount` 对 dex 会话报错）。

### 2.1 Follow-up 接口

- DEX 的 `invoke-custom` 和 `invoke-custom/range` 在 MCP 和 Python SDK 的出向
  方法查询中返回 `dynamic`（调用点的名称与描述符）和 `bootstrap`（引导方法）。
  成员反查和 CLI `xref` 可找到引导方法及 `method_handle` 引用。动态调用的最终目标
  由运行时决定。

- `edit_apply kind=smali` 接收类内部名 `class`、完整 Smali `source` 和新的
  `.dex` 输出 `path`。`dry_run=true` 只汇编并返回字节数。APK 中的类会输出
  其所在的 DEX 单元。用 `project_open` 打开产物可继续查询。
- APK/DEX 项目的 MCP `decompile` 默认选择 JADX，支持同步和异步调用。传入 JADX
  CLI 的可执行 fat JAR 路径 `decompiler_jar`，或设置 `MJT_JADX_JAR`。JADX
  用于生成 Dalvik 类的 Java 源码，构建 mjt 时无需安装。反编译使用目标类所在的
  DEX 单元。
- APK 的 `AndroidManifest.xml` 二进制字符串池按 UTF-8 或 UTF-16 解码，结果可用
  `search kind=strings` 查询。
- Python SDK 提供 `open_apk(path)` 和 `open_dex(path)`。可查询类、成员、
  Smali 文本及分析库。
- `container_mount` 接受 JAR、APK 和 DEX。按 `library` 或 `runtime` 角色可
  查询挂载内容，传入引用查询的 `role` 可筛选来源。引擎层次图包含挂载类，
  DEX 的超类链保留首个未登记的超类。
- libdex 定版后，再将锁定版本追加到 luiox-repo 并迁移包声明。

## 3. 架构

### 3.1 依赖装配

libdex 走**项目内联包定义**（`xmake/packages/libdex.lua`，libjbc 先例：git 直连 +
commit 锁；deps 对齐 `LUIOX_LIBCA_VERSION` 超集 + libjstr 0.0.4 + zlib）。libdex
定版后应进 luiox-repo 追加词条（只追加不修改），mjt 届时改词条消费。

### 3.2 DexDomain（新模块，`mjt/src/workspace/dex_domain.hpp/.cpp`）

`mjt::workspace::DexDomain`：dex 域事实聚合器。持有 libdex `DexFile` DOM（每
dex 文件一个）+ 类索引（internal_name → {dexIdx, classDefIdx}）+ 引用索引。

- 装载：`loadApk(path)`（libjzip 提取全部 classes*.dex → 逐个 readDex）/ 
  `loadDex(path)`（裸 dex）。两个失败模式：坏 zip / 坏 dex → Workspace 错误。
- 类事实：`listClasses()`（ClassInfo：internal_name/super/interfaces/access/
  version(=dex 版本)/resourceUri=dex 条目 URI）；`classInfo(name)`；
  `methodInfo/fieldInfo`。
- 引用扫描：`buildReferenceScan()` → ReferenceScan 三桶（语义逐条对齐
  reference_scan.cpp 的 JVM 扫描）：
  - invoke-{virtual,super,direct,static,interface} → Method ref + owner Type ref；
  - iget/iput/sget/sput 家族 → Field ref + owner Type ref；
  - new-instance/check-cast/instance-of/const-class → Type ref；
    new-array/filled-new-array → 数组描述符提元素类型（multianewarray 同款）；
  - const-string 形态合法类名 → 反射候选 Type ref（`looksLikeClassName` 同款）；
  - class 头 super/interfaces → class 级 Type ref（sourceMethod 空、insnIndex=-1）；
  - insnIndex = 指令序号（`decodeInsns` 程序序，含 payload 不展开——payload 指令
    按宽度跳过计序，与 jbc CodeDom 的「标签不是指令」口径对齐为「payload 是一条」）。
- 常量面：`constantFaces(class)` → cpStrings（dex string_ids，facet 预分同
  constantUtf8Facet）/ cpNumbers（静态初值 encoded_array）/ 逐方法 number 切片
  （const/const-wide/const-high16 字面量，MethodNumberSlice 同形）。
- 层次：`classRelations(name)`（super 链 + 接口 + 直接/全部子类 + 实现者，dex DOM
  反向索引）；`overridesOfMethod`（super 链同名同描述符）。
- 文本：`smaliText(name)`（libsmali `disassembleClass`）；`searchInstructions`
  （smali 文本扫描，SearchOptions 过滤）。
- 常量池视图：`constantPool(name, resolve)` → dex 五张 ID 表投影 ConstantPoolEntryInfo
  （tag = StringId/TypeId/ProtoId/FieldId/MethodId）。

### 3.3 格式接入（container_io 注册表 + WorkspaceBase 分派）

- `ContainerFormat` 增 `Apk` / `Dex` 枚举值；注册表默认行追加
  `("apk", apkImport, 无导出行)` / `("dex", dexImport, 无导出行)`。
- `apkImport`：zip magic 前置定性 → 容器沿用 jar 导入链
  （`ResourceHelper::importFromJar`，签名块前缀数据天然容忍；dex 条目落
  UnknownResource）+ 独立 reader 建 DexDomain。
  `dexImport`：`dex\n` magic 前置定性 → 容器单条目（uri = 文件路径）+ DexDomain。
  两个 importer 产物经 `ImportedArtifact.dexDomain` 槽上抛，`open` 尾部采纳进
  `dexDomain_`（jvm 格式恒空）。
- WorkspaceBase 查询成员的 DEX 分支：
  fieldInfo/constantPool/methodInstructions/searchInstructions/xrefsTo*/
  calleesOfMethod/overridesOfMethod/referenceScan。classRelations 使用 JVM 与 DEX
  共用的层次图。
- 类事实面已退役为 analysis_ops 自由函数（listClasses/classInfo 只认 jbc 条目），
  调用侧分支：ingest 桥与 MCP class_list/class_get 走 `dexDomain()->listClasses()/
  classInfo()`。
- APK/DEX 的 `exportArtifact` 仍无注册表导出行。Smali 写入使用专用的 DEX 输出
  接口（见 §2.1）。class bytes、method_jasm 等操作要求 JVM 类。

### 3.4 ingest 桥 dex 分支（`analysis_ingest.cpp`）

现有桥经自由函数走容器面（listResources/readResource 容器中立，dex 态天然工作）；
三处分支：

- 条目 kind：dex 条目 type="dex" → entryKind 映射 "dex"；资源串抽取跳过 dex 条目
  （二进制格式，可打印 run 抽取产生噪声）。
- 类事实：`listClasses` 换 `dexDomain()->listClasses()`（自由函数只认 jbc 条目）。
- 常量面循环：jbc 会话段替换为 DexDomain 常量面（`isDexBacked()` 判别，
  `dexDomain()` 访问器），DbClassConstantFaces 同构组装，replaceConstantFaces/
  replaceResources 等批 API 零改动。

### 3.5 MCP/CLI 面

- `project_info`：`artifact_kind` 报 "apk"/"dex"（ContainerFormat 投影），附
  `dex_units`（dex 条目 uri 清单）与 `dex_version`。
- `class_query kind=list/class`：dex 会话走 `dexDomain()`；`kind=jasm`：dex 类走
  `smaliText`，响应附 `format:"smali"`。
- MCP 的 DEX 反编译、Smali 写入和混合挂载见 §2.1。
- CLI：`open` 扩展名路由天然接受 apk/dex，info/classes/xref/subs 直接可用；
  strings/disasm/decompile/jar fix 对 dex 输入给出明确错误或空结果
  （jbc 解码零命中，语义为「无 class」）。

## 4. 数据映射（dex → analysis.db）

| analysis.db 面 | dex 来源 |
|---|---|
| inputs | apk 文件（path/sha256/size）；裸 dex 同 |
| entries | zip 条目（apk）/ dex 文件自身；dex 条目 kind="dex" |
| classes | class_defs：internal_name（描述符去 `L;`）、super_name、access（ACC 位语义同 JVM）、version=dex 版本 35..41、entry_id=dex 条目 |
| interfaces | type_list 声明序 |
| members | class_data：static/instance fields + direct/virtual methods（name/type/proto 描述符、flags、signature 无=空串） |
| refs | insn 扫描（§3.2 语义表）；文本分支直接灌（未登记目标走 target_class/name/desc 文本列） |
| cp_strings | string_ids（facet：Name/Descriptor/ClassRef 预分同 JVM 口径） |
| number_literals | 静态初值（member_id=0 哨兵）+ const 字面量（逐方法切片） |
| resources | APK 的 AXML 字符串池及其他资源的可打印串（DEX 条目跳过） |

## 5. 测试策略

- fixture：smali 文本经 libdex `smalict`（或仓内 SmaliAssembler）汇编出 dex 落
  `test_resource/dex/`（hello_refs.dex：多类 + invoke/field/type/const-string/
  const 字面量 + super/interfaces）；apk fixture 测试内 `test::writeRawZip`
  包 classes.dex + 资源条目。
- workspace 层（`mjt/test/workspace/dex_workspace_test.cpp`）：open 分派、
  listClasses/classInfo、referenceScan 三桶、层次闭包、常量面、smali 视图、
  export/addSupporting 守卫。
- ingest 层：dex 态整库导入 → db.findClass/superChain/referencesToMethod/
  searchStringsPaged 断言。
- MCP 层（`mjt/mcp/test/mcp_dex_test.cpp`）：project_open(apk) → search 四档 →
  xref references → class_query jasm(format=smali) → relations → decompile/edit_apply
  报错形态（对齐 `mcp_workspace_test.cpp` 的 McpSessionHarness 模式）。
- 构建门：逐 target `rm <exe> && xmake -r <target>` 后跑 gtest。

## 6. 提交切分

1. `feat(workspace): libdex 内联包接入`（xmake 装配）
2. `feat(workspace): dex 域——apk/dex 输入装载与引擎查询分支`
3. `feat(workspace): dex 事实灌库——ingest 桥 dex 分支`
4. `feat(mcp): dex 会话查询面——smali 视图与守卫`
5. `test: apk/dex 输入全链测试`
6. `docs: apk/dex 支持计划文档`
