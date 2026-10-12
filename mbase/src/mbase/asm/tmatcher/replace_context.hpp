#pragma once

#include <cassert>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "garena/arena.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/tree/insn_type_traits.hpp"
#include "jbytecode/tree/insn_list.hpp"

#include "mbase/asm/insn/insn_builder.hpp"
#include "mbase/asm/tmatcher/match_state.hpp"

namespace mjt::tmatcher {

///替换执行上下文：承载一次命中的匹配视图 + 扁平替换收集器。
///
///与只读 MatchContext 分离：本类型面向 ReplaceRuleHandler，提供向 builder
///追加替换指令、克隆命中区间内原始指令等能力。
///
///B8 clone 协议：origin/originRange 的克隆节点与区间内 Label 副本一律落
///`original` 绑定的目标方法 arena（ownerArena）。旧「unique_ptr 裸 Label
///所有权 + releaseOutputLabels 手工释放」机制随 arena 接管整体删除。
struct ReplaceContext
{
    jbytecode::tree::InsnList* original = nullptr;   //被匹配的原始指令表
    int                        start    = -1;        //命中区间起点（含）
    int                        end      = -1;        //命中区间终点（不含）
    const CaptureStore*        captures = nullptr;   //本次命中的捕获表
    mjt::base::InsnBuilder builder;               //替换指令的扁平收集器

    ///命中区间 Label → 目标 arena 副本映射（cloneOriginInsn 惰性登记，跨克隆共享）。
    std::unordered_map<jbytecode::core::Label*, jbytecode::core::Label*> clonedLabels;
    ///命中区间内出现的 Label（含前向跳转目标，见 prepareOriginClones）。
    std::unordered_set<jbytecode::core::Label*> matchedLabels;

    bool debug    = false;
    int  logDepth = 0;

    void log(const std::string& msg)
    {
        if (!debug) return;
        printf("%*s%s\n", logDepth * 2, "", msg.c_str());
    }

    void push(const std::string& label)
    {
        log("+ " + label);
        logDepth++;
    }

    void pop()
    {
        logDepth--;
        log("- ");
    }

    ///不追加替换指令，调用方删除命中区间。
    void replaceWithPop()
    {
        //空 builder 表示删除命中指令。
    }

    ///向 builder 追加一条替换指令。
    void replace(jbytecode::tree::AbstractInsnNode* insn)
    {
        if (insn) builder.add(insn);
    }

    ///扫描命中区间内的 Label，使 origin 能在前向跳转场景中正确重映射。
    void prepareOriginClones()
    {
        if (!original) return;
        for (int i = start; i >= 0 && i < end && i < original->size(); ++i) {
            auto* insn = original->get(i);
            if (insn && insn->getType() == jbytecode::tree::AbstractInsnNode::LABEL) {
                auto* label =
                    ca::core::tag_cast::cast<jbytecode::tree::LabelNode*>(insn)->label;
                if (label) matchedLabels.insert(label);
            }
        }
    }

    ///克隆匹配范围内的第 i 条指令（0-based），追加到 builder。
    ///使用预建 Label 集合确保前向跳转也会指向替换片段内的 clone。
    void origin(int i)
    {
        if (!original || start < 0 || end < start || i < 0) return;
        const int index = start + i;
        if (index < start || index >= end || index >= original->size()) return;
        auto* insn = original->get(index);
        if (insn) builder.add(cloneOriginInsn(insn));
    }

    ///克隆匹配范围内 [start, end) 区间的指令，追加到 builder。
    ///所有克隆共用同一个 clonedLabels 映射表。
    void originRange(int startIdx, int endIdx)
    {
        for (int i = startIdx; i < endIdx; i++) origin(i);
    }

    ///将收集的替换节点一次性 materialize 为 InsnList，所有权转移给返回值。
    ///返回列表绑定目标 arena：其中 arena 产物不归列表容器析构（随 arena 统一回收），
    ///builder 工厂产物（堆节点）仍由容器 delete（InsnList 混合清除语义）。
    jbytecode::tree::InsnList getInsnList()
    {
        jbytecode::tree::InsnList replacement(targetArena());
        jbytecode::tree::InsnList collected = builder.getInsnList();
        replacement.add(collected);
        return replacement;
    }

private:
    ///克隆产物的分配归宿：original 绑定的方法内容 arena。
    ///scratch 未绑定列表为 null——此时不得调用 origin 克隆（debug 断言）。
    garena::Arena* targetArena() const { return original ? original->ownerArena() : nullptr; }

