#pragma once

#include <memory>
#include <string>
#include <vector>

#include "jbytecode/tree/insn_list.hpp"

#include "mbase/asm/tmatcher/insn_matcher.hpp"
#include "mbase/asm/tmatcher/match_sequence.hpp"
#include "mbase/asm/tmatcher/match_tracer.hpp"
#include "mbase/asm/tmatcher/replace_rule.hpp"

namespace mjt::tmatcher {

///纯替换器：持有 ReplaceRule（序列 + handler），在 list 上驱动匹配→替换循环。
///
///与 InsnMatcher 的关系：内部用 findLongestMatch 自驱匹配（复用同一调度逻辑），
///但只做替换、不收集 MatchResult。与 InsnMatchReplacer 的关系：后者在 addRule 时
///把规则同步登记到 InsnMatcher（match 用）和 InsnReplacer（apply 用）。
///
///替换执行模型：遍历原 list（游标推进），命中且有 handler 时删除命中区间
///（remove + delete，修正旧实现的内存泄漏）。handler 先向 ReplaceContext 内部的
///InsnBuilder 扁平收集替换节点，成功后 materialize 为 InsnList，再用 insert 原语
///一键 splice 进原位置，游标跳到替换后。
class InsnReplacer
{
    std::vector<ReplaceRule> rules_;
    bool                     traceEnabled_ = false;
    MatchTrace               lastTrace_{std::vector<TraceEvent>{}};

public:
    InsnReplacer() = default;
    ~InsnReplacer();

    InsnReplacer(const InsnReplacer&)            = delete;
    InsnReplacer& operator=(const InsnReplacer&) = delete;
    InsnReplacer(InsnReplacer&&) noexcept            = default;
    InsnReplacer& operator=(InsnReplacer&&) noexcept = default;

    ///添加替换规则（序列 + 可选 handler）。handler 为空则命中时仅跳过区间。
    void addRule(ReplaceRule rule) { rules_.push_back(std::move(rule)); }

    void              setTraceEnabled(bool enabled) { traceEnabled_ = enabled; }
    bool              isTraceEnabled() const { return traceEnabled_; }
    const MatchTrace& lastTrace() const { return lastTrace_; }

    ///匹配并替换：对有 handler 的命中执行替换，原地修改 list。
    ///无 handler 的命中仅跳过命中区间。修正旧实现的内存泄漏。
    void replace(jbytecode::tree::InsnList& list);

    ///纯匹配：在规则上用 findLongestMatch 收集命中，不执行替换、不修改 list。
    ///供 InsnMatchReplacer::match 复用（replacer 持有规则，matcher 无需重复持有）。
    MatchResult matchResult(jbytecode::tree::InsnList& list);
};

}   //namespace mjt::tmatcher
