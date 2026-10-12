#pragma once

#include <unordered_map>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/core/datatype.hpp"
#include "mbase/hierarchy/class_hierarchy.hpp"
#include "mbase/util/span.hpp"

namespace mjt::base {

using FieldCode = ca::u64;

struct FieldEntry
{
    ClassIndex owner;
    FieldCode  code;
    ca::i32    fieldIndex;
    bool       isStatic;
    bool       isSourceField;
};

class FieldHierarchy
{
public:
    void build(const ClassHierarchy& ch);

    ca::i32           fieldCount() const { return static_cast<ca::i32>(fields_.size()); }
    const FieldEntry& entry(ca::i32 idx) const { return fields_[idx]; }

    bool isSourceField(ca::i32 fieldIdx) const;
    bool canRename(ca::i32 fieldIdx) const;

private:
    static ca::u32   hashFn(const ca::str::Utf8StringRef& s);
    static FieldCode makeFieldCode(const ca::str::Utf8StringRef& name,
                                   const ca::str::Utf8StringRef& desc);

    std::vector<FieldEntry> fields_;
};

}   //namespace mjt::base
