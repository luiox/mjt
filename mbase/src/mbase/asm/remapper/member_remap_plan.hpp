#pragma once

///@defgroup member_remap_plan 成员重映射规则模块
///
///`MemberRemapPlan` 只保存 class、field、method 的精确旧值到新值规则，并提供名称与
///descriptor 查询。它不遍历 ClassNode、不识别反射调用，也不修改 JAR 资源。
///
///应用规则请使用 `member_remapper.hpp` 中的 remap 入口。

#include <string>
#include <unordered_map>
#include <vector>

#include "libca/str/utf8_string_arena.hpp"

namespace mjt::base {

///@brief Planned field declaration/reference rewrite.
///
///The old owner/name/descriptor identify the member before any class-name
///mapping is applied. An
///empty old descriptor is a descriptorless mapping file
///rule and matches fields by owner/name
///while preserving the current descriptor.
///New owner/name/descriptor describe the declaration or
///call-site target after
///the structural pass updates its model.
struct FieldRemapRule
{
    ca::str::Utf8StringRef oldOwner;
    ca::str::Utf8StringRef oldName;
    ca::str::Utf8StringRef oldDesc;
    ca::str::Utf8StringRef newOwner;
    ca::str::Utf8StringRef newName;
    ca::str::Utf8StringRef newDesc;
};

///@brief Planned method declaration/reference rewrite.
///
///Method rules are exact. They intentionally include descriptors so later
///signature-changing passes can update overloads without relying on names
///alone.
struct MethodRemapRule
{
    ca::str::Utf8StringRef oldOwner;
    ca::str::Utf8StringRef oldName;
    ca::str::Utf8StringRef oldDesc;
    ca::str::Utf8StringRef newOwner;
    ca::str::Utf8StringRef newName;
    ca::str::Utf8StringRef newDesc;
};

///@brief Accumulated class/member remap plan for structural optimizers.
///
///The plan is a lightweight, deterministic contract between mapping producers and remap
///executors. It owns internal class mappings plus exact field and method rules.
///
///All Utf8StringRef values returned by plan methods point to the plan's
///internal arena and are valid for the plan's lifetime.
class MemberRemapPlan
{
public:
    ///@brief Add or replace an internal class-name mapping.
    void mapClass(ca::str::Utf8StringRef oldName, ca::str::Utf8StringRef newName);

    ///@brief Add an exact field declaration/reference mapping.
    void mapField(const FieldRemapRule& rule);

    ///@brief Add an exact method declaration/reference mapping.
    void mapMethod(const MethodRemapRule& rule);

    ///@brief Return mapped internal class name, or the original name.
    ca::str::Utf8StringRef mapInternalName(ca::str::Utf8StringRef name) const;

    ///@brief Rewrite object types embedded in a field descriptor.
    ///
    ///恒等快路（perf/renamer-desc-remap-allocs）：classMappings_ 为空时返回输入
    ///视图本身（非 plan arena 所有，与 mapInternalName 未命中返回输入同语义）；
    ///非空时无论是否发生改写（含未命中的 is_empty 分支——返回 intern 键视图），
    ///返回值均为 plan arena 视图。重复输入经 descCache_ 记忆化（命中返回
    ///plan arena 视图，零分配）。调用方一律即时消费（比较/assignIfChanged），
    ///不得跨期持有（同类注记见类级 arena 死亡点声明）。
    ca::str::Utf8StringRef mapFieldDescriptor(ca::str::Utf8StringRef desc) const;

    ///@brief Rewrite object types embedded in a method descriptor.
    ///
    ///恒等快路与 descCache_ 记忆化语义同 mapFieldDescriptor。
    ca::str::Utf8StringRef mapMethodDescriptor(ca::str::Utf8StringRef desc) const;

    ///@brief Return an exact field rule, or a descriptorless owner/name field rule.
    const FieldRemapRule* findField(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                    ca::str::Utf8StringRef desc) const;

    ///@brief Return an exact method rule by old owner/name/descriptor.
    const MethodRemapRule* findMethod(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                      ca::str::Utf8StringRef desc) const;

