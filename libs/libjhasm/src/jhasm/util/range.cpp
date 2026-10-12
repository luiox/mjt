#include "range.hpp"

#include <algorithm>

namespace jhasm::util {

bool Range::overlap(const Range& other) const noexcept
{
    return overlap(start_, end_, other.start_, other.end_);
}

bool Range::overlap(ca::i32 start, ca::i32 end, ca::i32 other_start, ca::i32 other_end) noexcept
{
    return std::max(start, other_start) <= std::max(end, other_end);
}

}   //namespace jhasm::util
