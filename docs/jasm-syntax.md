# Jasm 方言语法（mjt 权威版）

## 1 定位与权威性

本文描述 mjt 生态内实际流转的 Jasm 方言：`class_query(kind=jasm)` 与 pysdk `mjt.class_jasm` 返回的规范化文本，以及 `class_validate_jasm`、`apply_jasm` 接受的汇编源。本文以实现为准，上游文档（jumanji144/Jasm 的 `docs/language`）只做对照，两者冲突时以本文为准并在第 7 节列出差异。

推导所据的实现：

| 环节 | 实现 |
| --- | --- |
| 解析（读侧） | libjhasm `Tokenizer` / `DeclarationParser` / `ASTProcessor`，二进制包 `libjhasm` 0.2.0（`get_lib_version()`），固定上游 revision `aacebfa`（`get_upstream_revision()`），头文件在 xmake 包目录 `include/jhasm/` |
| 规范化输出（写侧） | libjhasm `JvmPrinter::print`（契约：将 classfile bytes 打印为可重新汇编的规范化 Jasm） |
| mjbase 门面 | `JvmJasmService::assemble` / `disassemble` / `disassembleMethod` / `validate`（mjbase `mjbase/jasm/jvm_jasm_service.cpp`，转调 `jhasm::assemble` / `jhasm::disassemble`） |
| 转义原语 | libjhasm `util::EscapeUtil`（`escape_string` / `escape_literal` / `unescape`） |
| mjt 显示层 | mjbase `IdentifierUtil::printReadable` / `needEscape` / `escapeUnicode` / `unescapeUnicode`（mjbase `mjbase/mapping/identifier_util.cpp`） |
| 测试 | `unittest/jhasm/`（37 个用例），fixtures 在 `unittest/jhasm/test_resource/jasm/` |

除引用实现外，本文的转义与规范化结论均经探针程序（链接 libjhasm 二进制，编译 fixture 与构造源码后取 `JvmPrinter` 输出与 classfile 常量池字节）实证，第 8 节标注了未能实证的存疑点。

## 2 词法：字符集与转义

### 2.1 token 类别

Token 分六类（`jhasm::parser::TokenType`）：`Number`（整数/浮点，含后缀 `l/L d/D f/F` 与 `nan`、`infinity`）、`String`（双引号内文本）、`Character`（单引号内文本）、`Identifier`（非空白且不属于其他类别的文本）、`Operator`（`{` `}` `:` `,` 四字符）、`Comment`（`//` 行注释内容）。源文本为 UTF-16，位置诊断以 UTF-16 code unit 计（`jhasm::util::Range` / `Location`）。

### 2.2 读写两侧的转义是两套规则

本方言的转义必须按"读侧（解析器）"与"写侧（规范化打印机）"分开陈述，两者不对称；另有一个只影响展示的 mjbase 显示层。混谈这三层是文档失真的主要来源。

### 2.3 读侧：标识符不反转义（与上游相反）

标识符 token（类名、字段名、方法名、标签、局部变量名等一切裸词）**不做任何反转义**：`\uXXXX` 在标识符里是名字的字面组成部分，原样进入 classfile 常量池。证据：`jhasm::ast::Identifier::literal()` 直接返回 `content()`（头文件），无 unescape 步骤；探针编译 `new \u0000` 得到常量池类名为字面 6 字符 `\u0000`，编译 `new \\u0000` 得到 7 字符 `\\u0000`；fixture `Example-escape-type-name.jasm` 与 `Example-obfuscated-indy.jasm` 的真实编译产物同证（常量池中为 `\u0000`、`\u0020` 字面文本）。因此"所见即所存"：源码里标识符写什么字节，JVM 名就是什么字节。

标识符接受任意非空白、非 `{}:,`、非引号触发字符的文本，含原始 UTF-8 非 ASCII（探针实证：字段名 é 原文可解析、可编译、可打印回原文）。

### 2.4 读侧：字符串与字符字面量反转义

