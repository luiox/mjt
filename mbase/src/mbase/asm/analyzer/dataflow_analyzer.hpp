#pragma once

///@defgroup dataflow_analyzer 分支敏感数据流分析通用引擎
///
///`DataflowAnalyzer<V>` 是一个模板化的栈帧数据流分析器，对 `MethodNode`
///产出每条指令的入口帧，并支持分支敏感的可达性修剪。它把原本在
///`ReAnalyzer`、`CodeAnalyzer`、`JumpPredictingAnalyzer`、
///`NullabilityAnalyzer` 中各自重复实现的 worklist / merge / 异常边 / 分支
///后继选择逻辑收敛到单一引擎；value 域特定的语义（常量折叠、null 细化、
///引用相等）通过 `Interpreter<V>` 与 `BranchPredictor<V>` 两个 hook 注入。
///
///## 设计
///
///- 引擎只负责 mechanics：worklist、不动点 merge、异常处理器边、分支
///后继选择、迭代预算。它不内联任何操作码语义，统一调用
///`Frame<V>::execute(insn, interpreter)`，避免重复 330 行操作码 switch。
///- 标签查找使用预构建的 `unordered_map<Label*, int>` 索引，避免旧分析器
///里 O(n) 的 `findLabelNode` 线性扫描。
///- `BranchPredictor<V>` 是可选的分支预测策略：当条件操作码的操作数已知
///时，predictor 返回 `true`/`false` 让引擎只入队可行后继；未知时返回
///`nullopt`，引擎按 JVM 标准保守地入队全部后继。
///- 异常边保守传播：处理器入口帧清空操作数栈并压入类型化 throwable 值，
///同时合并指令前与指令后局部状态，匹配共享分析器模型，避免在抛出
///操作码附近出现不合理的局部丢失。
///
///## 范围
///
///明确拒绝旧式 `JSR`/`RET` 子例程（`Frame<V>::execute` 对它们只有半套模型，
///且新版 classfile 已不再生成），与现有 `ReAnalyzer` 行为一致；`analyzeFrames`
///入口按 fail-open 返回空帧。需要处理这些操作码的通行证应回退到
///`JumpPredictingAnalyzer`、非修剪分析或跳过该方法。
///
///`invokedynamic` **不再拒绝**：`Frame<V>::execute` 已按描述符弹参与
///`naryOperation` 派发（见 `jbytecode/analysis/frame_execute.inl` 的
///`executeInvokeInsn`），值域只要实现 `naryOperation` 就能被分析。
///此前引擎无条件抛错，使任何含 lambda / 字符串拼接 / 记录类的方法都
///无法分析 —— 而这类方法在现代字节码里几乎是常态。
///
///## 可观测性
///
///`DataflowOptions::tracer` 可挂 `DataflowTracer::recording` 实例，
///事件包含 merge（下标 + 是否变更）、reject（原因：JSR/RET/预算超限/
///分支目标缺失）与最终 summary（指令数/可达数/迭代数），用于调试不收敛
///与覆盖缺失问题；关闭时（默认 `nullptr`）零开销。

#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "jbytecode/analysis/exceptions.hpp"
#include "jbytecode/analysis/interpreter.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/core/type.hpp"
#include "jbytecode/tree/insn_type_traits.hpp"
#include "jbytecode/tree/nodes.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

template<typename V>
class DataflowAnalyzer;

///@brief 可选的分支预测策略。
///
///引擎在处理条件跳转、`IFNULL`/`IFNONNULL`、`*SWITCH` 等指令时会调用
///`testBranch` 询问 predictor：当前指令前帧上的操作数值是否已知到足以
///判定分支走向。返回 `nullopt` 表示未知，引擎按 JVM 标准入队全部后继；
///返回 `true`/`false` 表示仅入队对应后继（true = 走 jump 目标，
///false = 走 fall-through）。`Instruction` 参数方便 predictor 复用
///已知的指令元信息（如 `TableSwitchInsnNode` 的边界）。
template<typename V>
class BranchPredictor
{
public:
    virtual ~BranchPredictor() = default;

    ///@brief 判定单操作数条件跳转（IFEQ/IFNE/.../IFNULL/IFNONNULL）。
    ///
    ///@param opcode 条件操作码。
    ///@param insn 触发判定的指令节点。
    ///@param beforeFrame 指令执行前的帧；predictor 可从中读取操作数。
    ///@return 分支走向，或 `nullopt` 表示未知。
    virtual std::optional<bool> testUnaryBranch(int                                  opcode,
                                                const jbytecode::tree::AbstractInsnNode* insn,
                                                const jbytecode::analysis::Frame<V>& beforeFrame)
    {
        (void)opcode;
        (void)insn;
        (void)beforeFrame;
        return std::nullopt;
    }

