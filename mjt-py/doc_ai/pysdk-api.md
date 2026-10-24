# mjt-py SDK API 参考（pysdk-api.md）

由 `gen_api_docs.py` 自省生成于 2026-10-25T00:43:14；函数改动后重跑本脚本即可刷新，勿手编。

生成方式：`mjt-mcp.exe --script mjt-py/src/gen_api_docs.py mjt-py/doc_ai/pysdk-api.md`

## 约定

- 键面：`h` = 容器句柄 int；`cref` = (h, 类名)；`mref`/`fref` = (h, cls, name, desc)；类名一律 internal name（`a/b/C` 形态）。
- 异常：键失效/不存在抛 `mjt.StaleRefError`（KeyError 子类）；协作层 deadline 越限抛 `mjt.DeadlineExceeded`（KeyboardInterrupt 子类）。
- 绑定函数没有 `__signature__`，inspect.signature 不可用；签名在 `__doc__` 首行（pybind11 生成），本文档已逐符号抄录。
- util 层取用：`from mjt import util as U`（exec 片段模块，不能 `import mjt_util`）。

## mjt（C++ 绑定）

### 常量

- `mjt.acc`：dict，8 项
- `mjt.opcodes`：dict，202 项

### mjt.add_entry

```
add_entry(h: typing.SupportsInt | typing.SupportsIndex, uri: str, bytes: bytes) -> None

新增条目。

Args:
    h: 容器句柄。
    uri: 条目 URI（已存在即拒）。
    bytes: 条目字节。类字节自动进 name↔URI 索引；容器内同名类
        按 JVM first-wins 保留首条，落选者经 duplicate_names 可查。
Raises:
    StaleRefError: URI 已存在，或句柄失效。
```

### mjt.add_field

```
add_field(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str], name: str, desc: str, access: typing.SupportsInt | typing.SupportsIndex) -> tuple[int, str, str, str]

新增字段。

Args:
    cref: 类键 (h, 类名)。
    name: 字段名。
    desc: JVM 字段描述符。
    access: 访问标志位集。
Returns:
    新字段 fref。
Raises:
    StaleRefError: 键冲突或类不存在。
```

### mjt.add_method

```
add_method(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str], name: str, desc: str, access: typing.SupportsInt | typing.SupportsIndex, max_stack: typing.SupportsInt | typing.SupportsIndex, max_locals: typing.SupportsInt | typing.SupportsIndex) -> tuple[int, str, str, str]

新增方法。

Args:
    cref: 类键 (h, 类名)。
    name: 方法名。
    desc: JVM 方法描述符。
    access: 访问标志位集（对照 mjt.acc）。
    max_stack: 初始 max_stack。
    max_locals: 初始 max_locals。
Returns:
    新方法 mref（空方法体）。
Raises:
    StaleRefError: 键冲突（同名同描述符已存在）或类不存在。
```

### mjt.add_try_catch

```
add_try_catch(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], start_uid: typing.SupportsInt | typing.SupportsIndex, end_uid: typing.SupportsInt | typing.SupportsIndex, handler_uid: typing.SupportsInt | typing.SupportsIndex, catch_type: str) -> None

新增异常表行。

Args:
    mref: 方法键。
    start_uid/end_uid/handler_uid: try 起点、终点（不含）与 handler
        入口，三者均须为 label 节点 uid。
    catch_type: 捕获异常类 internal name；空串 = catch-all。
Raises:
    StaleRefError: 任一 uid 不是 label 或不存在。
```

### mjt.analysis_evaluate

```
analysis_evaluate(expression: str, bindings: object = None, simplify: bool = False, max_depth: typing.SupportsInt | typing.SupportsIndex = 1024) -> dict

分析表达式求值：解析 →（可选）化简 → 变量绑定求值。

Args:
    expression: 表达式文本（mjbase expr 语法，如 "1 + 2 * 3"）。
    bindings: 变量名 → int 的绑定 dict；None 表示无绑定。
    simplify: 求值前是否先化简。
    max_depth: 求值深度上限（1..4096，默认 1024）。
Returns:
    {expression, normalized, simplified, evaluated, value|error}；
    evaluated=False 时错误详情在 error，无 value 键。
Raises:
    ValueError: 表达式为空或解析失败，max_depth 越界，绑定值无法
        转 int。
```

### mjt.changed

```
changed() -> mjt.PreservedAnalyses

MethodPass.run 返回值：报告本 pass 有改动。
```

### mjt.class_apply_jasm

```
class_apply_jasm(h: typing.SupportsInt | typing.SupportsIndex, name: str, source: str, source_name: str = '', validation: str = 'relaxed') -> dict

事务式汇编替换类字节（名称校验 + 候选工作区复验全过才落）。

Args:
    h: 容器句柄。
    name: 目标类 internal name。
    source: 完整 Jasm 文本。
    source_name: 诊断定位用的源名；缺省 <name>.jasm。
    validation: 校验档位（同 class_validate_jasm）。
Returns:
    报告 dict（同 class_validate_jasm；成功时 written 为新类字节数），
    成功后该类缓存树作废。
Raises:
    StaleRefError: 类名不存在。
    RuntimeError: 现类字节反汇编失败或事务落容器失败。
```

### mjt.class_apply_jasm_diff

```
class_apply_jasm_diff(h: typing.SupportsInt | typing.SupportsIndex, name: str, diff: str, validation: str = 'relaxed') -> dict

diff 应用后事务式汇编替换类字节。

Args:
    h: 容器句柄。
    name: 目标类 internal name。
    diff: 对当前规范全文的 unified diff。
    validation: 校验档位（同 class_validate_jasm）。
Returns:
    报告 dict（同 class_apply_jasm），成功后该类缓存树作废。
Raises:
    StaleRefError: 类名不存在。
    RuntimeError: 现类字节反汇编失败或事务落容器失败。
```

### mjt.class_bytes

```
class_bytes(h: typing.SupportsInt | typing.SupportsIndex, name: str) -> bytes

读取类字节（不建树）。

Args:
    h: 容器句柄。
    name: internal name。
Returns:
    bytes（落账字节；脏树不参与）。
Raises:
    StaleRefError: 类名未索引。
```

### mjt.class_hierarchy

```
class_hierarchy(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str]) -> dict

查询继承闭包（super 链 + 全部接口传递闭包 + 沿途声明成员集）。

Args:
    cref: 类键 (h, 类名)。
Returns:
    {classes: [{name, super, interfaces}]}（self 先序 BFS；容器外父类/
    接口如 java/lang/Object 不入闭包），methods/fields:
    [(owner, name, desc, access)] 沿途容器内类的声明成员集——建继承
    成员集不必再外跑 javap/反射。
Raises:
    StaleRefError: 起点类不存在。
```

### mjt.class_info

```
class_info(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str]) -> dict

读取类声明信息。

Args:
    cref: 类键 (h, 类名)。
Returns:
    {name, super, access, version, interfaces}。
```

### mjt.class_jasm

```
class_jasm(h: typing.SupportsInt | typing.SupportsIndex, name: str) -> dict

类字节反汇编为规范化 Jasm 全文。

Args:
    h: 容器句柄。
    name: internal name。
Returns:
    {internal_name, source}（source 为 LF 规范化全文）。读的是容器
    落账字节——树编辑后须先 flush/write_back 再取，否则读到旧字节。
Raises:
    StaleRefError: 类名不存在。
    RuntimeError: 类字节 malformed 无法反汇编。
```

### mjt.class_names

```
class_names(h: typing.SupportsInt | typing.SupportsIndex) -> list[str]

列出容器内已解析类名。

Args:
    h: 容器句柄。
Returns:
    internal name（a/b/C 形态）升序列表。
```

### mjt.class_set_access

```
class_set_access(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str], access: typing.SupportsInt | typing.SupportsIndex) -> None

改写类访问标志（整字替换，位集对照 mjt.acc）。

Args:
    cref: 类键 (h, 类名)。
    access: 新访问标志位集。
Raises:
    StaleRefError: 类不存在。
```

### mjt.class_set_interfaces

```
class_set_interfaces(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str], interfaces: collections.abc.Sequence[str]) -> None

整表替换类接口列表。

Args:
    cref: 类键 (h, 类名)。
    interfaces: 接口 internal name 列表（整表替换，不追加）。
Raises:
    StaleRefError: 类不存在。
```

### mjt.class_set_super

```
class_set_super(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str], super: str) -> None

改写类父类。

Args:
    cref: 类键 (h, 类名)。
    super: 新父类 internal name（树编辑；引用重写走 rename token 面）。
Raises:
    StaleRefError: 类不存在。
```

### mjt.class_set_version

```
class_set_version(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str], version: typing.SupportsInt | typing.SupportsIndex) -> None

改写类文件版本号。

Args:
    cref: 类键 (h, 类名)。
    version: 新版本号（major，如 52 = Java 8）。
Raises:
    StaleRefError: 类不存在。
```

### mjt.class_uri

```
class_uri(h: typing.SupportsInt | typing.SupportsIndex, name: str) -> str

类名反查条目 URI。

Args:
    h: 容器句柄。
    name: internal name。
Returns:
    条目 URI 字符串。
Raises:
    StaleRefError: 类名未索引。
```

### mjt.class_validate_jasm

