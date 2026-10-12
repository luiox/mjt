#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "mbase/spi/pass_manager.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

///pass/pipeline 的发布稳定性与默认链状态。
enum class PassStability
{
    Default,      ///< 属于默认能力，可作为正式发布承诺。
    OptIn,        ///< 只能显式选择，不计入默认能力。
    Experimental, ///< 实验性能力，不属于发布承诺。
    Stub,         ///< 空实现或兼容占位，不计入完成数量。
};

//前向声明：buildNestedPipeline 可选注入进度 sink（issue ）。
class ProgressSink;

///@brief per-pass 只读计数采集器（dry-run）。
///
///由调用方持有并传入 buildNestedPipeline（以及 buildMethodManager/buildClassManager）；
///指针为空时零包装、零行为差异。采集纯只读：每个 pass 实例被一层薄装饰器包住，
///在 pass run 前后记时间戳、按返回的 PreservedAnalyses 记 changed，异常路径记
///failed 后原样重抛——pass 的执行路径、参数与返回值一律透传，不感知采集。
struct PassRunStats
{
    ///单个 pass 槽位（注册序 = 展示序，scope 内 index 唯一）。
    struct Slot
    {
        std::string name;   ///< 注册时的 nameHint；空串表示未命名槽位
        const char* scope = nullptr;   ///< "method" / "class" / "global" / "post-class"
        ca::u64 processed  = 0;   ///< run 调用次数（处理单元数：方法/类/容器）
        ca::u64 changed    = 0;   ///< 其中伴随 IR 改动（PreservedAnalyses::changed）的次数
        ca::u64 failed     = 0;   ///< 其中抛出异常的次数（异常继续向上传播，不吞）
        double  durationMs = 0.0; ///< run 累计耗时（含失败调用）
    };

    ///按 method → class → global → post-class 注册序排列（与 --describe 展示序一致）。
    ///每次 build* 调用重建：槽位顺序与数量精确对应当次构建的流水线。
    std::vector<Slot> slots;
};

///@brief 流水线构建器：fluent 地组装三级 pass 序列，产出嵌套的 PassManager。
///
///三级 pass 分别用 addMethodPass/addClassPass/addGlobalPass 注册。
///buildNestedPipeline 构建带 Adaptor 的完整嵌套 GlobalPassManager：
///@code
///GlobalPM [
///(global passes)
///GlobalToClassPassAdaptor( ClassPM [
///(class passes)
///ClassToMethodPassAdaptor( MethodPM [ (method passes) ] )
///] )
///]
///@endcode
///
///每个 add*Pass 接受可选的 `nameHint`，用于 `--describe` 等元信息查询。nameHint
///约定为 PassNameRegistry 中注册的真实类名（如 "NopRemover"）；留空时对应
///`*PassNames` 槽位返回空串，不影响流水线执行。
///
///@warning 非线程安全。
class PipelineBuilder
{
public:
    ///添加一个方法级 pass（每次调用都会 new 一个新实例，保证多次构建互不影响）。
    PipelineBuilder& addMethodPass(std::function<std::unique_ptr<AsmMethodPass>()> factory,
                                   std::string nameHint = {});

    ///添加一个类级 pass。
    PipelineBuilder& addClassPass(std::function<std::unique_ptr<AsmClassPass>()> factory,
                                  std::string nameHint = {});

    ///添加一个全局级 pass。
    PipelineBuilder& addGlobalPass(std::function<std::unique_ptr<AsmGlobalPass>()> factory,
                                   std::string nameHint = {});

    ///@brief 添加一个「class pass 之后」的全局级 pass。
    ///
    ///buildNestedPipeline 把全部 class/method pass 打包进末尾的
    ///GlobalToClassPassAdaptor；本类 pass 追加在 adaptor 之后执行，用于需要
    ///观察 class pass 修改结果的下游（如 detector 的 after 快照）。
    PipelineBuilder& addPostClassPass(std::function<std::unique_ptr<AsmGlobalPass>()> factory,
                                      std::string nameHint = {});

    ///产出方法级 PassManager（每次调用都重新构造所有 pass 实例）。
    ///@param stats 可选只读计数采集器（dry-run）；nullptr 零包装、行为不变。
    ///非空时本调用仅按注册序**取用** slots 元引用，不做 sizing/清空
    ///——槽位的铺设只在 buildNestedPipeline 内完成；本方法应作为
    ///buildNestedPipeline 的一部分间接使用，绕过它直接传入未铺槽
    ///的 stats 属误用（越界访问休眠面）。
    MethodPassManager buildMethodManager(PassRunStats* stats = nullptr) const;

