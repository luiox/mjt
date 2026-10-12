#include "mbase/hierarchy/class_hierarchy.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "jbytecode/core/opcodes.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

namespace {

constexpr const char* kObjectName = "java/lang/Object";

bool isObject(ca::str::Utf8StringRef name)
{
    return name.equals(kObjectName);
}

}   //namespace

ClassHierarchy::ClassHierarchy()
{
    objectIndex_ = INVALID_CLASS;
}

void ClassHierarchy::build(ResourceContainer& container, ca::str::Utf8StringArena& arena,
                           ClassLookup lookup)
{
    arena_  = &arena;
    lookup_ = std::move(lookup);

    //── Phase 1: Scan ──
    for (auto& [uri, resource] : container.getResources()) {
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto*       cls   = ca::core::tag_cast::cast<ClassResource>(resource.get());
        const auto& bytes = cls->get();
        if (bytes.empty()) continue;

        auto                         classNode = std::make_unique<jbytecode::tree::ClassNode>();
        jbytecode::core::ClassReader reader(
            *arena_, bytes.data(), static_cast<ca::i32>(bytes.size()));
        reader.accept(
            classNode.get(),
            jbytecode::core::ClassReader::SKIP_DEBUG | jbytecode::core::ClassReader::SKIP_FRAMES);

        ca::str::Utf8StringRef name = arena_->intern(classNode->name);
        ClassIndex             idx  = static_cast<ClassIndex>(entries_.size());

        ca::u8 flags = 0;
        if (classNode->access & jbytecode::core::Opcodes::ACC_INTERFACE) flags |= CF_Interface;
        if (classNode->access & jbytecode::core::Opcodes::ACC_ENUM) flags |= CF_Enum;

        entries_.push_back({name, idx, INVALID_CLASS, 0, 0, flags, classNode.get()});
        nameToIndex_[name] = idx;

        if (isObject(name)) objectIndex_ = idx;

        ownedClassNodes_.push_back(std::move(classNode));
    }

    //── Phase 2: Edge resolution ──
    std::vector<std::vector<ClassIndex>> tempInterfaces(entries_.size());

    for (ca::usize i = 0; i < entries_.size(); i++) {
        if (entries_[i].classNode == nullptr) continue;

        //java/lang/Object has no super
        if (entries_[i].classNode->superName.is_empty()) {
            entries_[i].superIndex = INVALID_CLASS;
            continue;
        }

        //Resolve super class
        ca::str::Utf8StringRef superName = arena_->intern(entries_[i].classNode->superName);
        entries_[i].superIndex           = resolveSuper(superName);

        //Ensure tempInterfaces covers any new phantom entries
        while (tempInterfaces.size() < entries_.size()) tempInterfaces.emplace_back();

        //Resolve interfaces
        for (const auto& ifName : entries_[i].classNode->interfaces) {
            ca::str::Utf8StringRef ifInterned = arena_->intern(ifName);
            auto                   it         = nameToIndex_.find(ifInterned);
            if (it != nameToIndex_.end()) {
                if (i < tempInterfaces.size()) tempInterfaces[i].push_back(it->second);
            }
            else if (tombstones_.count(ifInterned) == 0) {
                //Try lookup
                ClassIndex ifIdx = INVALID_CLASS;
                if (lookup_) {
                    auto* resolved = lookup_(ifInterned);
                    if (resolved) {
                        ifIdx = addLookedUpClass(resolved);
                    }
                }
                if (ifIdx != INVALID_CLASS) {
                    //Resize tempInterfaces if new entries were added
                    while (tempInterfaces.size() < entries_.size()) tempInterfaces.emplace_back();
                    tempInterfaces[i].push_back(ifIdx);
                }
                else {
                    tombstones_.insert(ifInterned);
                }
            }
        }
    }

    //Ensure java/lang/Object exists
    if (objectIndex_ == INVALID_CLASS) {
        ca::str::Utf8StringRef objName = arena_->intern(kObjectName);
        //May have been created as phantom during edge resolution
        auto objIt = nameToIndex_.find(objName);
        if (objIt != nameToIndex_.end()) {
            objectIndex_ = objIt->second;
        }
        else {
            objectIndex_ = static_cast<ClassIndex>(entries_.size());
            entries_.push_back({objName, objectIndex_, INVALID_CLASS, 0, 0, CF_Broken, nullptr});
            nameToIndex_[objName] = objectIndex_;
            tempInterfaces.emplace_back();
        }
    }

    //Flatten interfaces
    for (ca::usize i = 0; i < entries_.size(); i++) {
        entries_[i].ifStart = static_cast<ca::i32>(interfaces_.size());
        for (ClassIndex ifIdx : tempInterfaces[i]) interfaces_.push_back(ifIdx);
        entries_[i].ifEnd = static_cast<ca::i32>(interfaces_.size());
    }

    //── Phase 3: Compute ancestors ──
    computeAncestors();
    computeSubtypeIntervals();

    //── Phase 3.5: Propagate broken ──
    propagateBroken();
}