    ///@brief 判定双操作数条件跳转（IF_ICMPEQ/.../IF_ACMPEQ/IF_ACMPNE）。
    virtual std::optional<bool> testBinaryBranch(int                                  opcode,
                                                 const jbytecode::tree::AbstractInsnNode* insn,
                                                 const jbytecode::analysis::Frame<V>& beforeFrame)
    {
        (void)opcode;
        (void)insn;
        (void)beforeFrame;
        return std::nullopt;
    }

    ///@brief 判定 `TABLESWITCH` 的目标。
    ///
    ///返回的 `int` 是目标在 `TableSwitchInsnNode::labels` 中的下标，
    ///或 `-1` 表示走 `dflt`。`nullopt` 表示未知。
    virtual std::optional<int> testTableSwitch(const jbytecode::tree::AbstractInsnNode* insn,
                                               const jbytecode::analysis::Frame<V>& beforeFrame)
    {
        (void)insn;
        (void)beforeFrame;
        return std::nullopt;
    }

    ///@brief 判定 `LOOKUPSWITCH` 的目标。
    ///
    ///返回的 `int` 是匹配 key 在 `LookupSwitchInsnNode::keys` 中的下标；
    ///返回 `-1` 表示走 `dflt`；`nullopt` 表示未知。
    virtual std::optional<int> testLookupSwitch(const jbytecode::tree::AbstractInsnNode* insn,
                                                const jbytecode::analysis::Frame<V>& beforeFrame)
    {
        (void)insn;
        (void)beforeFrame;
        return std::nullopt;
    }
};

///@brief 数据流跟踪事件类型。
enum class DataflowTracePhase
{
    MERGE,    ///< 某下标收到一次合并；aux = 帧是否发生变更（0/1）。
    REJECT,   ///< 不可恢复数据流错误；aux = DataflowRejectReason。
    SUMMARY   ///< 分析完成；insnIndex = 指令总数，aux = 可达指令数，iterations 单列。
};

///@brief `DataflowTracePhase::REJECT` 的原因码。
enum class DataflowRejectReason
{
    JSR,             ///< 旧式 JSR 子例程，引擎明确拒绝。
    RET,             ///< 旧式 RET 子例程，引擎明确拒绝。
    BUDGET_EXCEEDED, ///< worklist 迭代超出 maxIterations 预算。
    MISSING_LABEL    ///< 跳转目标标签不在指令列表中（畸形字节码）。
};

///@brief 一条结构化跟踪事件。
///
///全部为整型字段，不持有字符串——tracer 关闭或快照未被读取时无格式化开销。
struct DataflowTraceEvent
{
    DataflowTracePhase phase;
    int                insnIndex;   ///< MERGE/REJECT 为目标（被合并/被拒绝）指令下标；SUMMARY 为指令总数。
    int                aux;         ///< MERGE: 变更 0/1；REJECT: DataflowRejectReason；SUMMARY: 可达指令数。
    int                iterations;  ///< 仅 SUMMARY 有意义（worklist 出队迭代数），其余为 -1。
};

///@brief 一次分析的事件流快照（`snapshot` 产出，借用 tracer 的事件副本）。
class DataflowTrace
{
    std::vector<DataflowTraceEvent> events_;

public:
    explicit DataflowTrace(std::vector<DataflowTraceEvent> events)
        : events_(std::move(events))
    {}

    const std::vector<DataflowTraceEvent>& events() const { return events_; }

    ///@brief 人类可读的逐事件行文本，一行一事件。
    std::string toPrettyString() const
    {
        std::string sb;
        for (const auto& e : events_) {
            switch (e.phase) {
            case DataflowTracePhase::MERGE:
                sb += "MERGE idx=" + std::to_string(e.insnIndex) +
                      " changed=" + std::to_string(e.aux) + '\n';
                break;
            case DataflowTracePhase::REJECT:
                sb += "REJECT idx=" + std::to_string(e.insnIndex) + " reason=";
                switch (static_cast<DataflowRejectReason>(e.aux)) {
                case DataflowRejectReason::JSR: sb += "JSR"; break;
                case DataflowRejectReason::RET: sb += "RET"; break;
                case DataflowRejectReason::BUDGET_EXCEEDED: sb += "BUDGET_EXCEEDED"; break;
                case DataflowRejectReason::MISSING_LABEL: sb += "MISSING_LABEL"; break;
                }
                sb += '\n';
                break;
            case DataflowTracePhase::SUMMARY:
                sb += "SUMMARY insns=" + std::to_string(e.insnIndex) +
                      " reachable=" + std::to_string(e.aux) +
                      " iterations=" + std::to_string(e.iterations) + '\n';
                break;
            }
        }
        return sb;
    }
};

