#include "mbase/pass/opt/intrinsic_folding.hpp"

#include "mbase/asm/insn/insn_util.hpp"
#include "mbase/asm/matcher/pattern_matcher.hpp"
#include "mbase/asm/matcher/step_util.hpp"
#include "mbase/asm/tmatcher/combinators.hpp"
#include "mbase/asm/tmatcher/dsl.hpp"
#include "mbase/asm/tmatcher/insn_match_replacer.hpp"
#include "mbase/asm/tmatcher/replace_context.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/tree/insn_list.hpp"
#include "jstd/random.hpp"
#include "libca/core/cast.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <memory>

namespace mt  = mjt::tmatcher;
namespace mtc = mjt::tmatcher::combinators;

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::commons;
using namespace jbytecode::util;
using ca::str::ZUtf8StringRef;

//============================================================================
//BoxUnboxEliminator
//功能: 消除装箱后立即拆箱的冗余序列。
//模式: Integer.valueOf(int) + Integer.intValue => 删除两者
//等 8 种包装类型。
//C++ 独有, Java 无独立 pass。
//注意: 使用 InsnMatchReplacer（tmatcher 高层组合，又匹配又替换），与旧 box_unbox_pass 一致。
//============================================================================

static mt::InsnMatchReplacer& getBoxUnboxMatcher()
{
    static auto matcher = [] {
        mt::InsnMatchReplacer m;
        auto            remove = [](mt::ReplaceContext& ctx) { ctx.replaceWithPop(); };

        m.addRule(mtc::invokestatic("java/lang/Integer", "valueOf", "(I)Ljava/lang/Integer;") -
                      mtc::invokevirtual("java/lang/Integer", "intValue", "()I") >>
                  remove);
        m.addRule(mtc::invokestatic("java/lang/Long", "valueOf", "(J)Ljava/lang/Long;") -
                      mtc::invokevirtual("java/lang/Long", "longValue", "()J") >>
                  remove);
        m.addRule(mtc::invokestatic("java/lang/Float", "valueOf", "(F)Ljava/lang/Float;") -
                      mtc::invokevirtual("java/lang/Float", "floatValue", "()F") >>
                  remove);
        m.addRule(mtc::invokestatic("java/lang/Double", "valueOf", "(D)Ljava/lang/Double;") -
                      mtc::invokevirtual("java/lang/Double", "doubleValue", "()D") >>
                  remove);
        m.addRule(mtc::invokestatic("java/lang/Boolean", "valueOf", "(Z)Ljava/lang/Boolean;") -
                      mtc::invokevirtual("java/lang/Boolean", "booleanValue", "()Z") >>
                  remove);
        m.addRule(mtc::invokestatic("java/lang/Short", "valueOf", "(S)Ljava/lang/Short;") -
                      mtc::invokevirtual("java/lang/Short", "shortValue", "()S") >>
                  remove);
        m.addRule(mtc::invokestatic("java/lang/Byte", "valueOf", "(B)Ljava/lang/Byte;") -
                      mtc::invokevirtual("java/lang/Byte", "byteValue", "()B") >>
                  remove);
        m.addRule(mtc::invokestatic("java/lang/Character", "valueOf", "(C)Ljava/lang/Character;") -
                      mtc::invokevirtual("java/lang/Character", "charValue", "()C") >>
                  remove);

        return m;
    }();
    return matcher;
}

