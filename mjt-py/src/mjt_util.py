# mjt_util.py — mjt.util：纯 Python 二次封装层（不碰 C++）。
#
# 定案（mjt-py/doc_ai/mjt-py-sdk-spec.md v3 §4）：清单沿 v2，改基 v3 平函数面
# （Ref=键，零对象持有）；判据与 C++ pass 同款（移植语义，非引擎强制门）；
# 安全类工具（dry_run/save_verified）是可选调用。
#
# 载入方式：mjt 模块 init 时把本文件 exec 成 mjt.util 子模块（globals 里的
# `mjt` 由装载方注入——禁止在此 import mjt，嵌入初始化期 sys.modules 未注册）。
#
# kind 名对齐 facadeInsnKindName：insn/int/var/type/field/method/indy/jump/
# label/ldc/iinc/tableswitch/lookupswitch/multianewarray/frame/line；
# ldc cst_tag：1=str 2=int 3=i64 4=float 5=double。

import re
from dataclasses import dataclass

OP  = mjt.opcodes
ACC = mjt.acc
_UNCONDITIONAL = {"goto", "goto_w", "athrow", "ireturn", "lreturn", "freturn",
                  "dreturn", "areturn", "return", "tableswitch", "lookupswitch",
                  "jsr", "ret"}
_CONST_INT = {"iconst_m1", "iconst_0", "iconst_1", "iconst_2", "iconst_3",
              "iconst_4", "iconst_5", "bipush", "sipush"}
_TERMINAL_OPS = {"ireturn", "lreturn", "freturn", "dreturn", "areturn", "return",
                 "athrow", "goto", "goto_w", "tableswitch", "lookupswitch", "ret"}
_NON_INSN_KINDS = {"label", "line", "frame"}


# ── 函数面读取助手（v3：uid + detail dict） ──────────────────────────────

def _details(mref):
    """链表序 [(uid, detail_dict)]（含 label/line 节点）。"""
    return [(uid, mjt.insn_detail(mref, uid)) for uid, _op, _kind in mjt.insns(mref)]


def _real(details):
    """只留真指令（剔除 label/line/frame）。"""
    return [(uid, d) for uid, d in details if d["kind"] not in _NON_INSN_KINDS]


def _glob_match(pattern, value):
    """迭代式 * / ? 通配（C++ globMatch 移植）。"""
    p = v = 0
    star_p, star_v = -1, 0
    while v < len(value):
        if p < len(pattern) and (pattern[p] == "?" or pattern[p] == value[v]):
            p += 1
            v += 1
        elif p < len(pattern) and pattern[p] == "*":
            star_p = p
            p += 1
            star_v = v
        elif star_p >= 0:
            p = star_p + 1
            star_v += 1
            v = star_v
        else:
            return False
    while p < len(pattern) and pattern[p] == "*":
        p += 1
    return p == len(pattern)


def classes_where(h, **pred):
    """cref 惰性过滤（name glob / not_name / where callable，glob 吃点分名）。"""
    out = []
    for name in mjt.class_names(h):
        dotted = name.replace("/", ".")
        if pred.get("name") and not _glob_match(pred["name"], dotted):
            continue
        if pred.get("not_name") and _glob_match(pred["not_name"], dotted):
            continue
        if pred.get("where") and not pred["where"](name):
            continue
        out.append((h, name))
    return out


def methods_where(h, cref, **pred):
    """mref 过滤（name glob/not_name/name_in/name_not_in/static/abstract/
    native/has_handlers/where callable）。"""
    want = {k: bool(pred[k]) for k in ("static", "abstract", "native", "has_handlers")
            if k in pred}
    out = []
    for name, desc, _access in mjt.methods(cref):
        if not _glob_match(pred.get("name", "*"), name):
            continue
        if pred.get("not_name") and _glob_match(pred["not_name"], name):
            continue
        if pred.get("name_in") and name not in pred["name_in"]:
            continue
        if pred.get("name_not_in") and name in pred["name_not_in"]:
            continue
        mref = (cref[0], cref[1], name, desc)
        info = mjt.method_info(mref)
        if "static" in want and bool(info["access"] & ACC["STATIC"]) != want["static"]:
            continue
        if "abstract" in want and bool(info["access"] & ACC["ABSTRACT"]) != want["abstract"]:
            continue
        if "native" in want and bool(info["access"] & ACC["NATIVE"]) != want["native"]:
            continue
        if "has_handlers" in want and bool(mjt.try_catches(mref)) != want["has_handlers"]:
            continue
        if pred.get("where") and not pred["where"](mref):
            continue
        out.append(mref)
    return out


# ── 恒等 rethrow 假 handler（判据同款：混淆器假 handler 三形态保守子集） ──

