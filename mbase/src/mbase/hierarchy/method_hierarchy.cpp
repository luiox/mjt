#include "mbase/hierarchy/method_hierarchy.hpp"

#include <algorithm>
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

MethodCode makeCode(const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc)
{
    return (static_cast<MethodCode>(fnv1a32(name)) << 32) | fnv1a32(desc);
}

bool isVirtual(const jbytecode::tree::MethodNode* mn)
{
    if (mn->access & jbytecode::core::Opcodes::ACC_PRIVATE) return false;
    if (mn->access & jbytecode::core::Opcodes::ACC_STATIC) return false;
    auto name = mn->name;
    if (name.byte_length() > 0 && name.data()[0] == '<') return false;
    return true;
}

}   //namespace

void MethodHierarchy::build(const ClassHierarchy& ch)
{
    //── Phase 1: Enumerate all methods, assign codes and indices ──
    std::unordered_map<ClassIndex, std::unordered_map<MethodCode, ca::i32>> virtMap;
    isClassBroken_.resize(ch.classCount());

    for (ClassIndex cls = 0; cls < static_cast<ClassIndex>(ch.classCount()); cls++) {
        const auto& e       = ch.entry(cls);
        auto*       cn      = e.classNode;
        isClassBroken_[cls] = ch.isBroken(cls);

        if (!cn) continue;

        for (ca::usize m = 0; m < cn->methods.size(); m++) {
            auto*   mn   = cn->methods[m];
            ca::i32 mIdx = static_cast<ca::i32>(methods_.size());

            MethodEntry me;
            me.owner         = cls;
            me.code          = makeCode(mn->name, mn->desc);
            me.methodIndex   = static_cast<ca::i32>(m);
            me.isPrivate     = (mn->access & jbytecode::core::Opcodes::ACC_PRIVATE) != 0;
            me.isStatic      = (mn->access & jbytecode::core::Opcodes::ACC_STATIC) != 0;
            me.componentRoot = mIdx;
            methods_.push_back(me);

            if (isVirtual(mn)) virtMap[cls][me.code] = mIdx;
        }
    }

    if (methods_.empty()) {
        buildComponentSpans();
        return;
    }

    //── Phase 2: Initialize Union-Find ──
    uf_.resize(methods_.size());
    for (ca::i32 i = 0; i < static_cast<ca::i32>(methods_.size()); i++) uf_[i] = i;

    //── Phase 3: Walk hierarchy and unite override chains ──
    for (ca::i32 mIdx = 0; mIdx < static_cast<ca::i32>(methods_.size()); mIdx++) {
        const auto& me = methods_[mIdx];
        if (me.isPrivate || me.isStatic) continue;

        //Walk super chain
        ClassIndex cur = ch.superClass(me.owner);
        std::unordered_set<ClassIndex> seenSuper;
        while (cur != INVALID_CLASS) {
            //环检测（同类根因）：畸形继承环 A↔B 会让 super 链上升死循环。
            if (!seenSuper.insert(cur).second) break;
            auto it = virtMap.find(cur);
            if (it != virtMap.end()) {
                auto jt = it->second.find(me.code);
                if (jt != it->second.end()) {
                    ufUnite(mIdx, jt->second);
                    break;
                }
            }
            cur = ch.superClass(cur);
        }

        //Walk ancestors for interface methods
        auto anc = ch.ancestors(me.owner);
        for (ca::usize k = 0; k < anc.size; k++) {
            ClassIndex aIdx = anc[k];
            if (aIdx == me.owner) continue;
            if (!ch.isInterface(aIdx)) continue;

            auto it = virtMap.find(aIdx);
            if (it != virtMap.end()) {
                auto jt = it->second.find(me.code);
                if (jt != it->second.end()) ufUnite(mIdx, jt->second);
            }
        }
    }

    //── Phase 4: Determine component roots (shallowest method per component) ──
    std::unordered_map<ClassIndex, int> depthCache;
    for (ClassIndex i = 0; i < static_cast<ClassIndex>(ch.classCount()); i++) {
        int            d   = 0;
        ClassIndex     cur = i;
        //环检测（同类根因）：畸形继承环的深度无法定义，取 0 截断。
        std::unordered_set<ClassIndex> seenDepth;
        while (cur != INVALID_CLASS && cur != ch.objectIndex() && seenDepth.insert(cur).second) {
            d++;
            cur = ch.superClass(cur);
        }
        depthCache[i] = d;
    }

    std::unordered_map<ca::i32, std::vector<ca::i32>> groups;
    for (ca::i32 i = 0; i < static_cast<ca::i32>(methods_.size()); i++) {
        ca::i32 root = ufFind(i);
        groups[root].push_back(i);
    }

    for (auto& [root, members] : groups) {
        //根选择（平台确定性）：最浅深度优先；平级时接口所有者优先，再按
        //所有者类名字典序、方法下标决胜。classes 的索引顺序来自
        //ResourceContainer 的 unordered_map 迭代序，跨平台/跨运行不稳定
        //（Linux CI method_hierarchy_test 实证），不得进入根选择语义。
        auto better = [&](ca::i32 a, ca::i32 b) {
            const auto& ea = methods_[a];
            const auto& eb = methods_[b];
            int da = depthCache[ea.owner];
            int db = depthCache[eb.owner];
            if (da != db) return da < db;
            bool ia = ch.isInterface(ea.owner);
            bool ib = ch.isInterface(eb.owner);
            if (ia != ib) return ia;   //接口所有者优先
            if (ea.owner != eb.owner) return ch.entry(ea.owner).name < ch.entry(eb.owner).name;
            return a < b;
        };
        ca::i32 shallowest = members[0];
        for (ca::i32 m : members)
            if (better(m, shallowest)) shallowest = m;
        for (ca::i32 m : members) methods_[m].componentRoot = shallowest;

        //Check external override: source method's owner is broken
        ClassIndex srcOwner = methods_[shallowest].owner;
        if (isClassBroken_[srcOwner]) {
            for (ca::i32 m : members) methods_[m].componentRoot = -1;   //mark as external
        }
    }

    buildComponentSpans();
}

