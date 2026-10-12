#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "mbase/asm/tmatcher/match_sequence.hpp"
#include "mbase/asm/tmatcher/replace_context.hpp"
#include "mbase/asm/tmatcher/replace_step.hpp"

namespace mjt::tmatcher {

//============================================================================
//ReplaceRuleHandler：匹配命中 → 替换执行的抽象
//============================================================================

///匹配规则处理器抽象基类。一次命中触发一次 handle。
///两种实现同级：
///- LambdaHandler：直接 lambda，适合复杂/计算型替换（原 strategy lambda）
///- SequenceHandler：包装声明式 ReplaceSequence，适合结构化替换
class ReplaceRuleHandler
{
public:
    virtual ~ReplaceRuleHandler() = default;
    virtual void handle(ReplaceContext& ctx) = 0;
};

///Lambda 处理器：包装 std::function<void(ReplaceContext&)>。
class LambdaHandler : public ReplaceRuleHandler
{
    std::function<void(ReplaceContext&)> fn_;

public:
    explicit LambdaHandler(std::function<void(ReplaceContext&)> fn)
        : fn_(std::move(fn))
    {}

    void handle(ReplaceContext& ctx) override { fn_(ctx); }
};

///序列处理器：内部持有 ReplaceSequence，handle 时依次 emit。
class SequenceHandler : public ReplaceRuleHandler
{
    ReplaceSequence seq_;

public:
    ReplaceSequence&       sequence() { return seq_; }
    const ReplaceSequence& sequence() const { return seq_; }

    void handle(ReplaceContext& ctx) override { seq_.emit(ctx); }
};

//============================================================================
//ReplaceRule：一个命名匹配序列 + 一个可选的替换处理器
//============================================================================

///替换规则 = MatchSequence + 可选 ReplaceRuleHandler。
///handler 为空时该规则仅匹配不替换（match-only 用法）。
///InsnMatcher 用其中的序列部分，InsnReplacer/InsnMatchReplacer 用整体。
class ReplaceRule
{
    MatchSequence                            seq_;
    std::unique_ptr<ReplaceRuleHandler>      handler_;

public:
    ReplaceRule(ca::str::Utf8StringRef               name,
                std::unique_ptr<IMatchStep>          step,
                std::unique_ptr<ReplaceRuleHandler>  handler = nullptr)
        : seq_(name, std::move(step))
        , handler_(std::move(handler))
    {}

    ReplaceRule(const char*                          name,
                std::unique_ptr<IMatchStep>          step,
                std::unique_ptr<ReplaceRuleHandler>  handler = nullptr)
        : ReplaceRule(ca::str::Utf8StringRef::from_cstr(name),
                      std::move(step),
                      std::move(handler))
    {}

    ca::str::Utf8StringRef name() const { return seq_.name(); }
    IMatchStep&             step() const { return seq_.step(); }
    MatchSequence&      sequence() { return seq_; }
    const MatchSequence& sequence() const { return seq_; }
    bool                hasHandler() const { return !!handler_; }
    ReplaceRuleHandler* handler() const { return handler_.get(); }
};

}   //namespace mjt::tmatcher
