# mjt

JVM 字节码工具集的 **MCP 服务器**。通过 [Model Context Protocol](https://modelcontextprotocol.io)
把 classfile 静态分析、字节码编辑、mapping 读写与交叉引用分析暴露给 AI 编码助手。

[English](README.md) | **简体中文**

## 是什么

`mjt-mcp` 是一个 stdio MCP 服务器进程。它在一侧连接受信任的模型客户端（Claude Code、
opencode 等），在另一侧连接受分析的 JAR / classfile —— 分析能力全部在本地执行，
不依赖被分析产物的网络访问。

典型用途：

- 在改动一个陌生的 JAR 前，先摸清它的类结构、方法签名与调用关系
- 查询 mapping（ProGuard / R8 / Tiny / SRG / YGuard / Allatori 等格式）的解析结果
- 对 classfile 做字节码级编辑并落盘
- 追踪方法/字段的交叉引用
- 配合 CFR / Vineflower 做源码还原迭代

## 构建

需要 xmake 3.x 与 C++17 编译器（Windows 用 MSVC，Linux 用 gcc/clang）。

```sh
xmake repo --add luiox-repo https://github.com/luiox/luiox-repo.git
xmake f -p windows          # Linux 用 -p linux
xmake build mjt-mcp
```

依赖经 [luiox-repo](https://github.com/luiox/luiox-repo) 直连拉取：libca、libmcp、
libjstr、libjzip、libgarena、libjclassfile、libjstd、
libjdk_finder、libjhasm；zlib / nlohmann_json 走
[xrepo](https://xmake.io)。libjbc 走项目内联包定义（git 直连 + commit 锁，≥
mjbase 门控下界）。字节码基座本体是 mjbase，经 luiox-repo 直连包消费
（`add_requires("mjbase 0.0.22")`，包词条钉上游 main commit）。

### 测试

```sh
xmake f --with_tests=y
xmake build mjt_mcp_unittest
xmake run mjt_mcp_unittest --gtest_brief=1
```

部分用例需要 JDK；缺失时相关用例 `GTEST_SKIP` 而非失败。测试产物落在各项目根的
`test/` 目录，可用 `LIBCA_TEST_OUT_ROOT` 改写。

已包化的各库在 `unittest/` 自带单测（`libjhasm_unittest`、
`libjdk_finder_unittest`），构建与运行方式相同。

## 运行

```sh
xmake run mjt-mcp
```

或直接执行 `build/<plat>/<arch>/<mode>/artifacts/.../mjt-mcp.exe`。进程走 stdio
传输，按 MCP 协议与客户端握手。

Windows 下 exe 嵌了 `activeCodePage=UTF-8` 与 `longPathAware` 的 manifest，
使中文类名参数与长路径在默认 GBK 代码页下也能正常工作（需 Windows 10 1903+
识别前者）。

## CLI 用法

`mjt` 是普通命令行工具（`xmake build mjt`），九个面向 JAR 与 mapping
文件的批处理式命令。退出码约定：`0` 成功（含 `--help`）、`1` 用法错误
（参数缺失、输入文件不存在、格式名不识别、类选择器无命中、旗标值非法）、
`2` 运行失败（打开、解析、修复、导出、反编译、抽取、扫描或反汇编失败）。

```sh
mjt --version          # 输出版本号
mjt --help             # 命令树帮助
```

### `mjt info` — jar 概览

```sh
mjt info -i <jar> [--json]
```

输出条目数、类数、成员普查（方法/字段总数、native 方法数、两侧可见性
分布）、类形态分布（class / interface / enum / annotation / record /
module）、类来源分布（`jdk` / `known` / `unknown`，包前缀派生）、条目路径
形态 facet 分布（`regular`、`empty_segment`…）、字节码版本分布与 manifest
有无。普查事实来自分析库投影；`--json` 输出同一数据（键为稳定英文）。

### `mjt mapping` — mapping 格式互转

```sh
mjt mapping -i <in> -o <out> --from <fmt> --to <fmt>
```

经统一中间表示在格式间互转 mapping 文件。格式名大小写不敏感；SRG、
Tiny-V2、proguard 三族两两互通（其余注册格式：`Tiny-V1`、`Simple`、
`Enigma`、`R8`、`YGuard`、`Allatori`、`Shadow-Alias`）。格式名不识别时
fail-loud，退出码 `1`。

可选：用源 mapping 重映射 jar：

```sh
mjt mapping -i <in> -o <out> --from <fmt> --to <fmt> \
    --remap-jar <jar> [--remap-out <jar>] [--remap-policy bytecode-only|reflection|metadata|all]
```

`--remap-jar` 经 mjbase remapper 把源 mapping 应用到 jar；重映射副本缺省写
`<jar 主名>-remapped.jar`，可用 `--remap-out` 指定路径。

### `mjt jar fix` — jar 诊断与修复

```sh
mjt jar fix -i <in> -o <out>
```

扫描 jar，移除不可修复条目（class 字节未通过字节码校验、`.class` 条目不是
合法 class 字节、命中已知损坏输出标记的条目），把修复副本写到 `<out>`（已
存在的输出文件会被替换）。报告逐条列出修了什么、保留了什么；不可修的失败
给非零退出码与原因。输入输出必须是不同文件。

### `mjt decompile` — 反编译 jar 类为 Java 源码

```sh
mjt decompile -i <jar> -c <internalName> [-o <file.java>]   # 单类
mjt decompile -i <jar> -o <outdir> --all                    # 整 jar
mjt decompile -i <jar> -o <out.zip> --all --decompiler vineflower   # 归档输出
```

单类模式按内部名（slash 形，如 `fixture/Alpha`）取目标类字节，落到临时目录
短安全名（`Target.class`），后端以该文件为输入、classpath 指向原 jar 保跨
类型上下文。类名本身从不落盘——单段超过 255 UTF-16 单元或含文件系统非法
字符的敌意类名照样可达，且 vineflower「一个敌意类连累同 jar 全部反编译」
的级联毒化在结构性上不再可达。带 `-o` 时源码写 `.java` 文件；省略 `-o`
打印到 stdout。

批量模式整 jar 交给后端（CFR 写源码树，vineflower 写源码树或 `.zip` 归档
——归档条目名不受文件系统路径限制）。无法落盘的条目名会被跳过：本次运行
继续，stderr 列出跳过清单并指向单类模式或 `mjt disasm`。`--all` 显式确认
整 jar 成本；批量缺它时退出码 `1` 并给出类数提示。

公共选项（与 MCP decompile 契约对齐）：`--decompiler cfr|vineflower|jcdc`（缺省
`cfr`）、`--decompiler-jar <jar>`（`jcdc` 时承载可执行文件路径）、
`--timeout-ms <n>`（缺省 120000）、`--max-memory-mib <n>`（缺省 1024）。
`jcdc` 是实验性原生后端：不经 JVM，经 `MJT_JCDC_EXE` / `MJT_DECOMPILER_HOME`
发现，且恒传 `--synthetic`（成员全量显示，不静默隐藏）。

### `mjt strings` — 常量池字符串抽取

```sh
mjt strings -i <jar> [--class <internalName>] [--min-len N] [--json] [-o <file>]
```

抽取 jar 内类的常量池字符串（整 jar；`--class` 限定单类），输出去重列表，
按出现次数排序。出现 = 常量池条目：一个字符串字面量同时产出
`CONSTANT_String` 条目与背书 `CONSTANT_Utf8` 条目（`--json` 携带每个值的
`kinds` 细分：utf8 / string / class），同一文本被两个类引用即计 4。
`--min-len N` 过滤短于 N 字节的值（规范 UTF-8 字节长度，缺省 `1`）；畸形
class 条目跳过并计数。带 `-o` 时完整报告写文件、stdout 只留摘要；值恒单行
（控制字符转义）且恒为合法 UTF-8（常量池编码怪形经规范解码兜底）。

### `mjt xref` — 类/成员反查

```sh
mjt xref -i <jar> -c <internalName> [-m <member>] [--direction to|from|both] [--limit N] [--json]
```

反查类/成员周边的直接引用。`to`（缺省）找编码目标命中的全部位置（该类的
方法调用与字段访问、指向它的类型指令与类头声明）；`from` 找该类方法体发出
的全部引用；`both` 为并集（自引用边去重）。`-m <member>` 收缩到单个成员：
`name` 跨描述符匹配，`name(descriptor)`（如 `act(I)V`）精确匹配。类必须
存在于 jar；类名无命中或成员选择器语法非法退出码 `1`，成员选择器合法但
无命中输出空结果、退出码 `0`。

输出恒有界：`--limit` 限制引用行数（缺省 `200`）；超出时报告 `total` 并
提示放大 `--limit`——混淆 jar 反查可达百万行，无界输出是事故。每行引用含
kind（`method` / `field` / `type`）、来源（`class.method(desc)@insn` 或
类头行的 `(class header)`）与目标（`owner.name(desc)` 或类型名）。

### `mjt classes` — 类清单（形态与来源列）

```sh
mjt classes -i <jar> [--kind class|interface|enum|record|annotation|module] [--origin jdk|known|unknown] [--limit N] [--json] [-o <file>]
```

列出 jar 内全部类（内部名 + 形态 + 来源三列），数据来自分析库 census 投影——
与 `mjt info` 同源同果的派生列。`--kind` 按类形态过滤，`--origin` 按包前缀
来源过滤（`jdk` / `known` / `unknown`）；两者可组合（交集），值大小写不敏感。
行序 = 内部名字节序升序（跨平台确定）。输出缺省有界：`--limit` 限制 stdout
列表行数（缺省 `10000`）；超出时报告 `total` 并提示放大上限或加过滤项。
`-o` 不经防呆上限——完整清单写文件、stdout 只留摘要；`--json` 输出同一数据
（键为稳定英文）。

### `mjt disasm` — 单类 jasm 文本

```sh
mjt disasm -i <jar> -c <internalName> [-o <file>]
```

输出单类的规范化 jasm 文本——与 MCP `class_query kind=jasm` 同源。类按内部
名（slash 形）定位；类不存在退出码 `1`。纯文本面、不依赖反编译器：敌意类名、
批量反编译预检跳过的类、指令形状快速核对（`iload_0`、`invokestatic`…）都走
这里。省略 `-o` 时 jasm 文本打印到 stdout（重定向取文件语义）；带 `-o` 落盘。

### `mjt subs` — 单类成员清单

```sh
mjt subs -i <jar> -c <internalName> [--json]
```

列出单类的字段与方法：名、描述符、原始 access 旗标（JVMS 位掩码，十进制）
与 `native` 标记（仅方法）。字段在前，各组保持 classfile 声明序；头部报告类
形态（与 `mjt classes` 同源派生）。类不存在退出码 `1`；`--json` 输出同一数据
（键为稳定英文）。

语言：文案跟随 `MJT_LANG` 环境变量（缺省 `en_US`，支持 `zh_CN`）。

## 仓库结构

```
mjt/mcp/src/     MCP 服务器与工具集（核心产品代码）
mjt/mcp/test/    MCP 层单测
mjt/cli/         `mjt` CLI 二进制（info / mapping / jar fix / decompile / strings / xref / classes / disasm / subs）
mjt/test/cli/    CLI 层单测
mjt/src/shared/  MCP 工具集共用的底层能力（反编译接入、jar 重映射、workspace…）
skills/          MCP 技能定义
mjt-py/src/      嵌入式 Python SDK（mjt --script 嵌入解释器与 mjt.pyd
                 pybind11 binding；--with_pysdk 门控）
unittest/        已包化各库的单测（libjhasm、libjdk_finder）
test_helper/     测试脚手架（gtest 集成、产物路径解析）
xmake/           构建开关、libjbc 内联包定义、libca 规则内嵌、Windows manifest
                 规则、包仓覆盖
```

共享层在仓外：mjbase 源码树（兄弟目录依赖）与 luiox-repo 各包。`mjt` 是产品线，
`mjt-py` 是可选的 SDK 组件。

## 约定

- **No RTTI**：`dynamic_cast` / `typeid` 禁用，用 `typed::cast<T>` 家族
- **类型**：统一走 `ca::` 命名空间的别名（`i32`/`u32`/`usize`…），不裸写 `int`/`long`
- **错误**：`Result<T, E>` 值语义，不抛异常跨层
- **字符串**：`Utf8String` 家族；借用视图用 `std::string_view`
- **头文件**：模块内 `"file.hpp"`，跨模块 `"module/path/file.hpp"`，禁 `../`
- **注释**：Doxygen `///`；禁止 `/** */`

更完整的开发约定见 [AGENTS.md](AGENTS.md)。

## License

[MIT](LICENSE)