@dataclass
class HandlerRef:
    """一组共享同一 handler 体的恒等 rethrow 行（try 行 uid 四元组 + handler 体）。"""

    h: int                    # 容器句柄（键面完整性；删除只走 mref）
    mref: tuple               # (h, cls, name, desc)
    rows: list                # [(start_uid, end_uid, handler_uid, catch_type)]
    body: list                # [(uid, detail)] handler 体（[0] = handler label 节点）

    def remove(self):
        """删 handler 体指令（逆序；handler label 节点保留为孤儿标记，写回无感）
        + 按 uid 四元组反查行号删 try 行——键寻址，无悬垂。"""
        mref = self.mref
        for uid, _d in reversed(self.body[1:]):
            mjt.remove_insn(mref, uid)
        all_rows = mjt.try_catches(mref)
        mine = {tuple(r) for r in self.rows}
        for i in reversed(range(len(all_rows))):
            if tuple(all_rows[i]) in mine:
                mjt.remove_try_catch(mref, i)


def _handler_body(details, handler_uid):
    """handler 体 = handler label 起（含）到下一个 label 节点前（不含）。"""
    idx = next(i for i, (uid, d) in enumerate(details) if uid == handler_uid)
    j = idx + 1
    while j < len(details) and details[j][1]["kind"] != "label":
        j += 1
    return idx, j


def _identity_body_shape(body):
    """返回 (ok, wrap_desc)。三形态：裸 athrow / invokestatic 恒等包装 / store 桥。"""
    ins = _real(body)
    ops = [d["mnemonic"] for _u, d in ins]
    if ops == ["athrow"]:
        return True, None
    if len(ops) == 2 and ops[0] == "invokestatic" and ops[1] == "athrow":
        return True, ins[0][1].get("desc")
    if (len(ops) == 4 and ops[0] == "astore" and ops[1] == "aload"
            and ops[2] == "invokestatic" and ops[3] == "athrow"):
        return True, ins[2][1].get("desc")
    return False, None


def _desc_return(desc):
    return desc.rsplit(")", 1)[-1] if ")" in desc else ""


def _desc_param(desc):
    return desc[1:desc.index(")")] if ")" in desc else ""


def _wrap_ref(body):
    """恒等包装形态里 invokestatic 的 (owner, name, desc)；无则 (None, None, None)。"""
    for _u, d in _real(body):
        if d["mnemonic"] == "invokestatic":
            return d.get("owner"), d.get("name"), d.get("desc")
    return None, None, None


def _wrap_is_identity(h, owner, name, desc):
    """wrapper 方法体是否纯恒等（只有 aload 0 + areturn，参数原样返回）。
    方法不存在/签名对不上/体长不对一律 False —— 调用方据此走保守签名门。"""
    if not owner or not name or not desc:
        return False
    try:
        items = [d for _u, d in _details((h, owner, name, desc))
                 if d["kind"] in ("var", "insn", "int")]
    except Exception:
        return False
    if len(items) != 2:
        return False
    return (items[0]["mnemonic"] == "aload" and items[0].get("var", 0) == 0
            and items[1]["mnemonic"] == "areturn")


def _body_forbidden(body):
    return any(d["mnemonic"] in ("monitorenter", "monitorexit", "jsr", "ret")
               for _u, d in _real(body))


def _jump_into_body(details, body_label_uids):
    for _uid, d in details:
        if d["kind"] == "jump" and d.get("target") in body_label_uids:
            return True
    return False


def _fallthrough_into(details, handler_uid):
    """handler 前驱（非 label/line）必须是无条件转移/返回，排除顺序落入。"""
    idx = next(i for i, (uid, d) in enumerate(details) if uid == handler_uid)
    j = idx - 1
    while j >= 0 and details[j][1]["kind"] in _NON_INSN_KINDS:
        j -= 1
    if j < 0:
        return True
    return details[j][1]["mnemonic"] not in _UNCONDITIONAL


