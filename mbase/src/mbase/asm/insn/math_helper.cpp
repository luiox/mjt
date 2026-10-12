#include "mbase/asm/insn/math_helper.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <utility>

#include "jbytecode/jbytecode.hpp"

namespace mjt::base {

using namespace jbytecode::core;

bool MathHelper::isOneValueCondition(int opcode)
{
    return opcode == Opcodes::IFEQ || opcode == Opcodes::IFNE || opcode == Opcodes::IFLT ||
           opcode == Opcodes::IFGE || opcode == Opcodes::IFGT || opcode == Opcodes::IFLE;
}

bool MathHelper::condition(int value, int opcode)
{
    switch (opcode) {
    case Opcodes::IFEQ: return value == 0;
    case Opcodes::IFNE: return value != 0;
    case Opcodes::IFLT: return value < 0;
    case Opcodes::IFGE: return value >= 0;
    case Opcodes::IFGT: return value > 0;
    case Opcodes::IFLE: return value <= 0;
    default: return false;
    }
}

bool MathHelper::isTwoValuesCondition(int opcode)
{
    return (opcode >= Opcodes::IF_ICMPEQ && opcode <= Opcodes::IF_ICMPLE) ||
           (opcode >= Opcodes::IF_ACMPEQ && opcode <= Opcodes::IF_ACMPNE);
}

bool MathHelper::condition(int a, int b, int opcode)
{
    switch (opcode) {
    case Opcodes::IF_ICMPEQ: return a == b;
    case Opcodes::IF_ICMPNE: return a != b;
    case Opcodes::IF_ICMPLT: return a < b;
    case Opcodes::IF_ICMPGE: return a >= b;
    case Opcodes::IF_ICMPGT: return a > b;
    case Opcodes::IF_ICMPLE: return a <= b;
    default: return false;
    }
}

bool MathHelper::isBinaryOp(int opcode)
{
    switch (opcode) {
    case Opcodes::IADD:
    case Opcodes::ISUB:
    case Opcodes::IMUL:
    case Opcodes::IDIV:
    case Opcodes::IREM:
    case Opcodes::IAND:
    case Opcodes::IOR:
    case Opcodes::IXOR:
    case Opcodes::ISHL:
    case Opcodes::ISHR:
    case Opcodes::IUSHR:
    case Opcodes::LADD:
    case Opcodes::LSUB:
    case Opcodes::LMUL:
    case Opcodes::LDIV:
    case Opcodes::LREM:
    case Opcodes::LAND:
    case Opcodes::LOR:
    case Opcodes::LXOR:
    case Opcodes::LSHL:
    case Opcodes::LSHR:
    case Opcodes::LUSHR:
    case Opcodes::FADD:
    case Opcodes::FSUB:
    case Opcodes::FMUL:
    case Opcodes::FDIV:
    case Opcodes::FREM:
    case Opcodes::DADD:
    case Opcodes::DSUB:
    case Opcodes::DMUL:
    case Opcodes::DDIV:
    case Opcodes::DREM:
    case Opcodes::LCMP:
    case Opcodes::FCMPL:
    case Opcodes::FCMPG:
    case Opcodes::DCMPL:
    case Opcodes::DCMPG: return true;
    default: return false;
    }
}

double MathHelper::binaryOp(double a, double b, int opcode)
{
    int64_t ia = (int64_t)a, ib = (int64_t)b;
    switch (opcode) {
    case Opcodes::IADD:
    case Opcodes::LADD:
    case Opcodes::FADD:
    case Opcodes::DADD: return a + b;
    case Opcodes::ISUB:
    case Opcodes::LSUB:
    case Opcodes::FSUB:
    case Opcodes::DSUB: return a - b;
    case Opcodes::IMUL:
    case Opcodes::LMUL:
    case Opcodes::FMUL:
    case Opcodes::DMUL: return a * b;
    case Opcodes::IDIV:
    case Opcodes::LDIV:
    case Opcodes::FDIV:
    case Opcodes::DDIV: return b != 0 ? a / b : 0;
    case Opcodes::IREM:
    case Opcodes::LREM:
    case Opcodes::FREM:
    case Opcodes::DREM: return b != 0 ? fmod(a, b) : 0;
    case Opcodes::IAND:
    case Opcodes::LAND: return (double)(ia & ib);
    case Opcodes::IOR:
    case Opcodes::LOR: return (double)(ia | ib);
    case Opcodes::IXOR:
    case Opcodes::LXOR: return (double)(ia ^ ib);
    case Opcodes::ISHL:
    case Opcodes::LSHL: return (double)(ia << (ib & 0x3F));
    case Opcodes::ISHR:
    case Opcodes::LSHR: return (double)(ia >> (ib & 0x3F));
    case Opcodes::IUSHR: return (double)((uint64_t)ia >> (ib & 0x3F));
    case Opcodes::LUSHR: return (double)((uint64_t)ia >> (ib & 0x3F));
    default: return 0;
    }
}

bool MathHelper::isUnaryOp(int opcode)
{
    switch (opcode) {
    case Opcodes::INEG:
    case Opcodes::LNEG:
    case Opcodes::FNEG:
    case Opcodes::DNEG:
    case Opcodes::I2L:
    case Opcodes::I2F:
    case Opcodes::I2D:
    case Opcodes::I2B:
    case Opcodes::I2C:
    case Opcodes::I2S:
    case Opcodes::L2I:
    case Opcodes::L2F:
    case Opcodes::L2D:
    case Opcodes::F2I:
    case Opcodes::F2L:
    case Opcodes::F2D:
    case Opcodes::D2I:
    case Opcodes::D2L:
    case Opcodes::D2F: return true;
    default: return false;
    }
}

double MathHelper::unaryOp(double a, int opcode)
{
    int64_t ia = (int64_t)a;
    switch (opcode) {
    case Opcodes::INEG: return (double)(-ia);
    case Opcodes::LNEG: return (double)(-ia);
    case Opcodes::FNEG: return -(float)a;
    case Opcodes::DNEG: return -a;
    case Opcodes::I2L: return (double)(ia);
    case Opcodes::L2I: return (double)(int)ia;
    case Opcodes::I2F: return (float)ia;
    case Opcodes::F2I: return (double)(int)(float)a;
    case Opcodes::I2D: return (double)ia;
    case Opcodes::D2I: return (double)(int)a;
    case Opcodes::L2F: return (float)ia;
    case Opcodes::F2L: return (double)(int64_t)(float)a;
    case Opcodes::L2D: return (double)ia;
    case Opcodes::D2L: return (double)(int64_t)a;
    case Opcodes::F2D: return (float)a;
    case Opcodes::D2F: return (float)a;
    default: return a;
    }
}

int MathHelper::negateJumpOpcode(int opcode)
{
    switch (opcode) {
    case Opcodes::IFEQ: return Opcodes::IFNE;
    case Opcodes::IFNE: return Opcodes::IFEQ;
    case Opcodes::IFLT: return Opcodes::IFGE;
    case Opcodes::IFGE: return Opcodes::IFLT;
    case Opcodes::IFGT: return Opcodes::IFLE;
    case Opcodes::IFLE: return Opcodes::IFGT;
    case Opcodes::IF_ICMPEQ: return Opcodes::IF_ICMPNE;
    case Opcodes::IF_ICMPNE: return Opcodes::IF_ICMPEQ;
    case Opcodes::IF_ICMPLT: return Opcodes::IF_ICMPGE;
    case Opcodes::IF_ICMPGE: return Opcodes::IF_ICMPLT;
    case Opcodes::IF_ICMPGT: return Opcodes::IF_ICMPLE;
    case Opcodes::IF_ICMPLE: return Opcodes::IF_ICMPGT;
    default: return opcode;
    }
}

//---------------------------------------------------------------------------
//类型保留的 JVM 算术（Any 载荷）。实现沿用 constant_interpreter 中已验证
//的环绕算术、移位掩码与 MIN/-1 特例，集中到此处供所有常量折叠路径复用。
//---------------------------------------------------------------------------

namespace {

template<class SignedT, class UnsignedT>
SignedT fromUnsignedBits(UnsignedT value)
{
    static_assert(sizeof(SignedT) == sizeof(UnsignedT));
    SignedT result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

int javaIntAdd(int a, int b)
{
    return fromUnsignedBits<int>(static_cast<std::uint32_t>(a) +
                                 static_cast<std::uint32_t>(b));
}

int javaIntSub(int a, int b)
{
    return fromUnsignedBits<int>(static_cast<std::uint32_t>(a) -
                                 static_cast<std::uint32_t>(b));
}

int javaIntMul(int a, int b)
{
    return fromUnsignedBits<int>(static_cast<std::uint32_t>(a) *
                                 static_cast<std::uint32_t>(b));
}

std::int64_t javaLongAdd(std::int64_t a, std::int64_t b)
{
    return fromUnsignedBits<std::int64_t>(static_cast<std::uint64_t>(a) +
                                          static_cast<std::uint64_t>(b));
}

std::int64_t javaLongSub(std::int64_t a, std::int64_t b)
{
    return fromUnsignedBits<std::int64_t>(static_cast<std::uint64_t>(a) -
                                          static_cast<std::uint64_t>(b));
}

std::int64_t javaLongMul(std::int64_t a, std::int64_t b)
{
    return fromUnsignedBits<std::int64_t>(static_cast<std::uint64_t>(a) *
                                          static_cast<std::uint64_t>(b));
}

int javaIntShiftLeft(int value, int distance)
{
    const auto shift = static_cast<unsigned>(distance) & 0x1fU;
    return fromUnsignedBits<int>(static_cast<std::uint32_t>(value) << shift);
}

int javaIntShiftRight(int value, int distance)
{
    const auto shift = static_cast<unsigned>(distance) & 0x1fU;
    if (shift == 0) return value;
    auto bits = static_cast<std::uint32_t>(value) >> shift;
    if (value < 0) bits |= ~std::uint32_t{0} << (32U - shift);
    return fromUnsignedBits<int>(bits);
}

int javaIntUnsignedShiftRight(int value, int distance)
{
    const auto shift = static_cast<unsigned>(distance) & 0x1fU;
    return fromUnsignedBits<int>(static_cast<std::uint32_t>(value) >> shift);
}

std::int64_t javaLongShiftLeft(std::int64_t value, std::int64_t distance)
{
    const auto shift = static_cast<unsigned>(distance) & 0x3fU;
    return fromUnsignedBits<std::int64_t>(static_cast<std::uint64_t>(value) << shift);
}

std::int64_t javaLongShiftRight(std::int64_t value, std::int64_t distance)
{
    const auto shift = static_cast<unsigned>(distance) & 0x3fU;
    if (shift == 0) return value;
    auto bits = static_cast<std::uint64_t>(value) >> shift;
    if (value < 0) bits |= ~std::uint64_t{0} << (64U - shift);
    return fromUnsignedBits<std::int64_t>(bits);
}

std::int64_t javaLongUnsignedShiftRight(std::int64_t value, std::int64_t distance)
{
    const auto shift = static_cast<unsigned>(distance) & 0x3fU;
    return fromUnsignedBits<std::int64_t>(static_cast<std::uint64_t>(value) >> shift);
}

}   //namespace

std::pair<ca::core::Any, int> MathHelper::classifyValue(const ca::core::Any& v)
{
    if (auto* p = v.as<int>()) return {*p, 0};
    if (auto* p = v.as<bool>()) return {static_cast<int>(*p), 0};
    if (auto* p = v.as<short>()) return {static_cast<int>(*p), 0};
    if (auto* p = v.as<char>()) return {static_cast<int>(*p), 0};
    if (auto* p = v.as<unsigned short>()) return {static_cast<int>(*p), 0};
    if (auto* p = v.as<std::int8_t>()) return {static_cast<int>(*p), 0};
    if (auto* p = v.as<std::int16_t>()) return {static_cast<int>(*p), 0};
    if (auto* p = v.as<std::int64_t>()) return {*p, 1};
    if (auto* p = v.as<long long>()) {
        return {static_cast<std::int64_t>(*p), 1};
    }
    if (auto* p = v.as<float>()) return {*p, 2};
    if (auto* p = v.as<double>()) return {*p, 3};
    return {{}, -1};
}

ca::core::Any MathHelper::binaryAny(int opcode, const ca::core::Any& a, const ca::core::Any& b)
{
    if (!a.has_value() || !b.has_value()) return ca::core::Any{};

    auto [valA, kindA] = classifyValue(a);
    auto [valB, kindB] = classifyValue(b);
    const bool longShift =
        kindA == 1 && kindB == 0 &&
        (opcode == Opcodes::LSHL || opcode == Opcodes::LSHR || opcode == Opcodes::LUSHR);
    if (kindA < 0 || kindB < 0 || (kindA != kindB && !longShift)) return ca::core::Any{};

    if (kindA == 0) {
        int ia = valA.cast<int>();
        int ib = valB.cast<int>();
        switch (opcode) {
        case Opcodes::IADD: return javaIntAdd(ia, ib);
        case Opcodes::ISUB: return javaIntSub(ia, ib);
        case Opcodes::IMUL: return javaIntMul(ia, ib);
        case Opcodes::IDIV:
            if (ib == 0) return ca::core::Any{};
            if (ia == (std::numeric_limits<int>::min)() && ib == -1) return ia;
            return ia / ib;
        case Opcodes::IREM:
            if (ib == 0) return ca::core::Any{};
            if (ia == (std::numeric_limits<int>::min)() && ib == -1) return 0;
            return ia % ib;
        case Opcodes::ISHL: return javaIntShiftLeft(ia, ib);
        case Opcodes::ISHR: return javaIntShiftRight(ia, ib);
        case Opcodes::IUSHR: return javaIntUnsignedShiftRight(ia, ib);
        case Opcodes::IAND: return ia & ib;
        case Opcodes::IOR: return ia | ib;
        case Opcodes::IXOR: return ia ^ ib;
        default: return ca::core::Any{};
        }
    }
    if (kindA == 1) {
        std::int64_t la = valA.cast<std::int64_t>();
        std::int64_t lb = longShift ? static_cast<std::int64_t>(valB.cast<int>())
                                    : valB.cast<std::int64_t>();
        switch (opcode) {
        case Opcodes::LADD: return javaLongAdd(la, lb);
        case Opcodes::LSUB: return javaLongSub(la, lb);
        case Opcodes::LMUL: return javaLongMul(la, lb);
        case Opcodes::LDIV:
            if (lb == 0) return ca::core::Any{};
            if (la == (std::numeric_limits<std::int64_t>::min)() && lb == -1) return la;
            return la / lb;
        case Opcodes::LREM:
            if (lb == 0) return ca::core::Any{};
            if (la == (std::numeric_limits<std::int64_t>::min)() && lb == -1) {
                return std::int64_t{0};
            }
            return la % lb;
        case Opcodes::LSHL: return javaLongShiftLeft(la, lb);
        case Opcodes::LSHR: return javaLongShiftRight(la, lb);
        case Opcodes::LUSHR: return javaLongUnsignedShiftRight(la, lb);
        case Opcodes::LAND: return la & lb;
        case Opcodes::LOR: return la | lb;
        case Opcodes::LXOR: return la ^ lb;
        case Opcodes::LCMP:
            return static_cast<int>(la > lb ? 1 : (la < lb ? -1 : 0));
        default: return ca::core::Any{};
        }
    }
    if (kindA == 2) {
        float fa = valA.cast<float>();
        float fb = valB.cast<float>();
        switch (opcode) {
        case Opcodes::FADD: return fa + fb;
        case Opcodes::FSUB: return fa - fb;
        case Opcodes::FMUL: return fa * fb;
        case Opcodes::FDIV: return fa / fb;
        case Opcodes::FREM: return std::fmod(fa, fb);
        case Opcodes::FCMPL:
            if (std::isnan(fa) || std::isnan(fb)) return -1;
            return static_cast<int>(fa > fb ? 1 : (fa < fb ? -1 : 0));
        case Opcodes::FCMPG:
            if (std::isnan(fa) || std::isnan(fb)) return 1;
            return static_cast<int>(fa > fb ? 1 : (fa < fb ? -1 : 0));
        default: return ca::core::Any{};
        }
    }
    if (kindA == 3) {
        double da = valA.cast<double>();
        double db = valB.cast<double>();
        switch (opcode) {
        case Opcodes::DADD: return da + db;
        case Opcodes::DSUB: return da - db;
        case Opcodes::DMUL: return da * db;
        case Opcodes::DDIV: return da / db;
        case Opcodes::DREM: return std::fmod(da, db);
        case Opcodes::DCMPL:
            if (std::isnan(da) || std::isnan(db)) return -1;
            return static_cast<int>(da > db ? 1 : (da < db ? -1 : 0));
        case Opcodes::DCMPG:
            if (std::isnan(da) || std::isnan(db)) return 1;
            return static_cast<int>(da > db ? 1 : (da < db ? -1 : 0));
        default: return ca::core::Any{};
        }
    }
    return ca::core::Any{};
}

ca::core::Any MathHelper::unaryAny(int opcode, const ca::core::Any& value)
{
    if (!value.has_value()) return ca::core::Any{};

    auto [val, kind] = classifyValue(value);
    if (kind < 0) return ca::core::Any{};

    auto toDouble_ = [&](const ca::core::Any& v) -> double {
        if (kind == 0) return static_cast<double>(val.cast<int>());
        if (kind == 1) return static_cast<double>(val.cast<std::int64_t>());
        if (kind == 2) return static_cast<double>(val.cast<float>());
        return val.cast<double>();
    };

    switch (opcode) {
    case Opcodes::INEG:
        if (kind == 0) return javaIntSub(0, val.cast<int>());
        break;
    case Opcodes::FNEG:
        if (kind == 2) return -val.cast<float>();
        break;
    case Opcodes::LNEG:
        if (kind == 1) return javaLongSub(0, val.cast<std::int64_t>());
        break;
    case Opcodes::DNEG:
        if (kind == 3) return -val.cast<double>();
        break;
    case Opcodes::L2I:
        if (kind == 1) return static_cast<int>(val.cast<std::int64_t>());
        break;
    case Opcodes::F2I:
        if (kind == 2) return static_cast<int>(val.cast<float>());
        break;
    case Opcodes::D2I:
        if (kind == 3) return static_cast<int>(val.cast<double>());
        break;
    case Opcodes::I2B:
        if (kind == 0) return static_cast<int>(static_cast<std::int8_t>(val.cast<int>()));
        break;
    case Opcodes::I2C:
        if (kind == 0) return val.cast<int>() & 0x0000FFFF;
        break;
    case Opcodes::I2S:
        if (kind == 0) return static_cast<int>(static_cast<std::int16_t>(val.cast<int>()));
        break;
    case Opcodes::I2F:
    case Opcodes::L2F:
    case Opcodes::D2F: return static_cast<float>(toDouble_(value));
    case Opcodes::I2L:
    case Opcodes::F2L:
    case Opcodes::D2L: return static_cast<std::int64_t>(toDouble_(value));
    case Opcodes::I2D:
    case Opcodes::L2D:
    case Opcodes::F2D: return toDouble_(value);
    case Opcodes::CHECKCAST: return value;
    default: break;
    }
    return ca::core::Any{};
}

}   //namespace mjt::base
