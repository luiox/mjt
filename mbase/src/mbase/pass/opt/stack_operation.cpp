#include "mbase/pass/opt/stack_operation.hpp"

#include "mbase/analysis/method_frame_analysis.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "mbase/asm/matcher/pattern_matcher.hpp"
#include "mbase/asm/matcher/step_util.hpp"
#include "jbytecode/analysis/analyzer.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "libca/core/tag_cast.hpp"
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

//============================================================================
//stack_operation 私有实现
//本家族两个 pass 共享的 matcher 基建和帧分析 helper。原分散在 replace_passes.cpp
//和 dead_code_remover.cpp，现按家族聚合。类型/帧分析的 helper（AnalyzerCapacityGuard、
//StackExpansionPlan、planStackExpansion 等）与 dead_code_remover.cpp 保持一致语义。
//============================================================================

namespace {

//── matcher 基建（StackOperationSimplifier 使用）──────────────────────────

template<typename Fn>
static void addRule(PatternMatcher& m, std::vector<IMatchStep*> steps, Fn strategyFn)
{
    m.addRule(std::make_unique<MatchRule>(std::move(steps),
                                          std::make_unique<LambdaStrategy>(std::move(strategyFn))));
}

static bool applyUntilConverge(PatternMatcher& matcher, InsnList& insns,
                               ca::str::Utf8StringArena& arena)
{
    bool changed = false;
    while (matcher.apply(insns, arena)) {
        changed = true;
    }
    return changed;
}

//── 帧分析基建（StackOperationExpander 使用，与 dead_code_remover.cpp 同语义）──

void addCloned(InsnList& target, AbstractInsnNode* source, garena::Arena& arena,
               std::unordered_map<Label*, Label*>& clonedLabels)
{
    //B8 新 clone 协议：产物在 arena 上分配（唯一消费方 run 传入目标方法的 arena）。
    target.add(source->clone(arena, clonedLabels));
}

class AnalyzerCapacityGuard
{
public:
    explicit AnalyzerCapacityGuard(MethodNode& methodNode)
        : methodNode_(methodNode)
        , originalMaxStack_(methodNode.maxStack)
    {
        if (methodNode_.maxStack <= 0 && !methodNode_.instructions.isEmpty()) {
            methodNode_.maxStack = (std::max)(8, methodNode_.instructions.size());
        }
    }

    ~AnalyzerCapacityGuard()
    {
        if (!committed_) methodNode_.maxStack = originalMaxStack_;
    }

