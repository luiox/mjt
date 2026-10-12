#include "mbase/asm/matcher/match_step.hpp"
#include "mbase/asm/matcher/pattern_matcher.hpp"

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string_arena.hpp"

#include <cassert>

namespace mjt::base {

//========== MatchContext ==========

ca::core::Any MatchContext::internString(const char* data, std::size_t byteLength) const
{
    //字符串线示范(总方案 §5.2):arena 由 PatternMatcher::apply 必给,纯词法层的
    //std::string 装箱回退已删除 —— 产物代码 Any(std::string) 归零。
    assert(arena && "MatchContext::internString requires arena injected by PatternMatcher::apply");
    const auto* bytes = reinterpret_cast<const ca::u8*>(data);
    auto        ref   = arena->intern(bytes, byteLength);
    //intern 校验失败（非法 UTF-8）但输入非空时按原始字节入池，与 SymbolTable::addConstantString
    //保持一致，避免丢失字节流载体。
    if (!ref.data() && data && byteLength != 0) ref = arena->intern_raw(bytes, byteLength);
    return ca::core::Any(ref);
}

//========== SimpleStep ==========

bool SimpleStep::tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx)
{
    if (!now) return false;
    if (now->getOpcode() != opcode_) {
        return false;
    }
    if (predicate_ && !predicate_(now)) return false;
    return true;
}

//========== AnyStep ==========

bool AnyStep::tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx)
{
    return now && predicate_(now);
}

//========== AnyMultiStep ==========

bool AnyMultiStep::tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx)
{
    //贪婪匹配所有满足条件的连续指令
    matchedSize_ = 0;
    auto* n      = now;
    while (n) {
        //跳过 LabelNode / LineNumberNode / FrameNode 不计数
        int type = n->getType();
        if (type == jbytecode::tree::AbstractInsnNode::LABEL ||
            type == jbytecode::tree::AbstractInsnNode::LINE ||
            type == jbytecode::tree::AbstractInsnNode::FRAME) {
            n = n->getNext();
            continue;
        }
        matchedSize_++;
        n = n->getNext();
    }
    return matchedSize_ > 0;
}

//========== AndStep ==========

bool AndStep::tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx)
{
    for (auto* step : steps_) {
        if (!step->tryMatch(now, ctx)) return false;
    }
    return true;
}

//========== OrStep ==========

bool OrStep::tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx)
{
    for (auto* step : steps_) {
        if (step->tryMatch(now, ctx)) return true;
    }
    return false;
}

//========== NotStep ==========

bool NotStep::tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx)
{
    return !step_->tryMatch(now, ctx);
}

//========== MatchRule ==========

bool MatchRule::tryMatch(jbytecode::tree::InsnList& insnList, int startIndex, MatchContext& ctx)
{
    auto* node = insnList.get(startIndex);
    if (!node) return false;

    ctx.matchedNodes.clear();
    ctx.bindings.clear();

    int offset = 0;
    for (size_t i = 0; i < steps_.size(); i++) {
        auto* n = insnList.get(startIndex + offset);
        if (!n) return false;

        //Label、Line 和 Frame 是 CFG/调试元数据边界。指令替换规则不能跨越它们，
        //否则 apply 会删除边界节点并破坏跳转、异常表或 stack map 的所有权。
        if (!n || n->getType() == jbytecode::tree::AbstractInsnNode::LABEL ||
            n->getType() == jbytecode::tree::AbstractInsnNode::LINE ||
            n->getType() == jbytecode::tree::AbstractInsnNode::FRAME) {
            return false;
        }

        if (!steps_[i]->tryMatch(n, ctx)) return false;
        ctx.matchedNodes.push_back(n);
        offset += steps_[i]->getMatchedSize();
    }

    ctx.startIndex = startIndex;
    ctx.endIndex   = startIndex + offset;
    return true;
}

//========== PatternMatcher ==========

std::unordered_map<int, MatchResult> PatternMatcher::match(jbytecode::tree::InsnList& insnList)
{
    std::unordered_map<int, MatchResult> results;
    for (size_t ri = 0; ri < rules_.size(); ri++) {
        MatchContext ctx;
        ctx.original = &insnList;

        for (int i = 0; i < insnList.size(); i++) {
            if (rules_[ri]->tryMatch(insnList, i, ctx)) {
                results[static_cast<int>(ri)] = {ctx.startIndex, ctx.endIndex};
                break;
            }
        }
    }
    return results;
}

bool PatternMatcher::apply(jbytecode::tree::InsnList& insnList, ca::str::Utf8StringArena& arena)
{
    //字符串线示范:apply 必给 arena,原无 arena 重载删除,见 pattern_matcher.hpp。
    return applyInternal(insnList, arena);
}

bool PatternMatcher::applyInternal(jbytecode::tree::InsnList& insnList,
                                   ca::str::Utf8StringArena& arena)
{
    bool anyChanged = false;
    for (auto& rule : rules_) {
        MatchContext ctx;
        ctx.original  = &insnList;
        ctx.arena     = &arena;
        //B8 clone 协议：克隆节点落目标方法内容 arena（apply 的调用方传入的都是
        //目标方法的 instructions，owner arena 即所在 pass 的 MethodNode::arena）。
        ctx.nodeArena = insnList.ownerArena();
        jbytecode::tree::InsnList replacement;
        ctx.builder = &replacement;

        //从后往前匹配，避免索引偏移
        for (int i = insnList.size() - 1; i >= 0; i--) {
            bool matched = rule->tryMatch(insnList, i, ctx);
            if (matched) {
                if (!rule->getStrategy()) {
                    break;
                }

                ctx.resetReplacementDecision();
                rule->getStrategy()->apply(ctx);
                if (ctx.isReplacementCancelled()) {
                    replacement.clear();
                    continue;
                }

                anyChanged = true;
                //删除匹配区间（释放旧节点）
                for (int j = ctx.startIndex; j < ctx.endIndex; j++) {
                    auto* old = insnList.get(ctx.startIndex);
                    if (old) {
                        insnList.removeAndDelete(old);
                    }
                }
                //插入新指令
                auto* insertBefore = insnList.get(ctx.startIndex);
                auto* rNode        = replacement.getFirst();
                while (rNode) {
                    auto* next = rNode->getNext();
                    replacement.remove(rNode);
                    if (insertBefore)
                        insnList.insertBefore(insertBefore, rNode);
                    else
                        insnList.add(rNode);
                    rNode = next;
                }
                break;
            }
        }
    }
    return anyChanged;
}

}   //namespace mjt::base
