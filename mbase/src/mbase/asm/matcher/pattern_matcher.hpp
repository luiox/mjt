#pragma once

#include "jbytecode/tree/insn_list.hpp"
#include "jbytecode/core/primitives.hpp"
#include "garena/arena.hpp"
#include "libca/core/any.hpp"
#include "libca/core/tag_cast.hpp"
#include <cassert>
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstddef>

namespace ca::str {
class Utf8StringArena;
}

namespace mjt::base {

class IMatchStep;

///@brief 传给替换策略的一次匹配上下文。
///
///`matchedNodes` 中的指针是对原 instruction list 的借用，只能读取。策略把新节点写入
///`builder` 后，`PatternMatcher::apply` 会移除并释放原匹配范围；需要保留的指令必须先
///通过 `cloneMatchedNode` 克隆，不能把原指针再次插入。
///
///@par 生命周期与所有权
///上下文只在一次 `apply` 回调期间有效；`original`、`builder`、`arena` 和其中节点均不由上下文拥有。
///策略不得缓存指针、跨回调复用 `bindings` 或在回调后访问 `matchedNodes`。
class MatchContext
{
public:
    jbytecode::tree::InsnList*                      original   = nullptr;
    int                                             startIndex = -1;
    int                                             endIndex   = -1;
    jbytecode::tree::InsnList*                      builder    = nullptr;
    std::unordered_map<std::string, int>            bindings;
    std::vector<jbytecode::tree::AbstractInsnNode*> matchedNodes;

    ///作业级字符串池（借用，非拥有；见 与总方案 §5.2）。由 `PatternMatcher::apply`
    ///注入（apply 必给 arena，纯词法层的 std::string 回退已随字符串线删除）。arena 贯穿
    ///整个作业、晚于 ClassWriter 序列化释放，故经它 intern 的 `Utf8StringRef` 可安全
    ///存入 `LdcInsnNode` 并跨越序列化。
    ca::str::Utf8StringArena* arena = nullptr;

    ///克隆节点的内容 arena（B8 clone 协议）：`cloneMatchedNode` 产物与携带 Label 副本的
    ///分配归宿 = 所在 pass 的 MethodNode 的 arena。由 `PatternMatcher::apply` 注入
    ///（applyInternal 经 `insnList.ownerArena` 传递——apply 的调用方传入的都是目标
    ///方法的 instructions）；直构 MatchContext 的场景缺省时由 `cloneMatchedNode` 回退
    ///读取 `original->ownerArena`。
    garena::Arena* nodeArena = nullptr;

    ///@brief 用 arena 托管折叠出的字符串常量生命周期，返回适合存入 `LdcInsnNode` 的 `Any`。
    ///
    ///把字节 intern 进池（复刻 `SymbolTable::addConstantString` 的
    ///`intern → intern_raw` 回退：合法 UTF-8 走 intern，非法字节走 intern_raw），返回指向池内
    ///副本的 `Utf8StringRef`。前提：`arena` 已由 `PatternMatcher::apply` 注入（debug 断言）；
    ///字符串线示范后产物代码不再存在 `Any(std::string)` 装箱路径。
    ca::core::Any internString(const char* data, std::size_t byteLength) const;
    ca::core::Any internString(const std::string& s) const
    {
        return internString(s.data(), s.size());
    }

    ///放弃当前匹配的替换。策略已写入 builder 的候选节点会被释放，原指令保持不变。
    void cancelReplacement() { replacementCancelled_ = true; }

    ///查询策略是否已放弃当前匹配。
    bool isReplacementCancelled() const { return replacementCancelled_; }