双引号/单引号内按 Java 转义族反转义：短形式 `\n \r \t \b \f \" \' \\`，Unicode 形式 `\uXXXX`（十六进制大小写不敏感，探针实证 `"\u00e9\u4e2d"` 解析为 é中）。`EscapeUtil::unescape` 对非法 `\uXXXX`（如 `\u12XZ`）报 `InvalidUnicodeEscape`，对尾随反斜杠报 `TrailingBackslash`，并给出 code unit 偏移（`jhasm::util::UnescapeError`）。

### 2.5 写侧：规范化输出的转义

标识符位置（`EscapeUtil::escape_literal`，"按 Jasm identifier literal 规则转义"）：反斜杠写为 `\\`；NUL、空格、逗号写为 `\u0000`、`\u0020`、`\u002C`（十六进制大写，单测向量 `LiteralEscapeCoversSeparatorsNulAndSupplementaryCharacters`）；其余字符原样 UTF-8 输出，**非 ASCII 不转义**（探针实证：é 字段名打印为原文 é）。十六进制一律大写 `%04X` 风格。

字符串位置（`EscapeUtil::escape_string`）：换行写为 `\u000A`（不用 `\n` 短形式，探针实证）、双引号写为 `\"`、反斜杠写为 `\\`；空格、逗号、花括号、冒号及一切非 ASCII 原样（探针实证 `ldc "a b,c{d}e:f\u000Ag\"h\\i"` 的重打印形态）。

### 2.6 mjbase 显示层：展示用激进转义

mjt 反汇编注释、shadow 符号显示、workspace 符号表走 mjbase `IdentifierUtil::printReadable`（判定函数 `needEscape`）：只有 0x21–0x7E 且非 `\` 的字符原样保留，其余——空格、反斜杠、0x00–0x1F、0x7F、一切 ≥0x80（含全部非 ASCII 可显示字符）——一律转 `\uXXXX` 大写十六进制；非法 UTF-8 字节转 `\uFFFD`。配套 `unescapeUnicode` 反向还原（hex 大小写不敏感，非法序列原样透传，孤立代理解码为 WTF-8 字节）。`printReadable` 另有长游程压缩 `...(省略N个)...`（阈值参数控制，仅展示）。**该层是 mjt 的显示口径，不是 Jasm 语法**；AI 从 `insn_detail`、符号表等看到的 `\uXXXX` 是展示编码，经 `unescapeUnicode` 可还原，与 2.3/2.5 的源码语义无关。

### 2.7 AI 输出安全性结论

AI 生成 Jasm 时：可安全直出的字符集是 0x21–0x7E（除 `\`）加上换行与引号的转义形式；非 ASCII 直接输出 UTF-8 即可被解析（标识符与字符串都接受）；永远不要在标识符位置写 `\uXXXX` 并期望它变成别的字符——它就是字面文本。

## 3 文件骨架与声明文法

### 3.1 类骨架

文件由可选的顶层属性声明加一个类声明构成（摘自 `Example.jasm`）：

```jasm
.super java/lang/Object
.class public super Example {
    .method public exampleMethod (LExample;)LExample; {
        parameters: { this, other },
        code: {
        A:
            aload this
            areturn
        B:
        }
    }
}
```

类修饰符集（上游 `Basics.md`，`jhasm::ast::Modifiers` 校验）：`public private protected static final native abstract interface synthetic strict annotation enum super module`。`.super` 之后的超类型用 internal name（`a/b/C`）。`.class` 行的修饰符按 classfile 实际 access flags 回显，无 super 修饰时不补写（探针实证）。

### 3.2 成员声明

字段：`.field <modifiers> <name> <descriptor>`，修饰符集含 `volatile transient` 等（上游 `Basics.md`）。方法：`.method <modifiers> <name> <descriptor> { ... }`，体内可选段 `parameters:`、`exceptions:`、`code:`（及注解接口方法的 `default-value:`），可选段之间逗号分隔。fixture 里的真实字段与方法形态（探针打印含非 ASCII 名字的类）：

```jasm
.field public \u00E9 I

