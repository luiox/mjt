#include <pybind11/eval.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/expr/expr_eval.hpp"
#include "mjbase/expr/expr_parser.hpp"
#include "mjbase/expr/expr_print.hpp"
#include "mjbase/expr/expr_simplify.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/job_context.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/pass_registry.hpp"
#include "mjbase/spi/pipeline_builder.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"
#include "mjbase/workspace/workspace.hpp"

#include "jbytecode/jbytecode.hpp"

#include "pysdk_bridge.hpp"
#include "pysdk_facade.hpp"
#include "pysdk_jar.hpp"
#include "pysdk_limits.hpp"

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

// ── run_pass / run_pipeline 实现（mjbase spi 命名 pass 注册表 + 装配层） ──

/// 按名装配单 pass 到 builder（scope 投段 + 统计装饰器外层包裹）。
/// 工厂每次 build 重新 create——与装配层「实例不跨构建共享」纪律一致。
void addNamedPass(mjbase::spi::PipelineBuilder& builder, const std::string& name,
                  PyRunStats& stats)
{
    auto&      registry = mjbase::spi::defaultPassRegistry();
    const auto found    = registry.find(name);
    if (found.is_err()) {
        throw py::value_error("unknown pass '" + name + "'");
    }
    switch (found.unwrap().scope) {
    case mjbase::spi::PassScope::Method:
        builder.addMethodPass(
            [&registry, name, &stats]() -> std::unique_ptr<mjbase::spi::MethodPass> {
                auto pass = registry.createMethodPass(name);
                if (pass.is_err()) throw std::runtime_error("pass create failed: " + name);
                return std::make_unique<
                    PyStatsPass<mjbase::spi::MethodScope, mjbase::spi::MethodAnalysisManager>>(
                    std::move(pass).unwrap(), name, "method", stats);
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
                    std::move(pass).unwrap(), name, "class", stats);
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
                    std::move(pass).unwrap(), name, "global", stats);
            },
            name);
        break;
    default:
        // Cf 段未接入容器管线（装配层同口径：fail loud，不静默跳过）。
        throw py::value_error("pass '" + name + "' (cf scope) is not runnable via run_pass");
    }
}

/// 选择器解析（workspace applyPipeline 同口径）：pipeline 与 pass_names 互斥；
/// 两者均空时 allowFallback 才回落 default-optimize（opt_* 语义），否则报错。
void assembleSelection(mjbase::spi::PipelineBuilder&   builder,
                       const std::vector<std::string>& passNames,
                       const std::string&              pipeline,
                       bool                            allowFallback,
                       PyRunStats&                     stats)
{
    if (!pipeline.empty() && !passNames.empty()) {
        throw py::value_error("pipeline and pass_names are mutually exclusive");
    }
    mjbase::spi::registerBuiltinPipelines();
    if (!pipeline.empty()) {
        if (!mjbase::spi::PassRegistry::instance().hasPipeline(pipeline)) {
            throw py::value_error("unknown pipeline '" + pipeline + "'");
        }
        builder = mjbase::spi::PassRegistry::instance().getPipeline(pipeline);
        return;
    }
    if (!passNames.empty()) {
        for (const auto& name : passNames) addNamedPass(builder, name, stats);
        return;
    }
    if (!allowFallback) {
        throw py::value_error("exactly one of pass_names or pipeline is required");
    }
    builder = mjbase::spi::PassRegistry::instance().getPipeline("default-optimize");
}

/// jdk 扫描 → 层次图 → 公共父类求解 → Strict 帧重算注入（resolver 数据获取是
/// 调用方的事；对象存活须覆盖 build+run）。
struct JdkResolverChain
{
    std::unique_ptr<mjbase::KeyPool>                       pool;
    std::unique_ptr<mjbase::hierarchy::ClassHierarchy>     hierarchy;
    std::unique_ptr<mjbase::spi::CommonSuperClassResolver> superResolver;
    std::unique_ptr<mjbase::spi::StrictTypeResolverAdapter> typeResolver;

