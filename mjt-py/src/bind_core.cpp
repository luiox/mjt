#include <pybind11/eval.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/job_context.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/pipeline_builder.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"
#include "mjbase/workspace/workspace.hpp"

#include "jbytecode/jbytecode.hpp"

#include "pysdk_bridge.hpp"
#include "pysdk_facade.hpp"
#include "pysdk_jar.hpp"

#include <cstdio>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace py = pybind11;

using jbytecode::core::Opcodes;

namespace mjt::pysdk {
namespace {

// ═══════════════════════════════════════════════════════════════════════
// MethodPass 类型锚（Python 侧继承面）。
//
// mbase 时代的 PyMethodPassAdapter（python pass → 管线槽位）属私有侧管线
// 消费面，不在开源基座实现：mjbase 的管线载体是 libjbc CodeUnit/ClassSession
// （不是 ASM 树），适配器实现随私有仓库管线桥落地。这里保留脚本面形状：
// MethodPass 基类 + changed()/preserved_all() 返回锚，让既有脚本 import 不炸。
// ═══════════════════════════════════════════════════════════════════════

/// PreservedAnalyses 传递锚（基座自持值：changed 布尔；私有侧适配器负责
/// 换算成自家管线的保留集合）。
struct PyPreserved
{
    bool changed = false;
};

/// python 侧继承的基类（无 C++ 行为，仅作类型锚）。
struct PyMethodPassBase
{
    virtual ~PyMethodPassBase() = default;
};

// ═══════════════════════════════════════════════════════════════════════
// 统计装饰器（mbase PassRunStats 语义的 pysdk 侧等位物；mjbase 装配层未随迁
// 计数装饰器，见其 pipeline_builder.hpp 注记——按同款形态在消费方补）。
//
// 槽位语义对齐 mbase Slot：name/scope（"method"/"class"/"global"）、processed
// = 单元访问数、changed = PreservedAnalyses.changed() 计数、failed = 异常数、
// durationMs = 累计墙钟。构造即登记槽（processed=0 的 pass 也有槽可读）。
// ═══════════════════════════════════════════════════════════════════════

template <class UnitT, class AnalysisManagerT>
class PyStatsPass final : public mjbase::spi::PassModel<UnitT, AnalysisManagerT>
{
public:
    PyStatsPass(std::unique_ptr<mjbase::spi::PassModel<UnitT, AnalysisManagerT>> inner,
                std::string name, std::string scope, PyRunStats& out)
        : inner_(std::move(inner))
        , name_(std::move(name))
        , scope_(std::move(scope))
        , out_(out)
    {
        slotFor();   // 构造即登记槽位（零访问 pass 的 processed=0 可读）
    }

    mjbase::spi::PreservedAnalyses run(UnitT& unit, AnalysisManagerT& am) override
    {
        auto&       slot = slotFor();
        const auto  t0   = std::chrono::steady_clock::now();
        ++slot.processed;
        mjbase::spi::PreservedAnalyses pa;
        try {
            pa = inner_->run(unit, am);
        }
        catch (...) {
            ++slot.failed;
            slot.durationMs += elapsedMs(t0);
            throw;
        }
        if (pa.changed()) ++slot.changed;
        slot.durationMs += elapsedMs(t0);
        return pa;
    }

private:
    static double elapsedMs(const std::chrono::steady_clock::time_point& t0)
    {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()
                                                         - t0)
            .count();
    }

    /// 槽位按 (name, scope) 线性查建（pass 数少，O(段数) 可忽略；避免持指针
    /// 跨装饰器间 vector 扩容悬垂）。
    PyRunStats::Slot& slotFor()
    {
        for (auto& s : out_.slots) {
            if (s.name == name_ && s.scope == scope_) return s;
        }
        PyRunStats::Slot s;
        s.name  = name_;
        s.scope = scope_;
        out_.slots.push_back(std::move(s));
        return out_.slots.back();
    }

