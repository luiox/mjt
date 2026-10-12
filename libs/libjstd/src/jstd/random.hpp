#pragma once

#include "libca/core/datatype.hpp"

namespace jstd {

///@brief `java.util.Random` 位级复刻（48-bit LCG）。
///
///对齐 JDK 语义（legacy LCG 分支）：multiplier 0x5DEECE66D、addend 0xB、
///mask (1<<48)-1；`new Random(seed)` 做 `seed ^ multiplier` scramble。
///`nextInt(bound)` 含拒绝采样（非 2 的幂 bound 的 JDK 有符号判定），
///`nextFloat`/`nextDouble` 与 JDK 除法逐位一致。C++ 侧唯一权威实现
///（原散落于各模块 intrinsic_folding / key_codec 等处的复刻已收敛于此）。
///
///非线程安全（对齐 JDK 核心 `next` 无内部同步）。
class Random
{
public:
    ///@brief `new Random(seed)`：JDK scramble 后进入 48-bit 状态。
    explicit Random(ca::i64 seed) noexcept
    {
        setSeed(seed);
    }

    ///@brief 重置状态（JDK `setSeed`）。
    void setSeed(ca::i64 seed) noexcept
    {
        m_seed = (static_cast<ca::u64>(seed) ^ kMultiplier) & kMask;
    }

    ///@brief `Random.next(bits)`：取当前状态高 bits 位（1..32），位模式返回。
    ca::i32 next(int bits) noexcept
    {
        m_seed = (m_seed * kMultiplier + kAddend) & kMask;
        return static_cast<ca::i32>(m_seed >> (48 - bits));
    }

    ///@brief `nextInt`：`next(32)`，无界。
    ca::i32 nextInt() noexcept
    {
        return next(32);
    }

    ///@brief `nextInt(bound)`：0..bound-1（JDK 拒绝采样；2 的幂走快速分支）。
    ///
    ///契约：bound 必须 > 0（Java 抛 IllegalArgumentException）。
    ca::i32 nextInt(ca::i32 bound) noexcept
    {
        if ((bound & -bound) == bound) {
            const ca::i32 bits = next(31);
            return static_cast<ca::i32>((static_cast<ca::i64>(bound) * bits) >> 31);
        }
        for (;;) {
            const ca::i32 bits = next(31);
            const ca::i32 val  = bits % bound;
            //JDK 有符号判定 `bits - val + (bound - 1) >= 0`（int 溢出为负则拒绝）。
            if (bits - val + (bound - 1) >= 0) return val;
        }
    }

    ///@brief `nextLong`：`((long)next(32) << 32) + next(32)` 位拼接。
    ca::i64 nextLong() noexcept
    {
        const ca::i32 high = next(32);
        const ca::i32 low  = next(32);
        return static_cast<ca::i64>(
            (static_cast<ca::u64>(static_cast<ca::u32>(high)) << 32) +
            static_cast<ca::u64>(static_cast<ca::i64>(low)));
    }

    ///@brief `nextFloat`：`next(24) / 2^24`（2 的幂除法精确，与 JDK 逐位一致）。
    ca::f32 nextFloat() noexcept
    {
        return static_cast<ca::f32>(next(24)) * (1.0f / 16777216.0f);
    }

    ///@brief `nextDouble`：`(((long)next(26) << 27) + next(27)) / 2^53`。
    ca::f64 nextDouble() noexcept
    {
        const ca::u64 high = static_cast<ca::u32>(next(26));
        const ca::u32 low  = static_cast<ca::u32>(next(27));
        return static_cast<ca::f64>((high << 27) + low) * (1.0 / 9007199254740992.0);
    }

    ///@brief `nextBoolean`：`next(1) != 0`。
    bool nextBoolean() noexcept
    {
        return next(1) != 0;
    }

private:
    static constexpr ca::u64 kMultiplier = 0x5DEECE66DULL;
    static constexpr ca::u64 kAddend     = 0xBULL;
    static constexpr ca::u64 kMask       = (1ULL << 48) - 1;
    ca::u64 m_seed = 0;
};

}   //namespace jstd
