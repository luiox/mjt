#include "mbase/pass/opt/type_cast_simplifier.hpp"

#include "mbase/asm/insn/insn_util.hpp"
#include "mbase/asm/matcher/pattern_matcher.hpp"
#include "mbase/asm/matcher/step_util.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/jbytecode.hpp"
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

//============================================================================
//TypeCastSimplifier 私有实现
//PatternMatcher 驱动：折叠 primitive 窄化（i2b/i2c/i2s）、浮点转整数、以及白名单 helper
//（Float.intBitsToFloat、Double.longBitsToDouble、Integer.parseInt、Long.parseUnsignedLong）。
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
static void addUnary(PatternMatcher& m, IMatchStep* load, IMatchStep* cast, Fn build)
{
    m.addRule(std::make_unique<MatchRule>(std::vector<IMatchStep*>{load, cast},
                                          std::make_unique<LambdaStrategy>(std::move(build))));
}

template<typename Integer, typename Floating>
static Integer javaFloatingToInteger(Floating value)
{
    if (std::isnan(value)) return 0;
    const auto minimum = static_cast<Floating>(std::numeric_limits<Integer>::lowest());
    const auto maximum = static_cast<Floating>(std::numeric_limits<Integer>::max());
    if (value <= minimum) return std::numeric_limits<Integer>::lowest();
    if (value >= maximum) return std::numeric_limits<Integer>::max();
    return static_cast<Integer>(value);
}

static int javaIntToByte(ca::i32 value)
{
    value &= 0xFF;
    return value >= 0x80 ? value - 0x100 : value;
}

static int javaIntToChar(ca::i32 value)
{
    return value & 0xFFFF;
}

static int javaIntToShort(ca::i32 value)
{
    value &= 0xFFFF;
    return value >= 0x8000 ? value - 0x10000 : value;
}

static bool parseJavaIntStrict(const std::string& text, ca::i32& value)
{
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last  = first + text.size();
    if (*first == '+') {
        ++first;
        if (first == last) return false;
    }
    const auto result = std::from_chars(first, last, value, 10);
    return result.ec == std::errc{} && result.ptr == last;
}

static bool parseJavaUnsignedLongStrict(const std::string& text, ca::i32 radix, ca::i64& value)
{
    if (text.empty() || radix < 2 || radix > 36) return false;
    const char* first = text.data();
    const char* last  = first + text.size();
    if (*first == '-') return false;
    if (*first == '+') {
        ++first;
        if (first == last) return false;
    }
    ca::u64    parsed = 0;
    const auto result = std::from_chars(first, last, parsed, radix);
    if (result.ec != std::errc{} || result.ptr != last) return false;
    std::memcpy(&value, &parsed, sizeof(value));
    return true;
}

class ParseableIntStringStep : public IMatchStep
{
public:
    bool tryMatch(AbstractInsnNode* now, MatchContext&) override
    {
        std::string text;
        ca::i32     value = 0;
        return InsnUtil::getStringValue(now, text) && parseJavaIntStrict(text, value);
    }
};

class ParseableUnsignedLongRadixStep : public IMatchStep
{
public:
    bool tryMatch(AbstractInsnNode* now, MatchContext& ctx) override
    {
        if (ctx.matchedNodes.empty()) return false;
        std::string text;
        ca::i32     radix = 0;
        ca::i64     value = 0;
        return InsnUtil::getStringValue(ctx.matchedNodes[0], text) &&
               InsnUtil::getIntValue(now, radix) && parseJavaUnsignedLongStrict(text, radix, value);
    }
};