def identity_rethrows(h, mref, *, allow_overlap=False):
    """恒等 rethrow 假 handler 组列表（保守门：非 catch-all + monitor/jsr 透明 +
    无 jump 入体 + 前驱非 fallthrough + 恒等包装（签名单参同返回，或方法体已验证
    为 `aload 0; areturn` 的父类擦除型）+ 无跨组重叠）。

    allow_overlap=True 时跳过最后一道跨组重叠门，返回"除交叠外全过门"的组，
    供链式成组删除使用 —— 交叠组要在调用方按区间关系连成链、整链一起判、一起删；
    此时返回的组彼此可能交叠，单独删任意一组都会坏罐。"""
    rows_all = mjt.try_catches(mref)
    if not rows_all:
        return []
    details = _details(mref)
    pos_of = {uid: i for i, (uid, _d) in enumerate(details)}

    groups = {}
    for row in rows_all:
        groups.setdefault(row[2], []).append(row)   # 按 handler uid 分组

    out = []
    for handler_uid, rows in groups.items():
        if handler_uid not in pos_of:
            continue
        b0, b1 = _handler_body(details, handler_uid)
        body = details[b0:b1]
        if not body or body[0][1]["kind"] != "label":
            continue
        ok, wrap_desc = _identity_body_shape(body)
        if not ok:
            continue
        if any(not r[3] for r in rows):        # catch-all 不动（真语义 handler）
            continue
        if _body_forbidden(body):
            continue
        body_label_uids = {uid for uid, d in body if d["kind"] == "label"}
        if _jump_into_body(details, body_label_uids):
            continue
        if _fallthrough_into(details, handler_uid):
            continue
        # 恒等包装判据：单引用参数且返回类型相同（签名不是变换）。签名门过了以后，
        # 以 wrapper 方法体是否纯恒等为准 —— 方法体是 `aload 0; areturn` 就放行，
        # 哪怕它把 catch 类型擦宽成父类（如 (Exception)Exception 包 RuntimeException）。
        # 解析不到方法体时退回"返回类型 == catch 类型或 Throwable"的保守签名门。
        if wrap_desc is not None:
            if _desc_param(wrap_desc) != _desc_return(wrap_desc):
                continue
            owner, name, wdesc = _wrap_ref(body)
            if not _wrap_is_identity(h, owner, name, wdesc or wrap_desc):
                ret    = _desc_return(wrap_desc)
                caught = rows[0][3]
                if ret not in ("L" + caught + ";", "Ljava/lang/Throwable;"):
                    continue
        # 重叠门：本组 try 区间与其他组的区间不得交叠（保守，消除行序耦合）。
        # 链式删除走 allow_overlap=True 绕过本门，交叠本身改成建链依据。
        if allow_overlap:
            spans = [(pos_of.get(r[0]), pos_of.get(r[1])) for r in rows]
            if any(s is None or e is None for s, e in spans):
                continue
            out.append(HandlerRef(h=h, mref=mref, rows=rows, body=body))
            continue
        spans = [(pos_of.get(r[0]), pos_of.get(r[1])) for r in rows]
        if any(s is None or e is None for s, e in spans):
            continue
        other = [r for k, rs in groups.items() if k != handler_uid for r in rs]
        bad = False
        for s0, e0 in spans:
            for r2 in other:
                s1, e1 = pos_of.get(r2[0]), pos_of.get(r2[1])
                if s1 is None or e1 is None:
                    continue
                if not (e1 <= s0 or e0 <= s1):
                    bad = True
                    break
            if bad:
                break
        if bad:
            continue
        out.append(HandlerRef(h=h, mref=mref, rows=rows, body=body))
    return out


# 梦境 1/6 同判据别名。
fake_handlers = identity_rethrows


# ── 恒假谓词（guard）普查：保守形态子集 ──────────────────────────────────

@dataclass
class GuardRef:
    h: int            # 容器句柄
    mref: tuple
    branch: int       # ifeq/ifne uid（永不成立的分支）
    const: int        # 恒值载荷 uid（iconst_0/1）

    def remove(self):
        """恒假分支保守改写：载荷 iconst_0/1 → pop（消掉栈压入）、分支指令删除
        （修 v2 set_opcode_pop 单改分支的栈失衡 bug）。
        注意：跳转目标块若仅由该分支到达会变成不可达——写回端 COMPUTE_FRAMES
        校验兜底（dry_run/verify 场景下必过闸），自由使用前请先 dry_run。"""
        mjt.insn_set_opcode(self.mref, self.const, OP["pop"])
        mjt.remove_insn(self.mref, self.branch)


def guard_patterns(h, mref):
    """恒假谓词读取点普查（保守子集：iconst_0+ifne / iconst_1+ifeq 直连形态）。"""
    out = []
    details = _details(mref)
    for i in range(1, len(details)):
        uid, d  = details[i]
        puid, pd = details[i - 1]
        if d["kind"] != "jump" or d["mnemonic"] not in ("ifne", "ifeq"):
            continue
        if pd["mnemonic"] == "iconst_0" and d["mnemonic"] == "ifne":
            out.append(GuardRef(h=h, mref=mref, branch=uid, const=puid))
        elif pd["mnemonic"] == "iconst_1" and d["mnemonic"] == "ifeq":
            out.append(GuardRef(h=h, mref=mref, branch=uid, const=puid))
    return out


# ── 解密器形态站点普查 / 残留台账（dream2） ───────────────────────────────

@dataclass
class DecryptSite:
    cls: str
    method: str
    index: int
    mnemonic: str
    reason: str

    def row(self):
        return f"{self.cls}\t{self.method}\t{self.index}\t{self.mnemonic}\t{self.reason}"


def _is_string_return(desc):
    return desc.endswith(")Ljava/lang/String;")