///@brief 结构化事件收集器（镜像 `tmatcher::MatchTracer` 的 noop/recording 形态）。
///
///事件按分析调用累积；`options.tracer = &tracer` 借用（非持有），tracer 须
///活过 analyze 调用。关闭态（`noop`）所有方法立即返回，零开销。
class DataflowTracer
{
    bool                            enabled_;
    std::vector<DataflowTraceEvent> events_;

public:
    static DataflowTracer noop() { return DataflowTracer(false); }
    static DataflowTracer recording() { return DataflowTracer(true); }

    void merge(int index, bool changed)
    {
        if (!enabled_) return;
        events_.push_back({DataflowTracePhase::MERGE, index, changed ? 1 : 0, -1});
    }

    void reject(int index, DataflowRejectReason reason)
    {
        if (!enabled_) return;
        events_.push_back(
            {DataflowTracePhase::REJECT, index, static_cast<int>(reason), -1});
    }

    void summary(int instructions, int reachable, int iterations)
    {
        if (!enabled_) return;
        events_.push_back({DataflowTracePhase::SUMMARY, instructions, reachable, iterations});
    }

    DataflowTrace snapshot() const { return DataflowTrace(events_); }

private:
    explicit DataflowTracer(bool enabled)
        : enabled_(enabled)
    {}
};

///@brief 引擎行为可调项。
template<typename V>
struct DataflowOptions
{
    ///@brief worklist 最大出队次数，防止畸形字节码让分析永不终止。
    ///
    ///超限时 `analyze` 抛 `AnalyzerException`。默认 10 万，对正常方法
    ///远大于实际需要。
    int                 maxIterations              = 100000;
    ///@brief 是否向异常处理器入口传播帧（清栈 + 压 throwable，合并指令前后 locals）。
    ///
    ///默认开启，与 ASM Analyzer 一致。关闭后处理器入口帧为空，分支修剪
    ///仍按 JVM 标准走控制流，但可达性会更乐观（忽略 throw 路径）。
    bool                propagateExceptionHandlers = true;
    ///@brief 可选分支预测器；`nullptr` 等价于保守走全后继。
    BranchPredictor<V>* predictor = nullptr;
    ///@brief 可选事件收集器；`nullptr`（默认）关闭，事件路径零开销。
    DataflowTracer*     tracer = nullptr;
};

///@brief `DataflowAnalyzer` 一次 `analyze` 调用的输出快照。
///
///非空帧表示对应指令可达，且为进入该指令前的入口状态。结果拥有这些帧，
///调用者可在 analyzer 实例销毁后继续持有该表。指令指针是从被分析
///`MethodNode` 借用的，在字节码变更后应视为无效。
template<typename V>
class DataflowResult
{
public:
    using FrameType = jbytecode::analysis::Frame<V>;

    ///@brief 被分析方法的指令总数（含 LABEL/LINE/FRAME 等伪指令）。
    int size() const { return static_cast<int>(frames_.size()); }

    ///@brief 本次分析 worklist 出队迭代次数（收敛开销的直接观测指标）。
    int iterations() const { return iterations_; }

    ///@brief 第 `index` 条指令是否可达。
    ///
    ///越界索引返回 false。可达性的唯一判据是入口帧是否非空。
    bool reachable(int index) const
    {
        return index >= 0 && index < static_cast<int>(frames_.size()) && frames_[index] != nullptr;
    }

    ///@brief 取第 `index` 条指令的入口帧；不可达或越界时返回 `nullptr`。
    ///
    ///帧描述进入该指令前的 locals/operand stack/return 状态，所有权归结果，
    ///调用者不得释放。
    const FrameType* frameAt(int index) const
    {
        if (!reachable(index)) return nullptr;
        return frames_[index].get();
    }

    ///@brief 非 const 重载：供把帧作为 `Frame<V>*` 裸指针继续传递的调用方使用
    ///（形态与旧 `jbytecode::analysis::Analyzer::analyze` 返回值对齐）。
    FrameType* frameAt(int index)
    {
        if (!reachable(index)) return nullptr;
        return frames_[index].get();
    }

