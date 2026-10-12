#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/core/datatype.hpp"
#include "libca/log/log_macros.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/spi/analysis_manager.hpp"
#include "mbase/spi/preserved_analyses.hpp"

namespace mjt::base {

//前向声明：GlobalToClassPassAdaptor 持 ProgressSink 指针供进度反馈（issue ），
//仅用指针故前向声明足够，避免此处 include 整个 progress_sink.hpp。
class ProgressSink;

///方法过滤谓词：返回 true 表示对该方法运行 pass。
///用 std::function<void(PredicateT)> 风格，CLI 的 -m 把目标解析成此谓词注入。
using MethodPredicate = std::function<bool(const jbytecode::tree::MethodNode&)>;

//── 三级作用域的 AnalysisManager 具体类型（IR 单元见 pass-framework-design.md §1.1）──
using MethodAnalysisManager = AnalysisManager<jbytecode::tree::MethodNode>;
using ClassAnalysisManager  = AnalysisManager<jbytecode::tree::ClassNode>;
using GlobalAnalysisManager = AnalysisManager<ResourceContainer>;

//── 跨作用域 AM Proxy（设计文档 §4.1）──

///Proxy 结果：持有子级 AM 指针，Adaptor 运行前注入。
template<class ChildAMT>
struct InnerAMProxyResult
{
    ChildAMT* am = nullptr;
    ChildAMT& operator*() const { return *am; }
    ChildAMT* operator->() const { return am; }
};

///方法级 AM Proxy：注册在 ClassAnalysisManager 上，由 ClassToMethodPassAdaptor 注入。
struct MethodAnalysisManagerProxy
{
    using Result = InnerAMProxyResult<MethodAnalysisManager>;
    Result run(jbytecode::tree::ClassNode&, ClassAnalysisManager&) { return {}; }
};

///@brief 方法级「当前类」上下文（pinned）。
///
///方法级 pass（AsmMethodPass）常常需要知道当前方法所属的类，对齐 Java legacy
///的 AsmMethodContext.currentClass / getClassName：
///- 帧分析 Analyzer::analyze(owner, method) 的 owner 参数需要类名（Java 用
///context.getClassName）；
///- LocalVariableInfoRebuilder 构造 "this" 的描述符需要 L<classname>;；
///- 等等。
///
///由 ClassToMethodPassAdaptor 在遍历类内每个方法前，经 setPinned 注入到
///MethodAnalysisManager。方法级 pass 经 am.getPinned<CurrentClassContext> 取。
///pinned 保证它不受 PassManager 的 invalidate 影响（类在一次方法扫描里不变）。
///adaptor 离开类作用域时恢复原上下文或撤销注入，异常路径同样如此。
///
///取用约定：未注入时 getPtr 为 nullptr（如对单方法直接跑而无 class 上下文），
///调用方需自行判空降级。
struct CurrentClassContext
{
    ///pinned 结果：当前类的 ClassNode 指针（不持有所有权，由外层 ClassNode 存活）。
    struct Result
    {
        jbytecode::tree::ClassNode* classNode = nullptr;
    };
    Result run(jbytecode::tree::MethodNode&, MethodAnalysisManager&) { return {}; }
};

///类级 AM Proxy 预留类型。注入/撤销协议完成前不注册为稳定分析。
struct ClassAnalysisManagerProxy
{
    using Result = InnerAMProxyResult<ClassAnalysisManager>;
    Result run(ResourceContainer&, GlobalAnalysisManager&) { return {}; }
};

///@brief Pass 概念基模板：在某作用域 IR 单元上运行，返回保留的分析集合。
///
///对标 LLVM PassInfoMixin/PassConcept。三个具体作用域基类由它特化别名而来。
///
///@tparam IRUnitT 作用域 IR 单元（MethodNode / ClassNode / ResourceContainer）
///@tparam AnalysisManagerT 对应作用域的 AnalysisManager
template<class IRUnitT, class AnalysisManagerT>
class PassModel
{
public:
    virtual ~PassModel() = default;
    ///在 unit 上运行；返回本次变换后仍然有效的分析集合（驱动 AM 失效）。
    virtual PreservedAnalyses run(IRUnitT& unit, AnalysisManagerT& am) = 0;
};

//── 三个具体作用域 Pass 基类（名字沿用既定术语，见设计文档 D1）──
//canonical 定义在此；旧 asm_pass.hpp 的 void-run 版本已删除。
using AsmMethodPass = PassModel<jbytecode::tree::MethodNode, MethodAnalysisManager>;
using AsmClassPass  = PassModel<jbytecode::tree::ClassNode, ClassAnalysisManager>;
using AsmGlobalPass = PassModel<ResourceContainer, GlobalAnalysisManager>;

///@brief 同作用域 pass 的有序编排器，本身也是一个 Pass（可嵌套）。
///
///顺序运行所有 pass；每个 pass 后按其 PreservedAnalyses 失效 am 缓存，并把各 pass 的
///保留集合取交作为整体返回（只保留所有 pass 都保住的分析）。
///
///@par 生命周期与异常
///
///`PassManager` 以 `unique_ptr` 独占 pass；运行期间 IR 和 AnalysisManager 必须由调用方
///
///保持存活。pass 抛出的异常不在编排器中吞掉，调用方负责决定作业回滚或终止；已经完成的
///
///前序变换不会自动回滚，后续 pass 不再运行。异常 pass 可能已修改 IR，因此编排器在
///重抛前保守失效当前作用域缓存，并清理可达的子级普通缓存。`addPass(nullptr)` 属于配置
///错误，稳定抛出 `std::invalid_argument`。
///
///@par contract 责任
///每个 pass 必须准确返回
///`PreservedAnalyses`。编排器只负责调用 invalidate 和求交，
///不会替不可靠的 pass
///猜测分析是否仍然有效。
///
///@warning 非线程安全。
template<class IRUnitT, class AnalysisManagerT>
class PassManager : public PassModel<IRUnitT, AnalysisManagerT>
{
public:
    using PassT = PassModel<IRUnitT, AnalysisManagerT>;

