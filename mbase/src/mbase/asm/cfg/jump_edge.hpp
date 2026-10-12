#pragma once

#include <cstdint>
#include <map>
#include <vector>

namespace mjt::base::ast::cfg {

class BasicBlock;

///跳转类型枚举
enum class JumpType
{
    FallThrough,     //顺序执行（无跳转或末尾不是分支指令）
    Unconditional,   //goto
    Condition,       //条件跳转（ifXX）
    TableSwitch,     //tableswitch
    LookupSwitch     //lookupswitch
};

//── 跳转边基类 ──

class JumpEdge
{
public:
    JumpEdge(BasicBlock* source, JumpType type)
        : source_(source)
        , type_(type)
    {}
    virtual ~JumpEdge() = default;

    BasicBlock* source() const { return source_; }
    JumpType    type() const { return type_; }

    bool isFallThrough() const { return type_ == JumpType::FallThrough; }
    bool isUnconditional() const { return type_ == JumpType::Unconditional; }
    bool isCondition() const { return type_ == JumpType::Condition; }
    bool isTableSwitch() const { return type_ == JumpType::TableSwitch; }
    bool isLookupSwitch() const { return type_ == JumpType::LookupSwitch; }

private:
    BasicBlock* source_;
    JumpType    type_;
};

//── 具体跳转边类型 ──

///无条件跳转（goto）
class UnconditionJumpEdge : public JumpEdge
{
public:
    UnconditionJumpEdge(BasicBlock* source, BasicBlock* target)
        : JumpEdge(source, JumpType::Unconditional)
        , target_(target)
    {}
    BasicBlock* target() const { return target_; }

private:
    BasicBlock* target_;
};

///条件跳转（ifXX）：true 分支跳转，false 分支 fallthrough
class ConditionJumpEdge : public JumpEdge
{
public:
    ConditionJumpEdge(BasicBlock* source, int32_t jumpOpcode, BasicBlock* trueTarget,
                      BasicBlock* falseTarget)
        : JumpEdge(source, JumpType::Condition)
        , jumpOpcode_(jumpOpcode)
        , trueTarget_(trueTarget)
        , falseTarget_(falseTarget)
    {}
    int32_t     jumpOpcode() const { return jumpOpcode_; }
    BasicBlock* trueTarget() const { return trueTarget_; }
    BasicBlock* falseTarget() const { return falseTarget_; }

private:
    int32_t     jumpOpcode_;
    BasicBlock* trueTarget_;
    BasicBlock* falseTarget_;
};

///顺序执行（fallthrough）
class FallThroughEdge : public JumpEdge
{
public:
    FallThroughEdge(BasicBlock* source, BasicBlock* next)
        : JumpEdge(source, JumpType::FallThrough)
        , next_(next)
    {}
    BasicBlock* next() const { return next_; }

private:
    BasicBlock* next_;
};

///tableswitch
class TableSwitchJumpEdge : public JumpEdge
{
public:
    TableSwitchJumpEdge(BasicBlock* source, int32_t min, int32_t max,
                        std::vector<BasicBlock*> targets, BasicBlock* defaultTarget)
        : JumpEdge(source, JumpType::TableSwitch)
        , min_(min)
        , max_(max)
        , targets_(std::move(targets))
        , defaultTarget_(defaultTarget)
    {}
    int32_t                         min() const { return min_; }
    int32_t                         max() const { return max_; }
    const std::vector<BasicBlock*>& targets() const { return targets_; }
    BasicBlock*                     defaultTarget() const { return defaultTarget_; }

private:
    int32_t                  min_, max_;
    std::vector<BasicBlock*> targets_;
    BasicBlock*              defaultTarget_;
};

///lookupswitch
class LookupSwitchJumpEdge : public JumpEdge
{
public:
    LookupSwitchJumpEdge(BasicBlock* source, std::map<int32_t, BasicBlock*> targets,
                         BasicBlock* defaultTarget)
        : JumpEdge(source, JumpType::LookupSwitch)
        , targets_(std::move(targets))
        , defaultTarget_(defaultTarget)
    {}
    const std::map<int32_t, BasicBlock*>& targets() const { return targets_; }
    BasicBlock*                           defaultTarget() const { return defaultTarget_; }

private:
    std::map<int32_t, BasicBlock*> targets_;
    BasicBlock*                    defaultTarget_;
};

}   //namespace mjt::base::ast::cfg