    ///@brief 取第 `index` 条指令节点；越界返回 `nullptr`。
    ///
    ///指针借用自被分析的 `MethodNode`，指令列表变更后失效。
    jbytecode::tree::AbstractInsnNode* instructionAt(int index) const
    {
        if (index < 0 || index >= static_cast<int>(instructions_.size())) return nullptr;
        return instructions_[index];
    }

    ///@brief 反查指令节点在结果中的下标；未找到返回 -1。
    int indexOf(jbytecode::tree::AbstractInsnNode* insn) const
    {
        for (int i = 0; i < static_cast<int>(instructions_.size()); ++i) {
            if (instructions_[i] == insn) return i;
        }
        return -1;
    }

    ///@brief 列出所有可达指令的下标（指令列表顺序）。
    ///
    ///供需要遍历修剪后字节码、又不想在每个下标处重新检查 `reachable` 的
    ///通行证使用。
    std::vector<int> reachableInstructionIndices() const
    {
        std::vector<int> indices;
        for (int i = 0; i < static_cast<int>(frames_.size()); ++i) {
            if (frames_[i]) indices.push_back(i);
        }
        return indices;
    }

    ///@brief 列出所有不可达指令的下标（指令列表顺序）。
    ///
    ///含 LABEL/LINE/FRAME 等伪指令，因结果镜像原始指令列表。
    std::vector<int> unreachableInstructionIndices() const
    {
        std::vector<int> indices;
        for (int i = 0; i < static_cast<int>(frames_.size()); ++i) {
            if (!frames_[i]) indices.push_back(i);
        }
        return indices;
    }

    ///@brief 列出所有可达指令的节点指针（指令列表顺序）。
    ///
    ///与 `reachableInstructionIndices` 等价，但直接给出节点句柄；指针借用
    ///自被分析的 `MethodNode`，与 `instructionAt` 同生命周期。
    std::vector<jbytecode::tree::AbstractInsnNode*> reachableInstructions() const
    {
        std::vector<jbytecode::tree::AbstractInsnNode*> nodes;
        for (int i = 0; i < static_cast<int>(frames_.size()); ++i) {
            if (frames_[i]) nodes.push_back(instructions_[i]);
        }
        return nodes;
    }

    ///@brief 列出所有不可达指令的节点指针（指令列表顺序）。
    ///
    ///镜像 `unreachableInstructionIndices`，供诊断或保守清理通行证使用。
    std::vector<jbytecode::tree::AbstractInsnNode*> unreachableInstructions() const
    {
        std::vector<jbytecode::tree::AbstractInsnNode*> nodes;
        for (int i = 0; i < static_cast<int>(frames_.size()); ++i) {
            if (!frames_[i]) nodes.push_back(instructions_[i]);
        }
        return nodes;
    }

private:
    friend class DataflowAnalyzer<V>;

    std::vector<std::unique_ptr<FrameType>>          frames_;
    std::vector<jbytecode::tree::AbstractInsnNode*> instructions_;
    int                                              iterations_ = 0;
};

///@brief 模板化的分支敏感数据流分析器。
///
///引擎内部维护 worklist、不动点合并、异常处理器边和分支后继选择，所有
///value 域语义通过 `Interpreter<V>` 与可选的 `BranchPredictor<V>` 注入。
template<typename V>
class DataflowAnalyzer
{
public:
    using FrameType    = jbytecode::analysis::Frame<V>;
    using InterpreterT = jbytecode::analysis::Interpreter<V>;

    ///@brief 构造分析器。
    ///
    ///@param interpreter value 域语义来源，由调用方拥有；分析期间不得销毁。
    ///@param options 迭代预算、异常边开关与可选分支预测器。
    DataflowAnalyzer(InterpreterT* interpreter, DataflowOptions<V> options = {})
        : interpreter_(interpreter)
        , options_(options)
    {}

    ///@brief 访问注入的 interpreter。
    InterpreterT&       interpreter() { return *interpreter_; }
    const InterpreterT& interpreter() const { return *interpreter_; }

    ///@brief 对单个方法执行分支敏感数据流分析。
    ///
    ///@param owner 方法所属类的内部名（如 `"com/example/Foo"`），可为空，
    ///仅用于构造入口帧的 `this` 类型。
    ///@param method 被分析的方法节点；指令列表在分析期间不得变更。
    ///@return 每条指令的入口帧表。抽象/原生方法或空指令列表时返回空结果。
    ///
    ///抛 `jbytecode::analysis::AnalyzerException` 的情况：迭代预算超限、
    ///遇到 `JSR`/`RET`（当前版本明确拒绝）、分支目标标签缺失、栈高不一致
    ///等不可恢复的数据流错误；`invokedynamic` 不再拒绝，按 JVM 标准执行。
    DataflowResult<V> analyze(ca::str::Utf8StringRef owner, jbytecode::tree::MethodNode& method)
    {
        runDriver(owner, method);
        return std::move(result_);
    }

private:
    InterpreterT*       interpreter_;
    DataflowOptions<V>  options_;

