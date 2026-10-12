#include "mbase/pass/pass_registry.hpp"

#include "mbase/pass/opt/meta_info_remover.hpp"
#include "mbase/pass/opt/unused_label_remover.hpp"
#include "mbase/pass/opt/constant_folder.hpp"
#include "mbase/pass/opt/constant_jump_optimizer.hpp"
#include "mbase/pass/opt/constant_propagator.hpp"
#include "mbase/pass/opt/expression_simplifier.hpp"
#include "mbase/pass/opt/jump_label_layout.hpp"
#include "mbase/pass/opt/dead_code_remover.hpp"
#include "mbase/pass/opt/stack_operation.hpp"
#include "mbase/spi/class_file_sanitize_gate.hpp"

#include <algorithm>
#include <chrono>
#include <memory>

namespace mjt::base {

namespace {

//── 只读观测装饰器（dry-run）────────────────────────────
//
//stats != nullptr 时把每个 pass 实例包进 ObservedPass：run 纯透传（同参数、
//同返回值、异常原样重抛），只在前后计时并按 PreservedAnalyses::changed 记
//changed。stats == nullptr 时完全不参与构建，管线结构与基线一致。

template<class IRUnitT, class AnalysisManagerT>
class ObservedPass final : public PassModel<IRUnitT, AnalysisManagerT>
{
public:
    using InnerT = PassModel<IRUnitT, AnalysisManagerT>;
    using Clock  = std::chrono::steady_clock;

    ObservedPass(std::unique_ptr<InnerT> inner, PassRunStats::Slot* slot)
        : inner_(std::move(inner)), slot_(slot)
    {}

    PreservedAnalyses run(IRUnitT& unit, AnalysisManagerT& am) override
    {
        const auto begin = Clock::now();
        try {
            PreservedAnalyses pa = inner_->run(unit, am);
            record(pa.changed(), false, Clock::now() - begin);
            return pa;
        }
        catch (...) {
            record(false, true, Clock::now() - begin);
            throw;
        }
    }

private:
    void record(bool changed, bool failed, std::chrono::steady_clock::duration elapsed)
    {
        if (!slot_) return;
        slot_->processed += 1;
        if (changed) slot_->changed += 1;
        if (failed) slot_->failed += 1;
        slot_->durationMs += std::chrono::duration<double, std::milli>(elapsed).count();
    }

