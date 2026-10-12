#include "mbase/hierarchy/field_hierarchy.hpp"

#include <unordered_map>
#include <unordered_set>

#include "jbytecode/core/opcodes.hpp"

namespace mjt::base {

namespace {

ca::u32 fnv1a32(const ca::str::Utf8StringRef& s)
{
    auto    data = s.data();
    auto    len  = s.byte_length();
    ca::u32 h    = 2166136261u;
    for (ca::usize i = 0; i < len; i++) {
        h ^= static_cast<ca::u32>(data[i]);
        h *= 16777619u;
    }
    return h;
}

FieldCode makeCode(const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc)
{
    return (static_cast<FieldCode>(fnv1a32(name)) << 32) | fnv1a32(desc);
}

bool isInstanceField(const jbytecode::tree::FieldNode* fn)
{
    return (fn->access & jbytecode::core::Opcodes::ACC_STATIC) == 0;
}

}   //namespace

void FieldHierarchy::build(const ClassHierarchy& ch)
{
    //── Phase 1: Collect all instance fields with codes ──
    struct RawField
    {
        ClassIndex owner;
        FieldCode  code;
        ca::i32    fieldIndex;
        bool       isStatic;
    };
    std::vector<RawField> raw;

    for (ClassIndex cls = 0; cls < static_cast<ClassIndex>(ch.classCount()); cls++) {
        auto* cn = ch.entry(cls).classNode;
        if (!cn) continue;

        for (ca::usize f = 0; f < cn->fields.size(); f++) {
            auto* fn = cn->fields[f];
            raw.push_back({cls,
                           makeCode(fn->name, fn->desc),
                           static_cast<ca::i32>(f),
                           bool(fn->access & jbytecode::core::Opcodes::ACC_STATIC)});
        }
    }

    fields_.reserve(raw.size());
    for (auto& rf : raw) {
        //Determine if this field is a source: check if any ancestor has same code
        bool isSource = true;
        if (isInstanceField(ch.entry(rf.owner).classNode->fields[rf.fieldIndex])) {
            ClassIndex cur = ch.superClass(rf.owner);
            //环检测（同类根因）：畸形继承环 A↔B 会让 super 链上升死循环。
            std::unordered_set<ClassIndex> seenSuper;
            while (cur != INVALID_CLASS && seenSuper.insert(cur).second) {
                auto* pcn = ch.entry(cur).classNode;
                if (pcn) {
                    for (ca::usize pf = 0; pf < pcn->fields.size(); pf++) {
                        auto* pfn = pcn->fields[pf];
                        if (!isInstanceField(pfn)) continue;
                        if (makeCode(pfn->name, pfn->desc) == rf.code) {
                            isSource = false;
                            break;
                        }
                    }
                }
                if (!isSource) break;
                cur = ch.superClass(cur);
            }
        }

        FieldEntry fe;
        fe.owner         = rf.owner;
        fe.code          = rf.code;
        fe.fieldIndex    = rf.fieldIndex;
        fe.isStatic      = rf.isStatic;
        fe.isSourceField = isSource;
        fields_.push_back(fe);
    }
}

bool FieldHierarchy::isSourceField(ca::i32 fieldIdx) const
{
    if (fieldIdx < 0 || fieldIdx >= static_cast<ca::i32>(fields_.size())) return false;
    return fields_[fieldIdx].isSourceField;
}

bool FieldHierarchy::canRename(ca::i32 fieldIdx) const
{
    if (fieldIdx < 0 || fieldIdx >= static_cast<ca::i32>(fields_.size())) return false;
    const auto& f = fields_[fieldIdx];
    return f.isSourceField && !f.isStatic;
}

}   //namespace mjt::base