def decryptor_sites(h):
    """解密器形态站点普查（启发式口径）：返回 String 的 invokestatic（前 ≥2 条
    常量 int 载荷）/ invokedynamic 调用点。管线前后差分 = 残留台账。

    刻意保持 int 校准（census 口径稳定，勿随折叠门放宽）：str 键解密器
    （(String,int)→String 形态）会漏检，精确路径走 call_sites 定点坐标 +
    fold_call_sites——解密器已定位时按 (owner,name,desc) 全量回填，不依赖
    启发式。"""
    sites = []
    for name in mjt.class_names(h):
        for mn, md, _ma in mjt.methods((h, name)):
            mref    = (h, name, mn, md)
            details = _details(mref)
            for i, (uid, d) in enumerate(details):
                if d["kind"] == "method" and d["mnemonic"] == "invokestatic" \
                        and _is_string_return(d["desc"]):
                    consts = 0
                    j = i - 1
                    while j >= 0:
                        pd = details[j][1]
                        if pd["mnemonic"] in _CONST_INT or \
                                (pd["mnemonic"] == "ldc" and pd.get("cst_tag") == 2):
                            consts += 1
                            j -= 1
                        else:
                            break
                    if consts >= 2:
                        sites.append(DecryptSite(name, mn + md, i, "invokestatic",
                                                 "direct-shape"))
                elif d["kind"] == "indy" and _is_string_return(d["desc"]):
                    sites.append(DecryptSite(name, mn + md, i, "invokedynamic", "indy-shape"))
    return sites


def residual_sites(h):
    """管线跑完后的残留解密站点（=台账残余面；逐站附 shape 原因）。"""
    return decryptor_sites(h)


def dump_sites(sites, path):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("class\tmethod\tindex\tmnemonic\treason\n")
        for s in sites:
            fh.write(s.row() + "\n")


# ── 常量回填（dream2 折叠路径）：解密调用点 → 明文 ldc ────────────────────

def _desc_arg_count(desc):
    """方法描述符的参数个数（JVM 内部形态；L…; 一参、[ 前缀并入该参数）。"""
    params = _desc_param(desc)
    n, i = 0, 0
    while i < len(params):
        c = params[i]
        if c == "L":
            i = params.index(";", i) + 1
            n += 1
            continue
        if c != "[":
            n += 1
        i += 1
    return n


_ICONST_VALUE = {"iconst_m1": -1, "iconst_0": 0, "iconst_1": 1, "iconst_2": 2,
                 "iconst_3": 3, "iconst_4": 4, "iconst_5": 5}


def _const_load_value(d):
    """常量压栈指令的载荷值（iconst/bipush/sipush + 任意 ldc 载荷；非常量 →
    None）。ldc 全 tag 放行（str/int/long/float/double）：折叠路径里实参指令
    是被摘除不是被顶替，载荷新旧无关拆除安全性；int-only 旧门会拒掉 str 键
    解密器（样本实战 W2 墙）。"""
    mn = d["mnemonic"]
    if mn in _ICONST_VALUE:
        return _ICONST_VALUE[mn]
    if mn in ("bipush", "sipush"):
        return d.get("operand")
    if d["kind"] == "ldc" and "cst" in d:
        return d["cst"]
    return None


@dataclass
class CallSite:
    cls: str
    method: str          # name+desc
    uid: int             # 调用点 uid（v3 结构稳定键，可直接喂 fold_to_ldc）
    mnemonic: str
    owner: str
    name: str
    desc: str


def call_sites(h, owner=None, name=None, desc=None):
    """调用点定点普查：全罐方法调用指令（invokestatic/virtual/special/interface）
    按 owner/name/desc 精确过滤（None=通配）。返回链表序 CallSite 列表。
    invokedynamic 不在普查面（indy 无 owner 键，站点形态走 decryptor_sites 的
    indy-shape 或专用 bootstrap 桩）。解密器已由人工/上游定位时的精确路径入口
    ——启发式 decryptor_sites 漏检的 str 键解密器靠这里抓全。"""
    out = []
    for cn in mjt.class_names(h):
        for mn, md, _ma in mjt.methods((h, cn)):
            mref = (h, cn, mn, md)
            for uid, _op, kind in mjt.insns(mref):
                if kind != "method":
                    continue
                d = mjt.insn_detail(mref, uid)
                if owner is not None and d["owner"] != owner:
                    continue
                if name is not None and d["name"] != name:
                    continue
                if desc is not None and d["desc"] != desc:
                    continue
                out.append(CallSite(cn, mn + md, uid, d["mnemonic"],
                                    d["owner"], d["name"], d["desc"]))
    return out