    std::unique_ptr<mjbase::spi::PassModel<UnitT, AnalysisManagerT>> inner_;
    std::string name_;
    std::string scope_;
    PyRunStats& out_;
};

// ── run_pass 实现（mjbase spi 命名 pass 注册表 + 装配层，纯基座面） ────────

std::shared_ptr<PyRunStats> runPass(ca::u64 h, const std::string& name,
                                    const std::string& jdkPath)
{
    auto& session = FacadeRegistry::instance().get(h);
    // 默认注册表首次访问自种子（无需显式注册步骤）。
    auto&       registry = mjbase::spi::defaultPassRegistry();
    const auto  found    = registry.find(name);
    if (found.is_err()) {
        throw py::value_error("unknown pass '" + name + "'");
    }

    auto stats            = std::make_shared<PyRunStats>();
    stats->pipeline       = name;

    mjbase::spi::PassJobContext     job;
    mjbase::spi::PassPipelineOptions options;

    // jdk 扫描 → 层次图 → 公共父类求解 → Strict 帧重算注入（resolver 数据
    // 获取是调用方的事，管线只消费已建好的图；对象存活须覆盖 build+run）。
    std::unique_ptr<mjbase::KeyPool>                        jdkPool;
    std::unique_ptr<mjbase::hierarchy::ClassHierarchy>      jdkHierarchy;
    std::unique_ptr<mjbase::spi::CommonSuperClassResolver>  superResolver;
    std::unique_ptr<mjbase::spi::StrictTypeResolverAdapter> typeResolver;
    if (!jdkPath.empty()) {
        jdkPool      = std::make_unique<mjbase::KeyPool>();
        jdkHierarchy = std::make_unique<mjbase::hierarchy::ClassHierarchy>(*jdkPool);
        mjbase::workspace::scanJdkHierarchy(jdkPath, *jdkHierarchy, *jdkPool);
        superResolver = std::make_unique<mjbase::spi::CommonSuperClassResolver>(*jdkHierarchy);
        typeResolver  = std::make_unique<mjbase::spi::StrictTypeResolverAdapter>(*superResolver);
        options.typeResolver = typeResolver.get();
    }

    // 单 pass 装配：按注册 scope 投入对应段位，统计装饰器包在 pass 外层
    // （factory 每次 build 重新 create——与装配层「实例不跨构建共享」纪律一致）。
    mjbase::spi::PipelineBuilder builder;
    switch (found.unwrap().scope) {
    case mjbase::spi::PassScope::Method:
        builder.addMethodPass(
            [&registry, name, &stats]() -> std::unique_ptr<mjbase::spi::MethodPass> {
                auto pass = registry.createMethodPass(name);
                if (pass.is_err()) throw std::runtime_error("pass create failed: " + name);
                return std::make_unique<
                    PyStatsPass<mjbase::spi::MethodScope, mjbase::spi::MethodAnalysisManager>>(
                    std::move(pass).unwrap(), name, "method", *stats);
            },
            name);
        break;
    case mjbase::spi::PassScope::Class:
        builder.addClassPass(
            [&registry, name, &stats]() -> std::unique_ptr<mjbase::spi::ClassPass> {
                auto pass = registry.createClassPass(name);
                if (pass.is_err()) throw std::runtime_error("pass create failed: " + name);
                return std::make_unique<
                    PyStatsPass<jbc::ir::ClassSession, mjbase::spi::ClassAnalysisManager>>(
                    std::move(pass).unwrap(), name, "class", *stats);
            },
            name);
        break;
    case mjbase::spi::PassScope::Global:
        builder.addGlobalPass(
            [&registry, name, &stats]() -> std::unique_ptr<mjbase::spi::GlobalPass> {
                auto pass = registry.createGlobalPass(name);
                if (pass.is_err()) throw std::runtime_error("pass create failed: " + name);
                return std::make_unique<
                    PyStatsPass<mjbase::model::ResourceContainer,
                                mjbase::spi::GlobalAnalysisManager>>(
                    std::move(pass).unwrap(), name, "global", *stats);
            },
            name);
        break;
    default:
        // Cf 段未接入容器管线（装配层同口径：fail loud，不静默跳过）。
        throw py::value_error("pass '" + name + "' (cf scope) is not runnable via run_pass");
    }

    // 非 const：runContainerPipeline 收 GlobalPassManager&（非 const 左值）。
    auto gpm = builder.buildNestedPipeline(job, nullptr, nullptr, options);
    const auto ran = mjbase::spi::runContainerPipeline(*session.container, gpm, job);
    if (ran.is_err()) {
        const auto& err = ran.unwrap_err();
        throw std::runtime_error("pass run failed: "
                                 + std::string(mjbase::spi::to_string(err.stage)) + " "
                                 + err.target + ": " + err.detail);
    }

    session.clearTrees();   // 管线直驱容器字节 → 树缓存作废（lazy 重解析）
    session.lastStats    = stats;
    session.lastPipeline = name;
    return stats;
}

// ═══════════════════════════════════════════════════════════════════════
// util.py 装载（mjt.util；纯 Python 二次封装层）
// ═══════════════════════════════════════════════════════════════════════

void loadUtilModule(py::module_& m)
{
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
    try {
        candidates.push_back(fs::path(py::str(m.attr("__file__")).cast<std::string>()).parent_path() / "mjt_util.py");
    }
    catch (...) {
    }
    if (const char* env = std::getenv("MJT_UTIL_DIR")) {
        candidates.push_back(fs::path(env) / "mjt_util.py");
    }
#ifdef _WIN32
    char exeBuf[MAX_PATH] = {0};
    if (GetModuleFileNameA(nullptr, exeBuf, MAX_PATH) > 0) {
        candidates.push_back(fs::path(exeBuf).parent_path() / "mjt_util.py");
    }
#endif
    for (const auto& p : candidates) {
        std::error_code ec;
        if (!fs::exists(p, ec)) continue;
        try {
            py::object types = py::module_::import("types");
            py::object mod   = types.attr("ModuleType")("mjt.util");
            py::dict scope   = mod.attr("__dict__");
            scope["__name__"] = "mjt.util";
            scope["__file__"] = p.u8string();
            scope["mjt"]      = m;   // util 不得 import mjt（init 期间 sys.modules 未注册）
            py::module_::import("sys").attr("modules")["mjt.util"] = mod;
            py::eval_file(p.u8string(), scope);
            m.attr("util") = mod;
            return;
        }
        catch (const py::error_already_set& e) {
            py::module_::import("sys").attr("stderr").attr("write")(
                std::string("mjt: failed to load util layer: ") + e.what() + "\n");
            return;
        }
    }
}

}   // namespace

