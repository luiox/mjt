#include "mbase/pass/opt/fix_pattern_simplifier.hpp"
#include "mbase/asm/insn/math_helper.hpp"
#include "mbase/asm/matcher/pattern_matcher.hpp"
#include "mbase/asm/matcher/step_util.hpp"
#include "libca/core/cast.hpp"

using namespace mjt::base;
using namespace jbytecode::core;
using namespace jbytecode::tree;
using ca::str::Utf8StringRef;

template<typename Fn>
static void addRule(PatternMatcher& m, std::vector<IMatchStep*> steps, Fn strategyFn)
{
    m.addRule(std::make_unique<MatchRule>(std::move(steps),
                                          std::make_unique<LambdaStrategy>(std::move(strategyFn))));
}

bool isIfTwoSlotOpcode(int opcode)
{
    return opcode == Opcodes::IF_ICMPGE || opcode == Opcodes::IF_ICMPGT ||
           opcode == Opcodes::IF_ICMPLE || opcode == Opcodes::IF_ICMPLT ||
           opcode == Opcodes::IF_ICMPNE || opcode == Opcodes::IF_ICMPEQ;
}

IMatchStep* if2SlotStep()
{
    static auto* step = new AnyStep(
        [](AbstractInsnNode* insn) { return insn && isIfTwoSlotOpcode(insn->getOpcode()); });
    return step;
}

class SameSlotILoadAfterStoreStep : public IMatchStep
{
public:
    bool tryMatch(AbstractInsnNode* now, MatchContext& ctx) override
    {
        if (!now || now->getOpcode() != Opcodes::ILOAD || ctx.matchedNodes.size() < 2) {
            return false;
        }
        auto* store = ca::core::tag_cast::cast<VarInsnNode*>(ctx.matchedNodes[1]);
        auto* load  = ca::core::tag_cast::cast<VarInsnNode*>(now);
        return store && load && store->var == load->var;
    }
};

IMatchStep* sameSlotILoadAfterStoreStep()
{
    static auto* step = new SameSlotILoadAfterStoreStep();
    return step;
}


