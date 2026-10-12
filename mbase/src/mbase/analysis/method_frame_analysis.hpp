#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "jbytecode/tree/nodes.hpp"
#include "libca/str/utf8_string.hpp"
#include "mbase/asm/analyzer/dataflow_analyzer.hpp"
#include "mbase/asm/analyzer/original_source_interpreter.hpp"
#include "mbase/asm/analyzer/original_source_value.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

namespace detail {

///@brief 分析期临时容量守卫：analyze 前抬升 maxStack、析构即恢复原值。
///
///与各 pass 自持的 `AnalyzerCapacityGuard` 不同，本守卫**永不 commit**：帧分析是纯只读
///分析，不得对 IR 留下任何副作用。抬升只为让 `Analyzer` 有足够栈深完成定点分析，跑完
///（无论正常返回还是异常展开）立即恢复。这样即便调用方未自建 guard，`getResult` 也能在
///`maxStack<=0` 的畸形方法上成功求值，不会把 `ok=false` 污染进缓存——否则该失败结果会被
///后续正确建了 guard 的 pass 读到（顺序依赖的 stale bug）。
class TransientCapacityScope
{
public:
    explicit TransientCapacityScope(jbytecode::tree::MethodNode& method)
        : method_(method)
        , originalMaxStack_(method.maxStack)
    {
        if (method_.maxStack <= 0 && !method_.instructions.isEmpty()) {
            method_.maxStack = (std::max)(8, method_.instructions.size());
        }
    }
    ~TransientCapacityScope() { method_.maxStack = originalMaxStack_; }

    TransientCapacityScope(const TransientCapacityScope&)            = delete;
    TransientCapacityScope& operator=(const TransientCapacityScope&) = delete;

private:
    jbytecode::tree::MethodNode& method_;
    int                          originalMaxStack_;
};

}   //namespace detail

///@brief 通用 method 级帧数据流分析结果（对任意 `Interpreter<ValueT>` 族参数化）。
///
///@tparam InterpreterT 具体解释器类型（须可默认构造，如 OriginalSourceInterpreter）。
///@tparam ValueT 该解释器对应的 value 类型（`InterpreterT` 派生自 `Interpreter<ValueT>`）。
///
///@par 所有权（关键）
///`analyzeFrames` 产出的 `DataflowFrames` 快照自持帧所有权，Result 以
///unique_ptr 持有 interpreter、以 snapshot 持有帧，`frames` 裸指针在 Result
///生命周期内保持有效；unique_ptr 保证 Result 被移动（AM 缓存 emplace/返回
///引用时）后堆地址稳定，interpreter/frame 指针不悬空。
template<class InterpreterT, class ValueT>
struct FrameAnalysisResult
{
    ///analyze 是否成功（false = 方法为空或数据流引擎 fail-open，调用方应跳过）。
    bool ok = false;
    ///每条指令对应的帧（与 `MethodNode::instructions` 顺序一致，可能含 nullptr 表示不可达）。
    std::vector<jbytecode::analysis::Frame<ValueT>*> frames;

    FrameAnalysisResult()                                      = default;
    FrameAnalysisResult(FrameAnalysisResult&&)                 = default;
    FrameAnalysisResult& operator=(FrameAnalysisResult&&)      = default;
    FrameAnalysisResult(const FrameAnalysisResult&)            = delete;
    FrameAnalysisResult& operator=(const FrameAnalysisResult&) = delete;

    ///interpreter 持有到 Result 析构；帧所有权在 snapshot 内，frames 是其借用视图。
    std::unique_ptr<InterpreterT>   interpreter;
    DataflowFrames<ValueT>          snapshot;
};

