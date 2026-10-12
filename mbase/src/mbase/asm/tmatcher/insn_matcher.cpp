#include "mbase/asm/tmatcher/insn_matcher.hpp"

namespace mjt::tmatcher {

std::optional<SequenceWin> InsnMatcher::findWinner(jbytecode::tree::InsnList& list, int start)
{
    return findLongestMatch(sequences_,
                             list,
                             start,
                             traceEnabled_,
                             traceEnabled_ ? &lastTrace_ : nullptr);
}

MatchResult InsnMatcher::match(jbytecode::tree::InsnList& list)
{
    MatchResult result;
    if (traceEnabled_) {
        lastTrace_ = MatchTrace(std::vector<TraceEvent>{});
    }

    for (int i = 0; i < list.size();) {
        auto fw = findWinner(list, i);
        if (!fw) {
            i++;
            continue;
        }
        result.add(MatchedArea{sequences_[fw->sequenceIndex].name(),
                               fw->start,
                               fw->endExclusive,
                               fw->captures});
        i = fw->endExclusive;
    }
    return result;
}

}   //namespace mjt::tmatcher