void MethodHierarchy::buildComponentSpans()
{
    std::unordered_map<ca::i32, std::vector<ca::i32>> groups;
    for (ca::i32 i = 0; i < static_cast<ca::i32>(methods_.size()); i++)
        groups[methods_[i].componentRoot].push_back(i);

    componentMembers_.clear();
    componentSpans_ = std::vector<CompSpan>(methods_.size(), CompSpan{0, 0});

    for (auto& [root, members] : groups) {
        if (root < 0) continue;   //external override roots are -1
        componentSpans_[root].start = static_cast<ca::i32>(componentMembers_.size());
        for (ca::i32 m : members) componentMembers_.push_back(m);
        componentSpans_[root].end = static_cast<ca::i32>(componentMembers_.size());
    }
}

ca::i32 MethodHierarchy::ufFind(ca::i32 x)
{
    while (uf_[static_cast<ca::usize>(x)] != x) {
        uf_[static_cast<ca::usize>(x)] =
            uf_[static_cast<ca::usize>(uf_[static_cast<ca::usize>(x)])];
        x = uf_[static_cast<ca::usize>(x)];
    }
    return x;
}

void MethodHierarchy::ufUnite(ca::i32 a, ca::i32 b)
{
    ca::i32 ra = ufFind(a);
    ca::i32 rb = ufFind(b);
    if (ra != rb) uf_[static_cast<ca::usize>(ra)] = rb;
}

ca::i32 MethodHierarchy::findComponentRoot(ca::i32 methodIdx) const
{
    if (methodIdx < 0 || methodIdx >= static_cast<ca::i32>(methods_.size())) return -1;
    return methods_[methodIdx].componentRoot;
}

ca::i32 MethodHierarchy::sourceMethod(ca::i32 methodIdx) const
{
    return findComponentRoot(methodIdx);
}

Span<const ca::i32> MethodHierarchy::componentMethods(ca::i32 componentRoot) const
{
    if (componentRoot < 0 || componentRoot >= static_cast<ca::i32>(componentSpans_.size()))
        return {nullptr, 0};
    const auto& span = componentSpans_[componentRoot];
    if (span.start >= span.end) return {nullptr, 0};
    return {componentMembers_.data() + span.start, static_cast<ca::usize>(span.end - span.start)};
}

bool MethodHierarchy::isExternalOverride(ca::i32 methodIdx) const
{
    if (methodIdx < 0 || methodIdx >= static_cast<ca::i32>(methods_.size())) return true;
    return methods_[methodIdx].componentRoot == -1;
}

bool MethodHierarchy::canRename(ca::i32 methodIdx) const
{
    if (methodIdx < 0 || methodIdx >= static_cast<ca::i32>(methods_.size())) return false;
    const auto& me = methods_[methodIdx];
    //Private and static methods don't participate in polymorphism → safe to rename
    if (me.isPrivate || me.isStatic) return true;
    //Virtual methods: only if not overriding an external (library) method
    return !isExternalOverride(methodIdx);
}

}   //namespace mjt::base