///@brief 通用 method 级帧数据流分析（AM 分析契约：`using Result` + `run(MethodNode&, AM&)`）。
///
///把「建 interpreter → `analyzeFrames`（DataflowAnalyzer 引擎）→ fail-open 判定」
///这套各 pass 重复的样板收敛为单一模板。任意 `Interpreter<V>` 族只需一行别名即可
///获得同款生命周期安全的帧分析：既可注册进 AM 走缓存（多消费者场景），也可当一次性
///helper 直接构造（单消费者/pass 内定点循环，缓存无益甚至读脏的场景）。
///
///@par 引擎
///统一走 `DataflowAnalyzer`（`analyzeFrames` 入口，JSR/RET、迭代超限等不可恢复
///错误 fail-open 返回空帧），不再依赖旧 `jbytecode::analysis::Analyzer`。
///`invokedynamic` 正常分析（保守未知返回值）。
///
///@par 失效
///作为 AM 分析注册时，失效由 PassManager 依 PreservedAnalyses 驱动：任一 method pass 返回非
///`all`（即改写指令）后，结果自动从 AM 缓存删除，下次 getResult 重算，不会读到 stale 帧。
///
///@par maxStack 契约（自管容量）
///本分析对 `MethodNode::maxStack` 无净副作用：`run` 内部经 `detail::TransientCapacityScope`
///在 analyze 前临时抬升 maxStack、跑完（正常或异常）立即恢复。因此调用方**无需**先建
///`AnalyzerCapacityGuard` 即可安全 `getResult`；即便在 `maxStack<=0` 的畸形方法上也能求值成功，
///不会把 `ok=false` 污染进缓存。pass 侧的 `AnalyzerCapacityGuard` 职责因此收窄为**持久化
///maxStack 修复**：仅当 pass 改写了畸形方法并需输出合法容量时才 commit，这与帧缓存彻底解耦。
template<class InterpreterT, class ValueT>
struct FrameAnalysis
{
    using Result = FrameAnalysisResult<InterpreterT, ValueT>;

    Result run(jbytecode::tree::MethodNode& method, MethodAnalysisManager& am)
    {
        (void)am;
        Result result;

        //空方法：无帧，标记失败让调用方 fail-open（与迁移前 instructions.isEmpty 早退一致）。
        if (method.instructions.isEmpty()) return result;

        result.interpreter = std::make_unique<InterpreterT>();

        {
            //分析自管容量：临时抬升 maxStack 供引擎定点分析，出块（正常/异常）即恢复。
            detail::TransientCapacityScope capacity(method);
            //owner 名不参与帧溯源判定，沿用各 pass 迁移前使用的占位 "T"。
            result.snapshot = analyzeFrames<ValueT>(result.interpreter.get(),
                                                    ca::str::ZUtf8StringRef::from_static("T"),
                                                    method);
        }

        //fail-open 判定：引擎拒斥（JSR/RET、预算超限等）或畸形字节码返回空快照。
        result.ok = !result.snapshot.empty();
        result.frames = result.snapshot.frames();
        return result;
    }
};

///@brief Frame<OriginalSourceValue> 的规范别名（原本在多个 pass TU 内各自定义）。
using OriginalSourceFrame = jbytecode::analysis::Frame<OriginalSourceValue>;

///@brief 原始来源(OriginalSourceValue)帧分析——通用 FrameAnalysis 的具体特化别名。
///
///供 dead-code / pop 清理 / stack 展开等多个 method pass 通过
///`am.getResult<OriginalSourceFrameAnalysis>` 复用同一份缓存的帧，取代各 pass 自建
///analyzer 重复重算。这是目前唯一注册进 AM 走缓存的帧分析（多消费者、不改 unit 就重算）。
using OriginalSourceFrameAnalysis = FrameAnalysis<OriginalSourceInterpreter, OriginalSourceValue>;
using OriginalSourceFrameAnalysisResult =
    FrameAnalysisResult<OriginalSourceInterpreter, OriginalSourceValue>;

///@brief 把所有内置 method 级分析注册进给定 MethodAnalysisManager（幂等）。
///
///必须在任何 method pass 调用 `am.getResult<...>` 之前调用，否则 `getResult` 抛
///`std::logic_error`。凡是构造真实 method pipeline AM 的入口都应调用它
///（runAsmPipeline / GlobalToClassPassAdaptor）。
///
///@note 优先用 `makeMethodAnalysisManager` 工厂构造 AM；仅当需要向一个既有 AM 追加注册
///（如测试）时才直接调用本函数。
void registerBuiltinMethodAnalyses(MethodAnalysisManager& mam);

///@brief 构造一个已注册全部内置 method 级分析的 MethodAnalysisManager（推荐入口）。
///
///把「构造 + registerBuiltinMethodAnalyses」两步合一，消灭「手建 AM 却忘了注册内置分析」
///这一类 bug——漏注册时 method pass 的 `getResult` 会抛 `std::logic_error`。凡是需要
///method 级 AM 的入口（runAsmPipeline / GlobalToClassPassAdaptor / method_inliner 等）都
///应经此工厂构造，而非裸 `MethodAnalysisManager mam(arena)`。
///
///@param arena 作业级 arena，AM 借用（不持有所有权），调用方须保证其存活覆盖 AM 生命周期。
MethodAnalysisManager makeMethodAnalysisManager(ca::str::Utf8StringArena& arena);

}   //namespace mjt::base