    void commit() { committed_ = true; }

private:
    MethodNode& methodNode_;
    int         originalMaxStack_;
    bool        committed_ = false;
};

//OriginalSourceFrame 别名由 mbase/analysis/method_frame_analysis.hpp 规范提供。

struct StackExpansionPlan
{
    std::vector<ca::i32>                 popOpcodes;
    std::vector<const AbstractInsnNode*> producers;
};

bool stackFromTop(const OriginalSourceFrame* frame, ca::i32 fromTop, OriginalSourceValue& out)
{
    if (!frame || fromTop <= 0 || frame->getStackSize() < fromTop) return false;
    out = frame->getStack(frame->getStackSize() - fromTop);
    return true;
}

const AbstractInsnNode* constantProducer(const OriginalSourceValue& value)
{
    const auto* source = value.originalSource_ ? value.originalSource_ : &value;
    if (!source->isOneWayProduced()) return nullptr;
    const auto* producer = source->getProducer();
    if (!producer || !InsnUtil::isConstant(producer->getOpcode())) return nullptr;
    return producer;
}

bool appendPop(StackExpansionPlan& plan, const OriginalSourceValue& value)
{
    plan.popOpcodes.push_back(value.getSize() == 2 ? Opcodes::POP2 : Opcodes::POP);
    return true;
}

bool appendProducer(StackExpansionPlan& plan, const AbstractInsnNode* producer)
{
    if (!producer) return false;
    plan.producers.push_back(producer);
    return true;
}

bool planStackExpansion(AbstractInsnNode* insn, const OriginalSourceFrame* frame,
                        StackExpansionPlan& plan)
{
    if (!insn || !frame) return false;
    const int opcode = insn->getOpcode();

    OriginalSourceValue v1;
    if (!stackFromTop(frame, 1, v1)) return false;
    const auto* p1 = constantProducer(v1);
    if (!p1) return false;

    if (opcode == Opcodes::DUP) {
        return appendProducer(plan, p1);
    }

    if (opcode == Opcodes::DUP2) {
        if (v1.getSize() == 2) {
            return appendProducer(plan, p1);
        }
        OriginalSourceValue v2;
        if (!stackFromTop(frame, 2, v2)) return false;
        const auto* p2 = constantProducer(v2);
        if (!p2 || v2.getSize() != 1) return false;
        return appendProducer(plan, p2) && appendProducer(plan, p1);
    }

    OriginalSourceValue v2;
    if (!stackFromTop(frame, 2, v2)) return false;
    const auto* p2 = constantProducer(v2);
    if (!p2) return false;

    if (opcode == Opcodes::DUP_X1) {
        if (v1.getSize() != 1 || v2.getSize() != 1) return false;
        return appendPop(plan, v1) && appendPop(plan, v2) && appendProducer(plan, p1) &&
               appendProducer(plan, p2) && appendProducer(plan, p1);
    }

    if (opcode == Opcodes::SWAP) {
        if (v1.getSize() != 1 || v2.getSize() != 1) return false;
        return appendPop(plan, v1) && appendPop(plan, v2) && appendProducer(plan, p1) &&
               appendProducer(plan, p2);
    }

    OriginalSourceValue v3;
    const bool          hasV3 = stackFromTop(frame, 3, v3);
    const auto*         p3    = hasV3 ? constantProducer(v3) : nullptr;

    if (opcode == Opcodes::DUP_X2) {
        if (v1.getSize() == 1 && v2.getSize() == 2) {
            return appendPop(plan, v1) && appendPop(plan, v2) && appendProducer(plan, p1) &&
                   appendProducer(plan, p2) && appendProducer(plan, p1);
        }
        if (!p3 || v1.getSize() != 1 || v2.getSize() != 1 || v3.getSize() != 1) return false;
        return appendPop(plan, v1) && appendPop(plan, v2) && appendPop(plan, v3) &&
               appendProducer(plan, p1) && appendProducer(plan, p3) && appendProducer(plan, p2) &&
               appendProducer(plan, p1);
    }

    if (opcode == Opcodes::DUP2_X1) {
        if (v1.getSize() == 2 && v2.getSize() == 1) {
            return appendPop(plan, v1) && appendPop(plan, v2) && appendProducer(plan, p1) &&
                   appendProducer(plan, p2) && appendProducer(plan, p1);
        }
        if (!p3 || v1.getSize() != 1 || v2.getSize() != 1 || v3.getSize() != 1) return false;
        return appendPop(plan, v1) && appendPop(plan, v2) && appendPop(plan, v3) &&
               appendProducer(plan, p2) && appendProducer(plan, p1) && appendProducer(plan, p3) &&
               appendProducer(plan, p2) && appendProducer(plan, p1);
    }

    if (opcode != Opcodes::DUP2_X2) return false;

    if (v1.getSize() == 2 && v2.getSize() == 2) {
        return appendPop(plan, v1) && appendPop(plan, v2) && appendProducer(plan, p1) &&
               appendProducer(plan, p2) && appendProducer(plan, p1);
    }

    if (!p3) return false;
    if (v1.getSize() == 1 && v2.getSize() == 1 && v3.getSize() == 2) {
        return appendPop(plan, v1) && appendPop(plan, v2) && appendPop(plan, v3) &&
               appendProducer(plan, p2) && appendProducer(plan, p1) && appendProducer(plan, p3) &&
               appendProducer(plan, p2) && appendProducer(plan, p1);
    }
    if (v1.getSize() == 2 && v2.getSize() == 1 && v3.getSize() == 1) {
        return appendPop(plan, v1) && appendPop(plan, v2) && appendPop(plan, v3) &&
               appendProducer(plan, p1) && appendProducer(plan, p3) && appendProducer(plan, p2) &&
               appendProducer(plan, p1);
    }

    OriginalSourceValue v4;
    if (!stackFromTop(frame, 4, v4)) return false;
    const auto* p4 = constantProducer(v4);
    if (!p4 || v1.getSize() != 1 || v2.getSize() != 1 || v3.getSize() != 1 || v4.getSize() != 1)
        return false;
    return appendPop(plan, v1) && appendPop(plan, v2) && appendPop(plan, v3) &&
           appendPop(plan, v4) && appendProducer(plan, p2) && appendProducer(plan, p1) &&
           appendProducer(plan, p4) && appendProducer(plan, p3) && appendProducer(plan, p2) &&
           appendProducer(plan, p1);
}

void appendStackExpansion(InsnList& target, const StackExpansionPlan& plan, garena::Arena& arena,
                          std::unordered_map<Label*, Label*>& clonedLabels)
{
    for (auto opcode : plan.popOpcodes) {
        target.add(target.create<InsnNode>(opcode));
    }
    for (const auto* producer : plan.producers) {
        //B8 新 clone 协议：产物最终并回 methodNode.instructions，归该方法的 arena。
        target.add(const_cast<AbstractInsnNode*>(producer)->clone(arena, clonedLabels));
    }
}

//============================================================================
//StackOperationSimplifier
//对应 Java replace/StackOperationSimplifier.java。
//PatternMatcher 驱动：dup/pop 消除、pop/pop2 合并、ldc+dup 展开、ldc+swap 重排等。
//只匹配连续的栈操作形状，不跨 label/frame/异常边；至少一次替换算 changed。
//============================================================================

static PatternMatcher& getStackMatcher()
{
    static PatternMatcher matcher = [] {
        PatternMatcher m;

        //消除
        addRule(m, {StepUtil::dup(), StepUtil::pop()}, [](MatchContext&) {});
        addRule(m, {StepUtil::swap(), StepUtil::swap()}, [](MatchContext&) {});
        addRule(m, {StepUtil::dup2(), StepUtil::pop2()}, [](MatchContext&) {});

        //ldc1 - pop => nothing (any 1-slot load with no side effect + pop)
        addRule(m, {StepUtil::loadInt(), StepUtil::pop()}, [](MatchContext&) {});
        addRule(m, {StepUtil::loadFloat(), StepUtil::pop()}, [](MatchContext&) {});
        addRule(m, {StepUtil::loadStr(), StepUtil::pop()}, [](MatchContext&) {});
        //ldc2 - pop2 => nothing
        addRule(m, {StepUtil::loadLong(), StepUtil::pop2()}, [](MatchContext&) {});
        addRule(m, {StepUtil::loadDouble(), StepUtil::pop2()}, [](MatchContext&) {});
        //ldc1 - ldc1 - pop2 => nothing
        addRule(
            m, {StepUtil::loadInt(), StepUtil::loadInt(), StepUtil::pop2()}, [](MatchContext&) {});
        addRule(m,
                {StepUtil::loadFloat(), StepUtil::loadFloat(), StepUtil::pop2()},
                [](MatchContext&) {});

        //合并
        addRule(m, {StepUtil::pop(), StepUtil::pop()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::POP2));
        });
        addRule(m, {StepUtil::dup(), StepUtil::pop2()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::POP));
        });
        addRule(m, {StepUtil::loadInt(), StepUtil::pop2()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::POP));
        });
        addRule(m, {StepUtil::loadFloat(), StepUtil::pop2()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::POP));
        });
        addRule(m, {StepUtil::loadStr(), StepUtil::pop2()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::POP));
        });
        addRule(m, {StepUtil::dup_x1(), StepUtil::swap()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::DUP));
        });

        //ldc1 - dup => ldc1 ldc1
        addRule(m, {StepUtil::loadInt(), StepUtil::dup()}, [](MatchContext& ctx) {
            ctx.builder->add(cloneMatchedNode(ctx, 0));
            ctx.builder->add(cloneMatchedNode(ctx, 0));
        });
        addRule(m, {StepUtil::loadFloat(), StepUtil::dup()}, [](MatchContext& ctx) {
            ctx.builder->add(cloneMatchedNode(ctx, 0));
            ctx.builder->add(cloneMatchedNode(ctx, 0));
        });
        addRule(m, {StepUtil::loadStr(), StepUtil::dup()}, [](MatchContext& ctx) {
            ctx.builder->add(cloneMatchedNode(ctx, 0));
            ctx.builder->add(cloneMatchedNode(ctx, 0));
        });
        //ldc2 - dup2 => ldc2 ldc2
        addRule(m, {StepUtil::loadLong(), StepUtil::dup2()}, [](MatchContext& ctx) {
            ctx.builder->add(cloneMatchedNode(ctx, 0));
            ctx.builder->add(cloneMatchedNode(ctx, 0));
        });
        addRule(m, {StepUtil::loadDouble(), StepUtil::dup2()}, [](MatchContext& ctx) {
            ctx.builder->add(cloneMatchedNode(ctx, 0));
            ctx.builder->add(cloneMatchedNode(ctx, 0));
        });
        //ldc1 - swap - pop => pop - ldc1
        addRule(m, {StepUtil::loadInt(), StepUtil::swap(), StepUtil::pop()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::POP));
            ctx.builder->add(cloneMatchedNode(ctx, 0));
        });
        addRule(
            m, {StepUtil::loadFloat(), StepUtil::swap(), StepUtil::pop()}, [](MatchContext& ctx) {
                ctx.builder->add(new InsnNode(Opcodes::POP));
                ctx.builder->add(cloneMatchedNode(ctx, 0));
            });
        addRule(m, {StepUtil::loadStr(), StepUtil::swap(), StepUtil::pop()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::POP));
            ctx.builder->add(cloneMatchedNode(ctx, 0));
        });

        //ldc1(1) - ldc1(2) - swap => ldc1(2) - ldc1(1)
        addRule(
            m, {StepUtil::loadInt(), StepUtil::loadInt(), StepUtil::swap()}, [](MatchContext& ctx) {
                ctx.builder->add(cloneMatchedNode(ctx, 1));
                ctx.builder->add(cloneMatchedNode(ctx, 0));
            });
        addRule(m,
                {StepUtil::loadFloat(), StepUtil::loadFloat(), StepUtil::swap()},
                [](MatchContext& ctx) {
                    ctx.builder->add(cloneMatchedNode(ctx, 1));
                    ctx.builder->add(cloneMatchedNode(ctx, 0));
                });

        //ldc1(1) - ldc1(2) - dup_x1 - pop2 => ldc1(2)
        addRule(m,
                {StepUtil::loadInt(), StepUtil::loadInt(), StepUtil::dup_x1(), StepUtil::pop2()},
                [](MatchContext& ctx) { ctx.builder->add(cloneMatchedNode(ctx, 1)); });
        addRule(
            m,
            {StepUtil::loadFloat(), StepUtil::loadFloat(), StepUtil::dup_x1(), StepUtil::pop2()},
            [](MatchContext& ctx) { ctx.builder->add(cloneMatchedNode(ctx, 1)); });

        //ldc2(1) - ldc2(2) - dup2_x2 - pop2 - pop2 => ldc2(2)
        addRule(m,
                {StepUtil::loadLong(),
                 StepUtil::loadLong(),
                 StepUtil::dup2_x2(),
                 StepUtil::pop2(),
                 StepUtil::pop2()},
                [](MatchContext& ctx) { ctx.builder->add(cloneMatchedNode(ctx, 1)); });
        addRule(m,
                {StepUtil::loadDouble(),
                 StepUtil::loadDouble(),
                 StepUtil::dup2_x2(),
                 StepUtil::pop2(),
                 StepUtil::pop2()},
                [](MatchContext& ctx) { ctx.builder->add(cloneMatchedNode(ctx, 1)); });

        //ldc1(1) - ldc1(2) - dup_x1 - pop => ldc1(2) - ldc1(1)
        addRule(m,
                {StepUtil::loadInt(), StepUtil::loadInt(), StepUtil::dup_x1(), StepUtil::pop()},
                [](MatchContext& ctx) {
                    ctx.builder->add(cloneMatchedNode(ctx, 1));
                    ctx.builder->add(cloneMatchedNode(ctx, 0));
                });
        addRule(m,
                {StepUtil::loadFloat(), StepUtil::loadFloat(), StepUtil::dup_x1(), StepUtil::pop()},
                [](MatchContext& ctx) {
                    ctx.builder->add(cloneMatchedNode(ctx, 1));
                    ctx.builder->add(cloneMatchedNode(ctx, 0));
                });

        return m;
    }();
    return matcher;
}

}   //namespace

