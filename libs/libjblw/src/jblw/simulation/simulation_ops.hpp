#pragma once

#include <libca/core/datatype.hpp>

namespace jblw::simulation {

class SimulationOps final
{
public:
    static ca::i32 compare(ca::f64 b, ca::f64 a, ca::i32 ifNaN) noexcept;
    static ca::i32 compare(ca::f32 b, ca::f32 a, ca::i32 ifNaN) noexcept;
    static ca::i32 compare(ca::i64 b, ca::i64 a) noexcept;

    static ca::i64 execute(ca::i32 opcode, ca::i64 b, ca::i64 a);
    static ca::i64 executeShift(ca::i32 opcode, ca::i32 b, ca::i64 a);
    static ca::f64 execute(ca::i32 opcode, ca::f64 b, ca::f64 a);
    static ca::i32 execute(ca::i32 opcode, ca::i32 b, ca::i32 a);
    static ca::f32 execute(ca::i32 opcode, ca::f32 b, ca::f32 a);

    static bool test(ca::i32 opcode, ca::i32 value);
    static bool test(ca::i32 opcode, ca::i32 b, ca::i32 a);

    SimulationOps() = delete;
};

}   //namespace jblw::simulation
