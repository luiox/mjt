# APK/DEX 分析设计

mjt 通过格式注册表打开 APK 和裸 DEX，将 Dalvik 类、成员、常量和引用转换为工作区查询使用的事实。MCP、CLI 和 Python SDK 共用 DEX 事实层，分析数据库负责可持久化的查询数据，Smali 和 JADX 提供文本出口。

## 格式接入与依赖

`ContainerIoRegistry` 为 `ContainerFormat::Apk` 和 `ContainerFormat::Dex` 提供导入项。APK 导入先检查 ZIP 文件头，再通过 libjzip 读取资源和根目录的 `*.dex` 条目。裸 DEX 导入检查 `dex\n` 文件头，建立单条目资源容器。导入产物的 `ImportedArtifact::dexDomain` 随工作区持有，资源容器保留输入条目的原始字节。

libdex 负责单个 DEX 文件的结构解析、指令解码及 Smali 反汇编和汇编，直接消费方是 `DexDomain`。APK 容器读取与多 DEX 聚合由 mjt 负责。libdex 经 luiox-repo 的固定版本包消费，依赖版本与项目根的 libca、libjstr 和 zlib 配置对齐。

APK 和 DEX 的格式注册项只提供导入。APK 资源导出涉及条目布局和签名块，工作区的导出能力由格式注册表中的导出项决定。Smali 的派生 DEX 输出使用独立接口。

## DEX 事实与身份

`DexDomain` 持有每个 DEX 单元的 `DexFile`、单元 URI 和类索引。类索引用内部名定位 DEX 单元及 `class_def`，类枚举、成员查询、引用扫描和指令搜索使用同一份可见类集合。输入中出现同名类时，装载顺序中的首个定义胜出。

类描述符 `Lcom/example/Subject;` 在查询面表示为 `com/example/Subject`，成员描述符保留 DEX 中的形式，例如 `(I)Ljava/lang/String;`。方法和字段通过所属类、名称和描述符共同定位。一个 DEX 单元的 URI 可以包含多个类，类选择器通常使用内部名；单类 DEX 也可用其 URI 定位。

类事实包含父类、接口、访问标记、成员清单和所属 DEX 版本。常量事实包括类引用的字符串、静态字段初值及方法中的数值字面量。`constantPool` 将 DEX 的 String、Type、Proto、Field 和 Method ID 表投影为常量表视图。

## 引用与层次查询

引用扫描记录来源类、来源方法及其描述符、方法体内的指令序号和引用目标。类头的父类及接口引用使用指令序号 `-1`，来源方法为空。方法体中的指令序号按解码顺序计数，payload 占一条。

| 编码事实 | 查询表示 |
| --- | --- |
| 普通方法调用 | 方法引用及目标所属类的类型引用 |
| 字段访问 | 字段引用及目标所属类的类型引用 |
| 对象类型指令 | 类型引用，对象数组提取元素类型 |
| 形态合法的类名字符串 | 反射类型引用候选 |
| `invoke-custom`、`invoke-custom/range` | `dynamic` 调用点及 `bootstrap` 引导方法 |
| 方法或字段句柄 | `method_handle` 引用 |

动态调用点的目标文本由编码名称和方法描述符组成，例如 `apply(I)I`。引导方法和句柄引用保留完整的成员身份。MCP、Python SDK 和 CLI 的出向引用可以列出动态调用点，成员反查可以定位引导方法及方法句柄。动态调用的最终目标由运行时决定。

分析数据库的引用表保存直接方法、字段和类型引用。包含调用点或方法句柄表的 DEX 使用 `DexDomain` 扫描返回引用种类，保留引导方法、句柄和动态调用点的语义。

工作区的类层次图聚合 JVM 与 DEX 类，支持父类链、接口、子类、实现者和覆盖关系查询。已登记类之外的父类和接口以合成节点连接。解析后的虚调用查询基于类层次展开候选实现。

## 分析数据库映射

