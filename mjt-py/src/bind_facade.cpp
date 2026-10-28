#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "pysdk_facade.hpp"
#include "pysdk_jasm.hpp"
#include "pysdk_limits.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace py = pybind11;


// 声明在 pysdk_bridge.hpp；本 TU 为定义侧（平函数面绑定）。
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



/// 阶段稳定名与归因门（与 MCP diagnosticJson 同表）：编辑门（4）与重建
/// 管线（0/1/2）分属不同 gate。
std::pair<const char*, const char*> stageNameAndGate(int stage)
{
    switch (stage) {
    case 0: return {"assemble", "rebuild"};
    case 1: return {"disassemble", "rebuild"};
    case 2: return {"validate", "rebuild"};
    case 3: return {"resource", "resource"};
    case 4: return {"edit_gate", "edit"};
    }
    return {"unknown", "unknown"};
}

/// Jasm 报告 → dict（ok / internal_name / written / diagnostics）。
py::dict jasmReportToPy(const JasmReport& report)
{
    py::dict out;
    out["ok"]            = report.ok();
    out["internal_name"] = report.internalName;
    out["written"]       = py::cast(report.written);
    py::list diagnostics;
    for (const auto& diagnostic : report.diagnostics) {
        py::dict entry;
        entry["severity"]   = diagnostic.severity;   // 0=error 1=warning
        entry["stage"]      = diagnostic.stage;      // Assemble/Disassemble/Validate/Resource/EditGate
        const auto gateInfo = stageNameAndGate(diagnostic.stage);
        entry["stage_name"] = gateInfo.first;
        entry["gate"]       = gateInfo.second;
        entry["message"]    = diagnostic.message;
        entry["line"]       = diagnostic.line;
        entry["column"]     = diagnostic.column;
        diagnostics.append(entry);
    }
    out["diagnostics"] = diagnostics;
    return out;
}

py::dict jasmClassTextToPy(const JasmClassText& text)
{
    py::dict out;
    out["internal_name"] = text.internalName;
    out["source"]        = text.source;
    return out;
}

py::dict jasmMethodTextToPy(const JasmMethodText& text)
{
    py::dict out;
    out["internal_name"] = text.internalName;
    out["name"]          = text.name;
    out["descriptor"]    = text.descriptor;
    out["source"]        = text.source;
    return out;
}

/// jar 修复报告 → dict（resource_count/class_count/issues/removed）。
py::dict jarRepairToPy(const JarRepairReport& report)
{
    py::dict out;
    out["resource_count"] = report.resourceCount;
    out["class_count"]    = report.classCount;
    py::list issues;
    for (const auto& issue : report.issues) {
        py::dict entry;
        entry["uri"]      = issue.uri;
        entry["kind"]     = issue.kind;
        entry["message"]  = issue.message;
        entry["size"]     = issue.size;
        entry["selected"] = issue.selected;
        issues.append(entry);
    }
    out["issues"] = issues;
    py::list removed;
    for (const auto& uri : report.removed) removed.append(uri);
    out["removed"] = removed;
    return out;
}

}   // namespace

// ═══════════════════════════════════════════════════════════════════════
// 平函数面：键/bytes 进，值/bytes 出；逐调用按键重解析（容器/条目面 +
// 同文件增树面）。
// ═══════════════════════════════════════════════════════════════════════

