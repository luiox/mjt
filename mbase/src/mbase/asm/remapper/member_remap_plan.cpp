#include "mbase/asm/remapper/member_remap_plan.hpp"

#include <cstdio>
#include <cstdlib>

#include "jbytecode/commons/remapper.hpp"
#include "libca/log/log_macros.hpp"

namespace mjt::base {
namespace {

class PlanClassRemapper : public jbytecode::commons::Remapper
{
public:
    ///staticArena 退役:基类 intern 落点必选注入。调用方给一个寿命覆盖本
    ///remapper 使用的 arena(见 mapFieldDescriptor/mapMethodDescriptor 的成员 scratch)。
    explicit PlanClassRemapper(const MemberRemapPlan& plan, ca::str::Utf8StringArena& arena)
        : Remapper(arena)
        , plan_(plan)
    {}

    ca::str::Utf8StringRef map(ca::str::Utf8StringRef internalName) override
    {
        return plan_.mapInternalName(internalName);
    }

private:
    const MemberRemapPlan& plan_;
};

}   //namespace

void MemberRemapPlan::mapClass(ca::str::Utf8StringRef oldName,
                               ca::str::Utf8StringRef newName)
{
    classMappings_[arena_.intern(oldName)] = arena_.intern(newName);
    //类映射是描述符重映射的唯一输入面（mapDesc/mapMethodDesc 经 mapInternalName 只查
    //classMappings_），映射面变化即既有 desc 缓存失效。mapField/mapMethod 只增精确
    //成员规则、不经描述符映射路径，不触缓存（hpp descCache_ 注记）。
    descCache_.clear();
    verifyStorageOwned();
}

void MemberRemapPlan::mapField(const FieldRemapRule& rule)
{
    FieldRemapRule interned;
    interned.oldOwner = arena_.intern(rule.oldOwner);
    interned.oldName  = arena_.intern(rule.oldName);
    interned.oldDesc  = arena_.intern(rule.oldDesc);
    interned.newOwner = arena_.intern(rule.newOwner);
    interned.newName  = arena_.intern(rule.newName);
    interned.newDesc  = arena_.intern(rule.newDesc);
    fieldRules_.push_back(interned);
    verifyStorageOwned();
}

void MemberRemapPlan::mapMethod(const MethodRemapRule& rule)
{
    MethodRemapRule interned;
    interned.oldOwner = arena_.intern(rule.oldOwner);
    interned.oldName  = arena_.intern(rule.oldName);
    interned.oldDesc  = arena_.intern(rule.oldDesc);
    interned.newOwner = arena_.intern(rule.newOwner);
    interned.newName  = arena_.intern(rule.newName);
    interned.newDesc  = arena_.intern(rule.newDesc);
    methodRules_.push_back(interned);
    verifyStorageOwned();
}

void MemberRemapPlan::verifyStorageOwned() const
{
#ifndef NDEBUG
    //跨结构 ref 提升门禁（治理方案 §2.4-2，DEV-1 接线点二）：守「存入 plan 的
    //视图必属 arena_」纪律的将来回归——当前构造即正确（全量 intern），本断言
    //拦截后续新增绕过 intern 的直存路径。空视图为合法缺省，跳过。
    auto fail = [](const char* what) {
        CA_LOG_ERROR("[mbase][member_remap_plan] 存入的 {} 不属于 plan 私有 arena（绕过 intern 的悬垂直存）",
                     what);
        std::abort();
    };
    auto check = [&](const char* what, ca::str::Utf8StringRef ref) {
        if (!ref.is_empty() && !arena_.owns(ref)) fail(what);
    };
    for (const auto& [oldName, newName] : classMappings_) {
        check("classMappings_ 键", oldName);
        check("classMappings_ 值", newName);
    }
    for (const auto& [oldDesc, newDesc] : descCache_) {
        check("descCache_ 键", oldDesc);
        check("descCache_ 值", newDesc);
    }
    for (const auto& rule : fieldRules_) {
        check("fieldRules_ 字段", rule.oldOwner);
        check("fieldRules_ 字段", rule.oldName);
        check("fieldRules_ 字段", rule.oldDesc);
        check("fieldRules_ 字段", rule.newOwner);
        check("fieldRules_ 字段", rule.newName);
        check("fieldRules_ 字段", rule.newDesc);
    }
    for (const auto& rule : methodRules_) {
        check("methodRules_ 字段", rule.oldOwner);
        check("methodRules_ 字段", rule.oldName);
        check("methodRules_ 字段", rule.oldDesc);
        check("methodRules_ 字段", rule.newOwner);
        check("methodRules_ 字段", rule.newName);
        check("methodRules_ 字段", rule.newDesc);
    }
#endif
}

ca::str::Utf8StringRef MemberRemapPlan::mapInternalName(
    ca::str::Utf8StringRef name) const
{
    const auto it = classMappings_.find(name);
    return it == classMappings_.end() ? name : it->second;
}

ca::str::Utf8StringRef MemberRemapPlan::mapFieldDescriptor(
    ca::str::Utf8StringRef desc) const
{
    if (desc.is_empty()) return desc;
    //恒等快路（审计 H1 修法建议2）：desc 重映射只改写内嵌类名，classMappings_ 为空
    //时内容必恒等，直接返回输入视图零分配（与 mapInternalName 未命中返回输入同语义）。
    if (classMappings_.empty()) return desc;
    //desc→desc 缓存（审计 H1 修法建议3）：命中即零分配，返回 plan arena 视图。
    const auto cached = descCache_.find(desc);
    if (cached != descCache_.end()) return cached->second;
    //staticArena 退役:基类合成 intern 落到成员 scratch(寿命=本 plan,chunk 只分
    //配一次;审计 H1 修法建议1)。mapped 的唯一消费点是下方 arena_.intern 深拷贝进本
    //plan 的私有 arena,scratch 引用不外泄;键值经 intern 后入缓存,缓存视图均 plan 所有。
    PlanClassRemapper remapper(*this, scratch_);
    const auto               mapped = remapper.mapDesc(desc);
    const auto               key    = arena_.intern(desc);
    const auto               result = mapped.is_empty() ? key : arena_.intern(mapped);
    descCache_.emplace(key, result);
#ifndef NDEBUG
    //R36 P2:查询路径填充的门禁缺口——插入即验证键值均归 plan arena 所有
    //（不做全量 verifyStorageOwned，避免 O(n²)）。
    if (!arena_.owns(key) || !arena_.owns(result)) std::abort();
#endif
    return result;
}

ca::str::Utf8StringRef MemberRemapPlan::mapMethodDescriptor(
    ca::str::Utf8StringRef desc) const
{
    if (desc.is_empty()) return desc;
    //恒等快路（审计 H1 修法建议2）：同 mapFieldDescriptor——classMappings_ 为空时
    //逐参重拼产物与输入逐字节相同，直接返回输入视图，跳过 Type 解析与重拼。
    if (classMappings_.empty()) return desc;
    //desc→desc 缓存：同 mapFieldDescriptor。
    const auto cached = descCache_.find(desc);
    if (cached != descCache_.end()) return cached->second;
    //:scratch 为成员池（审计 H1 修法建议1）；缓存接线同 mapFieldDescriptor。
    PlanClassRemapper remapper(*this, scratch_);
    const auto               mapped = remapper.mapMethodDesc(desc);
    const auto               key    = arena_.intern(desc);
    const auto               result = mapped.is_empty() ? key : arena_.intern(mapped);
    descCache_.emplace(key, result);
#ifndef NDEBUG
    //R36 P2:同 mapFieldDescriptor,插入即验证键值均归 plan arena 所有。
    if (!arena_.owns(key) || !arena_.owns(result)) std::abort();
#endif
    return result;
}

const FieldRemapRule* MemberRemapPlan::findField(
    ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
    ca::str::Utf8StringRef desc) const
{
    const FieldRemapRule* descriptorless = nullptr;
    for (const auto& rule : fieldRules_) {
        if (!rule.oldOwner.equals(owner) || !rule.oldName.equals(name)) continue;
        if (rule.oldDesc.equals(desc)) return &rule;
        if (rule.oldDesc.is_empty() && descriptorless == nullptr) descriptorless = &rule;
    }
    return descriptorless;
}

const MethodRemapRule* MemberRemapPlan::findMethod(
    ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
    ca::str::Utf8StringRef desc) const
{
    for (const auto& rule : methodRules_) {
        if (rule.oldOwner.equals(owner) && rule.oldName.equals(name) &&
            rule.oldDesc.equals(desc)) {
            return &rule;
        }
    }
    return nullptr;
}

const FieldRemapRule* MemberRemapPlan::findFieldByName(
    ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name) const
{
    const FieldRemapRule* match = nullptr;
    for (const auto& rule : fieldRules_) {
        if (!rule.oldOwner.equals(owner) || !rule.oldName.equals(name)) continue;
        if (match != nullptr && !match->newName.equals(rule.newName)) return nullptr;
        match = &rule;
    }
    return match;
}

const MethodRemapRule* MemberRemapPlan::findMethodByName(
    ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name) const
{
    const MethodRemapRule* match = nullptr;
    for (const auto& rule : methodRules_) {
        if (!rule.oldOwner.equals(owner) || !rule.oldName.equals(name)) continue;
        if (match != nullptr && !match->newName.equals(rule.newName)) return nullptr;
        match = &rule;
    }
    return match;
}

bool MemberRemapPlan::empty() const
{
    return classMappings_.empty() && fieldRules_.empty() && methodRules_.empty();
}

}   //namespace mjt::base
