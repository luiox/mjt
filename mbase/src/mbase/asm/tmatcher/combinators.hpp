#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "libca/str/utf8_string.hpp"

#include "jbytecode/tree/insn_list.hpp"
#include "jbytecode/tree/insn_type_traits.hpp"
#include "jbytecode/core/opcodes.hpp"

#include "mbase/asm/tmatcher/match_step.hpp"
#include "mbase/asm/tmatcher/match_state.hpp"
#include "mbase/asm/tmatcher/match_tracer.hpp"

namespace mjt::tmatcher {

//============================================================================
//原子 Step：单指令匹配
//============================================================================

///通用叶子 step：用谓词判定当前指令。所有具体原子 step 的基类。
class LeafStep : public IMatchStep
{
    std::string                                                                  name_;
    std::function<bool(jbytecode::tree::AbstractInsnNode*, const CaptureStore&)> pred_;

public:
    LeafStep(std::string name,
             std::function<bool(jbytecode::tree::AbstractInsnNode*, const CaptureStore&)> pred)
        : name_(std::move(name))
        , pred_(std::move(pred))
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        tracer.enter(name_, depth, state.cursor);
        if (state.cursor >= list.size()) {
            tracer.fail(name_, depth, state.cursor, "cursor out of bounds");
            return {};
        }
        auto* now = list.get(state.cursor);
        if (!now || !pred_(now, state.captures)) {
            tracer.fail(name_, depth, state.cursor, "predicate failed");
            return {};
        }
        auto next = state.withCursor(state.cursor + 1);
        tracer.success(name_, depth, state.cursor, next.cursor);
        return {next};
    }
};

///匹配指定 opcode 的单条指令。
class OpcodeStep : public IMatchStep
{
    int opc_;
    std::string name_;

public:
    explicit OpcodeStep(int opc)
        : opc_(opc)
        , name_("opcode(" + std::to_string(opc) + ")")
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        tracer.enter(name_, depth, state.cursor);
        if (state.cursor >= list.size()) {
            tracer.fail(name_, depth, state.cursor, "cursor out of bounds");
            return {};
        }
        auto* now = list.get(state.cursor);
        if (!now || now->getOpcode() != opc_) {
            tracer.fail(name_, depth, state.cursor, "opcode mismatch");
            return {};
        }
        auto next = state.withCursor(state.cursor + 1);
        tracer.success(name_, depth, state.cursor, next.cursor);
        return {next};
    }
};

///匹配任意一条指令（issue 中的 anyOne）。
class AnyOneStep : public IMatchStep
{
public:
    std::string name() const override { return "anyOne"; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        tracer.enter("anyOne", depth, state.cursor);
        if (state.cursor >= list.size()) {
            tracer.fail("anyOne", depth, state.cursor, "cursor out of bounds");
            return {};
        }
        auto next = state.withCursor(state.cursor + 1);
        tracer.success("anyOne", depth, state.cursor, next.cursor);
        return {next};
    }
};

///匹配指定 opcode 的 VAR_INSN，并可选捕获槽位号。
class VarInsnStep : public IMatchStep
{
    int         opc_;
    std::string name_;

public:
    explicit VarInsnStep(int opc)
        : opc_(opc)
        , name_("varInsn(" + std::to_string(opc) + ")")
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        tracer.enter(name_, depth, state.cursor);
        if (state.cursor >= list.size()) {
            tracer.fail(name_, depth, state.cursor, "cursor out of bounds");
            return {};
        }
        auto* now = list.get(state.cursor);
        if (!now || now->getType() != jbytecode::tree::AbstractInsnNode::VAR_INSN ||
            now->getOpcode() != opc_) {
            tracer.fail(name_, depth, state.cursor, "varInsn mismatch");
            return {};
        }
        auto next = state.withCursor(state.cursor + 1);
        tracer.success(name_, depth, state.cursor, next.cursor);
        return {next};
    }
};