    ///@brief Return a field name rule when all owner/name matches rename to one target name.
    const FieldRemapRule* findFieldByName(ca::str::Utf8StringRef owner,
                                          ca::str::Utf8StringRef name) const;

    ///@brief Return a method name rule when all owner/name matches rename to one target name.
    const MethodRemapRule* findMethodByName(ca::str::Utf8StringRef owner,
                                            ca::str::Utf8StringRef name) const;

    ///@brief True when no class, field, or method mappings are present.
    bool empty() const;

private:
    ///-- arena 死亡点声明（memory-governance-plan §5-P1③）--
    ///本成员持有 MemberRemapPlan 私有的 Utf8StringArena：mapClass/mapField/
    ///mapMethod intern 的映射键值与新名；mapInternalName 命中时返回的新名、
    ///mapFieldDescriptor/mapMethodDescriptor 返回的改写描述符（const 方法经
    ///mutable 惰性 intern，此即 mutable 存在的原因，member_remap_plan.cpp:61-79），
    ///以及 findField/findMethod/findFieldByName/findMethodByName 返回的规则字段
    ///均为指向本 arena 的视图。
    ///视图寿命 = 本 plan 寿命；死亡点：随本 plan 栈对象离开所在作用域析构——当前
    ///构造点均为局部对象：ClassRenamer::run、ToolClassCombiner::run、
    ///WorkspaceBase::analyzeRename/applyRename、mjt jar_remapper。重写产物写回用
    ///调用方注入的作业级 arena（applyMemberRemapPlan），不依赖本 arena 视图跨期存活。
    ///消费者不得把视图带过本对象的死亡点；需跨期持有则 intern 进自己的 arena 或深拷贝。
    mutable ca::str::Utf8StringArena arena_;

    ///-- 性能 scratch（perf/renamer-desc-remap-allocs，审计 H1 修法建议1）--
    ///mapFieldDescriptor/mapMethodDescriptor 的重映射工作池（PlanClassRemapper 的
    ///intern 落点：解析中间串、重拼产物）。此前每次调用栈上新建 arena，而
    ///Utf8StringArena 构造即分配 ~64KB 初始 chunk（libca utf8_string_arena.hpp:31-32），
    ///"每调用新建 arena"形态的瞬时字节流量被 chunk 粒度主导（str-alloc-baseline C1）。
    ///提升为成员后 chunk 只分配一次，随 plan 生灭——合法 per-data-structure arena
    ///（str-ownership-spec §2.1）。**返回视图不落此池**：两个 mapXxxDescriptor 在返回前
    ///把结果 intern 进 arena_，scratch 内只是中间产物，不对外暴露引用。
    ///线程纪律与 arena_ 相同（plan 非线程安全，单线程使用）。
    mutable ca::str::Utf8StringArena scratch_;

    ///-- desc→desc 结果缓存（perf/renamer-desc-remap-allocs，审计 H1 修法建议3）--
    ///mapFieldDescriptor/mapMethodDescriptor 慢路结果的记忆化：键=输入描述符、
    ///值=改写结果，**键值均 intern 进 arena_**（视图寿命=plan 寿命，与类级契约一致；
    ///内容哈希按字节比较，调用方临时视图可直接查命中）。命中路径零分配。
    ///失效点：仅 mapClass（类映射是描述符重映射的唯一输入面）；mapField/mapMethod
    ///只增精确成员规则、不经描述符映射路径，不触缓存。改名规则构建期（mutator
    ///阶段）与查询期分离，构建后再查询的用法下缓存始终热且一致。
    mutable std::unordered_map<ca::str::Utf8StringRef, ca::str::Utf8StringRef> descCache_;
    std::unordered_map<ca::str::Utf8StringRef, ca::str::Utf8StringRef> classMappings_;
    std::vector<FieldRemapRule>                                         fieldRules_;
    std::vector<MethodRemapRule>                                        methodRules_;

    ///debug 抽样断言：已存映射键值/规则字段确属 arena_（跨结构 ref 提升门禁，
    ///治理方案 §2.4-2，DEV-1 接线点二）。三个 mutator 尾部调用；release 空实现。
    void verifyStorageOwned() const;
};

}   //namespace mjt::base