PreservedAnalyses BoxUnboxEliminator::run(jbytecode::tree::MethodNode& methodNode,
                                          MethodAnalysisManager&       context)
{
    (void)context;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    const auto beforeSize = methodNode.instructions.size();
    getBoxUnboxMatcher().apply(methodNode.instructions);
    const bool changed = methodNode.instructions.size() != beforeSize;
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//RandomSeedResolver
//对应 Java: replace/RandomSeedResolver.java
//功能: 将 `new Random(const_seed).nextXxx` 折叠为对应的常量值。
//
//Java 的真实匹配序列（对齐 Java matcher 的 5 个 step）：
//NEW java/util/Random ; 类型指令，desc == "java/util/Random"
//DUP ; 复制引用给 <init>
//LDC/LCONST <seed> ; 常量种子
//INVOKESPECIAL Random.<init>(J)V
//INVOKEVIRTUAL Random.nextXxxT ; nextInt/nextLong/nextFloat/nextDouble
//
//LCG 算法收敛于 libjstd（jstd::Random，唯一权威实现），对齐 java.util.Random：
//初始: seed = (seed ^ 0x5DEECE66DL) & ((1L << 48) - 1)
//next(b): seed = (seed * 0x5DEECE66DL + 0xBL) & ((1L << 48) - 1)
//return (int)(seed >>> (48 - b))
//nextInt = next(32)
//nextLong = ((long)next(32) << 32) + next(32)
//nextFloat= next(24) / ((float)(1 << 24))
//nextDouble= (((long)next(26) << 27) + next(27)) / (double)(1L << 53)
//====================================================================

namespace {

//NEW java/util/Random step：opcode == NEW 且 desc 匹配（Java 用 TypeInsnNode.desc）。
IMatchStep* newRandomStep()
{
    static auto* s = StepUtil::step(Opcodes::NEW, [](jbytecode::tree::AbstractInsnNode* insn) {
        if (insn->getType() != jbytecode::tree::AbstractInsnNode::TYPE_INSN) return false;
        auto* t = ca::core::tag_cast::cast<jbytecode::tree::TypeInsnNode*>(insn);
        return t->desc == ca::str::Utf8StringRef::from_cstr("java/util/Random");
    });
    return s;
}

//把一个 nextXxx 规则塞进 matcher：5 步固定，只有最后的 invokevirtual 不同。
template<class Calc>
void addRandomRule(PatternMatcher& m, const char* methodName, const char* methodDesc, Calc calc)
{
    m.addRule(std::make_unique<MatchRule>(
        std::vector<IMatchStep*>{
            newRandomStep(),
            StepUtil::dup(),
            StepUtil::loadLong(),
            StepUtil::invokespecial(ca::str::Utf8StringRef::from_cstr("java/util/Random"),
                                    ca::str::Utf8StringRef::from_cstr("<init>"),
                                    ca::str::Utf8StringRef::from_cstr("(J)V")),
            StepUtil::invokevirtual(ca::str::Utf8StringRef::from_cstr("java/util/Random"),
                                    ca::str::Utf8StringRef::from_cstr(methodName),
                                    ca::str::Utf8StringRef::from_cstr(methodDesc)),
        },
        std::make_unique<LambdaStrategy>([calc](MatchContext& ctx) {
            //ctx.matchedNodes[2] 是 loadLong 步骤匹配到的指令（NEW=0, DUP=1, loadLong=2）
            ca::i64 seed;
            if (InsnUtil::getLongValue(ctx.matchedNodes[2], seed)) {
                jstd::Random rng(seed);
                calc(ctx, rng);
            }
        })));
}

}   //namespace

static PatternMatcher& getSeedMatcher()
{
    static PatternMatcher matcher = [] {
        PatternMatcher m;

        //new Random(seed).nextInt => const int
        addRandomRule(m, "nextInt", "()I", [](MatchContext& ctx, jstd::Random& rng) {
            ctx.builder->add(InsnUtil::getIntInsn(rng.nextInt()));
        });
        //new Random(seed).nextLong => const long
        addRandomRule(m, "nextLong", "()J", [](MatchContext& ctx, jstd::Random& rng) {
            ctx.builder->add(InsnUtil::getLongInsn(rng.nextLong()));
        });
        //new Random(seed).nextFloat => const float
        addRandomRule(m, "nextFloat", "()F", [](MatchContext& ctx, jstd::Random& rng) {
            ctx.builder->add(InsnUtil::getFloatInsn(rng.nextFloat()));
        });
        //new Random(seed).nextDouble => const double
        addRandomRule(m, "nextDouble", "()D", [](MatchContext& ctx, jstd::Random& rng) {
            ctx.builder->add(InsnUtil::getDoubleInsn(rng.nextDouble()));
        });
        //nextBoolean consumes one Random bit and returns a JVM int boolean.
        addRandomRule(m, "nextBoolean", "()Z", [](MatchContext& ctx, jstd::Random& rng) {
            ctx.builder->add(InsnUtil::getIntInsn(rng.next(1)));
        });

        return m;
    }();
    return matcher;
}

PreservedAnalyses RandomSeedResolver::run(MethodNode& methodNode, MethodAnalysisManager& context)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    bool changed = false;
    while (getSeedMatcher().apply(methodNode.instructions, context.arena())) changed = true;
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//StdConsFunctionOptimizer
//对应 Java: StdConsFunctionOptimizer
//功能: 对已知 JDK 纯函数（String.length, Math.abs, Integer.parseInt 等）
//用常量参数调用时，折叠为常量结果。
//注意: 只折叠白名单纯函数；parse 失败或参数不是常量时保持原样。
//====================================================================

//原始 Java 注释:
//"方法调用优化 — 针对确定的无副作用的函数调用，在参数为常量时计算结果"
template<typename Fn>
static void addFuncRule2(PatternMatcher& m, std::vector<IMatchStep*> steps, Fn strategyFn)
{
    m.addRule(std::make_unique<MatchRule>(std::move(steps),
                                          std::make_unique<LambdaStrategy>(std::move(strategyFn))));
}

template<typename Fn>
static void addUnary(PatternMatcher& m, IMatchStep* load, IMatchStep* inv, Fn calc)
{
    addFuncRule2(m, {load, inv}, [calc](MatchContext& ctx) { calc(ctx); });
}

template<typename T>
static T javaFloatingMax(T a, T b)
{
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == static_cast<T>(0) && b == static_cast<T>(0)) {
        return (!std::signbit(a) || !std::signbit(b)) ? static_cast<T>(0) : a;
    }
    return a >= b ? a : b;
}

template<typename T>
static T javaFloatingMin(T a, T b)
{
    if (std::isnan(a)) return a;
    if (std::isnan(b)) return b;
    if (a == static_cast<T>(0) && b == static_cast<T>(0)) {
        return (std::signbit(a) || std::signbit(b)) ? -static_cast<T>(0) : a;
    }
    return a <= b ? a : b;
}

static bool parseJavaIntStrict(const std::string& text, int& out)
{
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last  = first + text.size();
    if (*first == '+') {
        ++first;
        if (first == last) return false;
    }
    int  value  = 0;
    auto result = std::from_chars(first, last, value, 10);
    if (result.ec != std::errc{} || result.ptr != last) return false;
    out = value;
    return true;
}

static bool parseJavaLongStrict(const std::string& text, ca::i64& out)
{
    if (text.empty()) return false;
    const char* first = text.data();
    const char* last  = first + text.size();
    if (*first == '+') {
        ++first;
        if (first == last) return false;
    }
    ca::i64 value  = 0;
    auto    result = std::from_chars(first, last, value, 10);
    if (result.ec != std::errc{} || result.ptr != last) return false;
    out = value;
    return true;
}

//============================================================================
//java.lang.Long / java.lang.Integer 位运算辅助 (issue )
//C++17 便携实现，语义对齐 JDK Long/Integer。
//============================================================================

static inline ca::i64 javaLongRotateLeft(ca::i64 v, int d)
{
    using U           = ca::u64;
    constexpr int N   = 64;
    int           cnt = d & (N - 1);
    if (cnt == 0) return v;
    U u = static_cast<U>(v);
    return static_cast<ca::i64>((u << cnt) | (u >> (N - cnt)));
}
static inline ca::i64 javaLongRotateRight(ca::i64 v, int d)
{
    using U           = ca::u64;
    constexpr int N   = 64;
    int           cnt = d & (N - 1);
    if (cnt == 0) return v;
    U u = static_cast<U>(v);
    return static_cast<ca::i64>((u >> cnt) | (u << (N - cnt)));
}
static inline int javaIntRotateLeft(int v, int d)
{
    using U           = ca::u32;
    constexpr int N   = 32;
    int           cnt = d & (N - 1);
    if (cnt == 0) return v;
    U u = static_cast<U>(static_cast<ca::i32>(v));
    return static_cast<int>(static_cast<ca::i32>((u << cnt) | (u >> (N - cnt))));
}
static inline int javaIntRotateRight(int v, int d)
{
    using U           = ca::u32;
    constexpr int N   = 32;
    int           cnt = d & (N - 1);
    if (cnt == 0) return v;
    U u = static_cast<U>(static_cast<ca::i32>(v));
    return static_cast<int>(static_cast<ca::i32>((u >> cnt) | (u << (N - cnt))));
}