PYBIND11_MODULE(mjt, m)
{
    m.doc() = "mjt-py SDK v3: functional face over mjbase core (Ref=key)";

    // ── RunStats（L5 报告等位物） ──
    py::class_<PyRunStats>(m, "RunStats")
        .def_property_readonly("pipeline", [](PyRunStats& s) { return s.pipeline; })
        .def_property_readonly("slots", [](PyRunStats& s) {
            py::list out;
            for (const auto& slot : s.slots) {
                py::dict d;
                d["name"]        = slot.name;
                d["scope"]       = slot.scope;
                d["processed"]   = slot.processed;
                d["changed"]     = slot.changed;
                d["failed"]      = slot.failed;
                d["duration_ms"] = slot.durationMs;
                out.append(d);
            }
            return out;
        })
        .def("__repr__", [](PyRunStats& s) {
            return "<RunStats " + s.pipeline + ": " + std::to_string(s.slots.size()) + " slots>";
        });

    // ── 管线 / 单 pass 面 ──
    m.def("run_pass", &runPass, py::arg("h"), py::arg("name"), py::arg("jdk") = "",
          "按名跑 mjbase 内置单 pass（NopRemover/ClassMemberSorter/ClassRenamer/...）");
    m.def("passes", [] {
        std::vector<std::string> out;
        for (const auto& info : mjbase::spi::defaultPassRegistry().list())
            out.push_back(std::string(info.name));
        return out;
    });

    // ── 自定义 pass 类型锚（适配器实现属私有侧管线消费面，见文件头注记） ──
    py::class_<PyMethodPassBase, std::shared_ptr<PyMethodPassBase>>(m, "MethodPass")
        .def(py::init<>(), "子类化并实现 run(mref) -> mjt.changed()/mjt.preserved_all()/bool/None");
    // MethodPass.run 返回锚（不透明：Python 侧只从 changed()/preserved_all() 取）。
    py::class_<PyPreserved>(m, "PreservedAnalyses")
        .def("__repr__", [](const PyPreserved& p) {
            return p.changed ? "PreservedAnalyses.none" : "PreservedAnalyses.all";
        });
    m.def("changed", [] { return PyPreserved{true}; });
    m.def("preserved_all", [] { return PyPreserved{false}; });

    // ── 合成样本 ──
    m.def("write_demo_jar", &writeSyntheticDemoJar, py::arg("path"),
          "合成混淆形态演示 jar（unittest/上手演示用；零样本字节入库）");

    // ── v3 平函数面（契约 v3 §2；主面）──
    bindFacade(m);

    // ── 常量面：mjt.opcodes（JVM 规范序助记名 → opcode 值）+ mjt.acc（访问标志）。
    {
        py::dict op;
        for (int i = 0; i <= 201; ++i) op[facadeOpcodeName(i)] = i;
        m.attr("opcodes") = op;
        py::dict acc;
        acc["PUBLIC"]    = Opcodes::ACC_PUBLIC;
        acc["PRIVATE"]   = Opcodes::ACC_PRIVATE;
        acc["PROTECTED"] = Opcodes::ACC_PROTECTED;
        acc["STATIC"]    = Opcodes::ACC_STATIC;
        acc["FINAL"]     = Opcodes::ACC_FINAL;
        acc["ABSTRACT"]  = Opcodes::ACC_ABSTRACT;
        acc["NATIVE"]    = Opcodes::ACC_NATIVE;
        acc["INTERFACE"] = Opcodes::ACC_INTERFACE;
        m.attr("acc")    = acc;
    }
    loadUtilModule(m);
}

}   // namespace mjt::pysdk
