#include "mbase/asm/tmatcher/insn_replacer.hpp"

#include "mbase/asm/tmatcher/replace_context.hpp"

namespace mjt::tmatcher {

InsnReplacer::~InsnReplacer() = default;

MatchResult InsnReplacer::matchResult(jbytecode::tree::InsnList& list)
{
    MatchResult result;
    if (traceEnabled_) {
        lastTrace_ = MatchTrace(std::vector<TraceEvent>{});
    }

    for (int i = 0; i < list.size();) {
        auto fw = findLongestMatch(rules_,
                                   list,
                                   i,
                                   traceEnabled_,
                                   traceEnabled_ ? &lastTrace_ : nullptr);
        if (!fw) {
            i++;
            continue;
        }
        result.add(MatchedArea{rules_[fw->sequenceIndex].name(),
                               fw->start,
                               fw->endExclusive,
                               fw->captures});
        i = fw->endExclusive;
    }
    return result;
}

void InsnReplacer::replace(jbytecode::tree::InsnList& list)
{
    if (traceEnabled_) {
        lastTrace_ = MatchTrace(std::vector<TraceEvent>{});
    }

    for (int i = 0; i < list.size();) {
        auto fw = findLongestMatch(rules_,
                                   list,
                                   i,
                                   traceEnabled_,
                                   traceEnabled_ ? &lastTrace_ : nullptr);
        if (!fw) {
            i++;
            continue;
        }

        auto& rule = rules_[fw->sequenceIndex];

        //无 handler：仅跳过命中区间
        if (!rule.hasHandler()) {
            i = fw->endExclusive;
            continue;
        }

        //有 handler：先在 InsnBuilder 中扁平收集替换节点；handler 成功后才链接成 InsnList。
        ReplaceContext ctx;
        ctx.original = &list;
        ctx.start    = fw->start;
        ctx.end      = fw->endExclusive;
        ctx.captures = &fw->captures;
        ctx.prepareOriginClones();

        rule.handler()->handle(ctx);

        auto replacement = ctx.getInsnList();

        //删除命中区间节点（removeAndDelete，修正旧实现只 unlink 的内存泄漏）。
        //对齐 matcher_core.cpp 已验证的删除顺序。
        int range = fw->endExclusive - fw->start;
        for (int j = 0; j < range; j++) {
            auto* node = list.get(fw->start);
            if (node) {
                list.removeAndDelete(node);
            }
        }

        //先统计替换产物条数（drain 前统计，drain 后 replacement 被清空）。
        int insertedCount = replacement.size();

        //一键把 replacement drain 进原位置（用 InsnList::insert 原语，
        //替掉旧实现手写的逐节点 splice 循环，消除脆弱的指针操作）。
        auto* insertBefore = list.get(fw->start);
        if (insertBefore) {
            list.insert(insertBefore, replacement);
        }
        else {
            list.add(replacement);
        }

        i = fw->start + insertedCount;
    }
}

}   //namespace mjt::tmatcher