    DataflowResult<V>                                result_;
    std::vector<std::vector<jbytecode::tree::TryCatchBlockNode*>> handlers_;
    std::vector<bool>                               queued_;
    std::deque<int>                                 worklist_;
    std::unordered_map<jbytecode::core::Label*, int> labelIndices_;

    void runDriver(ca::str::Utf8StringRef owner, jbytecode::tree::MethodNode& method)
    {
        result_.instructions_ = method.instructions.toArray();
        int count             = static_cast<int>(result_.instructions_.size());
        result_.frames_.clear();
        result_.frames_.resize(count);
        handlers_.assign(count, {});
        queued_.assign(count, false);
        worklist_.clear();
        labelIndices_.clear();

        if ((method.access & (jbytecode::core::Opcodes::ACC_ABSTRACT |
                              jbytecode::core::Opcodes::ACC_NATIVE)) != 0 ||
            count == 0) {
            return;
        }

        buildLabelIndex();
        buildHandlers(method);
        mergeFrame(0, makeInitialFrame(owner, method));

        int iterations = 0;
        while (!worklist_.empty()) {
            if (++iterations > options_.maxIterations) {
                if (options_.tracer) options_.tracer->reject(-1, DataflowRejectReason::BUDGET_EXCEEDED);
                throw jbytecode::analysis::AnalyzerException(
                    "DataflowAnalyzer iteration budget exceeded");
            }
            int index = worklist_.front();
            worklist_.pop_front();
            queued_[index] = false;
            simulate(index);
        }

        result_.iterations_ = iterations;
        if (options_.tracer) {
            int reachable = 0;
            for (const auto& frame : result_.frames_) {
                if (frame) ++reachable;
            }
            options_.tracer->summary(count, reachable, iterations);
        }
    }

    void buildLabelIndex()
    {
        for (int index = 0; index < static_cast<int>(result_.instructions_.size()); ++index) {
            jbytecode::tree::AbstractInsnNode* insn = result_.instructions_[index];
            if (!ca::core::tag_cast::isa<jbytecode::tree::LabelNode>(insn)) continue;
            auto* label                 = ca::core::tag_cast::cast<jbytecode::tree::LabelNode*>(insn);
            labelIndices_[label->label] = index;
        }
    }

    int indexOfLabel(jbytecode::core::Label* label) const
    {
        auto it = labelIndices_.find(label);
        return it == labelIndices_.end() ? -1 : it->second;
    }

    void buildHandlers(jbytecode::tree::MethodNode& method)
    {
        for (auto* handler : method.tryCatchBlocks) {
            int start = indexOfLabel(handler->start);
            int end   = indexOfLabel(handler->end);
            if (start < 0 || end < 0) continue;
            for (int index = start; index < end && index < static_cast<int>(handlers_.size());
                 ++index) {
                handlers_[index].push_back(handler);
            }
        }
    }

    int computeMaxLocals(jbytecode::tree::MethodNode& method)
    {
        int requiredLocals =
            jbytecode::core::Type::getArgumentsAndReturnSizes(method.desc) >> 2;
        if ((method.access & jbytecode::core::Opcodes::ACC_STATIC) != 0) requiredLocals--;

        int maxLocals = std::max(method.maxLocals, requiredLocals);
        for (auto* local : method.localVariables) {
            //单一真相源：Type::getSize（J/D→2，其余→1），替代手写 J/D 判断
            bool wide = jbytecode::core::Type::getType(local->descriptor).getSize() == 2;
            int end = local->index + (wide ? 2 : 1);
            if (end > maxLocals) maxLocals = end;
        }
        return maxLocals;
    }