```
class_validate_jasm(h: typing.SupportsInt | typing.SupportsIndex, name: str, source: str, source_name: str = '', validation: str = 'relaxed') -> dict

试汇编完整 Jasm（容器深拷贝上验证，绝不落容器）。

Args:
    h: 容器句柄。
    name: 目标类 internal name（与 source 声明名一致才通过）。
    source: 完整 Jasm 文本。
    source_name: 诊断定位用的源名；缺省 <name>.jasm。
    validation: 校验档位 "relaxed"（默认，JVM 赋值兼容语义）/ "strict"
        （精确类型匹配）/ "off"（跳过校验门，含重建后 validate）；非法值
        抛 ValueError。
Returns:
    报告 dict：{ok, internal_name, written, diagnostics}；diagnostics
    条目为 {severity, stage, stage_name, gate, message, line, column}
    （severity 0=error 1=warning；gate 区分编辑门 edit 与重建管线
    rebuild）。内容性错误走 diagnostics 不抛异常。
Raises:
    StaleRefError: 类名不存在。
    RuntimeError: 现类字节反汇编失败。
```

### mjt.class_validate_jasm_diff

```
class_validate_jasm_diff(h: typing.SupportsInt | typing.SupportsIndex, name: str, diff: str, validation: str = 'relaxed') -> dict

diff 试汇编：对 class_jasm 当前全文应用 unified diff 后在容器拷贝上
验证，绝不落容器。

Args:
    h: 容器句柄。
    name: 目标类 internal name。
    diff: 对当前规范全文的 unified diff。
    validation: 校验档位（同 class_validate_jasm）。
Returns:
    报告 dict（同 class_validate_jasm）；上下文失配在 diagnostics 里
    给 hunk 序号。
Raises:
    StaleRefError: 类名不存在。
    RuntimeError: 现类字节反汇编失败。
```

### mjt.clear_deadline

```
clear_deadline() -> None

清除 deadline（回到无上限）。
```

### mjt.close_container

```
close_container(h: typing.SupportsInt | typing.SupportsIndex) -> None

显式释放容器。

Args:
    h: 容器句柄。此后该容器全部键 fail-closed（不调则进程退出兜底）。
```

### mjt.decompiler_compare

```
decompiler_compare(h: typing.SupportsInt | typing.SupportsIndex, name: str, jar: str = '', vineflower_jar: str = '', java: str = '', timeout_ms: typing.SupportsInt | typing.SupportsIndex = 0) -> dict

CFR 与 Vineflower 双后端反编译同名类，双源并列返回。

Args:
    h: 容器句柄。
    name: 目标类 internal name。
    jar: CFR 反编译器 jar 路径（空串走环境变量发现）。
    vineflower_jar: Vineflower jar 路径（空串走环境变量发现）。
    java: java 可执行程序路径（空串走 PATH）。
    timeout_ms: 单后端超时毫秒；0 用缺省上限。
Returns:
    {internal_name, sources: [{internal_name, backend, decompiler_jar,
    source_scope, source}]}（backends 依序 cfr/vineflower）。
Raises:
    RuntimeError: 后端不可用（jar/java 发现失败）——fail-closed。
    StaleRefError: 类名不存在。
```

### mjt.duplicate_names

```
duplicate_names(h: typing.SupportsInt | typing.SupportsIndex) -> list[str]

列出同名类的 first-wins 落选者。

Args:
    h: 容器句柄。
Returns:
    internal name 升序列表（落选条目不可经类名寻址）。
```

### mjt.emit_field

```
emit_field(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], op: typing.SupportsInt | typing.SupportsIndex, owner: str, name: str, desc: str, pos: typing.SupportsInt | typing.SupportsIndex | None = None) -> int

发字段访问指令（getstatic/putstatic/getfield/putfield）。

Args:
    mref: 方法键；op: opcode 值。
    owner/name/desc: 目标字段坐标（owner 为 internal name，desc 为
    字段描述符）。
    pos: 插入位置——None 尾插；给 uid 则插在该节点前。
Returns:
    新节点 uid。
```

### mjt.emit_iinc

```
emit_iinc(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], var: typing.SupportsInt | typing.SupportsIndex, incr: typing.SupportsInt | typing.SupportsIndex, pos: typing.SupportsInt | typing.SupportsIndex | None = None) -> int

发 iinc 指令。

Args:
    mref: 方法键；var: 局部变量槽位号；incr: 增量。
    pos: 插入位置——None 尾插；给 uid 则插在该节点前。
Returns:
    新节点 uid。
```

### mjt.emit_insn

```
emit_insn(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], op: typing.SupportsInt | typing.SupportsIndex, pos: typing.SupportsInt | typing.SupportsIndex | None = None) -> int

发零操作数指令。

Args:
    mref: 方法键 (h, cls, name, desc)。
    op: opcode 值（对照 mjt.opcodes）。
    pos: 插入位置——None 尾插；给 uid 则插在该节点前。
Returns:
    新节点 uid。
Raises:
    StaleRefError: 方法或 pos uid 不存在。
```

### mjt.emit_jump

```
emit_jump(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], op: typing.SupportsInt | typing.SupportsIndex, target_uid: typing.SupportsInt | typing.SupportsIndex, pos: typing.SupportsInt | typing.SupportsIndex | None = None) -> int

发跳转指令（目标须为既有 label uid）。

Args:
    mref: 方法键；op: opcode 值；target_uid: 目标 label 节点 uid。
    pos: 插入位置——None 尾插；给 uid 则插在该节点前。
Returns:
    新节点 uid。
```

### mjt.emit_label

```
emit_label(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], pos: typing.SupportsInt | typing.SupportsIndex | None = None) -> int

建 label 节点（跳转/异常表的锚）。

Args:
    mref: 方法键。
    pos: 插入位置——None 尾插；给 uid 则插在该节点前。
Returns:
    label uid。
```

### mjt.emit_ldc

```
emit_ldc(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], value: object, pos: typing.SupportsInt | typing.SupportsIndex | None = None) -> int

发 ldc 常量压栈。

Args:
    mref: 方法键。
    value: 常量载荷（str/int/float；str 经会话 arena intern，按值
    安全；py float 落 double ldc；int 溢出 int32 升 i64 载荷）。
    pos: 插入位置——None 尾插；给 uid 则插在该节点前。
Returns:
    新节点 uid。
Raises:
    TypeError: value 不是 str/int/float。
```

### mjt.emit_method_call

```
emit_method_call(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], op: typing.SupportsInt | typing.SupportsIndex, owner: str, name: str, desc: str, itf: bool, pos: typing.SupportsInt | typing.SupportsIndex | None = None) -> int

发方法调用指令（invokestatic/virtual/special/interface）。

Args:
    mref: 方法键；op: opcode 值。
    owner/name/desc: 被调方法坐标。
    itf: 是否接口方法。
    pos: 插入位置——None 尾插；给 uid 则插在该节点前。
Returns:
    新节点 uid。
```

### mjt.emit_switch

```
emit_switch(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], kind: str, default_uid: typing.SupportsInt | typing.SupportsIndex, targets: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex], pos: typing.SupportsInt | typing.SupportsIndex | None = None, min: typing.SupportsInt | typing.SupportsIndex = 0, max: typing.SupportsInt | typing.SupportsIndex = 0, keys: collections.abc.Sequence[typing.SupportsInt | typing.SupportsIndex] = []) -> int

发 switch 指令。

Args:
    mref: 方法键。
    kind: "table"（tableswitch）或 "lookup"（lookupswitch）。
    default_uid: 缺省分支 label uid（既有节点）。
    targets: 各分支 label uid 列表（既有节点）。
    pos: 插入位置——None 尾插；给 uid 则插在该节点前。
    min/max: kind="table" 时的取值区间，min..max 逐个对应 targets
        （len(targets) == max-min+1，结构性校验 fail-closed）。
    keys: kind="lookup" 的匹配键表，与 targets 成对且严格升序
        （JVM 查找表序，乱序写出的类真机验证必炸——fail-closed）。
Returns:
    switch 节点自身的新 uid。
Raises:
    ValueError: kind 非法、targets 数目与区间不符或 keys 乱序。
```

### mjt.emit_type

```
emit_type(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], op: typing.SupportsInt | typing.SupportsIndex, type_desc: str, pos: typing.SupportsInt | typing.SupportsIndex | None = None) -> int

发类型操作数指令（new/anewarray/checkcast/instanceof/…）。

Args:
    mref: 方法键；op: opcode 值；type_desc: 类型描述符。
    pos: 插入位置——None 尾插；给 uid 则插在该节点前。
Returns:
    新节点 uid。
```

### mjt.emit_var

```
emit_var(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], op: typing.SupportsInt | typing.SupportsIndex, var: typing.SupportsInt | typing.SupportsIndex, pos: typing.SupportsInt | typing.SupportsIndex | None = None) -> int

发局部变量指令（aload/astore/iload/…）。

Args:
    mref: 方法键；op: opcode 值；var: 局部变量槽位号。
    pos: 插入位置——None 尾插；给 uid 则插在该节点前。
Returns:
    新节点 uid。
```

### mjt.export_class

```
export_class(h: typing.SupportsInt | typing.SupportsIndex, name: str, path: str) -> None

单类导出为 .class 文件。

Args:
    h: 容器句柄。
    name: internal name。
    path: 输出文件路径。脏树先写回，挂账改名一并落账。
Raises:
    StaleRefError: 类名不存在。
    RuntimeError: COMPUTE_FRAMES 写出或落盘失败。
```

