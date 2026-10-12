#include "mbase/pass/opt/constant_folder.hpp"
#include "mbase/asm/insn/math_helper.hpp"
#include "mbase/asm/matcher/pattern_matcher.hpp"
#include "mbase/asm/matcher/step_util.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <optional>
#include <algorithm>
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/analysis/analyzer.hpp"
#include "jbytecode/analysis/source_interpreter.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "mbase/asm/analyzer/constant_interpreter.hpp"
#include "mbase/asm/analyzer/constant_value.hpp"
#include "mbase/asm/analyzer/dataflow_analyzer.hpp"
#include "mbase/asm/insn/insn_builder.hpp"
#include "mbase/asm/insn/class_util.hpp"
#include "mbase/analysis/method_effect_analysis.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/model/resource_model.hpp"
#include "libca/core/any.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/log/log_macros.hpp"


namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;
//注意：不使用 using namespace jbytecode::analysis，因为 core 也有 Frame 类导致歧义

namespace {

//============================================================================
//ArithmeticSimplifier 私有实现
//PatternMatcher 驱动的常量算术折叠。原位于 replace_passes.cpp，因与 ConstantFolder
//同属「常量折叠」家族（同一功能的两条实现路径），现按家族聚合。
//============================================================================

///调试设施：MJT_ARITH_DEBUG=1 时打印每次折叠（用于复现 pipeline 语义回归）。
static bool arithDebugEnabled()
{
    static const bool enabled = std::getenv("MJT_ARITH_DEBUG") != nullptr;
    return enabled;
}

static void arithDebugFold(const char* rule, int opcode, const std::string& summary)
{
    if (!arithDebugEnabled()) return;
    CA_LOG_DEBUG("ARITH-FOLD rule={} op={} {}", rule, opcode, summary.c_str());
}

///调试设施：MJT_ARITH_OPS=<csv> 禁用的 opcode 列表（用于复现/二分折叠语义回归）。
static bool arithOpDisabled(int opcode)
{
    const char* raw = std::getenv("MJT_ARITH_OPS");
    if (!raw) return false;
    const std::string text(raw);
    const std::string needle = std::to_string(opcode);
    std::size_t       pos    = 0;
    while (pos <= text.size()) {
        const std::size_t comma = text.find(',', pos);
        const std::string token = text.substr(pos, comma == std::string::npos ? std::string::npos
                                                                             : comma - pos);
        if (token == needle) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

template<typename Fn>
static void addArithRule(PatternMatcher& m, std::vector<IMatchStep*> steps, Fn strategyFn)
{
    m.addRule(std::make_unique<MatchRule>(std::move(steps),
                                          std::make_unique<LambdaStrategy>(std::move(strategyFn))));
}

static bool applyArithUntilConverge(PatternMatcher& matcher, InsnList& insns,
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

static IMatchStep* constLong()
{
    return new AnyStep([](AbstractInsnNode* node) {
        ca::i64 value = 0;
        return InsnUtil::getLongValue(node, value);
    });
}

static IMatchStep* constFloat()
{
    return new AnyStep([](AbstractInsnNode* node) {
        float value = 0.0f;
        return InsnUtil::getFloatValue(node, value);
    });
}

static IMatchStep* constDouble()
{
    return new AnyStep([](AbstractInsnNode* node) {
        double value = 0.0;
        return InsnUtil::getDoubleValue(node, value);
    });
}

template<typename Fn>
static void addIntBin(PatternMatcher& m, int opcode, Fn calc)
{
    if (arithOpDisabled(opcode)) return;
    m.addRule(std::make_unique<MatchRule>(
        std::vector<IMatchStep*>{constInt(), constInt(), StepUtil::step(opcode)},
        std::make_unique<LambdaStrategy>([calc, opcode](MatchContext& ctx) {
            int a, b;
            if (!InsnUtil::getIntValue(ctx.matchedNodes[0], a) ||
                !InsnUtil::getIntValue(ctx.matchedNodes[1], b)) {
                ctx.cancelReplacement();
                return;
            }
            const auto result = calc(a, b);
            if (!result.has_value()) {
                ctx.cancelReplacement();
                return;
            }
            //H3：摘要串只在调试开启时构造（开关关闭时实参串的白构造归零）。
            if (arithDebugEnabled())
                arithDebugFold("intbin", opcode, "a=" + std::to_string(a) + " b=" + std::to_string(b) +
                                                     " -> " + std::to_string(*result));
            ctx.builder->add(InsnUtil::getIntInsn(*result));
        })));
}

template<typename Fn>
static void addIntBinWithSecond(PatternMatcher& m, int opcode, IMatchStep* second, Fn calc)
{
    m.addRule(std::make_unique<MatchRule>(
        std::vector<IMatchStep*>{constInt(), second, StepUtil::step(opcode)},
        std::make_unique<LambdaStrategy>([calc, opcode](MatchContext& ctx) {
            int a, b;
            if (!InsnUtil::getIntValue(ctx.matchedNodes[0], a) ||
                !InsnUtil::getIntValue(ctx.matchedNodes[1], b)) {
                ctx.cancelReplacement();
                return;
            }
            const auto result = calc(a, b);
            if (!result.has_value()) {
                ctx.cancelReplacement();
                return;
            }
            if (arithDebugEnabled())
                arithDebugFold("intbin2", opcode, "a=" + std::to_string(a) + " b=" + std::to_string(b) +
                                                      " -> " + std::to_string(*result));
            ctx.builder->add(InsnUtil::getIntInsn(*result));
        })));
}

template<typename Fn>
static void addLongBin(PatternMatcher& m, int opcode, Fn calc)
{
    m.addRule(std::make_unique<MatchRule>(
        std::vector<IMatchStep*>{constLong(), constLong(), StepUtil::step(opcode)},
        std::make_unique<LambdaStrategy>([calc](MatchContext& ctx) {
            ca::i64 a, b;
            if (!InsnUtil::getLongValue(ctx.matchedNodes[0], a) ||
                !InsnUtil::getLongValue(ctx.matchedNodes[1], b)) {
                ctx.cancelReplacement();
                return;
            }
            const auto result = calc(a, b);
            if (!result.has_value()) {
                ctx.cancelReplacement();
                return;
            }
            ctx.builder->add(InsnUtil::getLongInsn(*result));
        })));
}

template<typename Fn>
static void addLongBinWithSecond(PatternMatcher& m, int opcode, IMatchStep* second, Fn calc)
{
    m.addRule(std::make_unique<MatchRule>(
        std::vector<IMatchStep*>{constLong(), second, StepUtil::step(opcode)},
        std::make_unique<LambdaStrategy>([calc](MatchContext& ctx) {
            ca::i64 a, b;
            if (!InsnUtil::getLongValue(ctx.matchedNodes[0], a) ||
                !InsnUtil::getLongValue(ctx.matchedNodes[1], b)) {
                ctx.cancelReplacement();
                return;
            }
            const auto result = calc(a, b);
            if (!result.has_value()) {
                ctx.cancelReplacement();
                return;
            }
            ctx.builder->add(InsnUtil::getLongInsn(*result));
        })));
}

static IMatchStep* nonZeroInt()
{
    return new AnyStep([](AbstractInsnNode* node) {
        int value = 0;
        return InsnUtil::getIntValue(node, value) && value != 0;
    });
}

static IMatchStep* nonZeroLong()
{
    return new AnyStep([](AbstractInsnNode* node) {
        ca::i64 value = 0;
        return InsnUtil::getLongValue(node, value) && value != 0;
    });
}

//—— 恒等折叠 / 比较折叠辅助（对齐 Recaf deobf 通用 foldRedundantOperations 能力）——

static IMatchStep* constIntValue(int target)
{
    return new AnyStep([target](AbstractInsnNode* node) {
        int value = 0;
        return InsnUtil::getIntValue(node, value) && value == target;
    });
}

static IMatchStep* constLongValue(ca::i64 target)
{
    return new AnyStep([target](AbstractInsnNode* node) {
        ca::i64 value = 0;
        return InsnUtil::getLongValue(node, value) && value == target;
    });
}

static IMatchStep* constFloatValue(float target)
{
    return new AnyStep([target](AbstractInsnNode* node) {
        float value = 0.0f;
        return InsnUtil::getFloatValue(node, value) && value == target;
    });
}

static IMatchStep* constDoubleValue(double target)
{
    return new AnyStep([target](AbstractInsnNode* node) {
        double value = 0.0;
        return InsnUtil::getDoubleValue(node, value) && value == target;
    });
}

///恒等折叠的"x"操作数：必须是 0-pop-1-push 指令（常量/局部变量加载）。
///不能用 anyValue：若 x 是 2-pop 指令（如 IXOR），模式 `identity; x; op`
///会让 op 消费到 x 下方更早压入的值，折叠后栈效果与原始代码不一致
///（复现：Grunt2 Controlflow 混淆的 `dup_x1; iconst_m1; ixor; iand` 垃圾序列，
///[ICONST_M1][IXOR][IAND] 被错误折叠为 [IXOR]，helper 语义改变）。
static IMatchStep* singlePushValue()
{
    return new AnyStep([](AbstractInsnNode* node) {
        if (!node) return false;
        const int type = node->getType();
        if (type == AbstractInsnNode::LDC_INSN || type == AbstractInsnNode::INT_INSN) {
            return true;   //ldc/ldc_w/ldc2_w、bipush/sipush
        }
        if (type == AbstractInsnNode::VAR_INSN) {
            const int op = node->getOpcode();
            //ILOAD(21)..ALOAD_3(45) 全部是加载（ILOAD_0..ALOAD_3 简写仅数值定义）。
            return op >= Opcodes::ILOAD && op <= 45;
        }
        if (type == AbstractInsnNode::INSN) {
            const int op = node->getOpcode();
            return (op >= Opcodes::ICONST_M1 && op <= Opcodes::ICONST_5) ||
                   op == Opcodes::ACONST_NULL;
        }
        return false;
    });
}

///恒等折叠规则：`x OP identity = x`（identityOnLeft=false，x 在左）或
///`identity OP x = x`（identityOnLeft=true，x 在右）。删除 OP 与恒等元常量指令，
///克隆保留另一操作数（原指令仍由 matcher 移除并释放）。
///
///安全矩阵（对齐 Recaf foldRedundantOperations 并修正其 IEEE 754 / 除法定向问题）：
///- int/long：`*1`、`/1`（仅右）、`+0`、`|0`、`&(-1)`、`^0`、`<<0`/`>>0`/`>>>0`（仅右）。
///- float/double：仅 `*1.0`、`/1.0`（乘法/除法恒等元在 IEEE 754 下对 NaN/±0.0/Inf 均成立）；
///**不做 `+0.0`**（`-0.0 + 0.0 = +0.0`，会改变 signed zero）。
///- 非交换运算（除法）只注册右操作数恒等：`1 / x` 不是 `x`。
static void addIdentityFold(PatternMatcher& m, int opcode, IMatchStep* identityStep,
                            bool identityOnLeft)
{
    if (arithOpDisabled(-1)) return;   //MJT_ARITH_OPS=-1：禁用全部恒等折叠
    std::vector<IMatchStep*> steps;
    const int                keepIndex = identityOnLeft ? 1 : 0;
    if (identityOnLeft) {
        steps = {identityStep, singlePushValue(), StepUtil::step(opcode)};
    }
    else {
        steps = {singlePushValue(), identityStep, StepUtil::step(opcode)};
    }
    m.addRule(std::make_unique<MatchRule>(
        std::move(steps),
        std::make_unique<LambdaStrategy>([keepIndex, opcode, identityOnLeft](MatchContext& ctx) {
            const auto* kept = ctx.matchedNodes[static_cast<std::size_t>(keepIndex)];
            if (arithDebugEnabled()) {
                std::string matched =
                    fmt::format("ARITH-FOLD rule=identity op={} left={} keep={}:{} matched=[",
                                opcode, identityOnLeft ? 1 : 0, kept->getOpcode(),
                                kept->getType());
                for (auto* m : ctx.matchedNodes) {
                    matched += fmt::format(" {}:{}", m->getOpcode(), m->getType());
                }
                CA_LOG_DEBUG("{} ]", matched);
            }
            ctx.builder->add(cloneMatchedNode(ctx, keepIndex));
        })));
}

///比较折叠规则：两个常量操作数做 `*CMP*`，结果折叠为 int 常量（-1/0/1）。
///MathHelper::binaryAny 按 JVM spec 处理 NaN（FCMPL/DCMPL 与 FCMPG/DCMPG 差异）。
template<typename Fn>
static void addCmpFold(PatternMatcher& m, int opcode, IMatchStep* left, IMatchStep* right,
                       Fn readValue)
{
    m.addRule(std::make_unique<MatchRule>(
        std::vector<IMatchStep*>{left, right, StepUtil::step(opcode)},
        std::make_unique<LambdaStrategy>([readValue, opcode](MatchContext& ctx) {
            ca::core::Any a;
            ca::core::Any b;
            if (!readValue(ctx.matchedNodes[0], a) || !readValue(ctx.matchedNodes[1], b)) {
                ctx.cancelReplacement();
                return;
            }
            const auto result = MathHelper::binaryAny(opcode, a, b);
            if (auto* value = result.as<int>()) {
                ctx.builder->add(InsnUtil::getIntInsn(*value));
            }
            else {
                ctx.cancelReplacement();
            }
        })));
}

template<typename Fn>
static void addFloatBin(PatternMatcher& m, int opcode, Fn calc)
{
    m.addRule(std::make_unique<MatchRule>(
        std::vector<IMatchStep*>{constFloat(), constFloat(), StepUtil::step(opcode)},
        std::make_unique<LambdaStrategy>([calc](MatchContext& ctx) {
            float a, b;
            if (!InsnUtil::getFloatValue(ctx.matchedNodes[0], a) ||
                !InsnUtil::getFloatValue(ctx.matchedNodes[1], b)) {
                ctx.cancelReplacement();
                return;
            }
            const auto result = calc(a, b);
            if (!result.has_value()) {
                ctx.cancelReplacement();
                return;
            }
            ctx.builder->add(InsnUtil::getFloatInsn(*result));
        })));
}

template<typename Fn>
static void addDoubleBin(PatternMatcher& m, int opcode, Fn calc)
{
    m.addRule(std::make_unique<MatchRule>(
        std::vector<IMatchStep*>{constDouble(), constDouble(), StepUtil::step(opcode)},
        std::make_unique<LambdaStrategy>([calc](MatchContext& ctx) {
            double a, b;
            if (!InsnUtil::getDoubleValue(ctx.matchedNodes[0], a) ||
                !InsnUtil::getDoubleValue(ctx.matchedNodes[1], b)) {
                ctx.cancelReplacement();
                return;
            }
            const auto result = calc(a, b);
            if (!result.has_value()) {
                ctx.cancelReplacement();
                return;
            }
            ctx.builder->add(InsnUtil::getDoubleInsn(*result));
        })));
}

static std::optional<int> jvmIntBinary(int opcode, int left, int right)
{
    auto result = MathHelper::binaryAny(opcode, ca::core::Any(left), ca::core::Any(right));
    if (auto* value = result.as<int>()) return *value;
    return std::nullopt;
}

static std::optional<ca::i64> jvmLongBinary(int opcode, ca::i64 left, ca::i64 right)
{
    auto result = MathHelper::binaryAny(opcode, ca::core::Any(left), ca::core::Any(right));
    if (auto* value = result.as<ca::i64>()) return *value;
    return std::nullopt;
}

static std::optional<float> jvmFloatBinary(int opcode, float left, float right)
{
    auto result = MathHelper::binaryAny(opcode, ca::core::Any(left), ca::core::Any(right));
    if (auto* value = result.as<float>()) return *value;
    return std::nullopt;
}

static std::optional<double> jvmDoubleBinary(int opcode, double left, double right)
{
    auto result = MathHelper::binaryAny(opcode, ca::core::Any(left), ca::core::Any(right));
    if (auto* value = result.as<double>()) return *value;
    return std::nullopt;
}

static void addLongShift(PatternMatcher& m, int opcode)
{
    m.addRule(std::make_unique<MatchRule>(
        std::vector<IMatchStep*>{constLong(), constInt(), StepUtil::step(opcode)},
        std::make_unique<LambdaStrategy>([opcode](MatchContext& ctx) {
            ca::i64 value = 0;
            int     shift = 0;
            if (!InsnUtil::getLongValue(ctx.matchedNodes[0], value) ||
                !InsnUtil::getIntValue(ctx.matchedNodes[1], shift)) {
                ctx.cancelReplacement();
                return;
            }
            auto result = MathHelper::binaryAny(opcode, ca::core::Any(value), ca::core::Any(shift));
            if (auto* folded = result.as<ca::i64>()) {
                ctx.builder->add(InsnUtil::getLongInsn(*folded));
            }
            else {
                ctx.cancelReplacement();
            }
        })));
}

static PatternMatcher& getArithmeticMatcher()
{
    static PatternMatcher matcher = [] {
        PatternMatcher m;
        addIntBin(m, Opcodes::IADD, [](int a, int b) { return jvmIntBinary(Opcodes::IADD, a, b); });
        addIntBin(m, Opcodes::ISUB, [](int a, int b) { return jvmIntBinary(Opcodes::ISUB, a, b); });
        addIntBin(m, Opcodes::IMUL, [](int a, int b) { return jvmIntBinary(Opcodes::IMUL, a, b); });
        addIntBinWithSecond(m, Opcodes::IDIV, nonZeroInt(), [](int a, int b) {
            return jvmIntBinary(Opcodes::IDIV, a, b);
        });
        addIntBinWithSecond(m, Opcodes::IREM, nonZeroInt(), [](int a, int b) {
            return jvmIntBinary(Opcodes::IREM, a, b);
        });
        addIntBin(m, Opcodes::ISHL, [](int a, int b) { return jvmIntBinary(Opcodes::ISHL, a, b); });
        addIntBin(m, Opcodes::ISHR, [](int a, int b) { return jvmIntBinary(Opcodes::ISHR, a, b); });
        addIntBin(m, Opcodes::IUSHR, [](int a, int b) { return jvmIntBinary(Opcodes::IUSHR, a, b); });
        addIntBin(m, Opcodes::IAND, [](int a, int b) { return jvmIntBinary(Opcodes::IAND, a, b); });
        addIntBin(m, Opcodes::IOR, [](int a, int b) { return jvmIntBinary(Opcodes::IOR, a, b); });
        addIntBin(m, Opcodes::IXOR, [](int a, int b) { return jvmIntBinary(Opcodes::IXOR, a, b); });

        addLongBin(m, Opcodes::LADD, [](ca::i64 a, ca::i64 b) { return jvmLongBinary(Opcodes::LADD, a, b); });
        addLongBin(m, Opcodes::LSUB, [](ca::i64 a, ca::i64 b) { return jvmLongBinary(Opcodes::LSUB, a, b); });
        addLongBin(m, Opcodes::LMUL, [](ca::i64 a, ca::i64 b) { return jvmLongBinary(Opcodes::LMUL, a, b); });
        addLongBinWithSecond(m, Opcodes::LDIV, nonZeroLong(), [](ca::i64 a, ca::i64 b) {
            return jvmLongBinary(Opcodes::LDIV, a, b);
        });
        addLongBinWithSecond(m, Opcodes::LREM, nonZeroLong(), [](ca::i64 a, ca::i64 b) {
            return jvmLongBinary(Opcodes::LREM, a, b);
        });
        addLongShift(m, Opcodes::LSHL);
        addLongShift(m, Opcodes::LSHR);
        addLongShift(m, Opcodes::LUSHR);
        addLongBin(m, Opcodes::LAND, [](ca::i64 a, ca::i64 b) { return jvmLongBinary(Opcodes::LAND, a, b); });
        addLongBin(m, Opcodes::LOR, [](ca::i64 a, ca::i64 b) { return jvmLongBinary(Opcodes::LOR, a, b); });
        addLongBin(m, Opcodes::LXOR, [](ca::i64 a, ca::i64 b) { return jvmLongBinary(Opcodes::LXOR, a, b); });

        addFloatBin(m, Opcodes::FADD, [](float a, float b) { return jvmFloatBinary(Opcodes::FADD, a, b); });
        addFloatBin(m, Opcodes::FSUB, [](float a, float b) { return jvmFloatBinary(Opcodes::FSUB, a, b); });
        addFloatBin(m, Opcodes::FMUL, [](float a, float b) { return jvmFloatBinary(Opcodes::FMUL, a, b); });
        addFloatBin(m, Opcodes::FDIV, [](float a, float b) { return jvmFloatBinary(Opcodes::FDIV, a, b); });
        addFloatBin(m, Opcodes::FREM, [](float a, float b) { return jvmFloatBinary(Opcodes::FREM, a, b); });

        addDoubleBin(m, Opcodes::DADD, [](double a, double b) { return jvmDoubleBinary(Opcodes::DADD, a, b); });
        addDoubleBin(m, Opcodes::DSUB, [](double a, double b) { return jvmDoubleBinary(Opcodes::DSUB, a, b); });
        addDoubleBin(m, Opcodes::DMUL, [](double a, double b) { return jvmDoubleBinary(Opcodes::DMUL, a, b); });
        addDoubleBin(m, Opcodes::DDIV, [](double a, double b) { return jvmDoubleBinary(Opcodes::DDIV, a, b); });
        addDoubleBin(m, Opcodes::DREM, [](double a, double b) { return jvmDoubleBinary(Opcodes::DREM, a, b); });

        addArithRule(m, {constInt(), StepUtil::ineg()}, [](MatchContext& ctx) { arithDebugFold("unary", Opcodes::INEG, "");
            int v;
            if (!InsnUtil::getIntValue(ctx.matchedNodes[0], v)) {
                ctx.cancelReplacement();
                return;
            }
            auto result = MathHelper::unaryAny(Opcodes::INEG, ca::core::Any(v));
            if (auto* value = result.as<int>())
                ctx.builder->add(InsnUtil::getIntInsn(*value));
            else
                ctx.cancelReplacement();
        });
        addArithRule(m, {constLong(), StepUtil::lneg()}, [](MatchContext& ctx) { arithDebugFold("unary", Opcodes::LNEG, "");
            ca::i64 v;
            if (!InsnUtil::getLongValue(ctx.matchedNodes[0], v)) {
                ctx.cancelReplacement();
                return;
            }
            auto result = MathHelper::unaryAny(Opcodes::LNEG, ca::core::Any(v));
            if (auto* value = result.as<ca::i64>())
                ctx.builder->add(InsnUtil::getLongInsn(*value));
            else
                ctx.cancelReplacement();
        });
        addArithRule(m, {constFloat(), StepUtil::fneg()}, [](MatchContext& ctx) { arithDebugFold("unary", Opcodes::FNEG, "");
            float v;
            if (!InsnUtil::getFloatValue(ctx.matchedNodes[0], v)) {
                ctx.cancelReplacement();
                return;
            }
            ctx.builder->add(InsnUtil::getFloatInsn(-v));
        });
        addArithRule(m, {constDouble(), StepUtil::dneg()}, [](MatchContext& ctx) { arithDebugFold("unary", Opcodes::DNEG, "");
            double v;
            if (!InsnUtil::getDoubleValue(ctx.matchedNodes[0], v)) {
                ctx.cancelReplacement();
                return;
            }
            ctx.builder->add(InsnUtil::getDoubleInsn(-v));
        });

        //—— 恒等折叠（对齐 Recaf foldRedundantOperations，见 addIdentityFold 安全矩阵）——
        //乘法：x*1 / 1*x（int/long/float/double，交换律两侧都注册）。
        addIdentityFold(m, Opcodes::IMUL, constIntValue(1), false);
        addIdentityFold(m, Opcodes::IMUL, constIntValue(1), true);
        addIdentityFold(m, Opcodes::LMUL, constLongValue(1), false);
        addIdentityFold(m, Opcodes::LMUL, constLongValue(1), true);
        addIdentityFold(m, Opcodes::FMUL, constFloatValue(1.0f), false);
        addIdentityFold(m, Opcodes::FMUL, constFloatValue(1.0f), true);
        addIdentityFold(m, Opcodes::DMUL, constDoubleValue(1.0), false);
        addIdentityFold(m, Opcodes::DMUL, constDoubleValue(1.0), true);
        //除法：仅右操作数（x/1 = x；1/x 不是 x）。
        addIdentityFold(m, Opcodes::IDIV, constIntValue(1), false);
        addIdentityFold(m, Opcodes::LDIV, constLongValue(1), false);
        addIdentityFold(m, Opcodes::FDIV, constFloatValue(1.0f), false);
        addIdentityFold(m, Opcodes::DDIV, constDoubleValue(1.0), false);
        //加法：x+0 / 0+x（仅 int/long；float/double 的 +0.0 会改变 signed zero，不做）。
        addIdentityFold(m, Opcodes::IADD, constIntValue(0), false);
        addIdentityFold(m, Opcodes::IADD, constIntValue(0), true);
        addIdentityFold(m, Opcodes::LADD, constLongValue(0), false);
        addIdentityFold(m, Opcodes::LADD, constLongValue(0), true);
        //按位或：x|0 / 0|x。
        addIdentityFold(m, Opcodes::IOR, constIntValue(0), false);
        addIdentityFold(m, Opcodes::IOR, constIntValue(0), true);
        addIdentityFold(m, Opcodes::LOR, constLongValue(0), false);
        addIdentityFold(m, Opcodes::LOR, constLongValue(0), true);
        //按位与：x&-1 / -1&x。
        addIdentityFold(m, Opcodes::IAND, constIntValue(-1), false);
        addIdentityFold(m, Opcodes::IAND, constIntValue(-1), true);
        addIdentityFold(m, Opcodes::LAND, constLongValue(-1), false);
        addIdentityFold(m, Opcodes::LAND, constLongValue(-1), true);
        //按位异或：x^0 / 0^x。
        addIdentityFold(m, Opcodes::IXOR, constIntValue(0), false);
        addIdentityFold(m, Opcodes::IXOR, constIntValue(0), true);
        addIdentityFold(m, Opcodes::LXOR, constLongValue(0), false);
        addIdentityFold(m, Opcodes::LXOR, constLongValue(0), true);
        //移位：x<<0 / x>>0 / x>>>0（右操作数 0，int 与 long 左值）。
        addIdentityFold(m, Opcodes::ISHL, constIntValue(0), false);
        addIdentityFold(m, Opcodes::ISHR, constIntValue(0), false);
        addIdentityFold(m, Opcodes::IUSHR, constIntValue(0), false);
        addIdentityFold(m, Opcodes::LSHL, constIntValue(0), false);
        addIdentityFold(m, Opcodes::LSHR, constIntValue(0), false);
        addIdentityFold(m, Opcodes::LUSHR, constIntValue(0), false);

        //—— 比较折叠（Recaf Evaluator 支持 FCMPL/FCMPG/LCMP/DCMPL/DCMPG；mbase 此前缺失）——
        addCmpFold(m, Opcodes::FCMPL, constFloat(), constFloat(),
                   [](AbstractInsnNode* node, ca::core::Any& out) {
                       float v = 0.0f;
                       if (!InsnUtil::getFloatValue(node, v)) return false;
                       out = ca::core::Any(v);
                       return true;
                   });
        addCmpFold(m, Opcodes::FCMPG, constFloat(), constFloat(),
                   [](AbstractInsnNode* node, ca::core::Any& out) {
                       float v = 0.0f;
                       if (!InsnUtil::getFloatValue(node, v)) return false;
                       out = ca::core::Any(v);
                       return true;
                   });
        addCmpFold(m, Opcodes::LCMP, constLong(), constLong(),
                   [](AbstractInsnNode* node, ca::core::Any& out) {
                       ca::i64 v = 0;
                       if (!InsnUtil::getLongValue(node, v)) return false;
                       out = ca::core::Any(v);
                       return true;
                   });
        addCmpFold(m, Opcodes::DCMPL, constDouble(), constDouble(),
                   [](AbstractInsnNode* node, ca::core::Any& out) {
                       double v = 0.0;
                       if (!InsnUtil::getDoubleValue(node, v)) return false;
                       out = ca::core::Any(v);
                       return true;
                   });
        addCmpFold(m, Opcodes::DCMPG, constDouble(), constDouble(),
                   [](AbstractInsnNode* node, ca::core::Any& out) {
                       double v = 0.0;
                       if (!InsnUtil::getDoubleValue(node, v)) return false;
                       out = ca::core::Any(v);
                       return true;
                   });

        return m;
    }();
    return matcher;
}

//========== SimpleConstantFolder / LocalConstantFolder / ConstantFolder 共用 helper ==========

//获取 AbstractInsnNode 对应的常量指令，返回 nullptr 表示不能转为常量指令。
///B9 建点规范：产物在 `arena`（调用方归属方法域）上分配——fold 结果不经暂存器、
///经 insertBefore 直插绑定列表，堆双轨不适用。
static AbstractInsnNode* toConstantInsn(const ConstantValue& value, garena::Arena& arena)
{
    if (value.isNull()) {
        return arena.create<InsnNode>(Opcodes::ACONST_NULL);
    }
    if (value.isInteger() && value.isKnown()) {
        return InsnUtil::getIntInsn(arena, value.getAsInteger());
    }
    if (value.isLong() && value.isKnown()) {
        return InsnUtil::getLongInsn(arena, value.getAsLong());
    }
    if (value.getType() == jbytecode::analysis::BasicValue::FLOAT_VALUE &&
        value.getValue().has_value()) {
        if (auto* p = value.getValue().as<float>()) {
            return InsnUtil::getFloatInsn(arena, *p);
        }
    }
    if (value.getType() == jbytecode::analysis::BasicValue::DOUBLE_VALUE &&
        value.getValue().has_value()) {
        if (auto* p = value.getValue().as<double>()) {
            return InsnUtil::getDoubleInsn(arena, *p);
        }
    }
    if (value.isString() && value.getValue().has_value()) {
        if (auto* p = value.getValue().as<std::string>()) {
            return arena.create<LdcInsnNode>(*p);
        }
        if (auto* p = value.getValue().as<ca::str::Utf8StringRef>()) {
            return arena.create<LdcInsnNode>(p->to_std_string());
        }
    }
    //class literal（ldc Foo.class）：ConstantInterpreter 把 CONSTANT_Class 存为
    //jbytecode::core::Type。 对齐 Java toConstantInsn 的 value.getValue instanceof Type 分支。
    if (auto* p = value.getValue().as<jbytecode::core::Type>()) {
        return InsnUtil::getTypeInsn(arena, *p);
    }
    return nullptr;
}

//获取指令索引对应的帧（nullptr 安全）
using ConstantFrame = jbytecode::analysis::Frame<ConstantValue>;

static ConstantFrame* frameAt(ConstantFrame* const frames[], int index, int length)
{
    if (index < 0 || index >= length) return nullptr;
    return frames[index];
}

//Frame 使用“值数量”而不是 JVM 槽位数量。只有候选区间前已存在的每个值在终点 frame
//都未改变时，才允许保留原栈并只物化净新增值。
static bool preservesStackPrefix(const ConstantFrame& startFrame, const ConstantFrame& endFrame)
{
    if (endFrame.getStackSize() < startFrame.getStackSize()) return false;
    for (int index = 0; index < startFrame.getStackSize(); ++index) {
        const ConstantValue& startValue = startFrame.getStack(index);
        const ConstantValue& endValue   = endFrame.getStack(index);
        //ConstantValue 对两个 unknown int 的值相等只表示类型相同，不能证明它们来自同一
        //栈值。若窗口前已有未知值，保守放弃该候选，避免 SWAP/消费/重建形状误改写。
        if ((!startValue.isKnown() && !startValue.isNull()) ||
            (!endValue.isKnown() && !endValue.isNull()) || !startValue.equals(endValue)) {
            return false;
        }
    }
    return true;
}

//块范围
struct BlockRange
{
    int start;
    int endExclusive;
};

//折叠结果
struct FoldResult
{
    int                            endExclusive;
    std::vector<AbstractInsnNode*> replacement;
};

//构建基本块
static std::vector<BlockRange> buildBlocks(AbstractInsnNode** insnArray, int arrayLen)
{
    std::vector<BlockRange> ranges;
    int                     start = -1;
    for (int i = 0; i < arrayLen; i++) {
        auto* insn = insnArray[i];

        if (InsnUtil::isBoundaryNode(insn)) {
            if (start != -1) {
                ranges.push_back({start, i});
                start = -1;
            }
            continue;
        }

        if (start == -1) {
            start = i;
        }

        if (InsnUtil::isTerminator(insn->getOpcode())) {
            ranges.push_back({start, i + 1});
            start = -1;
        }
    }

    if (start != -1) {
        ranges.push_back({start, arrayLen});
    }
    return ranges;
}

//尝试折叠一个区间内的常量计算（`arena` = 区间所属方法域，替换节点在其上分配）
static std::optional<std::vector<AbstractInsnNode*>> tryFoldRange(ConstantFrame* const frames[],
                                                                  int frameLen, int start,
                                                                  int endExclusive,
                                                                  garena::Arena& arena)
{
    auto* startFrame = frameAt(frames, start, frameLen);
    auto* endFrame   = frameAt(frames, endExclusive, frameLen);
    if (!startFrame || !endFrame) return std::nullopt;

    int startSize = startFrame->getStackSize();
    int endSize   = endFrame->getStackSize();
    if (!preservesStackPrefix(*startFrame, *endFrame)) return std::nullopt;

    std::vector<AbstractInsnNode*> replacement;
    for (int idx = startSize; idx < endSize; idx++) {
        const ConstantValue& value     = endFrame->getStack(idx);
        auto*                constInsn = toConstantInsn(value, arena);
        if (!constInsn) return std::nullopt;
        replacement.push_back(constInsn);
    }

    return replacement;
}

//扫描并收集折叠结果（`arena` = 区间所属方法域，透传 tryFoldRange）
static void scanAndCollectFold(AbstractInsnNode** insnArray, int arrayLen,
                               ConstantFrame* const frames[], int frameLen, const BlockRange& block,
                               std::unordered_map<int, FoldResult>& foldMap, garena::Arena& arena)
{
    int index = block.start;
    while (index < block.endExclusive) {
        AbstractInsnNode* startInsn = insnArray[index];
        //检查起始指令是否为常量加载
        if (!InsnUtil::isConstant(startInsn->getOpcode())) {
            index++;
            continue;
        }

        auto* startFrame = frameAt(frames, index, frameLen);
        if (!startFrame) {
            index++;
            continue;
        }
        int safeLine = startFrame->getStackSize();

        //扩展窗口
        int end = index;
        while (end < block.endExclusive) {
            AbstractInsnNode* current = insnArray[end];
            if (!InsnUtil::isPureComputation(current)) break;

            auto* currentFrame = frameAt(frames, end, frameLen);
            if (!currentFrame) break;

            int consume = std::max(0, InsnUtil::getConsumeStackCount(current->getOpcode()));
            if (consume > 0) {
                int stackBottomIndex = currentFrame->getStackSize() - consume;
                if (stackBottomIndex < safeLine) break;
            }
            end++;
        }

        //尝试折叠（区间长度需大于 1，即有实质运算）
        if (end - index > 1) {
            auto replacement = tryFoldRange(frames, frameLen, index, end, arena);
            if (replacement.has_value()) {
                foldMap[index] = {end, std::move(replacement.value())};
                index          = end;
                continue;
            }
        }
        index++;
    }
}

}   //namespace

//========== SimpleConstantFolder（对应 Java SimpleConstantFolder）==========

//========== LocalConstantFolder（对应 Java LocalConstantFolder）==========

//============================================================================
//ConstantFolder（对应 Java 常量分组折叠实现）
//算法：粗扫描常量指令组 → ConstantInterpreter 帧分析收紧组 → 用终点栈帧折叠为常量加载。
//逐行对齐 Java 的 roughScanningRange / adjustFoldRange / handleFoldInsns / fold /
//addConstantNumber。迭代到指令数不动点（Java doConstantFold 外层 do-while）。
//============================================================================

namespace {

//—— 对齐 Java InsnUtil.isNumber/isInteger/isLong/isFloat/isDouble/isTypeCast/isStackOperateInsn ——
//ConstantFolder 专用谓词（与 LocalConstantFolder 的 helper 风格一致，本地静态）。
static bool acfIsInteger(AbstractInsnNode* insn)
{
    int op = insn->getOpcode();
    if ((op >= Opcodes::ICONST_M1 && op <= Opcodes::ICONST_5) || op == Opcodes::SIPUSH ||
        op == Opcodes::BIPUSH)
        return true;
    if (insn->getType() == AbstractInsnNode::LDC_INSN) {
        auto* ldc = ca::core::tag_cast::cast<LdcInsnNode*>(insn);
        return ldc->cst.as<int>() != nullptr;
    }
    return false;
}
static bool acfIsLong(AbstractInsnNode* insn)
{
    int op = insn->getOpcode();
    if (op == Opcodes::LCONST_0 || op == Opcodes::LCONST_1) return true;
    if (insn->getType() == AbstractInsnNode::LDC_INSN) {
        auto* ldc = ca::core::tag_cast::cast<LdcInsnNode*>(insn);
        return ldc->cst.as<ca::i64>() != nullptr || ldc->cst.as<long long>() != nullptr;
    }
    return false;
}
static bool acfIsFloat(AbstractInsnNode* insn)
{
    int op = insn->getOpcode();
    if (op >= Opcodes::FCONST_0 && op <= Opcodes::FCONST_2) return true;
    if (insn->getType() == AbstractInsnNode::LDC_INSN) {
        auto* ldc = ca::core::tag_cast::cast<LdcInsnNode*>(insn);
        return ldc->cst.as<float>() != nullptr;
    }
    return false;
}
static bool acfIsDouble(AbstractInsnNode* insn)
{
    int op = insn->getOpcode();
    if (op >= Opcodes::DCONST_0 && op <= Opcodes::DCONST_1) return true;
    if (insn->getType() == AbstractInsnNode::LDC_INSN) {
        auto* ldc = ca::core::tag_cast::cast<LdcInsnNode*>(insn);
        return ldc->cst.as<double>() != nullptr;
    }
    return false;
}
//Java InsnUtil.isNumber：int/long/float/double 常量加载。
static bool acfIsNumber(AbstractInsnNode* insn)
{
    return acfIsInteger(insn) || acfIsLong(insn) || acfIsFloat(insn) || acfIsDouble(insn);
}
//默认 folder 只处理无需重排窗口外栈值的栈操作。DUP_X* 的形式选择依赖
//category-1/category-2 组合，不能由 frame 值数量安全判断，保持原样。
static bool acfIsStackOperateInsn(AbstractInsnNode* insn)
{
    int op = insn->getOpcode();
    return op == Opcodes::DUP || op == Opcodes::DUP2 || op == Opcodes::SWAP ||
           op == Opcodes::POP || op == Opcodes::POP2;
}
//Java InsnUtil.isArithmeticInsn：复用 C++ InsnUtil::isArithmeticInsn(opcode)；
//另补 *CMP* 比较指令（MathHelper::binaryAny 支持其 JVM 语义计算，ConstantInterpreter
//经 binaryOperation 分发，组折叠可覆盖 FCMPL/FCMPG/LCMP/DCMPL/DCMPG → int 常量）。
static bool acfIsArithmeticInsn(AbstractInsnNode* insn)
{
    int op = insn->getOpcode();
    return InsnUtil::isArithmeticInsn(op) || op == Opcodes::FCMPL || op == Opcodes::FCMPG ||
           op == Opcodes::LCMP || op == Opcodes::DCMPL || op == Opcodes::DCMPG;
}
//Java InsnUtil.isTypeCast：opcode 在 [I2L, D2F] 内。
static bool acfIsTypeCast(AbstractInsnNode* insn)
{
    int op = insn->getOpcode();
    return op >= Opcodes::I2L && op <= Opcodes::D2F;
}
//Java isConstantInstruction：number / stackOp / arithmetic / typeCast。
static bool acfIsConstantInstruction(AbstractInsnNode* insn)
{
    return acfIsNumber(insn) || acfIsStackOperateInsn(insn) || acfIsArithmeticInsn(insn) ||
           acfIsTypeCast(insn);
}

//折叠区间 <begin, end>。
struct AcfRange
{
    int begin;
    int end;
};

//Java roughScanningRange：找常量指令组。起始必须是数值常量，组内只能含常量类指令。
static std::vector<AcfRange> acfRoughScanningRange(const std::vector<AbstractInsnNode*>& insns)
{
    std::vector<AcfRange> ranges;
    int                   size = static_cast<int>(insns.size());
    int                   i    = 0;
    while (i < size) {
        while (i < size && !acfIsNumber(insns[static_cast<std::size_t>(i)])) i++;
        if (i == size) break;
        int j = i;
        while (j < size && acfIsConstantInstruction(insns[static_cast<std::size_t>(j)])) j++;
        ranges.push_back({i, j});
        i = j;
    }
    return ranges;
}

//Java addConstantNumber：把单个 ConstantValue 物化为等价指令序列，追加到 builder。
//返回 false 表示类型无法处理（对齐 Java 返回 false）。
static bool acfAddConstantNumber(InsnBuilder& b, const ConstantValue& value)
{
    if (value.isInteger() && value.isKnown()) {
        b.add(InsnUtil::getIntInsn(value.getAsInteger()));
    }
    else if (value.isLong() && value.isKnown()) {
        b.add(InsnUtil::getLongInsn(value.getAsLong()));
    }
    else if (value.isString() && value.getValue().has_value()) {
        const auto& val = value.getValue();
        if (auto* p = val.as<std::string>())
            b.ldc(*p);
        else if (auto* p = val.as<ca::str::Utf8StringRef>())
            b.ldc(p->to_std_string());
        else
            return false;
    }
    else if (auto* p = value.getValue().as<float>()) {
        b.ldc(*p);
    }
    else if (auto* p = value.getValue().as<double>()) {
        b.ldc(*p);
    }
    else if (value.isNull()) {
        //Java 这里注释掉了 null 分支，故 null 不处理（走到底 return false）。忠实保留。
        return false;
    }
    else {
        //数组常量（char[]/float[]/double[]/int[]）与 Class literal：ConstantInterpreter
        //当前未把它们物化进 ConstantValue（C++ 的 ConstantValue 只装标量/string/Type），
        //故此处无法重建，与 Java 的 addConstantNumber 数组分支语义对齐——返回 false 跳过折叠。
        return false;
    }
    return true;
}

//Java fold：对 [start,end) 用终点栈帧构造等价常量加载序列。返回 nullopt 表示折叠失败。
static std::optional<std::vector<AbstractInsnNode*>> acfFold(ConstantFrame* const frames[],
                                                             int frameLen, int start, int end)
{
    auto* startFrame = (start >= 0 && start < frameLen) ? frames[start] : nullptr;
    auto* endFrame   = (end >= 0 && end < frameLen) ? frames[end] : nullptr;
    if (startFrame == nullptr || endFrame == nullptr) return std::nullopt;

    //先校验终点栈帧中超出起始栈大小的部分都已知（对齐 Java isKnown 校验）。
    int startSize = startFrame->getStackSize();
    int endSize   = endFrame->getStackSize();
    if (!preservesStackPrefix(*startFrame, *endFrame)) return std::nullopt;
    //若区间净产出无新增栈值（endSize <= startSize），折叠结果为空——删除原指令会破坏
    //代码（栈不平衡）。这种情况放弃折叠（比 Java 的空替换更安全；Java 此处是潜在 bug）。
    if (endSize <= startSize) return std::nullopt;
    for (int i = startSize; i < endSize; i++) {
        if (!endFrame->getStack(i).isKnown()) return std::nullopt;
    }

    InsnBuilder b;
    //起始为 0 或非 0 两种情况，Java 都是从 startSize 到 endSize 构造（两分支代码相同）。
    for (int i = startSize; i < endSize; i++) {
        const ConstantValue& value = endFrame->getStack(i);
        if (!acfAddConstantNumber(b, value)) {
            return std::nullopt;
        }
    }
    //取出 builder 内的指令。
    std::vector<AbstractInsnNode*> result;
    InsnList                       tmp = b.getInsnList();
    while (auto* insn = tmp.getFirst()) {
        tmp.remove(insn);
        result.push_back(insn);
    }
    return result;
}

struct AcfFold
{
    int                            begin;
    int                            end;
    std::vector<AbstractInsnNode*> replacement;
};

//一个粗扫描组内可能先出现依赖既有栈的计算、后出现可独立折叠的常量后缀。逐个起点
//选择最长的可证明候选，既不遗漏安全后缀，也不把 JVM 槽位数混入 Frame 值索引。
static void acfCollectFolds(const AcfRange& range, const std::vector<AbstractInsnNode*>& insns,
                            ConstantFrame* const frames[], int frameLen,
                            std::vector<AcfFold>& output)
{
    int cursor = range.begin;
    while (cursor < range.end) {
        bool found = false;
        for (int begin = cursor; begin < range.end && !found; ++begin) {
            if (!acfIsNumber(insns[static_cast<std::size_t>(begin)])) continue;
            for (int end = range.end; end > begin + 1; --end) {
                auto replacement = acfFold(frames, frameLen, begin, end);
                if (!replacement.has_value() ||
                    replacement->size() >= static_cast<std::size_t>(end - begin)) {
                    continue;
                }
                output.push_back({begin, end, std::move(*replacement)});
                cursor = end;
                found  = true;
                break;
            }
        }
        if (!found) break;
    }
}

}   //namespace

PreservedAnalyses SimpleConstantFolder::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    auto& insns = methodNode.instructions;
    if (insns.isEmpty()) return PreservedAnalyses::all();

    //使用 SourceInterpreter 进行帧分析，追踪值的来源指令。
    //统一走 DataflowAnalyzer 引擎一次性入口（analyzeFrames），失败返回空帧 fail-open。
    jbytecode::analysis::SourceInterpreter interpreter;
    const auto                             snapshot = analyzeFrames<jbytecode::analysis::SourceValue>(
        &interpreter, ca::str::ZUtf8StringRef::from_static("T"), methodNode);
    const std::vector<jbytecode::analysis::Frame<jbytecode::analysis::SourceValue>*>& frames =
        snapshot.frames();

    if (frames.empty() || frames.size() != static_cast<std::size_t>(insns.size())) {
        return PreservedAnalyses::all();
    }

    //建立指令到帧的映射
    std::unordered_map<AbstractInsnNode*,
                       jbytecode::analysis::Frame<jbytecode::analysis::SourceValue>*>
        frameMap;
    for (std::size_t i = 0; i < frames.size(); i++) {
        if (frames[i]) {
            frameMap[insns.get(static_cast<int>(i))] = frames[i];
        }
    }

    //记录替换映射，用于追溯已折叠的指令
    std::unordered_map<AbstractInsnNode*, AbstractInsnNode*> replace;
    //已从指令表摘除、待释放的节点（set 语义：同一节点只允许入队一次，
    //双重 remove 会让 InsnList 重复摘链破坏 first_/last_，二次 delete 崩溃）
    std::unordered_set<AbstractInsnNode*> deleted;
    bool                                  modified = false;

    //生产者消费计数：仅统计真正弹出该栈槽的指令（帧 i 的消费区间 = 栈顶
    //getConsumeStackCount(op) 个槽）。直线代码里未被消费的槽位会原样延续到
    //后续帧，按帧出现次数统计会把同一次消费重复计入，误杀全部折叠。
    //replace 链新产的常量不在任何帧中（计数 0），其唯一消费者是经 replace
    //追溯到它的后续折叠，配合 deleted 集合排除多消费。
    auto producerShared = [&frameMap](const AbstractInsnNode* producer) {
        std::size_t uses = 0;
        for (const auto& [insn, frame] : frameMap) {
            const int consume =
                std::max(0, InsnUtil::getConsumeStackCount(insn->getOpcode()));
            if (consume == 0) continue;
            const int size  = frame->getStackSize();
            const int begin = std::max(0, size - consume);
            for (int s = begin; s < size; ++s) {
                if (frame->getStack(s).insns_.contains(producer)) ++uses;
            }
        }
        return uses > 1;
    };

    //遍历指令快照，寻找可折叠的整型算术运算
    auto instructions = insns.toArray();
    for (auto* ain : instructions) {
        int op = ain->getOpcode();
        if (op != Opcodes::IADD && op != Opcodes::ISUB && op != Opcodes::IMUL &&
            op != Opcodes::IDIV && op != Opcodes::IREM && op != Opcodes::IXOR) {
            continue;
        }

        auto fit = frameMap.find(ain);
        if (fit == frameMap.end()) continue;
        auto* f = fit->second;
        if (f->getStackSize() < 2) continue;

        const jbytecode::analysis::SourceValue& arg1 = f->getStack(f->getStackSize() - 1);
        const jbytecode::analysis::SourceValue& arg2 = f->getStack(f->getStackSize() - 2);

        //每个操作数必须由唯一指令产生
        if (arg1.insns_.size() != 1 || arg2.insns_.size() != 1) continue;

        AbstractInsnNode* a1 = const_cast<AbstractInsnNode*>(arg1.insns_[0]);
        AbstractInsnNode* a2 = const_cast<AbstractInsnNode*>(arg2.insns_[0]);

        //沿替换链追溯到最终指令（中间结果可能已被替换）
        auto rt1 = replace.find(a1);
        if (rt1 != replace.end()) a1 = rt1->second;
        auto rt2 = replace.find(a2);
        if (rt2 != replace.end()) a2 = rt2->second;

        //操作数已在先前折叠中被摘除：其栈槽随之消失，本候选随之失效
        if (deleted.count(a1) || deleted.count(a2)) continue;

        //检查是否两个操作数都是已知整数常量
        int v1, v2;
        if (!InsnUtil::getIntValue(a1, v1) || !InsnUtil::getIntValue(a2, v2)) continue;

        //共享生产者只放弃折叠，不删生产者（保守，正确性优先）
        if (producerShared(a1) || producerShared(a2)) continue;

        //统一走 MathHelper，避免 C++ 有符号溢出和 MIN / -1 与 JVM 语义不一致。
        auto folded = MathHelper::binaryAny(op, ca::core::Any(v2), ca::core::Any(v1));
        auto* result = folded.as<int>();
        if (!result) continue;

        auto* newValue = InsnUtil::getIntInsn(insns, *result);
        replace[ain]   = newValue;

        //替换算术指令为结果常量，删除操作数指令
        insns.set(ain, newValue);
        insns.remove(a1);
        insns.remove(a2);
        deleted.insert(a1);
        deleted.insert(a2);
        modified = true;
    }

    //遍历中的指令已从列表中移除，现在释放内存（arena 节点交由 arena 回收，B7）
    for (auto* node : deleted) {
        if (!methodNode.arena().owns(node)) delete node;
    }

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses LocalConstantFolder::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    auto& insns = methodNode.instructions;
    if (insns.isEmpty()) return PreservedAnalyses::all();

    //使用 ConstantInterpreter 进行常量传播帧分析。
    //统一走 DataflowAnalyzer 引擎一次性入口（analyzeFrames），失败返回空帧 fail-open。
    BasicReferenceHandler   refHandler;
    ConstantInterpreter     interpreter(&refHandler);
    const auto              snapshot = analyzeFrames<ConstantValue>(
        &interpreter, ca::str::ZUtf8StringRef::from_static("T"), methodNode);
    const std::vector<ConstantFrame*>& frames = snapshot.frames();

    if (frames.empty()) return PreservedAnalyses::all();
    if (frames.size() != static_cast<std::size_t>(insns.size())) return PreservedAnalyses::all();

    auto insnArray = insns.toArray();
    int  arrayLen  = static_cast<int>(insnArray.size());
    int  frameLen  = static_cast<int>(frames.size());

    //划分基本块
    auto blocks = buildBlocks(insnArray.data(), arrayLen);
    if (blocks.empty()) return PreservedAnalyses::all();

    //收集折叠映射（使用指令索引，不涉及 InsnList 内存管理）
    std::unordered_map<int, FoldResult> foldMap;
    for (const auto& block : blocks) {
        scanAndCollectFold(insnArray.data(), arrayLen, frames.data(), frameLen, block, foldMap,
                           methodNode.arena());
    }

    if (foldMap.empty()) return PreservedAnalyses::all();

    //在指令列表中就地执行折叠替换（从头到尾遍历，从尾到头替换以避免索引偏移）
    //构建一个替换映射：原始指令列表中被替换的索引区间 → 替换指令列表
    //采用倒序遍历 foldMap 进行插入/删除
    std::vector<std::pair<int, FoldResult>> sortedFolds(foldMap.begin(), foldMap.end());
    std::sort(sortedFolds.begin(), sortedFolds.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });

    for (const auto& [startIdx, fold] : sortedFolds) {
        int endIdx = fold.endExclusive;
        //在原列表中 endIdx-1 处插入替换指令，然后删除 [startIdx, endIdx)
        //先插入替换指令
        for (auto* repl : fold.replacement) {
            insns.insertBefore(insnArray[startIdx], repl);
        }
        //删除原区间内的指令（从后往前删以避免索引问题）
        for (int i = endIdx - 1; i >= startIdx; i--) {
            insns.removeAndDelete(insnArray[i]);
        }
    }

    return PreservedAnalyses::none();
}

PreservedAnalyses ConstantFolder::run(jbytecode::tree::MethodNode& methodNode,
                                      MethodAnalysisManager&       am)
{
    (void)am;
    auto& insns = methodNode.instructions;
    if (insns.isEmpty()) return PreservedAnalyses::all();

    bool anyModified = false;

    //外层 do-while：迭代到指令数不动点（对齐 Java run 的 startSize/endSize 循环）。
    int startSize, endSize;
    do {
        startSize = insns.size();

        //指令快照（对齐 Java Arrays.asList(methodNode.instructions.toArray)）。
        auto instructions = insns.toArray();
        int  arrayLen     = static_cast<int>(instructions.size());

        //1. 粗扫描
        auto ranges = acfRoughScanningRange(instructions);
        if (ranges.empty()) break;

        //2. ConstantInterpreter 帧分析（统一走 DataflowAnalyzer 引擎一次性入口）。
        BasicReferenceHandler   refHandler;
        ConstantInterpreter     interpreter(&refHandler);
        const auto              snapshot = analyzeFrames<ConstantValue>(
            &interpreter, ca::str::ZUtf8StringRef::from_static("T"), methodNode);
        const std::vector<ConstantFrame*>& frames = snapshot.frames();
        if (frames.empty() || frames.size() != instructions.size()) break;
        int frameLen = static_cast<int>(frames.size());

        //3. 仅收集可独立证明的候选区间。Frame 的栈按值而非 JVM 槽位计数，不能把
        //InsnUtil 的槽位消耗数混入范围收紧；否则 DUP2/long/double 会被错误判断。
        std::vector<AcfFold> folds;
        for (const auto& range : ranges) {
            acfCollectFolds(range, instructions, frames.data(), frameLen, folds);
        }
        if (folds.empty()) {
            endSize = insns.size();
            continue;
        }
        //倒序处理（按 begin 从大到小），保证前面区间的下标不受后面删除影响。
        std::sort(folds.begin(), folds.end(), [](const AcfFold& a, const AcfFold& b) {
            return a.begin > b.begin;
        });
        for (auto& fold : folds) {
            AbstractInsnNode* startNode = instructions[static_cast<std::size_t>(fold.begin)];
            //在 startNode 前插入等价常量。
            for (auto* equiv : fold.replacement) {
                insns.insertBefore(startNode, equiv);
            }
            //删除原区间 [begin, end) 的节点：共 (end - begin) 条，从 startNode 往后数。
            //（不靠 endNode 指针判定，因为区间可能延伸到列表尾，endNode 为空。）
            auto* node     = startNode;
            int   toDelete = fold.end - fold.begin;
            for (int k = 0; k < toDelete && node != nullptr; k++) {
                auto* next = node->getNext();
                insns.removeAndDelete(node);
                node = next;
            }
        }
        anyModified = true;

        endSize = insns.size();
    } while (endSize < startSize);

    return anyModified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses ArithmeticSimplifier::run(jbytecode::tree::MethodNode& methodNode,
                                            MethodAnalysisManager&       context)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    if (std::getenv("MJT_ARITH_DISABLE")) return PreservedAnalyses::all();
    const bool changed = applyArithUntilConverge(getArithmeticMatcher(), methodNode.instructions, context.arena());
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