def fold_call_sites(h, callee, value_of):
    """定点折叠：callee=(owner, name, desc) 的全部调用点 → 明文 ldc。

    与 fold_decryptor_sites 的启发式普查路径互补：实参常量值按声明序组成
    元组交 value_of(args) 求明文——脚本侧就地解密（xor/查表都行），解密器
    本体随后通常一并删除。str 键解密器（样本实战 W1/W2 墙）的正式出路。

    门与 fold_to_ldc 同款 fail-closed：带 receiver 的调用点（非 invokestatic）
    存在即拒（callee 坐标对到实例方法是调用方错误，静默空转会藏错）；实参
    非纯常量压栈即拒，不落半成品。value_of 返回 None → 该站跳过（信任门，
    同 fold_decryptor_sites）。返回 [(site, ldc_uid)]（跳过的不入）；同方法
    内按 uid 降序折叠（防御序，多站互不踩位）。
    部分进度语义同 fold_decryptor_sites：单站拒绝原样抛，已折站保持已折、
    后续站不落——调用方按台账自行重放。"""
    owner, name, desc = callee
    found = call_sites(h, owner, name, desc)
    bad = [s for s in found if s.mnemonic != "invokestatic"]
    if bad:
        raise ValueError(
            f"fold_call_sites: {owner}.{name}{desc} 有 {len(bad)} 个带 receiver"
            f" 调用点（{bad[0].mnemonic}，{bad[0].cls}.{bad[0].method}），"
            "无法等位回填——callee 应为 static 方法坐标")
    sites = found
    jobs = {}
    for s in sites:
        k = s.method.index("(")
        mref = (h, s.cls, s.method[:k], s.method[k:])
        jobs.setdefault(mref, []).append(s)
    out = []
    for mref, items in jobs.items():
        details = _details(mref)
        pos_of = {uid: i for i, (uid, _d) in enumerate(details)}
        for s in sorted(items, key=lambda t: t.uid, reverse=True):
            pos = pos_of[s.uid]
            n_args = _desc_arg_count(desc)
            vals, j = [], pos - 1
            while j >= 0 and len(vals) < n_args:
                uid, pd = details[j]
                if pd["kind"] not in _NON_INSN_KINDS:
                    v = _const_load_value(pd)
                    if v is None:
                        raise ValueError(
                            f"fold_call_sites: {s.cls}.{s.method} uid {s.uid}"
                            f" 实参 #{len(vals)} 非常量压栈（{pd['mnemonic']}），拒绝折叠")
                    vals.append(v)
                j -= 1
            if len(vals) < n_args:
                raise ValueError(
                    f"fold_call_sites: {s.cls}.{s.method} uid {s.uid}"
                    f" 实参载荷不足（需 {n_args}），拒绝折叠")
            plain = value_of(tuple(reversed(vals)))
            if plain is None:
                continue
            out.append((s, fold_to_ldc(h, mref, s.uid, plain)))
    return out


def fold_to_ldc(h, mref, call_uid, value):
    """把一个解密调用点回填为明文常量：call_uid 处的调用换成 ldc value，
    其紧邻的常量实参载荷一并删除；返回新 ldc uid。

    栈语义（栈机没有 dst，回填=栈产物等位顶替）：调用点的返回值由 ldc 等位
    顶替，调用结果的消费方无需任何改写（消费方只认栈顶类型；value 类型须与
    调用点返回类型一致，由脚本保证，write_back COMPUTE_FRAMES 兜底校验）。
    实参必须是纯常量压栈（iconst/bipush/sipush/任意 ldc 载荷）——来自局部
    变量或表达式的实参无法在指令面安全摘除：抛 ValueError 且不落任何改动
    （先校验后变更，fail-closed 无半成品）。

    str 载荷按值安全：绑定层将 str intern 进会话 arena（与树内字符串同源
    同寿命），写出的类字节确定；int/float 直传 emit_ldc 不受影响。

    参数:
        h:        容器句柄（=mref[0]；util 层惯例的显式传参）
        mref:     方法键 (h, cls, name, desc)
        call_uid: 解密调用点 uid（invokestatic / invokedynamic；带 receiver 的
                  invokevirtual/invokespecial 拒绝——receiver 无法等位重绑）
        value:    明文常量（int/float/str 直传 emit_ldc；py float 落 double ldc）

    调用点与实参 uid 随即失效（remove 墓地 fail-closed）；新 ldc uid 结构编辑下
    稳定，可用于后续定位。"""
    if h != mref[0]:
        raise ValueError("fold_to_ldc: h and mref[0] disagree")
    d = mjt.insn_detail(mref, call_uid)
    if d["kind"] == "method":
        if d["mnemonic"] != "invokestatic":
            raise ValueError(
                f"fold_to_ldc: {d['mnemonic']} 带 receiver，无法等位回填"
                "（只支持 invokestatic/invokedynamic）")
        n_args = _desc_arg_count(d["desc"])
    elif d["kind"] == "indy":
        n_args = _desc_arg_count(d["desc"])
    else:
        raise ValueError(f"fold_to_ldc: uid {call_uid} 不是调用点（kind={d['kind']}）")

    # 实参载荷定位：倒序走链，label/line/frame 不是栈载荷跳过；非常量压栈即拒
    #（门 = _const_load_value：int 常量 + 任意 ldc 载荷，实参被摘除不顶替，
    # 载荷新旧无关安全）。
    details = _details(mref)
    pos = next(i for i, (uid, _d) in enumerate(details) if uid == call_uid)
    args = []
    j = pos - 1
    while j >= 0 and len(args) < n_args:
        uid, pd = details[j]
        if pd["kind"] not in _NON_INSN_KINDS:
            if _const_load_value(pd) is None:
                raise ValueError(
                    f"fold_to_ldc: 实参 #{len(args)} 非常量压栈（{pd['mnemonic']}），拒绝回填")
            args.append(uid)
        j -= 1
    if len(args) < n_args:
        raise ValueError(f"fold_to_ldc: 实参载荷不足（需 {n_args}），拒绝回填")

    # 校验全过才变更：ldc 插调用点前 → 摘调用点 → 摘实参（uid 稳定，顺序无关）。
    new_uid = mjt.emit_ldc(mref, value, pos=call_uid)
    mjt.remove_insn(mref, call_uid)
    for uid in args:
        mjt.remove_insn(mref, uid)
    return new_uid