static PatternMatcher& getFixPatternMatcher()
{
    static PatternMatcher matcher = [] {
        PatternMatcher m;

        //ineg + iconst_m1 + iadd = ~n -> iconst_m1 + ixor
        //Java 注释: "利用补码恒等式 -n-1 = ~n"
        addRule(
            m, {StepUtil::ineg(), StepUtil::iconst_m1(), StepUtil::iadd()}, [](MatchContext& ctx) {
                ctx.builder->add(new InsnNode(Opcodes::ICONST_M1));
                ctx.builder->add(new InsnNode(Opcodes::IXOR));
            });

        //swap + dup_x1 + iand + isub -> iconst_m1 + ixor + iand
        addRule(m,
                {StepUtil::swap(), StepUtil::dup_x1(), StepUtil::iand(), StepUtil::isub()},
                [](MatchContext& ctx) {
                    ctx.builder->add(new InsnNode(Opcodes::ICONST_M1));
                    ctx.builder->add(new InsnNode(Opcodes::IXOR));
                    ctx.builder->add(new InsnNode(Opcodes::IAND));
                });

        //dup_x1 + ior + swap + isub -> iconst_m1 + ixor + iand
        addRule(m,
                {StepUtil::dup_x1(), StepUtil::ior(), StepUtil::swap(), StepUtil::isub()},
                [](MatchContext& ctx) {
                    ctx.builder->add(new InsnNode(Opcodes::ICONST_M1));
                    ctx.builder->add(new InsnNode(Opcodes::IXOR));
                    ctx.builder->add(new InsnNode(Opcodes::IAND));
                });

        //dup_x1 + ior + isub + ineg -> iconst_m1 + ixor + iand
        addRule(m,
                {StepUtil::dup_x1(), StepUtil::ior(), StepUtil::isub(), StepUtil::ineg()},
                [](MatchContext& ctx) {
                    ctx.builder->add(new InsnNode(Opcodes::ICONST_M1));
                    ctx.builder->add(new InsnNode(Opcodes::IXOR));
                    ctx.builder->add(new InsnNode(Opcodes::IAND));
                });

        //swap + isub + iconst_1 + isub -> isub + iconst_m1 + ixor
        addRule(m,
                {StepUtil::swap(), StepUtil::isub(), StepUtil::iconst_1(), StepUtil::isub()},
                [](MatchContext& ctx) {
                    ctx.builder->add(new InsnNode(Opcodes::ISUB));
                    ctx.builder->add(new InsnNode(Opcodes::ICONST_M1));
                    ctx.builder->add(new InsnNode(Opcodes::IXOR));
                });

        //swap + iconst_m1 + ixor + iadd -> isub + iconst_m1 + ixor
        addRule(m,
                {StepUtil::swap(), StepUtil::iconst_m1(), StepUtil::ixor(), StepUtil::iadd()},
                [](MatchContext& ctx) {
                    ctx.builder->add(new InsnNode(Opcodes::ISUB));
                    ctx.builder->add(new InsnNode(Opcodes::ICONST_M1));
                    ctx.builder->add(new InsnNode(Opcodes::IXOR));
                });

        //dup_x1 + iconst_m1 + ixor + iand + iadd -> ior
        //Java 注释: "~a & ~b = ~(a|b), 配合 iadd 化简"
        addRule(m,
                {StepUtil::dup_x1(),
                 StepUtil::iconst_m1(),
                 StepUtil::ixor(),
                 StepUtil::iand(),
                 StepUtil::iadd()},
                [](MatchContext& ctx) { ctx.builder->add(new InsnNode(Opcodes::IOR)); });

        //loadInt + dup + ixor -> iconst_0 (x ^ x == 0)
        addRule(m, {StepUtil::loadInt(), StepUtil::dup(), StepUtil::ixor()}, [](MatchContext& ctx) {
            ctx.builder->add(new InsnNode(Opcodes::ICONST_0));
        });

        //死代码清理: 压入常量后立即弹出
        addRule(m,
                {StepUtil::iconst_1(),
                 StepUtil::dup(),
                 StepUtil::iadd(),
                 StepUtil::i2l(),
                 StepUtil::l2i(),
                 StepUtil::pop()},
                [](MatchContext&) {});
        addRule(m, {StepUtil::iconst_1(), StepUtil::pop()}, [](MatchContext&) {});
        addRule(m,
                {StepUtil::iconst_1(),
                 StepUtil::dup(),
                 StepUtil::iadd(),
                 StepUtil::dup(),
                 StepUtil::i2l(),
                 StepUtil::l2i(),
                 StepUtil::swap(),
                 StepUtil::pop(),
                 StepUtil::pop()},
                [](MatchContext&) {});
        addRule(m,
                {StepUtil::iconst_m1(), StepUtil::dup(), StepUtil::imul(), StepUtil::pop()},
                [](MatchContext&) {});
        addRule(m,
                {StepUtil::iconst_2(),
                 StepUtil::dup(),
                 StepUtil::imul(),
                 StepUtil::i2l(),
                 StepUtil::l2i(),
                 StepUtil::dup(),
                 StepUtil::swap(),
                 StepUtil::pop()},
                [](MatchContext&) {});
        addRule(m, {StepUtil::step(Opcodes::LCONST_1), StepUtil::pop2()}, [](MatchContext&) {});
        addRule(m, {StepUtil::step(Opcodes::LCONST_0), StepUtil::pop2()}, [](MatchContext&) {});

        //常量折叠条件跳转: constInt + iadd + constInt + IF_ICMP* -> 比较折叠后的值
        //The comparison opcode and label are cloned from the matched jump so
        //all six integer compare variants preserve their original branch edge.
        addRule(
            m, {new AnyStep([](AbstractInsnNode* insn) {
                    int value = 0;
                    return InsnUtil::getIntValue(insn, value);
                }),
                StepUtil::iadd(),
                new AnyStep([](AbstractInsnNode* insn) {
                    int value = 0;
                    return InsnUtil::getIntValue(insn, value);
                }),
                if2SlotStep()}, [](MatchContext& ctx) {
                int leftVal = 0, rightVal = 0;
                if (!InsnUtil::getIntValue(ctx.matchedNodes[0], leftVal) ||
                    !InsnUtil::getIntValue(ctx.matchedNodes[2], rightVal)) {
                    ctx.cancelReplacement();
                    return;
                }

                const int compareOpcode = ctx.matchedNodes[3]->getOpcode();
                const bool equality =
                    compareOpcode == Opcodes::IF_ICMPEQ || compareOpcode == Opcodes::IF_ICMPNE;
                if (!equality && leftVal != 0) {
                    ctx.cancelReplacement();
                    return;
                }

                auto difference = MathHelper::binaryAny(
                    Opcodes::ISUB, ca::core::Any(rightVal), ca::core::Any(leftVal));
                auto* value = difference.as<int>();
                if (!value) {
                    ctx.cancelReplacement();
                    return;
                }
                ctx.builder->add(InsnUtil::getIntInsn(*value));
                ctx.builder->add(cloneMatchedNode(ctx, 3));
            });

        //Legacy rule2: a temporary local stores a producer that is immediately
        //reloaded for an integer comparison. Matching the same slot in the
        //step avoids an apply-until-converge loop on mismatched store/load
        //pairs, while cloning keeps matcher ownership rules intact.
        addRule(m,
                {StepUtil::loadInt(),
                 StepUtil::step(Opcodes::ISTORE),
                 StepUtil::loadInt(),
                 sameSlotILoadAfterStoreStep(),
                 if2SlotStep()},
                [](MatchContext& ctx) {
                    ctx.builder->add(cloneMatchedNode(ctx, 0));
                    ctx.builder->add(cloneMatchedNode(ctx, 1));
                    ctx.builder->add(cloneMatchedNode(ctx, 2));
                    ctx.builder->add(cloneMatchedNode(ctx, 0));
                    ctx.builder->add(cloneMatchedNode(ctx, 4));
                });

        return m;
    }();
    return matcher;
}

PreservedAnalyses FixPatternSimplifier::run(jbytecode::tree::MethodNode& methodNode,
                                            MethodAnalysisManager&       context)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    bool changed = false;
    while (getFixPatternMatcher().apply(methodNode.instructions, context.arena())) {
        changed = true;
    }
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
