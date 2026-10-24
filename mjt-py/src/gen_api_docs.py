# gen_api_docs.py — mjt-py API 文档生成器（自省 mjt + mjt.util，产出 Markdown）。
#
# 用法（宿主 --script）：
#   mjt-mcp.exe --script gen_api_docs.py [out.md]
# 缺省输出到脚本旁 pysdk-api.md。函数改动后重跑本脚本即可刷新，勿手编。
#
# 自省路径：C++ 绑定函数无 __signature__（inspect.signature 抛 TypeError），
# 签名取 __doc__ 首行（pybind11 自动生成）；util 层是 exec 片段成的模块，
# Python 函数对象用 inspect 正常自省。


def _is_native(obj):
    """pybind11 函数是 builtin_function_or_method 实例。"""
    return type(obj).__name__ == "builtin_function_or_method"


def _sig_of_native(obj):
    """native 函数签名 = __doc__ 首行（pybind11 生成）；无则占位。"""
    doc = getattr(obj, "__doc__", None) or ""
    lines = doc.splitlines()
    first = lines[0].strip() if lines else ""
    return first if first else "(signature unavailable)"


def _sig_of_py(obj):
    """Python 函数/类签名（inspect；失败回退名字）。"""
    import inspect

    try:
        return obj.__name__ + str(inspect.signature(obj))
    except (TypeError, ValueError):
        return obj.__name__


def _doc_body(obj, is_native):
    """docstring 正文：native 的 __doc__ 首行是签名，正文取首行之后。"""
    doc = (getattr(obj, "__doc__", None) or "").strip("\n")
    if not doc:
        return ""
    if is_native:
        parts = doc.split("\n", 1)
        return parts[1].strip("\n") if len(parts) > 1 else ""
    return doc.rstrip()


def _render_function(out, prefix, name, obj, is_native):
    """单函数符号：签名 + docstring 全文（围栏块保真）。"""
    sig = _sig_of_native(obj) if is_native else _sig_of_py(obj)
    out.append(f"### {prefix}{name}\n")
    out.append("```")
    out.append(sig)
    body = _doc_body(obj, is_native)
    if body:
        out.append("")
        out.append(body)
    out.append("```")
    if not is_native and getattr(obj, "__name__", name) != name:
        out.append("")
        out.append(f"`{name}` 是 `{obj.__name__}` 的别名（同一函数对象）。")
    out.append("")


def _render_class(out, prefix, name, cls, is_native):
    """类符号：类 doc + dataclass 字段 + 公开属性 + 公开成员方法。"""
    out.append(f"### {prefix}{name}\n")
    out.append("```")
    out.append(f"class {name}")
    doc = (getattr(cls, "__doc__", None) or "").strip("\n")
    if doc:
        out.append("")
        out.append(doc)
    out.append("```")
    if not is_native:
        import dataclasses

        if dataclasses.is_dataclass(cls):
            out.append("字段：\n")
            for f in dataclasses.fields(cls):
                if f.name.startswith("_"):
                    continue    # 内部字段（如 dry_run 分叉的父容器键）不入参考
                ftype = f.type if isinstance(f.type, str) \
                    else getattr(f.type, "__name__", str(f.type))
                out.append(f"- `{f.name}`：{ftype}")
            out.append("")
    properties = []
    methods = []
    for attr in sorted(dir(cls)):
        if attr.startswith("_"):
            continue
        try:
            val = getattr(cls, attr)
        except Exception:                                    # noqa: BLE001
            continue
        if isinstance(val, property):
            doc1 = (getattr(val, "__doc__", None) or "").strip().splitlines()
            properties.append((attr, doc1[0] if doc1 else ""))
        elif callable(val) or isinstance(val, type):
            methods.append((attr, val))
    if properties:
        out.append("属性：\n")
        for attr, doc1 in properties:
            out.append(f"- `{attr}`：{doc1}" if doc1 else f"- `{attr}`")
        out.append("")
    for attr, val in methods:
        out.append(f"#### {prefix}{name}.{attr}\n")
        out.append("```")
        out.append(_sig_of_native(val) if is_native else _sig_of_py(val))
        body = _doc_body(val, is_native)
        if body:
            out.append("")
            out.append(body)
        out.append("```")
        out.append("")


