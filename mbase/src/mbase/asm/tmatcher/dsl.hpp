#pragma once

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "libca/str/utf8_string.hpp"

#include "jbytecode/tree/insn_list.hpp"

#include "mbase/asm/insn/insn_util.hpp"
#include "mbase/asm/tmatcher/combinators.hpp"
#include "mbase/asm/tmatcher/replace_rule.hpp"
#include "mbase/asm/tmatcher/replace_step.hpp"

namespace mjt::tmatcher {

//============================================================================
//CaptureVar<T>：类型化变量捕获
//============================================================================

template<typename T>
class CaptureVar
{
    struct State
    {
        T    value{};
        bool hasValue = false;
    };
    std::shared_ptr<State> state_;
    ca::str::Utf8StringRef name_;

public:
    explicit CaptureVar(ca::str::Utf8StringRef name)
        : state_(std::make_shared<State>())
        , name_(name)
    {}

    ca::str::Utf8StringRef name() const { return name_; }

    void setCache(T value) const
    {
        state_->value    = value;
        state_->hasValue = true;
    }

    T get() const
    {
        if (!state_->hasValue) {
            throw std::runtime_error("CaptureVar " + name_.to_std_string() + " not bound yet");
        }
        return state_->value;
    }
};

template<typename T>
CaptureVar<T> var(ca::str::Utf8StringRef name)
{
    return CaptureVar<T>(name);
}

template<typename T>
CaptureVar<T> var(const char* name)
{
    return var<T>(ca::str::Utf8StringRef::from_cstr(name));
}

///load(var)：产生一个匹配 step，匹配任意一条指令并按类型提取常量值到 var。
template<typename T>
std::unique_ptr<IMatchStep> load(CaptureVar<T>& cv)
{
    auto name        = cv.name();
    auto captureCopy = cv;
    return combinators::capture(
        name,
        combinators::anyOne(),
        [captureCopy, name](jbytecode::tree::InsnList& list,
                            int                        start,
                            int                        end,
                            const CaptureStore&        captures) mutable -> ca::core::Any {
            (void)end;
            (void)captures;
            auto* node = list.get(start);
            if (!node) throw std::runtime_error("load capture: null node");
            if constexpr (std::is_same_v<T, int>) {
                int val;
                if (!mjt::base::InsnUtil::getIntValue(node, val)) {
                    throw std::runtime_error("load capture: expected int constant");
                }
                captureCopy.setCache(val);
                return val;
            }
            else if constexpr (std::is_same_v<T, double>) {
                double val;
                if (!mjt::base::InsnUtil::getDoubleValue(node, val)) {
                    throw std::runtime_error("load capture: expected double constant");
                }
                captureCopy.setCache(val);
                return val;
            }
            else if constexpr (std::is_same_v<T, float>) {
                float val;
                if (!mjt::base::InsnUtil::getFloatValue(node, val)) {
                    throw std::runtime_error("load capture: expected float constant");
                }
                captureCopy.setCache(val);
                return val;
            }
            else if constexpr (std::is_same_v<T, ca::i64>) {
                ca::i64 val;
                if (!mjt::base::InsnUtil::getLongValue(node, val)) {
                    throw std::runtime_error("load capture: expected long constant");
                }
                captureCopy.setCache(val);
                return val;
            }
            throw std::runtime_error("unsupported capture type");
        });
}

//============================================================================
//运算符重载：- (seq) / | (or) / >> (rule)
//============================================================================

///a - b = seq(a, b)
inline std::unique_ptr<IMatchStep> operator-(std::unique_ptr<IMatchStep> a,
                                             std::unique_ptr<IMatchStep> b)
{
    std::vector<std::unique_ptr<IMatchStep>> nodes;
    nodes.push_back(std::move(a));
    nodes.push_back(std::move(b));
    return combinators::seq(std::move(nodes));
}

///a | b = or_(a, b)
inline std::unique_ptr<IMatchStep> operator|(std::unique_ptr<IMatchStep> a,
                                             std::unique_ptr<IMatchStep> b)
{
    std::vector<std::unique_ptr<IMatchStep>> branches;
    branches.push_back(std::move(a));
    branches.push_back(std::move(b));
    return combinators::or_(std::move(branches));
}

//---- operator>> : step + handler → ReplaceRule ----
//产出的 ReplaceRule 归 InsnMatchReplacer / InsnReplacer（又匹配又替换的层）。
//纯匹配器 InsnMatcher 不认 ReplaceRule，只认 MatchSequence（addSequence）。

///step >> lambda → ReplaceRule(LambdaHandler)
template<typename Fn>
ReplaceRule operator>>(std::unique_ptr<IMatchStep> step, Fn&& strategy)
{
    return ReplaceRule("unnamed",
                       std::move(step),
                       std::make_unique<LambdaHandler>(
                           std::function<void(ReplaceContext&)>(std::forward<Fn>(strategy))));
}

///step >> ReplaceSequence → ReplaceRule(SequenceHandler)
inline ReplaceRule operator>>(std::unique_ptr<IMatchStep> step, ReplaceSequence seq)
{
    auto handler = std::make_unique<SequenceHandler>();
    handler->sequence() = std::move(seq);
    return ReplaceRule("unnamed", std::move(step), std::move(handler));
}

//============================================================================
//大括号语法糖：{a - b - c} 隐式构造序列
//============================================================================

///{step1, step2, ...} → seq(...)。支持 {iload(a) - iload(b) - ixor} 写法。
inline std::unique_ptr<IMatchStep> makeSeq(std::initializer_list<std::unique_ptr<IMatchStep>> steps)
{
    std::vector<std::unique_ptr<IMatchStep>> v;
    for (auto& s : steps) v.push_back(std::move(const_cast<std::unique_ptr<IMatchStep>&>(s)));
    return combinators::seq(std::move(v));
}

///{replaceStep1, replaceStep2, ...} → ReplaceSequence。
inline ReplaceSequence makeReplaceSeq(
    std::initializer_list<std::unique_ptr<ReplaceStep>> steps)
{
    ReplaceSequence seq;
    for (auto& s : steps) seq.add(std::move(const_cast<std::unique_ptr<ReplaceStep>&>(s)));
    return seq;
}

}   //namespace mjt::tmatcher