///方法调用匹配基类：INVOKESTATIC / INVOKEVIRTUAL 等。
class InvokeStep : public IMatchStep
{
    int                   opc_;
    ca::str::Utf8StringRef owner_;
    ca::str::Utf8StringRef methodName_;
    ca::str::Utf8StringRef desc_;
    std::string           name_;

public:
    InvokeStep(int                   opc,
               ca::str::Utf8StringRef owner,
               ca::str::Utf8StringRef methodName,
               ca::str::Utf8StringRef desc,
               std::string            label)
        : opc_(opc)
        , owner_(owner)
        , methodName_(methodName)
        , desc_(desc)
        , name_(std::move(label))
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        tracer.enter(name_, depth, state.cursor);
        if (state.cursor >= list.size()) {
            tracer.fail(name_, depth, state.cursor, "cursor out of bounds");
            return {};
        }
        auto* now = list.get(state.cursor);
        if (!now || now->getOpcode() != opc_) {
            tracer.fail(name_, depth, state.cursor, "opcode mismatch");
            return {};
        }
        auto* m = ca::core::tag_cast::cast<jbytecode::tree::MethodInsnNode>(now);
        bool ok = m->owner == owner_ && m->name == methodName_ && m->desc == desc_;
        if (!ok) {
            tracer.fail(name_, depth, state.cursor, "owner/name/desc mismatch");
            return {};
        }
        auto next = state.withCursor(state.cursor + 1);
        tracer.success(name_, depth, state.cursor, next.cursor);
        return {next};
    }
};

//============================================================================
//逻辑组合子：seq / or / and / not
//============================================================================

///顺序组合：依次匹配所有子 step，前一个的候选状态喂给后一个。
class SeqStep : public IMatchStep
{
    std::vector<std::unique_ptr<IMatchStep>> nodes_;
    std::string                              name_;

public:
    explicit SeqStep(std::vector<std::unique_ptr<IMatchStep>> nodes)
        : nodes_(std::move(nodes))
        , name_("seq")
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        tracer.enter("seq", depth, state.cursor, "size=" + std::to_string(nodes_.size()));
        std::vector<MatchState> states = {state};
        for (size_t i = 0; i < nodes_.size(); i++) {
            std::vector<MatchState> next;
            for (auto& s : states) {
                auto branch = nodes_[i]->match(list, s, tracer, depth + 1);
                next.insert(next.end(), branch.begin(), branch.end());
            }
            if (next.empty()) {
                tracer.fail("seq", depth, state.cursor, "child " + std::to_string(i) + " failed");
                return {};
            }
            states = std::move(next);
        }
        std::sort(states.begin(), states.end(),
                  [](auto& a, auto& b) { return a.cursor > b.cursor; });
        tracer.success("seq",
                       depth,
                       state.cursor,
                       states[0].cursor,
                       "candidates=" + std::to_string(states.size()));
        return states;
    }
};

///选择组合：任一子 step 成功即成功，合并所有候选。
class OrStep : public IMatchStep
{
    std::vector<std::unique_ptr<IMatchStep>> branches_;
    std::string                              name_;

public:
    explicit OrStep(std::vector<std::unique_ptr<IMatchStep>> branches)
        : branches_(std::move(branches))
        , name_("or")
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        tracer.enter("or", depth, state.cursor, "branches=" + std::to_string(branches_.size()));
        std::vector<MatchState> results;
        for (auto& branch : branches_) {
            auto matched = branch->match(list, state, tracer, depth + 1);
            results.insert(results.end(), matched.begin(), matched.end());
        }
        if (results.empty()) {
            tracer.fail("or", depth, state.cursor, "all branches failed");
            return {};
        }
        std::sort(results.begin(), results.end(),
                  [](auto& a, auto& b) { return a.cursor > b.cursor; });
        tracer.success("or",
                       depth,
                       state.cursor,
                       results[0].cursor,
                       "candidates=" + std::to_string(results.size()));
        return results;
    }
};

