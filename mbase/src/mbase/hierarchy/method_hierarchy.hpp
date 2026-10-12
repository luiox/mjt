#pragma once

#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/core/datatype.hpp"
#include "mbase/hierarchy/class_hierarchy.hpp"
#include "mbase/util/span.hpp"

namespace mjt::base {

using MethodCode = ca::u64;

struct MethodEntry
{
    ClassIndex owner;
    MethodCode code;
    ca::i32    methodIndex;
    bool       isPrivate;
    bool       isStatic;
    ca::i32    componentRoot;
};

class MethodHierarchy
{
public:
    void build(const ClassHierarchy& ch);

    ca::i32            methodCount() const { return static_cast<ca::i32>(methods_.size()); }
    const MethodEntry& entry(ca::i32 idx) const { return methods_[idx]; }

    ca::i32             findComponentRoot(ca::i32 methodIdx) const;
    ca::i32             sourceMethod(ca::i32 methodIdx) const;
    Span<const ca::i32> componentMethods(ca::i32 componentRoot) const;
    bool                isExternalOverride(ca::i32 methodIdx) const;
    bool                canRename(ca::i32 methodIdx) const;

private:
    struct CompSpan
    {
        ca::i32 start;
        ca::i32 end;
    };

    static ca::u32 hashFn(const ca::str::Utf8StringRef& s);

    ca::i32 ufFind(ca::i32 x);
    void    ufUnite(ca::i32 a, ca::i32 b);

    void buildComponentSpans();

    std::vector<MethodEntry> methods_;
    std::vector<ca::i32>     uf_;
    std::vector<ca::i32>     componentMembers_;
    std::vector<CompSpan>    componentSpans_;
    std::vector<bool>        isClassBroken_;
};

}   //namespace mjt::base
