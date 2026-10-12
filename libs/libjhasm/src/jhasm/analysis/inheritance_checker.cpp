#include "inheritance_checker.hpp"

namespace jhasm::analysis {

const EmptyInheritanceChecker& EmptyInheritanceChecker::instance() noexcept
{
    static const EmptyInheritanceChecker checker;
    return checker;
}

bool EmptyInheritanceChecker::isSubclassOf(ca::str::Utf8StringRef child,
                                           ca::str::Utf8StringRef parent) const
{
    return child == parent;
}

ca::str::Utf8String EmptyInheritanceChecker::commonSuperclass(ca::str::Utf8StringRef first,
                                                              ca::str::Utf8StringRef second) const
{
    if (first == second) return first.substr(0, first.length());
    return ca::str::Utf8String("java/lang/Object");
}

}   //namespace jhasm::analysis