    JdkResolverChain(const std::string& jdkPath)
    {
        if (jdkPath.empty()) return;
        pool          = std::make_unique<mjbase::KeyPool>();
        hierarchy     = std::make_unique<mjbase::hierarchy::ClassHierarchy>(*pool);
        mjbase::workspace::scanJdkHierarchy(jdkPath, *hierarchy, *pool);
        superResolver = std::make_unique<mjbase::spi::CommonSuperClassResolver>(*hierarchy);
        typeResolver  = std::make_unique<mjbase::spi::StrictTypeResolverAdapter>(*superResolver);
    }
};

/// 管线在给定容器上执行（candidate 由调用方决定落会话还是试算副本）。
void runPipelineOnContainer(mjbase::model::ResourceContainer&       container,
                            const std::vector<std::string>&         passNames,
                            const std::string&                      pipeline,
                            bool                                    allowFallback,
                            const std::string&                      jdkPath,
                            PyRunStats&                             stats)
{
    deadlineCheck();
    mjbase::spi::PipelineBuilder      builder;
    assembleSelection(builder, passNames, pipeline, allowFallback, stats);
    mjbase::spi::PassJobContext      job;
    mjbase::spi::PassPipelineOptions options;
    JdkResolverChain                 resolver(jdkPath);
    if (resolver.typeResolver) options.typeResolver = resolver.typeResolver.get();

    // 非 const：runContainerPipeline 收 GlobalPassManager&（非 const 左值）。
    auto gpm = builder.buildNestedPipeline(job, nullptr, nullptr, options);
    const auto ran = mjbase::spi::runContainerPipeline(container, gpm, job);
    if (ran.is_err()) {
        const auto& err = ran.unwrap_err();
        throw std::runtime_error("pass run failed: "
                                 + std::string(mjbase::spi::to_string(err.stage)) + " "
                                 + err.target + ": " + err.detail);
    }
}

std::shared_ptr<PyRunStats> runPass(ca::u64 h, const std::string& name,
                                    const std::string& jdkPath)
{
    auto& session = FacadeRegistry::instance().get(h);
    auto  stats   = std::make_shared<PyRunStats>();
    stats->pipeline = name;
    runPipelineOnContainer(*session.container, {name}, "", false, jdkPath, *stats);
    session.clearTrees();   // 管线直驱容器字节 → 树缓存作废（lazy 重解析）
    session.lastStats    = stats;
    session.lastPipeline = name;
    return stats;
}

/// run_pipeline：pass_names XOR pipeline（无回落——显式跑，两者均空即错）。
std::shared_ptr<PyRunStats> runPipeline(ca::u64 h, std::vector<std::string> passNames,
                                        const std::string& pipeline,
                                        const std::string& jdkPath)
{
    auto& session = FacadeRegistry::instance().get(h);
    auto  stats   = std::make_shared<PyRunStats>();
    stats->pipeline =
        !pipeline.empty() ? pipeline
                          : (passNames.size() == 1 ? passNames.front() : "pass_names");
    runPipelineOnContainer(*session.container, std::move(passNames), pipeline, false,
                           jdkPath, *stats);
    session.clearTrees();
    session.lastStats    = stats;
    session.lastPipeline = stats->pipeline;
    return stats;
}

/// opt_preview：管线跑在容器克隆上（试算），diff 后丢弃副本——会话字节零改。
py::dict optPreview(ca::u64 h, std::vector<std::string> passNames,
                    const std::string& pipeline, const std::string& jdkPath)
{
    auto& session = FacadeRegistry::instance().get(h);
    auto  clone   = facadeCloneContainer(session);
    auto  stats   = std::make_shared<PyRunStats>();
    runPipelineOnContainer(*clone, passNames, pipeline, true, jdkPath, *stats);
    const auto diffs = facadeDiffContainers(*session.container, *clone);
    py::dict   out;
    out["changed"] = !diffs.empty();
    py::list entries;
    for (const auto& diff : diffs) {
        py::list ranges;
        for (const auto& range : diff.ranges) {
            py::dict rangeOut;
            rangeOut["offset"]        = range.offset;
            rangeOut["before_length"] = range.beforeLength;
            rangeOut["after_length"]  = range.afterLength;
            ranges.append(rangeOut);
        }
        py::dict entry;
        entry["uri"]         = diff.uri;
        entry["before_size"] = diff.beforeSize;
        entry["after_size"]  = diff.afterSize;
        entry["ranges"]      = ranges;
        entries.append(entry);
    }
    out["diffs"] = entries;
    return out;
}