    FrameType makeInitialFrame(ca::str::Utf8StringRef owner, jbytecode::tree::MethodNode& method)
    {
        int maxLocals = computeMaxLocals(method);
        int maxStack  = method.maxStack > 0 ? method.maxStack : 64;
        FrameType frame(maxLocals, maxStack);

        bool              isStatic   = (method.access & jbytecode::core::Opcodes::ACC_STATIC) != 0;
        jbytecode::core::Type methodType = jbytecode::core::Type::getMethodType(method.desc);
        std::vector<jbytecode::core::Type> args = methodType.getArgumentTypes();

        int local = 0;
        if (!isStatic) {
            jbytecode::core::Type ownerType =
                owner.is_empty()
                    ? jbytecode::core::Type::getObjectType(
                          ca::str::ZUtf8StringRef::from_static("this"))
                    : jbytecode::core::Type::getObjectType(owner);
            frame.setLocal(local++, interpreter_->newParameterValue(false, 0, ownerType));
        }

        for (std::size_t i = 0; i < args.size(); ++i) {
            jbytecode::core::Type arg = args[i];
            frame.setLocal(local, interpreter_->newParameterValue(!isStatic, local, arg));
            local++;
            if (arg.getSize() == 2) {
                frame.setLocal(local, interpreter_->newEmptyValue(local));
                local++;
            }
        }

        while (local < frame.getLocals()) {
            frame.setLocal(local, interpreter_->newEmptyValue(local));
            local++;
        }

        jbytecode::core::Type returnType = methodType.getReturnType();
        if (returnType == jbytecode::core::Type::VOID_TYPE) {
            frame.clearReturn();
        }
        else {
            frame.setReturn(interpreter_->newReturnTypeValue(returnType));
        }
        return frame;
    }

    void mergeFrame(int index, const FrameType& incoming)
    {
        if (index < 0 || index >= static_cast<int>(result_.frames_.size())) return;

        bool changed = false;
        if (!result_.frames_[index]) {
            result_.frames_[index] = std::make_unique<FrameType>(incoming);
            changed                = true;
        }
        else {
            changed = result_.frames_[index]->merge(incoming, interpreter_);
        }

        if (changed && !queued_[index]) {
            queued_[index] = true;
            worklist_.push_back(index);
        }
        if (options_.tracer) options_.tracer->merge(index, changed);
    }

    void mergeLabel(jbytecode::core::Label* label, const FrameType& incoming)
    {
        int index = indexOfLabel(label);
        if (index < 0) {
            if (options_.tracer) options_.tracer->reject(-1, DataflowRejectReason::MISSING_LABEL);
            throw jbytecode::analysis::AnalyzerException("branch target label not found");
        }
        mergeFrame(index, incoming);
    }

    void propagateHandlers(int index, const FrameType& before, const FrameType& after)
    {
        if (!options_.propagateExceptionHandlers) return;
        if (index < 0 || index >= static_cast<int>(handlers_.size())) return;

        for (auto* handler : handlers_[index]) {
            int handlerIndex = indexOfLabel(handler->handler);
            if (handlerIndex < 0) continue;

            jbytecode::core::Type catchType =
                handler->type.is_empty()
                    ? jbytecode::core::Type::getObjectType(
                          ca::str::ZUtf8StringRef::from_static("java/lang/Throwable"))
                    : jbytecode::core::Type::getObjectType(handler->type);

            //ASM merges both the pre- and post-instruction locals because an
            //exception edge may observe either state around a throwing opcode.
            FrameType beforeHandler = before;
            beforeHandler.clearStack();
            V exceptionValue = interpreter_->newExceptionValue(handler, &beforeHandler, catchType);
            beforeHandler.push(exceptionValue);
            mergeFrame(handlerIndex, beforeHandler);

            FrameType afterHandler = after;
            afterHandler.clearStack();
            afterHandler.push(exceptionValue);
            mergeFrame(handlerIndex, afterHandler);
        }
    }

    static bool isReturnOpcode(int opcode)
    {
        return opcode == jbytecode::core::Opcodes::IRETURN ||
               opcode == jbytecode::core::Opcodes::LRETURN ||
               opcode == jbytecode::core::Opcodes::FRETURN ||
               opcode == jbytecode::core::Opcodes::DRETURN ||
               opcode == jbytecode::core::Opcodes::ARETURN ||
               opcode == jbytecode::core::Opcodes::RETURN;
    }

    void executeInsn(jbytecode::tree::AbstractInsnNode* insn, FrameType& frame)
    {
        if (insn->getOpcode() >= 0) {
            frame.execute(insn, interpreter_);
        }
    }

    void mergeKnownBranch(int index, jbytecode::core::Label* label, const FrameType& after, bool taken)
    {
        if (taken) {
            mergeLabel(label, after);
        }
        else {
            mergeFrame(index + 1, after);
        }
    }