///与组合：所有子 step 必须在同一游标位置成功，捕获表合并（冲突则丢弃候选）。
class AndStep : public IMatchStep
{
    std::vector<std::unique_ptr<IMatchStep>> nodes_;
    std::string                              name_;

public:
    explicit AndStep(std::vector<std::unique_ptr<IMatchStep>> nodes)
        : nodes_(std::move(nodes))
        , name_("and")
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        tracer.enter("and", depth, state.cursor, "size=" + std::to_string(nodes_.size()));
        std::vector<MatchState> merged;
        for (size_t i = 0; i < nodes_.size(); i++) {
            auto branch = nodes_[i]->match(list, state, tracer, depth + 1);
            if (branch.empty()) {
                tracer.fail("and", depth, state.cursor, "branch " + std::to_string(i) + " failed");
                return {};
            }
            if (i == 0) {
                merged = std::move(branch);
                continue;
            }
            std::vector<MatchState> next;
            for (auto& left : merged) {
                for (auto& right : branch) {
                    if (left.cursor != right.cursor) continue;
                    auto captures = left.captures.merge(right.captures);
                    if (!captures) continue;
                    next.push_back({left.cursor, *captures});
                }
            }
            if (next.empty()) {
                tracer.fail("and", depth, state.cursor, "no merged candidate");
                return {};
            }
            merged = std::move(next);
        }
        std::sort(merged.begin(), merged.end(),
                  [](auto& a, auto& b) { return a.cursor > b.cursor; });
        tracer.success("and",
                       depth,
                       state.cursor,
                       merged[0].cursor,
                       "candidates=" + std::to_string(merged.size()));
        return merged;
    }
};

///否定组合：子 step 失败则成功（游标不前进），成功则失败。
class NotStep : public IMatchStep
{
    std::unique_ptr<IMatchStep> node_;
    std::string                 name_;

public:
    explicit NotStep(std::unique_ptr<IMatchStep> node)
        : node_(std::move(node))
        , name_("not")
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        tracer.enter("not", depth, state.cursor);
        auto matched = node_->match(list, state, tracer, depth + 1);
        if (!matched.empty()) {
            tracer.fail("not", depth, state.cursor, "child matched");
            return {};
        }
        tracer.success("not", depth, state.cursor, state.cursor, "negative check passed");
        return {state};
    }
};

//============================================================================
//量词组合子：optional / repeat / any / anyMax / repeatMax / repeatMn
//============================================================================

///可选组合：匹配 0 或 1 次，贪婪优先（先尝试匹配，再回退不匹配）。
class OptionalStep : public IMatchStep
{
    std::unique_ptr<IMatchStep> node_;
    std::string                 name_;

public:
    explicit OptionalStep(std::unique_ptr<IMatchStep> node)
        : node_(std::move(node))
        , name_("optional")
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        tracer.enter("optional", depth, state.cursor);
        auto matched = node_->match(list, state, tracer, depth + 1);
        matched.push_back(state);
        std::sort(matched.begin(), matched.end(),
                  [](auto& a, auto& b) { return a.cursor > b.cursor; });
        tracer.success("optional", depth, state.cursor, matched[0].cursor);
        return matched;
    }
};

///重复组合：重复匹配 [min, max] 次。max=-1 表示无上限。
///候选按 cursor 降序排列（贪婪优先），findWinner 取最长。
class RepeatStep : public IMatchStep
{
    std::unique_ptr<IMatchStep> node_;
    int                         min_;
    int                         max_;
    std::string                 name_;

public:
    RepeatStep(std::unique_ptr<IMatchStep> node, int min, int max)
        : node_(std::move(node))
        , min_(min)
        , max_(max)
        , name_("repeat(min=" + std::to_string(min) + ",max=" +
               (max == -1 ? std::string("inf") : std::to_string(max)) + ")")
    {
        if (min < 0 || (max != -1 && max < min)) {
            throw std::invalid_argument("Invalid repeat range: min=" + std::to_string(min) +
                                        ", max=" + std::to_string(max));
        }
    }