PreservedAnalyses StackOperationSimplifier::run(jbytecode::tree::MethodNode& methodNode,
                                                MethodAnalysisManager&       context)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    const bool changed = applyUntilConverge(getStackMatcher(), methodNode.instructions, context.arena());
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//StackOperationExpander
//对应 Java dataflow/StackOperationExpander.java。只展开所有 operand 都能追溯到
//单一常量 producer 的 DUP/SWAP 族指令。
//============================================================================

PreservedAnalyses StackOperationExpander::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    //guard 保留：本 pass 展开指令后经 commit 持久化修复的 maxStack（分析自身只临时抬升）。
    AnalyzerCapacityGuard capacity(methodNode);

    //帧分析走 method 级 AM，复用/重算缓存的帧（取代自建 analyzer 重复重算）。
    const auto& frameResult = am.getResult<OriginalSourceFrameAnalysis>(methodNode);
    if (!frameResult.ok) return PreservedAnalyses::all();
    const auto& frames = frameResult.frames;

    auto instructions = methodNode.instructions.toArray();
    if (frames.size() != instructions.size()) return PreservedAnalyses::all();

    std::unordered_map<const AbstractInsnNode*, StackExpansionPlan> plans;
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        auto* insn = instructions[i];
        if (!insn) continue;
        StackExpansionPlan plan;
        if (planStackExpansion(insn, frames[i], plan)) {
            plans.emplace(insn, std::move(plan));
        }
    }

    if (plans.empty()) return PreservedAnalyses::all();

    //绑定方法 arena：result 里的 arena 克隆产物并入 instructions 后表已排空；
    //绑定保证任何残余节点不被误 delete（unbound 表会 delete arena 节点）。
    InsnList                           result(&methodNode.arena());
    std::unordered_map<Label*, Label*> clonedLabels;
    for (auto* insn : instructions) {
        auto it = plans.find(insn);
        if (it != plans.end()) {
            appendStackExpansion(result, it->second, methodNode.arena(), clonedLabels);
        }
        else {
            addCloned(result, insn, methodNode.arena(), clonedLabels);
        }
    }

    methodNode.instructions.clear();
    methodNode.instructions.add(result);
    capacity.commit();
    return PreservedAnalyses::none();
}

}   //namespace mjt::base