    ///每次执行策略前重置替换决策，仅供 PatternMatcher 调用。
    void resetReplacementDecision() { replacementCancelled_ = false; }

private:
    bool replacementCancelled_ = false;
};

///@brief 克隆匹配指令，供替换列表使用（B8 新 clone 协议）。
///
///原匹配节点属于 `ctx.original`，会在策略返回后释放；此函数返回的新节点由调用方的
///replacement list 接管（最终移回 `ctx.original`，故克隆体落目标方法 arena）。超出
///`matchedNodes` 范围时抛出 `std::out_of_range`，调用方不能把异常转换成半完成的替换。
///
///Label 语义：克隆发生在同一 instruction list 内，跳转/switch 目标 Label 仍保留在
///原列表中。新协议下同 arena Label 经 resolveCloneLabel O(1) 直返（同域不换身份），
///旧「identity labelMap（跳转目标映射到自身）」语义由此天然满足。
inline jbytecode::tree::AbstractInsnNode* cloneMatchedNode(const MatchContext& ctx,
                                                           std::size_t         index)
{
    jbytecode::tree::AbstractInsnNode* node = ctx.matchedNodes.at(index);
    garena::Arena*                     arena = ctx.nodeArena;
    if (!arena && ctx.original) arena = ctx.original->ownerArena();
    assert(arena != nullptr &&
           "cloneMatchedNode: target method arena unavailable (original list unbound?)");
    std::unordered_map<jbytecode::core::Label*, jbytecode::core::Label*> labelMap;
    return node->clone(*arena, labelMap);
}

struct MatchResult
{
    int startIndex = -1;
    int endIndex   = -1;
};

///修改策略接口：只读 matchedNodes，只向 builder 写入新分配或 clone 的节点。
class IModifyStrategy
{
public:
    virtual ~IModifyStrategy()            = default;
    virtual void apply(MatchContext& ctx) = 0;
};

class LambdaStrategy : public IModifyStrategy
{
public:
    explicit LambdaStrategy(std::function<void(MatchContext&)> fn)
        : fn_(std::move(fn))
    {}
    void apply(MatchContext& ctx) override { fn_(ctx); }

private:
    std::function<void(MatchContext&)> fn_;
};

///@brief 有序匹配 step 和可选替换策略。
///
///`steps_` 的 step 指针由规则创建方负责生命周期管理；策略由 `unique_ptr` 独占。
///规则尝试匹配失败时不得修改 instruction list。
class MatchRule
{
public:
    MatchRule() = default;
    MatchRule(std::vector<IMatchStep*> steps, std::unique_ptr<IModifyStrategy> strategy = nullptr)
        : steps_(std::move(steps))
        , strategy_(std::move(strategy))
    {}

    MatchRule(MatchRule&&)            = default;
    MatchRule& operator=(MatchRule&&) = default;

    bool tryMatch(jbytecode::tree::InsnList& insnList, int startIndex, MatchContext& ctx);

    void             setStrategy(std::unique_ptr<IModifyStrategy> s) { strategy_ = std::move(s); }
    IModifyStrategy* getStrategy() const { return strategy_.get(); }

private:
    std::vector<IMatchStep*>         steps_;
    std::unique_ptr<IModifyStrategy> strategy_;
};

///@brief 运行规则匹配和原子替换的模式匹配器。
///
///匹配阶段只借用节点，且不会跨越 Label、Line 或 Frame 边界；应用阶段按规则删除原范围、再把
///builder 节点移入原列表。`apply` 失败时保持该次候选不变，调用方仍须用 roundtrip/verifier
///测试证明规则语义。
class PatternMatcher
{
public:
    void addRule(std::unique_ptr<MatchRule> rule) { rules_.push_back(std::move(rule)); }

    std::unordered_map<int, MatchResult> match(jbytecode::tree::InsnList& insnList);

    ///@brief 执行规则匹配与原子替换(唯一入口,字符串线示范 )。
    ///
    ///`arena` 必给:注入每次匹配的 `MatchContext`,使策略经 `MatchContext::internString`
    ///分配生命周期安全的 `Utf8StringRef`(见 与总方案 §5.2)。原"无 arena"重载
    ///已删除——其 std::string 装箱回退是全仓最后一处产物 `Any(std::string)`,随字符串线
    ///归零;调用方(各 pass)经 `MethodAnalysisManager::arena` 取作业级池传入。
    bool apply(jbytecode::tree::InsnList& insnList, ca::str::Utf8StringArena& arena);

private:
    bool applyInternal(jbytecode::tree::InsnList& insnList, ca::str::Utf8StringArena& arena);

    std::vector<std::unique_ptr<MatchRule>> rules_;
};

}   //namespace mjt::base