`analysis_ingest` 通过工作区资源容器登记输入和条目，通过 `DexDomain` 获取类、成员、引用和常量。成员身份使用内部名和描述符，与 JVM 输入共享查询模型。

| 数据面 | DEX 或 APK 来源 |
| --- | --- |
| `inputs` | 输入路径、容器内容锚及大小 |
| `entries` | APK 中的条目或裸 DEX 文件，DEX 条目的 `kind` 为 `dex` |
| `classes` | 可见的 `class_def`，包括内部名、父类、访问标记、版本及所属条目 |
| `interfaces` | 类声明的接口列表 |
| `members` | 字段和方法的名称、描述符及访问标记 |
| `refs` | 直接方法引用、直接字段引用及类型引用 |
| `cp_strings` | 类引用的字符串及成员元数据 |
| `number_literals` | 静态字段初值和方法中的数值字面量 |
| `resources` | APK 的 AXML 字符串池及其他资源的可打印字符串 |

DEX 条目作为字节码容器参与登记，资源字符串抽取跳过其二进制载荷。`AndroidManifest.xml` 的二进制字符串池按 UTF-8 或 UTF-16 解码，结果可通过字符串搜索查询。

## MCP、CLI 和 Python SDK

MCP 的 `project_info` 返回 `artifact_kind`、已装载的 `dex_units` 和首个 DEX 单元的 `dex_version`。类事实中的版本来自该类所属的单元。`class_query kind=jasm` 对 DEX 类返回 Smali 文本，并附带 `format:"smali"`。类、成员、字符串、数值、资源、交叉引用和层次查询使用工作区的 DEX 查询路径。

CLI 的 `info`、`classes`、`xref` 和 `subs` 接受 APK/DEX。`disasm` 对 DEX 类输出 Smali。`strings` 在整输入模式下遍历每个 DEX 单元的字符串表并累加出现次数，指定 `--class` 时查询该类引用的字符串。

Python SDK 通过 `open_apk(path)` 和 `open_dex(path)` 建立会话，提供类、成员、Smali 文本及分析数据库查询。

## Smali 输出与反编译

`edit_apply kind=smali` 接收类内部名 `class`、完整 Smali 文本 `source` 和新的 `.dex` 输出路径 `path`。替换类保持原有类描述符，汇编在 DEX 副本上完成，输出包含目标类所在的整个 DEX 单元。`dry_run=true` 返回汇编后的字节数，用 `project_open` 打开派生文件可以继续查询。

APK/DEX 的 MCP `decompile` 默认使用 JADX，支持同步和异步调用。`decompiler_jar` 指定 JADX CLI 的可执行 fat JAR，也可通过 `MJT_JADX_JAR` 配置。反编译输入是目标类所属的 DEX 单元，JADX 在运行时调用。

## 挂载与项目恢复

`container_mount` 接受 JAR、APK 和 DEX，角色包括 `support`、`library` 和 `runtime`。主容器优先于依赖，`library` 优先于 `runtime`，同角色在当前会话中按挂载顺序选择首个定义。交叉引用、描述符补全和类层次使用同一解析顺序。带 `role` 的查询可按引用来源筛选，角色过滤的内容枚举保留各容器的物理条目。

已登记的依赖路径、指纹和角色保存在 `project.json`，项目重开时重新装载并检查指纹。登记文件按路径排序，重开时按文件中的输入顺序恢复。缺失或内容变化的依赖记录在 `mount_restore_errors` 中，成功重新挂载对应路径后清除该项警告。主文件在同一路径更新时刷新其登记指纹，保留依赖登记。

## 错误与分析覆盖

APK 的每个 DEX 单元独立解析。存在有效单元时工作区可继续分析，损坏单元通过 `dex_load_errors` 返回；没有有效 DEX 单元时打开失败。单类或单方法的解码错误按查询的容错路径跳过。

`project_open` 和 `project_info` 在存在 DEX 装载错误或挂载恢复错误时返回 `analysis_incomplete:true`，并附带对应错误列表。调用方可据此判断查询结果所覆盖的输入。