    void simulate(int index)
    {
        jbytecode::tree::AbstractInsnNode* insn   = result_.instructions_[index];
        FrameType                          before = *result_.frames_[index];
        FrameType                          after  = before;
        int                                opcode = insn->getOpcode();

        if (opcode == jbytecode::core::Opcodes::JSR || opcode == jbytecode::core::Opcodes::RET) {
            if (options_.tracer) {
                options_.tracer->reject(
                    index,
                    opcode == jbytecode::core::Opcodes::JSR ? DataflowRejectReason::JSR
                                                            : DataflowRejectReason::RET);
            }
            throw jbytecode::analysis::AnalyzerException(
                "DataflowAnalyzer does not support legacy JSR/RET subroutines");
        }
        //INVOKEDYNAMIC 按 JVM 标准执行：Frame::execute 按描述符出参/回值，
        //value 域（BasicInterpreter/ConstantInterpreter 等）对 indy 返回值
        //保守取类型化未知，与旧 jbytecode::analysis::Analyzer 行为一致。

        executeInsn(insn, after);

        switch (opcode) {
        case jbytecode::core::Opcodes::GOTO:
        {
            auto* jump = ca::core::tag_cast::cast<jbytecode::tree::JumpInsnNode*>(insn);
            mergeLabel(jump->label, after);
            return;
        }
        case jbytecode::core::Opcodes::IFEQ:
        case jbytecode::core::Opcodes::IFNE:
        case jbytecode::core::Opcodes::IFLT:
        case jbytecode::core::Opcodes::IFGE:
        case jbytecode::core::Opcodes::IFGT:
        case jbytecode::core::Opcodes::IFLE:
        case jbytecode::core::Opcodes::IFNULL:
        case jbytecode::core::Opcodes::IFNONNULL:
        {
            auto*                jump = ca::core::tag_cast::cast<jbytecode::tree::JumpInsnNode*>(insn);
            std::optional<bool>  taken;
            if (options_.predictor) {
                //predictor 接收 execute 前的帧，自行 peek 操作数；engine 不修改 before。
                taken = options_.predictor->testUnaryBranch(opcode, insn, before);
            }
            if (taken.has_value()) {
                mergeKnownBranch(index, jump->label, after, *taken);
            }
            else {
                mergeLabel(jump->label, after);
                mergeFrame(index + 1, after);
            }
            return;
        }
        case jbytecode::core::Opcodes::IF_ICMPEQ:
        case jbytecode::core::Opcodes::IF_ICMPNE:
        case jbytecode::core::Opcodes::IF_ICMPLT:
        case jbytecode::core::Opcodes::IF_ICMPGE:
        case jbytecode::core::Opcodes::IF_ICMPGT:
        case jbytecode::core::Opcodes::IF_ICMPLE:
        case jbytecode::core::Opcodes::IF_ACMPEQ:
        case jbytecode::core::Opcodes::IF_ACMPNE:
        {
            auto*                jump = ca::core::tag_cast::cast<jbytecode::tree::JumpInsnNode*>(insn);
            std::optional<bool>  taken;
            if (options_.predictor) {
                taken = options_.predictor->testBinaryBranch(opcode, insn, before);
            }
            if (taken.has_value()) {
                mergeKnownBranch(index, jump->label, after, *taken);
            }
            else {
                mergeLabel(jump->label, after);
                mergeFrame(index + 1, after);
            }
            return;
        }
        case jbytecode::core::Opcodes::TABLESWITCH: mergeTableSwitch(insn, before, after); return;
        case jbytecode::core::Opcodes::LOOKUPSWITCH: mergeLookupSwitch(insn, before, after); return;
        case jbytecode::core::Opcodes::ATHROW: propagateHandlers(index, before, after); return;
        default: break;
        }

        if (isReturnOpcode(opcode)) {
            propagateHandlers(index, before, after);
            return;
        }

        mergeFrame(index + 1, after);
        propagateHandlers(index, before, after);
    }

    void mergeTableSwitch(jbytecode::tree::AbstractInsnNode* insn, const FrameType& before,
                          const FrameType& after)
    {
        auto*              table = ca::core::tag_cast::cast<jbytecode::tree::TableSwitchInsnNode*>(insn);
        std::optional<int> selected;
        if (options_.predictor) {
            selected = options_.predictor->testTableSwitch(insn, before);
        }
        //predictor 契约是返回 labels 下标（-1 = dflt）。越界下标按「未知」
        //保守展开全部后继，而不是直接索引数组。
        jbytecode::core::Label* target = nullptr;
        if (selected) {
            int sel = *selected;
            if (sel < 0) {
                target = table->dflt;
            }
            else if (static_cast<std::size_t>(sel) < table->labels.size()) {
                target = table->labels[static_cast<std::size_t>(sel)];
            }
        }
        if (target != nullptr) {
            mergeLabel(target, after);
            return;
        }
        mergeLabel(table->dflt, after);
        for (auto* label : table->labels) {
            mergeLabel(label, after);
        }
    }