    ///追加一个同作用域 pass（移动持有）。返回 *this 便于链式。
    PassManager& addPass(std::unique_ptr<PassT> pass)
    {
        if (!pass) throw std::invalid_argument("PassManager::addPass: null pass");
        passes_.push_back(std::move(pass));
        return *this;
    }

    PreservedAnalyses run(IRUnitT& unit, AnalysisManagerT& am) override
    {
        auto pa = PreservedAnalyses::all();
        //诊断探针（临时，MJT_PASS_PROBE=1）：逐 pass 打印起止时间戳与 pass 名，
        //与护栏的 RSS 曲线（MJT_MEM_CSV）按时间对齐即可归属内存增长到具体 pass。
        static const bool passProbe = std::getenv("MJT_PASS_PROBE") != nullptr;
        const auto        probeT0   = std::chrono::steady_clock::now();
        ca::usize         passIndex = 0;
        for (auto& pass : passes_) {
            if (passProbe) {
                const auto now = std::chrono::steady_clock::now();
                //RTTI 全局禁用（/GR-），不能 typeid 取 pass 名；pass 序号按
                //buildNestedPipeline 的注册顺序对照。
                //诊断统一走 ca::log 门面：不依赖 stderr/std::cerr，
                //规避 subprocess.hpp #undef stderr 造成的 C2065。
                CA_LOG_DEBUG("[pass-probe] t={:.1f}s begin #{}",
                             std::chrono::duration<double>(now - probeT0).count(),
                             passIndex);
            }
            PreservedAnalyses passPA;
            try {
                passPA = pass->run(unit, am);
            }
            catch (...) {
                //pass 可能在抛出异常前已经修改 IR；该路径没有任何保留声明，因此调用方捕获异常后
                //不能继续观察旧的分析结果。
                am.invalidate(unit, PreservedAnalyses::none());
                if constexpr (std::is_same_v<IRUnitT, jbytecode::tree::ClassNode>) {
                    if (auto* proxy = am.template getPinned<MethodAnalysisManagerProxy>()) {
                        if (proxy->am) proxy->am->clearAll();
                    }
                }
                throw;
            }
            am.invalidate(unit, passPA);   //失效发生点
            //类级 pass 可能直接改写类内 MethodNode。类级 pass 改动 IR 时无法证明方法级
            //分析仍有效，经 pinned proxy 清空子级缓存，避免后续方法 pass 命中旧结果。
            if constexpr (std::is_same_v<IRUnitT, jbytecode::tree::ClassNode>) {
                if (passPA.changed()) {
                    if (auto* proxy = am.template getPinned<MethodAnalysisManagerProxy>()) {
                        if (proxy->am) proxy->am->clearAll();
                    }
                }
            }
            if (passProbe) {
                const auto now = std::chrono::steady_clock::now();
                CA_LOG_DEBUG("[pass-probe] t={:.1f}s end   #{}",
                             std::chrono::duration<double>(now - probeT0).count(),
                             passIndex);
            }
            ++passIndex;
            pa.intersect(passPA);
        }
        return pa;
    }