void bindFacade(py::module_& m)
{
    // 失效语义异常：StaleRefError 为 KeyError 子类（脚本可 except KeyError 兜底）。
    // 必须用 register_exception（非 exception 构造）才同时装 translator——
    // 只 py::exception<T>(m,...) 建类型不装 translator，旧键 fail-closed 会
    // 全部以 SystemError 逃逸。
    static auto& staleExc = py::register_exception<StaleRefError>(m, "StaleRefError",
                                                                  PyExc_KeyError);
    (void)staleExc;

    // 协作层 deadline 异常：KeyboardInterrupt 子类——运行时上限不该被
    // 脚本里顺手的 except Exception 吞掉（与嵌入层 trace 抛 KeyboardInterrupt
    // 同一家族，except mjt.DeadlineExceeded 可精确捕）。
    static auto& deadlineExc = py::register_exception<DeadlineExceeded>(m, "DeadlineExceeded",
                                                                        PyExc_KeyboardInterrupt);
    (void)deadlineExc;


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
        py::arg("path"), "打开 jar 并导入全部资源，返回新容器句柄。\n\n"
        "Args:\n"
        "    path: jar 文件路径。\n"
        "Returns:\n"
        "    容器句柄 int（后续全部操作显式传参；用完 close_container 释放，\n"
        "    不调则进程退出兜底）。\n"
        "Raises:\n"
        "    ValueError: path 为空。\n"
        "    RuntimeError: 路径不是文件或读取/解析失败（打开失败不泄漏句柄）。\n"
        "    mjt.DeadlineExceeded: 协作层 deadline 越限。");

    m.def("new_container", []() { return FacadeRegistry::instance().create().handle; },
          "新建空容器。\n\n"
          "Returns:\n"
          "    容器句柄 int（用完 close_container 释放）。");

    m.def(
        "close_container", [](ca::u64 h) { FacadeRegistry::instance().close(h); }, py::arg("h"),
        "显式释放容器。\n\n"
        "Args:\n"
        "    h: 容器句柄。此后该容器全部键 fail-closed（不调则进程退出兜底）。");

    // ── 条目面 ──
    m.def(
        "list_entries",
        [](ca::u64 h) {
            py::list out;
            for (const auto& [uri, type] : facadeListEntries(FacadeRegistry::instance().get(h)))
                out.append(py::make_tuple(uri, type));
            return out;
        },
        py::arg("h"), "容器全部条目清单。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "Returns:\n"
        "    [(uri, 类型名)] 列表——含 class/MANIFEST 等非类资源。");

    m.def(
        "get_entry_bytes",
        [](ca::u64 h, const std::string& uri) {
            return toPyBytes(facadeGetEntryBytes(FacadeRegistry::instance().get(h), uri));
        },
        py::arg("h"), py::arg("uri"), "读取条目原始字节。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    uri: 条目 URI。\n"
        "Returns:\n"
        "    bytes（class/manifest/任意资源均可读）。\n"
        "Raises:\n"
        "    StaleRefError: 条目不存在。");

    m.def(
        "set_entry_bytes",
        [](ca::u64 h, const std::string& uri, py::bytes data) {
            facadeSetEntryBytes(FacadeRegistry::instance().get(h), uri, fromPyBytes(data));
        },
        py::arg("h"), py::arg("uri"), py::arg("bytes"), "替换条目字节。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    uri: 条目 URI。\n"
        "    bytes: 新字节。\n"
        "Raises:\n"
        "    StaleRefError: 条目不存在。类条目覆盖后缓存树作废重解析；\n"
        "    manifest 条目重析；其他资源直换。");

    m.def(
        "add_entry",
        [](ca::u64 h, const std::string& uri, py::bytes data) {
            facadeAddEntry(FacadeRegistry::instance().get(h), uri, fromPyBytes(data));
        },
        py::arg("h"), py::arg("uri"), py::arg("bytes"),
        "新增条目。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    uri: 条目 URI（已存在即拒）。\n"
        "    bytes: 条目字节。类字节自动进 name↔URI 索引；容器内同名类\n"
        "        按 JVM first-wins 保留首条，落选者经 duplicate_names 可查。\n"
        "Raises:\n"
        "    StaleRefError: URI 已存在，或句柄失效。");

    m.def(
        "remove_entry",
        [](ca::u64 h, const std::string& uri) { facadeRemoveEntry(FacadeRegistry::instance().get(h), uri); },
        py::arg("h"), py::arg("uri"), "删除条目。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    uri: 条目 URI。索引与树缓存同步清理；同名落选类按序晋升。\n"
        "Raises:\n"
        "    StaleRefError: 条目不存在。");

    m.def(
        "load_class_file",
        [](ca::u64 h, const std::string& path, std::optional<std::string> uri, bool replace) {
            facadeLoadClassFile(FacadeRegistry::instance().get(h), path, std::move(uri), replace);
        },
        py::arg("h"), py::arg("path"), py::arg("uri") = std::nullopt,
        py::arg("replace") = false, "把磁盘散装 .class 文件装入容器。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    path: .class 文件路径。\n"
        "    uri: 条目 URI；缺省取文件名。\n"
        "    replace: 同名条目已存在时是否覆盖；缺省抛错。\n"
        "Raises:\n"
        "    StaleRefError: URI 冲突且 replace=False，或文件解析失败。");

    // ── 类名/类字节（索引 O(1)） ──
    m.def("class_names",
          [](ca::u64 h) { return facadeClassNames(FacadeRegistry::instance().get(h)); }, py::arg("h"),
          "列出容器内已解析类名。\n\n"
          "Args:\n"
          "    h: 容器句柄。\n"
          "Returns:\n"
          "    internal name（a/b/C 形态）升序列表。");
    m.def("class_uri",
          [](ca::u64 h, const std::string& name) { return facadeClassUri(FacadeRegistry::instance().get(h), name); },
          py::arg("h"), py::arg("name"), "类名反查条目 URI。\n\n"
          "Args:\n"
          "    h: 容器句柄。\n"
          "    name: internal name。\n"
          "Returns:\n"
          "    条目 URI 字符串。\n"
          "Raises:\n"
          "    StaleRefError: 类名未索引。");
    m.def("duplicate_names",
          [](ca::u64 h) { return facadeDuplicateNames(FacadeRegistry::instance().get(h)); }, py::arg("h"),
          "列出同名类的 first-wins 落选者。\n\n"
          "Args:\n"
          "    h: 容器句柄。\n"
          "Returns:\n"
          "    internal name 升序列表（落选条目不可经类名寻址）。");
    m.def(
        "class_bytes",
        [](ca::u64 h, const std::string& name) {
            return toPyBytes(facadeClassBytes(FacadeRegistry::instance().get(h), name));
        },
        py::arg("h"), py::arg("name"), "读取类字节（不建树）。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    name: internal name。\n"
        "Returns:\n"
        "    bytes（落账字节；脏树不参与）。\n"
        "Raises:\n"
        "    StaleRefError: 类名未索引。");
    m.def(
        "set_class_bytes",
        [](ca::u64 h, const std::string& name, py::bytes data) {
            facadeSetClassBytes(FacadeRegistry::instance().get(h), name, fromPyBytes(data));
        },
        py::arg("h"), py::arg("name"), py::arg("bytes"),
        "写回/新增类字节。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    name: internal name；不存在则以 name + \".class\" 为 URI 新增条目。\n"
        "    bytes: 类字节。覆盖时缓存树作废重解析。\n"
        "Raises:\n"
        "    StaleRefError: 句柄失效或字节头解析不出类名。");

    // ── 导出 ──
    m.def(
        "export_jar",
        [](ca::u64 h, const std::string& path, std::vector<std::string> uris) {
            facadeExportJar(FacadeRegistry::instance().get(h), path, uris);
        },
        py::arg("h"), py::arg("path"), py::arg("uris") = std::vector<std::string>{},
        "导出 jar。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    path: 输出 jar 路径。\n"
        "    uris: 子集条目清单；空表导出全量。\n\n"
        "导出前自动 flush 全部脏树并落账挂账改名（两阶段落盘：暂存 + 改名）。\n"
        "Raises:\n"
        "    RuntimeError: 落盘失败或 COMPUTE_FRAMES 写出失败。");
    m.def(
        "export_class",
        [](ca::u64 h, const std::string& name, const std::string& path) {
            facadeExportClass(FacadeRegistry::instance().get(h), name, path);
        },
        py::arg("h"), py::arg("name"), py::arg("path"),
        "单类导出为 .class 文件。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    name: internal name。\n"
        "    path: 输出文件路径。脏树先写回，挂账改名一并落账。\n"
        "Raises:\n"
        "    StaleRefError: 类名不存在。\n"
        "    RuntimeError: COMPUTE_FRAMES 写出或落盘失败。");

    // ── 完整性 / 条目落账 ──
    m.def(
        "sync_entries",
        [](ca::u64 h) { return facadeSyncEntries(FacadeRegistry::instance().get(h)); },
        py::arg("h"), "落账全部挂账的类改名条目搬移（老 URI 字节 → 新 URI + 索引"
        "迁移）。\n\n"
        "Args:\n"
        "    h: 容器句柄。write_back/flush/save/export 出口自动调用，也可显式调。\n"
        "Returns:\n"
        "    [\"old -> new\"] 迁移清单（无挂账为空表）。");
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
        "容器完整性体检。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "Returns:\n"
        "    {match, mismatch, malformed, dep_missing, non_class} 计数 +\n"
        "    {mismatch_details, malformed_details, dep_missing_details} 明细\n"
        "    [(entry, detail)]。口径：逐 class 条目解析（malformed）、entry 路径\n"
        "    与 this_class 声明比对（mismatch）、常量池引用的容器外非 JDK 类清点\n"
        "    （dep_missing）；只读不改动。");
    m.def(
        "sanitize_lvt",
        [](ca::u64 h) {
            const auto rep = facadeSanitizeLvt(FacadeRegistry::instance().get(h));
            return py::make_tuple(rep.dropped, rep.deduped, rep.classes);
        },
        py::arg("h"), "LVT 清洗：剔除脱链/乱序局部变量表条目并去重。\n\n"
        "Args:\n"
        "    h: 容器句柄（全容器逐类逐方法；改动标脏）。\n"
        "Returns:\n"
        "    (dropped, deduped, classes) 计数三元组。");

    // ═══════════════════════════════════════════════════════════════════
    // 树函数面。cref=(h, 类名)、mref/fref=(h, 类名, 成员名, 描述符)。
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

    // 对齐 MCP 工具面：desc 可省略——省略时按成员名在宿主类内唯一解析
    // 回填；名字不存在报缺失，真重载（同多名异描述符）报歧义并列候选。
    auto resolveMemberDesc = [sess](ca::u64 h, const std::string& cls,
                                    const std::string& name, bool fields) {
        std::vector<std::string> hits;
        for (const auto& [memberName, memberDesc, memberAccess] :
             facadeMembers(sess(h), cls, fields)) {
            (void)memberAccess;
            if (memberName != name) continue;
            if (std::find(hits.begin(), hits.end(), memberDesc) == hits.end())
                hits.push_back(memberDesc);
        }
        if (hits.size() == 1) return hits.front();
        if (hits.empty())
            throw StaleRefError(std::string(fields ? "field" : "method") + " not found: " +
                                cls + "." + name);
        std::string message = std::string("desc is ambiguous for ") + cls + "." + name +
                              "; candidates: ";
        for (std::size_t index = 0; index < hits.size(); ++index) {
            if (index > 0) message += ", ";
            message += hits[index];
        }
        throw StaleRefError(message);
    };

    auto pyDetail = [](const InsnDetail& d) {
        py::dict out;
        out["uid"]     = d.uid;
        out["opcode"]  = d.opcode;
        out["kind"]    = facadeInsnKindName(d.kind);
        out["mnemonic"] = (d.opcode >= 0 && d.opcode <= 201) ? facadeInsnMnemonic(d.opcode) : "?";
        if (d.kind == insn_kind::kInt) out["operand"] = d.operand;
        if (d.kind == insn_kind::kVar) out["var"] = d.var;
        if (d.kind == insn_kind::kType) out["desc"] = d.desc;
        if (d.kind == insn_kind::kField) {
            out["owner"] = d.owner;
            out["name"]  = d.name;
            out["desc"]  = d.desc;
        }
        if (d.kind == insn_kind::kMethod) {
            out["owner"] = d.owner;
            out["name"]  = d.name;
            out["desc"]  = d.desc;
            out["itf"]   = d.itf;
        }
        if (d.kind == insn_kind::kIndy) {
            out["name"] = d.name;
            out["desc"] = d.desc;
        }
        if (d.kind == insn_kind::kJump) out["target"] = d.targetUid;
        if (d.kind == insn_kind::kLdc) {
            out["cst_tag"] = d.cstTag;
            if (d.cstTag == 1) out["cst"] = d.cstStr;
            else if (d.cstTag == 2 || d.cstTag == 3) out["cst"] = static_cast<long long>(d.cstInt);
            else if (d.cstTag == 4 || d.cstTag == 5) out["cst"] = d.cstFloat;
            else out["cst"] = py::none();
        }
        if (d.kind == insn_kind::kIinc) {
            out["var"]  = d.var;
            out["incr"] = d.incr;
        }
        if (d.kind == insn_kind::kTableSwitch ||
            d.kind == insn_kind::kLookupSwitch) {
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
        if (d.kind == insn_kind::kMultiANewArray) {
            out["desc"] = d.desc;
            out["dims"] = d.dims;
        }
        if (d.kind == insn_kind::kLine) out["line"] = d.line;
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
        py::arg("cref"), "附着解析类（幂等，进树缓存）。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "Returns:\n"
        "    同一 cref（链式书写用）。\n"
        "Raises:\n"
        "    StaleRefError: 类不存在或解析失败。");

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
        py::arg("cref"), "读取类声明信息。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "Returns:\n"
        "    {name, super, access, version, interfaces}。");

    m.def(
        "methods",
        [sess, clsOf](const ClassRef& ref) {
            auto [h, name] = clsOf(ref);
            py::list out;
            for (const auto& [n, d, a] : facadeMembers(sess(h), name, true))
                out.append(py::make_tuple(n, d, a));
            return out;
        },
        py::arg("cref"), "列出方法声明。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "Returns:\n"
        "    [(name, desc, access)] 列表（声明序；desc 为 JVM 描述符，access\n"
        "    为访问标志位集，可对照 mjt.acc）。\n"
        "Raises:\n"
        "    StaleRefError: 类不存在。");
    m.def(
        "fields",
        [sess, clsOf](const ClassRef& ref) {
            auto [h, name] = clsOf(ref);
            py::list out;
            for (const auto& [n, d, a] : facadeMembers(sess(h), name, false))
                out.append(py::make_tuple(n, d, a));
            return out;
        },
        py::arg("cref"), "列出字段声明。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "Returns:\n"
        "    [(name, desc, access)] 列表（声明序）。\n"
        "Raises:\n"
        "    StaleRefError: 类不存在。");
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
        "查询继承闭包（super 链 + 全部接口传递闭包 + 沿途声明成员集）。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "Returns:\n"
        "    {classes: [{name, super, interfaces}]}（self 先序 BFS；容器外父类/\n"
        "    接口如 java/lang/Object 不入闭包），methods/fields:\n"
        "    [(owner, name, desc, access)] 沿途容器内类的声明成员集——建继承\n"
        "    成员集不必再外跑 javap/反射。\n"
        "Raises:\n"
        "    StaleRefError: 起点类不存在。");

    m.def(
        "get_method",
        [sess, clsOf, methodRef, resolveMemberDesc](const ClassRef& ref, const std::string& name,
                                                    const std::string& desc) {
            auto [h, cls] = clsOf(ref);
            const std::string resolvedDesc =
                desc.empty() ? resolveMemberDesc(h, cls, name, false) : desc;
            facadeMethodInfo(sess(h), cls, name, resolvedDesc);   // 存在性校验
            return methodRef(h, cls, name, resolvedDesc);
        },
        py::arg("cref"), py::arg("name"), py::arg("desc") = std::string{},
        "取方法键（存在性校验）。desc 可省略：省略时按方法名在宿主类内唯一\n"
        "解析回填（真重载报歧义并列候选，名字不存在报缺失）。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "    name: 方法名。\n"
        "    desc: 方法描述符；可省略，省略时按名字唯一解析。\n"
        "Returns:\n"
        "    mref (h, cls, name, desc)。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在或同名重载歧义。");
    m.def(
        "get_field",
        [sess, clsOf, methodRef, resolveMemberDesc](const ClassRef& ref, const std::string& name,
                                                    const std::string& desc) {
            auto [h, cls] = clsOf(ref);
            const std::string resolvedDesc =
                desc.empty() ? resolveMemberDesc(h, cls, name, true) : desc;
            facadeFieldInfo(sess(h), cls, name, resolvedDesc);
            return methodRef(h, cls, name, resolvedDesc);
        },
        py::arg("cref"), py::arg("name"), py::arg("desc") = std::string{},
        "取字段键（存在性校验）。desc 可省略：省略时按字段名在宿主类内唯一\n"
        "解析回填（同名列异描述符报歧义并列候选，名字不存在报缺失）。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "    name: 字段名。\n"
        "    desc: 字段描述符；可省略，省略时按名字唯一解析。\n"
        "Returns:\n"
        "    fref (h, cls, name, desc)。\n"
        "Raises:\n"
        "    StaleRefError: 字段不存在或同名列歧义。");

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
        py::arg("mref"), "读取方法声明信息。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "Returns:\n"
        "    {name, desc, access, max_stack, max_locals, exceptions}。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在。");
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
        py::arg("fref"), "读取字段声明信息。\n\n"
        "Args:\n"
        "    fref: 字段键 (h, cls, name, desc)。\n"
        "Returns:\n"
        "    {name, desc, access}。\n"
        "Raises:\n"
        "    StaleRefError: 字段不存在。");

    // ── 写（类） ──
    m.def(
        "class_set_super",
        [sess, clsOf](const ClassRef& ref, const std::string& super) {
            auto [h, name] = clsOf(ref);
            facadeClassSetSuper(sess(h), name, super);
        },
        py::arg("cref"), py::arg("super"), "改写类父类。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "    super: 新父类 internal name（树编辑；引用重写走 rename token 面）。\n"
        "Raises:\n"
        "    StaleRefError: 类不存在。");
    m.def(
        "class_set_access",
        [sess, clsOf](const ClassRef& ref, int access) {
            auto [h, name] = clsOf(ref);
            facadeClassSetAccess(sess(h), name, access);
        },
        py::arg("cref"), py::arg("access"), "改写类访问标志（整字替换，位集对照 mjt.acc）。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "    access: 新访问标志位集。\n"
        "Raises:\n"
        "    StaleRefError: 类不存在。");
    m.def(
        "class_set_version",
        [sess, clsOf](const ClassRef& ref, int version) {
            auto [h, name] = clsOf(ref);
            facadeClassSetVersion(sess(h), name, version);
        },
        py::arg("cref"), py::arg("version"), "改写类文件版本号。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "    version: 新版本号（major，如 52 = Java 8）。\n"
        "Raises:\n"
        "    StaleRefError: 类不存在。");
    m.def(
        "class_set_interfaces",
        [sess, clsOf](const ClassRef& ref, const std::vector<std::string>& ifaces) {
            auto [h, name] = clsOf(ref);
            facadeClassSetInterfaces(sess(h), name, ifaces);
        },
        py::arg("cref"), py::arg("interfaces"), "整表替换类接口列表。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "    interfaces: 接口 internal name 列表（整表替换，不追加）。\n"
        "Raises:\n"
        "    StaleRefError: 类不存在。");
    m.def(
        "rename_class",
        [sess, clsOf, classRef](const ClassRef& ref, const std::string& new_name) {
            auto [h, name] = clsOf(ref);
            return classRef(h, facadeRenameClass(sess(h), name, new_name));
        },
        py::arg("cref"), py::arg("new_name"), "类改名并迁移索引。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 旧类名)。\n"
        "    new_name: 新 internal name。\n"
        "Returns:\n"
        "    新 cref；旧 cref 随即失效。只改类自身身份，引用重写走 rename\n"
        "    token 面；条目 URI 搬移在写回/导出边界落账（sync_entries）。\n"
        "Raises:\n"
        "    StaleRefError: 旧类不存在或新名已占用。");

    // ── 写（方法/字段） ──
    m.def(
        "rename_method",
        [sess, memOf, methodRef](const MemberRef& ref, const std::string& new_name) {
            auto [h, cls, name, desc] = memOf(ref);
            return methodRef(h, cls, facadeRenameMethod(sess(h), cls, name, desc, new_name), desc);
        },
        py::arg("mref"), py::arg("new_name"), "方法改名。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "    new_name: 新方法名。\n"
        "Returns:\n"
        "    新 mref；旧 mref 随即失效。只改声明，调用点重写走 rename token 面。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在或新名冲突。");
    m.def(
        "rename_field",
        [sess, memOf, methodRef](const MemberRef& ref, const std::string& new_name) {
            auto [h, cls, name, desc] = memOf(ref);
            return methodRef(h, cls, facadeRenameField(sess(h), cls, name, desc, new_name), desc);
        },
        py::arg("fref"), py::arg("new_name"), "字段改名。\n\n"
        "Args:\n"
        "    fref: 字段键 (h, cls, name, desc)。\n"
        "    new_name: 新字段名。\n"
        "Returns:\n"
        "    新 fref；旧 fref 随即失效。\n"
        "Raises:\n"
        "    StaleRefError: 字段不存在或新名冲突。");
    m.def(
        "method_set_desc",
        [sess, memOf, methodRef](const MemberRef& ref, const std::string& new_desc) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeMethodSetDesc(sess(h), cls, name, desc, new_desc);
            return methodRef(h, cls, name, new_desc);
        },
        py::arg("mref"), py::arg("new_desc"), "改写方法描述符。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "    new_desc: 新 JVM 方法描述符。方法体不随之改写。\n"
        "Returns:\n"
        "    新 mref；旧 mref 随即失效。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在。");
    m.def(
        "method_set_access",
        [sess, memOf](const MemberRef& ref, int access) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeMethodSetAccess(sess(h), cls, name, desc, access);
        },
        py::arg("mref"), py::arg("access"), "改写方法访问标志（整字替换，位集对照 mjt.acc）。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "    access: 新访问标志位集。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在。");
    m.def(
        "method_set_max_stack",
        [sess, memOf](const MemberRef& ref, int v) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeMethodSetMaxStack(sess(h), cls, name, desc, v);
        },
        py::arg("mref"), py::arg("v"), "改写方法 max_stack。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "    v: 新 max_stack 值。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在。");
    m.def(
        "method_set_max_locals",
        [sess, memOf](const MemberRef& ref, int v) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeMethodSetMaxLocals(sess(h), cls, name, desc, v);
        },
        py::arg("mref"), py::arg("v"), "改写方法 max_locals。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "    v: 新 max_locals 值。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在。");
    m.def(
        "method_set_exceptions",
        [sess, memOf](const MemberRef& ref, const std::vector<std::string>& exceptions) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeMethodSetExceptions(sess(h), cls, name, desc, exceptions);
        },
        py::arg("mref"), py::arg("exceptions"), "整表替换方法 throws 声明。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "    exceptions: 异常类 internal name 列表（整表替换）。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在。");
    m.def(
        "field_set_access",
        [sess, memOf](const MemberRef& ref, int access) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeFieldSetAccess(sess(h), cls, name, desc, access);
        },
        py::arg("fref"), py::arg("access"), "改写字段访问标志（整字替换，位集对照 mjt.acc）。\n\n"
        "Args:\n"
        "    fref: 字段键 (h, cls, name, desc)。\n"
        "    access: 新访问标志位集。\n"
        "Raises:\n"
        "    StaleRefError: 字段不存在。");
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
        py::arg("max_stack"), py::arg("max_locals"), "新增方法。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "    name: 方法名。\n"
        "    desc: JVM 方法描述符。\n"
        "    access: 访问标志位集（对照 mjt.acc）。\n"
        "    max_stack: 初始 max_stack。\n"
        "    max_locals: 初始 max_locals。\n"
        "Returns:\n"
        "    新方法 mref（空方法体）。\n"
        "Raises:\n"
        "    StaleRefError: 键冲突（同名同描述符已存在）或类不存在。");
    m.def(
        "add_field",
        [sess, clsOf, methodRef](const ClassRef& ref, const std::string& name,
                                 const std::string& desc, int access) {
            auto [h, cls] = clsOf(ref);
            facadeAddField(sess(h), cls, name, desc, access);
            return methodRef(h, cls, name, desc);
        },
        py::arg("cref"), py::arg("name"), py::arg("desc"), py::arg("access"), "新增字段。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "    name: 字段名。\n"
        "    desc: JVM 字段描述符。\n"
        "    access: 访问标志位集。\n"
        "Returns:\n"
        "    新字段 fref。\n"
        "Raises:\n"
        "    StaleRefError: 键冲突或类不存在。");
    m.def(
        "remove_method",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeRemoveMethod(sess(h), cls, name, desc);
        },
        py::arg("mref"), "删除方法（摘链并销毁；mref 与其全部指令 uid 随即失效）。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在。");
    m.def(
        "remove_field",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeRemoveField(sess(h), cls, name, desc);
        },
        py::arg("fref"), "删除字段（摘链并销毁；fref 随即失效）。\n\n"
        "Args:\n"
        "    fref: 字段键 (h, cls, name, desc)。\n"
        "Raises:\n"
        "    StaleRefError: 字段不存在。");

    // ── 指令面（uid 跨结构编辑稳定；remove 后进墓地解析即抛） ──
    m.def(
        "insn_count",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeInsnCount(sess(h), cls, name, desc);
        },
        py::arg("mref"), "方法体节点总数（含 label/line/frame 节点）。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "Returns:\n"
        "    int 节点数。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在。");
    m.def(
        "insns",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            py::list out;
            for (const auto& b : facadeInsns(sess(h), cls, name, desc))
                out.append(py::make_tuple(b.uid, b.opcode, facadeInsnKindName(b.kind)));
            return out;
        },
        py::arg("mref"), "列出方法体全部节点。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "Returns:\n"
        "    [(uid, opcode, kind)] 链表序列表（含 label/line/frame 节点；\n"
        "    kind 名见模块 doc）。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在。");
    m.def(
        "insn_detail",
        [sess, memOf, pyDetail](const MemberRef& ref, ca::u32 uid) {
            auto [h, cls, name, desc] = memOf(ref);
            return pyDetail(facadeInsnDetail(sess(h), cls, name, desc, uid));
        },
        py::arg("mref"), py::arg("uid"), "读取单节点详情（按 kind 展开全字段）。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "    uid: 节点 uid。\n"
        "Returns:\n"
        "    dict——公共字段 uid/opcode/kind/mnemonic；按 kind 附操作数字段：\n"
        "    int→operand、var→var、type→desc、field/method→owner/name/desc\n"
        "    （method 另有 itf）、ldc→cst_tag/cst、jump→target、iinc→var/incr、\n"
        "    switch→min/max/default/targets/keys、multianewarray→desc/dims、\n"
        "    line→line。\n"
        "Raises:\n"
        "    StaleRefError: uid 不存在或已删除（墓地 fail-closed）。");
    m.def(
        "insn_index",
        [sess, memOf](const MemberRef& ref, ca::u32 uid) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeInsnIndex(sess(h), cls, name, desc, uid);
        },
        py::arg("mref"), py::arg("uid"), "查节点链表位次（诊断用，O(n)；定位用 uid 不用位次）。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "    uid: 节点 uid。\n"
        "Returns:\n"
        "    int 位次。\n"
        "Raises:\n"
        "    StaleRefError: uid 不存在。");
    m.def(
        "first_insn",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeFirstInsn(sess(h), cls, name, desc);
        },
        py::arg("mref"), "取首个节点 uid（空方法体为 None）。");
    m.def(
        "last_insn",
        [sess, memOf](const MemberRef& ref) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeLastInsn(sess(h), cls, name, desc);
        },
        py::arg("mref"), "取末个节点 uid（空方法体为 None）。");
    m.def(
        "next_insn",
        [sess, memOf](const MemberRef& ref, ca::u32 uid) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeNextInsn(sess(h), cls, name, desc, uid);
        },
        py::arg("mref"), py::arg("uid"), "取后继节点 uid（无后继为 None）。");
    m.def(
        "prev_insn",
        [sess, memOf](const MemberRef& ref, ca::u32 uid) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadePrevInsn(sess(h), cls, name, desc, uid);
        },
        py::arg("mref"), py::arg("uid"), "取前驱节点 uid（无前驱为 None）。");

    m.def(
        "insn_set_opcode",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, int op) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetOpcode(sess(h), cls, name, desc, uid, op);
        },
        py::arg("mref"), py::arg("uid"), py::arg("op"), "改写节点 opcode。\n\n"
        "Args:\n"
        "    mref: 方法键；uid: 节点 uid；op: 新 opcode 值（对照 mjt.opcodes）。\n"
        "Raises:\n"
        "    StaleRefError: uid 不存在。");
    m.def(
        "insn_set_var",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, int var) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetVar(sess(h), cls, name, desc, uid, var);
        },
        py::arg("mref"), py::arg("uid"), py::arg("var"), "改写局部变量索引（var 类节点）。\n\n"
        "Args:\n"
        "    mref: 方法键；uid: 节点 uid；var: 新局部变量槽位号。\n"
        "Raises:\n"
        "    StaleRefError: uid 不存在。");
    m.def(
        "insn_set_incr",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, int incr) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetIncr(sess(h), cls, name, desc, uid, incr);
        },
        py::arg("mref"), py::arg("uid"), py::arg("incr"), "改写 iinc 增量。\n\n"
        "Args:\n"
        "    mref: 方法键；uid: iinc 节点 uid；incr: 新增量。\n"
        "Raises:\n"
        "    StaleRefError: uid 不存在。");
    m.def(
        "insn_set_operand",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, int operand) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetOperand(sess(h), cls, name, desc, uid, operand);
        },
        py::arg("mref"), py::arg("uid"), py::arg("operand"), "改写 int 操作数（bipush/sipush/newarray 类节点）。\n\n"
        "Args:\n"
        "    mref: 方法键；uid: 节点 uid；operand: 新操作数值。\n"
        "Raises:\n"
        "    StaleRefError: uid 不存在。");
    m.def(
        "insn_set_desc",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, const std::string& type_desc) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetDesc(sess(h), cls, name, desc, uid, type_desc);
        },
        py::arg("mref"), py::arg("uid"), py::arg("type_desc"), "改写类型操作数（type/multianewarray 类节点）。\n\n"
        "Args:\n"
        "    mref: 方法键；uid: 节点 uid；type_desc: 新类型描述符。\n"
        "Raises:\n"
        "    StaleRefError: uid 不存在。");
    m.def(
        "insn_set_field_ref",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, const std::string& owner,
                      const std::string& ref_name, const std::string& ref_desc) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetFieldRef(sess(h), cls, name, desc, uid, owner, ref_name, ref_desc);
        },
        py::arg("mref"), py::arg("uid"), py::arg("owner"), py::arg("name"), py::arg("desc"),
        "改写字段引用（field 类节点）。\n\n"
        "Args:\n"
        "    mref: 方法键；uid: 节点 uid。\n"
        "    owner/name/desc: 新字段引用坐标（owner 为 internal name，desc 为\n"
        "    字段描述符）。\n"
        "Raises:\n"
        "    StaleRefError: uid 不存在。");
    m.def(
        "insn_set_method_ref",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, const std::string& owner,
                      const std::string& ref_name, const std::string& ref_desc, bool itf) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetMethodRef(sess(h), cls, name, desc, uid, owner, ref_name, ref_desc, itf);
        },
        py::arg("mref"), py::arg("uid"), py::arg("owner"), py::arg("name"), py::arg("desc"),
        py::arg("itf"),
        "改写方法引用（method 类节点）。\n\n"
        "Args:\n"
        "    mref: 方法键；uid: 节点 uid。\n"
        "    owner/name/desc: 新方法引用坐标。\n"
        "    itf: 是否接口方法。\n"
        "Raises:\n"
        "    StaleRefError: uid 不存在。");
    m.def(
        "insn_set_cst",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, py::handle value) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetCst(sess(h), cls, name, desc, uid, anyFromPy(sess(h), value));
        },
        py::arg("mref"), py::arg("uid"), py::arg("value"), "改写 ldc 载荷。\n\n"
        "Args:\n"
        "    mref: 方法键；uid: ldc 节点 uid；value: 新载荷（str/int/float）。\n"
        "Raises:\n"
        "    TypeError: value 不是 str/int/float。\n"
        "    StaleRefError: uid 不存在。");
    m.def(
        "insn_set_target",
        [sess, memOf](const MemberRef& ref, ca::u32 uid, ca::u32 target_uid) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeInsnSetTarget(sess(h), cls, name, desc, uid, target_uid);
        },
        py::arg("mref"), py::arg("uid"), py::arg("target_uid"), "改写跳转目标 label。\n\n"
        "Args:\n"
        "    mref: 方法键；uid: jump 节点 uid；target_uid: 新目标 label uid。\n"
        "Raises:\n"
        "    StaleRefError: uid 或目标 label 不存在。");

    m.def(
        "emit_insn",
        [sess, memOf](const MemberRef& ref, int op, std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitInsn(sess(h), cls, name, desc, op, pos);
        },
        py::arg("mref"), py::arg("op"), py::arg("pos") = std::nullopt,
        "发零操作数指令。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "    op: opcode 值（对照 mjt.opcodes）。\n"
        "    pos: 插入位置——None 尾插；给 uid 则插在该节点前。\n"
        "Returns:\n"
        "    新节点 uid。\n"
        "Raises:\n"
        "    StaleRefError: 方法或 pos uid 不存在。");
    m.def(
        "emit_var",
        [sess, memOf](const MemberRef& ref, int op, int var, std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitVar(sess(h), cls, name, desc, op, var, pos);
        },
        py::arg("mref"), py::arg("op"), py::arg("var"), py::arg("pos") = std::nullopt,
        "发局部变量指令（aload/astore/iload/…）。\n\n"
        "Args:\n"
        "    mref: 方法键；op: opcode 值；var: 局部变量槽位号。\n"
        "    pos: 插入位置——None 尾插；给 uid 则插在该节点前。\n"
        "Returns:\n"
        "    新节点 uid。");
    m.def(
        "emit_iinc",
        [sess, memOf](const MemberRef& ref, int var, int incr, std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitIinc(sess(h), cls, name, desc, var, incr, pos);
        },
        py::arg("mref"), py::arg("var"), py::arg("incr"), py::arg("pos") = std::nullopt,
        "发 iinc 指令。\n\n"
        "Args:\n"
        "    mref: 方法键；var: 局部变量槽位号；incr: 增量。\n"
        "    pos: 插入位置——None 尾插；给 uid 则插在该节点前。\n"
        "Returns:\n"
        "    新节点 uid。");
    m.def(
        "emit_type",
        [sess, memOf](const MemberRef& ref, int op, const std::string& type_desc,
                      std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitType(sess(h), cls, name, desc, op, type_desc, pos);
        },
        py::arg("mref"), py::arg("op"), py::arg("type_desc"), py::arg("pos") = std::nullopt,
        "发类型操作数指令（new/anewarray/checkcast/instanceof/…）。\n\n"
        "Args:\n"
        "    mref: 方法键；op: opcode 值；type_desc: 类型描述符。\n"
        "    pos: 插入位置——None 尾插；给 uid 则插在该节点前。\n"
        "Returns:\n"
        "    新节点 uid。");
    m.def(
        "emit_field",
        [sess, memOf](const MemberRef& ref, int op, const std::string& owner,
                      const std::string& ref_name, const std::string& ref_desc,
                      std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitField(sess(h), cls, name, desc, op, owner, ref_name, ref_desc, pos);
        },
        py::arg("mref"), py::arg("op"), py::arg("owner"), py::arg("name"), py::arg("desc"),
        py::arg("pos") = std::nullopt,
        "发字段访问指令（getstatic/putstatic/getfield/putfield）。\n\n"
        "Args:\n"
        "    mref: 方法键；op: opcode 值。\n"
        "    owner/name/desc: 目标字段坐标（owner 为 internal name，desc 为\n"
        "    字段描述符）。\n"
        "    pos: 插入位置——None 尾插；给 uid 则插在该节点前。\n"
        "Returns:\n"
        "    新节点 uid。");
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
        py::arg("itf"), py::arg("pos") = std::nullopt,
        "发方法调用指令（invokestatic/virtual/special/interface）。\n\n"
        "Args:\n"
        "    mref: 方法键；op: opcode 值。\n"
        "    owner/name/desc: 被调方法坐标。\n"
        "    itf: 是否接口方法。\n"
        "    pos: 插入位置——None 尾插；给 uid 则插在该节点前。\n"
        "Returns:\n"
        "    新节点 uid。");
    m.def(
        "emit_ldc",
        [sess, memOf](const MemberRef& ref, py::handle value, std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitLdc(sess(h), cls, name, desc, anyFromPy(sess(h), value), pos);
        },
        py::arg("mref"), py::arg("value"), py::arg("pos") = std::nullopt,
        "发 ldc 常量压栈。\n\n"
        "Args:\n"
        "    mref: 方法键。\n"
        "    value: 常量载荷（str/int/float；str 经会话 arena intern，按值\n"
        "    安全；py float 落 double ldc；int 溢出 int32 升 i64 载荷）。\n"
        "    pos: 插入位置——None 尾插；给 uid 则插在该节点前。\n"
        "Returns:\n"
        "    新节点 uid。\n"
        "Raises:\n"
        "    TypeError: value 不是 str/int/float。");
    m.def(
        "emit_jump",
        [sess, memOf](const MemberRef& ref, int op, ca::u32 target_uid,
                      std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitJump(sess(h), cls, name, desc, op, target_uid, pos);
        },
        py::arg("mref"), py::arg("op"), py::arg("target_uid"), py::arg("pos") = std::nullopt,
        "发跳转指令（目标须为既有 label uid）。\n\n"
        "Args:\n"
        "    mref: 方法键；op: opcode 值；target_uid: 目标 label 节点 uid。\n"
        "    pos: 插入位置——None 尾插；给 uid 则插在该节点前。\n"
        "Returns:\n"
        "    新节点 uid。");
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
        "发 switch 指令。\n\n"
        "Args:\n"
        "    mref: 方法键。\n"
        "    kind: \"table\"（tableswitch）或 \"lookup\"（lookupswitch）。\n"
        "    default_uid: 缺省分支 label uid（既有节点）。\n"
        "    targets: 各分支 label uid 列表（既有节点）。\n"
        "    pos: 插入位置——None 尾插；给 uid 则插在该节点前。\n"
        "    min/max: kind=\"table\" 时的取值区间，min..max 逐个对应 targets\n"
        "        （len(targets) == max-min+1，结构性校验 fail-closed）。\n"
        "    keys: kind=\"lookup\" 的匹配键表，与 targets 成对且严格升序\n"
        "        （JVM 查找表序，乱序写出的类真机验证必炸——fail-closed）。\n"
        "Returns:\n"
        "    switch 节点自身的新 uid。\n"
        "Raises:\n"
        "    ValueError: kind 非法、targets 数目与区间不符或 keys 乱序。");
    m.def(
        "emit_label",
        [sess, memOf](const MemberRef& ref, std::optional<ca::u32> pos) {
            auto [h, cls, name, desc] = memOf(ref);
            return facadeEmitLabel(sess(h), cls, name, desc, pos);
        },
        py::arg("mref"), py::arg("pos") = std::nullopt, "建 label 节点（跳转/异常表的锚）。\n\n"
        "Args:\n"
        "    mref: 方法键。\n"
        "    pos: 插入位置——None 尾插；给 uid 则插在该节点前。\n"
        "Returns:\n"
        "    label uid。");
    m.def(
        "remove_insn",
        [sess, memOf](const MemberRef& ref, ca::u32 uid) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeRemoveInsn(sess(h), cls, name, desc, uid);
        },
        py::arg("mref"), py::arg("uid"), "删除节点（摘链进墓地；uid 后续解析 fail-closed）。\n\n"
        "Args:\n"
        "    mref: 方法键。\n"
        "    uid: 待删节点 uid。被删 uid 不复用；引用它的跳转目标由脚本负责\n"
        "    重绑。\n"
        "Raises:\n"
        "    StaleRefError: uid 不存在（含已删）。");

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
        py::arg("mref"), "列出异常表。\n\n"
        "Args:\n"
        "    mref: 方法键 (h, cls, name, desc)。\n"
        "Returns:\n"
        "    [(start_uid, end_uid, handler_uid, catch_type)] 行列表（catch_type\n"
        "    为空串表示 catch-all）。\n"
        "Raises:\n"
        "    StaleRefError: 方法不存在。");
    m.def(
        "add_try_catch",
        [sess, memOf](const MemberRef& ref, ca::u32 start_uid, ca::u32 end_uid,
                      ca::u32 handler_uid, const std::string& catch_type) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeAddTryCatch(sess(h), cls, name, desc, start_uid, end_uid, handler_uid,
                              catch_type);
        },
        py::arg("mref"), py::arg("start_uid"), py::arg("end_uid"), py::arg("handler_uid"),
        py::arg("catch_type"), "新增异常表行。\n\n"
        "Args:\n"
        "    mref: 方法键。\n"
        "    start_uid/end_uid/handler_uid: try 起点、终点（不含）与 handler\n"
        "        入口，三者均须为 label 节点 uid。\n"
        "    catch_type: 捕获异常类 internal name；空串 = catch-all。\n"
        "Raises:\n"
        "    StaleRefError: 任一 uid 不是 label 或不存在。");
    m.def(
        "remove_try_catch",
        [sess, memOf](const MemberRef& ref, ca::usize row_index) {
            auto [h, cls, name, desc] = memOf(ref);
            facadeRemoveTryCatch(sess(h), cls, name, desc, row_index);
        },
        py::arg("mref"), py::arg("row_index"), "按行号删除异常表行。\n\n"
        "Args:\n"
        "    mref: 方法键。\n"
        "    row_index: try_catches 序中的行号（行号随删行前移，批量删建议\n"
        "        降序进行）。\n"
        "Raises:\n"
        "    StaleRefError: 行号越界。");

    // ── 写回/落盘 ──
    m.def(
        "write_back",
        [sess, clsOf](const ClassRef& ref, bool compute_frames) {
            auto [h, name] = clsOf(ref);
            facadeWriteBack(sess(h), name, compute_frames);
        },
        py::arg("cref"), py::arg("compute_frames") = true,
        "单类写回：树 → 字节 + 清脏。\n\n"
        "Args:\n"
        "    cref: 类键 (h, 类名)。\n"
        "    compute_frames: 是否重算 StackMapTable（缺省 True；写不出即抛，\n"
        "        兼具结构/栈校验语义）。\n"
        "Raises:\n"
        "    RuntimeError: COMPUTE_FRAMES 写出失败（方法体结构/栈不一致）。\n"
        "    mjt.DeadlineExceeded: 协作层 deadline 越限。");
    m.def(
        "flush",
        [sess](ca::u64 h, bool compute_frames) {
            return facadeFlush(sess(h), compute_frames);
        },
        py::arg("h"), py::arg("compute_frames") = true, "全容器脏树写回。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    compute_frames: 是否重算 StackMapTable（缺省 True）。\n"
        "Returns:\n"
        "    写出的类名列表（升序）。\n"
        "Raises:\n"
        "    RuntimeError: 写出失败。\n"
        "    mjt.DeadlineExceeded: 协作层 deadline 越限。");
    m.def(
        "save",
        [sess](ca::u64 h, const std::string& path, bool compute_frames) {
            facadeSave(sess(h), path, compute_frames);
        },
        py::arg("h"), py::arg("path"), py::arg("compute_frames") = true,
        "落盘 jar：flush 全脏 + 两阶段写文件；未解析/未脏类字节原样导出。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    path: 输出 jar 路径。\n"
        "    compute_frames: flush 时是否重算 StackMapTable（缺省 True）。\n"
        "Raises:\n"
        "    RuntimeError: 写出失败。\n"
        "    mjt.DeadlineExceeded: 协作层 deadline 越限。");

    // ── rename token 面（单活动 token） ──
    m.def(
        "rename_begin", [sess](ca::u64 h) { facadeRenameBegin(sess(h)); }, py::arg("h"),
        "重置 rename 会话（收计划前调用；丢弃未提交计划）。\n\n"
        "Args:\n"
        "    h: 容器句柄。");
    m.def(
        "token_rename_class",
        [sess](ca::u64 h, const std::string& old_name, const std::string& new_name) {
            facadeTokenRenameClass(sess(h), old_name, new_name);
        },
        py::arg("h"), py::arg("old_name"), py::arg("new_name"),
        "类改名入计划（不立即生效，rename_commit 时带引用完整性整体应用）。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    old_name: 旧类名（点分或 internal 均可，内部规范化）。\n"
        "    new_name: 新类名（同上）。\n"
        "Raises:\n"
        "    RuntimeError: 未先 rename_begin，或改名无法落实。");
    m.def(
        "token_rename_method",
        [sess](ca::u64 h, const std::string& owner, const std::string& name,
               const std::string& desc, const std::string& new_name) {
            facadeTokenRenameMethod(sess(h), owner, name, desc, new_name);
        },
        py::arg("h"), py::arg("owner"), py::arg("name"), py::arg("desc"), py::arg("new_name"),
        "方法改名入计划（rename_commit 时连同全部调用点重写）。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    owner/name/desc: 目标方法坐标（owner 为 internal name）。\n"
        "    new_name: 新方法名。");
    m.def(
        "token_rename_field",
        [sess](ca::u64 h, const std::string& owner, const std::string& name,
               const std::string& desc, const std::string& new_name) {
            facadeTokenRenameField(sess(h), owner, name, desc, new_name);
        },
        py::arg("h"), py::arg("owner"), py::arg("name"), py::arg("desc"), py::arg("new_name"),
        "字段改名入计划（rename_commit 时连同全部引用点重写）。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    owner/name/desc: 目标字段坐标。\n"
        "    new_name: 新字段名。");
    m.def(
        "rename_commit",
        [sess](ca::u64 h, bool reflection_strings, bool annotations, bool signatures,
               bool resources) {
            facadeTokenCommit(sess(h), reflection_strings, annotations, signatures, resources);
        },
        py::arg("h"), py::arg("reflection_strings") = false, py::arg("annotations") = false,
        py::arg("signatures") = false, py::arg("resources") = true,
        "提交改名计划到容器（字节码引用重写恒开）。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    reflection_strings: 是否重写字符串形态的反射引用（Class.forName\n"
        "        等）。\n"
        "    annotations: 是否重写注解内的类型引用。\n"
        "    signatures: 是否重写泛型签名。\n"
        "    resources: 是否迁移条目 URI 并重写 MANIFEST/services（缺省开——\n"
        "        导出 jar 新类不该挂旧 URI）。\n\n"
        "提交前自动 flush 全部脏树（compute_frames=false，remap 保帧）；成功后\n"
        "全部缓存树作废。\n"
        "Raises:\n"
        "    RuntimeError: 提交失败。\n"
        "    mjt.DeadlineExceeded: 协作层 deadline 越限。");
    m.def(
        "mapping_dump",
        [sess](ca::u64 h, const std::string& path, const std::string& fmt) {
            facadeMappingDump(sess(h), path, fmt);
        },
        py::arg("h"), py::arg("path"), py::arg("fmt") = "Enigma",
        "导出改名映射 sidecar。\n\n"
        "Args:\n"
        "    h: 容器句柄（token 面或 rename_apply_suggestions 收进的映射）。\n"
        "    path: 输出文件路径。\n"
        "    fmt: Enigma / Tiny-V2 / Tiny-V1 / ProGuard / SRG / Simple / R8。\n"
        "Raises:\n"
        "    RuntimeError: 格式未知或写出失败。");
    m.def(
        "mapping_counts",
        [sess](ca::u64 h) {
            const auto [c, mm, f] = facadeMappingCounts(sess(h));
            return py::make_tuple(c, mm, f);
        },
        py::arg("h"), "映射条目计数。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "Returns:\n"
        "    (class, method, field) 计数三元组。");

    // ── 运行报告 ──
    m.def(
        "last_run_report",
        [sess](ca::u64 h) -> py::object {
            auto& session = sess(h);
            if (!session.lastStats) return py::none();
            return py::cast(*session.lastStats);
        },
        py::arg("h"), "取该容器最近一次管线运行报告。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "Returns:\n"
        "    RunStats；本会话尚未跑过管线时为 None。");

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
        "批量改名：读建议文件，引擎全量裁决并应用。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    path: 建议文件路径（v1 schema：proposals 列表，条目含 kind/name/\n"
        "        newName/confidence）。\n"
        "    policy: 改写范围——bytecode-only / bytecode-reflect /\n"
        "        bytecode-metadata / all。\n"
        "Returns:\n"
        "    (proposed, accepted, rejected, renamed_members, rejected_entries)；\n"
        "    rejected_entries = [{kind, owner, name, desc, new_name, source,\n"
        "    reason, detail}]（fail-closed 报错面：撞名条目自含 owner/old/new\n"
        "    与撞名对象定位）。\n\n"
        "成功后映射收进 rename token 面（mapping_dump/mapping_counts 直接\n"
        "可用），缓存树作废重建索引。");
    m.def(
        "shadow_generate",
        [](ca::u64 h) {
            const auto rep = facadeShadowGenerate(FacadeRegistry::instance().get(h));
            return py::make_tuple(rep.classes, rep.methods, rep.fields, rep.kept);
        },
        py::arg("h"),
        "机械占位名批量生成（幂等：已登记条目保留，覆盖会话 shadow 表）。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "Returns:\n"
        "    (classes, methods, fields, kept) 计数四元组；token 补号随行。\n"
        "Raises:\n"
        "    RuntimeError: 生成失败。");
    m.def(
        "shadow_load",
        [](ca::u64 h, const std::string& path) {
            return facadeShadowLoadFile(FacadeRegistry::instance().get(h), path);
        },
        py::arg("h"), py::arg("path"), "载入 shadow sidecar（严格 schema，整表替换会话表）。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    path: sidecar 文件路径。\n"
        "Returns:\n"
        "    条目数。\n"
        "Raises:\n"
        "    RuntimeError: 文件缺失或 schema 不符。");
    m.def(
        "shadow_dump",
        [](ca::u64 h, const std::string& path) {
            facadeShadowDumpFile(FacadeRegistry::instance().get(h), path);
        },
        py::arg("h"), py::arg("path"), "会话 shadow 表写出 sidecar（v2 JSON，token 列随行）。");
    m.def(
        "shadow_entries",
        [](ca::u64 h) {
            py::list out;
            for (const auto& [kind, owner, desc, raw, shadow, token] :
                 facadeShadowEntries(FacadeRegistry::instance().get(h)))
                out.append(py::make_tuple(kind, owner, desc, raw, shadow, token));
            return out;
        },
        py::arg("h"), "列出会话 shadow 表全条目。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "Returns:\n"
        "    [(kind, owner, desc, raw, shadow, token8)] 列表；kind ∈\n"
        "    class/field/method；空表（未生成/未载入）为空列表。");

    // ── Jasm 文本面（libjhasm）──
    m.def(
        "class_jasm",
        [](ca::u64 h, const std::string& name) {
            return jasmClassTextToPy(facadeClassJasm(FacadeRegistry::instance().get(h), name));
        },
        py::arg("h"), py::arg("name"), "类字节反汇编为规范化 Jasm 全文。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    name: internal name。\n"
        "Returns:\n"
        "    {internal_name, source}（source 为 LF 规范化全文）。读的是容器\n"
        "    落账字节——树编辑后须先 flush/write_back 再取，否则读到旧字节。\n"
        "Raises:\n"
        "    StaleRefError: 类名不存在。\n"
        "    RuntimeError: 类字节 malformed 无法反汇编。");
    m.def(
        "method_jasm",
        [](ca::u64 h, const std::string& cls, const std::string& name,
           const std::string& desc) {
            return jasmMethodTextToPy(
                facadeMethodJasm(FacadeRegistry::instance().get(h), cls, name, desc));
        },
        py::arg("h"), py::arg("cls"), py::arg("name"), py::arg("desc"),
        "单方法反汇编为 Jasm 声明段。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    cls/name/desc: 方法坐标。\n"
        "Returns:\n"
        "    {internal_name, name, descriptor, source}（source 为 .method 声明，\n"
        "    与类全文内方法段逐字节一致；读落账字节，树编辑后先 flush）。\n"
        "Raises:\n"
        "    StaleRefError: 类或方法不存在。");
    m.def(
        "class_validate_jasm",
        [](ca::u64 h, const std::string& name, const std::string& source,
           const std::string& source_name, const std::string& validation) {
            auto& session = FacadeRegistry::instance().get(h);
            return jasmReportToPy(
                facadeValidateJasm(session, name, source_name, source, validation));
        },
        py::arg("h"), py::arg("name"), py::arg("source"),
        py::arg("source_name") = "",
        py::arg("validation") = "relaxed",
        "试汇编完整 Jasm（容器深拷贝上验证，绝不落容器）。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    name: 目标类 internal name（与 source 声明名一致才通过）。\n"
        "    source: 完整 Jasm 文本。\n"
        "    source_name: 诊断定位用的源名；缺省 <name>.jasm。\n"
        "    validation: 校验档位 \"relaxed\"（默认，JVM 赋值兼容语义）/ \"strict\"\n"
        "        （精确类型匹配）/ \"off\"（跳过校验门，含重建后 validate）；非法值\n"
        "        抛 ValueError。\n"
        "Returns:\n"
        "    报告 dict：{ok, internal_name, written, diagnostics}；diagnostics\n"
        "    条目为 {severity, stage, stage_name, gate, message, line, column}\n"
        "    （severity 0=error 1=warning；gate 区分编辑门 edit 与重建管线\n"
        "    rebuild）。内容性错误走 diagnostics 不抛异常。\n"
        "Raises:\n"
        "    StaleRefError: 类名不存在。\n"
        "    RuntimeError: 现类字节反汇编失败。");
    m.def(
        "class_apply_jasm",
        [](ca::u64 h, const std::string& name, const std::string& source,
           const std::string& source_name, const std::string& validation) {
            auto& session = FacadeRegistry::instance().get(h);
            return jasmReportToPy(
                facadeApplyJasm(session, name, source_name, source, validation));
        },
        py::arg("h"), py::arg("name"), py::arg("source"), py::arg("source_name") = "",
        py::arg("validation") = "relaxed",
        "事务式汇编替换类字节（名称校验 + 候选工作区复验全过才落）。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    name: 目标类 internal name。\n"
        "    source: 完整 Jasm 文本。\n"
        "    source_name: 诊断定位用的源名；缺省 <name>.jasm。\n"
        "    validation: 校验档位（同 class_validate_jasm）。\n"
        "Returns:\n"
        "    报告 dict（同 class_validate_jasm；成功时 written 为新类字节数），\n"
        "    成功后该类缓存树作废。\n"
        "Raises:\n"
        "    StaleRefError: 类名不存在。\n"
        "    RuntimeError: 现类字节反汇编失败或事务落容器失败。");
    m.def(
        "class_validate_jasm_diff",
        [](ca::u64 h, const std::string& name, const std::string& diff,
           const std::string& validation) {
            auto& session = FacadeRegistry::instance().get(h);
            return jasmReportToPy(facadeValidateJasmDiff(session, name, diff, validation));
        },
        py::arg("h"), py::arg("name"), py::arg("diff"), py::arg("validation") = "relaxed",
        "diff 试汇编：对 class_jasm 当前全文应用 unified diff 后在容器拷贝上\n"
        "验证，绝不落容器。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    name: 目标类 internal name。\n"
        "    diff: 对当前规范全文的 unified diff。\n"
        "    validation: 校验档位（同 class_validate_jasm）。\n"
        "Returns:\n"
        "    报告 dict（同 class_validate_jasm）；上下文失配在 diagnostics 里\n"
        "    给 hunk 序号。\n"
        "Raises:\n"
        "    StaleRefError: 类名不存在。\n"
        "    RuntimeError: 现类字节反汇编失败。");
    m.def(
        "class_apply_jasm_diff",
        [](ca::u64 h, const std::string& name, const std::string& diff,
           const std::string& validation) {
            auto& session = FacadeRegistry::instance().get(h);
            return jasmReportToPy(facadeApplyJasmDiff(session, name, diff, validation));
        },
        py::arg("h"), py::arg("name"), py::arg("diff"), py::arg("validation") = "relaxed",
        "diff 应用后事务式汇编替换类字节。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    name: 目标类 internal name。\n"
        "    diff: 对当前规范全文的 unified diff。\n"
        "    validation: 校验档位（同 class_validate_jasm）。\n"
        "Returns:\n"
        "    报告 dict（同 class_apply_jasm），成功后该类缓存树作废。\n"
        "Raises:\n"
        "    StaleRefError: 类名不存在。\n"
        "    RuntimeError: 现类字节反汇编失败或事务落容器失败。");
    m.def(
        "method_validate_jasm",
        [](ca::u64 h, const std::string& cls, const std::string& name,
           const std::string& desc, const std::string& source,
           const std::string& source_name, const std::string& validation) {
            auto& session = FacadeRegistry::instance().get(h);
            return jasmReportToPy(facadeValidateMethodJasm(session, cls, name, desc,
                                                           source_name, source, validation));
        },
        py::arg("h"), py::arg("cls"), py::arg("name"), py::arg("desc"), py::arg("source"),
        py::arg("source_name") = "",
        py::arg("validation") = "relaxed",
        "方法级 Jasm 全量替换试汇编（source 拼接回类全文后在容器拷贝上验证）。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    cls/name/desc: 目标方法坐标。\n"
        "    source: 完整 .method 声明文本。\n"
        "    source_name: 诊断定位用的源名；缺省 <name>.jasm。\n"
        "    validation: 校验档位（同 class_validate_jasm）。\n"
        "Returns:\n"
        "    报告 dict（同 class_validate_jasm）。\n"
        "Raises:\n"
        "    StaleRefError: 类或方法不存在。\n"
        "    RuntimeError: 现类字节反汇编失败。");
    m.def(
        "method_apply_jasm",
        [](ca::u64 h, const std::string& cls, const std::string& name,
           const std::string& desc, const std::string& source,
           const std::string& source_name, const std::string& validation) {
            auto& session = FacadeRegistry::instance().get(h);
            return jasmReportToPy(facadeApplyMethodJasm(session, cls, name, desc,
                                                        source_name, source, validation));
        },
        py::arg("h"), py::arg("cls"), py::arg("name"), py::arg("desc"), py::arg("source"),
        py::arg("source_name") = "",
        py::arg("validation") = "relaxed",
        "方法级 Jasm 全量替换（source 拼接回类全文后事务式汇编落容器）。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    cls/name/desc: 目标方法坐标。\n"
        "    source: 完整 .method 声明文本。\n"
        "    source_name: 诊断定位用的源名；缺省 <name>.jasm。\n"
        "    validation: 校验档位（同 class_validate_jasm）。\n"
        "Returns:\n"
        "    报告 dict（同 class_apply_jasm），成功后该类缓存树作废。\n"
        "Raises:\n"
        "    StaleRefError: 类或方法不存在。\n"
        "    RuntimeError: 现类字节反汇编失败或事务落容器失败。");
    m.def(
        "method_validate_jasm_diff",
        [](ca::u64 h, const std::string& cls, const std::string& name,
           const std::string& desc, const std::string& diff,
           const std::string& validation) {
            auto& session = FacadeRegistry::instance().get(h);
            return jasmReportToPy(
                facadeValidateMethodJasmDiff(session, cls, name, desc, diff, validation));
        },
        py::arg("h"), py::arg("cls"), py::arg("name"), py::arg("desc"), py::arg("diff"),
        py::arg("validation") = "relaxed",
        "方法级 diff 试汇编：diff 相对 method_jasm 产物应用，拼接回类全文后\n"
        "在容器拷贝上验证，绝不落容器。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    cls/name/desc: 目标方法坐标。\n"
        "    diff: 对 method_jasm 产物的 unified diff。\n"
        "    validation: 校验档位（同 class_validate_jasm）。\n"
        "Returns:\n"
        "    报告 dict（同 class_validate_jasm）。\n"
        "Raises:\n"
        "    StaleRefError: 类或方法不存在。\n"
        "    RuntimeError: 现类字节反汇编失败。");
    m.def(
        "method_apply_jasm_diff",
        [](ca::u64 h, const std::string& cls, const std::string& name,
           const std::string& desc, const std::string& diff,
           const std::string& validation) {
            auto& session = FacadeRegistry::instance().get(h);
            return jasmReportToPy(
                facadeApplyMethodJasmDiff(session, cls, name, desc, diff, validation));
        },
        py::arg("h"), py::arg("cls"), py::arg("name"), py::arg("desc"), py::arg("diff"),
        py::arg("validation") = "relaxed",
        "方法级 diff 应用：diff 相对 method_jasm 产物应用，拼接回类全文后\n"
        "事务式汇编落容器。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    cls/name/desc: 目标方法坐标。\n"
        "    diff: 对 method_jasm 产物的 unified diff。\n"
        "    validation: 校验档位（同 class_validate_jasm）。\n"
        "Returns:\n"
        "    报告 dict（同 class_apply_jasm），成功后该类缓存树作废。\n"
        "Raises:\n"
        "    StaleRefError: 类或方法不存在。\n"
        "    RuntimeError: 现类字节反汇编失败或事务落容器失败。");

    // ── jar 差异 / 修复 / 反编译对比 / shadow 文件面 ──
    m.def(
        "jar_diff",
        [](ca::u64 h, const std::string& path) {
            auto&       session = FacadeRegistry::instance().get(h);
            const auto  report  = facadeJarDiff(session, path);
            py::dict    out;
            out["reference_path"] = report.referencePath;
            out["workspace_only"] = report.workspaceOnly;
            out["reference_only"] = report.referenceOnly;
            out["modified"]       = report.modified;
            py::list diffs;
            for (const auto& diff : report.diffs) {
                py::list ranges;
                for (const auto& range : diff.ranges) {
                    py::dict rangeOut;
                    rangeOut["offset"]        = range.offset;
                    rangeOut["before_length"] = range.beforeLength;
                    rangeOut["after_length"]  = range.afterLength;
                    ranges.append(rangeOut);
                }
                py::dict diffOut;
                diffOut["uri"]         = diff.uri;
                diffOut["before_size"] = diff.beforeSize;
                diffOut["after_size"]  = diff.afterSize;
                diffOut["ranges"]      = ranges;
                diffs.append(diffOut);
            }
            out["diffs"] = diffs;
            return out;
        },
        py::arg("h"), py::arg("path"),
        "会话容器与外部参考 JAR 的全资源字节比较。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    path: 参考 jar 路径。\n"
        "Returns:\n"
        "    报告 dict：{reference_path, workspace_only, reference_only,\n"
        "    modified, diffs}；计数口径——workspace_only 仅会话容器有、\n"
        "    reference_only 仅参考有、modified 两侧都有但字节不同；diffs 按\n"
        "    uri 升序，逐条目 {uri, before_size, after_size, ranges}，range 为\n"
        "    单公共前后缀收缩 [{offset, before_length, after_length}]。\n"
        "Raises:\n"
        "    RuntimeError: 参考 jar 缺失或导入失败。");
    m.def(
        "jar_fix_analyze",
        [](ca::u64 h, bool remove_invalid_class_entries, bool remove_corrupt_output_markers) {
            auto& session = FacadeRegistry::instance().get(h);
            return jarRepairToPy(facadeJarRepairAnalyze(session,
                                                        remove_invalid_class_entries,
                                                        remove_corrupt_output_markers));
        },
        py::arg("h"), py::arg("remove_invalid_class_entries") = false,
        py::arg("remove_corrupt_output_markers") = false,
        "jar 修复只读扫描。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    remove_invalid_class_entries: 是否选中无效类条目（.class 扩展名\n"
        "        但类字节无效）。\n"
        "    remove_corrupt_output_markers: 是否选中损坏输出标记条目（uri 含\n"
        "        corrupt_output_stream 标记）。\n"
        "Returns:\n"
        "    报告 dict：{resource_count, class_count, issues, removed}；issue\n"
        "    为 {uri, kind, message, size, selected}（kind ∈ invalid_class /\n"
        "    invalid_class_entry / corrupt_output_marker），selected 按开关回填。\n"
        "Raises:\n"
        "    StaleRefError: 句柄失效。");
    m.def(
        "jar_fix_apply",
        [](ca::u64 h, bool remove_invalid_class_entries, bool remove_corrupt_output_markers) {
            auto& session = FacadeRegistry::instance().get(h);
            return jarRepairToPy(facadeJarRepairApply(session,
                                                      remove_invalid_class_entries,
                                                      remove_corrupt_output_markers));
        },
        py::arg("h"), py::arg("remove_invalid_class_entries") = false,
        py::arg("remove_corrupt_output_markers") = false,
        "jar 修复执行：同 jar_fix_analyze 扫描后移除全部选中条目。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    remove_invalid_class_entries / remove_corrupt_output_markers:\n"
        "        同 jar_fix_analyze 的选中开关。\n"
        "Returns:\n"
        "    报告 dict（同 jar_fix_analyze；removed 列出实际移除条目，索引与\n"
        "    树缓存同步清理）。\n"
        "Raises:\n"
        "    StaleRefError: 句柄失效。");
    m.def(
        "decompiler_compare",
        [](ca::u64 h, const std::string& name, const std::string& jar,
           const std::string& vineflower_jar, const std::string& java,
           std::int64_t timeout_ms) {
            auto&       session = FacadeRegistry::instance().get(h);
            const auto  report =
                facadeDecompilerCompare(session, name, jar, vineflower_jar, java, timeout_ms);
            py::dict out;
            out["internal_name"] = report.internalName;
            py::list sources;
            for (const auto& source : report.sources) {
                py::dict entry;
                entry["internal_name"]  = source.internalName;
                entry["backend"]        = source.backend;
                entry["decompiler_jar"] = source.decompilerJar;
                entry["source_scope"]   = source.sourceScope;
                entry["source"]         = source.source;
                sources.append(entry);
            }
            out["sources"] = sources;
            return out;
        },
        py::arg("h"), py::arg("name"), py::arg("jar") = "", py::arg("vineflower_jar") = "",
        py::arg("java") = "", py::arg("timeout_ms") = 0,
        "CFR 与 Vineflower 双后端反编译同名类，双源并列返回。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    name: 目标类 internal name。\n"
        "    jar: CFR 反编译器 jar 路径（空串走环境变量发现）。\n"
        "    vineflower_jar: Vineflower jar 路径（空串走环境变量发现）。\n"
        "    java: java 可执行程序路径（空串走 PATH）。\n"
        "    timeout_ms: 单后端超时毫秒；0 用缺省上限。\n"
        "Returns:\n"
        "    {internal_name, sources: [{internal_name, backend, decompiler_jar,\n"
        "    source_scope, source}]}（backends 依序 cfr/vineflower）。\n"
        "Raises:\n"
        "    RuntimeError: 后端不可用（jar/java 发现失败）——fail-closed。\n"
        "    StaleRefError: 类名不存在。");
    m.def(
        "shadow_generate_file",
        [](const std::string& path, const std::string& file) {
            const auto report = facadeShadowGenerateFile(path, file);
            py::dict   out;
            out["file"]             = report.file;
            out["assigned_classes"] = report.assignedClasses;
            out["assigned_methods"] = report.assignedMethods;
            out["assigned_fields"]  = report.assignedFields;
            out["kept_entries"]     = report.keptEntries;
            out["removed"]          = report.removed;
            out["total_entries"]    = report.totalEntries;
            return out;
        },
        py::arg("path"), py::arg("file") = "",
        "shadow sidecar 生成（文件面，不依赖打开容器）。\n\n"
        "Args:\n"
        "    path: jar 路径。\n"
        "    file: sidecar 输出路径；缺省 `<jar>.mjt-shadow`，显式给出时不得\n"
        "        指向 jar 本身。\n"
        "Returns:\n"
        "    报告 dict：{file, assigned_classes, assigned_methods,\n"
        "    assigned_fields, kept_entries, removed, total_entries}；幂等回填——\n"
        "    已登记条目保留，只写 sidecar 永不改 jar。\n"
        "Raises:\n"
        "    RuntimeError: jar 缺失、file 指向 jar 本身或写出失败。");
    m.def(
        "shadow_prune_file",
        [](const std::string& path, const std::string& file) {
            const auto report = facadeShadowPruneFile(path, file);
            py::dict   out;
            out["file"]          = report.file;
            out["removed"]       = report.removed;
            out["total_entries"] = report.totalEntries;
            return out;
        },
        py::arg("path"), py::arg("file") = "",
        "shadow sidecar 清理（文件面）：剔除 jar 中已不存在的条目。\n\n"
        "Args:\n"
        "    path: jar 路径。\n"
        "    file: sidecar 路径；缺省 `<jar>.mjt-shadow`。\n"
        "Returns:\n"
        "    报告 dict：{file, removed, total_entries}；removed == 0 不回写\n"
        "    文件。\n"
        "Raises:\n"
        "    RuntimeError: sidecar 缺失或 jar 缺失。");
}

}   // namespace mjt::pysdk