def _render_module(out, title, prefix, module):
    """枚举模块公开符号（跳过下划线私有）：常量/函数/类分组输出。"""
    import types

    constants, functions, classes = [], [], []
    for name in dir(module):
        if name.startswith("_"):
            continue
        try:
            obj = getattr(module, name)
        except Exception:                                    # noqa: BLE001
            continue
        if isinstance(obj, types.ModuleType):
            continue    # 子模块（如 mjt.util）单独成节，不进常量清单
        if (callable(obj) and not isinstance(obj, type)
                and not _is_native(obj)
                and str(getattr(obj, "__module__", "")) != module.__name__):
            continue    # 借入的 Python 对象（如 dataclass 装饰器）不算本模块 API
        if isinstance(obj, type):
            classes.append((name, obj))
        elif callable(obj):
            functions.append((name, obj))
        else:
            constants.append((name, obj))
    out.append(f"## {title}\n")
    if constants:
        out.append("### 常量\n")
        for name, obj in constants:
            size = f"，{len(obj)} 项" if hasattr(obj, "__len__") else ""
            out.append(f"- `{prefix}{name}`：{type(obj).__name__}{size}")
        out.append("")
    for name, obj in functions:
        _render_function(out, prefix, name, obj, _is_native(obj))
    for name, obj in classes:
        _render_class(out, prefix, name, obj, not _is_py_class(obj))
    out.append("")


def _is_py_class(obj):
    """Python 类（util 层 dataclass 等）vs pybind11 类型。"""
    return str(getattr(obj, "__module__", "")).startswith("mjt.util")


def main():
    import datetime
    import os
    import sys

    import mjt

    out_path = sys.argv[1] if len(sys.argv) > 1 else \
        os.path.join(os.path.dirname(os.path.abspath(sys.argv[0])), "pysdk-api.md")

    out = []
    out.append("# mjt-py SDK API 参考（pysdk-api.md）\n")
    out.append(f"由 `gen_api_docs.py` 自省生成于 "
               f"{datetime.datetime.now().isoformat(timespec='seconds')}"
               "；函数改动后重跑本脚本即可刷新，勿手编。\n")
    out.append("生成方式：`mjt-mcp.exe --script mjt-py/src/gen_api_docs.py"
               " mjt-py/doc_ai/pysdk-api.md`\n")
    out.append("## 约定\n")
    out.append("- 键面：`h` = 容器句柄 int；`cref` = (h, 类名)；`mref`/`fref` ="
               " (h, cls, name, desc)；类名一律 internal name（`a/b/C` 形态）。")
    out.append("- 异常：键失效/不存在抛 `mjt.StaleRefError`（KeyError 子类）；"
               "协作层 deadline 越限抛 `mjt.DeadlineExceeded`（KeyboardInterrupt 子类）。")
    out.append("- 绑定函数没有 `__signature__`，inspect.signature 不可用；签名在"
               " `__doc__` 首行（pybind11 生成），本文档已逐符号抄录。")
    out.append("- util 层取用：`from mjt import util as U`（exec 片段模块，不能"
               " `import mjt_util`）。\n")

    _render_module(out, "mjt（C++ 绑定）", "mjt.", mjt)
    _render_module(out, "mjt.util（Python 层）", "mjt.util.", mjt.util)

    text = "\n".join(out) + "\n"
    with open(out_path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(text)
    n_bind = sum(1 for n in dir(mjt) if not n.startswith("_") and callable(getattr(mjt, n))
                 and not isinstance(getattr(mjt, n), type))
    n_util = sum(1 for n in dir(mjt.util) if not n.startswith("_") and callable(getattr(mjt.util, n))
                 and not isinstance(getattr(mjt.util, n), type))
    n_cls = sum(1 for n in dir(mjt) if not n.startswith("_") and isinstance(getattr(mjt, n), type)) \
        + sum(1 for n in dir(mjt.util)
              if not n.startswith("_") and isinstance(getattr(mjt.util, n), type))
    print(f"pysdk-api.md written: {out_path}")
    print(f"lines={text.count(chr(10))} bind_functions={n_bind} util_callables={n_util} "
          f"classes={n_cls}")


main()