### mjt.export_jar

```
export_jar(h: typing.SupportsInt | typing.SupportsIndex, path: str, uris: collections.abc.Sequence[str] = []) -> None

导出 jar。

Args:
    h: 容器句柄。
    path: 输出 jar 路径。
    uris: 子集条目清单；空表导出全量。

导出前自动 flush 全部脏树并落账挂账改名（两阶段落盘：暂存 + 改名）。
Raises:
    RuntimeError: 落盘失败或 COMPUTE_FRAMES 写出失败。
```

### mjt.field_info

```
field_info(fref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str]) -> dict

读取字段声明信息。

Args:
    fref: 字段键 (h, cls, name, desc)。
Returns:
    {name, desc, access}。
Raises:
    StaleRefError: 字段不存在。
```

### mjt.field_set_access

```
field_set_access(fref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], access: typing.SupportsInt | typing.SupportsIndex) -> None

改写字段访问标志（整字替换，位集对照 mjt.acc）。

Args:
    fref: 字段键 (h, cls, name, desc)。
    access: 新访问标志位集。
Raises:
    StaleRefError: 字段不存在。
```

### mjt.fields

```
fields(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str]) -> list

列出字段声明。

Args:
    cref: 类键 (h, 类名)。
Returns:
    [(name, desc, access)] 列表（声明序）。
Raises:
    StaleRefError: 类不存在。
```

### mjt.first_insn

```
first_insn(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str]) -> int | None

取首个节点 uid（空方法体为 None）。
```

### mjt.flush

```
flush(h: typing.SupportsInt | typing.SupportsIndex, compute_frames: bool = True) -> list[str]

全容器脏树写回。

Args:
    h: 容器句柄。
    compute_frames: 是否重算 StackMapTable（缺省 True）。
Returns:
    写出的类名列表（升序）。
Raises:
    RuntimeError: 写出失败。
    mjt.DeadlineExceeded: 协作层 deadline 越限。
```

### mjt.get_entry_bytes

```
get_entry_bytes(h: typing.SupportsInt | typing.SupportsIndex, uri: str) -> bytes

读取条目原始字节。

Args:
    h: 容器句柄。
    uri: 条目 URI。
Returns:
    bytes（class/manifest/任意资源均可读）。
Raises:
    StaleRefError: 条目不存在。
```

### mjt.get_field

```
get_field(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str], name: str, desc: str = '') -> tuple[int, str, str, str]

取字段键（存在性校验）。desc 可省略：省略时按字段名在宿主类内唯一
解析回填（同名列异描述符报歧义并列候选，名字不存在报缺失）。

Args:
    cref: 类键 (h, 类名)。
    name: 字段名。
    desc: 字段描述符；可省略，省略时按名字唯一解析。
Returns:
    fref (h, cls, name, desc)。
Raises:
    StaleRefError: 字段不存在或同名列歧义。
```

### mjt.get_method

```
get_method(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str], name: str, desc: str = '') -> tuple[int, str, str, str]

取方法键（存在性校验）。desc 可省略：省略时按方法名在宿主类内唯一
解析回填（真重载报歧义并列候选，名字不存在报缺失）。

Args:
    cref: 类键 (h, 类名)。
    name: 方法名。
    desc: 方法描述符；可省略，省略时按名字唯一解析。
Returns:
    mref (h, cls, name, desc)。
Raises:
    StaleRefError: 方法不存在或同名重载歧义。
```

### mjt.insn_count

```
insn_count(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str]) -> int

方法体节点总数（含 label/line/frame 节点）。

Args:
    mref: 方法键 (h, cls, name, desc)。
Returns:
    int 节点数。
Raises:
    StaleRefError: 方法不存在。
```

### mjt.insn_detail

```
insn_detail(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex) -> dict

读取单节点详情（按 kind 展开全字段）。

Args:
    mref: 方法键 (h, cls, name, desc)。
    uid: 节点 uid。
Returns:
    dict——公共字段 uid/opcode/kind/mnemonic；按 kind 附操作数字段：
    int→operand、var→var、type→desc、field/method→owner/name/desc
    （method 另有 itf）、ldc→cst_tag/cst、jump→target、iinc→var/incr、
    switch→min/max/default/targets/keys、multianewarray→desc/dims、
    line→line。
Raises:
    StaleRefError: uid 不存在或已删除（墓地 fail-closed）。
```

### mjt.insn_index

```
insn_index(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex) -> int

查节点链表位次（诊断用，O(n)；定位用 uid 不用位次）。

Args:
    mref: 方法键 (h, cls, name, desc)。
    uid: 节点 uid。
Returns:
    int 位次。
Raises:
    StaleRefError: uid 不存在。
```

### mjt.insn_set_cst

```
insn_set_cst(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex, value: object) -> None

改写 ldc 载荷。

Args:
    mref: 方法键；uid: ldc 节点 uid；value: 新载荷（str/int/float）。
Raises:
    TypeError: value 不是 str/int/float。
    StaleRefError: uid 不存在。
```

### mjt.insn_set_desc

```
insn_set_desc(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex, type_desc: str) -> None

改写类型操作数（type/multianewarray 类节点）。

Args:
    mref: 方法键；uid: 节点 uid；type_desc: 新类型描述符。
Raises:
    StaleRefError: uid 不存在。
```

### mjt.insn_set_field_ref

```
insn_set_field_ref(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex, owner: str, name: str, desc: str) -> None

改写字段引用（field 类节点）。

Args:
    mref: 方法键；uid: 节点 uid。
    owner/name/desc: 新字段引用坐标（owner 为 internal name，desc 为
    字段描述符）。
Raises:
    StaleRefError: uid 不存在。
```

### mjt.insn_set_incr

```
insn_set_incr(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex, incr: typing.SupportsInt | typing.SupportsIndex) -> None

改写 iinc 增量。

Args:
    mref: 方法键；uid: iinc 节点 uid；incr: 新增量。
Raises:
    StaleRefError: uid 不存在。
```

### mjt.insn_set_method_ref

```
insn_set_method_ref(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex, owner: str, name: str, desc: str, itf: bool) -> None

改写方法引用（method 类节点）。

Args:
    mref: 方法键；uid: 节点 uid。
    owner/name/desc: 新方法引用坐标。
    itf: 是否接口方法。
Raises:
    StaleRefError: uid 不存在。
```

### mjt.insn_set_opcode

```
insn_set_opcode(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex, op: typing.SupportsInt | typing.SupportsIndex) -> None

改写节点 opcode。

Args:
    mref: 方法键；uid: 节点 uid；op: 新 opcode 值（对照 mjt.opcodes）。
Raises:
    StaleRefError: uid 不存在。
```

### mjt.insn_set_operand

```
insn_set_operand(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex, operand: typing.SupportsInt | typing.SupportsIndex) -> None

改写 int 操作数（bipush/sipush/newarray 类节点）。

Args:
    mref: 方法键；uid: 节点 uid；operand: 新操作数值。
Raises:
    StaleRefError: uid 不存在。
```

### mjt.insn_set_target

```
insn_set_target(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex, target_uid: typing.SupportsInt | typing.SupportsIndex) -> None

改写跳转目标 label。

Args:
    mref: 方法键；uid: jump 节点 uid；target_uid: 新目标 label uid。
Raises:
    StaleRefError: uid 或目标 label 不存在。
```

### mjt.insn_set_var

```
insn_set_var(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex, var: typing.SupportsInt | typing.SupportsIndex) -> None

改写局部变量索引（var 类节点）。

Args:
    mref: 方法键；uid: 节点 uid；var: 新局部变量槽位号。
Raises:
    StaleRefError: uid 不存在。
```

### mjt.insns

```
insns(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str]) -> list

列出方法体全部节点。

Args:
    mref: 方法键 (h, cls, name, desc)。
Returns:
    [(uid, opcode, kind)] 链表序列表（含 label/line/frame 节点；
    kind 名见模块 doc）。
Raises:
    StaleRefError: 方法不存在。
```

### mjt.jar_diff

```
jar_diff(h: typing.SupportsInt | typing.SupportsIndex, path: str) -> dict

会话容器与外部参考 JAR 的全资源字节比较。

Args:
    h: 容器句柄。
    path: 参考 jar 路径。
Returns:
    报告 dict：{reference_path, workspace_only, reference_only,
    modified, diffs}；计数口径——workspace_only 仅会话容器有、
    reference_only 仅参考有、modified 两侧都有但字节不同；diffs 按
    uri 升序，逐条目 {uri, before_size, after_size, ranges}，range 为
    单公共前后缀收缩 [{offset, before_length, after_length}]。
Raises:
    RuntimeError: 参考 jar 缺失或导入失败。
```

### mjt.jar_fix_analyze

```
jar_fix_analyze(h: typing.SupportsInt | typing.SupportsIndex, remove_invalid_class_entries: bool = False, remove_corrupt_output_markers: bool = False) -> dict

jar 修复只读扫描。

Args:
    h: 容器句柄。
    remove_invalid_class_entries: 是否选中无效类条目（.class 扩展名
        但类字节无效）。
    remove_corrupt_output_markers: 是否选中损坏输出标记条目（uri 含
        corrupt_output_stream 标记）。
Returns:
    报告 dict：{resource_count, class_count, issues, removed}；issue
    为 {uri, kind, message, size, selected}（kind ∈ invalid_class /
    invalid_class_entry / corrupt_output_marker），selected 按开关回填。
Raises:
    StaleRefError: 句柄失效。
```