//reverse bits (SWAR 分治)
static inline ca::i64 javaLongReverse(ca::i64 v)
{
    ca::u64 x = static_cast<ca::u64>(v);
    x = ((x >> 1) & 0x5555555555555555ULL) | ((x & 0x5555555555555555ULL) << 1);
    x = ((x >> 2) & 0x3333333333333333ULL) | ((x & 0x3333333333333333ULL) << 2);
    x = ((x >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((x & 0x0F0F0F0F0F0F0F0FULL) << 4);
    x = ((x >> 8) & 0x00FF00FF00FF00FFULL) | ((x & 0x00FF00FF00FF00FFULL) << 8);
    x = ((x >> 16) & 0x0000FFFF0000FFFFULL) | ((x & 0x0000FFFF0000FFFFULL) << 16);
    x = (x >> 32) | (x << 32);
    return static_cast<ca::i64>(x);
}
static inline int javaIntReverse(int v)
{
    ca::u32 x = static_cast<ca::u32>(static_cast<ca::i32>(v));
    x = ((x >> 1) & 0x55555555U) | ((x & 0x55555555U) << 1);
    x = ((x >> 2) & 0x33333333U) | ((x & 0x33333333U) << 2);
    x = ((x >> 4) & 0x0F0F0F0FU) | ((x & 0x0F0F0F0FU) << 4);
    x = ((x >> 8) & 0x00FF00FFU) | ((x & 0x00FF00FFU) << 8);
    x = (x >> 16) | (x << 16);
    return static_cast<int>(static_cast<ca::i32>(x));
}

//reverse bytes
static inline ca::i64 javaLongReverseBytes(ca::i64 v)
{
    ca::u64 x = static_cast<ca::u64>(v);
    x = ((x >> 8) & 0x00FF00FF00FF00FFULL) | ((x & 0x00FF00FF00FF00FFULL) << 8);
    x = ((x >> 16) & 0x0000FFFF0000FFFFULL) | ((x & 0x0000FFFF0000FFFFULL) << 16);
    x = (x >> 32) | (x << 32);
    return static_cast<ca::i64>(x);
}
static inline int javaIntReverseBytes(int v)
{
    ca::u32 x = static_cast<ca::u32>(static_cast<ca::i32>(v));
    x = ((x >> 8) & 0x00FF00FFU) | ((x & 0x00FF00FFU) << 8);
    x = (x >> 16) | (x << 16);
    return static_cast<int>(static_cast<ca::i32>(x));
}

//bitCount (popcount, SWAR)
static inline int javaLongBitCount(ca::i64 v)
{
    ca::u64 x = static_cast<ca::u64>(v);
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    x = x + (x >> 8);
    x = x + (x >> 16);
    x = x + (x >> 32);
    return static_cast<int>(x & 0x7FULL);
}
static inline int javaIntBitCount(int v)
{
    ca::u32 x = static_cast<ca::u32>(static_cast<ca::i32>(v));
    x = x - ((x >> 1) & 0x55555555U);
    x = (x & 0x33333333U) + ((x >> 2) & 0x33333333U);
    x = (x + (x >> 4)) & 0x0F0F0F0FU;
    x = x + (x >> 8);
    x = x + (x >> 16);
    return static_cast<int>(x & 0x3FU);
}

//numberOfLeadingZeros (二分)
static inline int javaLongNumberOfLeadingZeros(ca::i64 v)
{
    if (v == 0) return 64;
    ca::u64 x = static_cast<ca::u64>(v);
    int     n = 0;
    if ((x >> 32) == 0) { n += 32; x <<= 32; }
    if ((x >> 48) == 0) { n += 16; x <<= 16; }
    if ((x >> 56) == 0) { n += 8; x <<= 8; }
    if ((x >> 60) == 0) { n += 4; x <<= 4; }
    if ((x >> 62) == 0) { n += 2; x <<= 2; }
    if ((x >> 63) == 0) { n += 1; }
    return n;
}
static inline int javaIntNumberOfLeadingZeros(int v)
{
    if (v == 0) return 32;
    ca::u32 x = static_cast<ca::u32>(static_cast<ca::i32>(v));
    int     n = 0;
    if ((x >> 16) == 0) { n += 16; x <<= 16; }
    if ((x >> 24) == 0) { n += 8; x <<= 8; }
    if ((x >> 28) == 0) { n += 4; x <<= 4; }
    if ((x >> 30) == 0) { n += 2; x <<= 2; }
    if ((x >> 31) == 0) { n += 1; }
    return n;
}

//numberOfTrailingZeros (二分)
static inline int javaLongNumberOfTrailingZeros(ca::i64 v)
{
    if (v == 0) return 64;
    ca::u64 x = static_cast<ca::u64>(v);
    int     n = 0;
    if ((x & 0xFFFFFFFFULL) == 0) { n += 32; x >>= 32; }
    if ((x & 0xFFFFULL) == 0) { n += 16; x >>= 16; }
    if ((x & 0xFFULL) == 0) { n += 8; x >>= 8; }
    if ((x & 0xFULL) == 0) { n += 4; x >>= 4; }
    if ((x & 0x3ULL) == 0) { n += 2; x >>= 2; }
    if ((x & 0x1ULL) == 0) { n += 1; }
    return n;
}
static inline int javaIntNumberOfTrailingZeros(int v)
{
    if (v == 0) return 32;
    ca::u32 x = static_cast<ca::u32>(static_cast<ca::i32>(v));
    int     n = 0;
    if ((x & 0xFFFFU) == 0) { n += 16; x >>= 16; }
    if ((x & 0xFFU) == 0) { n += 8; x >>= 8; }
    if ((x & 0xFU) == 0) { n += 4; x >>= 4; }
    if ((x & 0x3U) == 0) { n += 2; x >>= 2; }
    if ((x & 0x1U) == 0) { n += 1; }
    return n;
}

//highestOneBit / lowestOneBit
static inline ca::i64 javaLongHighestOneBit(ca::i64 v)
{
    if (v == 0) return 0;
    return static_cast<ca::i64>(static_cast<ca::u64>(1ULL) << (63 - javaLongNumberOfLeadingZeros(v)));
}
static inline int javaIntHighestOneBit(int v)
{
    if (v == 0) return 0;
    return static_cast<int>(1U << (31 - javaIntNumberOfLeadingZeros(v)));
}
//lowestOneBit: v & -v，但在无符号域计算取负以避免 MIN_VALUE 的有符号溢出 UB
static inline ca::i64 javaLongLowestOneBit(ca::i64 v)
{
    ca::u64 u = static_cast<ca::u64>(v);
    return static_cast<ca::i64>(u & (~u + 1));
}
static inline int javaIntLowestOneBit(int v)
{
    ca::u32 u = static_cast<ca::u32>(static_cast<ca::i32>(v));
    return static_cast<int>(static_cast<ca::i32>(u & (~u + 1)));
}

//signum (for Long)
static inline int javaSignum(ca::i64 v) { return (v > 0) ? 1 : (v < 0) ? -1 : 0; }

//toXxxString / toString (格式对齐 JDK Long/Integer)
//JDK Long.toBinaryString(-1) = "11111111...111" (64个1)
//JDK Long.toString(long, radix): radix<2||>36 时默认 10
static std::string javaLongToBinaryString(ca::i64 v)
{
    ca::u64 x = static_cast<ca::u64>(v);
    if (x == 0) return "0";
    std::string s;
    s.reserve(64);
    while (x) { s.push_back((x & 1) ? '1' : '0'); x >>= 1; }
    std::reverse(s.begin(), s.end());
    return s;
}
static std::string javaLongToHexString(ca::i64 v)
{
    static const char hex[] = "0123456789abcdef";
    ca::u64          x = static_cast<ca::u64>(v);
    if (x == 0) return "0";
    std::string s;
    s.reserve(16);
    while (x) { s.push_back(hex[x & 0xFULL]); x >>= 4; }
    std::reverse(s.begin(), s.end());
    return s;
}
static std::string javaLongToOctalString(ca::i64 v)
{
    ca::u64 x = static_cast<ca::u64>(v);
    if (x == 0) return "0";
    std::string s;
    s.reserve(22);
    while (x) { s.push_back(static_cast<char>('0' + (x & 0x7ULL))); x >>= 3; }
    std::reverse(s.begin(), s.end());
    return s;
}
static std::string javaLongToString(ca::i64 v)
{
    if (v == 0) return "0";
    bool      neg = v < 0;
    ca::u64   x   = neg ? static_cast<ca::u64>(-(v + 1)) + 1 : static_cast<ca::u64>(v);
    std::string s;
    s.reserve(20);
    while (x) { s.push_back(static_cast<char>('0' + (x % 10))); x /= 10; }
    if (neg) s.push_back('-');
    std::reverse(s.begin(), s.end());
    return s;
}
static std::string javaLongToString(ca::i64 v, int radix)
{
    if (radix < 2 || radix > 36) radix = 10;
    if (v == 0) return "0";
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    bool      neg = v < 0;
    ca::u64   x   = neg ? static_cast<ca::u64>(-(v + 1)) + 1 : static_cast<ca::u64>(v);
    std::string s;
    s.reserve(64);
    while (x) { s.push_back(digits[x % static_cast<ca::u64>(radix)]); x /= static_cast<ca::u64>(radix); }
    if (neg) s.push_back('-');
    std::reverse(s.begin(), s.end());
    return s;
}
static std::string javaIntToBinaryString(int v)
{
    ca::u32 x = static_cast<ca::u32>(static_cast<ca::i32>(v));
    if (x == 0) return "0";
    std::string s;
    s.reserve(32);
    while (x) { s.push_back((x & 1) ? '1' : '0'); x >>= 1; }
    std::reverse(s.begin(), s.end());
    return s;
}
static std::string javaIntToHexString(int v)
{
    static const char hex[] = "0123456789abcdef";
    ca::u32          x = static_cast<ca::u32>(static_cast<ca::i32>(v));
    if (x == 0) return "0";
    std::string s;
    s.reserve(8);
    while (x) { s.push_back(hex[x & 0xFU]); x >>= 4; }
    std::reverse(s.begin(), s.end());
    return s;
}
static std::string javaIntToOctalString(int v)
{
    ca::u32 x = static_cast<ca::u32>(static_cast<ca::i32>(v));
    if (x == 0) return "0";
    std::string s;
    s.reserve(11);
    while (x) { s.push_back(static_cast<char>('0' + (x & 0x7U))); x >>= 3; }
    std::reverse(s.begin(), s.end());
    return s;
}
static std::string javaIntToString(int v)
{
    if (v == 0) return "0";
    bool     neg = v < 0;
    ca::u32  x   = neg ? static_cast<ca::u32>(-(v + 1)) + 1 : static_cast<ca::u32>(v);
    std::string s;
    s.reserve(11);
    while (x) { s.push_back(static_cast<char>('0' + (x % 10))); x /= 10; }
    if (neg) s.push_back('-');
    std::reverse(s.begin(), s.end());
    return s;
}
static std::string javaIntToString(int v, int radix)
{
    if (radix < 2 || radix > 36) radix = 10;
    if (v == 0) return "0";
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    bool     neg = v < 0;
    ca::u32  x   = neg ? static_cast<ca::u32>(-(v + 1)) + 1 : static_cast<ca::u32>(v);
    std::string s;
    s.reserve(32);
    while (x) { s.push_back(digits[x % static_cast<ca::u32>(radix)]); x /= static_cast<ca::u32>(radix); }
    if (neg) s.push_back('-');
    std::reverse(s.begin(), s.end());
    return s;
}

static IMatchStep* constStringStep()
{
    static auto* step = new AnyStep([](AbstractInsnNode* n) {
        std::string value;
        return InsnUtil::getStringValue(n, value);
    });
    return step;
}

static IMatchStep* parseableIntStringStep()
{
    static auto* step = new AnyStep([](AbstractInsnNode* n) {
        std::string value;
        int         parsed = 0;
        return InsnUtil::getStringValue(n, value) && parseJavaIntStrict(value, parsed);
    });
    return step;
}

static IMatchStep* parseableLongStringStep()
{
    static auto* step = new AnyStep([](AbstractInsnNode* n) {
        std::string value;
        ca::i64     parsed = 0;
        return InsnUtil::getStringValue(n, value) && parseJavaLongStrict(value, parsed);
    });
    return step;
}

static IMatchStep* constIntStep()
{
    static auto* step = new AnyStep([](AbstractInsnNode* n) {
        int value = 0;
        return InsnUtil::getIntValue(n, value);
    });
    return step;
}

static IMatchStep* constLongStep()
{
    static auto* step = new AnyStep([](AbstractInsnNode* n) {
        ca::i64 value = 0;
        return InsnUtil::getLongValue(n, value);
    });
    return step;
}

static IMatchStep* constFloatStep()
{
    static auto* step = new AnyStep([](AbstractInsnNode* n) {
        float value = 0.0f;
        return InsnUtil::getFloatValue(n, value);
    });
    return step;
}

static IMatchStep* constDoubleStep()
{
    static auto* step = new AnyStep([](AbstractInsnNode* n) {
        double value = 0.0;
        return InsnUtil::getDoubleValue(n, value);
    });
    return step;
}

class ValidSubstringEndStep : public IMatchStep
{
public:
    bool tryMatch(AbstractInsnNode* now, MatchContext& ctx) override
    {
        std::string text;
        int         start = 0;
        int         end   = 0;
        return ctx.matchedNodes.size() >= 2 &&
               InsnUtil::getStringValue(ctx.matchedNodes[0], text) &&
               InsnUtil::getIntValue(ctx.matchedNodes[1], start) &&
               InsnUtil::getIntValue(now, end) && start >= 0 && end >= start &&
               end <= static_cast<int>(text.size());
    }
};

static IMatchStep* validSubstringEndStep()
{
    static auto* step = new ValidSubstringEndStep();
    return step;
}

static PatternMatcher& getStdConsMatcher()
{
    static PatternMatcher matcher = [] {
        PatternMatcher m;

        //String.length on constant string
        addFuncRule2(m,
                     {constStringStep(),
                      StepUtil::invokevirtual(ca::str::Utf8StringRef::from_cstr("java/lang/String"),
                                              ca::str::Utf8StringRef::from_cstr("length"),
                                              ca::str::Utf8StringRef::from_cstr("()I"))},
                     [](MatchContext& ctx) {
                         std::string s;
                         if (InsnUtil::getStringValue(ctx.matchedNodes[0], s))
                             ctx.builder->add(InsnUtil::getIntInsn(static_cast<int>(s.length())));
                     });

        //String.isEmpty on constant string
        addFuncRule2(m,
                     {constStringStep(),
                      StepUtil::invokevirtual(ca::str::Utf8StringRef::from_cstr("java/lang/String"),
                                              ca::str::Utf8StringRef::from_cstr("isEmpty"),
                                              ca::str::Utf8StringRef::from_cstr("()Z"))},
                     [](MatchContext& ctx) {
                         std::string s;
                         if (InsnUtil::getStringValue(ctx.matchedNodes[0], s))
                             ctx.builder->add(InsnUtil::getIntInsn(s.empty() ? 1 : 0));
                     });

        //String.substring(start, end) with constant args
        addFuncRule2(
            m,
            {constStringStep(),
             constIntStep(),
             validSubstringEndStep(),
             StepUtil::invokevirtual(ca::str::Utf8StringRef::from_cstr("java/lang/String"),
                                     ca::str::Utf8StringRef::from_cstr("substring"),
                                     ca::str::Utf8StringRef::from_cstr("(II)Ljava/lang/String;"))},
            [](MatchContext& ctx) {
                std::string s;
                int         start, end;
                if (InsnUtil::getStringValue(ctx.matchedNodes[0], s) &&
                    InsnUtil::getIntValue(ctx.matchedNodes[1], start) &&
                    InsnUtil::getIntValue(ctx.matchedNodes[2], end)) {
                    //折叠结果经 ctx.arena intern 为 Utf8StringRef 存入 LdcInsnNode，
                    //生命周期由作业级 arena 托管，跨 ClassWriter 序列化安全。
                    const std::string sub = s.substr(start, end - start);
                    ctx.builder->add(new LdcInsnNode(ctx.internString(sub)));
                }
            });

        //String.valueOf(text.toCharArray) is content-equivalent to text.
        //The rule intentionally starts at toCharArray so the original receiver
        //producer remains on the stack while both library calls are removed.
        addFuncRule2(
            m,
            {StepUtil::invokevirtual(ca::str::Utf8StringRef::from_cstr("java/lang/String"),
                                     ca::str::Utf8StringRef::from_cstr("toCharArray"),
                                     ca::str::Utf8StringRef::from_cstr("()[C")),
             StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/String"),
                                    ca::str::Utf8StringRef::from_cstr("valueOf"),
                                    ca::str::Utf8StringRef::from_cstr("([C)Ljava/lang/String;"))},
            [](MatchContext& ctx) { (void)ctx; });

        //Math.abs(int) on constant
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                        ca::str::Utf8StringRef::from_cstr("abs"),
                                        ca::str::Utf8StringRef::from_cstr("(I)I")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v)) {
                         if (v == (std::numeric_limits<int>::min)()) {
                             ctx.builder->add(InsnUtil::getIntInsn(v));
                         }
                         else {
                             ctx.builder->add(InsnUtil::getIntInsn(v < 0 ? -v : v));
                         }
                     }
                 });

        //Math.abs(long) on constant
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                        ca::str::Utf8StringRef::from_cstr("abs"),
                                        ca::str::Utf8StringRef::from_cstr("(J)J")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v)) {
                         if (v == (std::numeric_limits<ca::i64>::min)()) {
                             ctx.builder->add(InsnUtil::getLongInsn(v));
                         }
                         else {
                             ctx.builder->add(InsnUtil::getLongInsn(v < 0 ? -v : v));
                         }
                     }
                 });

        //Math.abs(float/double) on constants. std::fabs preserves the useful
        //Java edge behavior for NaN and signed zero in these unary cases.
        addUnary(m,
                 constFloatStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                        ca::str::Utf8StringRef::from_cstr("abs"),
                                        ca::str::Utf8StringRef::from_cstr("(F)F")),
                 [](MatchContext& ctx) {
                     float v;
                     if (InsnUtil::getFloatValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getFloatInsn(std::fabs(v)));
                 });

        addUnary(m,
                 constDoubleStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                        ca::str::Utf8StringRef::from_cstr("abs"),
                                        ca::str::Utf8StringRef::from_cstr("(D)D")),
                 [](MatchContext& ctx) {
                     double v;
                     if (InsnUtil::getDoubleValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getDoubleInsn(std::fabs(v)));
                 });

        //Math.max(int, int)
        addFuncRule2(m,
                     {constIntStep(),
                      constIntStep(),
                      StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                             ca::str::Utf8StringRef::from_cstr("max"),
                                             ca::str::Utf8StringRef::from_cstr("(II)I"))},
                     [](MatchContext& ctx) {
                         int a, b;
                         if (InsnUtil::getIntValue(ctx.matchedNodes[0], a) &&
                             InsnUtil::getIntValue(ctx.matchedNodes[1], b))
                             ctx.builder->add(InsnUtil::getIntInsn((std::max)(a, b)));
                     });

        //Math.max(long/float/double)
        addFuncRule2(m,
                     {constLongStep(),
                      constLongStep(),
                      StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                             ca::str::Utf8StringRef::from_cstr("max"),
                                             ca::str::Utf8StringRef::from_cstr("(JJ)J"))},
                     [](MatchContext& ctx) {
                         ca::i64 a, b;
                         if (InsnUtil::getLongValue(ctx.matchedNodes[0], a) &&
                             InsnUtil::getLongValue(ctx.matchedNodes[1], b))
                             ctx.builder->add(InsnUtil::getLongInsn((std::max)(a, b)));
                     });

        addFuncRule2(m,
                     {constFloatStep(),
                      constFloatStep(),
                      StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                             ca::str::Utf8StringRef::from_cstr("max"),
                                             ca::str::Utf8StringRef::from_cstr("(FF)F"))},
                     [](MatchContext& ctx) {
                         float a, b;
                         if (InsnUtil::getFloatValue(ctx.matchedNodes[0], a) &&
                             InsnUtil::getFloatValue(ctx.matchedNodes[1], b))
                             ctx.builder->add(InsnUtil::getFloatInsn(javaFloatingMax(a, b)));
                     });

        addFuncRule2(m,
                     {constDoubleStep(),
                      constDoubleStep(),
                      StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                             ca::str::Utf8StringRef::from_cstr("max"),
                                             ca::str::Utf8StringRef::from_cstr("(DD)D"))},
                     [](MatchContext& ctx) {
                         double a, b;
                         if (InsnUtil::getDoubleValue(ctx.matchedNodes[0], a) &&
                             InsnUtil::getDoubleValue(ctx.matchedNodes[1], b))
                             ctx.builder->add(InsnUtil::getDoubleInsn(javaFloatingMax(a, b)));
                     });

        //Math.min(int, int)
        addFuncRule2(m,
                     {constIntStep(),
                      constIntStep(),
                      StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                             ca::str::Utf8StringRef::from_cstr("min"),
                                             ca::str::Utf8StringRef::from_cstr("(II)I"))},
                     [](MatchContext& ctx) {
                         int a, b;
                         if (InsnUtil::getIntValue(ctx.matchedNodes[0], a) &&
                             InsnUtil::getIntValue(ctx.matchedNodes[1], b))
                             ctx.builder->add(InsnUtil::getIntInsn((std::min)(a, b)));
                     });

        //Math.min(long/float/double)
        addFuncRule2(m,
                     {constLongStep(),
                      constLongStep(),
                      StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                             ca::str::Utf8StringRef::from_cstr("min"),
                                             ca::str::Utf8StringRef::from_cstr("(JJ)J"))},
                     [](MatchContext& ctx) {
                         ca::i64 a, b;
                         if (InsnUtil::getLongValue(ctx.matchedNodes[0], a) &&
                             InsnUtil::getLongValue(ctx.matchedNodes[1], b))
                             ctx.builder->add(InsnUtil::getLongInsn((std::min)(a, b)));
                     });

        addFuncRule2(m,
                     {constFloatStep(),
                      constFloatStep(),
                      StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                             ca::str::Utf8StringRef::from_cstr("min"),
                                             ca::str::Utf8StringRef::from_cstr("(FF)F"))},
                     [](MatchContext& ctx) {
                         float a, b;
                         if (InsnUtil::getFloatValue(ctx.matchedNodes[0], a) &&
                             InsnUtil::getFloatValue(ctx.matchedNodes[1], b))
                             ctx.builder->add(InsnUtil::getFloatInsn(javaFloatingMin(a, b)));
                     });

        addFuncRule2(m,
                     {constDoubleStep(),
                      constDoubleStep(),
                      StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Math"),
                                             ca::str::Utf8StringRef::from_cstr("min"),
                                             ca::str::Utf8StringRef::from_cstr("(DD)D"))},
                     [](MatchContext& ctx) {
                         double a, b;
                         if (InsnUtil::getDoubleValue(ctx.matchedNodes[0], a) &&
                             InsnUtil::getDoubleValue(ctx.matchedNodes[1], b))
                             ctx.builder->add(InsnUtil::getDoubleInsn(javaFloatingMin(a, b)));
                     });

        //Integer.signum(int)
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("signum"),
                                        ca::str::Utf8StringRef::from_cstr("(I)I")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(v > 0 ? 1 : v < 0 ? -1 : 0));
                 });

        //Integer.parseInt(String) on constant string
        addFuncRule2(
            m,
            {parseableIntStringStep(),
             StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                    ca::str::Utf8StringRef::from_cstr("parseInt"),
                                    ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;)I"))},
            [](MatchContext& ctx) {
                std::string s;
                if (InsnUtil::getStringValue(ctx.matchedNodes[0], s)) {
                    int value = 0;
                    if (parseJavaIntStrict(s, value)) {
                        ctx.builder->add(InsnUtil::getIntInsn(value));
                    }
                }
            });

        //Long.parseLong(String)
        addFuncRule2(
            m,
            {parseableLongStringStep(),
             StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                    ca::str::Utf8StringRef::from_cstr("parseLong"),
                                    ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;)J"))},
            [](MatchContext& ctx) {
                std::string s;
                if (InsnUtil::getStringValue(ctx.matchedNodes[0], s)) {
                    ca::i64 value = 0;
                    if (parseJavaLongStrict(s, value)) {
                        ctx.builder->add(InsnUtil::getLongInsn(value));
                    }
                }
            });

