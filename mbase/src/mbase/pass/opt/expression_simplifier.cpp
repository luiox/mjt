#include "mbase/pass/opt/expression_simplifier.hpp"

#include "mbase/asm/insn/insn_util.hpp"
#include "mbase/asm/insn/math_helper.hpp"
#include "mbase/asm/matcher/pattern_matcher.hpp"
#include "mbase/asm/matcher/step_util.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/core/any.hpp"
#include <cstddef>
#include <memory>
#include <optional>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

//============================================================================
//ExpressionSimplifier 私有实现
//PatternMatcher 驱动的代数恒等式化简（x+0→x、x^x→0、swap;iadd→iadd、结合律等）。
//原位于 replace_passes.cpp，现独立成专一 .cpp。
//============================================================================

namespace {

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

static IMatchStep* constInt()
{
    return new AnyStep([](AbstractInsnNode* node) {
        int value = 0;
        return InsnUtil::getIntValue(node, value);
    });
}

static std::optional<int> jvmIntBinary(int opcode, int left, int right)
{
    auto result = MathHelper::binaryAny(opcode, ca::core::Any(left), ca::core::Any(right));
    if (auto* value = result.as<int>()) return *value;
    return std::nullopt;
}

class SameIntValueStep : public IMatchStep
{
public:
    explicit SameIntValueStep(std::size_t previousIndex)
        : previousIndex_(previousIndex)
    {}