.method public \u4E2D\u6587 ()V {
    parameters: { this },
    code: {
        new \u4E2D
        pop
        ldc "é中文"
        return
    }
}
```

注意此例正是 2.3 的活标本：字段名是字面 6 字符 `\u00E9`，字符串内容才是真 é中文。

### 3.3 类级属性声明

以下声明紧邻类头或成员之前（上游 `Attributes.md`，`jhasm::ast::specific.hpp` 有对应 AST 节点，fixture 均可编译）：`.inner <modifiers> { name: …, inner: …, outer: … }`、`.implements`、`.sourcefile "…"`、`.outer-class`、`.outer-method`、`.permitted-subclass`、`.nest-host`、`.nest-member`、`.record-component`。

注解（摘自 `Example-class-annotation.jasm`）：

```jasm
.visible-annotation TestAnnotation {
    number: 15,
    bogusNumber: NaN,
    charValue: 'c',
    subWithString: .annotation org/jetbrains/annotations/NotNull {
        value: "Hello, world!"
    },
    subWithEnum: .annotation EnumHolder {
        value: .enum net/fabricmc/api/EnvType CLIENT
    },
    subWithClass: .annotation ClassHolder {
        value: { java/lang/String }
    },
    stringArray: { "one", "two", "three" }
}
```

`.invisible-annotation` 为 `RetentionPolicy.CLASS`；嵌套注解一律 `.annotation`；枚举值 `.enum <internalName> <constant>`；类值放数组 `{ java/lang/String }`。方法默认值 `default-value:` 支持常量、数组、`.enum`、`.annotation`（上游 `Attributes.md`）。

### 3.4 值文法

值可以是 identifier（类/成员路径、描述符）、number、string、character、数组 `{ v1, v2 }` 或键值对象 `{ key: value, … }`（`jhasm::ast::Array` / `Object`）；声明（`.annotation`、`.enum`）亦可作值。

## 4 指令与操作数形态

### 4.1 常量加载

`ldc` 承载全部非常量池简单形态：整数 `ldc 41`、long `ldc 50L`、float `ldc 100.0F`、double `ldc 0.03333333333333333D`、`ldc NaN` / `NaND` / `NaNF`、`ldc infinity` / `-infinity`、类常量 `ldc Ljava/lang/String;`、字符串 `ldc "Hello, World!"`、字符 `ldc 'e'`（各 fixture 实证；`NaN` 等价 `NaND`，见 `Example-nan.jasm`）。浮点最短可往返表示用 `%.9g`（float）与 `%.17g`（double）口径（mjbase `class_render.cpp` 的同源注释口径）。

condy（摘自 `Example-condy.jasm`）：

```jasm
ldc { dynamicConstName, Ljava/lang/String;, { invokestatic, StringProvider.get, (II)Ljava/lang/String; }, { 1, 2 } }
```

四元组为 `{ 名字, 类型描述符, BSM 引用, 静态参数 }`，名字可以是任意 identifier（`Example-condy-int-name.jasm` 用 `5000`）。

### 4.2 字段与方法调用

字段访问 `Owner.name Desc` 三段空格分隔：`getstatic java/lang/System.out Ljava/io/PrintStream;`、`putfield Example.someBool Z`。方法调用 `Owner.name (Desc)Ret`：`invokevirtual java/lang/String.length ()I`；接口形式 `invokeinterface java/util/Collection.isEmpty ()Z`；`invokestaticinterface` 为本方言保留助记符（`Example-type-infer-list.jasm`）。

### 4.3 invokedynamic

长形式（canonical 输出恒为此形）：

```jasm
invokedynamic hello (Ljava/io/PrintStream;Ljava/lang/String;)V { invokestatic, Example.bootstrapMethod, (Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/CallSite; } { }
```

依次为indy 名、方法描述符、`{ invokestatic, Owner.bsm, BSM描述符 }`、静态参数对象。输入可写短形式 `invokedynamic get ()Ljava/util/function/Supplier; LambdaMetafactory.metafactory { … }`（省略 BSM 花括号对象），canonical 回读时统一输出长形式且 BSM 全限定（探针实证：短形式 `LambdaMetafactory.metafactory` 打回为 `java/lang/invoke/LambdaMetafactory.metafactory`）。

### 4.4 其余操作数

`iinc <var> <incr>`（`iinc 1 25`）；`multianewarray <desc> <dims>`（`multianewarray [[I 2`）；`anewarray [Ljava/lang/Object;`；`new` / `checkcast` / `instanceof` 后接类型 internal name；`wide` 按其背板指令展开（mjbase `class_render.cpp` 的 `operandText` 对 `Wide` 委派背板）。

### 4.5 switch

canonical 单行对象形态（探针实证）：

```jasm
tableswitch { min: 0, max: 4, cases: { L0, L1, L2, L3, L4 }, default: L5 }
```

`lookupswitch` 的 canonical 形态未单独实证：现有 fixture 的 switch 均以 `tableswitch` 书写，单目 switch（min==max）canonical 化为 `tableswitch` 单目形态（探针实证 `Example-athrow-before-return.jasm`），读侧是否接受 `lookupswitch` 助记符见第 8 节存疑点。

## 5 标签、异常表、局部变量与行号

标签为 `Identifier:`，可前向引用，可出现在任意指令之间（上游 `Instructions.md`；`ast::Label`）。canonical 输出把标签统一重编号为 `L0…Ln`（探针实证：输入 `A:`/`B:` 打回为 `L0:`/`L1:`），无引用需要时不生成尾随标签。跳转指令目标写标签名：`ifeq C`、`goto D`（输入形态）；canonical 下即 `ifeq L2` 等。

异常表挂在方法体的 `exceptions:` 段，每项四元组 `{ start, end, handler, type }`，type 为 internal name 或全捕获 `*`（摘自 `Example-exception.jasm`）：

```jasm
exceptions: {
    { A, B, B, * }
},
```

局部变量在输入里可用 `parameters: { this, other }` 命名，并用名字读写（`istore value`、`astore ex`）；**canonical 输出不回显 `parameters:` 段，一切局部变量按槽位索引打印**（`aload 0`、`istore 1`，探针实证）。`line N` 声明（LineNumberTable）附着于标签后的指令（`Example-anewarray-array.jasm` 输入与 canonical 均为 `L0:` 后 `line 3`）。

## 6 确定性与可 diff 保证

`JvmPrinter` 的契约是"可重新汇编的规范化 Jasm"：同一 classfile 字节必产生同一文本。行尾为纯 LF（探针按字节计数 CR=0；`mjt/mcp/src/tool/diff.cpp` 同注"class_jasm canonical 输出为 LF"），diff 工具会把客户端 CRLF 剥成 LF 再匹配 base。缩进骨架固定：成员 4 空格、`code:`/标签 8 空格、指令 12 空格（探针实证）。

规范化包括：标签重编号为 `L*`、局部变量换槽位索引、`parameters:` 不回显、`bipush`/`sipush` 类窄整数量并入 `ldc`（`bipush 20` 打回 `ldc 20`；`iconst_*` 保留）、indy 短形式展开为长形式、注释不保留。因此 canonical 文本与手写源逐行不同是常态，语义等价以重新汇编为准。

往返验证：`unittest/jhasm/jvm_printer_test.cpp` 的 `RoundTripsAllUpstreamLegalJvmFixtures` 对全部 44 个 `.jasm` fixture 执行"编译 → 打印 → 再编译"并比对类型名、字段、方法与注解数量；本次实机运行 `libjhasm_unittest` 37 个用例全绿。带 `// SKIP-ROUND-TRIP-EQUALITY` 首行注释的 fixture（`Example-comment.jasm` 等 5 个）豁免注释保真——注释本来就不在往返保证内。

取数口径：`class_jasm` 读容器落账字节而非树缓存，树编辑（`add_method`、`emit_*` 等）后须先 flush 再取（`mjt-py/doc_ai/pysdk-object-shapes.md`）。

## 7 与上游 jumanji144/Jasm 的差异清单

上游 checkout 为 `/d/WorkSpace/Jasm`（origin `jumanji144/Jasm`，revision `aacebfa`），Java 实现；我们的 libjhasm 是其 C++ 重写魔改版。

1. **标识符反转义语义相反**：上游 `ASTIdentifier.literal()` 返回 `EscapeUtil.unescape(content())`，即源里 `\u0020` 求值为真空格；我们 `jhasm::ast::Identifier::literal()` 返回 token 原文，`\u0020` 进名字的就是这 6 个字符。上游 `docs/language/Syntax.md`"escape sequences will be turned into the respective character"对我方标识符不成立。
2. **写侧标识符转义范围不同**：上游 `EscapeUtil.escapeLiteral` 转义空格、逗号、点、冒号、花括号、两种引号，外加一大片控制/不可见码位区段（`LITERAL_UNICODE_ESCAPE_RANGES`）与 NBSP/全角空格；我们实证转义 `\\` 与 NUL/空格/逗号，非 ASCII 与其余字符原样。逐字符完整对照未验证（见第 8 节）。
3. **写侧字符串换行形态不同**：上游 `escapeString` 用 `\n` 短形式；我们输出 `\u000A`。
4. **mjbase 显示层为我方独有**：`needEscape` 的"非 ASCII 一律 `\uXXXX` 大写"清洗策略（2.6 节）在上游无对应物，是本仓保证"AI 必定可输出、显示无歧义"的口径。
5. **读侧诊断显式化**：上游 `unescape` 对未知转义静默透传；我们 `EscapeUtil::unescape` 返回 `UnescapeError`（`TrailingBackslash` / `TruncatedUnicodeEscape` / `InvalidUnicodeEscape`）带 code unit 偏移。
6. **词法对齐**：token 六分类、数字后缀、`nan`/`infinity`、对象/数组值、声明结构均与上游一致，fixtures 直接沿用上游测试资源（`RoundTripsAllUpstreamLegalJvmFixtures`）。

## 8 AI 生成注意事项与存疑点

1. **标识符位置不要写 `\uXXXX` 期待反转义**：写 `new \u00E9` 得到的类名字面就是 `\u00E9`。要真 é 直接写 UTF-8 é。
2. **含空格/逗号/NUL 的真名在本方言里无法从源码构造**：裸空格切 token，`\u0020` 变字面文本；反向地，printer 遇到此类真名（来自外部 classfile）会按 `escape_literal` 输出 `\u0020`，读回即不对称改名。处理此类类时不要假设 jasm 文本可无损重汇编，diff 场景（文本稳定）不受影响。
3. **字符串换行**：源里写 `\n` 可解析，但 canonical 输出是 `\u000A`；对 canonical 文本做字符串匹配时按 `\u000A` 找。
4. **别拿 canonical 文本与手写源逐行 diff**：标签重编号、局部变量索引化、`bipush`→`ldc`、indy 长形式化、注释丢失都是规范化行为（第 6 节）。
5. **mjt 显示层的 `\uXXXX` 不是源码**：反汇编注释、符号表里的转义是展示编码（2.6 节），粘进 Jasm 源会变成字面文本。
6. **描述符里的非 ASCII**（混淆产物常见）：在 Jasm 里属标识符 token，原样 UTF-8 即可，无需也不应转义。

存疑点（实现未验证/待确认）：

- `escape_literal` 对点、冒号、花括号、引号及上游不可见区段是否转义：libjhasm 为二进制包，头文件未逐字符明示，现有单测向量只覆盖反斜杠/NUL/空格/逗号/补充平面字符；来自 classfile 的真名若含这些语法字符，canonical 形态待实验确认。
- `lookupswitch` 助记符的读侧接受性与多目 canonical 行形态未实证（现有 fixture 均用 `tableswitch`，单目已证并入 `tableswitch`）。
- 常量加载规范化的精确边界（哪些窄形式并入 `ldc`、`iconst_*` 之外的保留集）只抽样实证，未系统验证。
- 读侧字符串 `\uXXXX` 的代理对区（`\uD800` 单独出现）行为未实证；mjbase 显示层已证按 WTF-8 解码（`identifier_util_test.cpp`），libjhasm 读侧未测。
