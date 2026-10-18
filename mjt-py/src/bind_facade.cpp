#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "pysdk_facade.hpp"

#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;

using jbytecode::tree::AbstractInsnNode;

// 声明契约在 pysdk_bridge.hpp；本 TU 为定义侧（v3 平函数面）。
namespace mjt::pysdk {
void bindFacade(py::module_& m);
}

namespace {

}   // namespace

namespace mjt::pysdk {
namespace {

py::bytes toPyBytes(const std::vector<ca::u8>& bytes)
{
    return py::bytes(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<py::ssize_t>(bytes.size()));
}

std::vector<ca::u8> fromPyBytes(py::bytes data)
{
    std::string raw = data;
    return std::vector<ca::u8>(raw.begin(), raw.end());
}

/// py 值 → ldc 载荷（str/int/float；int 溢出 int32 升 i64）。仅绑定 TU 使用。
/// 字符串载荷经 facadeInternedStr 收进会话 arena——Any 存视图，绝不能指向
/// 本函数的临时 std::string（悬垂写出现场：ldc 字节随堆残留物漂移）。
ca::core::Any anyFromPy(FacadeSession& s, py::handle value)
{
    if (py::isinstance<py::str>(value)) {
        return facadeInternedStr(s, py::str(value).cast<std::string>());
    }
    if (py::isinstance<py::int_>(value)) {
        const long long v = PyLong_AsLongLong(value.ptr());
        if (v > std::numeric_limits<int>::max() || v < std::numeric_limits<int>::min())
            return ca::core::Any(static_cast<ca::i64>(v));
        return ca::core::Any(static_cast<int>(v));
    }
    if (py::isinstance<py::float_>(value)) {
        return ca::core::Any(static_cast<double>(py::reinterpret_borrow<py::float_>(value)));
    }
    throw py::type_error("ldc value must be str/int/float");
}


}   // namespace

// ═══════════════════════════════════════════════════════════════════════
// v3 平函数面（契约 §2）：键/bytes 进，值/bytes 出；逐调用按键重解析。
// B1 落容器/条目面；B2 同文件增树面（§2.2-2.6）。
// ═══════════════════════════════════════════════════════════════════════

void bindFacade(py::module_& m)
{
    // 失效语义异常：StaleRefError 为 KeyError 子类（脚本可 except KeyError 兜底）。
    // register_exception（非 exception 构造）才会同时装 translator——
    // 实测补刀盘：只 py::exception<T>(m,...) 建类型不装 translator，
    // 旧键 fail-closed 全部 SystemError 逃逸。
    static auto& staleExc = py::register_exception<StaleRefError>(m, "StaleRefError",
                                                                  PyExc_KeyError);
    (void)staleExc;


    // ── 会话生命周期 ──
    m.def(
        "open_jar",
        [](const std::string& path) {
            auto& session = FacadeRegistry::instance().create();
            try {
                facadeImportJar(session, path);
            }
            catch (...) {
                FacadeRegistry::instance().close(session.handle);
                throw;
            }
            return session.handle;
        },
        py::arg("path"), "打开 jar → 容器句柄（Ref=键；句柄须显式传参）");

    m.def("new_container", []() { return FacadeRegistry::instance().create().handle; },
          "新建空容器 → 句柄");

    m.def(
        "close_container", [](ca::u64 h) { FacadeRegistry::instance().close(h); }, py::arg("h"),
        "显式释放容器；此后该容器全部键 fail-closed（不调则进程退出兜底）");

    // ── 条目面 ──
    m.def(
        "list_entries",
        [](ca::u64 h) {
            py::list out;
            for (const auto& [uri, type] : facadeListEntries(FacadeRegistry::instance().get(h)))
                out.append(py::make_tuple(uri, type));
            return out;
        },
        py::arg("h"), "全部条目 (uri, 类型)——含 MANIFEST 等非类资源");

    m.def(
        "get_entry_bytes",
        [](ca::u64 h, const std::string& uri) {
            return toPyBytes(facadeGetEntryBytes(FacadeRegistry::instance().get(h), uri));
        },
        py::arg("h"), py::arg("uri"), "条目原始字节");

    m.def(
        "set_entry_bytes",
        [](ca::u64 h, const std::string& uri, py::bytes data) {
            facadeSetEntryBytes(FacadeRegistry::instance().get(h), uri, fromPyBytes(data));
        },
        py::arg("h"), py::arg("uri"), py::arg("bytes"), "条目字节替换（类条目作废缓存树）");

    m.def(
        "add_entry",
        [](ca::u64 h, const std::string& uri, py::bytes data) {
            facadeAddEntry(FacadeRegistry::instance().get(h), uri, fromPyBytes(data));
        },
        py::arg("h"), py::arg("uri"), py::arg("bytes"),
        "新增条目；类字节自动进索引（同名 first-wins 记落选者）");

    m.def(
        "remove_entry",
        [](ca::u64 h, const std::string& uri) { facadeRemoveEntry(FacadeRegistry::instance().get(h), uri); },
        py::arg("h"), py::arg("uri"), "删除条目（winner 移除时落选者晋升）");

    m.def(
        "load_class_file",
        [](ca::u64 h, const std::string& path, std::optional<std::string> uri, bool replace) {
            facadeLoadClassFile(FacadeRegistry::instance().get(h), path, std::move(uri), replace);
        },
        py::arg("h"), py::arg("path"), py::arg("uri") = std::nullopt,
        py::arg("replace") = false, "磁盘 loose .class 入容器（URI 默认 = 文件名）");

    // ── 类名/类字节（索引 O(1)） ──
    m.def("class_names",
          [](ca::u64 h) { return facadeClassNames(FacadeRegistry::instance().get(h)); }, py::arg("h"),
          "已解析类名（internal name，升序）");
    m.def("class_uri",
          [](ca::u64 h, const std::string& name) { return facadeClassUri(FacadeRegistry::instance().get(h), name); },
          py::arg("h"), py::arg("name"), "类名 → 条目 URI");
    m.def("duplicate_names",
          [](ca::u64 h) { return facadeDuplicateNames(FacadeRegistry::instance().get(h)); }, py::arg("h"),
          "first-wins 落选类名清单（#962）");
    m.def(
        "class_bytes",
        [](ca::u64 h, const std::string& name) {
            return toPyBytes(facadeClassBytes(FacadeRegistry::instance().get(h), name));
        },
        py::arg("h"), py::arg("name"), "类字节（不建树）");
    m.def(
        "set_class_bytes",
        [](ca::u64 h, const std::string& name, py::bytes data) {
            facadeSetClassBytes(FacadeRegistry::instance().get(h), name, fromPyBytes(data));
        },
        py::arg("h"), py::arg("name"), py::arg("bytes"),
        "类字节写回/新增（不存在则新增；覆盖作废缓存树）");

    // ── 导出 ──
    m.def(
        "export_jar",
        [](ca::u64 h, const std::string& path, std::vector<std::string> uris) {
            facadeExportJar(FacadeRegistry::instance().get(h), path, uris);
        },
        py::arg("h"), py::arg("path"), py::arg("uris") = std::vector<std::string>{},
        "导出 jar（uris 空=全量；先自动 flush 脏树 + 落账挂账改名再落盘，两阶段）");
    m.def(
        "export_class",
        [](ca::u64 h, const std::string& name, const std::string& path) {
            facadeExportClass(FacadeRegistry::instance().get(h), name, path);
        },
        py::arg("h"), py::arg("name"), py::arg("path"),
        "单类导出为 .class 文件（脏树先写回 + 挂账改名落账）");

    // ── 完整性 / 条目落账 ──
    m.def(
        "sync_entries",
        [](ca::u64 h) { return facadeSyncEntries(FacadeRegistry::instance().get(h)); },
        py::arg("h"), "落账挂账的类改名条目搬移 → [\"old -> new\"]（write_back/export "
                      "边界自动调用，也可显式调）");
    m.def(
        "verify_container",
        [](ca::u64 h) {
            const auto rep = facadeVerifyContainer(FacadeRegistry::instance().get(h));
            py::dict out;
            out["match"]      = rep.match;
            out["mismatch"]   = rep.mismatch;
            out["malformed"]  = rep.malformed;
            out["dep_missing"] = rep.depMissing;
            out["non_class"]  = rep.nonClass;
            auto toList = [](const std::vector<VerifyIssue>& v) {
                py::list out;
                for (const auto& i : v) out.append(py::make_tuple(i.entry, i.detail));
                return out;
            };
            out["mismatch_details"]  = toList(rep.mismatchDetails);
            out["malformed_details"] = toList(rep.malformedDetails);
            out["dep_missing_details"] = toList(rep.depMissingDetails);
            return out;
        },
        py::arg("h"),
        "容器完整性体检：match/mismatch/malformed/dep_missing/non_class 计数 + "
        "明细 (entry, detail) 列表（entry 探针 + 解析闸 + 非 JDK 外部依赖清点）");
    m.def(
        "sanitize_lvt",
        [](ca::u64 h) {
            const auto rep = facadeSanitizeLvt(FacadeRegistry::instance().get(h));
            return py::make_tuple(rep.dropped, rep.deduped, rep.classes);
        },
        py::arg("h"), "LVT 清洗 → (dropped, deduped, classes)；脱链/乱序条目剔除 + 重复去重");

    // ═══════════════════════════════════════════════════════════════════
    // 树函数面（B2，契约 §2.2-2.6）。cref=(h, name)、mref/fref=(h, cls, name, desc)。
    // ═══════════════════════════════════════════════════════════════════

    using ClassRef  = std::tuple<ca::u64, std::string>;
    using MemberRef = std::tuple<ca::u64, std::string, std::string, std::string>;

    auto sess = [](ca::u64 h) -> FacadeSession& { return FacadeRegistry::instance().get(h); };

    auto clsOf = [](const ClassRef& r) -> std::pair<ca::u64, std::string> {
        return {std::get<0>(r), std::get<1>(r)};
    };
    auto memOf = [](const MemberRef& r) {
        return std::make_tuple(std::get<0>(r), std::get<1>(r), std::get<2>(r), std::get<3>(r));
    };
    auto classRef = [](ca::u64 h, const std::string& name) { return ClassRef{h, name}; };
    auto methodRef = [](ca::u64 h, const std::string& cls, const std::string& name,
                        const std::string& desc) { return MemberRef{h, cls, name, desc}; };

    auto pyDetail = [](const InsnDetail& d) {
        py::dict out;
        out["uid"]     = d.uid;
        out["opcode"]  = d.opcode;
        out["kind"]    = facadeInsnKindName(d.kind);
        out["mnemonic"] = (d.opcode >= 0 && d.opcode <= 201) ? facadeOpcodeName(d.opcode) : "?";
        if (d.kind == AbstractInsnNode::INT_INSN) out["operand"] = d.operand;
        if (d.kind == AbstractInsnNode::VAR_INSN) out["var"] = d.var;
        if (d.kind == AbstractInsnNode::TYPE_INSN) out["desc"] = d.desc;
        if (d.kind == AbstractInsnNode::FIELD_INSN) {
            out["owner"] = d.owner;
            out["name"]  = d.name;
            out["desc"]  = d.desc;
        }
        if (d.kind == AbstractInsnNode::METHOD_INSN) {
            out["owner"] = d.owner;
            out["name"]  = d.name;
            out["desc"]  = d.desc;
            out["itf"]   = d.itf;
        }
        if (d.kind == AbstractInsnNode::INVOKE_DYNAMIC_INSN) {
            out["name"] = d.name;
            out["desc"] = d.desc;
        }
        if (d.kind == AbstractInsnNode::JUMP_INSN) out["target"] = d.targetUid;
        if (d.kind == AbstractInsnNode::LDC_INSN) {
            out["cst_tag"] = d.cstTag;
            if (d.cstTag == 1) out["cst"] = d.cstStr;
            else if (d.cstTag == 2 || d.cstTag == 3) out["cst"] = static_cast<long long>(d.cstInt);
            else if (d.cstTag == 4 || d.cstTag == 5) out["cst"] = d.cstFloat;
            else out["cst"] = py::none();
        }
        if (d.kind == AbstractInsnNode::IINC_INSN) {
            out["var"]  = d.var;
            out["incr"] = d.incr;
        }
        if (d.kind == AbstractInsnNode::TABLESWITCH_INSN ||
            d.kind == AbstractInsnNode::LOOKUPSWITCH_INSN) {
            out["min"] = d.switchMin;
            out["max"] = d.switchMax;
            out["default"] = d.defaultUid;
            py::list t;
            for (const auto u : d.targetUids) t.append(u);
            out["targets"] = t;
            py::list k;
            for (const auto key : d.switchKeys) k.append(key);
            out["keys"] = k;
        }
        if (d.kind == AbstractInsnNode::MULTIANEWARRAY_INSN) {
            out["desc"] = d.desc;
            out["dims"] = d.dims;
        }
        if (d.kind == AbstractInsnNode::LINE) out["line"] = d.line;
        return out;
    };

    // ── 读 ──
    m.def(
        "parse_class",
        [sess, clsOf, classRef](const ClassRef& ref) {
            auto [h, name] = clsOf(ref);
            facadeParseClass(sess(h), name);
            return classRef(h, name);
        },
        py::arg("cref"), "附着解析（幂等，进缓存）→ cref");

    m.def(
        "class_info",
        [sess, clsOf](const ClassRef& ref) {
            auto [h, name] = clsOf(ref);
            auto ci        = facadeClassInfo(sess(h), name);
            py::dict out;
            out["name"]       = ci.name;
            out["super"]      = ci.superName;
            out["access"]     = ci.access;
            out["version"]    = ci.version;
            out["interfaces"] = ci.interfaces;
            return out;
        },
        py::arg("cref"), "(name, super, access, version, interfaces)");

    m.def(
        "methods",
        [sess, clsOf](const ClassRef& ref) {
            auto [h, name] = clsOf(ref);
            py::list out;
            for (const auto& [n, d, a] : facadeMembers(sess(h), name, true))
                out.append(py::make_tuple(n, d, a));
            return out;
        },
        py::arg("cref"), "[(name, desc, access)]");
    m.def(
        "fields",
        [sess, clsOf](const ClassRef& ref) {
            auto [h, name] = clsOf(ref);
            py::list out;
            for (const auto& [n, d, a] : facadeMembers(sess(h), name, false))
                out.append(py::make_tuple(n, d, a));
            return out;
        },
        py::arg("cref"), "[(name, desc, access)]");
    m.def(
        "class_hierarchy",
        [sess, clsOf](const ClassRef& ref) {
            auto [h, name] = clsOf(ref);
            const auto rep = facadeClassHierarchy(sess(h), name);
            py::dict out;
            py::list classes;
            for (const auto& c : rep.classes) {
                py::dict d;
                d["name"]       = c.name;
                d["super"]      = c.superName;
                d["interfaces"] = c.interfaces;
                classes.append(d);
            }
            out["classes"] = classes;
            py::list methods;
            for (const auto& [owner, n, d, a] : rep.methods)
                methods.append(py::make_tuple(owner, n, d, a));
            out["methods"] = methods;
            py::list fields;
            for (const auto& [owner, n, d, a] : rep.fields)
                fields.append(py::make_tuple(owner, n, d, a));
            out["fields"] = fields;
            return out;
        },
        py::arg("cref"),
        "继承闭包查询：{classes: [{name, super, interfaces}]（self 先序 BFS，容器外"
        "父类/接口如 java/lang/Object 不入闭包），methods/fields: [(owner, name, "
        "desc, access)] 沿途声明成员集}；源码树建继承成员集不必再外跑 javap/反射");

    m.def(
        "get_method",
        [sess, clsOf, methodRef](const ClassRef& ref, const std::string& name,
                                 const std::string& desc) {
            auto [h, cls] = clsOf(ref);
            facadeMethodInfo(sess(h), cls, name, desc);   // 存在性校验
            return methodRef(h, cls, name, desc);
        },
        py::arg("cref"), py::arg("name"), py::arg("desc"), "mref（不存在抛错）");
    m.def(
        "get_field",
        [sess, clsOf, methodRef](const ClassRef& ref, const std::string& name,
                                 const std::string& desc) {
            auto [h, cls] = clsOf(ref);
            facadeFieldInfo(sess(h), cls, name, desc);
            return methodRef(h, cls, name, desc);
        },
        py::arg("cref"), py::arg("name"), py::arg("desc"), "fref（不存在抛错）");

    m.def(
        "method_info",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            auto mi                   = facadeMethodInfo(sess(h), cls, name, desc);
            py::dict out;
            out["name"]       = mi.name;
            out["desc"]       = mi.desc;
            out["access"]     = mi.access;
            out["max_stack"]  = mi.maxStack;
            out["max_locals"] = mi.maxLocals;
            out["exceptions"] = mi.exceptions;
            return out;
        },
        py::arg("mref"));
    m.def(
        "field_info",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            auto fi                   = facadeFieldInfo(sess(h), cls, name, desc);
            py::dict out;
            out["name"]   = fi.name;
            out["desc"]   = fi.desc;
            out["access"] = fi.access;
            return out;
        },
        py::arg("fref"));

    // ── 写（类） ──
    m.def(
        "class_set_super",
        [sess, clsOf](const ClassRef& ref, const std::string& super) {
            auto [h, name] = clsOf(ref);
            facadeClassSetSuper(sess(h), name, super);
        },
        py::arg("cref"), py::arg("super"));
    m.def(
        "class_set_access",
        [sess, clsOf](const ClassRef& ref, int access) {
            auto [h, name] = clsOf(ref);
            facadeClassSetAccess(sess(h), name, access);
        },
        py::arg("cref"), py::arg("access"));
    m.def(
        "class_set_version",
        [sess, clsOf](const ClassRef& ref, int version) {
            auto [h, name] = clsOf(ref);
            facadeClassSetVersion(sess(h), name, version);
        },
        py::arg("cref"), py::arg("version"));
    m.def(
        "class_set_interfaces",
        [sess, clsOf](const ClassRef& ref, const std::vector<std::string>& ifaces) {
            auto [h, name] = clsOf(ref);
            facadeClassSetInterfaces(sess(h), name, ifaces);
        },
        py::arg("cref"), py::arg("interfaces"));
    m.def(
        "rename_class",
        [sess, clsOf, classRef](const ClassRef& ref, const std::string& new_name) {
            auto [h, name] = clsOf(ref);
            return classRef(h, facadeRenameClass(sess(h), name, new_name));
        },
        py::arg("cref"), py::arg("new_name"), "类改名 + 索引迁移 → 新 cref（旧 cref 失效）");

    // ── 写（方法/字段） ──
    m.def(
        "rename_method",
        [sess, memOf, methodRef](const MemberRef& ref, const std::string& new_name) {
            auto [h, cls, name, desc] = memOf(ref);
            return methodRef(h, cls, facadeRenameMethod(sess(h), cls, name, desc, new_name), desc);
        },
        py::arg("mref"), py::arg("new_name"), "→ 新 mref（旧 mref 失效）");
    m.def(
        "rename_field",
        [sess, memOf, methodRef](const MemberRef& ref, const std::string& new_name) {
            auto [h, cls, name, desc] = memOf(ref);
            return methodRef(h, cls, facadeRenameField(sess(h), cls, name, desc, new_name), desc);
        },
        py::arg("fref"), py::arg("new_name"), "→ 新 fref（旧 fref 失效）");
    m.def(
        "method_set_desc",
        [sess, memOf, methodRef](const MemberRef& ref, const std::string& new_desc) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeMethodSetDesc(sess(h), cls, name, desc, new_desc);
            return methodRef(h, cls, name, new_desc);
        },
        py::arg("mref"), py::arg("new_desc"), "→ 新 mref");
    m.def(
        "method_set_access",
        [sess, memOf](const MemberRef& ref, int access) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeMethodSetAccess(sess(h), cls, name, desc, access);
        },
        py::arg("mref"), py::arg("access"));
    m.def(
        "method_set_max_stack",
        [sess, memOf](const MemberRef& ref, int v) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeMethodSetMaxStack(sess(h), cls, name, desc, v);
        },
        py::arg("mref"), py::arg("v"));
    m.def(
        "method_set_max_locals",
        [sess, memOf](const MemberRef& ref, int v) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeMethodSetMaxLocals(sess(h), cls, name, desc, v);
        },
        py::arg("mref"), py::arg("v"));
    m.def(
        "method_set_exceptions",
        [sess, memOf](const MemberRef& ref, const std::vector<std::string>& exceptions) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeMethodSetExceptions(sess(h), cls, name, desc, exceptions);
        },
        py::arg("mref"), py::arg("exceptions"));
    m.def(
        "field_set_access",
        [sess, memOf](const MemberRef& ref, int access) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeFieldSetAccess(sess(h), cls, name, desc, access);
        },
        py::arg("fref"), py::arg("access"));
    m.def(
        "add_method",
        [sess, clsOf, methodRef](const ClassRef& ref, const std::string& name,
                                 const std::string& desc, int access, int max_stack,
                                 int max_locals) {
            auto [h, cls] = clsOf(ref);
            facadeAddMethod(sess(h), cls, name, desc, access, max_stack, max_locals);
            return methodRef(h, cls, name, desc);
        },
        py::arg("cref"), py::arg("name"), py::arg("desc"), py::arg("access"),
        py::arg("max_stack"), py::arg("max_locals"), "→ mref");
    m.def(
        "add_field",
        [sess, clsOf, methodRef](const ClassRef& ref, const std::string& name,
                                 const std::string& desc, int access) {
            auto [h, cls] = clsOf(ref);
            facadeAddField(sess(h), cls, name, desc, access);
            return methodRef(h, cls, name, desc);
        },
        py::arg("cref"), py::arg("name"), py::arg("desc"), py::arg("access"), "→ fref");
    m.def(
        "remove_method",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeRemoveMethod(sess(h), cls, name, desc);
        },
        py::arg("mref"), "摘链并销毁；mref 随即失效");
    m.def(
        "remove_field",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeRemoveField(sess(h), cls, name, desc);
        },
        py::arg("fref"), "摘链并销毁；fref 随即失效");

    // ── 指令面 ──
    m.def(
        "insn_count",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeInsnCount(sess(h), cls, name, desc);
        },
        py::arg("mref"));
    m.def(
        "insns",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            py::list out;
            for (const auto& b : facadeInsns(sess(h), cls, name, desc))
                out.append(py::make_tuple(b.uid, b.opcode, facadeInsnKindName(b.kind)));
            return out;
        },
        py::arg("mref"), "[(uid, opcode, kind)]");
    m.def(
        "insn_detail",
        [sess, memOf, pyDetail](const MemberRef& ref, ca::u32 uid) {
            auto [h, cls, name, desc] = memOf(ref);
            return pyDetail(facadeInsnDetail(sess(h), cls, name, desc, uid));
        },
        py::arg("mref"), py::arg("uid"), "按 kind 展开全字段 dict");
    m.def(
        "insn_index",
        [sess, memOf](const MemberRef& ref, ca::u32 uid) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeInsnIndex(sess(h), cls, name, desc, uid);
        },
        py::arg("mref"), py::arg("uid"), "诊断用位次（O(n)，非键）");
    m.def(
        "first_insn",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeFirstInsn(sess(h), cls, name, desc);
        },
        py::arg("mref"), "uid | None");
    m.def(
        "last_insn",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeLastInsn(sess(h), cls, name, desc);
        },
        py::arg("mref"), "uid | None");
    m.def(
        "next_insn",
        [sess, memOf](const MemberRef& ref, ca::u32 uid) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeNextInsn(sess(h), cls, name, desc, uid);
        },
        py::arg("mref"), py::arg("uid"), "uid | None");
    m.def(
        "prev_insn",
        [sess, memOf](const MemberRef& ref, ca::u32 uid) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadePrevInsn(sess(h), cls, name, desc, uid);
        },
        py::arg("mref"), py::arg("uid"), "uid | None");

    m.def(
        "insn_set_opcode",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, int op) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetOpcode(sess(h), cls, name, desc, uid, op);
        },
        py::arg("mref"), py::arg("uid"), py::arg("op"));
    m.def(
        "insn_set_var",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, int var) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetVar(sess(h), cls, name, desc, uid, var);
        },
        py::arg("mref"), py::arg("uid"), py::arg("var"));
    m.def(
        "insn_set_incr",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, int incr) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetIncr(sess(h), cls, name, desc, uid, incr);
        },
        py::arg("mref"), py::arg("uid"), py::arg("incr"));
    m.def(
        "insn_set_operand",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, int operand) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetOperand(sess(h), cls, name, desc, uid, operand);
        },
        py::arg("mref"), py::arg("uid"), py::arg("operand"));
    m.def(
        "insn_set_desc",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, const std::string& type_desc) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetDesc(sess(h), cls, name, desc, uid, type_desc);
        },
        py::arg("mref"), py::arg("uid"), py::arg("type_desc"));
    m.def(
        "insn_set_field_ref",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, const std::string& owner,
                      const std::string& ref_name, const std::string& ref_desc) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetFieldRef(sess(h), cls, name, desc, uid, owner, ref_name, ref_desc);
        },
        py::arg("mref"), py::arg("uid"), py::arg("owner"), py::arg("name"), py::arg("desc"));
    m.def(
        "insn_set_method_ref",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, const std::string& owner,
                      const std::string& ref_name, const std::string& ref_desc, bool itf) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetMethodRef(sess(h), cls, name, desc, uid, owner, ref_name, ref_desc, itf);
        },
        py::arg("mref"), py::arg("uid"), py::arg("owner"), py::arg("name"), py::arg("desc"),
        py::arg("itf"));
    m.def(
        "insn_set_cst",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, py::handle value) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetCst(sess(h), cls, name, desc, uid, anyFromPy(sess(h), value));
        },
        py::arg("mref"), py::arg("uid"), py::arg("value"), "value: str/int/float");
    m.def(
        "insn_set_target",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, ca::u32 target_uid) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetTarget(sess(h), cls, name, desc, uid, target_uid);
        },
        py::arg("mref"), py::arg("uid"), py::arg("target_uid"));

    m.def(
        "emit_insn",
        [sess, memOf](const MemberRef& ref, int op, std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitInsn(sess(h), cls, name, desc, op, pos);
        },
        py::arg("mref"), py::arg("op"), py::arg("pos") = std::nullopt,
        "pos=None 尾插 / pos=uid 插其前 → 新 uid");
    m.def(
        "emit_var",
        [sess, memOf](const MemberRef& ref, int op, int var, std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitVar(sess(h), cls, name, desc, op, var, pos);
        },
        py::arg("mref"), py::arg("op"), py::arg("var"), py::arg("pos") = std::nullopt);
    m.def(
        "emit_iinc",
        [sess, memOf](const MemberRef& ref, int var, int incr, std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitIinc(sess(h), cls, name, desc, var, incr, pos);
        },
        py::arg("mref"), py::arg("var"), py::arg("incr"), py::arg("pos") = std::nullopt);
    m.def(
        "emit_type",
        [sess, memOf](const MemberRef& ref, int op, const std::string& type_desc,
                      std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitType(sess(h), cls, name, desc, op, type_desc, pos);
        },
        py::arg("mref"), py::arg("op"), py::arg("type_desc"), py::arg("pos") = std::nullopt);
    m.def(
        "emit_field",
        [sess, memOf](const MemberRef& ref, int op, const std::string& owner,
                      const std::string& ref_name, const std::string& ref_desc,
                      std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitField(sess(h), cls, name, desc, op, owner, ref_name, ref_desc, pos);
        },
        py::arg("mref"), py::arg("op"), py::arg("owner"), py::arg("name"), py::arg("desc"),
        py::arg("pos") = std::nullopt);
    m.def(
        "emit_method_call",
        [sess, memOf](const MemberRef& ref, int op, const std::string& owner,
                      const std::string& ref_name, const std::string& ref_desc, bool itf,
                      std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitMethodCall(sess(h), cls, name, desc, op, owner, ref_name, ref_desc,
                                        itf, pos);
        },
        py::arg("mref"), py::arg("op"), py::arg("owner"), py::arg("name"), py::arg("desc"),
        py::arg("itf"), py::arg("pos") = std::nullopt);
    m.def(
        "emit_ldc",
        [sess, memOf](const MemberRef& ref, py::handle value, std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitLdc(sess(h), cls, name, desc, anyFromPy(sess(h), value), pos);
        },
        py::arg("mref"), py::arg("value"), py::arg("pos") = std::nullopt,
        "value: str/int/float");
    m.def(
        "emit_jump",
        [sess, memOf](const MemberRef& ref, int op, ca::u32 target_uid,
                      std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitJump(sess(h), cls, name, desc, op, target_uid, pos);
        },
        py::arg("mref"), py::arg("op"), py::arg("target_uid"), py::arg("pos") = std::nullopt);
    m.def(
        "emit_switch",
        [sess, memOf](const MemberRef& ref, const std::string& kind, ca::u32 default_uid,
                      const std::vector<ca::u32>& targets, std::optional<ca::u32> pos, int min,
                      int max, const std::vector<int>& keys) {
            auto [h, cls, name, desc] = memOf(ref);
            auto& s = sess(h);
            if (kind == "table") {
                return facadeEmitTableSwitch(s, cls, name, desc, min, max, default_uid, targets,
                                             pos);
            }
            if (kind == "lookup") {
                return facadeEmitLookupSwitch(s, cls, name, desc, keys, default_uid, targets, pos);
            }
            throw py::value_error("emit_switch: kind must be \"table\" or \"lookup\"");
        },
        py::arg("mref"), py::arg("kind"), py::arg("default_uid"), py::arg("targets"),
        py::arg("pos") = std::nullopt, py::arg("min") = 0, py::arg("max") = 0,
        py::arg("keys") = std::vector<int>{},
        "kind=\"table\": min..max 逐个对应 targets（len(targets)==max-min+1）；"
        "kind=\"lookup\": keys/targets 成对且 keys 严格升序；default_uid 与 targets "
        "均为既有 label uid（emit_jump 同款语义）→ switch 自身的新 uid");
    m.def(
        "emit_label",
        [sess, memOf](const MemberRef& ref, std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitLabel(sess(h), cls, name, desc, pos);
        },
        py::arg("mref"), py::arg("pos") = std::nullopt, "→ label uid");
    m.def(
        "remove_insn",
        [sess, memOf](const MemberRef& ref, ca::u32 uid) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeRemoveInsn(sess(h), cls, name, desc, uid);
        },
        py::arg("mref"), py::arg("uid"), "摘链进墓地；uid 后续解析 fail-closed");

    // ── 异常表 ──
    m.def(
        "try_catches",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            py::list out;
            for (const auto& [s0, e0, h0, t] : facadeTryCatches(sess(h), cls, name, desc))
                out.append(py::make_tuple(s0, e0, h0, t));
            return out;
        },
        py::arg("mref"), "[(start_uid, end_uid, handler_uid, catch_type)]");
    m.def(
        "add_try_catch",
        [sess, memOf](const MemberRef& ref, ca::u32 start_uid, ca::u32 end_uid,
                      ca::u32 handler_uid, const std::string& catch_type) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeAddTryCatch(sess(h), cls, name, desc, start_uid, end_uid, handler_uid,
                              catch_type);
        },
        py::arg("mref"), py::arg("start_uid"), py::arg("end_uid"), py::arg("handler_uid"),
        py::arg("catch_type"), "三 uid 均须为 label 节点");
    m.def(
        "remove_try_catch",
        [sess, memOf](const MemberRef& ref, ca::usize row_index) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeRemoveTryCatch(sess(h), cls, name, desc, row_index);
        },
        py::arg("mref"), py::arg("row_index"));

    // ── 写回/落盘 ──
    m.def(
        "write_back",
        [sess, clsOf](const ClassRef& ref, bool compute_frames) {
            auto [h, name] = clsOf(ref);
            facadeWriteBack(sess(h), name, compute_frames);
        },
        py::arg("cref"), py::arg("compute_frames") = true,
        "树→字节 + 清脏；COMPUTE_FRAMES 写不出即抛（结构/栈校验语义）");
    m.def(
        "flush",
        [sess](ca::u64 h, bool compute_frames) {
            return facadeFlush(sess(h), compute_frames);
        },
        py::arg("h"), py::arg("compute_frames") = true, "全脏树写回 → 写出的类名列表");
    m.def(
        "save",
        [sess](ca::u64 h, const std::string& path, bool compute_frames) {
            facadeSave(sess(h), path, compute_frames);
        },
        py::arg("h"), py::arg("path"), py::arg("compute_frames") = true,
        "flush 全脏 + 两阶段落盘；未触碰类字节原样导出");

    // ── rename token 面（契约 §2.7；单活动 token） ──
    m.def(
        "rename_begin", [sess](ca::u64 h) { facadeRenameBegin(sess(h)); }, py::arg("h"),
        "重置 rename 会话（收计划前调用）");
    m.def(
        "token_rename_class",
        [sess](ca::u64 h, const std::string& old_name, const std::string& new_name) {
            facadeTokenRenameClass(sess(h), old_name, new_name);
        },
        py::arg("h"), py::arg("old_name"), py::arg("new_name"),
        "类改名入计划（点分/internal 均可，内部规范化）");
    m.def(
        "token_rename_method",
        [sess](ca::u64 h, const std::string& owner, const std::string& name,
               const std::string& desc, const std::string& new_name) {
            facadeTokenRenameMethod(sess(h), owner, name, desc, new_name);
        },
        py::arg("h"), py::arg("owner"), py::arg("name"), py::arg("desc"), py::arg("new_name"));
    m.def(
        "token_rename_field",
        [sess](ca::u64 h, const std::string& owner, const std::string& name,
               const std::string& desc, const std::string& new_name) {
            facadeTokenRenameField(sess(h), owner, name, desc, new_name);
        },
        py::arg("h"), py::arg("owner"), py::arg("name"), py::arg("desc"), py::arg("new_name"));
    m.def(
        "rename_commit",
        [sess](ca::u64 h, bool reflection_strings, bool annotations, bool signatures,
               bool resources) {
            facadeTokenCommit(sess(h), reflection_strings, annotations, signatures, resources);
        },
        py::arg("h"), py::arg("reflection_strings") = false, py::arg("annotations") = false,
        py::arg("signatures") = false, py::arg("resources") = true,
        "应用计划到容器（字节码面恒开；resources 默认开：条目 URI 迁移 + "
        "MANIFEST/services 重写）；提交前自动 flush 脏树；成功后全部缓存树作废");
    m.def(
        "mapping_dump",
        [sess](ca::u64 h, const std::string& path, const std::string& fmt) {
            facadeMappingDump(sess(h), path, fmt);
        },
        py::arg("h"), py::arg("path"), py::arg("fmt") = "Enigma",
        "导出 sidecar（Enigma/Tiny-V2/Tiny-V1/ProGuard/SRG/Simple/R8）");
    m.def(
        "mapping_counts",
        [sess](ca::u64 h) {
            const auto [c, mm, f] = facadeMappingCounts(sess(h));
            return py::make_tuple(c, mm, f);
        },
        py::arg("h"), "(class, method, field) 计数");

    // ── 运行报告 ──
    m.def(
        "last_run_report",
        [sess](ca::u64 h) -> py::object {
            auto& session = sess(h);
            if (!session.lastStats) return py::none();
            return py::cast(*session.lastStats);
        },
        py::arg("h"), "最近一次 run_pass 的报告（无则 None）");

    // ── 批量改名 / shadow 显示层 ──
    m.def(
        "rename_apply_suggestions",
        [](ca::u64 h, const std::string& path, const std::string& policy) {
            const auto rep =
                facadeRenameApplySuggestions(FacadeRegistry::instance().get(h), path, policy);
            py::list rejected;
            for (const auto& entry : rep.rejectedEntries) {
                py::dict d;
                d["kind"] = entry.kind;
                d["owner"] = entry.owner;
                d["name"] = entry.name;
                d["desc"] = entry.desc;
                d["new_name"] = entry.newName;
                d["source"] = entry.source;
                d["reason"] = entry.reason;
                d["detail"] = entry.detail;
                rejected.append(d);
            }
            return py::make_tuple(rep.proposed, rep.accepted, rep.rejected,
                                  rep.renamedMembers, rejected);
        },
        py::arg("h"), py::arg("path"), py::arg("policy") = "all",
        "批量改名：读建议文件（v1 schema）→ 引擎裁决+应用 → "
        "(proposed, accepted, rejected, renamed_members, rejected_entries)；"
        "rejected_entries=[{kind, owner, name, desc, new_name, source, reason, detail}]"
        "（fail-closed 报错面：撞名条目自含 owner/old/new/撞名对象定位）；"
        "映射收进 token 面；policy: bytecode-only/bytecode-reflect/bytecode-metadata/all");
    m.def(
        "shadow_generate",
        [](ca::u64 h) {
            const auto rep = facadeShadowGenerate(FacadeRegistry::instance().get(h));
            return py::make_tuple(rep.classes, rep.methods, rep.fields, rep.kept);
        },
        py::arg("h"),
        "机械占位名批量生成（幂等）→ (classes, methods, fields, kept)；token 补号随行");
    m.def(
        "shadow_load",
        [](ca::u64 h, const std::string& path) {
            return facadeShadowLoadFile(FacadeRegistry::instance().get(h), path);
        },
        py::arg("h"), py::arg("path"), "载入 shadow sidecar（严格 schema）→ 条目数");
    m.def(
        "shadow_dump",
        [](ca::u64 h, const std::string& path) {
            facadeShadowDumpFile(FacadeRegistry::instance().get(h), path);
        },
        py::arg("h"), py::arg("path"), "会话 shadow 表写出 sidecar（v2 JSON）");
    m.def(
        "shadow_entries",
        [](ca::u64 h) {
            py::list out;
            for (const auto& [kind, owner, desc, raw, shadow, token] :
                 facadeShadowEntries(FacadeRegistry::instance().get(h)))
                out.append(py::make_tuple(kind, owner, desc, raw, shadow, token));
            return out;
        },
        py::arg("h"), "[(kind, owner, desc, raw, shadow, token8)]；kind ∈ class/field/method");
}

}   // namespace mjt::pysdk
