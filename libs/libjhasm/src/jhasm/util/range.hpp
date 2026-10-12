#pragma once

#include <libca/core/datatype.hpp>

namespace jhasm::util {

///@brief 表示 Jasm 源文本中的 UTF-16 code unit 闭区间。
///
///下标与 Java `String` 一致，均以 UTF-16 code unit 计数。`empty` 返回的
///`[-1, -1]` 是未知范围哨兵，不表示零长度区间。
class Range
{
public:
    ///@brief 用 UTF-16 code unit 起止端点构造范围。
    constexpr Range(ca::i32 start, ca::i32 end) noexcept
        : start_(start)
        , end_(end)
    {}

    ///@brief 返回未知范围哨兵。
    static constexpr Range empty() noexcept { return Range(-1, -1); }

    ///@brief 返回闭区间起点。
    constexpr ca::i32 start() const noexcept { return start_; }

    ///@brief 返回闭区间终点。
    constexpr ca::i32 end() const noexcept { return end_; }

    ///@brief 判断位置是否位于闭区间内。
    constexpr bool within(ca::i32 position) const noexcept
    {
        return position >= start_ && position <= end_;
    }

    ///@brief 判断位置是否严格位于区间内部。
    constexpr bool within_exclusive(ca::i32 position) const noexcept
    {
        return position > start_ && position < end_;
    }

    ///@brief 按上游 Range 语义判断两个范围是否重叠。
    bool overlap(const Range& other) const noexcept;

    ///@brief 按上游 Range 语义判断两组范围端点是否重叠。
    static bool overlap(ca::i32 start, ca::i32 end, ca::i32 other_start,
                        ca::i32 other_end) noexcept;

    constexpr bool operator==(const Range& other) const noexcept
    {
        return start_ == other.start_ && end_ == other.end_;
    }

private:
    ca::i32 start_;
    ca::i32 end_;
};

}   //namespace jhasm::util