    std::unique_ptr<InnerT> inner_;
    PassRunStats::Slot*     slot_;
};

template<class IRUnitT, class AnalysisManagerT>
std::unique_ptr<PassModel<IRUnitT, AnalysisManagerT>> observe(
    std::unique_ptr<PassModel<IRUnitT, AnalysisManagerT>> inner, PassRunStats::Slot* slot)
{
    if (!slot) return inner;
    return std::make_unique<ObservedPass<IRUnitT, AnalysisManagerT>>(std::move(inner), slot);
}

static void addFixPatternReplacePhase(PipelineBuilder& builder)
{
    //legacy fixPatternReplaceManager 的保守认证子集。
    builder.addMethodPass([] { return std::make_unique<ArithmeticSimplifier>(); },
                          "ArithmeticSimplifier")
        .addMethodPass([] { return std::make_unique<ExpressionSimplifier>(); },
                       "ExpressionSimplifier")
        .addMethodPass([] { return std::make_unique<StackOperationSimplifier>(); },
                       "StackOperationSimplifier")
        //帧驱动栈操作展开（对齐 Recaf deobf pass1）：operand 可追溯到单一常量
        //producer 的 DUP/SWAP 族指令展开为 producer 克隆，简化后续折叠输入。
        .addMethodPass([] { return std::make_unique<StackOperationExpander>(); },
                       "StackOperationExpander")
        .addMethodPass([] { return std::make_unique<NopRemover>(); }, "NopRemover");
}

static void addPeepholeGarbageCleanPhase(PipelineBuilder& builder)
{
    //legacy peepholeGarbageCleanManager 的执行顺序。
    builder.addMethodPass([] { return std::make_unique<SafeDeadCodeRemover>(); },
                          "SafeDeadCodeRemover")
        .addMethodPass([] { return std::make_unique<NopRemover>(); }, "NopRemover")
        .addMethodPass([] { return std::make_unique<UnusedLabelRemover>(); },
                       "UnusedLabelRemover")
        .addMethodPass([] { return std::make_unique<PopUnUsedLocalVariables>(); },
                       "PopUnUsedLocalVariables")
        .addMethodPass([] { return std::make_unique<UselessPopCleaner>(); },
                       "UselessPopCleaner");
}

static void addSimpleConstantFoldPhase(PipelineBuilder& builder)
{
    //legacy simpleConstantFoldManager 的执行顺序。
    builder.addMethodPass([] { return std::make_unique<UnusedLabelRemover>(); },
                          "UnusedLabelRemover")
        .addMethodPass([] { return std::make_unique<ConstantFolder>(); },
                       "ConstantFolder")
        //变量常量传播（对齐 Recaf deobf VariableFolding）：单 store 常量 → 复制到
        //各 load 处，让后续控制流折叠看到更确定的常量。
        .addMethodPass([] { return std::make_unique<ConsLocalVarOptimizer>(); },
                       "ConsLocalVarOptimizer");
}

static void addControlFlowSimplifyPhase(PipelineBuilder& builder)
{
    //legacy controlFlowSimplifyManager 的执行顺序。
    builder.addMethodPass([] { return std::make_unique<ConstantSwitchFolder>(); },
                          "ConstantSwitchFolder")
        .addMethodPass([] { return std::make_unique<ConsJumpOptimizer>(); },
                       "ConsJumpOptimizer")
        .addMethodPass([] { return std::make_unique<RedundantJumpRemover>(); },
                       "RedundantJumpRemover")
        .addMethodPass([] { return std::make_unique<SafeDeadCodeRemover>(); },
                       "SafeDeadCodeRemover")
        .addMethodPass([] { return std::make_unique<RedundantJumpRemover>(); },
                       "RedundantJumpRemover")
        .addMethodPass([] { return std::make_unique<UnusedLabelRemover>(); },
                       "UnusedLabelRemover");
}

static PipelineBuilder buildFixPatternReplacePhase()
{
    PipelineBuilder builder;
    addFixPatternReplacePhase(builder);
    return builder;
}

static PipelineBuilder buildPeepholeGarbageCleanPhase()
{
    PipelineBuilder builder;
    addPeepholeGarbageCleanPhase(builder);
    return builder;
}

static PipelineBuilder buildSimpleConstantFoldPhase()
{
    PipelineBuilder builder;
    addSimpleConstantFoldPhase(builder);
    return builder;
}

static PipelineBuilder buildControlFlowSimplifyPhase()
{
    PipelineBuilder builder;
    addControlFlowSimplifyPhase(builder);
    return builder;
}

static PipelineBuilder buildDefaultOptimizePipeline()
{
    PipelineBuilder builder;
    addFixPatternReplacePhase(builder);
    addSimpleConstantFoldPhase(builder);
    addControlFlowSimplifyPhase(builder);
    addPeepholeGarbageCleanPhase(builder);
    //全局常量传播段（对齐 Recaf deobf StaticValueInlining / CallResultInlining 的
    //全程序能力；mbase 版更强：putstatic 全程序审计 / 纯函数摘要，且保守跨类语义）：
    //常量 static 字段（仅初始化无 putstatic）→ getstatic 替换为常量加载；
    //纯函数静态调用 → 返回常量替换（保留参数求值与槽位宽度）。
    //按优化管线语义注册为 post-class 段：在全部方法级 phase 之后执行，观察优化后的最终方法体。
    //此前误注册为 addGlobalPass（buildNestedPipeline 中先于 adaptor 执行），
    //与两处文档及 addPostClassPass 的设计意图相反。
    builder.addPostClassPass([] { return std::make_unique<ConsStaticFieldReplacer>(); },
                             "ConsStaticFieldReplacer")
        .addPostClassPass([] { return std::make_unique<ConsReturnPropagator>(); },
                          "ConsReturnPropagator");
    return builder;
}

//对齐 Java AsmOptimizePhases.allMetaInfoRemoveManager：
//细粒度元信息清理链（FinalRemover 等 8 个子 pass），不进 default-optimize 默认链。
static void addMetaInfoRemovePhase(PipelineBuilder& builder)
{
    builder.addClassPass([] { return std::make_unique<FinalRemover>(); }, "FinalRemover")
        .addClassPass([] { return std::make_unique<SyntheticBridgeRemover>(); },
                      "SyntheticBridgeRemover")
        .addClassPass([] { return std::make_unique<KotlinMetadataRemover>(); },
                      "KotlinMetadataRemover")
        .addClassPass([] { return std::make_unique<SourceInfoRemover>(); },
                      "SourceInfoRemover")
        .addMethodPass([] { return std::make_unique<LineNumberRemover>(); },
                       "LineNumberRemover")
        .addMethodPass([] { return std::make_unique<LocalVariableInfoRemover>(); },
                       "LocalVariableInfoRemover")
        .addMethodPass([] { return std::make_unique<ParametersInfoRemover>(); },
                       "ParametersInfoRemover")
        .addMethodPass([] { return std::make_unique<ExceptionInfoRemover>(); },
                       "ExceptionInfoRemover")
        .addClassPass([] { return std::make_unique<SignatureRemover>(); },
                      "SignatureRemover");
}

//对齐 Java AsmOptimizePhases.allIllegalMetaInfoRemoveManager：
//只接入 legacy manager 中列出的 5 个 pass；UnknownAttributeCleaner /
//IllegalLocalVariableInfoRemover 可按名称单独调用。
static void addIllegalMetaInfoRemovePhase(PipelineBuilder& builder)
{
    builder
        .addClassPass([] { return std::make_unique<IllegalAnnotationRemover>(); },
                      "IllegalAnnotationRemover")
        .addClassPass([] { return std::make_unique<IllegalSignatureRemover>(); },
                      "IllegalSignatureRemover")
        .addClassPass(
            [] { return std::make_unique<IllegalTypeAnnotationRemover>(); },
            "IllegalTypeAnnotationRemover")
        .addClassPass([] { return std::make_unique<IllegalSourceInfoRemover>(); },
                      "IllegalSourceInfoRemover")
        .addMethodPass([] { return std::make_unique<IllegalVarargsRemover>(); },
                       "IllegalVarargsRemover");
}

}   //namespace

//── PipelineBuilder ──────────────────────────────────────────

PipelineBuilder& PipelineBuilder::addMethodPass(
    std::function<std::unique_ptr<AsmMethodPass>()> factory, std::string nameHint)
{
    methodFactories_.emplace_back(std::move(nameHint), std::move(factory));
    return *this;
}

PipelineBuilder& PipelineBuilder::addClassPass(
    std::function<std::unique_ptr<AsmClassPass>()> factory, std::string nameHint)
{
    classFactories_.emplace_back(std::move(nameHint), std::move(factory));
    return *this;
}

PipelineBuilder& PipelineBuilder::addGlobalPass(
    std::function<std::unique_ptr<AsmGlobalPass>()> factory, std::string nameHint)
{
    globalFactories_.emplace_back(std::move(nameHint), std::move(factory));
    return *this;
}

PipelineBuilder& PipelineBuilder::addPostClassPass(
    std::function<std::unique_ptr<AsmGlobalPass>()> factory, std::string nameHint)
{
    postClassFactories_.emplace_back(std::move(nameHint), std::move(factory));
    return *this;
}

std::vector<std::string> PipelineBuilder::methodPassNames() const
{
    std::vector<std::string> out;
    out.reserve(methodFactories_.size());
    for (const auto& [name, _] : methodFactories_) out.push_back(name);
    return out;
}

std::vector<std::string> PipelineBuilder::classPassNames() const
{
    std::vector<std::string> out;
    out.reserve(classFactories_.size());
    for (const auto& [name, _] : classFactories_) out.push_back(name);
    return out;
}

std::vector<std::string> PipelineBuilder::globalPassNames() const
{
    std::vector<std::string> out;
    out.reserve(globalFactories_.size());
    for (const auto& [name, _] : globalFactories_) out.push_back(name);
    return out;
}

std::vector<std::string> PipelineBuilder::postClassPassNames() const
{
    std::vector<std::string> out;
    out.reserve(postClassFactories_.size());
    for (const auto& [name, _] : postClassFactories_) out.push_back(name);
    return out;
}

MethodPassManager PipelineBuilder::buildMethodManager(PassRunStats* stats) const
{
    MethodPassManager mgr;
    for (std::size_t i = 0; i < methodFactories_.size(); ++i) {
        PassRunStats::Slot* slot = stats ? &stats->slots[i] : nullptr;
        mgr.addPass(observe(methodFactories_[i].second(), slot));
    }
    return mgr;
}

ClassPassManager PipelineBuilder::buildClassManager(MethodPredicate predicate,
                                                    const PassPipelineOptions& options,
                                                    PassRunStats*              stats) const
{
    ClassPassManager mgr;
    for (std::size_t i = 0; i < classFactories_.size(); ++i) {
        PassRunStats::Slot* slot = stats ? &stats->slots[methodFactories_.size() + i] : nullptr;
        mgr.addPass(observe(classFactories_[i].second(), slot));
    }
    //如果有 method pass，用 ClassToMethodPassAdaptor 包装（Adaptor 不再持 AM 引用，
    //运行时经 ClassAnalysisManager 上的 MethodAnalysisManagerProxy 获取）。
    if (!methodFactories_.empty()) {
        mgr.addPass(std::make_unique<ClassToMethodPassAdaptor>(
            buildMethodManager(stats), std::move(predicate), options));
    }
    return mgr;
}

GlobalPassManager PipelineBuilder::buildNestedPipeline(
    int classWriterFlags, jbytecode::core::ICommonSuperClassProvider* typeResolver,
    MethodPredicate predicate, ProgressSink* progress, const PassPipelineOptions& options,
    PassRunStats* stats) const
{
    //只读计数采集：stats 非空时先按注册序铺槽位（method → class →
    //global → post-class，与 --describe 展示序一致）；stats 为空时零开销。
    if (stats) {
        stats->slots.clear();
        const auto fill = [stats](const std::vector<std::string>& names, const char* scope) {
            for (const auto& name : names) {
                PassRunStats::Slot slot;
                slot.name  = name;
                slot.scope = scope;
                stats->slots.push_back(std::move(slot));
            }
        };
        fill(methodPassNames(), "method");
        fill(classPassNames(), "class");
        fill(globalPassNames(), "global");
        fill(postClassPassNames(), "post-class");
    }

    GlobalPassManager mgr;
    //输入净化门（issue ）：head 位置先于一切 pass（含自行 readClass 的容器级
    //global pass）校验每个 ClassResource 的 CP 引用语义。失败语义跟随容错档位：
    //perClassFaultIsolation 记 faultReport（sanitize_error）并剔除坏类，默认档位
    //fail-fast 抛出。非法 CP 引用在 reader 里本就产出垃圾值/触发递归栈溢出
    //（同族），必须在任何消费之前拒绝。
    mgr.addPass(std::make_unique<ClassFileSanitizeGate>(options));
    //global pass 直接加入
    for (std::size_t i = 0; i < globalFactories_.size(); ++i) {
        const size_t base        = methodFactories_.size() + classFactories_.size();
        PassRunStats::Slot* slot = stats ? &stats->slots[base + i] : nullptr;
        mgr.addPass(observe(globalFactories_[i].second(), slot));
    }
    //class + method pass 经 GlobalToClassPassAdaptor 包装（AM 在 run 内创建）。
    //注意：即使 classFactories_ 为空，有 method pass 时也需生成 adaptor
    //（否则 method pass 不会跑；class pass = 空 ClassPM，adaptor 仍触发 method pass）。
    //options 同一份透传给内外两级 adaptor：类级消费 perClassFaultIsolation，
    //方法级消费 perMethodFaultIsolation，诊断共用 faultReport（issue ）。
    ClassPassManager cpm = buildClassManager(std::move(predicate), options, stats);
    if (!cpm.empty()) {
        mgr.addPass(std::make_unique<GlobalToClassPassAdaptor>(std::move(cpm), classWriterFlags,
                                                               typeResolver, progress, options));
    }
    //post-class global pass 追加在 adaptor 之后，可观察 class pass 的修改结果。
    for (std::size_t i = 0; i < postClassFactories_.size(); ++i) {
        const size_t base = methodFactories_.size() + classFactories_.size() +
                            globalFactories_.size();
        PassRunStats::Slot* slot = stats ? &stats->slots[base + i] : nullptr;
        mgr.addPass(observe(postClassFactories_[i].second(), slot));
    }
    return mgr;
}

//── PassRegistry ─────────────────────────────────────────────

PassRegistry& PassRegistry::instance()
{
    static PassRegistry registry;
    return registry;
}

PassRegistry& PassRegistry::registerPipeline(const std::string& name,
                                             const std::string& description,
                                             PipelineFactory    factory,
                                             PassStability      stability)
{
    Entry entry;
    entry.meta = {name, description, stability};
    entry.factory = std::move(factory);
    entries_[name] = std::move(entry);
    return *this;
}

bool PassRegistry::hasPipeline(const std::string& name) const
{
    return entries_.find(name) != entries_.end();
}

PipelineBuilder PassRegistry::getPipeline(const std::string& name) const
{
    auto it = entries_.find(name);
    if (it == entries_.end()) return PipelineBuilder{};
    return it->second.factory();
}

const PassPipelineInfo* PassRegistry::pipelineInfo(const std::string& name) const
{
    auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second.meta;
}

std::vector<PassPipelineInfo> PassRegistry::listPipelines() const
{
    std::vector<PassPipelineInfo> result;
    result.reserve(entries_.size());
    for (const auto& [name, entry] : entries_) {
        result.push_back(entry.meta);
    }
    //固定顺序便于 `mjt opt --list-pipelines` 用于发布 smoke test 和文档示例。
    std::sort(result.begin(),
              result.end(),
              [](const PassPipelineInfo& a, const PassPipelineInfo& b) { return a.name < b.name; });
    return result;
}

//── registerBuiltinPipelines ─────────────────────────────────

ca::usize registerBuiltinPipelines()
{
    auto& reg = PassRegistry::instance();
    if (reg.hasPipeline("default-optimize")) return 0;   //幂等，避免重复注册

    reg.registerPipeline(
        "phase.fix-pattern-replace",
        "mbase.opt.pipeline.phase.fix_pattern_replace",
        []() { return buildFixPatternReplacePhase(); },
        PassStability::OptIn);

    reg.registerPipeline("phase.simple-constant-fold",
                         "mbase.opt.pipeline.phase.simple_constant_fold",
                         []() { return buildSimpleConstantFoldPhase(); },
                         PassStability::OptIn);

    reg.registerPipeline(
        "phase.control-flow-simplify",
        "mbase.opt.pipeline.phase.control_flow_simplify",
        []() { return buildControlFlowSimplifyPhase(); },
        PassStability::OptIn);

    reg.registerPipeline(
        "phase.peephole-garbage-clean",
        "mbase.opt.pipeline.phase.peephole_garbage_clean",
        []() { return buildPeepholeGarbageCleanPhase(); },
        PassStability::OptIn);

    reg.registerPipeline(
        "default-optimize",
        "mbase.opt.pipeline.default_optimize",
        []() { return buildDefaultOptimizePipeline(); },
        PassStability::Default);

    reg.registerPipeline("phase.meta-info-remove",
                         "mbase.opt.pipeline.phase.meta_info_remove",
                         []() {
                             PipelineBuilder builder;
                             addMetaInfoRemovePhase(builder);
                             return builder;
                         },
                         PassStability::OptIn);

    reg.registerPipeline(
        "phase.illegal-meta-info-remove",
        "mbase.opt.pipeline.phase.illegal_meta_info_remove",
        []() {
            PipelineBuilder builder;
            addIllegalMetaInfoRemovePhase(builder);
            return builder;
        },
        PassStability::OptIn);

    //返回实际注册的流水线数，避免硬编码常量随新增 pipeline 漂移（对齐 registerBuiltinPasses）。
    return reg.listPipelines().size();
}

}   //namespace mjt::base