def fold_decryptor_sites(h, value_of, sites=None):
    """残留解密站点批量回填（dream2 折叠路径的 util 批口）。

    value_of(site) -> 明文值或 None（None=该站跳过——信任哪些站点由脚本决定，
    安全门是可选项不是引擎强制门）。sites=None 时取 residual_sites(h) 现查。

    返回 [(site, ldc_uid)]（跳过的不入）。uid 先统一解析再折叠（uid 结构编辑下
    稳定，多站互不踩位）；单站回填失败原样抛（fold_to_ldc fail-closed 语义，
    已折站点保持已折、后续站点不落——调用方按台账自行重放）。"""
    if sites is None:
        sites = residual_sites(h)
    jobs = {}
    for s in sites:
        k = s.method.index("(")
        mref = (h, s.cls, s.method[:k], s.method[k:])
        uid = mjt.insns(mref)[s.index][0]
        jobs.setdefault(mref, []).append((uid, s))
    out = []
    for mref, items in jobs.items():
        for uid, s in sorted(items, key=lambda t: t[1].index, reverse=True):
            plain = value_of(s)
            if plain is None:
                continue
            out.append((s, fold_to_ldc(h, mref, uid, plain)))
    return out


# ── 方法克隆（同类复制体；emit_* 原语逐条重建） ────────────────────────────

_CLONE_UNSUPPORTED_KINDS = ("tableswitch", "lookupswitch", "indy", "multianewarray")
# kind 名对齐 facadeInsnKindName（注意：IntInsnNode——bipush/sipush/newarray——
# 的 kind 是 "int"，零操作数 InsnNode 才是 "insn"）。
_CLONE_SUPPORTED_KINDS = ("label", "line", "frame", "insn", "int", "var", "iinc",
                          "type", "field", "method", "ldc", "jump")