static PatternMatcher& getCastMatcher()
{
    static PatternMatcher m = [] {
        PatternMatcher m;

        //JVM 基本类型转换 (12种)
        addUnary(m, constInt(), StepUtil::i2l(), [](MatchContext& ctx) {
            int v;
            if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getLongInsn(static_cast<ca::i64>(v)));
        });
        addUnary(m, constInt(), StepUtil::i2f(), [](MatchContext& ctx) {
            int v;
            if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getFloatInsn(static_cast<float>(v)));
        });
        addUnary(m, constInt(), StepUtil::i2d(), [](MatchContext& ctx) {
            int v;
            if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getDoubleInsn(static_cast<double>(v)));
        });
        addUnary(m, constInt(), StepUtil::i2b(), [](MatchContext& ctx) {
            int v;
            if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getIntInsn(javaIntToByte(v)));
        });
        addUnary(m, constInt(), StepUtil::i2c(), [](MatchContext& ctx) {
            int v;
            if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getIntInsn(javaIntToChar(v)));
        });
        addUnary(m, constInt(), StepUtil::i2s(), [](MatchContext& ctx) {
            int v;
            if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getIntInsn(javaIntToShort(v)));
        });
        addUnary(m, constLong(), StepUtil::l2i(), [](MatchContext& ctx) {
            ca::i64 v;
            if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getIntInsn(static_cast<int>(v)));
        });
        addUnary(m, constLong(), StepUtil::l2f(), [](MatchContext& ctx) {
            ca::i64 v;
            if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getFloatInsn(static_cast<float>(v)));
        });
        addUnary(m, constLong(), StepUtil::l2d(), [](MatchContext& ctx) {
            ca::i64 v;
            if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getDoubleInsn(static_cast<double>(v)));
        });
        addUnary(m, constFloat(), StepUtil::f2i(), [](MatchContext& ctx) {
            float v;
            if (InsnUtil::getFloatValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getIntInsn(javaFloatingToInteger<int>(v)));
        });
        addUnary(m, constFloat(), StepUtil::f2l(), [](MatchContext& ctx) {
            float v;
            if (InsnUtil::getFloatValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getLongInsn(javaFloatingToInteger<ca::i64>(v)));
        });
        addUnary(m, constFloat(), StepUtil::f2d(), [](MatchContext& ctx) {
            float v;
            if (InsnUtil::getFloatValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getDoubleInsn(static_cast<double>(v)));
        });
        addUnary(m, constDouble(), StepUtil::d2i(), [](MatchContext& ctx) {
            double v;
            if (InsnUtil::getDoubleValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getIntInsn(javaFloatingToInteger<int>(v)));
        });
        addUnary(m, constDouble(), StepUtil::d2l(), [](MatchContext& ctx) {
            double v;
            if (InsnUtil::getDoubleValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getLongInsn(javaFloatingToInteger<ca::i64>(v)));
        });
        addUnary(m, constDouble(), StepUtil::d2f(), [](MatchContext& ctx) {
            double v;
            if (InsnUtil::getDoubleValue(ctx.matchedNodes[0], v))
                ctx.builder->add(InsnUtil::getFloatInsn(static_cast<float>(v)));
        });

        //方法级类型转换
        //load(I) + invokestatic Float.intBitsToFloat => load(F)
        addRule(m,
                {constInt(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Float"),
                                        ca::str::Utf8StringRef::from_cstr("intBitsToFloat"),
                                        ca::str::Utf8StringRef::from_cstr("(I)F"))},
                [](MatchContext& ctx) {
                    int v;
                    if (InsnUtil::getIntValue(ctx.matchedNodes[0], v)) {
                        float r;
                        memcpy(&r, &v, sizeof(r));
                        ctx.builder->add(InsnUtil::getFloatInsn(r));
                    }
                });

        //load(J) + invokestatic Double.longBitsToDouble => load(D)
        addRule(m,
                {constLong(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Double"),
                                        ca::str::Utf8StringRef::from_cstr("longBitsToDouble"),
                                        ca::str::Utf8StringRef::from_cstr("(J)D"))},
                [](MatchContext& ctx) {
                    ca::i64 v;
                    if (InsnUtil::getLongValue(ctx.matchedNodes[0], v)) {
                        double r;
                        memcpy(&r, &v, sizeof(r));
                        ctx.builder->add(InsnUtil::getDoubleInsn(r));
                    }
                });

        //load(Str) + invokestatic Integer.parseInt => load(I)
        addRule(
            m,
            {new ParseableIntStringStep(),
             StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                    ca::str::Utf8StringRef::from_cstr("parseInt"),
                                    ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;)I"))},
            [](MatchContext& ctx) {
                std::string s;
                if (InsnUtil::getStringValue(ctx.matchedNodes[0], s)) {
                    ca::i32 value = 0;
                    if (parseJavaIntStrict(s, value)) ctx.builder->add(InsnUtil::getIntInsn(value));
                }
            });

        //load(Str) + load(radix) + invokestatic Long.parseUnsignedLong(string, radix) => load(J)
        addRule(
            m,
            {StepUtil::loadStr(),
             new ParseableUnsignedLongRadixStep(),
             StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                    ca::str::Utf8StringRef::from_cstr("parseUnsignedLong"),
                                    ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;I)J"))},
            [](MatchContext& ctx) {
                std::string s;
                ca::i32     radix = 0;
                ca::i64     value = 0;
                if (InsnUtil::getStringValue(ctx.matchedNodes[0], s) &&
                    InsnUtil::getIntValue(ctx.matchedNodes[1], radix) &&
                    parseJavaUnsignedLongStrict(s, radix, value))
                    ctx.builder->add(InsnUtil::getLongInsn(value));
            });

        return m;
    }();
    return m;
}

}   //namespace

PreservedAnalyses TypeCastSimplifier::run(jbytecode::tree::MethodNode& methodNode,
                                          MethodAnalysisManager&       context)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    const bool changed = applyUntilConverge(getCastMatcher(), methodNode.instructions, context.arena());
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