### mjt.jar_fix_apply

```
jar_fix_apply(h: typing.SupportsInt | typing.SupportsIndex, remove_invalid_class_entries: bool = False, remove_corrupt_output_markers: bool = False) -> dict

jar 修复执行：同 jar_fix_analyze 扫描后移除全部选中条目。

Args:
    h: 容器句柄。
    remove_invalid_class_entries / remove_corrupt_output_markers:
        同 jar_fix_analyze 的选中开关。
Returns:
    报告 dict（同 jar_fix_analyze；removed 列出实际移除条目，索引与
    树缓存同步清理）。
Raises:
    StaleRefError: 句柄失效。
```

### mjt.last_insn

```
last_insn(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str]) -> int | None

取末个节点 uid（空方法体为 None）。
```

### mjt.last_run_report

```
last_run_report(h: typing.SupportsInt | typing.SupportsIndex) -> object

取该容器最近一次管线运行报告。

Args:
    h: 容器句柄。
Returns:
    RunStats；本会话尚未跑过管线时为 None。
```

### mjt.list_entries

```
list_entries(h: typing.SupportsInt | typing.SupportsIndex) -> list

容器全部条目清单。

Args:
    h: 容器句柄。
Returns:
    [(uri, 类型名)] 列表——含 class/MANIFEST 等非类资源。
```

### mjt.load_class_file

```
load_class_file(h: typing.SupportsInt | typing.SupportsIndex, path: str, uri: str | None = None, replace: bool = False) -> None

把磁盘散装 .class 文件装入容器。

Args:
    h: 容器句柄。
    path: .class 文件路径。
    uri: 条目 URI；缺省取文件名。
    replace: 同名条目已存在时是否覆盖；缺省抛错。
Raises:
    StaleRefError: URI 冲突且 replace=False，或文件解析失败。
```

### mjt.mapping_counts

```
mapping_counts(h: typing.SupportsInt | typing.SupportsIndex) -> tuple[int, int, int]

映射条目计数。

Args:
    h: 容器句柄。
Returns:
    (class, method, field) 计数三元组。
```

### mjt.mapping_dump

```
mapping_dump(h: typing.SupportsInt | typing.SupportsIndex, path: str, fmt: str = 'Enigma') -> None

导出改名映射 sidecar。

Args:
    h: 容器句柄（token 面或 rename_apply_suggestions 收进的映射）。
    path: 输出文件路径。
    fmt: Enigma / Tiny-V2 / Tiny-V1 / ProGuard / SRG / Simple / R8。
Raises:
    RuntimeError: 格式未知或写出失败。
```

### mjt.method_apply_jasm

```
method_apply_jasm(h: typing.SupportsInt | typing.SupportsIndex, cls: str, name: str, desc: str, source: str, source_name: str = '', validation: str = 'relaxed') -> dict

方法级 Jasm 全量替换（source 拼接回类全文后事务式汇编落容器）。

Args:
    h: 容器句柄。
    cls/name/desc: 目标方法坐标。
    source: 完整 .method 声明文本。
    source_name: 诊断定位用的源名；缺省 <name>.jasm。
    validation: 校验档位（同 class_validate_jasm）。
Returns:
    报告 dict（同 class_apply_jasm），成功后该类缓存树作废。
Raises:
    StaleRefError: 类或方法不存在。
    RuntimeError: 现类字节反汇编失败或事务落容器失败。
```

### mjt.method_apply_jasm_diff

```
method_apply_jasm_diff(h: typing.SupportsInt | typing.SupportsIndex, cls: str, name: str, desc: str, diff: str, validation: str = 'relaxed') -> dict

方法级 diff 应用：diff 相对 method_jasm 产物应用，拼接回类全文后
事务式汇编落容器。

Args:
    h: 容器句柄。
    cls/name/desc: 目标方法坐标。
    diff: 对 method_jasm 产物的 unified diff。
    validation: 校验档位（同 class_validate_jasm）。
Returns:
    报告 dict（同 class_apply_jasm），成功后该类缓存树作废。
Raises:
    StaleRefError: 类或方法不存在。
    RuntimeError: 现类字节反汇编失败或事务落容器失败。
```

### mjt.method_info

```
method_info(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str]) -> dict

读取方法声明信息。

Args:
    mref: 方法键 (h, cls, name, desc)。
Returns:
    {name, desc, access, max_stack, max_locals, exceptions}。
Raises:
    StaleRefError: 方法不存在。
```

### mjt.method_jasm

```
method_jasm(h: typing.SupportsInt | typing.SupportsIndex, cls: str, name: str, desc: str) -> dict

单方法反汇编为 Jasm 声明段。

Args:
    h: 容器句柄。
    cls/name/desc: 方法坐标。
Returns:
    {internal_name, name, descriptor, source}（source 为 .method 声明，
    与类全文内方法段逐字节一致；读落账字节，树编辑后先 flush）。
Raises:
    StaleRefError: 类或方法不存在。
```

### mjt.method_set_access

```
method_set_access(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], access: typing.SupportsInt | typing.SupportsIndex) -> None

改写方法访问标志（整字替换，位集对照 mjt.acc）。

Args:
    mref: 方法键 (h, cls, name, desc)。
    access: 新访问标志位集。
Raises:
    StaleRefError: 方法不存在。
```

### mjt.method_set_desc

```
method_set_desc(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], new_desc: str) -> tuple[int, str, str, str]

改写方法描述符。

Args:
    mref: 方法键 (h, cls, name, desc)。
    new_desc: 新 JVM 方法描述符。方法体不随之改写。
Returns:
    新 mref；旧 mref 随即失效。
Raises:
    StaleRefError: 方法不存在。
```

### mjt.method_set_exceptions

```
method_set_exceptions(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], exceptions: collections.abc.Sequence[str]) -> None

整表替换方法 throws 声明。

Args:
    mref: 方法键 (h, cls, name, desc)。
    exceptions: 异常类 internal name 列表（整表替换）。
Raises:
    StaleRefError: 方法不存在。
```

### mjt.method_set_max_locals

```
method_set_max_locals(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], v: typing.SupportsInt | typing.SupportsIndex) -> None

改写方法 max_locals。

Args:
    mref: 方法键 (h, cls, name, desc)。
    v: 新 max_locals 值。
Raises:
    StaleRefError: 方法不存在。
```

### mjt.method_set_max_stack

```
method_set_max_stack(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], v: typing.SupportsInt | typing.SupportsIndex) -> None

改写方法 max_stack。

Args:
    mref: 方法键 (h, cls, name, desc)。
    v: 新 max_stack 值。
Raises:
    StaleRefError: 方法不存在。
```

### mjt.method_validate_jasm

```
method_validate_jasm(h: typing.SupportsInt | typing.SupportsIndex, cls: str, name: str, desc: str, source: str, source_name: str = '', validation: str = 'relaxed') -> dict

方法级 Jasm 全量替换试汇编（source 拼接回类全文后在容器拷贝上验证）。

Args:
    h: 容器句柄。
    cls/name/desc: 目标方法坐标。
    source: 完整 .method 声明文本。
    source_name: 诊断定位用的源名；缺省 <name>.jasm。
    validation: 校验档位（同 class_validate_jasm）。
Returns:
    报告 dict（同 class_validate_jasm）。
Raises:
    StaleRefError: 类或方法不存在。
    RuntimeError: 现类字节反汇编失败。
```

### mjt.method_validate_jasm_diff

```
method_validate_jasm_diff(h: typing.SupportsInt | typing.SupportsIndex, cls: str, name: str, desc: str, diff: str, validation: str = 'relaxed') -> dict

方法级 diff 试汇编：diff 相对 method_jasm 产物应用，拼接回类全文后
在容器拷贝上验证，绝不落容器。

Args:
    h: 容器句柄。
    cls/name/desc: 目标方法坐标。
    diff: 对 method_jasm 产物的 unified diff。
    validation: 校验档位（同 class_validate_jasm）。
Returns:
    报告 dict（同 class_validate_jasm）。
Raises:
    StaleRefError: 类或方法不存在。
    RuntimeError: 现类字节反汇编失败。
```

### mjt.methods

```
methods(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str]) -> list

列出方法声明。

Args:
    cref: 类键 (h, 类名)。
Returns:
    [(name, desc, access)] 列表（声明序；desc 为 JVM 描述符，access
    为访问标志位集，可对照 mjt.acc）。
Raises:
    StaleRefError: 类不存在。
```

### mjt.new_container

```
new_container() -> int

新建空容器。

Returns:
    容器句柄 int（用完 close_container 释放）。
```

### mjt.next_insn

```
next_insn(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex) -> int | None

取后继节点 uid（无后继为 None）。
```

### mjt.open_jar

```
open_jar(path: str) -> int

打开 jar 并导入全部资源，返回新容器句柄。

Args:
    path: jar 文件路径。
Returns:
    容器句柄 int（后续全部操作显式传参；用完 close_container 释放，
    不调则进程退出兜底）。
Raises:
    ValueError: path 为空。
    RuntimeError: 路径不是文件或读取/解析失败（打开失败不泄漏句柄）。
    mjt.DeadlineExceeded: 协作层 deadline 越限。
```

### mjt.opt_apply