    bool      empty() const { return passes_.empty(); }
    ca::usize size() const { return passes_.size(); }

private:
    std::vector<std::unique_ptr<PassT>> passes_;
};

using MethodPassManager = PassManager<jbytecode::tree::MethodNode, MethodAnalysisManager>;
using ClassPassManager  = PassManager<jbytecode::tree::ClassNode, ClassAnalysisManager>;
using GlobalPassManager = PassManager<ResourceContainer, GlobalAnalysisManager>;

//── 可选容错档位（issue ）──

///@brief 容错档位的跳过诊断收集器（issue ）。
///
///作业结束汇总的载体：容错档位下 adaptor 每跳过一个单元（类/方法）追加一条，
///run 正常返回后调用方即可读取汇总（跳过数 / 失败类清单）。诊断行对齐既有
///stable-skip 契约格式（DiagnosticClassFileCleaner）：
///`SKIP <scope>=<target> reason=<reason> detail=<detail>`。
///调用方拥有实例并负责生命周期与清理；不传入（nullptr）时容错仍然生效，只是不记录。
struct PassFaultReport
{
    ///单条跳过记录。
    struct Entry
    {
        std::string scope;    ///< 跳过粒度："class" / "method"
        std::string target;   ///< 类资源 URI；方法级为 "<classInternalName>.<name><descriptor>"
        std::string reason;   ///< 失败阶段："sanitize_error" / "reader_error" / "pass_error" /
                              ///< "writer_error"（sanitize_error 见 净化门）
        std::string detail;   ///< 异常 what；空表示无细节

        ///stable-skip 诊断行（与既有契约断言格式一致）；无 detail 时省略该段。
        std::string toLine() const
        {
            std::string line = "SKIP ";
            line += scope;
            line += '=';
            line += target;
            line += " reason=";
            line += reason;
            if (!detail.empty()) {
                line += " detail=";
                line += detail;
            }
            return line;
        }
    };

    ///按发生顺序追加的跳过记录。
    std::vector<Entry> entries;

    ///追加一条跳过记录。
    void add(Entry entry) { entries.push_back(std::move(entry)); }

    ///跳过单元总数（作业结束汇总的「跳过数」）。
    ca::usize skippedCount() const { return entries.size(); }

    ///失败类清单（scope=="class" 的 target，按发生顺序去重）。
    std::vector<std::string> failedClasses() const
    {
        std::vector<std::string> classes;
        for (const auto& entry : entries) {
            if (entry.scope != "class") continue;
            bool seen = false;
            for (const auto& existing : classes) {
                if (existing == entry.target) {
                    seen = true;
                    break;
                }
            }
            if (!seen) classes.push_back(entry.target);
        }
        return classes;
    }