//============================================================================
//java.lang.Long / java.lang.Integer 位运算和格式化常量折叠 (issue )
//============================================================================

        //── Long 一元 ──────────────────────────────────────────────────────

        //Long.reverse(J)J
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("reverse"),
                                        ca::str::Utf8StringRef::from_cstr("(J)J")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getLongInsn(javaLongReverse(v)));
                 });

        //Long.reverseBytes(J)J
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("reverseBytes"),
                                        ca::str::Utf8StringRef::from_cstr("(J)J")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getLongInsn(javaLongReverseBytes(v)));
                 });

        //Long.bitCount(J)I
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("bitCount"),
                                        ca::str::Utf8StringRef::from_cstr("(J)I")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(javaLongBitCount(v)));
                 });

        //Long.numberOfLeadingZeros(J)I
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("numberOfLeadingZeros"),
                                        ca::str::Utf8StringRef::from_cstr("(J)I")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(javaLongNumberOfLeadingZeros(v)));
                 });

        //Long.numberOfTrailingZeros(J)I
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("numberOfTrailingZeros"),
                                        ca::str::Utf8StringRef::from_cstr("(J)I")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(javaLongNumberOfTrailingZeros(v)));
                 });

        //Long.highestOneBit(J)J
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("highestOneBit"),
                                        ca::str::Utf8StringRef::from_cstr("(J)J")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getLongInsn(javaLongHighestOneBit(v)));
                 });

        //Long.lowestOneBit(J)J
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("lowestOneBit"),
                                        ca::str::Utf8StringRef::from_cstr("(J)J")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getLongInsn(javaLongLowestOneBit(v)));
                 });

        //Long.signum(J)I
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("signum"),
                                        ca::str::Utf8StringRef::from_cstr("(J)I")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(javaSignum(v)));
                 });

        //Long.toBinaryString(J)Ljava/lang/String;
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("toBinaryString"),
                                        ca::str::Utf8StringRef::from_cstr("(J)Ljava/lang/String;")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(new LdcInsnNode(ctx.internString(javaLongToBinaryString(v))));
                 });

        //Long.toHexString(J)Ljava/lang/String;
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("toHexString"),
                                        ca::str::Utf8StringRef::from_cstr("(J)Ljava/lang/String;")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(new LdcInsnNode(ctx.internString(javaLongToHexString(v))));
                 });

        //Long.toOctalString(J)Ljava/lang/String;
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("toOctalString"),
                                        ca::str::Utf8StringRef::from_cstr("(J)Ljava/lang/String;")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(new LdcInsnNode(ctx.internString(javaLongToOctalString(v))));
                 });

        //Long.toString(J)Ljava/lang/String;
        addUnary(m,
                 constLongStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                        ca::str::Utf8StringRef::from_cstr("toString"),
                                        ca::str::Utf8StringRef::from_cstr("(J)Ljava/lang/String;")),
                 [](MatchContext& ctx) {
                     ca::i64 v;
                     if (InsnUtil::getLongValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(new LdcInsnNode(ctx.internString(javaLongToString(v))));
                 });

        //── Long 二元 ──────────────────────────────────────────────────────

        //Long.rotateLeft(JI)J
        addFuncRule2(
            m,
            {constLongStep(), constIntStep(),
             StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                    ca::str::Utf8StringRef::from_cstr("rotateLeft"),
                                    ca::str::Utf8StringRef::from_cstr("(JI)J"))},
            [](MatchContext& ctx) {
                ca::i64 v;
                int     d;
                if (InsnUtil::getLongValue(ctx.matchedNodes[0], v) &&
                    InsnUtil::getIntValue(ctx.matchedNodes[1], d))
                    ctx.builder->add(InsnUtil::getLongInsn(javaLongRotateLeft(v, d)));
            });

        //Long.rotateRight(JI)J
        addFuncRule2(
            m,
            {constLongStep(), constIntStep(),
             StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                    ca::str::Utf8StringRef::from_cstr("rotateRight"),
                                    ca::str::Utf8StringRef::from_cstr("(JI)J"))},
            [](MatchContext& ctx) {
                ca::i64 v;
                int     d;
                if (InsnUtil::getLongValue(ctx.matchedNodes[0], v) &&
                    InsnUtil::getIntValue(ctx.matchedNodes[1], d))
                    ctx.builder->add(InsnUtil::getLongInsn(javaLongRotateRight(v, d)));
            });

        //Long.toString(JI)Ljava/lang/String;
        addFuncRule2(
            m,
            {constLongStep(), constIntStep(),
             StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Long"),
                                    ca::str::Utf8StringRef::from_cstr("toString"),
                                    ca::str::Utf8StringRef::from_cstr("(JI)Ljava/lang/String;"))},
            [](MatchContext& ctx) {
                ca::i64 v;
                int     radix;
                if (InsnUtil::getLongValue(ctx.matchedNodes[0], v) &&
                    InsnUtil::getIntValue(ctx.matchedNodes[1], radix))
                    ctx.builder->add(new LdcInsnNode(ctx.internString(javaLongToString(v, radix))));
            });

        //── Integer 一元 ───────────────────────────────────────────────────

        //Integer.reverse(I)I
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("reverse"),
                                        ca::str::Utf8StringRef::from_cstr("(I)I")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(javaIntReverse(v)));
                 });

        //Integer.reverseBytes(I)I
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("reverseBytes"),
                                        ca::str::Utf8StringRef::from_cstr("(I)I")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(javaIntReverseBytes(v)));
                 });

        //Integer.bitCount(I)I
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("bitCount"),
                                        ca::str::Utf8StringRef::from_cstr("(I)I")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(javaIntBitCount(v)));
                 });

        //Integer.numberOfLeadingZeros(I)I
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("numberOfLeadingZeros"),
                                        ca::str::Utf8StringRef::from_cstr("(I)I")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(javaIntNumberOfLeadingZeros(v)));
                 });

        //Integer.numberOfTrailingZeros(I)I
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("numberOfTrailingZeros"),
                                        ca::str::Utf8StringRef::from_cstr("(I)I")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(javaIntNumberOfTrailingZeros(v)));
                 });

        //Integer.highestOneBit(I)I
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("highestOneBit"),
                                        ca::str::Utf8StringRef::from_cstr("(I)I")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(javaIntHighestOneBit(v)));
                 });

        //Integer.lowestOneBit(I)I
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("lowestOneBit"),
                                        ca::str::Utf8StringRef::from_cstr("(I)I")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(InsnUtil::getIntInsn(javaIntLowestOneBit(v)));
                 });

        //Integer.signum(I)I — 已在 v675 前存在，此处不重复

        //Integer.toBinaryString(I)Ljava/lang/String;
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("toBinaryString"),
                                        ca::str::Utf8StringRef::from_cstr("(I)Ljava/lang/String;")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(new LdcInsnNode(ctx.internString(javaIntToBinaryString(v))));
                 });

        //Integer.toHexString(I)Ljava/lang/String;
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("toHexString"),
                                        ca::str::Utf8StringRef::from_cstr("(I)Ljava/lang/String;")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(new LdcInsnNode(ctx.internString(javaIntToHexString(v))));
                 });

        //Integer.toOctalString(I)Ljava/lang/String;
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("toOctalString"),
                                        ca::str::Utf8StringRef::from_cstr("(I)Ljava/lang/String;")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(new LdcInsnNode(ctx.internString(javaIntToOctalString(v))));
                 });

        //Integer.toString(I)Ljava/lang/String;
        addUnary(m,
                 constIntStep(),
                 StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                        ca::str::Utf8StringRef::from_cstr("toString"),
                                        ca::str::Utf8StringRef::from_cstr("(I)Ljava/lang/String;")),
                 [](MatchContext& ctx) {
                     int v;
                     if (InsnUtil::getIntValue(ctx.matchedNodes[0], v))
                         ctx.builder->add(new LdcInsnNode(ctx.internString(javaIntToString(v))));
                 });

        //── Integer 二元 ───────────────────────────────────────────────────

        //Integer.rotateLeft(II)I
        addFuncRule2(
            m,
            {constIntStep(), constIntStep(),
             StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                    ca::str::Utf8StringRef::from_cstr("rotateLeft"),
                                    ca::str::Utf8StringRef::from_cstr("(II)I"))},
            [](MatchContext& ctx) {
                int a, d;
                if (InsnUtil::getIntValue(ctx.matchedNodes[0], a) &&
                    InsnUtil::getIntValue(ctx.matchedNodes[1], d))
                    ctx.builder->add(InsnUtil::getIntInsn(javaIntRotateLeft(a, d)));
            });

        //Integer.rotateRight(II)I
        addFuncRule2(
            m,
            {constIntStep(), constIntStep(),
             StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                    ca::str::Utf8StringRef::from_cstr("rotateRight"),
                                    ca::str::Utf8StringRef::from_cstr("(II)I"))},
            [](MatchContext& ctx) {
                int a, d;
                if (InsnUtil::getIntValue(ctx.matchedNodes[0], a) &&
                    InsnUtil::getIntValue(ctx.matchedNodes[1], d))
                    ctx.builder->add(InsnUtil::getIntInsn(javaIntRotateRight(a, d)));
            });

        //Integer.toString(II)Ljava/lang/String;
        addFuncRule2(
            m,
            {constIntStep(), constIntStep(),
             StepUtil::invokestatic(ca::str::Utf8StringRef::from_cstr("java/lang/Integer"),
                                    ca::str::Utf8StringRef::from_cstr("toString"),
                                    ca::str::Utf8StringRef::from_cstr("(II)Ljava/lang/String;"))},
            [](MatchContext& ctx) {
                int v, radix;
                if (InsnUtil::getIntValue(ctx.matchedNodes[0], v) &&
                    InsnUtil::getIntValue(ctx.matchedNodes[1], radix))
                    ctx.builder->add(new LdcInsnNode(ctx.internString(javaIntToString(v, radix))));
            });

        return m;
    }();
    return matcher;
}

PreservedAnalyses StdConsFunctionOptimizer::run(MethodNode&            methodNode,
                                                MethodAnalysisManager& context)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    bool changed = false;
    //传入作业级 arena，使 String.substring 等折叠规则能分配生命周期安全的字符串常量。
    while (getStdConsMatcher().apply(methodNode.instructions, context.arena())) changed = true;
    return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