```
opt_apply(h: typing.SupportsInt | typing.SupportsIndex, pass_names: collections.abc.Sequence[str] = [], pipeline: str = '', jdk: str = '') -> mjt.RunStats

优化落盘：管线跑在会话容器上。

Args:
    h: 容器句柄。
    pass_names: pass 名列表（与 pipeline 互斥）。
    pipeline: 管线名；与 pass_names 均空时回落 default-optimize。
    jdk: JDK 路径（可选，跨类层次帧解析用）。
Returns:
    RunStats；成功后树缓存作废并登记最近运行报告。
Raises:
    ValueError: 选择器非法或 pass/管线名未知。
    RuntimeError: pass 运行失败。
```

### mjt.opt_preview

```
opt_preview(h: typing.SupportsInt | typing.SupportsIndex, pass_names: collections.abc.Sequence[str] = [], pipeline: str = '', jdk: str = '') -> dict

优化试算：管线跑在容器克隆上，diff 后丢弃副本——会话容器零改动。

Args:
    h: 容器句柄。
    pass_names: pass 名列表（与 pipeline 互斥）。
    pipeline: 管线名；与 pass_names 均空时回落 default-optimize。
    jdk: JDK 路径（可选，跨类层次帧解析用）。
Returns:
    {changed: bool, diffs: [{uri, before_size, after_size, ranges:
    [{offset, before_length, after_length}]}]}。
Raises:
    ValueError: 选择器非法或 pass/管线名未知。
    RuntimeError: pass 运行失败。
```

### mjt.parse_class

```
parse_class(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str]) -> tuple[int, str]

附着解析类（幂等，进树缓存）。

Args:
    cref: 类键 (h, 类名)。
Returns:
    同一 cref（链式书写用）。
Raises:
    StaleRefError: 类不存在或解析失败。
```

### mjt.pass_describe

```
pass_describe() -> dict

pass/管线目录（run_pipeline 可用面）。

Returns:
    {passes: [{name, description, scope}], pipelines: [{name, description}]}；scope ∈ method/class/global。
```

### mjt.passes

```
passes() -> list[str]

列出可用 pass 名（run_pass / run_pipeline 的 pass_names 取值面）。

Returns:
    list[str]，pass 名列表。
```

### mjt.preserved_all

```
preserved_all() -> mjt.PreservedAnalyses

MethodPass.run 返回值：报告本 pass 无改动（全部保留）。
```

### mjt.prev_insn

```
prev_insn(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex) -> int | None

取前驱节点 uid（无前驱为 None）。
```

### mjt.remove_entry

```
remove_entry(h: typing.SupportsInt | typing.SupportsIndex, uri: str) -> None

删除条目。

Args:
    h: 容器句柄。
    uri: 条目 URI。索引与树缓存同步清理；同名落选类按序晋升。
Raises:
    StaleRefError: 条目不存在。
```

### mjt.remove_field

```
remove_field(fref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str]) -> None

删除字段（摘链并销毁；fref 随即失效）。

Args:
    fref: 字段键 (h, cls, name, desc)。
Raises:
    StaleRefError: 字段不存在。
```

### mjt.remove_insn

```
remove_insn(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], uid: typing.SupportsInt | typing.SupportsIndex) -> None

删除节点（摘链进墓地；uid 后续解析 fail-closed）。

Args:
    mref: 方法键。
    uid: 待删节点 uid。被删 uid 不复用；引用它的跳转目标由脚本负责
    重绑。
Raises:
    StaleRefError: uid 不存在（含已删）。
```

### mjt.remove_method

```
remove_method(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str]) -> None

删除方法（摘链并销毁；mref 与其全部指令 uid 随即失效）。

Args:
    mref: 方法键 (h, cls, name, desc)。
Raises:
    StaleRefError: 方法不存在。
```

### mjt.remove_try_catch

```
remove_try_catch(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], row_index: typing.SupportsInt | typing.SupportsIndex) -> None

按行号删除异常表行。

Args:
    mref: 方法键。
    row_index: try_catches 序中的行号（行号随删行前移，批量删建议
        降序进行）。
Raises:
    StaleRefError: 行号越界。
```

### mjt.rename_apply_suggestions

```
rename_apply_suggestions(h: typing.SupportsInt | typing.SupportsIndex, path: str, policy: str = 'all') -> tuple[int, int, int, int, list]

批量改名：读建议文件，引擎全量裁决并应用。

Args:
    h: 容器句柄。
    path: 建议文件路径（v1 schema：proposals 列表，条目含 kind/name/
        newName/confidence）。
    policy: 改写范围——bytecode-only / bytecode-reflect /
        bytecode-metadata / all。
Returns:
    (proposed, accepted, rejected, renamed_members, rejected_entries)；
    rejected_entries = [{kind, owner, name, desc, new_name, source,
    reason, detail}]（fail-closed 报错面：撞名条目自含 owner/old/new
    与撞名对象定位）。

成功后映射收进 rename token 面（mapping_dump/mapping_counts 直接
可用），缓存树作废重建索引。
```

### mjt.rename_begin

```
rename_begin(h: typing.SupportsInt | typing.SupportsIndex) -> None

重置 rename 会话（收计划前调用；丢弃未提交计划）。

Args:
    h: 容器句柄。
```

### mjt.rename_class

```
rename_class(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str], new_name: str) -> tuple[int, str]

类改名并迁移索引。

Args:
    cref: 类键 (h, 旧类名)。
    new_name: 新 internal name。
Returns:
    新 cref；旧 cref 随即失效。只改类自身身份，引用重写走 rename
    token 面；条目 URI 搬移在写回/导出边界落账（sync_entries）。
Raises:
    StaleRefError: 旧类不存在或新名已占用。
```

### mjt.rename_commit

```
rename_commit(h: typing.SupportsInt | typing.SupportsIndex, reflection_strings: bool = False, annotations: bool = False, signatures: bool = False, resources: bool = True) -> None

提交改名计划到容器（字节码引用重写恒开）。

Args:
    h: 容器句柄。
    reflection_strings: 是否重写字符串形态的反射引用（Class.forName
        等）。
    annotations: 是否重写注解内的类型引用。
    signatures: 是否重写泛型签名。
    resources: 是否迁移条目 URI 并重写 MANIFEST/services（缺省开——
        导出 jar 新类不该挂旧 URI）。

提交前自动 flush 全部脏树（compute_frames=false，remap 保帧）；成功后
全部缓存树作废。
Raises:
    RuntimeError: 提交失败。
    mjt.DeadlineExceeded: 协作层 deadline 越限。
```

### mjt.rename_field

```
rename_field(fref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], new_name: str) -> tuple[int, str, str, str]

字段改名。

Args:
    fref: 字段键 (h, cls, name, desc)。
    new_name: 新字段名。
Returns:
    新 fref；旧 fref 随即失效。
Raises:
    StaleRefError: 字段不存在或新名冲突。
```

### mjt.rename_method

```
rename_method(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str], new_name: str) -> tuple[int, str, str, str]

方法改名。

Args:
    mref: 方法键 (h, cls, name, desc)。
    new_name: 新方法名。
Returns:
    新 mref；旧 mref 随即失效。只改声明，调用点重写走 rename token 面。
Raises:
    StaleRefError: 方法不存在或新名冲突。
```

### mjt.run_pass

```
run_pass(h: typing.SupportsInt | typing.SupportsIndex, name: str, jdk: str = '') -> mjt.RunStats

按名运行 mjbase 内置单 pass。

Args:
    h: 容器句柄。
    name: pass 名（mjt.passes() 列出可用面）。
    jdk: JDK 路径——pass 需要跨类层次解析帧时给出；空串跳过。
Returns:
    RunStats（同时登记为该容器最近运行报告）。管线直驱容器字节，
    成功后该容器全部缓存树作废。
Raises:
    ValueError: pass 名未注册。
    RuntimeError: pass 运行失败。
```

### mjt.run_pipeline

```
run_pipeline(h: typing.SupportsInt | typing.SupportsIndex, pass_names: collections.abc.Sequence[str] = [], pipeline: str = '', jdk: str = '') -> mjt.RunStats

依序运行多个 pass，或按名运行整条管线。

Args:
    h: 容器句柄。
    pass_names: pass 名列表（与 pipeline 互斥，两者必须给其一，
        无回落）。
    pipeline: 管线名（mjt.pass_describe() 列出可用面）。
    jdk: JDK 路径（可选，跨类层次帧解析用）。
Returns:
    RunStats；成功后树缓存作废并登记最近运行报告。
Raises:
    ValueError: pass_names 与 pipeline 同时给出、均空、或名字未知。
    RuntimeError: pass 运行失败。
```

### mjt.sanitize_lvt

```
sanitize_lvt(h: typing.SupportsInt | typing.SupportsIndex) -> tuple[int, int, int]

LVT 清洗：剔除脱链/乱序局部变量表条目并去重。

Args:
    h: 容器句柄（全容器逐类逐方法；改动标脏）。
Returns:
    (dropped, deduped, classes) 计数三元组。
```

### mjt.save

```
save(h: typing.SupportsInt | typing.SupportsIndex, path: str, compute_frames: bool = True) -> None

落盘 jar：flush 全脏 + 两阶段写文件；未解析/未脏类字节原样导出。

Args:
    h: 容器句柄。
    path: 输出 jar 路径。
    compute_frames: flush 时是否重算 StackMapTable（缺省 True）。
Raises:
    RuntimeError: 写出失败。
    mjt.DeadlineExceeded: 协作层 deadline 越限。
```

