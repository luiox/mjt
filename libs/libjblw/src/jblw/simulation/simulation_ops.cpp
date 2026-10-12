#include "simulation_ops.hpp"

#include "jblw/code/opcodes.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace jblw::simulation {
namespace {
template<typename To, typename From>
To copyBits(From value) noexcept
{
    static_assert(sizeof(To) == sizeof(From), "Bit copy requires equal-sized types");
    To result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

template<typename Signed, typename Unsigned>
Signed add(Signed a, Signed b) noexcept
{
    return copyBits<Signed>(static_cast<Unsigned>(a) + static_cast<Unsigned>(b));
}

template<typename Signed, typename Unsigned>
Signed subtract(Signed a, Signed b) noexcept
{
    return copyBits<Signed>(static_cast<Unsigned>(a) - static_cast<Unsigned>(b));
}

template<typename Signed, typename Unsigned>
Signed multiply(Signed a, Signed b) noexcept
{
    return copyBits<Signed>(static_cast<Unsigned>(a) * static_cast<Unsigned>(b));
}

template<typename Signed>
Signed divide(Signed a, Signed b)
{
    if (b == 0) throw std::domain_error("JVM integer division by zero");
    if (a == std::numeric_limits<Signed>::min() && b == static_cast<Signed>(-1)) return a;
    return static_cast<Signed>(a / b);
}

template<typename Signed>
Signed remainder(Signed a, Signed b)
{
    if (b == 0) throw std::domain_error("JVM integer remainder by zero");
    if (a == std::numeric_limits<Signed>::min() && b == static_cast<Signed>(-1)) return 0;
    return static_cast<Signed>(a % b);
}

template<typename Signed, typename Unsigned>
Signed arithmeticShiftRight(Signed value, ca::u32 shift) noexcept
{
    if (shift == 0) return value;
    Unsigned bits = static_cast<Unsigned>(value);
    bits >>= shift;
    if (value < 0) bits |= (~static_cast<Unsigned>(0)) << (sizeof(Signed) * 8U - shift);
    return copyBits<Signed>(bits);
}

[[noreturn]] void unexpectedOpcode()
{
    throw std::invalid_argument("Unexpected simulation opcode");
}
}   //namespace

ca::i32 SimulationOps::compare(ca::f64 b, ca::f64 a, ca::i32 ifNaN) noexcept
{
    if (std::isnan(a) || std::isnan(b)) return ifNaN;
    if (a < b) return -1;
    if (a > b) return 1;
    if (a == 0.0 && b == 0.0) {
        const ca::u64 aBits = copyBits<ca::u64>(a);
        const ca::u64 bBits = copyBits<ca::u64>(b);
        if (aBits != bBits) return std::signbit(a) ? -1 : 1;
    }
    return 0;
}

ca::i32 SimulationOps::compare(ca::f32 b, ca::f32 a, ca::i32 ifNaN) noexcept
{
    if (std::isnan(a) || std::isnan(b)) return ifNaN;
    if (a < b) return -1;
    if (a > b) return 1;
    if (a == 0.0F && b == 0.0F) {
        const ca::u32 aBits = copyBits<ca::u32>(a);
        const ca::u32 bBits = copyBits<ca::u32>(b);
        if (aBits != bBits) return std::signbit(a) ? -1 : 1;
    }
    return 0;
}

ca::i32 SimulationOps::compare(ca::i64 b, ca::i64 a) noexcept
{
    return a < b ? -1 : (a > b ? 1 : 0);
}

ca::i64 SimulationOps::execute(ca::i32 opcode, ca::i64 b, ca::i64 a)
{
    using O = code::JavaOpcodes;
    switch (opcode) {
    case O::LADD: return add<ca::i64, ca::u64>(a, b);
    case O::LSUB: return subtract<ca::i64, ca::u64>(a, b);
    case O::LMUL: return multiply<ca::i64, ca::u64>(a, b);
    case O::LDIV: return divide(a, b);
    case O::LREM: return remainder(a, b);
    case O::LAND: return a & b;
    case O::LOR: return a | b;
    case O::LXOR: return a ^ b;
    default: unexpectedOpcode();
    }
}

ca::i64 SimulationOps::executeShift(ca::i32 opcode, ca::i32 b, ca::i64 a)
{
    using O             = code::JavaOpcodes;
    const ca::u32 shift = static_cast<ca::u32>(b) & 0x3FU;
    switch (opcode) {
    case O::LSHL: return copyBits<ca::i64>(static_cast<ca::u64>(a) << shift);
    case O::LSHR: return arithmeticShiftRight<ca::i64, ca::u64>(a, shift);
    case O::LUSHR: return copyBits<ca::i64>(static_cast<ca::u64>(a) >> shift);
    default: unexpectedOpcode();
    }
}

ca::f64 SimulationOps::execute(ca::i32 opcode, ca::f64 b, ca::f64 a)
{
    using O = code::JavaOpcodes;
    switch (opcode) {
    case O::DADD: return a + b;
    case O::DSUB: return a - b;
    case O::DMUL: return a * b;
    case O::DDIV: return a / b;
    case O::DREM: return std::fmod(a, b);
    default: unexpectedOpcode();
    }
}

ca::i32 SimulationOps::execute(ca::i32 opcode, ca::i32 b, ca::i32 a)
{
    using O             = code::JavaOpcodes;
    const ca::u32 shift = static_cast<ca::u32>(b) & 0x1FU;
    switch (opcode) {
    case O::IADD: return add<ca::i32, ca::u32>(a, b);
    case O::ISUB: return subtract<ca::i32, ca::u32>(a, b);
    case O::IMUL: return multiply<ca::i32, ca::u32>(a, b);
    case O::IDIV: return divide(a, b);
    case O::IREM: return remainder(a, b);
    case O::ISHL: return copyBits<ca::i32>(static_cast<ca::u32>(a) << shift);
    case O::ISHR: return arithmeticShiftRight<ca::i32, ca::u32>(a, shift);
    case O::IUSHR: return copyBits<ca::i32>(static_cast<ca::u32>(a) >> shift);
    case O::IAND: return a & b;
    case O::IOR: return a | b;
    case O::IXOR: return a ^ b;
    default: unexpectedOpcode();
    }
}

ca::f32 SimulationOps::execute(ca::i32 opcode, ca::f32 b, ca::f32 a)
{
    using O = code::JavaOpcodes;
    switch (opcode) {
    case O::FADD: return a + b;
    case O::FSUB: return a - b;
    case O::FMUL: return a * b;
    case O::FDIV: return a / b;
    case O::FREM: return std::fmod(a, b);
    default: unexpectedOpcode();
    }
}

bool SimulationOps::test(ca::i32 opcode, ca::i32 value)
{
    using O = code::JavaOpcodes;
    switch (opcode) {
    case O::IFEQ: return value == 0;
    case O::IFNE: return value != 0;
    case O::IFLT: return value < 0;
    case O::IFGE: return value >= 0;
    case O::IFGT: return value > 0;
    case O::IFLE: return value <= 0;
    default: unexpectedOpcode();
    }
}

bool SimulationOps::test(ca::i32 opcode, ca::i32 b, ca::i32 a)
{
    using O = code::JavaOpcodes;
    switch (opcode) {
    case O::IF_ICMPEQ: return a == b;
    case O::IF_ICMPNE: return a != b;
    case O::IF_ICMPLT: return a < b;
    case O::IF_ICMPGE: return a >= b;
    case O::IF_ICMPGT: return a > b;
    case O::IF_ICMPLE: return a <= b;
    default: unexpectedOpcode();
    }
}

}   //namespace jblw::simulation
