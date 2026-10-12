#pragma once

#include "jbytecode/tree/insn_list.hpp"
#include <string>
#include <vector>
#include <functional>
#include <unordered_map>

namespace mjt::base {

class MatchContext;

///匹配步骤接口
class IMatchStep
{
public:
    virtual ~IMatchStep()                                                            = default;
    virtual bool tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx) = 0;
    virtual int  getMatchedSize() { return 1; }
};

///单指令匹配 — 按 opcode + 可选 predicate
class SimpleStep : public IMatchStep
{
public:
    explicit SimpleStep(int opcode)
        : opcode_(opcode)
    {}
    SimpleStep(int opcode, std::function<bool(jbytecode::tree::AbstractInsnNode*)> predicate)
        : opcode_(opcode)
        , predicate_(std::move(predicate))
    {}
    bool tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx) override;

private:
    int                                                     opcode_;
    std::function<bool(jbytecode::tree::AbstractInsnNode*)> predicate_;
};

///匹配任意指令（满足 predicate）
class AnyStep : public IMatchStep
{
public:
    explicit AnyStep(std::function<bool(jbytecode::tree::AbstractInsnNode*)> predicate)
        : predicate_(std::move(predicate))
    {}
    bool tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx) override;

private:
    std::function<bool(jbytecode::tree::AbstractInsnNode*)> predicate_;
};

///匹配任意数量指令（贪婪）
class AnyMultiStep : public IMatchStep
{
public:
    AnyMultiStep() = default;
    bool tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx) override;
    int  getMatchedSize() override { return matchedSize_; }

private:
    int matchedSize_ = 0;
};

///AND — 多个 step 必须同时匹配同一条指令
class AndStep : public IMatchStep
{
public:
    explicit AndStep(std::vector<IMatchStep*> steps)
        : steps_(std::move(steps))
    {}
    bool tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx) override;

private:
    std::vector<IMatchStep*> steps_;
};

///OR — 第一个匹配的 step 胜出
class OrStep : public IMatchStep
{
public:
    explicit OrStep(std::vector<IMatchStep*> steps)
        : steps_(std::move(steps))
    {}
    bool tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx) override;

private:
    std::vector<IMatchStep*> steps_;
};

///NOT — 反转匹配结果
class NotStep : public IMatchStep
{
public:
    explicit NotStep(IMatchStep* step)
        : step_(step)
    {}
    bool tryMatch(jbytecode::tree::AbstractInsnNode* now, MatchContext& ctx) override;

private:
    IMatchStep* step_;
};

}   //namespace mjt::base