### mjt.set_class_bytes

```
set_class_bytes(h: typing.SupportsInt | typing.SupportsIndex, name: str, bytes: bytes) -> None

写回/新增类字节。

Args:
    h: 容器句柄。
    name: internal name；不存在则以 name + ".class" 为 URI 新增条目。
    bytes: 类字节。覆盖时缓存树作废重解析。
Raises:
    StaleRefError: 句柄失效或字节头解析不出类名。
```

### mjt.set_deadline_ms

```
set_deadline_ms(ms: typing.SupportsInt | typing.SupportsIndex) -> None

设置整体 deadline（毫秒，从现在起）。

Args:
    ms: 大于 0 生效；小于 0 立即过期（测试位）；0 清除。

facade 长操作（import/flush/verify/sanitize/run_pass 等检查点）越限
抛 mjt.DeadlineExceeded（KeyboardInterrupt 子类）。嵌入宿主另有逐行
trace 与 watchdog 硬层，见 doc_ai/pysdk-script-limits.md。
```

### mjt.set_entry_bytes

```
set_entry_bytes(h: typing.SupportsInt | typing.SupportsIndex, uri: str, bytes: bytes) -> None

替换条目字节。

Args:
    h: 容器句柄。
    uri: 条目 URI。
    bytes: 新字节。
Raises:
    StaleRefError: 条目不存在。类条目覆盖后缓存树作废重解析；
    manifest 条目重析；其他资源直换。
```

### mjt.shadow_dump

```
shadow_dump(h: typing.SupportsInt | typing.SupportsIndex, path: str) -> None

会话 shadow 表写出 sidecar（v2 JSON，token 列随行）。
```

### mjt.shadow_entries

```
shadow_entries(h: typing.SupportsInt | typing.SupportsIndex) -> list

列出会话 shadow 表全条目。

Args:
    h: 容器句柄。
Returns:
    [(kind, owner, desc, raw, shadow, token8)] 列表；kind ∈
    class/field/method；空表（未生成/未载入）为空列表。
```

### mjt.shadow_generate

```
shadow_generate(h: typing.SupportsInt | typing.SupportsIndex) -> tuple[int, int, int, int]

机械占位名批量生成（幂等：已登记条目保留，覆盖会话 shadow 表）。

Args:
    h: 容器句柄。
Returns:
    (classes, methods, fields, kept) 计数四元组；token 补号随行。
Raises:
    RuntimeError: 生成失败。
```

### mjt.shadow_generate_file

```
shadow_generate_file(path: str, file: str = '') -> dict

shadow sidecar 生成（文件面，不依赖打开容器）。

Args:
    path: jar 路径。
    file: sidecar 输出路径；缺省 `<jar>.mjt-shadow`，显式给出时不得
        指向 jar 本身。
Returns:
    报告 dict：{file, assigned_classes, assigned_methods,
    assigned_fields, kept_entries, removed, total_entries}；幂等回填——
    已登记条目保留，只写 sidecar 永不改 jar。
Raises:
    RuntimeError: jar 缺失、file 指向 jar 本身或写出失败。
```

### mjt.shadow_load

```
shadow_load(h: typing.SupportsInt | typing.SupportsIndex, path: str) -> int

载入 shadow sidecar（严格 schema，整表替换会话表）。

Args:
    h: 容器句柄。
    path: sidecar 文件路径。
Returns:
    条目数。
Raises:
    RuntimeError: 文件缺失或 schema 不符。
```

### mjt.shadow_prune_file

```
shadow_prune_file(path: str, file: str = '') -> dict

shadow sidecar 清理（文件面）：剔除 jar 中已不存在的条目。

Args:
    path: jar 路径。
    file: sidecar 路径；缺省 `<jar>.mjt-shadow`。
Returns:
    报告 dict：{file, removed, total_entries}；removed == 0 不回写
    文件。
Raises:
    RuntimeError: sidecar 缺失或 jar 缺失。
```

### mjt.sync_entries

```
sync_entries(h: typing.SupportsInt | typing.SupportsIndex) -> list[str]

落账全部挂账的类改名条目搬移（老 URI 字节 → 新 URI + 索引迁移）。

Args:
    h: 容器句柄。write_back/flush/save/export 出口自动调用，也可显式调。
Returns:
    ["old -> new"] 迁移清单（无挂账为空表）。
```

### mjt.token_rename_class

```
token_rename_class(h: typing.SupportsInt | typing.SupportsIndex, old_name: str, new_name: str) -> None

类改名入计划（不立即生效，rename_commit 时带引用完整性整体应用）。

Args:
    h: 容器句柄。
    old_name: 旧类名（点分或 internal 均可，内部规范化）。
    new_name: 新类名（同上）。
Raises:
    RuntimeError: 未先 rename_begin，或改名无法落实。
```

### mjt.token_rename_field

```
token_rename_field(h: typing.SupportsInt | typing.SupportsIndex, owner: str, name: str, desc: str, new_name: str) -> None

字段改名入计划（rename_commit 时连同全部引用点重写）。

Args:
    h: 容器句柄。
    owner/name/desc: 目标字段坐标。
    new_name: 新字段名。
```

### mjt.token_rename_method

```
token_rename_method(h: typing.SupportsInt | typing.SupportsIndex, owner: str, name: str, desc: str, new_name: str) -> None

方法改名入计划（rename_commit 时连同全部调用点重写）。

Args:
    h: 容器句柄。
    owner/name/desc: 目标方法坐标（owner 为 internal name）。
    new_name: 新方法名。
```

### mjt.try_catches

```
try_catches(mref: tuple[typing.SupportsInt | typing.SupportsIndex, str, str, str]) -> list

列出异常表。

Args:
    mref: 方法键 (h, cls, name, desc)。
Returns:
    [(start_uid, end_uid, handler_uid, catch_type)] 行列表（catch_type
    为空串表示 catch-all）。
Raises:
    StaleRefError: 方法不存在。
```

### mjt.verify_container

```
verify_container(h: typing.SupportsInt | typing.SupportsIndex) -> dict

容器完整性体检。

Args:
    h: 容器句柄。
Returns:
    {match, mismatch, malformed, dep_missing, non_class} 计数 +
    {mismatch_details, malformed_details, dep_missing_details} 明细
    [(entry, detail)]。口径：逐 class 条目解析（malformed）、entry 路径
    与 this_class 声明比对（mismatch）、常量池引用的容器外非 JDK 类清点
    （dep_missing）；只读不改动。
```

### mjt.write_back

```
write_back(cref: tuple[typing.SupportsInt | typing.SupportsIndex, str], compute_frames: bool = True) -> None

单类写回：树 → 字节 + 清脏。

Args:
    cref: 类键 (h, 类名)。
    compute_frames: 是否重算 StackMapTable（缺省 True；写不出即抛，
        兼具结构/栈校验语义）。
Raises:
    RuntimeError: COMPUTE_FRAMES 写出失败（方法体结构/栈不一致）。
    mjt.DeadlineExceeded: 协作层 deadline 越限。
```

### mjt.write_demo_jar

```
write_demo_jar(path: str) -> None

写出合成混淆形态演示 jar（unittest/上手演示用；零样本字节入库）。

Args:
    path: 输出 jar 路径。

罐内 5 个类：me/demo/{Api,App,Color,Crypto,Util}，带假 handler、
恒假谓词、字符串解密调用点等可辨形态。
```

### mjt.DeadlineExceeded

```
class DeadlineExceeded
```
#### mjt.DeadlineExceeded.add_note

```
Add a note to the exception
```

#### mjt.DeadlineExceeded.with_traceback

```
Set self.__traceback__ to tb and return self.
```

### mjt.MethodPass

```
class MethodPass

自定义 pass 类型锚（Python 侧继承）：子类实现 run(mref)，返回 mjt.changed() / mjt.preserved_all() 报告是否改动。适配到具体管线的 实现在私有侧，基座面只保类型形状。
```
### mjt.PreservedAnalyses

```
class PreservedAnalyses

MethodPass.run 的返回锚（不透明值，不可构造）。
```
### mjt.RunStats

```
class RunStats

单次管线/pass 运行的统计报告（mjt.last_run_report 取最近一次）。
```
属性：

- `pipeline`：运行的管线名或单 pass 名。
- `slots`：逐 pass 槽位列表 [{name, scope, processed, changed, failed, duration_ms}]；scope ∈ method/class/global。

### mjt.StaleRefError

```
class StaleRefError
```
#### mjt.StaleRefError.add_note

```
Add a note to the exception
```

#### mjt.StaleRefError.with_traceback

```
Set self.__traceback__ to tb and return self.
```


## mjt.util（Python 层）

### mjt.util.call_sites

```
call_sites(h, owner=None, name=None, desc=None)

定点普查方法调用点：全罐 invokestatic/virtual/special/interface 按
owner/name/desc 过滤（None=通配）。

invokedynamic 不在普查面（indy 无 owner 键，站点形态走 decryptor_sites
的 indy-shape）。解密器已定位时的精确路径入口：启发式 decryptor_sites
漏检的 str 键解密器靠这里抓全。

Args:
    h: 容器句柄。
    owner: 被调方类（internal name，None=通配）。
    name: 被调方方法名（None=通配）。
    desc: 被调方方法描述符（None=通配）。

Returns:
    CallSite 列表（链表序；cls/method 是调用方坐标，owner/name/desc 是
    被调方坐标）。
```