    void mergeLookupSwitch(jbytecode::tree::AbstractInsnNode* insn, const FrameType& before,
                           const FrameType& after)
    {
        auto*              lookup = ca::core::tag_cast::cast<jbytecode::tree::LookupSwitchInsnNode*>(insn);
        std::optional<int> selected;
        if (options_.predictor) {
            selected = options_.predictor->testLookupSwitch(insn, before);
        }
        //同 mergeTableSwitch：越界下标退回保守展开。
        jbytecode::core::Label* target = nullptr;
        if (selected) {
            int sel = *selected;
            if (sel < 0) {
                target = lookup->dflt;
            }
            else if (static_cast<std::size_t>(sel) < lookup->labels.size()) {
                target = lookup->labels[static_cast<std::size_t>(sel)];
            }
        }
        if (target != nullptr) {
            mergeLabel(target, after);
            return;
        }
        mergeLabel(lookup->dflt, after);
        for (auto* label : lookup->labels) {
            mergeLabel(label, after);
        }
    }
};

///@brief 一次数据流分析的帧快照：持有帧所有权，并暴露与
///`jbytecode::analysis::Analyzer::analyze` 同形的下标对齐裸指针视图。
///
///供从旧引擎迁移的调用方零语义切换：`data[i]` 是第 `i` 条指令的入口帧，
///不可达为 `nullptr`；`empty` 表示分析无结果（abstract/native、空方法），
///调用方按 fail-open 跳过。帧的生命周期由快照持有，不随分析器销毁失效。
template<typename V>
class DataflowFrames
{
public:
    using FrameType = jbytecode::analysis::Frame<V>;

    DataflowFrames() = default;

    explicit DataflowFrames(DataflowResult<V>&& result)
        : result_(std::move(result))
    {
        index_.reserve(static_cast<std::size_t>(result_.size()));
        for (int i = 0; i < result_.size(); ++i) {
            index_.push_back(result_.frameAt(i));
        }
        //abstract/native 方法返回全空帧表，折叠为 empty 以复刻旧引擎
        //`analyze` 对这两种方法直接返回 {} 的行为（调用方 fail-open 判据）。
        bool anyReachable = false;
        for (auto* frame : index_) {
            if (frame) {
                anyReachable = true;
                break;
            }
        }
        if (!anyReachable) index_.clear();
    }

    ///无任何帧（分析无结果）时为 true。
    bool empty() const { return index_.empty(); }

    ///下标对齐的指令总数（含 LABEL/LINE/FRAME 伪指令）。
    std::size_t size() const { return index_.size(); }

    ///下标对齐的裸帧视图，元素含义与旧 `Analyzer::analyze` 返回值一致
    ///（不可达为 `nullptr`）。
    const std::vector<FrameType*>& frames() const { return index_; }

    ///C 风格裸指针视图，等价旧 `Analyzer::analyze` 返回值（不可达为 nullptr）。
    FrameType* const* data() const { return index_.data(); }

    ///可达性/指令反查等结构化查询入口。
    const DataflowResult<V>& result() const { return result_; }

private:
    DataflowResult<V>      result_;
    std::vector<FrameType*> index_;
};

///@brief 单方法数据流分析的一次性入口（`DataflowAnalyzer` 引擎的推荐入口）。
///
///把「建引擎 → analyze → 失败 fail-open」的样板收敛为一次调用：任何不可恢复
///的数据流错误（`JSR`/`RET`、迭代预算超限、分支目标缺失等）一律返回空快照，
///不向外抛异常——与旧 `jbytecode::analysis::Analyzer` 调用点的
///`catch (...) { 返回无帧; }` 语义一致。`invokedynamic` 正常分析，不触发 fail-open。
///
///@param interpreter 值域语义来源，调用方持有，仅在本调用期间被借用。
///@param owner 方法所属类内部名（构造入口帧 `this` 类型用），可为空。
///@param options 分支剪枝/异常边/迭代预算等精度开关；默认保守全后继。
template<typename V>
DataflowFrames<V> analyzeFrames(jbytecode::analysis::Interpreter<V>* interpreter,
                                ca::str::Utf8StringRef                owner,
                                jbytecode::tree::MethodNode&          method,
                                DataflowOptions<V>                    options = {})
{
    DataflowAnalyzer<V> engine(interpreter, options);
    try {
        return DataflowFrames<V>(engine.analyze(owner, method));
    }
    catch (...) {
        return DataflowFrames<V>{};
    }
}

}   //namespace mjt::base