/// opt_apply：管线落在会话容器上（无选择器回落 default-optimize），成功后
/// 树缓存作废并登记最近一次运行报告。
std::shared_ptr<PyRunStats> optApply(ca::u64 h, std::vector<std::string> passNames,
                                     const std::string& pipeline,
                                     const std::string& jdkPath)
{
    auto& session = FacadeRegistry::instance().get(h);
    auto  stats   = std::make_shared<PyRunStats>();
    stats->pipeline =
        !pipeline.empty() ? pipeline
                          : (passNames.size() == 1 ? passNames.front() : "default-optimize");
    runPipelineOnContainer(*session.container, std::move(passNames), pipeline, true,
                           jdkPath, *stats);
    session.clearTrees();
    session.lastStats    = stats;
    session.lastPipeline = stats->pipeline;
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
    m.doc() = "mjt-py SDK：Java 字节码容器与改写面（键式 API）。\n\n"
              "容器操作全部按纯值键寻址：容器句柄 h（int）、类键 cref=(h, 类名)、\n"
              "成员键 mref/fref=(h, cls, name, desc)，类名一律 internal name\n"
              "（a/b/C 形态）。键失效（容器已关/类已改名/指令已删）抛\n"
              "mjt.StaleRefError（KeyError 子类）；树缓存与脏纪律由 C++ 侧自管。\n"
              "长操作受协作层 deadline 约束（mjt.set_deadline_ms），越限抛\n"
              "mjt.DeadlineExceeded（KeyboardInterrupt 子类）。纯 Python 二次\n"
              "封装层在 mjt.util（from mjt import util as U）。";

    // ── RunStats（管线运行报告） ──
    py::class_<PyRunStats>(m, "RunStats",
                           "单次管线/pass 运行的统计报告（mjt.last_run_report 取最近一次）。")
        .def_property_readonly("pipeline", [](PyRunStats& s) { return s.pipeline; },
                               "运行的管线名或单 pass 名。")
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
        }, "逐 pass 槽位列表 [{name, scope, processed, changed, failed, "
           "duration_ms}]；scope ∈ method/class/global。")
        .def("__repr__", [](PyRunStats& s) {
            return "<RunStats " + s.pipeline + ": " + std::to_string(s.slots.size()) + " slots>";
        });

    // ── 管线 / 单 pass 面 ──
    m.def("run_pass", &runPass, py::arg("h"), py::arg("name"), py::arg("jdk") = "",
          "按名运行 mjbase 内置单 pass。\n\n"
          "Args:\n"
          "    h: 容器句柄。\n"
          "    name: pass 名（mjt.passes() 列出可用面）。\n"
          "    jdk: JDK 路径——pass 需要跨类层次解析帧时给出；空串跳过。\n"
          "Returns:\n"
          "    RunStats（同时登记为该容器最近运行报告）。管线直驱容器字节，\n"
          "    成功后该容器全部缓存树作废。\n"
          "Raises:\n"
          "    ValueError: pass 名未注册。\n"
          "    RuntimeError: pass 运行失败。");
    m.def("run_pipeline", &runPipeline, py::arg("h"), py::arg("pass_names") = std::vector<std::string>{},
          py::arg("pipeline") = "", py::arg("jdk") = "",
          "依序运行多个 pass，或按名运行整条管线。\n\n"
          "Args:\n"
          "    h: 容器句柄。\n"
          "    pass_names: pass 名列表（与 pipeline 互斥，两者必须给其一，\n"
          "        无回落）。\n"
          "    pipeline: 管线名（mjt.pass_describe() 列出可用面）。\n"
          "    jdk: JDK 路径（可选，跨类层次帧解析用）。\n"
          "Returns:\n"
          "    RunStats；成功后树缓存作废并登记最近运行报告。\n"
          "Raises:\n"
          "    ValueError: pass_names 与 pipeline 同时给出、均空、或名字未知。\n"
          "    RuntimeError: pass 运行失败。");
    m.def(
        "opt_preview",
        [](ca::u64 h, std::vector<std::string> passNames, const std::string& pipeline,
           const std::string& jdk) { return optPreview(h, std::move(passNames), pipeline, jdk); },
        py::arg("h"), py::arg("pass_names") = std::vector<std::string>{},
        py::arg("pipeline") = "", py::arg("jdk") = "",
        "优化试算：管线跑在容器克隆上，diff 后丢弃副本——会话容器零改动。\n\n"
        "Args:\n"
        "    h: 容器句柄。\n"
        "    pass_names: pass 名列表（与 pipeline 互斥）。\n"
        "    pipeline: 管线名；与 pass_names 均空时回落 default-optimize。\n"
        "    jdk: JDK 路径（可选，跨类层次帧解析用）。\n"
        "Returns:\n"
        "    {changed: bool, diffs: [{uri, before_size, after_size, ranges:\n"
        "    [{offset, before_length, after_length}]}]}。\n"
        "Raises:\n"
        "    ValueError: 选择器非法或 pass/管线名未知。\n"
        "    RuntimeError: pass 运行失败。");
    m.def("opt_apply", &optApply, py::arg("h"), py::arg("pass_names") = std::vector<std::string>{},
          py::arg("pipeline") = "", py::arg("jdk") = "",
          "优化落盘：管线跑在会话容器上。\n\n"
          "Args:\n"
          "    h: 容器句柄。\n"
          "    pass_names: pass 名列表（与 pipeline 互斥）。\n"
          "    pipeline: 管线名；与 pass_names 均空时回落 default-optimize。\n"
          "    jdk: JDK 路径（可选，跨类层次帧解析用）。\n"
          "Returns:\n"
          "    RunStats；成功后树缓存作废并登记最近运行报告。\n"
          "Raises:\n"
          "    ValueError: 选择器非法或 pass/管线名未知。\n"
          "    RuntimeError: pass 运行失败。");
    m.def("passes", [] {
        std::vector<std::string> out;
        for (const auto& info : mjbase::spi::defaultPassRegistry().list())
            out.push_back(std::string(info.name));
        return out;
    }, "列出可用 pass 名（run_pass / run_pipeline 的 pass_names 取值面）。\n\n"
       "Returns:\n"
       "    list[str]，pass 名列表。");
    m.def(
        "pass_describe",
        [] {
            py::dict out;
            py::list passes;
            for (const auto& pass : mjbase::spi::defaultPassRegistry().list()) {
                // scope 输出小写（workspace availablePasses 同口径）。
                const std::string scope =
                    pass.scope == mjbase::spi::PassScope::Method   ? "method"
                    : pass.scope == mjbase::spi::PassScope::Class  ? "class"
                    : pass.scope == mjbase::spi::PassScope::Global ? "global"
                                                                   : "cf";
                py::dict entry;
                entry["name"]        = std::string(pass.name);
                entry["description"] = std::string(pass.description);
                entry["scope"]       = scope;
                passes.append(entry);
            }
            out["passes"] = passes;
            mjbase::spi::registerBuiltinPipelines();
            py::list pipelines;
            for (const auto& pipeline : mjbase::spi::PassRegistry::instance().listPipelines()) {
                py::dict entry;
                entry["name"]        = pipeline.name;
                entry["description"] = pipeline.description;
                pipelines.append(entry);
            }
            out["pipelines"] = pipelines;
            return out;
        },
        "pass/管线目录（run_pipeline 可用面）。\n\n"
        "Returns:\n"
        "    {passes: [{name, description, scope}], pipelines: [{name, "
        "description}]}；scope ∈ method/class/global。");
    m.def(
        "analysis_evaluate",
        [](const std::string& expression, py::object bindings, bool simplify,
           int max_depth) {
            if (expression.empty()) throw py::value_error("expression is required");
            std::string parseError;
            auto        parsed = mjbase::expr::parseExpression(expression, &parseError);
            if (!parsed) throw py::value_error("cannot parse expression: " + parseError);
            if (simplify) parsed = mjbase::expr::simplify(parsed).simplified;
            mjbase::expr::VarBindings vars;
            if (!bindings.is_none()) {
                const auto dict = bindings.cast<py::dict>();
                for (const auto& item : dict) {
                    const auto name  = item.first.cast<std::string>();
                    const auto value = item.second.cast<long long>();
                    vars.emplace(name, mjbase::expr::ExprValue(static_cast<ca::i64>(value)));
                }
            }
            if (max_depth <= 0 || max_depth > 4096) {
                throw py::value_error("max_depth must be between 1 and 4096");
            }
            const auto result =
                mjbase::expr::eval(*parsed, vars, static_cast<ca::u32>(max_depth));
            py::dict out;
            out["expression"] = expression;
            out["normalized"] = mjbase::expr::toSExpr(*parsed);
            out["simplified"] = simplify;
            out["evaluated"]  = result.ok;
            if (result.ok)
                out["value"] = result.value.rawValue();
            else
                out["error"] = result.error;
            return out;
        },
        py::arg("expression"), py::arg("bindings") = py::none(), py::arg("simplify") = false,
        py::arg("max_depth") = 1024,
        "分析表达式求值：解析 →（可选）化简 → 变量绑定求值。\n\n"
        "Args:\n"
        "    expression: 表达式文本（mjbase expr 语法，如 \"1 + 2 * 3\"）。\n"
        "    bindings: 变量名 → int 的绑定 dict；None 表示无绑定。\n"
        "    simplify: 求值前是否先化简。\n"
        "    max_depth: 求值深度上限（1..4096，默认 1024）。\n"
        "Returns:\n"
        "    {expression, normalized, simplified, evaluated, value|error}；\n"
        "    evaluated=False 时错误详情在 error，无 value 键。\n"
        "Raises:\n"
        "    ValueError: 表达式为空或解析失败，max_depth 越界，绑定值无法\n"
        "        转 int。");

    // ── 自定义 pass 类型锚（适配器实现属私有侧管线消费面，见文件头注记） ──
    py::class_<PyMethodPassBase, std::shared_ptr<PyMethodPassBase>>(
        m, "MethodPass",
        "自定义 pass 类型锚（Python 侧继承）：子类实现 run(mref)，返回 "
        "mjt.changed() / mjt.preserved_all() 报告是否改动。适配到具体管线的 "
        "实现在私有侧，基座面只保类型形状。")
        .def(py::init<>(), "构造（子类化并实现 run(mref)）。");
    // MethodPass.run 返回锚（不透明：Python 侧只从 changed()/preserved_all() 取）。
    py::class_<PyPreserved>(m, "PreservedAnalyses",
                            "MethodPass.run 的返回锚（不透明值，不可构造）。")
        .def("__repr__", [](const PyPreserved& p) {
            return p.changed ? "PreservedAnalyses.none" : "PreservedAnalyses.all";
        });
    m.def("changed", [] { return PyPreserved{true}; },
          "MethodPass.run 返回值：报告本 pass 有改动。");
    m.def("preserved_all", [] { return PyPreserved{false}; },
          "MethodPass.run 返回值：报告本 pass 无改动（全部保留）。");

    // ── 协作层 deadline（长操作检查点抛 mjt.DeadlineExceeded） ──
    m.def("set_deadline_ms",
          [](std::int64_t ms) { setDeadlineMs(ms); },
          py::arg("ms"),
          "设置整体 deadline（毫秒，从现在起）。\n\n"
          "Args:\n"
          "    ms: 大于 0 生效；小于 0 立即过期（测试位）；0 清除。\n\n"
          "facade 长操作（import/flush/verify/sanitize/run_pass 等检查点）越限\n"
          "抛 mjt.DeadlineExceeded（KeyboardInterrupt 子类）。嵌入宿主另有逐行\n"
          "trace 与 watchdog 硬层，见 doc_ai/pysdk-script-limits.md。");
    m.def("clear_deadline", [] { setDeadlineMs(0); },
          "清除 deadline（回到无上限）。");

    // ── 合成样本 ──
    m.def("write_demo_jar", &writeSyntheticDemoJar, py::arg("path"),
          "写出合成混淆形态演示 jar（unittest/上手演示用；零样本字节入库）。\n\n"
          "Args:\n"
          "    path: 输出 jar 路径。\n\n"
          "罐内 5 个类：me/demo/{Api,App,Color,Crypto,Util}，带假 handler、\n"
          "恒假谓词、字符串解密调用点等可辨形态。");

    // ── 平函数面（主面）──
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