    ///区间内 Label → 目标 arena 副本（强制换新，保证片段内部前向跳转指向克隆
    ///LabelNode 的 Label，而非将被移除的原节点）；区间外 Label 原样直返（同域
    ///identity，继续指向幸存的原节点）。
    ///注意不能委托 resolveCloneLabel：其对「label 已归属目标 arena」短路直返，
    ///不会查询映射表，无法对区间内的同域 Label 实现换新。
    jbytecode::core::Label* cloneMatchedLabel(jbytecode::core::Label* label)
    {
        if (label == nullptr || matchedLabels.find(label) == matchedLabels.end()) return label;

        auto it = clonedLabels.find(label);
        if (it != clonedLabels.end()) return it->second;

        auto* arena = targetArena();
        assert(arena != nullptr && "origin clone requires a method-bound original list");
        auto* copy           = arena->create<jbytecode::core::Label>();
        copy->setOwner(arena);
        copy->bytecodeOffset = label->bytecodeOffset;
        copy->line           = label->line;
        copy->flags          = label->flags;
        clonedLabels.emplace(label, copy);
        return copy;
    }

    ///克隆一条原始指令：Label 携带节点手工重建（强制经 cloneMatchedLabel 换新
    ///区间内 Label），无 Label 节点走 B8 新 clone 协议（clone(arena, map)）。
    jbytecode::tree::AbstractInsnNode*
    cloneOriginInsn(const jbytecode::tree::AbstractInsnNode* insn)
    {
        using Node     = jbytecode::tree::AbstractInsnNode;
        garena::Arena* arena = targetArena();
        assert(arena != nullptr && "origin clone requires a method-bound original list");
        switch (insn->getType()) {
        case Node::LABEL:
        {
            const auto* node = ca::core::tag_cast::cast<const jbytecode::tree::LabelNode*>(insn);
            return arena->create<jbytecode::tree::LabelNode>(cloneMatchedLabel(node->label));
        }
        case Node::JUMP_INSN:
        {
            const auto* node = ca::core::tag_cast::cast<const jbytecode::tree::JumpInsnNode*>(insn);
            return arena->create<jbytecode::tree::JumpInsnNode>(
                insn->getOpcode(), cloneMatchedLabel(node->label));
        }
        case Node::TABLESWITCH_INSN:
        {
            const auto* node =
                ca::core::tag_cast::cast<const jbytecode::tree::TableSwitchInsnNode*>(insn);
            std::vector<jbytecode::core::Label*> labels;
            labels.reserve(node->labels.size());
            for (auto* label : node->labels) labels.push_back(cloneMatchedLabel(label));
            return arena->create<jbytecode::tree::TableSwitchInsnNode>(
                node->min, node->max, cloneMatchedLabel(node->dflt), labels);
        }
        case Node::LOOKUPSWITCH_INSN:
        {
            const auto* node =
                ca::core::tag_cast::cast<const jbytecode::tree::LookupSwitchInsnNode*>(insn);
            std::vector<jbytecode::core::Label*> labels;
            labels.reserve(node->labels.size());
            for (auto* label : node->labels) labels.push_back(cloneMatchedLabel(label));
            return arena->create<jbytecode::tree::LookupSwitchInsnNode>(
                cloneMatchedLabel(node->dflt), node->keys, labels);
        }
        case Node::LINE:
        {
            const auto* node =
                ca::core::tag_cast::cast<const jbytecode::tree::LineNumberNode*>(insn);
            return arena->create<jbytecode::tree::LineNumberNode>(
                node->line, cloneMatchedLabel(node->start));
        }
        default:
            return insn->clone(*arena, clonedLabels);
        }
    }
};

}   //namespace mjt::tmatcher