def clone_method(h, mref, new_name):
    """同类克隆方法 → 新 mref（新 name、同 desc/access/max_stack/max_locals/
    exceptions；指令体按 uid 序逐条 emit_* 重建，try 行按 label 映射复制）。

    限制（§2 指令面原语边界，如实）：
    - tableswitch/lookupswitch/indy/multianewarray 无法重建（缺 emit_switch/
      emit_indy/emit_multianewarray 原语）→ NotImplementedError；newarray 同理
      （缺 emit_int 原语，ldc 无数组等位）→ NotImplementedError；预扫先于构建，
      不落半成品；
    - bipush/sipush 重建为等值 int ldc（emit_insn 只能造零操作数 InsnNode，
      ldc 与之栈效果/值等价；字节形态有别——javap 可见 ldc）；
    - ldc 只可复制 int（cst_tag 2）与 str（cst_tag 1，绑定层 intern 进会话
      arena，按值安全）；long/float/double（3/4/5）因 emit_ldc 无显式宽度
      控制（py 值形态决定 tag，宽限必然漂移）→ NotImplementedError；
    - LineNumberTable / StackMapTable 不复制（line/frame 节点跳过；write_back
      COMPUTE_FRAMES 重算帧）；
    - 前向跳转先暂绑既有 label（无前序 label 时落占位），目标 label 生成后
      insn_set_target 重绑；占位 label 是孤儿节点，ClassWriter 不为其产出字节。
    同名同描述符冲突 → add_method 抛错（fail-closed）；克隆体不继承 LVT。"""
    if h != mref[0]:
        raise ValueError("clone_method: h and mref[0] disagree")
    info = mjt.method_info(mref)
    src = _details(mref)

    # 预扫：不支持形态先拒，构建零半成品。
    for uid, d in src:
        if d["kind"] in _CLONE_UNSUPPORTED_KINDS:
            raise NotImplementedError(
                f"clone_method: {d['kind']} 当前指令面无 emit 原语，无法克隆（uid {uid}）")
        if d["kind"] not in _CLONE_SUPPORTED_KINDS:
            raise NotImplementedError(f"clone_method: kind {d['kind']} 未覆盖（uid {uid}）")
        if d["kind"] == "int" and d["opcode"] == 188:   # newarray：无等位原语
            raise NotImplementedError(
                f"clone_method: newarray 无 emit_int/数组等位原语，无法克隆（uid {uid}）")
        if d["kind"] == "ldc" and d.get("cst_tag", 2) not in (1, 2):
            raise NotImplementedError(
                f"clone_method: ldc cst_tag={d.get('cst_tag')} 无法保真复制"
                "（3/4/5=emit_ldc 无显式宽度控制，py 值形态决定 tag 必漂移）")

    new = mjt.add_method((h, mref[1]), new_name, mref[3], info["access"],
                         info["max_stack"], info["max_locals"])
    if info["exceptions"]:
        mjt.method_set_exceptions(new, list(info["exceptions"]))

    label_map = {}      # 源 label uid → 克隆 label uid
    fixups = []         # (克隆 jump uid, 源目标 label uid)——前向跳转后绑
    for uid, d in src:
        k, op = d["kind"], d["opcode"]
        if k == "label":
            label_map[uid] = mjt.emit_label(new)
        elif k in ("line", "frame"):
            continue
        elif k == "insn":
            mjt.emit_insn(new, op)
        elif k == "int":
            # bipush/sipush：emit_insn 只能造零操作数 InsnNode，等值 int ldc 栈等位。
            # （newarray 已在预扫描拒绝；此分支只会见到 bipush/sipush。）
            mjt.emit_ldc(new, d["operand"])
        elif k == "var":
            mjt.emit_var(new, op, d["var"])
        elif k == "iinc":
            mjt.emit_iinc(new, d["var"], d["incr"])
        elif k == "type":
            mjt.emit_type(new, op, d["desc"])
        elif k == "field":
            mjt.emit_field(new, op, d["owner"], d["name"], d["desc"])
        elif k == "method":
            mjt.emit_method_call(new, op, d["owner"], d["name"], d["desc"],
                                 bool(d.get("itf")))
        elif k == "ldc":
            mjt.emit_ldc(new, d["cst"])
        elif k == "jump":
            t = d.get("target")
            if t in label_map:
                mjt.emit_jump(new, op, label_map[t])
            elif label_map:
                # 前向：暂绑既有 label，目标生成后重绑（不落占位节点）。
                nu = mjt.emit_jump(new, op, next(iter(label_map.values())))
                fixups.append((nu, t))
            else:
                # 体首前向跳转（无任何既有 label 可暂绑）：占位 label 兜底。
                placeholder = mjt.emit_label(new)
                nu = mjt.emit_jump(new, op, placeholder, pos=placeholder)
                fixups.append((nu, t))
        else:
            raise NotImplementedError(f"clone_method: kind {k} 未覆盖")

    for ju, t in fixups:
        if t not in label_map:
            raise ValueError(f"clone_method: jump 目标 label uid {t} 不在方法体内")
        mjt.insn_set_target(new, ju, label_map[t])

    for s0, e0, h0, ct in mjt.try_catches(mref):
        if s0 not in label_map or e0 not in label_map or h0 not in label_map:
            raise ValueError("clone_method: try 行引用方法体外 label，拒绝克隆")
        mjt.add_try_catch(new, label_map[s0], label_map[e0], label_map[h0], ct)
    return new


# ── 字符串普查（dream8） ─────────────────────────────────────────────────

@dataclass
class StringCensus:
    pairs: list

    def top(self, n=20):
        return self.pairs[:n]

    def __len__(self):
        return len(self.pairs)


def string_census(h, min_len=1):
    """全罐 ldc 字符串直方图（降序）。"""
    counter = {}
    for name in mjt.class_names(h):
        for mn, md, _ma in mjt.methods((h, name)):
            for _uid, d in _details((h, name, mn, md)):
                if d["kind"] == "ldc" and d.get("cst_tag") == 1 and len(d["cst"]) >= min_len:
                    counter[d["cst"]] = counter.get(d["cst"], 0) + 1
    return StringCensus(sorted(counter.items(), key=lambda kv: -kv[1]))


# ── 清洁度（dream4） ─────────────────────────────────────────────────────

@dataclass
class Cleanliness:
    insns: int = 0
    handlers: int = 0
    branches: int = 0
    const_loads: int = 0
    calls: int = 0

    def __sub__(self, other):
        return Cleanliness(self.insns - other.insns, self.handlers - other.handlers,
                           self.branches - other.branches, self.const_loads - other.const_loads,
                           self.calls - other.calls)


def cleanliness(h, mref):
    details = _details(mref)
    c = Cleanliness()
    c.insns    = len(_real(details))
    c.handlers = len(mjt.try_catches(mref))
    for _uid, d in details:
        mn = d["mnemonic"]
        if mn.startswith("if") or mn in ("goto", "goto_w", "tableswitch", "lookupswitch"):
            c.branches += 1
        if mn == "ldc" or mn in _CONST_INT:
            c.const_loads += 1
        if mn.startswith("invoke"):
            c.calls += 1
    return c


# ── dry_run（dream5）：容器分叉 + verify 门 + diff（v3：无独立树，fork=临时容器） ──

