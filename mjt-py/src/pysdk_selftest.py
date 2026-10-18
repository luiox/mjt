# pysdk_selftest.py — mjt-py SDK 绑定层自测（由 mjt_pysdk_unittest 嵌入执行）。
#
# v3 函数面（Ref=键）16 阶段：装载/导航 → util 判据 + 直改直删 + verify 落盘 →
# 内置单 pass（run_pass）+ 报告读出 → MethodPass 类型锚（适配器属私有侧管线面）→
# rename token + sidecar → dry_run（分叉试算 + verify 门）→ 字节往返 + 普查 →
# 管线写回链路 → fail-closed 异常面 + 容器体检 + rename→export→verify 闭环 →
# class_hierarchy 继承闭包 → enum values() 数组型 owner 随类改名（悬空回归位）→
# rename_apply_suggestions 被拒流水 → fold_to_ldc 解密点回填（实参同摘 +
# fail-closed 边界）→ clone_method 体逐条重建（异常表/跳转/直调/虚调）→
# dream2 折叠路径（普查→回填→残留归零）→ call_sites 定点普查 + fold_call_sites
# （str 键解密器批折 + 实例 callee 守卫）。
# 全程合成样本（mjt.write_demo_jar），零样本字节入库。


def run_all():
    import json
    import os
    import tempfile

    import mjt
    from mjt.util import (call_sites, clone_method, decryptor_sites,
                          dry_run, dump_sites, fold_call_sites,
                          fold_decryptor_sites, fold_to_ldc, guard_patterns,
                          identity_rethrows, residual_sites, save_verified,
                          string_census)

    fails = []

    def check(name, cond, detail=""):
        if not cond:
            fails.append(f"{name}: {detail}")

    tmp = tempfile.gettempdir()

    def fresh(name):
        p = os.path.join(tmp, f"mjt_pysdk_{name}.jar")
        mjt.write_demo_jar(p)
        return p, mjt.open_jar(p)

    # ── 1. 装载/导航 ──
    p1, h = fresh("nav")
    names = mjt.class_names(h)
    check("class count", len(names) == 5, str(names))
    check("class sorted", names == ["me/demo/Api", "me/demo/App", "me/demo/Color",
                                    "me/demo/Crypto", "me/demo/Util"],
          str(names))
    check("no duplicates", mjt.duplicate_names(h) == [], str(mjt.duplicate_names(h)))
    cref_app = (h, "me/demo/App")
    check("app init+main+guarded", len(mjt.methods(cref_app)) == 3,
          str(mjt.methods(cref_app)))
    main_mref = mjt.get_method(cref_app, "main", "([Ljava/lang/String;)V")
    info = mjt.method_info(main_mref)
    check("main static", bool(info["access"] & mjt.acc["STATIC"]), str(info))
    ldcs = [d["cst"] for uid, op, kind in mjt.insns(main_mref)
            for d in [mjt.insn_detail(main_mref, uid)]
            if d["kind"] == "ldc" and d.get("cst_tag") == 1]
    check("main ldc", "demo-payload" in ldcs, str(ldcs))
    calls = [d["name"] for uid, op, kind in mjt.insns(main_mref)
             for d in [mjt.insn_detail(main_mref, uid)]
             if d["kind"] == "method"]
    check("main calls decode", "decode" in calls, str(calls))
    guarded_mref = mjt.get_method(cref_app, "guarded", "()V")
    check("guarded handler rows", len(mjt.try_catches(guarded_mref)) == 1,
          str(mjt.try_catches(guarded_mref)))

    # ── 2. util：恒等 handler 判据 + 直改直删 + verify 落盘 ──
    p2, h = fresh("strip")

    def count_idents(hh):
        n = 0
        for name in mjt.class_names(hh):
            for mn, md, _ma in mjt.methods((hh, name)):
                n += len(identity_rethrows(hh, (hh, name, mn, md)))
        return n

    n0 = count_idents(h)
    check("identity found", n0 == 2, str(n0))
    for name in mjt.class_names(h):
        for mn, md, _ma in mjt.methods((h, name)):
            for hr in identity_rethrows(h, (h, name, mn, md)):
                hr.remove()
    rep = save_verified(h, os.path.join(tmp, "mjt_pysdk_stripped.jar"))
    check("save_verified classes", rep["classes"] == 5, str(rep))
    h2 = mjt.open_jar(rep["path"])
    n1 = count_idents(h2)
    mjt.close_container(h2)
    check("identity gone", n1 == 0, str(n1))
    mjt.close_container(h)

    # ── 3. 内置单 pass（run_pass 冒烟）+ 报告读出 ──
    p3, h = fresh("runpass")
    check("passes listed", "NopRemover" in mjt.passes() and "ClassMemberSorter" in mjt.passes(),
          str(mjt.passes()))
    stats = mjt.run_pass(h, "ClassMemberSorter")
    check("stats pipeline", stats.pipeline == "ClassMemberSorter", str(stats))
    slots = [s for s in stats.slots if s["name"] == "ClassMemberSorter"]
    check("stats slot present", len(slots) == 1, str(stats.slots))
    check("slot scope class", slots and slots[0]["scope"] == "class", str(slots))
    check("last_run_report alias", mjt.last_run_report(h) is not None
          and mjt.last_run_report(h).pipeline == "ClassMemberSorter")
    residual = residual_sites(h)
    # 合成样本的 dec 站点无 crypto 锚，判据保守保留——记录口径即可：
    dump_sites(residual, os.path.join(tmp, "mjt_pysdk_ledger.tsv"))
    check("ledger written",
          os.path.getsize(os.path.join(tmp, "mjt_pysdk_ledger.tsv")) > 0)
    mjt.close_container(h)

    # ── 4. MethodPass 类型锚（适配器实现属私有侧管线面；基座面保锚形状） ──
    class PyAnchorPass(mjt.MethodPass):
        name = "py-selftest-anchor"

    anchor = PyAnchorPass()
    check("pass anchor", isinstance(anchor, mjt.MethodPass), str(type(anchor)))
    check("changed repr", repr(mjt.changed()) == "PreservedAnalyses.none",
          repr(mjt.changed()))
    check("preserved repr", repr(mjt.preserved_all()) == "PreservedAnalyses.all",
          repr(mjt.preserved_all()))

    # ── 5. rename token + 双 sidecar 语义 ──
    p5, h = fresh("rename")
    mjt.rename_begin(h)
    mjt.token_rename_class(h, "me.demo.Util", "me.demo.Helper")   # 点分入口（规范化）
    mjt.token_rename_method(h, "me/demo/Crypto", "dec",
                            "(II)Ljava/lang/String;", "decode2")
    check("mapping counts", mjt.mapping_counts(h) == (1, 1, 0), str(mjt.mapping_counts(h)))
    mjt.rename_commit(h)
    check("renamed class present", "me/demo/Helper" in mjt.class_names(h),
          str(mjt.class_names(h)))
    check("old class gone", "me/demo/Util" not in mjt.class_names(h))
    check("renamed method",
          any(mn == "decode2" for mn, _md, _ma in mjt.methods((h, "me/demo/Crypto"))))
    check("refs remapped",
          any(d["owner"] == "me/demo/Helper"
              for name in mjt.class_names(h)
              for mn, md, _ma in mjt.methods((h, name))
              for uid, op, kind in mjt.insns((h, name, mn, md))
              for d in [mjt.insn_detail((h, name, mn, md), uid)]
              if d["kind"] == "method"),
          "App->Helper 引用未随改")
    sidecar = os.path.join(tmp, "mjt_pysdk_phase2.mappings")
    mjt.mapping_dump(h, sidecar, "enigma")
    check("sidecar written", os.path.getsize(sidecar) > 0)
    mjt.close_container(h)

    # ── 6. dry_run（分叉试算 + verify 门 + diff） ──
    p6, h = fresh("dryrun")

    def strip_guards(mref):
        gs = guard_patterns(mref[0], mref)
        for g in gs:
            g.remove()
        return bool(gs)

    draft = dry_run(h, (h, "me/demo/Util"), strip_guards)
    check("dryrun verify ok", draft.verify_ok, draft.summary)
    check("dryrun no-op diff", draft.changed is False, draft.summary)

    def breaker(mref):
        for uid, op, _kind in list(mjt.insns(mref)):
            if mjt.insn_detail(mref, uid)["mnemonic"] in ("ireturn", "areturn", "return"):
                mjt.remove_insn(mref, uid)
        return True

    bad = dry_run(h, (h, "me/demo/Util"), breaker)
    check("dryrun rejects broken", not bad.verify_ok, bad.summary)
    try:
        bad.apply()
        check("apply refuses broken", False, "no exception")
    except RuntimeError:
        check("apply refuses broken", True)
    mjt.close_container(h)

    # ── 7. 字节往返 + 普查（fresh jar 上做，不依赖 phase5 改名） ──
    p7, h = fresh("bytes")
    raw = mjt.class_bytes(h, "me/demo/Util")
    check("raw bytes", raw[:4] == bytes.fromhex("CAFEBABE"), raw[:8].hex())
    scratch = mjt.new_container()
    mjt.add_entry(scratch, "Util.class", raw)
    sref = (scratch, "me/demo/Util")
    mjt.parse_class(sref)
    check("parse name", mjt.class_info(sref)["name"] == "me/demo/Util")
    mjt.write_back(sref, compute_frames=False)
    b2 = mjt.class_bytes(scratch, "me/demo/Util")
    check("write roundtrip", b2[:4] == bytes.fromhex("CAFEBABE"), b2[:8].hex())
    mjt.close_container(scratch)
    census = string_census(h, min_len=4)
    texts = [t for t, _ in census.top(50)]
    check("census has payload", any("HIDDEN" in t or "demo" in t for t in texts),
          str(texts[:5]))
    check("sites heuristic", isinstance(decryptor_sites(h), list))
    mjt.close_container(h)

    # ── 8. 管线写回链路（run_pass 后容器仍可 save_verified 全绿） ──
    p8, h = fresh("clean")
    mjt.run_pass(h, "NopRemover")
    rep = save_verified(h, os.path.join(tmp, "mjt_pysdk_runpass.jar"))
    check("runpass saved clean", rep["classes"] == 5 and rep["skipped"] == [], str(rep))
    mjt.close_container(h)

    # ── 9. fail-closed 异常面 + 完整性体检 + rename→export→verify 闭环 ──
    p9, h = fresh("verify")
    try:
        mjt.class_bytes(h, "me/demo/Nope")
        check("stale key raises KeyError", False, "no exception")
    except mjt.StaleRefError:
        check("stale key raises KeyError", True)
    except Exception as e:                                   # noqa: BLE001
        check("stale key raises KeyError", False, f"{type(e).__name__}: {e}")
    try:
        mjt.class_names(999)
        check("stale handle raises", False, "no exception")
    except mjt.StaleRefError:
        check("stale handle raises", True)
    except Exception as e:                                   # noqa: BLE001
        check("stale handle raises", False, f"{type(e).__name__}: {e}")
    rep0 = mjt.verify_container(h)
    check("verify clean", rep0["match"] == 5 and rep0["mismatch"] == 0
          and rep0["malformed"] == 0, str(rep0))
    check("sanitize noop", mjt.sanitize_lvt(h) == (0, 0, 5), str(mjt.sanitize_lvt(h)))
    # G2 回归位：rename → 不 flush 直接 export → 新键可见 + entry/声明一致。
    mjt.rename_class((h, "me/demo/Util"), "me/demo/Renamed")
    mjt.export_jar(h, os.path.join(tmp, "mjt_pysdk_synced.jar"))
    hr = mjt.open_jar(os.path.join(tmp, "mjt_pysdk_synced.jar"))
    names = mjt.class_names(hr)
    check("export kept rename", "me/demo/Renamed" in names and "me/demo/Util" not in names,
          str(names))
    rep1 = mjt.verify_container(hr)
    check("export verify clean", rep1["mismatch"] == 0 and rep1["malformed"] == 0, str(rep1))
    mjt.close_container(hr)
    mjt.close_container(h)

    # ── 10. class_hierarchy 继承闭包（super 链 + 接口闭包 + 沿途成员集） ──
    p10, h = fresh("hier")
    hier = mjt.class_hierarchy((h, "me/demo/Color"))
    check("hier closure", [c["name"] for c in hier["classes"]]
          == ["me/demo/Color", "me/demo/Api"], str(hier["classes"]))
    check("hier iface", hier["classes"][0]["interfaces"] == ["me/demo/Api"],
          str(hier["classes"][0]))
    check("hier super", hier["classes"][0]["super"] == "java/lang/Object",
          str(hier["classes"][0]))
    owners = {m[0] for m in hier["methods"]}
    check("hier member owners", "me/demo/Api" in owners and "me/demo/Color" in owners,
          str(owners))
    check("hier values present",
          any(o == "me/demo/Color" and n == "values"
              for o, n, _d, _a in hier["methods"]),
          str([m for m in hier["methods"] if m[1] == "values"]))
    check("hier $VALUES field",
          any(o == "me/demo/Color" and n == "$VALUES" and d == "[Lme/demo/Color;"
              for o, n, d, _a in hier["fields"]),
          str(hier["fields"]))
    hier2 = mjt.class_hierarchy((h, "me/demo/Crypto"))
    check("hier self only", [c["name"] for c in hier2["classes"]] == ["me/demo/Crypto"],
          str(hier2["classes"]))
    try:
        mjt.class_hierarchy((h, "me/demo/Nope"))
        check("hier stale raises", False, "no exception")
    except mjt.StaleRefError:
        check("hier stale raises", True)
    except Exception as e:                                   # noqa: BLE001
        check("hier stale raises", False, f"{type(e).__name__}: {e}")
    mjt.close_container(h)

    # ── 11. enum values() 数组型 owner 随类改名（悬空回归位，minzi 姿态） ──
    # demo 罐的 me/demo/Color.values() 经 `invokevirtual [Lme/demo/Color;.clone()`
    # 克隆 $VALUES 数组；类改名后该数组型 owner 的元素类名必须同步改写，
    # 否则 receiver（[L新名;）与 owner（[L旧名;）失配、真机一调 values() 即炸
    # （entry 探针与 -Xverify:all 都抓不到，须独立扫描）。
    p11, h = fresh("enumrename")

    def dangling_real(hh):
        present = set(mjt.class_names(hh))
        real = 0
        for cn in mjt.class_names(hh):
            for mn, md, _ma in mjt.methods((hh, cn)):
                for uid, _op, kind in mjt.insns((hh, cn, mn, md)):
                    d = mjt.insn_detail((hh, cn, mn, md), uid)
                    if kind == "method" and d["owner"].startswith("[L") \
                            and d["owner"][2:-1] not in present:
                        real += 1
        return real

    mjt.rename_begin(h)
    mjt.token_rename_class(h, "me.demo.Color", "obf.Color0")   # 点分入口（规范化）
    mjt.rename_commit(h)
    check("enum renamed", "obf/Color0" in mjt.class_names(h)
          and "me/demo/Color" not in mjt.class_names(h), str(mjt.class_names(h)))
    check("array owner real=0", dangling_real(h) == 0, str(dangling_real(h)))
    vals = mjt.get_method((h, "obf/Color0"), "values", "()[Lobf/Color0;")
    clone_owners = [d["owner"] for uid, _op, kind in mjt.insns(vals)
                    for d in [mjt.insn_detail(vals, uid)] if kind == "method"]
    check("clone owner follows rename", clone_owners == ["[Lobf/Color0;"], str(clone_owners))
    mjt.close_container(h)

    # ── 12. rename_apply_suggestions 被拒流水（fail-closed 报错面） ──
    # 跨类撞名拒收时被拒条目自含 owner/old/new/撞名对象定位，脚本可直接读。
    p12, h = fresh("suggest")
    sugg_path = os.path.join(tmp, "mjt_pysdk_suggest.json")
    with open(sugg_path, "w", encoding="utf-8") as fp:
        json.dump({
            "version": 1,
            "source": "ai:selftest",
            "proposals": [
                {"kind": "class", "name": "me/demo/Util",
                 "newName": "me/demo/Taken", "confidence": 1.0},
                {"kind": "class", "name": "me/demo/Api",
                 "newName": "me/demo/Taken", "confidence": 1.0},
            ],
        }, fp)
    rep = mjt.rename_apply_suggestions(h, sugg_path, "bytecode-only")
    check("sugg counts", rep[0] == 2 and rep[1] == 1 and rep[2] == 1, str(rep[:4]))
    entry = rep[4][0]
    check("sugg entry coords",
          entry["kind"] == "class" and entry["name"] == "me/demo/Api"
          and entry["new_name"] == "me/demo/Taken" and entry["reason"] == "conflict"
          and "me/demo/Util" in entry["detail"] and "me/demo/Taken" in entry["detail"],
          str(entry))
    mjt.close_container(h)

    # ── 13. fold_to_ldc（解密点回填：实参同摘 + 栈产物等位顶替 + fail-closed 边界） ──
    # 合成 int 回填位全链验证：site = iconst_3; iconst_4; invokestatic combine(II)I;
    # ireturn → 折叠后 = ldc 42; ireturn（两常量实参同摘，栈一致过 COMPUTE_FRAMES）。
    # str 载荷绑定层 intern 后按值安全（下方 str 回填位做值往返断言）。
    p13, h = fresh("fold")
    util_cref = (h, "me/demo/Util")
    op = mjt.opcodes
    mjt.add_method(util_cref, "combine", "(II)I",
                   mjt.acc["PUBLIC"] | mjt.acc["STATIC"], 2, 2)
    combine = (h, "me/demo/Util", "combine", "(II)I")
    mjt.emit_var(combine, op["iload"], 0)
    mjt.emit_var(combine, op["iload"], 1)
    mjt.emit_insn(combine, op["iadd"])
    mjt.emit_insn(combine, op["ireturn"])
    mjt.add_method(util_cref, "site", "()I",
                   mjt.acc["PUBLIC"] | mjt.acc["STATIC"], 2, 2)
    site = (h, "me/demo/Util", "site", "()I")
    mjt.emit_insn(site, op["iconst_3"])
    mjt.emit_insn(site, op["iconst_4"])
    call_uid = mjt.emit_method_call(site, op["invokestatic"], "me/demo/Util",
                                    "combine", "(II)I", False)
    mjt.emit_insn(site, op["ireturn"])
    ldc_uid = fold_to_ldc(h, site, call_uid, 42)
    body1 = [(uid, mjt.insn_detail(site, uid)) for uid, o, k in mjt.insns(site)]
    real1 = [d for uid, d in body1 if d["kind"] not in ("label", "line", "frame")]
    check("folded body shape", [d["mnemonic"] for d in real1] == ["ldc", "ireturn"],
          str([d["mnemonic"] for d in real1]))
    check("folded ldc value+uid", len(body1) == 2 and body1[0][1]["kind"] == "ldc"
          and body1[0][1]["cst"] == 42 and body1[0][0] == ldc_uid, str(body1))
    try:
        mjt.insn_detail(site, call_uid)
        check("fold removed call uid", False, "no exception")
    except mjt.StaleRefError:
        check("fold removed call uid", True)
    # 非常量实参拒折（main 的 decode 调用前面是 invokevirtual，非纯常量压栈），
    # 且拒绝必须原子——不落任何改动。
    amref = mjt.get_method((h, "me/demo/App"), "main", "([Ljava/lang/String;)V")
    main_call = [uid for uid, o, k in mjt.insns(amref)
                 if k == "method" and mjt.insn_detail(amref, uid)["name"] == "decode"][0]
    n_before = mjt.insn_count(amref)
    try:
        fold_to_ldc(h, amref, main_call, 0)
        check("fold rejects non-const args", False, "no exception")
    except ValueError:
        check("fold rejects non-const args", True)
    except Exception as e:                                   # noqa: BLE001
        check("fold rejects non-const args", False, f"{type(e).__name__}: {e}")
    check("fold rejection atomic", mjt.insn_count(amref) == n_before,
          f"{n_before} -> {mjt.insn_count(amref)}")
    # 带 receiver 的 invokevirtual 拒折。
    println_uid = [uid for uid, o, k in mjt.insns(amref)
                   if k == "method"
                   and mjt.insn_detail(amref, uid)["mnemonic"] == "invokevirtual"][0]
    try:
        fold_to_ldc(h, amref, println_uid, 0)
        check("fold rejects receiver call", False, "no exception")
    except ValueError:
        check("fold rejects receiver call", True)
    # str 载荷值往返：合成 String 回填位 site_str = iconst_3; iconst_4;
    # invokestatic decode(II)Ljava/lang/String;; areturn → ldc "PLAIN"; areturn。
    mjt.add_method(util_cref, "site_str", "(I)Ljava/lang/String;",
                   mjt.acc["PUBLIC"] | mjt.acc["STATIC"], 2, 1)
    sref = (h, "me/demo/Util", "site_str", "(I)Ljava/lang/String;")
    mjt.emit_insn(sref, op["iconst_3"])
    mjt.emit_insn(sref, op["iconst_4"])
    scall = mjt.emit_method_call(sref, op["invokestatic"], "me/demo/Crypto",
                                 "decode", "(II)Ljava/lang/String;", False)
    mjt.emit_insn(sref, op["areturn"])
    sldc = fold_to_ldc(h, sref, scall, "PLAIN")
    sbody = [(uid, mjt.insn_detail(sref, uid)) for uid, o, k in mjt.insns(sref)]
    sreal = [d for uid, d in sbody if d["kind"] not in ("label", "line", "frame")]
    check("fold str body shape", [d["mnemonic"] for d in sreal] == ["ldc", "areturn"],
          str([d["mnemonic"] for d in sreal]))
    check("fold str value", sreal[0]["cst"] == "PLAIN" and sbody[0][0] == sldc,
          str(sbody[0][1].get("cst")))
    # 合法性闸 + 落盘复验（int 路径全链：写回 → save → 重开可见）。
    mjt.write_back((h, "me/demo/Util"), compute_frames=True)
    rep = save_verified(h, os.path.join(tmp, "mjt_pysdk_folded.jar"))
    h3 = mjt.open_jar(rep["path"])
    site2 = mjt.get_method((h3, "me/demo/Util"), "site", "()I")
    plain = [mjt.insn_detail(site2, uid)["cst"] for uid, o, k in mjt.insns(site2)
             if k == "ldc"]
    check("fold survives roundtrip", plain == [42], str(plain))
    sstr2 = mjt.get_method((h3, "me/demo/Util"), "site_str", "(I)Ljava/lang/String;")
    splain = [mjt.insn_detail(sstr2, u)["cst"] for u, o, k in mjt.insns(sstr2)
              if k == "ldc"]
    check("fold str survives roundtrip", splain == ["PLAIN"], str(splain))
    mjt.close_container(h3)
    mjt.close_container(h)

    # ── 14. clone_method（体逐条重建：异常表/跳转重绑/var/field/虚调/int ldc） ──
    p14, h = fresh("clone")
    src = mjt.get_method((h, "me/demo/Crypto"), "guarded", "()V")
    info0 = mjt.method_info(src)
    clone = clone_method(h, src, "guarded2")
    info1 = mjt.method_info(clone)
    check("clone header", info1["name"] == "guarded2" and info1["desc"] == info0["desc"]
          and info1["access"] == info0["access"]
          and info1["max_stack"] == info0["max_stack"]
          and info1["max_locals"] == info0["max_locals"]
          and info1["exceptions"] == info0["exceptions"], str(info1))
    src_d = [(uid, mjt.insn_detail(src, uid)) for uid, o, k in mjt.insns(src)]
    new_d = [(uid, mjt.insn_detail(clone, uid)) for uid, o, k in mjt.insns(clone)]
    check("clone node count", len(src_d) == len(new_d),
          f"{len(src_d)} vs {len(new_d)}")
    check("clone kinds", [d["kind"] for _u, d in src_d] == [d["kind"] for _u, d in new_d])
    _skip = {"uid", "target"}   # uid 键面不同（新方法新号）；target 由下条对位验证
    check("clone insn fields", all(
        {k: v for k, v in a.items() if k not in _skip}
        == {k: v for k, v in b.items() if k not in _skip}
        for (_ua, a), (_ub, b) in zip(src_d, new_d)),
        "逐节点字段（除 uid/target）应一致")
    src_labels = [uid for uid, d in src_d if d["kind"] == "label"]
    new_labels = [uid for uid, d in new_d if d["kind"] == "label"]
    jump_ok = all(new_labels[src_labels.index(a["target"])] == b["target"]
                  for (_ua, a), (_ub, b) in zip(src_d, new_d)
                  if a["kind"] == "jump")
    check("clone jump retarget", jump_ok, "跳转须落在同位次克隆 label 上")
    rows0 = mjt.try_catches(src)
    rows1 = mjt.try_catches(clone)
    check("clone try row count+type", len(rows1) == len(rows0) == 1
          and rows1[0][3] == rows0[0][3], str(rows1))
    check("clone try labels", all(u in new_labels for u in rows1[0][:3]), str(rows1[0]))
    # var 体克隆（Util.add）。
    add_src = mjt.get_method((h, "me/demo/Util"), "add", "(II)I")
    add_clone = clone_method(h, add_src, "add2")
    add_d = [mjt.insn_detail(add_clone, u) for u, o, k in mjt.insns(add_clone)
             if k not in ("label", "line", "frame")]
    check("clone var body", [d["mnemonic"] for d in add_d]
          == ["iload", "iload", "iadd", "ireturn"]
          and [d.get("var") for d in add_d[:2]] == [0, 1], str(add_d))
    # field/type/虚调体克隆（Color.values）。
    vals_src = mjt.get_method((h, "me/demo/Color"), "values", "()[Lme/demo/Color;")
    vals_clone = clone_method(h, vals_src, "values2")
    vals_d = [mjt.insn_detail(vals_clone, u) for u, o, k in mjt.insns(vals_clone)
              if k not in ("label", "line", "frame")]
    check("clone values body",
          [d["mnemonic"] for d in vals_d]
          == ["getstatic", "invokevirtual", "checkcast", "areturn"]
          and vals_d[0]["name"] == "$VALUES"
          and vals_d[0]["desc"] == "[Lme/demo/Color;"
          and vals_d[2]["desc"] == "[Lme/demo/Color;",
          str([(d["mnemonic"], d.get("name"), d.get("desc")) for d in vals_d]))
    # int ldc 克隆（emit_ldc int 按值安全）。
    mjt.add_method((h, "me/demo/Util"), "konst", "()I",
                   mjt.acc["PUBLIC"] | mjt.acc["STATIC"], 1, 1)
    konst = (h, "me/demo/Util", "konst", "()I")
    mjt.emit_ldc(konst, 7)
    mjt.emit_insn(konst, op["ireturn"])
    konst_clone = clone_method(h, konst, "konst2")
    konst_d = [mjt.insn_detail(konst_clone, u) for u, o, k in mjt.insns(konst_clone)
               if k not in ("label", "line", "frame")]
    check("clone int ldc", [d["mnemonic"] for d in konst_d] == ["ldc", "ireturn"]
          and konst_d[0]["cst"] == 7 and konst_d[0]["cst_tag"] == 2, str(konst_d))
    # 直调体克隆（decode：iconst_1 / bipush→等值 int ldc / invokestatic / areturn；
    # emit 面无 IntInsnNode 原语，bipush 7 栈等位重建为 ldc 7——字节形态有别、语义等价）。
    dec_src = mjt.get_method((h, "me/demo/Crypto"), "decode", "(II)Ljava/lang/String;")
    dec_clone = clone_method(h, dec_src, "decode2")
    dec_d = [mjt.insn_detail(dec_clone, u) for u, o, k in mjt.insns(dec_clone)
             if k not in ("label", "line", "frame")]
    check("clone decode body",
          [d["mnemonic"] for d in dec_d] == ["iconst_1", "ldc", "invokestatic", "areturn"]
          and dec_d[1]["kind"] == "ldc" and dec_d[1]["cst"] == 7
          and dec_d[2]["owner"] == "me/demo/Crypto" and dec_d[2]["name"] == "dec"
          and dec_d[2]["desc"] == "(II)Ljava/lang/String;",
          str([(d["mnemonic"], d.get("operand"), d.get("cst")) for d in dec_d]))
    # str ldc 克隆（cst_tag 1 绑定层 intern 后按值安全；demo 带 str ldc 的是 dec）。
    dec_src = mjt.get_method((h, "me/demo/Crypto"), "dec", "(II)Ljava/lang/String;")
    dec2 = clone_method(h, dec_src, "dec2")
    src_ldcs = [mjt.insn_detail(dec_src, u)["cst"]
                for u, o, k in mjt.insns(dec_src) if k == "ldc"]
    new_ldcs = [mjt.insn_detail(dec2, u)["cst"]
                for u, o, k in mjt.insns(dec2) if k == "ldc"]
    check("clone str ldc value", src_ldcs != [] and new_ldcs == src_ldcs,
          f"{src_ldcs} vs {new_ldcs}")
    mjt.write_back((h, "me/demo/Crypto"), compute_frames=True)
    mjt.write_back((h, "me/demo/Util"), compute_frames=True)
    mjt.write_back((h, "me/demo/Color"), compute_frames=True)
    rep = save_verified(h, os.path.join(tmp, "mjt_pysdk_cloned.jar"))
    check("clone saved clean", rep["classes"] == 5 and rep["skipped"] == [], str(rep))
    h3 = mjt.open_jar(rep["path"])
    g2 = mjt.get_method((h3, "me/demo/Crypto"), "guarded2", "()V")
    check("clone reopen try rows", len(mjt.try_catches(g2)) == 1
          and mjt.try_catches(g2)[0][3] == "java/io/IOException", str(mjt.try_catches(g2)))
    k2 = mjt.get_method((h3, "me/demo/Util"), "konst2", "()I")
    check("clone reopen int ldc",
          [mjt.insn_detail(k2, u)["cst"] for u, o, k in mjt.insns(k2) if k == "ldc"] == [7])
    d2 = mjt.get_method((h3, "me/demo/Crypto"), "dec2", "(II)Ljava/lang/String;")
    check("clone reopen str ldc",
          [mjt.insn_detail(d2, u)["cst"] for u, o, k in mjt.insns(d2) if k == "ldc"]
          == src_ldcs)
    mjt.close_container(h3)
    mjt.close_container(h)

    # ── 15. dream2 折叠路径（普查 → 批口信任门/守卫 → 残留台账口径） ──
    p15, h = fresh("dream2")
    sites0 = decryptor_sites(h)
    check("dream2 census", len(sites0) == 1 and sites0[0].cls == "me/demo/Crypto"
          and sites0[0].mnemonic == "invokestatic", str([(s.cls, s.method) for s in sites0]))
    skipped_run = fold_decryptor_sites(h, lambda s: None)    # 信任门=None 全跳过
    check("dream2 skip gate", skipped_run == [] and len(residual_sites(h)) == 1,
          str(skipped_run))
    # str 载荷批量回填（绑定层 intern 后按值安全）：evaluator 给明文 → 全站折叠
    # → 残留台账归零；decode 体从「载荷+invokestatic」收成单条 ldc。
    folded_run = fold_decryptor_sites(h, lambda s: "PLAIN")
    check("dream2 str fold", len(folded_run) == 1
          and folded_run[0][1] is not None, str(folded_run))
    dec_after = mjt.insn_detail((h, "me/demo/Crypto", "decode",
                                 "(II)Ljava/lang/String;"),
                                folded_run[0][1])
    check("dream2 str fold value", dec_after["cst"] == "PLAIN", str(dec_after.get("cst")))
    check("dream2 residual drained", len(residual_sites(h)) == 0,
          str(len(residual_sites(h))))
    mjt.close_container(h)

    # ── 16. call_sites 定点普查 + fold_call_sites（str 键解密器，W1/W2 正式出路） ──
    # 启发式 decryptor_sites 刻意保持 int 校准（阶段 15 口径不变）；str 键解密器
    # 由脚本定点：call_sites 按 (owner,name,desc) 抓全 → 实参常量值元组交
    # value_of 就地解密 → ldc 回填。旧 fold 实参门拒 str ldc（W2），此处即回归位。
    p16, h = fresh("callsites")
    op = mjt.opcodes
    mjt.add_method((h, "me/demo/Util"), "strdec", "(Ljava/lang/String;I)Ljava/lang/String;",
                   mjt.acc["PUBLIC"] | mjt.acc["STATIC"], 2, 2)
    sdec = (h, "me/demo/Util", "strdec", "(Ljava/lang/String;I)Ljava/lang/String;")
    mjt.emit_var(sdec, op["aload"], 0)
    mjt.emit_insn(sdec, op["areturn"])          # 恒等体（折叠路径不执行它）

    def make_site(name, key, n):
        mjt.add_method((h, "me/demo/Util"), name, "()Ljava/lang/String;",
                       mjt.acc["PUBLIC"] | mjt.acc["STATIC"], 3, 1)
        sref = (h, "me/demo/Util", name, "()Ljava/lang/String;")
        mjt.emit_ldc(sref, key)                 # str ldc 实参（旧门拒收形态）
        mjt.emit_ldc(sref, n)                   # int ldc 实参
        mjt.emit_method_call(sref, op["invokestatic"], "me/demo/Util",
                             "strdec", "(Ljava/lang/String;I)Ljava/lang/String;", False)
        mjt.emit_insn(sref, op["areturn"])

    make_site("site2", "SALT", 7)
    make_site("site3", "K", 1)
    # 同方法双调用点（uid 降序防御序的覆盖位）：site4 = 两个 strdec 调用串联。
    mjt.add_method((h, "me/demo/Util"), "site4", "()Ljava/lang/String;",
                   mjt.acc["PUBLIC"] | mjt.acc["STATIC"], 3, 1)
    s4 = (h, "me/demo/Util", "site4", "()Ljava/lang/String;")
    mjt.emit_ldc(s4, "AB")
    mjt.emit_ldc(s4, 2)
    mjt.emit_method_call(s4, op["invokestatic"], "me/demo/Util",
                         "strdec", "(Ljava/lang/String;I)Ljava/lang/String;", False)
    mjt.emit_ldc(s4, "CD")
    mjt.emit_ldc(s4, 3)
    mjt.emit_method_call(s4, op["invokestatic"], "me/demo/Util",
                         "strdec", "(Ljava/lang/String;I)Ljava/lang/String;", False)
    mjt.emit_insn(s4, op["areturn"])
    dec_sites = call_sites(h, "me/demo/Util", "strdec",
                           "(Ljava/lang/String;I)Ljava/lang/String;")
    check("callsites found", len(dec_sites) == 4
          and all(s.cls == "me/demo/Util" and s.mnemonic == "invokestatic"
                  for s in dec_sites), str([(s.cls, s.method) for s in dec_sites]))
    # 跨类通配：Crypto.dec 被 decode 内部调用（owner 匹配、name 通配）。
    dec_hits = call_sites(h, owner="me/demo/Crypto", name="dec")
    check("callsites wildcard", len(dec_hits) == 1
          and dec_hits[0].cls == "me/demo/Crypto"
          and dec_hits[0].name == "dec", str([(s.cls, s.method) for s in dec_hits]))
    # 信任门：value_of 全 None → 零折叠。
    check("callsites skip gate",
          fold_call_sites(h, ("me/demo/Util", "strdec",
                              "(Ljava/lang/String;I)Ljava/lang/String;"),
                          lambda args: None) == []
          and len(call_sites(h, name="strdec")) == 4, "skip gate 后站点应原样")
    # 批折：实参值按声明序给脚本，就地解密（倒序 + 计数即一段"解密逻辑"）。
    # 同方法双站走 uid 降序防御序。
    folded = fold_call_sites(h, ("me/demo/Util", "strdec",
                                 "(Ljava/lang/String;I)Ljava/lang/String;"),
                             lambda args: args[0][::-1] + "!" + str(args[1]))
    check("callsites folded", len(folded) == 4
          and all(uid is not None for _s, uid in folded), str(folded))
    want = {"site2": ["TLAS!7"], "site3": ["K!1"], "site4": ["BA!2", "DC!3"]}
    shapes = {"site2": ["ldc", "areturn"], "site3": ["ldc", "areturn"],
              "site4": ["ldc", "ldc", "areturn"]}
    got = {}
    for name, want_shape in shapes.items():
        m2 = (h, "me/demo/Util", name, "()Ljava/lang/String;")
        body = [mjt.insn_detail(m2, u) for u, o, k in mjt.insns(m2)
                if k not in ("label", "line", "frame")]
        check(f"callsites {name} shape", [d["mnemonic"] for d in body] == want_shape,
              str([d["mnemonic"] for d in body]))
        got[name] = [d["cst"] for d in body if d["kind"] == "ldc"]
    check("callsites values", got == want, str(got))
    check("callsites drained", call_sites(h, name="strdec") == [],
          "折叠后 strdec 调用点应清零")
    # 实例 callee 守卫：println 是 invokevirtual，整体拒收（fail-closed，不静默空转）。
    v_hits = call_sites(h, name="println")
    check("callsites instance present", len(v_hits) >= 1
          and v_hits[0].mnemonic == "invokevirtual", str(v_hits))
    try:
        fold_call_sites(h, (v_hits[0].owner, "println", v_hits[0].desc),
                        lambda args: None)
        check("callsites rejects instance callee", False, "no exception")
    except ValueError:
        check("callsites rejects instance callee", True)
    # 非常量实参 fail-closed：decode 在 main 里的调用点前面是 invokevirtual。
    try:
        fold_call_sites(h, ("me/demo/Crypto", "decode",
                            "(II)Ljava/lang/String;"), lambda args: "x")
        check("callsites rejects non-const", False, "no exception")
    except ValueError:
        check("callsites rejects non-const", True)
    # 落盘复验：折叠值过 COMPUTE_FRAMES + 重开可见。
    mjt.write_back((h, "me/demo/Util"), compute_frames=True)
    rep = save_verified(h, os.path.join(tmp, "mjt_pysdk_callsites.jar"))
    check("callsites saved clean", rep["skipped"] == [], str(rep))
    h4 = mjt.open_jar(rep["path"])
    s2 = mjt.get_method((h4, "me/demo/Util"), "site2", "()Ljava/lang/String;")
    vals = [mjt.insn_detail(s2, u)["cst"] for u, o, k in mjt.insns(s2) if k == "ldc"]
    check("callsites reopen", vals == ["TLAS!7"], str(vals))
    mjt.close_container(h4)
    mjt.close_container(h)

    return fails