    ///产出类级 PassManager。
    ///@param predicate 可选方法过滤谓词（CLI -m），透传给 ClassToMethodPassAdaptor。
    ///@param options 可选容错档位（issue ），透传给两个 Adaptor。
    ///@param stats 可选只读计数采集器（dry-run）；nullptr 零包装、行为不变。
    ///非空时同 buildMethodManager：仅取用 slots 元引用，铺槽在
    ///buildNestedPipeline；直接独立使用属误用。
    ClassPassManager buildClassManager(MethodPredicate predicate = nullptr,
                                       const PassPipelineOptions& options = {},
                                       PassRunStats*              stats   = nullptr) const;
    ///@brief 构建完整嵌套的全局流水线（带 Adaptor）。
    ///
    ///method pass → ClassToMethodPassAdaptor → ClassPM → GlobalToClassPassAdaptor
    ///→ GlobalPM（global pass 附加在其后）。
    ///
    ///@param classWriterFlags ClassWriter flags（COMPUTE_MAXS | COMPUTE_FRAMES，默认 0）
    ///@param typeResolver 可选 ICommonSuperClassProvider
    ///@param predicate 可选方法过滤谓词（CLI -m）；nullptr 不过滤
    ///@param progress 可选进度 sink（issue ）；nullptr 无进度反馈
    ///@param options 可选容错档位与诊断收集（issue ）；默认全关，语义不变。
    ///faultReport 指针由调用方持有并保证存活至流水线 run 结束。
    ///@param stats 可选只读计数采集器（dry-run）；nullptr 零包装、行为
    ///不变。非空时每个 pass 实例包一层透传装饰器（run 前后计时、
    ///按返回的 PreservedAnalyses 记 changed、异常记 failed 后原样
    ///重抛），槽位按 method → class → global → post-class 注册序
    ///填入 stats->slots（先清空重建，调用方持有并保证存活至 run）。
    ///**一次构建对应一个专用 stats**：本调用先 clear slots，
    ///复用同一 stats 二次构建会令旧管线持有的 Slot* 全部悬垂。
    GlobalPassManager buildNestedPipeline(
        int                                         classWriterFlags = 0,
        jbytecode::core::ICommonSuperClassProvider* typeResolver     = nullptr,
        MethodPredicate predicate = nullptr, ProgressSink* progress = nullptr,
        const PassPipelineOptions& options = {}, PassRunStats* stats = nullptr) const;

    bool empty() const
    {
        return methodFactories_.empty() && classFactories_.empty() && globalFactories_.empty() &&
               postClassFactories_.empty();
    }
    ca::usize methodPassCount() const { return methodFactories_.size(); }
    ca::usize classPassCount() const { return classFactories_.size(); }
    ca::usize globalPassCount() const { return globalFactories_.size(); }
    ca::usize postClassPassCount() const { return postClassFactories_.size(); }

    ///@brief 返回方法级 pass 的 nameHint 列表（按注册顺序，与 methodPassCount 对齐）。
    ///
    ///未传 nameHint 的槽位返回空串，调用方需自行处理。
    std::vector<std::string> methodPassNames() const;
    ///@brief 返回类级 pass 的 nameHint 列表。
    std::vector<std::string> classPassNames() const;
    ///@brief 返回全局级 pass 的 nameHint 列表。
    std::vector<std::string> globalPassNames() const;
    ///@brief 返回 post-class 段 pass 的 nameHint 列表。
    std::vector<std::string> postClassPassNames() const;

private:
    std::vector<std::pair<std::string, std::function<std::unique_ptr<AsmMethodPass>()>>>
        methodFactories_;
    std::vector<std::pair<std::string, std::function<std::unique_ptr<AsmClassPass>()>>>
                                          classFactories_;
    std::vector<std::pair<std::string, std::function<std::unique_ptr<AsmGlobalPass>()>>>
        globalFactories_;
    std::vector<std::pair<std::string, std::function<std::unique_ptr<AsmGlobalPass>()>>>
        postClassFactories_;
};

///@brief 已注册流水线的元信息。
struct PassPipelineInfo
{
    std::string name;
    std::string description;
    PassStability stability = PassStability::OptIn;
};

///@brief Pass 流水线注册中心：名字 → 配置（工厂）。
///
///对标 Java AsmOptimizePhases 的意图（常用 pass 组合集中配置），但用「名字→可查询
///配置」解决 Java 版工厂方法无法跨二进制复用的问题。避开 Java SPIRegistry 的失败
///（registerDefaults 是 no-op、零调用）：注册通过显式 registerBuiltinPipelines
///bootstrap，查询点在 CLI 入口。
///
///@warning 非线程安全。注册和查询应在同一线程的初始化阶段完成。
class PassRegistry
{
public:
    using PipelineFactory = std::function<PipelineBuilder()>;

    static PassRegistry& instance();

    ///@brief 注册一个命名流水线。同名重复注册覆盖旧值。
    ///@return *this 便于链式注册。
    PassRegistry& registerPipeline(const std::string& name, const std::string& description,
                                   PipelineFactory factory,
                                   PassStability stability = PassStability::OptIn);

    ///查询是否已注册某名字。
    bool hasPipeline(const std::string& name) const;

    ///取某名字的 PipelineBuilder（每次都重新调用 factory，得到全新 pass 实例）。
    ///未注册返回空的 PipelineBuilder。
    PipelineBuilder getPipeline(const std::string& name) const;

    ///@brief 查询单个 pipeline 的元信息（name/description/stability）。
    ///
    ///供 `--describe` 等单点查询使用，避免线性扫描 `listPipelines`。返回指针
    ///指向 registry 内部数据，仅在 registry 未重新注册同名 pipeline 期间有效；
    ///未注册返回 `nullptr`。
    const PassPipelineInfo* pipelineInfo(const std::string& name) const;

    ///列出所有已注册流水线的元信息（按 name 排序，供 CLI 稳定输出）。
    std::vector<PassPipelineInfo> listPipelines() const;

private:
    PassRegistry() = default;
    struct Entry
    {
        PassPipelineInfo meta;
        PipelineFactory  factory;
    };
    std::unordered_map<std::string, Entry> entries_;
};

///@brief 注册内置流水线（default-optimize 等）。程序入口调用一次。
///
///把散落在 mjt-optimizer/createMethodManager 与 mjt-pass/createOptMethodManager
///的同一份 pass 组合集中到这里注册一次，两处入口都查 "default-optimize"。
///
///@return 已注册的流水线数量（便于启动日志）。
ca::usize registerBuiltinPipelines();

}   //namespace mjt::base
