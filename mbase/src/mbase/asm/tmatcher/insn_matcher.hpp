#pragma once

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "jbytecode/tree/insn_list.hpp"

#include "mbase/asm/tmatcher/match_sequence.hpp"
#include "mbase/asm/tmatcher/match_state.hpp"
#include "mbase/asm/tmatcher/match_tracer.hpp"

namespace mjt::tmatcher {

///单条序列在 start 处的最长命中结果。
struct SequenceWin
{
    int          sequenceIndex;   //sequences 中的下标
    int          start;
    int          endExclusive;
    CaptureStore captures;
};

///在一组序列中找出从 start 出发的最长命中（最长匹配优先，同长按注册顺序）。
///模板化：InsnMatcher 传 vector<MatchSequence>，InsnReplacer 传 vector<ReplaceRule>
///（ReplaceRule 须提供 name/step）。无命中返回 nullopt。
///这是 InsnMatcher 与 InsnReplacer 共享的调度逻辑，避免两份调度代码。
template<typename SeqContainer>
std::optional<SequenceWin> findLongestMatch(SeqContainer&               sequences,
                                            jbytecode::tree::InsnList&  list,
                                            int                         start,
                                            bool                        tracerEnabled,
                                            MatchTrace*                 outTrace)
{
    std::optional<SequenceWin> best;
    int                        bestLength = -1;
    MatchTrace                 bestTrace{std::vector<TraceEvent>{}};

    size_t idx = 0;
    for (auto it = sequences.begin(); it != sequences.end(); ++it, ++idx) {
        auto&       seq    = *it;
        MatchTracer tracer = tracerEnabled ? MatchTracer::recording() : MatchTracer::noop();
        MatchState  init   = MatchState::initial(start);
        auto        states = seq.step().match(list, init, tracer, 0);

        if (states.empty()) {
            continue;
        }

        std::sort(states.begin(), states.end(),
                  [](auto& a, auto& b) { return a.cursor > b.cursor; });
        auto& bestState = states[0];
        int   length    = bestState.cursor - start;
        if (length <= 0) continue;

        if (length > bestLength || (length == bestLength && !best.has_value())) {
            bestLength = length;
            if (tracerEnabled) bestTrace = tracer.snapshot();
            best = SequenceWin{static_cast<int>(idx), start, bestState.cursor, bestState.captures};
        }
    }

    if (best && tracerEnabled && outTrace) {
        *outTrace = std::move(bestTrace);
    }
    return best;
}

///纯匹配器：只持有 MatchSequence，唯一职责是 match 返回命中区域。
///不包含任何替换/handler 概念。又匹配又替换的场景用 InsnMatchReplacer。
class InsnMatcher
{
    std::vector<MatchSequence> sequences_;
    bool                       traceEnabled_ = false;
    MatchTrace                 lastTrace_{std::vector<TraceEvent>{}};

public:
    ///添加匹配序列。
    void addSequence(MatchSequence seq) { sequences_.push_back(std::move(seq)); }

    void              setTraceEnabled(bool enabled) { traceEnabled_ = enabled; }
    bool              isTraceEnabled() const { return traceEnabled_; }
    const MatchTrace& lastTrace() const { return lastTrace_; }

    ///纯匹配：遍历 list 所有位置，对每个位置取最长命中，收集到 MatchResult。
    ///不修改 list。命中区间不重叠（命中后游标跳到 endExclusive）。
    MatchResult match(jbytecode::tree::InsnList& list);

    ///暴露给 InsnReplacer 复用的内部调度（按最长匹配在每个游标取一次命中）。
    std::optional<SequenceWin> findWinner(jbytecode::tree::InsnList& list, int start);
};

}   //namespace mjt::tmatcher
