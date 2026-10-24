# pysdk 常用对象形状

## 装载与签名自省（先读这节）

mjt_util.py 是宿主在 mjt 模块初始化时 exec 进 mjt.util 的片段，`mjt` 由装载方注入其 globals；脚本里一律 `from mjt import util as U`，`import mjt_util` 会 NameError——该文件不是独立模块，不在 sys.modules 里。

pybind 绑定函数没有 `__signature__`，`inspect.signature()` 直接抛 TypeError；签名在 `__doc__` 里（首行即调用形态），`print(mjt.class_info.__doc__)` 可读。

## 键面

容器句柄 h 是 int；cref=(h, 类名)、mref=fref=(h, 类名, 成员名, 描述符)。类名一律 internal name（`a/b/C` 形态，非点分；点分名只在 token_rename_* 入口参数里被规范化接受）。StaleRefError 表示键失效（容器已关、类已改名、指令已删），不是参数类型错误。

## CallSite（U.call_sites(h, owner=None, name=None, desc=None)）

| 字段 | 含义 |
|------|------|
| cls | 调用方类（internal name） |
| method | 调用方方法 `"名(描述符)"` 拼接串，如 `main([Ljava/lang/String;)V`；拆名用 `method.index("(")` 切 |
| uid | 调用点指令 uid（可直接喂 `mjt.insn_detail` / `U.fold_to_ldc`） |
| mnemonic | `invokestatic` / `invokevirtual` / `invokespecial` / `invokeinterface` |
| owner | **被调方**类 |
| name | **被调方**方法名 |
| desc | **被调方**方法描述符 |

调用方坐标在 cls/method；owner/name/desc 是被调方坐标。call_sites 的三个过滤参数按被调方过滤（None=通配），别把 owner 当调用方读。invokedynamic 不在普查面（无 owner 键，站点形态走 decryptor_sites 的 indy-shape）。

## DecryptSite（U.decryptor_sites(h) / U.residual_sites(h)）

| 字段 | 含义 |
|------|------|
| cls / method | 站点所在类与 `"名(描述符)"` 拼接串（同 CallSite 调用方口径） |
| index | 方法体链表序位次（`mjt.insns(mref)[index]` 取 uid） |
| mnemonic | `invokestatic` / `invokedynamic` |
| reason | 形态判定（`direct-shape` / `indy-shape`） |

`.row()` 给 TSV 行；`U.dump_sites(sites, path)` 按同格式落盘。

## StringCensus（U.string_census(h, min_len=1)）

对象不可迭代（`for s in census` 抛 TypeError），条目在 `.pairs`：`[(str, count)]` 按出现次数降序。`len(census)` 是总条目数，`.top(n)` 取前 n 条。

## HandlerRef（U.identity_rethrows(h, mref)）

| 字段 | 含义 |
|------|------|
| h / mref | 容器句柄与方法键（删除走 mref，不要攒 h 调底层） |
| rows | `[(start_uid, end_uid, handler_uid, catch_type)]` 本组 try 行 |
| body | `[(uid, detail)]` handler 体指令，首元素是 handler label 节点 |

`.remove()` 删 handler 体指令并按 uid 四元组删对应 try 行。

## GuardRef（U.guard_patterns(h, mref)）

branch 是恒假分支指令 uid，const 是恒值载荷 uid；`.remove()` 把载荷改写为 pop 并删分支。跳转目标块可能因此不可达，自由使用前先 `U.dry_run` 试算。

## DryRunDraft（U.dry_run(h, cref, fn, method=None)）与 Cleanliness

verify_ok / verify_error / changed / diff（方法名到 Cleanliness 差值的 dict）；`.summary` 一行结论，`.apply()` 落地（verify 不过抛 RuntimeError）。Cleanliness 字段：insns / handlers / branches / const_loads / calls，支持相减。

## 常用绑定返回形状

| 调用 | 返回 |
|------|------|
| mjt.class_names(h) | internal name 升序 list |
| mjt.class_info(cref) | dict：name / super / access / version / interfaces |
| mjt.methods(cref) | `[(name, desc, access)]` |
| mjt.fields(cref) | 同 methods |
| mjt.insns(mref) | `[(uid, opcode, kind)]`，链表序，含 label/line/frame 节点 |
| mjt.insn_detail(mref, uid) | dict：公共 uid / opcode / kind / mnemonic，按 kind 附操作数字段（method/field 带 owner/name/desc，ldc 带 cst_tag/cst，jump 带 target，switch 带 min/max/targets/keys） |
| mjt.method_info(mref) | dict：name / desc / access / max_stack / max_locals / exceptions |
| mjt.try_catches(mref) | `[(start_uid, end_uid, handler_uid, catch_type)]` |
| mjt.class_jasm(h, name) | dict：internal_name / source（LF 规范化全文） |
| mjt.method_jasm(h, cls, name, desc) | dict：internal_name / name / descriptor / source |
| U.classes_where(h, name=…, not_name=…, where=…) | `[(h, 类名)]` cref 列表（name glob 吃点分名） |
| U.methods_where(h, cref, …) | mref 列表 |
| U.call_sites(h, owner, name, desc) | CallSite 列表（链表序） |
| U.string_census(h, min_len) | StringCensus |
| U.decryptor_sites(h) / U.residual_sites(h) | DecryptSite 列表 |
| U.identity_rethrows(h, mref) / U.fake_handlers | HandlerRef 列表 |
| U.guard_patterns(h, mref) | GuardRef 列表 |
| U.init_order(h, roots=None) | dict：order（被依赖者在前）/ cycles（同 SCC 组）/ no_clinit |

## jasm 面读落账字节

class_jasm / method_jasm / class_validate_jasm 读的是容器落账字节，不是树缓存：add_method、emit_*、class_set_super 等树编辑之后要先 `mjt.flush(h, compute_frames=…)`（或 write_back / export）再取 jasm，否则读到旧字节。init_order 走 jasm 面，同受此约束。
