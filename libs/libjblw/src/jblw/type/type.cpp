#include "type.hpp"

namespace jblw::type {

bool operator==(const Type& lhs, const Type& rhs) noexcept
{
    return lhs.equals(rhs);
}

bool operator!=(const Type& lhs, const Type& rhs) noexcept
{
    return !lhs.equals(rhs);
}

}   //namespace jblw::type
