# pysdk_selftest.py — mjt-py SDK 绑定层自测（由 mjt_pysdk_unittest 嵌入执行）。
#
# v3 函数面（Ref=键）25 阶段：装载/导航 → util 判据 + 直改直删 + verify 落盘 →
# 内置单 pass（run_pass）+ 报告读出 → MethodPass 类型锚（适配器属私有侧管线面）→
# rename token + sidecar → dry_run（分叉试算 + verify 门）→ 字节往返 + 普查 →
# 管线写回链路 → fail-closed 异常面 + 容器体检 + rename→export→verify 闭环 →
# class_hierarchy 继承闭包 → enum values() 数组型 owner 随类改名（悬空回归位）→
# rename_apply_suggestions 被拒流水 → fold_to_ldc 解密点回填（实参同摘 +
# fail-closed 边界）→ clone_method 体逐条重建（异常表/跳转/直调/虚调）→
# dream2 折叠路径（普查→回填→残留归零）→ call_sites 定点普查 + fold_call_sites
# （str 键解密器批折 + 实例 callee 守卫）→ 协作层 deadline → Jasm 文本面 →
# jar_diff（自身零差异 + 单类修改识别）→ jar_fix（三类问题扫描 + 开关 +
# apply 移除）→ pass 管线面（pass_describe/run_pipeline 互斥面/opt_preview
# 试算零改动/opt_apply 落盘）→ analysis_evaluate（求值/化简/绑定/fail-closed）→
# shadow sidecar 文件面（generate 幂等/prune 失配清理/缺侧车报错）→
# decompiler_compare（fail-closed + fake java 双后端实跑）→ init_order（静态
# 初始化拓扑序：超类边 + clinit 引用边 + 环报告 + roots 子图）。
# 全程合成样本（mjt.write_demo_jar），零样本字节入库。
# 文件尾有 __main__ 兜底入口：--script 宿主 exec 的 __name__ 是 "__main__"，
# 直接 `--script pysdk_selftest.py` 即跑全套并按 fail 数定退出码。