    std::string name() const override { return name_; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        tracer.enter(name_, depth, state.cursor, "min=" + std::to_string(min_) + ",max=" +
                                                     (max_ == -1 ? std::string("inf")
                                                                 : std::to_string(max_)));
        std::vector<MatchState> accepted;
        std::vector<MatchState> current = {state};
        if (min_ == 0) accepted.push_back(state);
        int upperBound = max_ == -1 ? list.size() - state.cursor : max_;
        for (int count = 1; count <= upperBound; count++) {
            std::vector<MatchState> next;
            for (auto& candidate : current) {
                for (auto& out : node_->match(list, candidate, tracer, depth + 1)) {
                    if (out.cursor <= candidate.cursor) {
                        tracer.fail(name_, depth, candidate.cursor, "child did not advance");
                        continue;
                    }
                    next.push_back(out);
                }
            }
            if (next.empty()) break;
            current = std::move(next);
            if (count >= min_) {
                accepted.insert(accepted.end(), current.begin(), current.end());
            }
        }
        if (accepted.empty()) {
            tracer.fail(name_, depth, state.cursor, "min bound not met");
            return {};
        }
        std::sort(accepted.begin(), accepted.end(),
                  [](auto& a, auto& b) { return a.cursor > b.cursor; });
        tracer.success(name_,
                       depth,
                       state.cursor,
                       accepted[0].cursor,
                       "candidates=" + std::to_string(accepted.size()));
        return accepted;
    }
};

//============================================================================
//捕获与守卫组合子
//============================================================================

///捕获组合：匹配子 step，用 extractor 从命中区间提取值绑定到变量。
class CaptureStep : public IMatchStep
{
    ca::str::Utf8StringRef                                                       name_;
    std::unique_ptr<IMatchStep>                                                  node_;
    std::function<ca::core::Any(jbytecode::tree::InsnList&, int, int, const CaptureStore&)>
        extractor_;

public:
    CaptureStep(ca::str::Utf8StringRef name,
                std::unique_ptr<IMatchStep>                                       node,
                std::function<ca::core::Any(jbytecode::tree::InsnList&, int, int, const CaptureStore&)>
                    extractor)
        : name_(name)
        , node_(std::move(node))
        , extractor_(std::move(extractor))
    {}

    std::string name() const override { return "capture(" + name_.to_std_string() + ")"; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        std::string label = "capture(" + name_.to_std_string() + ")";
        tracer.enter(label, depth, state.cursor);
        int  start   = state.cursor;
        auto matched = node_->match(list, state, tracer, depth + 1);
        if (matched.empty()) {
            tracer.fail(label, depth, state.cursor, "child failed");
            return {};
        }
        std::vector<MatchState> out;
        for (auto& candidate : matched) {
            ca::core::Any captured;
            try {
                captured = extractor_(list, start, candidate.cursor, candidate.captures);
            }
            catch (...) {
                tracer.fail(label, depth, state.cursor, "extractor error");
                continue;
            }
            auto bound = candidate.captures.bind(name_, captured);
            if (!bound) {
                tracer.fail(label, depth, state.cursor, "capture conflict");
                continue;
            }
            out.push_back(candidate.withCaptures(*bound));
        }
        if (out.empty()) {
            tracer.fail(label, depth, state.cursor, "all candidates rejected");
            return {};
        }
        std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.cursor > b.cursor; });
        tracer.success(label, depth, state.cursor, out[0].cursor);
        return out;
    }
};

///守卫组合：不消费指令，仅检查捕获表是否满足谓词。
class GuardStep : public IMatchStep
{
    std::string                                       description_;
    std::function<bool(const CaptureStore&)>          pred_;
    std::string                                       name_;

public:
    GuardStep(std::string description, std::function<bool(const CaptureStore&)> pred)
        : description_(std::move(description))
        , pred_(std::move(pred))
        , name_("guard(" + description_ + ")")
    {}

    std::string name() const override { return name_; }

    std::vector<MatchState> match(jbytecode::tree::InsnList& list,
                                  const MatchState&          state,
                                  MatchTracer&               tracer,
                                  int                        depth) override
    {
        (void)list;
        tracer.enter(name_, depth, state.cursor, description_);
        if (!pred_(state.captures)) {
            tracer.fail(name_, depth, state.cursor, description_);
            return {};
        }
        tracer.success(name_, depth, state.cursor, state.cursor, description_);
        return {state};
    }
};

//============================================================================
//工厂函数：返回 std::unique_ptr<IMatchStep>
//============================================================================