ClassIndex ClassHierarchy::findOrCreateEntry(ca::str::Utf8StringRef name)
{
    auto it = nameToIndex_.find(name);
    if (it != nameToIndex_.end()) return it->second;

    if (tombstones_.count(name)) return INVALID_CLASS;

    tombstones_.insert(name);
    ClassIndex idx = static_cast<ClassIndex>(entries_.size());
    entries_.push_back({name, idx, INVALID_CLASS, 0, 0, CF_Broken, nullptr});
    nameToIndex_[name] = idx;

    if (isObject(name)) objectIndex_ = idx;

    return idx;
}

ClassIndex ClassHierarchy::resolveSuper(ca::str::Utf8StringRef superName)
{
    //Already known
    auto it = nameToIndex_.find(superName);
    if (it != nameToIndex_.end()) return it->second;

    //Known missing
    if (tombstones_.count(superName)) return INVALID_CLASS;

    //Try callback
    if (lookup_) {
        auto* resolved = lookup_(superName);
        if (resolved) {
            ClassIndex idx = addLookedUpClass(resolved);

            //Recursively resolve this new class's own super
            if (!resolved->superName.is_empty()) {
                ca::str::Utf8StringRef parentName = arena_->intern(resolved->superName);
                if (!parentName.equals(arena_->intern(kObjectName))) {
                    entries_[idx].superIndex = resolveSuper(parentName);
                }
                else if (objectIndex_ != INVALID_CLASS) {
                    entries_[idx].superIndex = objectIndex_;
                }
            }

            return idx;
        }
    }

    //Create phantom
    return findOrCreateEntry(superName);
}

ClassIndex ClassHierarchy::addLookedUpClass(jbytecode::tree::ClassNode* classNode)
{
    ca::str::Utf8StringRef name = arena_->intern(classNode->name);

    //Check again (could have been added by recursive resolveSuper)
    auto it = nameToIndex_.find(name);
    if (it != nameToIndex_.end()) return it->second;

    ClassIndex idx = static_cast<ClassIndex>(entries_.size());

    ca::u8 flags = 0;
    if (classNode->access & jbytecode::core::Opcodes::ACC_INTERFACE) flags |= CF_Interface;
    if (classNode->access & jbytecode::core::Opcodes::ACC_ENUM) flags |= CF_Enum;

    entries_.push_back({name, idx, INVALID_CLASS, 0, 0, flags, classNode});
    nameToIndex_[name] = idx;

    if (isObject(name)) objectIndex_ = idx;

    return idx;
}