    bool tryMatch(AbstractInsnNode* now, MatchContext& ctx) override
    {
        if (previousIndex_ >= ctx.matchedNodes.size()) {
            return false;
        }

        int previousValue = 0;
        int currentValue  = 0;
        return InsnUtil::getIntValue(ctx.matchedNodes[previousIndex_], previousValue) &&
               InsnUtil::getIntValue(now, currentValue) && previousValue == currentValue;
    }

private:
    std::size_t previousIndex_;
};

static IMatchStep* sameIntValueAs(std::size_t previousIndex)
{
    return new SameIntValueStep(previousIndex);
}

static PatternMatcher& getExpressionMatcher()
{
    static PatternMatcher matcher = [] {
        PatternMatcher m;

        //--- 恒等元 ---
        addRule(m,
                {StepUtil::loadInt(), StepUtil::iconst_0(), StepUtil::iadd()},
                [](MatchContext& ctx) { ctx.builder->add(cloneMatchedNode(ctx, 0)); });
        addRule(m,
                {StepUtil::loadInt(), StepUtil::iconst_0(), StepUtil::ior()},
                [](MatchContext& ctx) { ctx.builder->add(cloneMatchedNode(ctx, 0)); });
        addRule(m,
                {StepUtil::loadInt(), StepUtil::iconst_0(), StepUtil::ixor()},
                [](MatchContext& ctx) { ctx.builder->add(cloneMatchedNode(ctx, 0)); });
        addRule(m,
                {StepUtil::loadInt(), StepUtil::iconst_1(), StepUtil::imul()},
                [](MatchContext& ctx) { ctx.builder->add(cloneMatchedNode(ctx, 0)); });
        addRule(m,
                {StepUtil::loadInt(), StepUtil::iconst_m1(), StepUtil::iand()},
                [](MatchContext& ctx) { ctx.builder->add(cloneMatchedNode(ctx, 0)); });
        addRule(
            m, {StepUtil::loadInt(), StepUtil::iconst_0(), StepUtil::iand()}, [](MatchContext&) {});
        addRule(m, {StepUtil::iconst_m1(), StepUtil::iand()}, [](MatchContext&) {});
        addRule(m, {StepUtil::iconst_0(), StepUtil::ixor()}, [](MatchContext&) {});

        //--- 逆元/交换律 ---
        addRule(m, {StepUtil::swap(), StepUtil::iadd()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::IADD));
        });
        addRule(m, {StepUtil::swap(), StepUtil::ixor()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::IXOR));
        });
        addRule(m, {StepUtil::swap(), StepUtil::iand()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::IAND));
        });
        addRule(m, {StepUtil::swap(), StepUtil::ior()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::IOR));
        });

        //--- 结合律 (load+op+load+op => sum+op) ---
        auto assoc = [](PatternMatcher& m, IMatchStep* op, int resultOp) {
            addRule(m, {constInt(), op, constInt(), op}, [resultOp](MatchContext& ctx) {
                int v1 = 0, v2 = 0;
                if (!InsnUtil::getIntValue(ctx.matchedNodes[0], v1) ||
                    !InsnUtil::getIntValue(ctx.matchedNodes[2], v2)) {
                    ctx.cancelReplacement();
                    return;
                }
                switch (resultOp) {
                case Opcodes::IADD:
                case Opcodes::IMUL:
                case Opcodes::IOR:
                case Opcodes::IXOR:
                case Opcodes::IAND:
                    const auto result = jvmIntBinary(resultOp, v1, v2);
                    if (!result.has_value()) {
                        ctx.cancelReplacement();
                        return;
                    }
                    ctx.builder->add(InsnUtil::getIntInsn(*result));
                    break;
                }
                ctx.builder->add(new InsnNode(resultOp));
            });
        };
        assoc(m, StepUtil::iadd(), Opcodes::IADD);
        assoc(m, StepUtil::imul(), Opcodes::IMUL);
        assoc(m, StepUtil::ior(), Opcodes::IOR);
        assoc(m, StepUtil::ixor(), Opcodes::IXOR);
        assoc(m, StepUtil::iand(), Opcodes::IAND);

        //--- 特定恒等式 ---
        addRule(m, {StepUtil::loadInt(), StepUtil::dup(), StepUtil::ixor()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::ICONST_0));
        });
        addRule(m,
                {StepUtil::loadInt(), StepUtil::iconst_0(), StepUtil::iand()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::ICONST_0)); });
        addRule(m,
                {StepUtil::loadInt(), StepUtil::iconst_1(), StepUtil::ishl()},
                [](MatchContext& ctx) {
                    ctx.builder->add(cloneMatchedNode(ctx, 0));
                    ctx.builder->add(new InsnNode(Opcodes::ICONST_2));
                    ctx.builder->add(new InsnNode(Opcodes::IMUL));
                });
        addRule(m, {StepUtil::ineg(), StepUtil::ineg()}, [](MatchContext&) {});
        addRule(m, {StepUtil::lneg(), StepUtil::lneg()}, [](MatchContext&) {});

        //--- ineg 相关：iconst_m1 - imul => ineg ---
        addRule(m, {StepUtil::iconst_m1(), StepUtil::imul()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::INEG));
        });
        addRule(m,
                {StepUtil::iconst_m1(), StepUtil::ixor(), StepUtil::iconst_m1(), StepUtil::isub()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::INEG)); });
        addRule(m,
                {StepUtil::iconst_m1(), StepUtil::ixor(), StepUtil::iconst_1(), StepUtil::iadd()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::INEG)); });
        addRule(m,
                {StepUtil::iconst_m1(), StepUtil::isub(), StepUtil::iconst_m1(), StepUtil::ixor()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::INEG)); });
        addRule(m,
                {StepUtil::iconst_m1(), StepUtil::iadd(), StepUtil::iconst_m1(), StepUtil::ixor()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::INEG)); });

        //--- iadd 相关 ---
        addRule(m, {StepUtil::ineg(), StepUtil::isub()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::IADD));
        });
        addRule(m,
                {StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::isub(),
                 StepUtil::iconst_1(),
                 StepUtil::isub()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::IADD)); });
        addRule(m,
                {StepUtil::dup2(),
                 StepUtil::ior(),
                 StepUtil::dup_x2(),
                 StepUtil::pop(),
                 StepUtil::iand(),
                 StepUtil::iadd()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::IADD)); });
        addRule(m,
                {StepUtil::dup2(),
                 StepUtil::ixor(),
                 StepUtil::dup_x2(),
                 StepUtil::pop(),
                 StepUtil::iand(),
                 StepUtil::iconst_2(),
                 StepUtil::imul(),
                 StepUtil::iadd()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::IADD)); });
        addRule(m,
                {StepUtil::dup2(),
                 StepUtil::ior(),
                 StepUtil::iconst_2(),
                 StepUtil::imul(),
                 StepUtil::dup_x2(),
                 StepUtil::pop(),
                 StepUtil::ixor(),
                 StepUtil::isub()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::IADD)); });

        //--- isub 相关 ---
        addRule(m, {StepUtil::ineg(), StepUtil::iadd()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::ISUB));
        });
        addRule(m,
                {StepUtil::swap(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iadd(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::ISUB)); });
        addRule(m,
                {StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iadd(),
                 StepUtil::iconst_1(),
                 StepUtil::iadd()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::ISUB)); });
        addRule(m,
                {StepUtil::dup2(),
                 StepUtil::ixor(),
                 StepUtil::dup_x2(),
                 StepUtil::pop(),
                 StepUtil::swap(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iand(),
                 StepUtil::iconst_2(),
                 StepUtil::imul(),
                 StepUtil::isub()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::ISUB)); });
        addRule(m,
                {StepUtil::dup2(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iand(),
                 StepUtil::dup_x2(),
                 StepUtil::pop(),
                 StepUtil::swap(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iand(),
                 StepUtil::isub()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::ISUB)); });
        addRule(m,
                {StepUtil::dup2(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iand(),
                 StepUtil::iconst_2(),
                 StepUtil::imul(),
                 StepUtil::dup_x2(),
                 StepUtil::pop(),
                 StepUtil::ixor(),
                 StepUtil::isub()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::ISUB)); });

        //--- iand 相关 ---
        addRule(m,
                {StepUtil::swap(),
                 StepUtil::dup_x1(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::ior(),
                 StepUtil::swap(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::isub()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::IAND)); });

        //--- ior 相关 ---
        addRule(m,
                {StepUtil::dup_x1(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iand(),
                 StepUtil::iadd()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::IOR)); });

        //--- ixor 相关 ---
        addRule(m,
                {StepUtil::dup2(),
                 StepUtil::ior(),
                 StepUtil::dup_x2(),
                 StepUtil::pop(),
                 StepUtil::iand(),
                 StepUtil::isub()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::IXOR)); });
        addRule(m,
                {StepUtil::dup2(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iand(),
                 StepUtil::dup_x2(),
                 StepUtil::pop(),
                 StepUtil::swap(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iand(),
                 StepUtil::ior()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::IXOR)); });
        addRule(m,
                {StepUtil::dup2(),
                 StepUtil::ior(),
                 StepUtil::dup_x2(),
                 StepUtil::pop(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::swap(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::ior(),
                 StepUtil::iand()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::IXOR)); });
        addRule(m,
                {StepUtil::dup2(),
                 StepUtil::ior(),
                 StepUtil::dup_x2(),
                 StepUtil::pop(),
                 StepUtil::iand(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iand()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::IXOR)); });

        //--- 常量折叠衍生恒等式（来自混淆代码）---
        //(x | ~y) - ~y => x & y
        //模式: x, y, iconst_m1, ixor, ior, y, iconst_m1, ixor, isub
        addRule(m,
                {StepUtil::loadInt(),
                 StepUtil::loadInt(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::ior(),
                 sameIntValueAs(1),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::isub()},
                [](MatchContext& ctx) {
                    ctx.builder->add(cloneMatchedNode(ctx, 0));   //x
                    ctx.builder->add(cloneMatchedNode(ctx, 1));   //y
                    ctx.builder->add(new InsnNode(Opcodes::IAND));
                });

        //x + (y & ~x) => x | y
        //模式: x, y, x, iconst_m1, ixor, iand, iadd
        addRule(m,
                {StepUtil::loadInt(),
                 StepUtil::loadInt(),
                 sameIntValueAs(0),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iand(),
                 StepUtil::iadd()},
                [](MatchContext& ctx) {
                    ctx.builder->add(cloneMatchedNode(ctx, 0));   //x
                    ctx.builder->add(cloneMatchedNode(ctx, 1));   //y
                    ctx.builder->add(new InsnNode(Opcodes::IOR));
                });

        return m;
    }();
    return matcher;
}

}   //namespace

PreservedAnalyses ExpressionSimplifier::run(jbytecode::tree::MethodNode& methodNode,
                                            MethodAnalysisManager&       context)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    const bool changed = applyUntilConverge(getExpressionMatcher(), methodNode.instructions, context.arena());
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