@dataclass
class DryRunDraft:
    verify_ok: bool = False
    verify_error: str = ""
    changed: bool = False
    diff: object = None
    _parent_h: int = 0
    _cls_name: str = ""
    _fork_bytes: bytes = b""

    @property
    def summary(self):
        if not self.verify_ok:
            return f"verify failed: {self.verify_error}"
        return "verify ok, changed" if self.changed else "verify ok, unchanged"

    def apply(self):
        """试算结果落地（整类字节换入父容器；树缓存随之作废重解析）。"""
        if not self.verify_ok:
            raise RuntimeError("dry_run draft failed verify; refusing to apply")
        mjt.set_class_bytes(self._parent_h, self._cls_name, self._fork_bytes)


def _structural_ok(h, cls_name):
    """结构闸：每个具体方法体必须以终结指令收尾（xreturn/athrow/goto/switch/ret）。

    线性 lint 口径（不追 CFG）：引擎 COMPUTE_FRAMES 闸查类型/栈帧一致性，
    查不出"删了 return 掉出方法体"这类结构性破洞，这里补位。
    """
    for mn, md, ma in mjt.methods((h, cls_name)):
        if ma & (ACC["ABSTRACT"] | ACC["NATIVE"]):
            continue
        mref = (h, cls_name, mn, md)
        uids = [uid for uid, op, _k in mjt.insns(mref) if op >= 0]
        if not uids:
            return False
        last = mjt.insn_detail(mref, uids[-1])
        if last["mnemonic"] not in _TERMINAL_OPS:
            return False
    return True


def dry_run(h, cref, fn, method=None):
    """在容器分叉副本上跑 fn(mref) + verify 门（结构闸 + COMPUTE_FRAMES 写出），
    返回 Draft（满意再 .apply()）；父容器不动。method=None 对全类方法跑。
    fn 返回 True 表示有改动（管线 pass 语义的函数化等位物）。"""
    parent_h, cls_name = cref
    before_clean = {}
    for mn, md, _ma in mjt.methods(cref):
        before_clean[mn + md] = cleanliness(h, (h, cls_name, mn, md))

    fork_h = mjt.new_container()
    try:
        mjt.add_entry(fork_h, cls_name + ".class", mjt.class_bytes(h, cls_name))
        fork_cref = (fork_h, cls_name)
        changed = False
        for mn, md, _ma in mjt.methods(fork_cref):
            if method is not None and mn + md != method:
                continue
            if fn((fork_h, cls_name, mn, md)) is True:
                changed = True

        draft = DryRunDraft()
        draft.changed   = changed
        draft._parent_h = parent_h
        draft._cls_name = cls_name
        if not _structural_ok(fork_h, cls_name):
            draft.verify_error = "structural gate: method body does not end in a terminator"
        else:
            try:
                mjt.write_back(fork_cref, compute_frames=True)   # 校验即写出
                draft._fork_bytes = mjt.class_bytes(fork_h, cls_name)
                draft.verify_ok   = True
            except Exception as e:                               # noqa: BLE001 — 报给脚本
                draft.verify_error = str(e)

        if draft.verify_ok:
            after_clean = {}
            for mn, md, _ma in mjt.methods(fork_cref):
                after_clean[mn + md] = cleanliness(fork_h, (fork_h, cls_name, mn, md))
            draft.diff = {k: after_clean.get(k, Cleanliness()) - v
                          for k, v in before_clean.items()}
        return draft
    finally:
        mjt.close_container(fork_h)


# ── 改名助手（dream3） ───────────────────────────────────────────────────

_OBF_NAME = re.compile(r"^[a-z]{1,2}[0-9]{0,2}$|^ab$|^abc$")


def is_obfuscated_name(name):
    if name in ("<init>", "<clinit>"):
        return False
    return bool(_OBF_NAME.match(name))


_semantic_counter = {"cls": 0, "method": 0}


def semantic_name(cls=None, m=None):
    """占位语义名生成（策略可整体替换：接 LLM/词典后改这里即可）。"""
    if m is None:
        _semantic_counter["cls"] += 1
        return f"RenamedClass{_semantic_counter['cls']}"
    _semantic_counter["method"] += 1
    return f"renamedMethod{_semantic_counter['method']}"


def reset_semantic_names():
    _semantic_counter["cls"]    = 0
    _semantic_counter["method"] = 0


# ── 容器体检 / LVT 清洗（可选安全门，实测补刀批） ────────────────────────

def sanitize_lvt(h):
    """LVT 清洗（G1 兜底）：剔除 start/end 标签已脱链（墓地/删节点）与乱序条目、
    按 (index,start,end,name,desc) 去重；返回 (dropped, deduped, classes)。"""
    return mjt.sanitize_lvt(h)


# ── 落盘校验（可选安全门） ────────────────────────────────────────────────

def save_verified(h, path):
    """save(compute_frames=True) + 重开逐类解析复核；返回报告 dict。"""
    mjt.save(h, path, compute_frames=True)
    check = mjt.open_jar(path)
    try:
        skipped = []
        for name in mjt.class_names(check):
            try:
                mjt.parse_class((check, name))
            except Exception:                                    # noqa: BLE001
                skipped.append(name)
        return {"path": path, "classes": len(mjt.class_names(check)), "skipped": skipped}
    finally:
        mjt.close_container(check)