void ClassHierarchy::computeAncestors()
{
    ca::usize n = entries_.size();
    ancestorSpans_.resize(n);
    std::vector<ca::u8> visitState(n, 0);

    //DFS with memoization: compute ancestors for each class
    std::function<void(ClassIndex)> dfs = [&](ClassIndex cur) {
        if (visitState[cur] == 2) return;   //already computed
        if (visitState[cur] == 1) return;   //cycle detected

        visitState[cur] = 1;

        std::vector<ClassIndex> result;
        result.push_back(cur);

        //Collect from interfaces
        const auto& entry = entries_[cur];
        for (ca::i32 j = entry.ifStart; j < entry.ifEnd; j++) {
            ClassIndex ifIdx = interfaces_[j];
            if (visitState[ifIdx] != 2 && visitState[ifIdx] != 1) {
                //Need to compute interface's ancestors first
                dfs(ifIdx);
            }
            if (visitState[ifIdx] == 2) {
                auto span = ancestorSpans_[ifIdx];
                for (ca::i32 k = span.start; k < span.end; k++) result.push_back(ancestors_[k]);
            }
        }

        //Collect from super class
        ClassIndex superIdx = entry.superIndex;
        if (superIdx != INVALID_CLASS && superIdx != cur && !isObject(nameOf(superIdx))) {
            if (visitState[superIdx] == 0) dfs(superIdx);
            if (visitState[superIdx] == 2) {
                auto span = ancestorSpans_[superIdx];
                for (ca::i32 k = span.start; k < span.end; k++) result.push_back(ancestors_[k]);
            }
        }
        else if (superIdx != INVALID_CLASS && isObject(nameOf(superIdx))) {
            //Object is an ancestor of everything
            result.push_back(superIdx);
        }

        //Deduplicate preserving depth order (first occurrence kept). The
        //previous nested scan made a long inheritance chain quadratic for
        //every class; a membership set keeps this pass linear in the number
        //of collected ancestors while retaining the public ordering.
        std::unordered_set<ClassIndex> seen;
        seen.reserve(result.size());
        std::vector<ClassIndex> deduped;
        deduped.reserve(result.size());
        for (ClassIndex a : result) {
            if (seen.insert(a).second) deduped.push_back(a);
        }
        result.swap(deduped);

        //Store to flat array
        ancestorSpans_[cur].start = static_cast<ca::i32>(ancestors_.size());
        for (ClassIndex a : result) ancestors_.push_back(a);
        ancestorSpans_[cur].end = static_cast<ca::i32>(ancestors_.size());

        visitState[cur] = 2;
    };

    for (ClassIndex i = 0; i < static_cast<ClassIndex>(n); i++) {
        if (visitState[i] == 0) dfs(i);
    }
}

void ClassHierarchy::computeSubtypeIntervals()
{
    const auto count = entries_.size();
    subtypeIntervalStart_.assign(count, 0);
    subtypeIntervalEnd_.assign(count, 0);

    std::vector<ClassIndex> firstChild(count, INVALID_CLASS);
    std::vector<ClassIndex> nextSibling(count, INVALID_CLASS);
    std::vector<ClassIndex> roots;
    roots.reserve(count);
    for (ClassIndex idx = 0; idx < static_cast<ClassIndex>(count); ++idx) {
        const auto parent = entries_[idx].superIndex;
        if (parent >= 0 && parent < static_cast<ClassIndex>(count) && parent != idx) {
            nextSibling[idx]   = firstChild[parent];
            firstChild[parent] = idx;
        }
        else if (parent == INVALID_CLASS || idx == objectIndex_) {
            roots.push_back(idx);
        }
    }

    std::vector<ca::u8> state(count, 0);
    ca::u32             clock = 1;
    for (const auto root : roots) {
        if (state[root] != 0) continue;
        std::vector<std::pair<ClassIndex, bool>> stack;
        stack.emplace_back(root, false);
        while (!stack.empty()) {
            const auto [idx, exit] = stack.back();
            stack.pop_back();
            if (exit) {
                state[idx]               = 2;
                subtypeIntervalEnd_[idx] = clock++;
                continue;
            }
            if (state[idx] != 0) continue;
            state[idx]                 = 1;
            subtypeIntervalStart_[idx] = clock++;
            stack.emplace_back(idx, true);
            for (auto child = firstChild[idx]; child != INVALID_CLASS; child = nextSibling[child]) {
                if (state[child] == 0) stack.emplace_back(child, false);
            }
        }
    }

    //Malformed superclass cycles have no root. Isolate their members so a
    //corrupt hierarchy cannot manufacture subtype relationships.
    for (ClassIndex idx = 0; idx < static_cast<ClassIndex>(count); ++idx) {
        if (state[idx] != 0) continue;
        subtypeIntervalStart_[idx] = clock++;
        subtypeIntervalEnd_[idx]   = clock++;
        state[idx]                 = 2;
    }
}

void ClassHierarchy::propagateBroken()
{
    for (ca::usize i = 0; i < entries_.size(); i++) {
        if (isBroken(static_cast<ClassIndex>(i))) {
            entries_[i].flags |= CF_HasMissingDep;
            continue;
        }
        //Check if any ancestor is broken
        auto span = ancestorSpans_[i];
        for (ca::i32 j = span.start; j < span.end; j++) {
            ClassIndex anc = ancestors_[j];
            if (anc != static_cast<ClassIndex>(i) && isBroken(anc)) {
                entries_[i].flags |= CF_HasMissingDep;
                break;
            }
        }
    }
}

//── Queries ──

