#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jbytecode/tree/insn_list.hpp"

#include "mbase/asm/tmatcher/insn_matcher.hpp"
#include "mbase/asm/tmatcher/insn_replacer.hpp"
#include "mbase/asm/tmatcher/match_sequence.hpp"
#include "mbase/asm/tmatcher/match_tracer.hpp"
#include "mbase/asm/tmatcher/replace_rule.hpp"

namespace mjt::tmatcher {

///高层组合器：又匹配又替换的便捷层。内部持有 ReplaceRule（序列+handler），
///对外提供"加规则、一步 apply"的简洁接口。
///
///这是 operator>> DSL 的归属点：step >> handler 产出 ReplaceRule，只能加到这里。
///- apply(list)：匹配→替换一步到位（委托 InsnReplacer）。
///- match(list)：纯匹配拿结果（在规则上用 findLongestMatch，不执行替换）。
///需要单独用纯匹配器或纯替换器的场景，直接用 InsnMatcher / InsnReplacer。
class InsnMatchReplacer
{
    InsnReplacer replacer_;

public:
    ///添加规则（>> DSL 产出的 ReplaceRule）。同步可用于 match 与 apply。
    void addRule(ReplaceRule rule) { replacer_.addRule(std::move(rule)); }

    void              setTraceEnabled(bool enabled) { replacer_.setTraceEnabled(enabled); }
    bool              isTraceEnabled() const { return replacer_.isTraceEnabled(); }
    const MatchTrace& lastTrace() const { return replacer_.lastTrace(); }

    ///匹配并替换（委托 InsnReplacer）。
    void apply(jbytecode::tree::InsnList& list) { replacer_.replace(list); }

    ///纯匹配：在规则上用 findLongestMatch 收集命中，不执行替换、不修改 list。
    MatchResult match(jbytecode::tree::InsnList& list) { return replacer_.matchResult(list); }
};

}   //namespace mjt::tmatcher