    bool empty() const { return entries.empty(); }
    ///清空（复用同一报告跑下一作业前调用）。
    void clear() { entries.clear(); }
};

///@brief pass 管线可选容错档位（issue ）。
///
///默认全关：管线语义与历史版本完全一致——任一方法/类失败即抛出，作业级原子
///提交（dev_plan C）。两个档位相互独立、分别由对应 adaptor 消费：
///- perMethodFaultIsolation：ClassToMethodPassAdaptor 方法循环 per-method
///try/catch，失败方法跳过并记录诊断，类内其余方法继续；
///- perClassFaultIsolation：GlobalToClassPassAdaptor per-class try/catch，
///失败类跳过（不写回，原 bytes 保持原样）并记录诊断，其余类继续，作业
///结束照常统一提交成功类，汇总经 faultReport 带出。
///非 std 异常任何档位都不吞（向上传播，由调用方终止作业）。
struct PassPipelineOptions
{
    ///方法级失败隔离档位（ClassToMethodPassAdaptor 消费）。
    bool perMethodFaultIsolation = false;
    ///类级失败隔离档位（GlobalToClassPassAdaptor 消费）。
    bool perClassFaultIsolation = false;
    ///可选诊断收集；容错档位下每跳过一个单元追加一条。调用方拥有并负责清理。
    PassFaultReport* faultReport = nullptr;
};

///@brief 把方法级 pipeline 适配成一个类级 pass：对类内每个 method 跑一遍。
///
///正规化 legacy AsmClassPassManager.runOrdered 的「method pass 在类内逐方法应用」语义
///（见设计文档 §3）。method 级 AnalysisManager 经 cam 上的 MethodAnalysisManagerProxy
///获取（S3 Proxy 跨层失效联动，Adaptor 不再直接持引用）。
///缺失或为空的 proxy 属于 pipeline 配置错误，`run` 抛出 `std::logic_error`，不得空指针崩溃。
///
///容错档位（issue ）：options.perMethodFaultIsolation 开启时方法循环改为
///per-method try/catch（仅 `std::exception`，非 std 异常照常传播），失败方法跳过、
///其余方法继续；失败按 `PreservedAnalyses::none` 参与聚合（异常 pass 可能已改 IR，
///保守驱动外层重序列化），诊断经 options.faultReport 记录。默认关闭，语义不变。
class ClassToMethodPassAdaptor : public PassModel<jbytecode::tree::ClassNode, ClassAnalysisManager>
{
public:
    ///@param predicate 可选方法过滤（-m 目标）；nullptr 表示不过滤，对每个方法都跑。
    ///@param options 可选容错档位与诊断收集（issue ）；默认全关，语义不变。
    explicit ClassToMethodPassAdaptor(MethodPassManager mpm, MethodPredicate predicate = nullptr,
                                      PassPipelineOptions options = {})
        : mpm_(std::move(mpm))
        , predicate_(std::move(predicate))
        , options_(options)
    {}

    PreservedAnalyses run(jbytecode::tree::ClassNode& clazz, ClassAnalysisManager& cam) override
    {
        //经 pinned proxy 取方法级 AM（不受 class 级 pass 的 invalidate 影响）。
        auto* proxyResult = cam.getPinned<MethodAnalysisManagerProxy>();
        if (!proxyResult || !proxyResult->am) {
            throw std::logic_error(
                "ClassToMethodPassAdaptor: MethodAnalysisManagerProxy is not pinned");
        }
        auto& mam         = *proxyResult->am;
        //注入「当前类」上下文（pinned），方法级 pass 经 mam.getPinned<CurrentClassContext>
        //取到所属 ClassNode，对齐 Java AsmMethodContext.currentClass。类在整个方法扫描
        //期间不变，故在循环外设一次即可。
        const auto* previousContext = mam.getPinned<CurrentClassContext>();
        const bool  hadPreviousContext = previousContext != nullptr;
        const auto  savedContext =
            hadPreviousContext ? *previousContext : CurrentClassContext::Result{};
        mam.setPinned<CurrentClassContext>({&clazz});
        auto restoreContext = [&] {
            if (hadPreviousContext) {
                mam.setPinned<CurrentClassContext>(savedContext);
            }
            else {
                mam.clearPinned<CurrentClassContext>();
            }
        };

        try {
            auto pa = PreservedAnalyses::all();
            for (auto* method : clazz.methods) {
                if (!method) continue;
                //-m 过滤：谓词返回 false 则跳过该方法（保留全部分析）
                if (predicate_ && !predicate_(*method)) continue;
                if (!options_.perMethodFaultIsolation) {
                    pa.intersect(mpm_.run(*method, mam));
                    continue;
                }
                //容错档位（issue ）：仅捕获 std::exception，失败方法跳过，其余继续。
                //PassManager 已在重抛前对该方法保守失效全部缓存（该路径无任何保留声明），
                //此处无需再清理；按 none 参与聚合使整体视为已改动，驱动外层保留其余
                //方法的成功变换并重序列化。非 std 异常不吞，照常向外传播。
                try {
                    pa.intersect(mpm_.run(*method, mam));
                }
                catch (const std::exception& e) {
                    pa.intersect(PreservedAnalyses::none());
                    if (options_.faultReport) {
                        options_.faultReport->add({/*scope*/ "method",
                                                   methodTarget(clazz, *method),
                                                   /*reason*/ "pass_error", e.what()});
                    }
                }
            }
            restoreContext();
            return pa;
        }
        catch (...) {
            restoreContext();
            throw;
        }
    }

private:
    ///方法诊断目标："<classInternalName>.<name><descriptor>"。
    static std::string methodTarget(const jbytecode::tree::ClassNode&  clazz,
                                    const jbytecode::tree::MethodNode& method)
    {
        std::string target = clazz.name.to_std_string();
        target += '.';
        target += method.name.to_std_string();
        target += method.desc.to_std_string();
        return target;
    }