### mjt.util.classes_where

```
classes_where(h, **pred)

按谓词惰性过滤容器内类。

Args:
    h: 容器句柄。
    **pred: 过滤谓词。name / not_name：点分名 glob（``*`` / ``?``）；
        where：callable(internal_name) 再过滤。

Returns:
    [(h, 类名)] cref 列表（class_names 升序序）。
```

### mjt.util.cleanliness

```
cleanliness(h, mref)

统计方法体清洁度计数（指令数/handler 数/分支数/常量压栈数/调用数）。

Args:
    h: 容器句柄。
    mref: 方法键 (h, cls, name, desc)。

Returns:
    Cleanliness（与其他 Cleanliness 相减得 diff）。
```

### mjt.util.clone_method

```
clone_method(h, mref, new_name)

同类克隆方法 → 新 mref（新 name、同 desc/access/max_stack/max_locals/
exceptions；指令体按 uid 序逐条 emit_* 重建，try 行按 label 映射复制）。

限制（指令面 emit 原语边界，预扫先于构建、不落半成品）：
- tableswitch/lookupswitch/indy/multianewarray 无法重建（缺 emit_switch/
  emit_indy/emit_multianewarray 原语）；newarray 同理（缺 emit_int 原语，
  ldc 无数组等位）→ NotImplementedError；
- bipush/sipush 重建为等值 int ldc（emit_insn 只能造零操作数 InsnNode，
  ldc 与之栈效果/值等价；字节形态有别——javap 可见 ldc）；
- ldc 只可复制 int（cst_tag 2）与 str（cst_tag 1，绑定层 intern 进会话
  arena，按值安全）；long/float/double（3/4/5）因 emit_ldc 无显式宽度
  控制（py 值形态决定 tag，宽度必然漂移）→ NotImplementedError；
- LineNumberTable / StackMapTable 不复制（line/frame 节点跳过；write_back
  COMPUTE_FRAMES 重算帧）；
- 前向跳转先暂绑既有 label（无前序 label 时落占位），目标 label 生成后
  insn_set_target 重绑；占位 label 是孤儿节点，ClassWriter 不为其产出字节。

Args:
    h: 容器句柄（须与 mref[0] 一致）。
    mref: 源方法键 (h, cls, name, desc)。
    new_name: 克隆方法名（同名同描述符冲突 → add_method 抛错 fail-closed）。

Returns:
    新方法 mref (h, cls, new_name, desc)；克隆体不继承 LVT。

Raises:
    NotImplementedError: 源体含无 emit 原语的形态（见上限制）。
    ValueError: 跳转目标或 try 行引用方法体外 label。
```

### mjt.util.decryptor_sites

```
decryptor_sites(h)

启发式普查解密器形态站点：返回 String 的 invokestatic（前至少 2 条
常量 int 载荷）与 invokedynamic 调用点；管线前后差分即残留台账。

刻意保持 int 校准（普查口径稳定，勿随折叠门放宽）：str 键解密器
（(String,int)→String 形态）会漏检，精确路径走 call_sites 定点坐标 +
fold_call_sites——解密器已定位时按 (owner, name, desc) 全量回填，
不依赖启发式。

Args:
    h: 容器句柄。

Returns:
    DecryptSite 列表（全罐扫描序）。
```

### mjt.util.dry_run

```
dry_run(h, cref, fn, method=None)

在容器分叉副本上试跑 fn(mref) + verify 门，父容器不动。

verify 门 = 结构闸（每方法体终结指令收尾）+ COMPUTE_FRAMES 写出；
满意再对返回的 Draft 调 .apply() 落地。fn 返回 True 表示有改动
（fn 返回 True 表示有改动，与管线 pass 的 changed 语义一致）。

Args:
    h: 容器句柄。
    cref: 试算类 (h, 类名)。
    fn: callable(mref)；对分叉副本的每个方法调用。
    method: "名(描述符)" 串——只跑匹配的方法；None 对全类方法跑。

Returns:
    DryRunDraft（verify 结论 + 清洁度 diff + apply()）。
```

### mjt.util.dump_sites

```
dump_sites(sites, path)

站点列表落 TSV 文件（表头行 + 逐站 row 行）。

Args:
    sites: DecryptSite 列表。
    path: 输出文件路径。
```

### mjt.util.fake_handlers

```
identity_rethrows(h, mref, *, allow_overlap=False)

普查恒等 rethrow 假 handler 组（保守门，非保守形态一律不报）。

保守门：非 catch-all、handler 体不含 monitor/jsr、无 jump 入体、
前驱非顺序落入、恒等包装（签名单参同返回，或方法体已验证为
``aload 0; areturn`` 的父类擦除型）、组间 try 区间不交叠。

Args:
    h: 容器句柄。
    mref: 方法键 (h, cls, name, desc)。
    allow_overlap: 跳过组间重叠门，返回"除交叠外全过门"的组，供链式
        成组删除——交叠组要在调用方按区间关系连成链、整链一起判一起删；
        此时返回的组彼此可能交叠，单独删任意一组都会破坏字节码。

Returns:
    HandlerRef 列表（每组共享同一 handler 体）。
```

`fake_handlers` 是 `identity_rethrows` 的别名（同一函数对象）。

### mjt.util.fold_call_sites

```
fold_call_sites(h, callee, value_of)

定点折叠：callee=(owner, name, desc) 的全部调用点回填为明文 ldc。

与 fold_decryptor_sites 的启发式普查路径互补：实参常量值按声明序组成
元组交 value_of(args) 求明文——脚本侧就地解密（xor/查表都行），解密器
本体随后通常一并删除。str 键解密器（启发式漏检形态）的正式出路。

门与 fold_to_ldc 同款 fail-closed：带 receiver 的调用点（非 invokestatic）
存在即拒（callee 坐标对到实例方法是调用方错误，静默空转会藏错）；实参
非纯常量压栈即拒，不落半成品。

Args:
    h: 容器句柄。
    callee: 被折方法坐标 (owner, name, desc)。
    value_of: callable(args) → 明文值或 None（None=该站跳过）。

Returns:
    [(site, ldc_uid)]（跳过的不入）；同方法内按 uid 降序折叠（防御序，
    多站互不踩位）。

Raises:
    ValueError: callee 存在带 receiver 的调用点，或某站实参非常量压栈 /
        载荷不足。单站拒绝原样抛，已折站保持已折、后续站不落——调用方
        按台账自行重放。
```

### mjt.util.fold_decryptor_sites

```
fold_decryptor_sites(h, value_of, sites=None)

残留解密站点批量回填（启发式普查路径的 util 批口；uid 先统一解析
再折叠，结构编辑下稳定、多站互不踩位）。

Args:
    h: 容器句柄。
    value_of: callable(site) → 明文值或 None（None=该站跳过——信任哪些
        站点由脚本决定，安全门是可选项不是引擎强制门）。
    sites: 站点列表；None 时取 residual_sites(h) 现查。

Returns:
    [(site, ldc_uid)]（跳过的不入）。

Raises:
    ValueError: 单站回填失败原样抛（fold_to_ldc fail-closed 语义），
        已折站点保持已折、后续站点不落——调用方按台账自行重放。
```

### mjt.util.fold_to_ldc

```
fold_to_ldc(h, mref, call_uid, value)

把一个解密调用点回填为明文常量：call_uid 处的调用换成 ldc value，
其紧邻的常量实参载荷一并删除；返回新 ldc uid。

栈语义（栈机没有 dst，回填=栈产物等位顶替）：调用点的返回值由 ldc 等位
顶替，调用结果的消费方无需任何改写（消费方只认栈顶类型；value 类型须与
调用点返回类型一致，由脚本保证，write_back COMPUTE_FRAMES 兜底校验）。
实参必须是纯常量压栈（iconst/bipush/sipush/任意 ldc 载荷）——来自局部
变量或表达式的实参无法在指令面安全摘除：抛 ValueError 且不落任何改动
（先校验后变更，fail-closed 无半成品）。

str 载荷按值安全：绑定层将 str intern 进会话 arena（与树内字符串同源
同寿命），写出的类字节确定；int/float 直传 emit_ldc 不受影响。

Args:
    h: 容器句柄（须与 mref[0] 一致）。
    mref: 方法键 (h, cls, name, desc)。
    call_uid: 解密调用点 uid；invokestatic / invokedynamic，带 receiver
        的 invokevirtual/invokespecial 拒绝（receiver 无法等位重绑）。
    value: 明文常量（int/float/str；py float 落 double ldc）。

Returns:
    新 ldc 的 uid（结构编辑下稳定，可用于后续定位）。

Raises:
    ValueError: uid 不是调用点、实参非常量压栈或载荷不足；校验全过才
        变更，fail-closed 无半成品。调用点与实参 uid 随即失效（remove
        墓地 fail-closed）。
```

### mjt.util.guard_patterns

```
guard_patterns(h, mref)

普查恒假谓词分支（保守子集：iconst_0+ifne / iconst_1+ifeq 直连形态）。

Args:
    h: 容器句柄。
    mref: 方法键 (h, cls, name, desc)。

Returns:
    GuardRef 列表（分支 uid + 恒值载荷 uid）。
```

### mjt.util.identity_rethrows