namespace combinators {

//---------- 原子 step ----------

inline std::unique_ptr<IMatchStep> leaf(
    std::string                                                                  name,
    std::function<bool(jbytecode::tree::AbstractInsnNode*, const CaptureStore&)> pred)
{
    return std::make_unique<LeafStep>(std::move(name), std::move(pred));
}

inline std::unique_ptr<IMatchStep> opcode(int opc)
{
    return std::make_unique<OpcodeStep>(opc);
}

///匹配任意一条指令（issue 中的 anyOne）。
inline std::unique_ptr<IMatchStep> anyOne()
{
    return std::make_unique<AnyOneStep>();
}

///匹配任意多条指令（issue 中的 any 量词）。等价 repeat(anyOne, 0, -1)。
inline std::unique_ptr<IMatchStep> any()
{
    return std::make_unique<RepeatStep>(std::make_unique<AnyOneStep>(), 0, -1);
}

///匹配 0 到 max 条任意指令（issue 中的 anyMax）。
inline std::unique_ptr<IMatchStep> anyMax(int max)
{
    return std::make_unique<RepeatStep>(std::make_unique<AnyOneStep>(), 0, max);
}

inline std::unique_ptr<IMatchStep> varInsn(int opc)
{
    return std::make_unique<VarInsnStep>(opc);
}

inline std::unique_ptr<IMatchStep> invokestatic(ca::str::Utf8StringRef owner,
                                                ca::str::Utf8StringRef name,
                                                ca::str::Utf8StringRef desc)
{
    return std::make_unique<InvokeStep>(jbytecode::core::Opcodes::INVOKESTATIC,
                                        owner,
                                        name,
                                        desc,
                                        "invokestatic(" + owner.to_std_string() + "." +
                                            name.to_std_string() + desc.to_std_string() + ")");
}

inline std::unique_ptr<IMatchStep> invokestatic(const char* owner,
                                                const char* name,
                                                const char* desc)
{
    return invokestatic(ca::str::Utf8StringRef::from_cstr(owner),
                        ca::str::Utf8StringRef::from_cstr(name),
                        ca::str::Utf8StringRef::from_cstr(desc));
}

inline std::unique_ptr<IMatchStep> invokevirtual(ca::str::Utf8StringRef owner,
                                                 ca::str::Utf8StringRef name,
                                                 ca::str::Utf8StringRef desc)
{
    return std::make_unique<InvokeStep>(jbytecode::core::Opcodes::INVOKEVIRTUAL,
                                        owner,
                                        name,
                                        desc,
                                        "invokevirtual(" + owner.to_std_string() + "." +
                                            name.to_std_string() + desc.to_std_string() + ")");
}

inline std::unique_ptr<IMatchStep> invokevirtual(const char* owner,
                                                 const char* name,
                                                 const char* desc)
{
    return invokevirtual(ca::str::Utf8StringRef::from_cstr(owner),
                         ca::str::Utf8StringRef::from_cstr(name),
                         ca::str::Utf8StringRef::from_cstr(desc));
}

//---------- 逻辑组合子 ----------

inline std::unique_ptr<IMatchStep> seq(std::vector<std::unique_ptr<IMatchStep>> nodes)
{
    return std::make_unique<SeqStep>(std::move(nodes));
}

template<typename... Args, typename = std::enable_if_t<(sizeof...(Args) > 1)>>
std::unique_ptr<IMatchStep> seq(Args&&... args)
{
    std::vector<std::unique_ptr<IMatchStep>> nodes;
    (nodes.push_back(std::forward<Args>(args)), ...);
    return seq(std::move(nodes));
}

inline std::unique_ptr<IMatchStep> or_(std::vector<std::unique_ptr<IMatchStep>> branches)
{
    return std::make_unique<OrStep>(std::move(branches));
}

template<typename... Args, typename = std::enable_if_t<(sizeof...(Args) > 1)>>
std::unique_ptr<IMatchStep> or_(Args&&... args)
{
    std::vector<std::unique_ptr<IMatchStep>> branches;
    (branches.push_back(std::forward<Args>(args)), ...);
    return or_(std::move(branches));
}

inline std::unique_ptr<IMatchStep> and_(std::vector<std::unique_ptr<IMatchStep>> nodes)
{
    return std::make_unique<AndStep>(std::move(nodes));
}

template<typename... Args, typename = std::enable_if_t<(sizeof...(Args) > 1)>>
std::unique_ptr<IMatchStep> and_(Args&&... args)
{
    std::vector<std::unique_ptr<IMatchStep>> nodes;
    (nodes.push_back(std::forward<Args>(args)), ...);
    return and_(std::move(nodes));
}

inline std::unique_ptr<IMatchStep> not_(std::unique_ptr<IMatchStep> node)
{
    return std::make_unique<NotStep>(std::move(node));
}

inline std::unique_ptr<IMatchStep> optional(std::unique_ptr<IMatchStep> node)
{
    return std::make_unique<OptionalStep>(std::move(node));
}

//---------- 量词组合子 ----------

///重复精确 n 次。
inline std::unique_ptr<IMatchStep> repeat(std::unique_ptr<IMatchStep> node, int n)
{
    return std::make_unique<RepeatStep>(std::move(node), n, n);
}

///重复 [min, max] 次，max=-1 表示无上限。
inline std::unique_ptr<IMatchStep> repeat(std::unique_ptr<IMatchStep> node, int min, int max)
{
    return std::make_unique<RepeatStep>(std::move(node), min, max);
}

///重复最多 max 次（0 到 max）。issue 中的 repeatMax。
inline std::unique_ptr<IMatchStep> repeatMax(std::unique_ptr<IMatchStep> node, int max)
{
    return std::make_unique<RepeatStep>(std::move(node), 0, max);
}

///重复最少 min 次、最多 max 次。issue 中的 repeatMn。
inline std::unique_ptr<IMatchStep> repeatMn(std::unique_ptr<IMatchStep> node, int min, int max)
{
    return std::make_unique<RepeatStep>(std::move(node), min, max);
}

//---------- 捕获与守卫 ----------

inline std::unique_ptr<IMatchStep> capture(
    ca::str::Utf8StringRef                                                       name,
    std::unique_ptr<IMatchStep>                                                  node,
    std::function<ca::core::Any(jbytecode::tree::InsnList&, int, int, const CaptureStore&)>
        extractor)
{
    return std::make_unique<CaptureStep>(name, std::move(node), std::move(extractor));
}

inline std::unique_ptr<IMatchStep> capture(const char*                                                        name,
                                           std::unique_ptr<IMatchStep>                                        node,
                                           std::function<ca::core::Any(jbytecode::tree::InsnList&,
                                                                       int,
                                                                       int,
                                                                       const CaptureStore&)> extractor)
{
    return capture(ca::str::Utf8StringRef::from_cstr(name), std::move(node), std::move(extractor));
}

inline std::unique_ptr<IMatchStep> captureVarSlot(ca::str::Utf8StringRef name, int opc)
{
    return capture(name,
                   varInsn(opc),
                   [](jbytecode::tree::InsnList& list, int start, int end, const CaptureStore&)
                       -> ca::core::Any {
                       (void)end;
                       auto* node = list.get(start);
                       auto* vin  = ca::core::tag_cast::cast<jbytecode::tree::VarInsnNode>(node);
                       return vin->var;
                   });
}

inline std::unique_ptr<IMatchStep> captureVarSlot(const char* name, int opc)
{
    return captureVarSlot(ca::str::Utf8StringRef::from_cstr(name), opc);
}

inline std::unique_ptr<IMatchStep> guard(std::string                              description,
                                         std::function<bool(const CaptureStore&)> pred)
{
    return std::make_unique<GuardStep>(std::move(description), std::move(pred));
}

inline std::unique_ptr<IMatchStep> guardEquals(ca::str::Utf8StringRef left,
                                               ca::str::Utf8StringRef right)
{
    return guard(left.to_std_string() + "==" + right.to_std_string(),
                 [left, right](const CaptureStore& captures) {
        if (!captures.contains(left) || !captures.contains(right)) return false;
        return detail::any_equals(captures.get(left), captures.get(right));
    });
}

inline std::unique_ptr<IMatchStep> guardEquals(const char* left, const char* right)
{
    return guardEquals(ca::str::Utf8StringRef::from_cstr(left),
                       ca::str::Utf8StringRef::from_cstr(right));
}

}   //namespace combinators

}   //namespace mjt::tmatcher