    MethodPassManager   mpm_;
    MethodPredicate     predicate_;
    PassPipelineOptions options_;
};

///@brief 把类级 pipeline 适配成一个全局级 pass：对容器内每个 class 跑一遍。
///
///跨类 pass 的载体 + jar 级编排核心（见 pass-framework-design.md §3）。对
///ResourceContainer 内每个 ClassResource：读字节 → ClassNode → 跑 class pipeline →
///写回字节（仅当 pipeline 改动）。arena 经 GlobalAnalysisManager::arena 取，
///reader/writer/各级 AM 共享同一 arena（单 arena 贯穿作业）。
///
///Class 级 Method 级 AM 在 run 内部创建（经 ClassAnalysisManagerProxy 向 Class 级
///pass 暴露）。S3 引入 Proxy 跨层失效联动后，MethodToClass 方向联动也经此 Proxy 传递。
///Class/Method 普通分析缓存只在当前 class 内有效；切换资源前必须清空，不能因
///`ClassNode`/`MethodNode` 地址复用而跨 class 命中。pinned proxy 贯穿本次 adaptor 调用。
///reader、class/method pass 或 writer 失败时抛出包含资源 URI 的
///`std::runtime_error`；作业级原子性（dev_plan C）：写回推迟到全部 class 成功后统一
///提交，失败时容器保持原样（无部分写回）。
///
///容错档位（issue ）：options.perClassFaultIsolation 开启时改为 per-class 跳过
///模式——仅捕获 `std::exception`（非 std 异常照常传播终止作业），失败类记录诊断后
///跳过（不进 pending，原 bytes 保持原样），其余类继续；作业结束照常统一提交全部
///成功类，汇总（跳过数/失败类清单）经 options.faultReport 带出。诊断 reason 按失败
///阶段区分：reader_error / pass_error / writer_error。默认关闭，语义不变。
class GlobalToClassPassAdaptor : public PassModel<ResourceContainer, GlobalAnalysisManager>
{
public:
    ///@param progress 可选进度 sink（issue ）；nullptr 表示无进度反馈（默认，单测路径）。
    ///@param options 可选容错档位与诊断收集（issue ）；默认全关，语义不变。
    explicit GlobalToClassPassAdaptor(
        ClassPassManager cpm, int classWriterFlags = 0,
        jbytecode::core::ICommonSuperClassProvider* typeResolver = nullptr,
        ProgressSink*                               progress     = nullptr,
        PassPipelineOptions                         options      = {})
        : cpm_(std::move(cpm))
        , classWriterFlags_(classWriterFlags)
        , typeResolver_(typeResolver)
        , progress_(progress)
        , options_(options)
    {}

    PreservedAnalyses run(ResourceContainer& container, GlobalAnalysisManager& gam) override;

private:
    ClassPassManager                            cpm_;
    int                                         classWriterFlags_;
    jbytecode::core::ICommonSuperClassProvider* typeResolver_;
    ProgressSink*                               progress_;
    PassPipelineOptions                         options_;
};

}   //namespace mjt::base