ClassIndex ClassHierarchy::indexOf(ca::str::Utf8StringRef name) const
{
    auto it = nameToIndex_.find(arena_->intern(name));
    return it != nameToIndex_.end() ? it->second : INVALID_CLASS;
}

ca::str::Utf8StringRef ClassHierarchy::nameOf(ClassIndex idx) const
{
    return entries_[idx].name;
}

const ClassEntry& ClassHierarchy::entry(ClassIndex idx) const
{
    return entries_[idx];
}

ClassIndex ClassHierarchy::superClass(ClassIndex idx) const
{
    return entries_[idx].superIndex;
}

Span<const ClassIndex> ClassHierarchy::interfaces(ClassIndex idx) const
{
    const auto& e = entries_[idx];
    return {interfaces_.data() + e.ifStart, static_cast<ca::usize>(e.ifEnd - e.ifStart)};
}

bool ClassHierarchy::isSubType(ClassIndex sub, ClassIndex super) const
{
    if (sub < 0 || super < 0 || sub >= static_cast<ClassIndex>(entries_.size()) ||
        super >= static_cast<ClassIndex>(entries_.size()))
        return false;
    if (sub == super) return true;
    if (super == objectIndex_) return true;
    if (sub == objectIndex_) return false;

    //JVM class inheritance has one superclass, so Euler containment answers
    //ordinary class queries in O(1). Interface inheritance is a DAG and
    //retains the exact ancestor-table lookup below.
    if (!isInterface(super)) {
        return subtypeIntervalStart_[super] <= subtypeIntervalStart_[sub] &&
               subtypeIntervalEnd_[sub] <= subtypeIntervalEnd_[super];
    }

    const auto& span = ancestorSpans_[sub];
    for (ca::i32 i = span.start; i < span.end; i++) {
        if (ancestors_[i] == super) return true;
    }
    return false;
}

ClassIndex ClassHierarchy::commonSuper(ClassIndex a, ClassIndex b) const
{
    if (a == b) return a;
    if (a == objectIndex_ || b == objectIndex_) return objectIndex_;
    if (a < 0 || b < 0 || a >= static_cast<ClassIndex>(entries_.size()) ||
        b >= static_cast<ClassIndex>(entries_.size()))
        return INVALID_CLASS;

    const auto& spanA = ancestorSpans_[a];
    const auto& spanB = ancestorSpans_[b];

    for (ca::i32 i = spanA.start; i < spanA.end; i++) {
        ClassIndex candidate = ancestors_[i];
        for (ca::i32 j = spanB.start; j < spanB.end; j++) {
            if (ancestors_[j] == candidate) return candidate;
        }
    }

    return objectIndex_;
}

Span<const ClassIndex> ClassHierarchy::ancestors(ClassIndex idx) const
{
    const auto& span = ancestorSpans_[idx];
    return {ancestors_.data() + span.start, static_cast<ca::usize>(span.end - span.start)};
}

bool ClassHierarchy::isInterface(ClassIndex idx) const
{
    return (entries_[idx].flags & CF_Interface) != 0;
}

bool ClassHierarchy::isEnum(ClassIndex idx) const
{
    return (entries_[idx].flags & CF_Enum) != 0;
}

bool ClassHierarchy::isBroken(ClassIndex idx) const
{
    return (entries_[idx].flags & CF_Broken) != 0;
}

bool ClassHierarchy::hasMissingDep(ClassIndex idx) const
{
    return (entries_[idx].flags & CF_HasMissingDep) != 0;
}

void ClassHierarchy::remap(Span<std::pair<ca::str::Utf8StringRef, ca::str::Utf8StringRef>> mapping)
{
    for (ca::usize i = 0; i < mapping.size; i++) {
        auto [oldName, newName] = mapping[i];
        auto it                 = nameToIndex_.find(oldName);
        if (it == nameToIndex_.end()) continue;
        ClassIndex idx        = it->second;
        nameToIndex_[newName] = idx;
        nameToIndex_.erase(it);
        entries_[idx].name = newName;
    }
    for (ca::usize i = 0; i < mapping.size; i++) {
        auto [oldName, newName] = mapping[i];
        auto idx                = nameToIndex_[newName];
        ancestorSpans_[idx]     = {0, 0};
        (void)idx;
    }
    for (ca::usize i = 0; i < mapping.size; i++) {
        tombstones_.erase(mapping[i].first);
    }
}

}   //namespace mjt::base