def run_all():
    import json
    import os
    import tempfile

    import mjt
    from mjt.util import (call_sites, clone_method, decryptor_sites,
                          dry_run, dump_sites, fold_call_sites,
                          fold_decryptor_sites, fold_to_ldc, guard_patterns,
                          identity_rethrows, init_order, native_table,
                          residual_sites, save_verified, string_census)

    fails = []

    def check(name, cond, detail=""):
        if not cond:
            fails.append(f"{name}: {detail}")

    #干净环境冒烟（env -i）没有 TEMP/TMP，gettempdir 在 Windows 上会退到
    #C:\Windows 且不可写。此时以脚本所在目录 .smoke-tmp 兜底（交付目录必可写），
    #并回写进程环境——C++ 侧（反编译工作目录走 GetTempPath）同享兜底。
    tmp = tempfile.gettempdir()
    fallback_tmp = None
    if not (os.environ.get("TEMP") or os.environ.get("TMP") or os.environ.get("TMPDIR")):
        fallback_tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".smoke-tmp")
        os.makedirs(fallback_tmp, exist_ok=True)
        tmp = fallback_tmp
        os.environ["TEMP"] = tmp
        os.environ["TMP"] = tmp

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

    # ── 14b. native_table（跨容器 native 扫描表：glob 过滤/排序/位面） ──
    p14b, h = fresh("native")
    mjt.add_method((h, "me/demo/Util"), "nativeProbe", "()V",
                   mjt.acc["NATIVE"] | mjt.acc["PUBLIC"], 0, 0)
    mjt.add_method((h, "me/demo/Util"), "nativeStatic", "(I)I",
                   mjt.acc["NATIVE"] | mjt.acc["STATIC"] | mjt.acc["PUBLIC"], 0, 0)
    rows = native_table(h)
    check("native rows", [r["name"] for r in rows] == ["nativeProbe", "nativeStatic"],
          str(rows))
    check("native row shape",
          rows[0]["class"] == "me/demo/Util" and rows[0]["descriptor"] == "()V"
          and rows[0]["static"] is False and rows[1]["static"] is True
          and rows[1]["access"] & mjt.acc["NATIVE"], str(rows))
    check("native filter desc",
          [r["name"] for r in native_table(h, desc="(I)I")] == ["nativeStatic"])
    check("native filter name",
          [r["name"] for r in native_table(h, name="nativeStatic")] == ["nativeStatic"])
    check("native filter cls",
          all(r["class"] == "me/demo/Util" for r in native_table(h, cls="me/demo/*")))
    check("native empty", native_table(h, cls="no/such/*") == [])
    # 落盘重开保真：native 方法无 Code 属性，COMPUTE_FRAMES 写回后仍可扫出。
    out14b = os.path.join(tmp, "mjt_pysdk_native_out.jar")
    mjt.save(h, out14b)
    h14b = mjt.open_jar(out14b)
    check("native reopen", [r["name"] for r in native_table(h14b)]
          == ["nativeProbe", "nativeStatic"], str(native_table(h14b)))
    mjt.close_container(h14b)
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

    # ── 17. 协作层 deadline（set_deadline_ms → 长操作检查点抛 DeadlineExceeded） ──
    p17, h17 = fresh("limits")
    mjt.parse_class((h17, "me/demo/Util"))
    mjt.rename_method((h17, "me/demo/Util", "add", "(II)I"), "plus")
    mjt.set_deadline_ms(-1)   # 测试位：立即过期
    try:
        mjt.flush(h17, compute_frames=True)
        check("limits flush raises", False, "no exception")
    except mjt.DeadlineExceeded as e:
        check("limits flush raises", isinstance(e, KeyboardInterrupt),
              "DeadlineExceeded must be a KeyboardInterrupt subclass")
    mjt.clear_deadline()
    mjt.flush(h17, compute_frames=True)   # 清除后复原（不得毒化后续阶段）
    mjt.close_container(h17)

    # ── 18. Jasm 文本面（类/方法级读写 + patch/diff + fail-closed） ──
    p18, h18 = fresh("jasm")
    cls = (h18, "me/demo/Util")
    text = mjt.class_jasm(h18, "me/demo/Util")["source"]
    check("jasm class shape", ".class public super me/demo/Util" in text
          and ".method public static add (II)I" in text and "iadd" in text, text[:80])
    check("jasm validate ok", mjt.class_validate_jasm(h18, "me/demo/Util", text)["ok"],
          str(mjt.class_validate_jasm(h18, "me/demo/Util", text)))
    check("jasm apply fixpoint",
          mjt.class_apply_jasm(h18, "me/demo/Util", text)["ok"]
          and mjt.class_jasm(h18, "me/demo/Util")["source"] == text, "roundtrip drift")
    m = mjt.method_jasm(h18, "me/demo/Util", "add", "(II)I")
    check("jasm method shape", m["descriptor"] == "(II)I" and "iadd" in m["source"],
          str(m["source"]))
    diff = "@@ -5,1 +5,1 @@\n-            iadd\n+            isub\n"
    rep = mjt.method_apply_jasm_diff(h18, "me/demo/Util", "add", "(II)I", diff)
    check("jasm method diff apply", rep["ok"], str(rep))
    mjt.write_back(cls, compute_frames=True)
    mref = mjt.get_method(cls, "add", "(II)I")
    ops = [mjt.insn_detail(mref, u)["opcode"] for u, o, k in mjt.insns(mref) if k == "insn"]
    check("jasm method diff opcode", 0x64 in ops and 0x60 not in ops,
          str([hex(o) for o in ops]))
    bad = mjt.class_apply_jasm_diff(h18, "me/demo/Util",
                                    "@@ -4,1 +4,1 @@\n-            iadd\n+            isub\n")
    check("jasm diff context mismatch", not bad["ok"]
          and "context mismatch" in bad["diagnostics"][0]["message"], str(bad))
    other_source = (".super java/lang/Object\n"
                    ".class public me/demo/Other {\n"
                    "}\n")
    check("jasm name mismatch rejected",
          not mjt.class_validate_jasm(h18, "me/demo/Util", other_source)["ok"],
          "name mismatch must fail")
    try:
        mjt.class_jasm(h18, "me/demo/Nope")
        check("jasm unknown class", False, "no exception")
    except KeyError:
        check("jasm unknown class", True)
    mjt.close_container(h18)

    # ── 19. jar_diff（自身零差异 + 单类修改识别 + 缺文件 fail-closed） ──
    p19, h19 = fresh("jardiff")
    same = mjt.jar_diff(h19, p19)
    check("jardiff self clean",
          same["workspace_only"] == 0 and same["reference_only"] == 0
          and same["modified"] == 0 and same["diffs"] == [], str(same))
    # 参考侧单类改动（iadd → isub 同长替换）：diff 收缩为单 range。
    h19b = mjt.open_jar(p19)
    diff19 = "@@ -5,1 +5,1 @@\n-            iadd\n+            isub\n"
    rep19 = mjt.method_apply_jasm_diff(h19b, "me/demo/Util", "add", "(II)I", diff19)
    check("jardiff ref edit ok", rep19["ok"], str(rep19))
    p19b = os.path.join(tmp, "mjt_pysdk_jardiff_ref.jar")
    mjt.save(h19b, p19b)
    mjt.close_container(h19b)
    changed = mjt.jar_diff(h19, p19b)
    check("jardiff modified count", changed["modified"] == 1
          and changed["workspace_only"] == 0 and changed["reference_only"] == 0,
          str(changed))
    check("jardiff util uri", len(changed["diffs"]) == 1
          and changed["diffs"][0]["uri"] == "me/demo/Util.class"
          and len(changed["diffs"][0]["ranges"]) == 1
          and changed["diffs"][0]["ranges"][0]["before_length"] >= 1
          and changed["diffs"][0]["ranges"][0]["after_length"] >= 1, str(changed))
    try:
        mjt.jar_diff(h19, os.path.join(tmp, "mjt_pysdk_missing_ref.jar"))
        check("jardiff missing ref", False, "no exception")
    except RuntimeError:
        check("jardiff missing ref", True)
    mjt.close_container(h19)

    # ── 20. jar_fix（三类问题扫描 + 开关回填 + apply 移除） ──
    # 注：demo jar 的 App 类带守卫/死代码形态，本就过不了 Jasm 校验——断言
    # 只钉注入的两类问题，App 的 invalid_class 属既有底噪。
    p20, h20 = fresh("jarfix")
    mjt.set_entry_bytes(h20, "me/demo/Util.class", b"\xCA\xFE\xBA\xBE\x00\x00\x00\x33")
    mjt.add_entry(h20, "corrupt_output_stream/tmp.bin", b"marker")
    all_off = mjt.jar_fix_analyze(h20, remove_invalid_class_entries=False,
                                  remove_corrupt_output_markers=False)
    kinds = {issue["kind"] for issue in all_off["issues"]}
    check("jarfix kinds", {"corrupt_output_marker", "invalid_class"} <= kinds, str(all_off))
    flagged = {issue["uri"]: issue for issue in all_off["issues"]}
    check("jarfix injected flagged", "me/demo/Util.class" in flagged
          and flagged["me/demo/Util.class"]["kind"] == "invalid_class"
          and "corrupt_output_stream/tmp.bin" in flagged
          and flagged["corrupt_output_stream/tmp.bin"]["kind"] == "corrupt_output_marker",
          str(all_off))
    check("jarfix unselected", all(not issue["selected"] for issue in all_off["issues"]),
          str(all_off))
    check("jarfix class count", all_off["class_count"] == 5
          and all_off["resource_count"] >= 7, str(all_off))
    all_on = mjt.jar_fix_analyze(h20, remove_invalid_class_entries=True,
                                 remove_corrupt_output_markers=True)
    check("jarfix selected", all(issue["selected"] for issue in all_on["issues"]),
          str(all_on))
    applied = mjt.jar_fix_apply(h20, remove_invalid_class_entries=True,
                                remove_corrupt_output_markers=True)
    check("jarfix removed", "me/demo/Util.class" in applied["removed"]
          and "corrupt_output_stream/tmp.bin" in applied["removed"], str(applied))
    clean = mjt.jar_fix_analyze(h20, remove_invalid_class_entries=True,
                                remove_corrupt_output_markers=True)
    check("jarfix clean after apply", clean["issues"] == []
          and clean["removed"] == [], str(clean))
    check("jarfix util gone", "me/demo/Util" not in mjt.class_names(h20),
          str(mjt.class_names(h20)))
    mjt.close_container(h20)

    # ── 21. pass 管线面（describe/run_pipeline 互斥/opt_preview 零改动/opt_apply） ──
    p21, h21 = fresh("pipeline")
    cat = mjt.pass_describe()
    names21 = [p["name"] for p in cat["passes"]]
    check("describe passes", "NopRemover" in names21 and "ClassMemberSorter" in names21
          and all(p["description"] != "" and p["scope"] in ("method", "class", "global")
                  for p in cat["passes"]), str(cat)[:200])
    check("describe pipelines", any(p["name"] == "default-optimize" for p in cat["pipelines"]),
          str(cat["pipelines"]))
    stats21 = mjt.run_pipeline(h21, pass_names=["NopRemover", "ClassMemberSorter"])
    check("runpipe multi slots",
          {s["name"] for s in stats21.slots} == {"NopRemover", "ClassMemberSorter"},
          str(stats21.slots))
    for bad_args in ({"pass_names": ["NopRemover"], "pipeline": "default-optimize"},
                     {}):
        try:
            mjt.run_pipeline(h21, **bad_args)
            check("runpipe xor", False, f"no exception for {bad_args}")
        except ValueError:
            check("runpipe xor", True)
    try:
        mjt.run_pipeline(h21, pipeline="no-such-pipeline")
        check("runpipe unknown pipeline", False, "no exception")
    except ValueError:
        check("runpipe unknown pipeline", True)
    stats_named = mjt.run_pipeline(h21, pipeline="default-optimize")
    check("runpipe named", stats_named.pipeline == "default-optimize", str(stats_named))
    bytes_before = mjt.class_bytes(h21, "me/demo/Util")
    preview = mjt.opt_preview(h21)
    check("preview shape", isinstance(preview["changed"], bool)
          and isinstance(preview["diffs"], list), str(preview)[:200])
    check("preview zero mutation", mjt.class_bytes(h21, "me/demo/Util") == bytes_before,
          "opt_preview must not touch the session container")
    mjt.opt_preview(h21, pipeline="default-optimize")   # 显式管线 + 回落两用面
    stats_apply = mjt.opt_apply(h21)
    check("optapply default", stats_apply.pipeline == "default-optimize", str(stats_apply))
    p21b = os.path.join(tmp, "mjt_pysdk_optapplied.jar")
    mjt.save(h21, p21b)
    mjt.close_container(h21)
    h21b = mjt.open_jar(p21b)
    verify21 = mjt.verify_container(h21b)
    check("optapply verify clean", verify21["malformed"] == 0 and verify21["mismatch"] == 0,
          str({"malformed": verify21["malformed"], "mismatch": verify21["mismatch"]}))
    mjt.close_container(h21b)

    # ── 22. analysis_evaluate（求值/化简/绑定/fail-closed） ──
    r22 = mjt.analysis_evaluate("1 + 2 * 3")
    check("expr eval", r22["evaluated"] and r22["value"] == 7
          and r22["simplified"] is False and isinstance(r22["normalized"], str), str(r22))
    r22b = mjt.analysis_evaluate("1 + 2 * 3", simplify=True)
    check("expr simplify flag", r22b["simplified"] is True and r22b["evaluated"], str(r22b))
    r22c = mjt.analysis_evaluate("x + 1", bindings={"x": 41})
    check("expr bindings", r22c["evaluated"] and r22c["value"] == 42, str(r22c))
    r22d = mjt.analysis_evaluate("y * 2")
    check("expr unbound", not r22d["evaluated"] and "error" in r22d, str(r22d))
    for bad_expr in ({"expression": "1 + * 2"},
                     {"expression": "1 + 1", "max_depth": 0},
                     {"expression": "1 + 1", "max_depth": 8192}):
        try:
            mjt.analysis_evaluate(**bad_expr)
            check("expr fail-closed", False, f"no exception for {bad_expr}")
        except ValueError:
            check("expr fail-closed", True)

    # ── 23. shadow sidecar 文件面（generate 幂等/prune 失配清理/缺侧车报错） ──
    p23, h23 = fresh("shadowfile")
    mjt.close_container(h23)
    sidecar23 = p23 + ".mjt-shadow"
    gen23 = mjt.shadow_generate_file(p23)
    check("shadow gen file", gen23["file"] == sidecar23
          and gen23["assigned_classes"] > 0 and gen23["total_entries"] > 0
          and gen23["removed"] == 0, str(gen23))
    regen23 = mjt.shadow_generate_file(p23)
    check("shadow gen idempotent", regen23["assigned_classes"] == 0
          and regen23["kept_entries"] == gen23["total_entries"]
          and regen23["total_entries"] == gen23["total_entries"], str(regen23))
    prune_idle = mjt.shadow_prune_file(p23)
    check("shadow prune idle", prune_idle["removed"] == 0
          and prune_idle["total_entries"] == gen23["total_entries"], str(prune_idle))
    h23b = mjt.open_jar(p23)
    mjt.remove_entry(h23b, "me/demo/Color.class")
    mjt.save(h23b, p23)
    mjt.close_container(h23b)
    prune23 = mjt.shadow_prune_file(p23)
    check("shadow prune stale", prune23["removed"] > 0
          and prune23["total_entries"] < gen23["total_entries"], str(prune23))
    p23b, h23c = fresh("shadowfile_nosidecar")
    mjt.close_container(h23c)
    try:
        mjt.shadow_prune_file(p23b)
        check("shadow prune missing sidecar", False, "no exception")
    except RuntimeError:
        check("shadow prune missing sidecar", True)
    try:
        mjt.shadow_generate_file(p23b, file=p23b)
        check("shadow sidecar is jar", False, "no exception")
    except RuntimeError:
        check("shadow sidecar is jar", True)

    # ── 24. decompiler_compare（fail-closed + fake java 双后端实跑） ──
    p24, h24 = fresh("compare")
    try:
        mjt.decompiler_compare(h24, "me/demo/App", jar="no-such-decompiler.jar")
        check("compare fail-closed", False, "no exception")
    except RuntimeError as e:
        check("compare fail-closed", "not available" in str(e), str(e))
    fake_java = os.environ.get("MJT_PYSDK_FAKE_JAVA", "")
    # unittest 宿主注入替身 java（本 exe 再入）；独立冒烟无宿主环境，
    # 按 GTEST_SKIP 惯例记 skip 而非 fail，保持 smoke_sdk.py 独立可跑。
    check("compare fake java env", True,
          "skip: MJT_PYSDK_FAKE_JAVA unset outside unittest host"
          if not fake_java else "")
    if fake_java:
        import zipfile
        fake_cfr = os.path.join(tmp, "fake-pysdk-decompiler.jar")
        fake_vf = os.path.join(tmp, "fake-pysdk-vineflower.jar")
        for jar_path in (fake_cfr, fake_vf):
            with zipfile.ZipFile(jar_path, "w") as zf:
                zf.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\n")
        compared = mjt.decompiler_compare(h24, "me/demo/App", jar=fake_cfr,
                                          vineflower_jar=fake_vf, java=fake_java)
        check("compare shape", compared["internal_name"] == "me/demo/App"
              and len(compared["sources"]) == 2, str(compared)[:200])
        backends = [s["backend"] for s in compared["sources"]]
        check("compare backends", backends == ["cfr", "vineflower"], str(backends))
        check("compare sources", all(s["source_scope"] == "class"
                                     and "public class App" in s["source"]
                                     and s["decompiler_jar"] != ""
                                     for s in compared["sources"]), str(compared)[:200])
    mjt.close_container(h24)

    # ── 25. init_order（静态初始化拓扑序：超类边 + clinit 引用边 + 环 + roots 子图） ──
    # demo 罐本无 <clinit>，这里用绑定面合成依赖结构：Crypto 自含 clinit；
    # Util 无引用 clinit 但 super 改指 Crypto（超类边来源）；App 的 clinit 引用
    # Color 与 Crypto；Color 的 clinit 引用 App（App↔Color 成环）。
    # jasm 面读落账字节，树编辑后先 flush 再查。
    p25, h25 = fresh("initorder")
    op25 = mjt.opcodes
    mjt.add_method((h25, "me/demo/Crypto"), "<clinit>", "()V",
                   mjt.acc["STATIC"], 0, 0)
    mjt.emit_insn((h25, "me/demo/Crypto", "<clinit>", "()V"), op25["return"])
    mjt.class_set_super((h25, "me/demo/Util"), "me/demo/Crypto")
    mjt.add_method((h25, "me/demo/Util"), "<clinit>", "()V",
                   mjt.acc["STATIC"], 0, 0)
    mjt.emit_insn((h25, "me/demo/Util", "<clinit>", "()V"), op25["return"])
    mjt.add_method((h25, "me/demo/App"), "<clinit>", "()V",
                   mjt.acc["STATIC"], 2, 0)
    app_cl = (h25, "me/demo/App", "<clinit>", "()V")
    mjt.emit_method_call(app_cl, op25["invokestatic"], "me/demo/Color", "values",
                         "()[Lme/demo/Color;", False)
    mjt.emit_insn(app_cl, op25["pop"])
    mjt.emit_insn(app_cl, op25["iconst_0"])
    mjt.emit_insn(app_cl, op25["iconst_0"])
    mjt.emit_method_call(app_cl, op25["invokestatic"], "me/demo/Crypto", "decode",
                         "(II)Ljava/lang/String;", False)
    mjt.emit_insn(app_cl, op25["pop"])
    mjt.emit_insn(app_cl, op25["return"])
    mjt.add_method((h25, "me/demo/Color"), "<clinit>", "()V",
                   mjt.acc["STATIC"], 1, 0)
    color_cl = (h25, "me/demo/Color", "<clinit>", "()V")
    mjt.emit_insn(color_cl, op25["aconst_null"])
    mjt.emit_method_call(color_cl, op25["invokestatic"], "me/demo/App", "main",
                         "([Ljava/lang/String;)V", False)
    mjt.emit_insn(color_cl, op25["return"])
    mjt.flush(h25, compute_frames=True)
    full = init_order(h25)
    check("init full order", full["order"] == ["me/demo/Crypto", "me/demo/Util"],
          str(full["order"]))
    check("init full cycles", full["cycles"] == [["me/demo/App", "me/demo/Color"]],
          str(full["cycles"]))
    check("init no_clinit", full["no_clinit"] == ["me/demo/Api"], str(full["no_clinit"]))
    leaf = init_order(h25, ["me/demo/Crypto"])
    check("init roots leaf", leaf["order"] == ["me/demo/Crypto"] and leaf["cycles"] == [],
          str(leaf))
    sub = init_order(h25, ["me/demo/Util"])
    check("init roots super closure",
          sub["order"] == ["me/demo/Crypto", "me/demo/Util"] and sub["cycles"] == [],
          str(sub))
    cyc = init_order(h25, ["me/demo/App"])
    check("init roots cycle", cyc["order"] == ["me/demo/Crypto"]
          and cyc["cycles"] == [["me/demo/App", "me/demo/Color"]], str(cyc))
    mjt.close_container(h25)

    # ── 26. fail_soft 容错档位（坏类不拖垮整作业，跳过清单进 stats） ──
    # 结构性坏类（垃圾字节）在默认档位经 sanitize 门 fail-fast 整作业报错；
    # fail_soft 档位记 sanitize_error 跳过并继续，其余类照常产出（门契约：
    # sanitize 失败类从容器剔除，导出不再见）。opt_preview 跑在克隆上，
    # 跳过清单随结果带出且不动会话容器。
    p26, h26 = fresh("failsoft")
    mjt.close_container(h26)
    import zipfile
    p26bad = os.path.join(tmp, "mjt_pysdk_failsoft_bad.jar")
    with zipfile.ZipFile(p26) as zin, \
         zipfile.ZipFile(p26bad, "w", zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            zout.writestr(item.filename, zin.read(item.filename))
        zout.writestr("me/demo/Broken.class", b"\xCA\xFE\xBA\xBE\x00\x01")
    h26b = mjt.open_jar(p26bad)
    try:
        mjt.run_pipeline(h26b, pass_names=["NopRemover"])
        check("failsoft default throws", False, "no exception")
    except RuntimeError as e:
        check("failsoft default throws",
              "ClassFileSanitizeGate: failed at 'me/demo/Broken.class'" in str(e), str(e))
    stats26 = mjt.run_pipeline(h26b, pass_names=["NopRemover", "ClassMemberSorter"],
                               fail_soft=True)
    check("failsoft skip reported",
          [(s["scope"], s["target"], s["reason"]) for s in stats26.skipped]
          == [("class", "me/demo/Broken.class", "sanitize_error")], str(stats26.skipped))
    check("failsoft good classes ran",
          all(s["processed"] > 0 for s in stats26.slots), str(stats26.slots))
    p26out = os.path.join(tmp, "mjt_pysdk_failsoft_out.jar")
    mjt.save(h26b, p26out)
    with zipfile.ZipFile(p26out) as zout:
        check("failsoft broken erased", "me/demo/Broken.class" not in zout.namelist(),
              str([n for n in zout.namelist() if "Broken" in n]))
    mjt.close_container(h26b)
    h26c = mjt.open_jar(p26bad)
    preview26 = mjt.opt_preview(h26c, pass_names=["NopRemover"], fail_soft=True)
    check("failsoft preview skip",
          [(s["scope"], s["target"], s["reason"]) for s in preview26["skipped"]]
          == [("class", "me/demo/Broken.class", "sanitize_error")], str(preview26["skipped"]))
    mjt.close_container(h26c)

    if fallback_tmp is not None:
        import shutil
        shutil.rmtree(fallback_tmp, ignore_errors=True)
    return fails


if __name__ == "__main__":
    # --script 宿主 exec 本文件时 globals 的 __name__ 是 "__main__"，本块生效；
    # unittest 宿主走 import（__name__ = 模块名），本块不进。退出码对齐宿主契约
    # （pysdk_embed）：0=全绿、1=有 fail——sys.exit 抛 SystemExit，走
    # PyErr_Print 的 handle_system_exit 通道按码整进程退出，不落 traceback；
    # 2（用法错）与 3（watchdog 硬终止）是宿主侧码，脚本面不产。
    import sys

    _fails = run_all()
    for _f in _fails:
        print("FAIL " + _f)
    sys.exit(1 if _fails else 0)