```
identity_rethrows(h, mref, *, allow_overlap=False)

普查恒等 rethrow 假 handler 组（保守门，非保守形态一律不报）。

保守门：非 catch-all、handler 体不含 monitor/jsr、无 jump 入体、
前驱非顺序落入、恒等包装（签名单参同返回，或方法体已验证为
``aload 0; areturn`` 的父类擦除型）、组间 try 区间不交叠。

Args:
    h: 容器句柄。
    mref: 方法键 (h, cls, name, desc)。
    allow_overlap: 跳过组间重叠门，返回"除交叠外全过门"的组，供链式
        成组删除——交叠组要在调用方按区间关系连成链、整链一起判一起删；
        此时返回的组彼此可能交叠，单独删任意一组都会破坏字节码。

Returns:
    HandlerRef 列表（每组共享同一 handler 体）。
```

### mjt.util.init_order

```
init_order(h, roots=None)

静态初始化依赖拓扑序：按依赖序重放 <clinit> 的分析路径入口。

节点 = 容器内带 <clinit> 的类；边两条来源：(1) 超类边——子类初始化隐式
触发超类初始化，沿 class_info 的 super 链上溯，链上带 <clinit> 的容器内
祖先都是依赖；(2) clinit 引用边——<clinit> 的 method jasm 文本抽
``a/b/C`` 形态类名（过滤容器内已知类名集），被引用类带 <clinit> 即成边，
自身引用排除。

Args:
    h: 容器句柄。
    roots: 只排子图时给类名列表——roots 及其沿依赖边的传递闭包参与
        排序，闭包外类不进 order/cycles；无 <clinit> 或不在容器内的
        root 忽略。None 对全容器排序。

Returns:
    dict：
        order     类名列表，被依赖者在前；依赖环的下游类仍在 order，
                  位置在环之后
        cycles    [[同 SCC 类名…]…] 强连通环组，成员不进 order，
                  重放时任选起点
        no_clinit 无 <clinit> 的类列表（供参考，不参与排序；恒为容器
                  全量口径，不随 roots 收缩）
    同位次按类名出稳定序（迭代 Tarjan，无递归深度风险）。

口径与坑：jasm 面读容器落账字节，add_method/class_set_super 等树编辑
须先 flush/write_back 再调；引用边是文本级保守近似（字符串 ldc 里的
类名、编译期常量 getstatic 等不触发初始化的引用也计边）——重放序只会
偏保守不会漏依赖；单类 jasm 反汇编失败按无引用边计（超类边仍在），
不让一个坏类毒化全图。
```

### mjt.util.is_obfuscated_name

```
is_obfuscated_name(name)

启发式判断混淆名（1-2 个小写字母 + 至多两位数字，或 ab/abc 形态）。

<init>/<clinit> 恒为 False。
```

### mjt.util.methods_where

```
methods_where(h, cref, **pred)

按谓词过滤类内方法。

Args:
    h: 容器句柄。
    cref: 类键 (h, 类名)。
    **pred: 过滤谓词。name / not_name：方法名 glob；name_in /
        name_not_in：名字集合；static / abstract / native /
        has_handlers：布尔存在性过滤（缺省不查该项）；where：
        callable(mref) 再过滤。

Returns:
    mref (h, cls, name, desc) 列表（声明序）。
```

### mjt.util.native_table

```
native_table(h, cls='*', name='*', desc='*')

扫描容器内全部 native 方法生成数据表（trace/hook 点位底表）。

组合 classes_where + methods_where(native=True)：类名按点分名 glob、
方法名与描述符按 glob 过滤，全缺省即全量清单。

Args:
    h: 容器句柄。
    cls: 类名过滤，点分名 glob（``*`` / ``?``），如 ``com/example/*``。
    name: 方法名 glob。
    desc: 方法描述符 glob，如 ``()V``、``*Ljava/lang/String;*``。

Returns:
    行字典列表，按 (class, name, descriptor) 升序：
    {"class": 内部名, "name": 方法名, "descriptor": 描述符,
     "static": 是否静态, "access": 原始 access 掩码}。
```

### mjt.util.reset_semantic_names

```
reset_semantic_names()

清零 semantic_name 的类/方法计数器。
```

### mjt.util.residual_sites

```
residual_sites(h)

管线跑完后的残留解密站点（逐站附 shape 判定原因）。

Args:
    h: 容器句柄。

Returns:
    DecryptSite 列表（decryptor_sites 同口径现查）。
```

### mjt.util.sanitize_lvt

```
sanitize_lvt(h)

LVT 清洗：剔除 start/end 标签已脱链（墓地/删节点）与乱序条目、
按 (index, start, end, name, desc) 去重。

Args:
    h: 容器句柄。

Returns:
    (dropped, deduped, classes) 计数三元组。
```

### mjt.util.save_verified

```
save_verified(h, path)

落盘校验：save(compute_frames=True) 后重开逐类解析复核。

Args:
    h: 容器句柄。
    path: 输出 jar 路径。

Returns:
    报告 dict：path / classes（重开可见类数）/ skipped（解析失败类名
    列表，应为空）。
```

### mjt.util.semantic_name

```
semantic_name(cls=None, m=None)

占位语义名生成（RenamedClass<n> / renamedMethod<n>；策略可整体替换：
接 LLM/词典后改这里即可）。
```

### mjt.util.string_census

```
string_census(h, min_len=1)

全罐 ldc 字符串直方图（按出现次数降序）。

Args:
    h: 容器句柄。
    min_len: 只统计长度不小于该值的字符串（默认 1）。

Returns:
    StringCensus。
```

### mjt.util.CallSite

```
class CallSite

方法调用点（call_sites 产物）。

字段：cls/method 调用方类与 "名(描述符)" 拼接串；uid 调用点 uid
（结构编辑下稳定，可直接喂 fold_to_ldc）；mnemonic 调用指令
（invokestatic/virtual/special/interface）；owner/name/desc 被调方
坐标（owner 为 internal name）。
```
字段：

- `cls`：str
- `method`：str
- `uid`：int
- `mnemonic`：str
- `owner`：str
- `name`：str
- `desc`：str

### mjt.util.Cleanliness

```
class Cleanliness

方法体清洁度计数（五项字段）；支持相减得差值。
```
字段：

- `insns`：int
- `handlers`：int
- `branches`：int
- `const_loads`：int
- `calls`：int

### mjt.util.DecryptSite

```
class DecryptSite

解密器形态站点（decryptor_sites/residual_sites 产物）。

字段：cls/method 站点所在类与 "名(描述符)" 拼接串；index 方法体链表
序位次（mjt.insns(mref)[index] 取 uid）；mnemonic 调用形态
（invokestatic/invokedynamic）；reason 形态判定（direct-shape/
indy-shape）。row() 给 TSV 行。
```
字段：

- `cls`：str
- `method`：str
- `index`：int
- `mnemonic`：str
- `reason`：str

#### mjt.util.DecryptSite.row

```
row(self)

TSV 行（tab 分隔五字段：cls/method/index/mnemonic/reason）。
```

### mjt.util.DryRunDraft

```
class DryRunDraft

dry_run 试算结果（满意再调 apply() 落地）。

verify_ok / verify_error：结构闸 + COMPUTE_FRAMES 写出校验结论；
changed：fn 是否报告了改动；diff：方法名 → Cleanliness 差值 dict
（verify 未过时为 None）。
```
字段：

- `verify_ok`：bool
- `verify_error`：str
- `changed`：bool
- `diff`：object

属性：

- `summary`：一行结论（verify 结论 + 是否有改动）。

#### mjt.util.DryRunDraft.apply

```
apply(self)

试算结果落地：整类字节换入父容器（树缓存随之作废重解析）。

Raises:
    RuntimeError: verify 未过（拒绝落地）。
```

### mjt.util.GuardRef

```
class GuardRef

恒假谓词分支（guard_patterns 产物；remove() 做保守改写）。

字段：h 容器句柄；mref 方法键 (h, cls, name, desc)；branch 恒假分支
指令 uid（ifeq/ifne）；const 恒值载荷 uid（iconst_0/1）。
```
字段：

- `h`：int
- `mref`：tuple
- `branch`：int
- `const`：int

#### mjt.util.GuardRef.remove

```
remove(self)

恒假分支保守改写：载荷 iconst_0/1 改 pop（消掉栈压入）+ 分支指令删除。

注意：跳转目标块若仅由该分支到达会变成不可达——写回端
COMPUTE_FRAMES 校验兜底（dry_run/verify 场景下必过闸），
自由使用前请先 dry_run。
```

### mjt.util.HandlerRef

```
class HandlerRef

一组共享同一 handler 体的恒等 rethrow 行（try 行 uid 四元组 + handler 体）。
```
字段：

- `h`：int
- `mref`：tuple
- `rows`：list
- `body`：list

#### mjt.util.HandlerRef.remove

```
remove(self)

删除本组假 handler：handler 体指令逆序摘除 + 对应 try 行删除。

handler label 节点保留为孤儿标记（写回端不产出字节）；try 行按
uid 四元组反查删除，键寻址无悬垂。
```

### mjt.util.StringCensus

```
class StringCensus

全罐 ldc 字符串直方图（条目在 pairs: [(str, count)]，按出现次数降序；
len() 是总条目数，top(n) 取前 n 条）。
```
字段：

- `pairs`：list

#### mjt.util.StringCensus.top

```
top(self, n=20)

取前 n 条（默认 20）。
```


