#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "jbytecode/tree/insn_list.hpp"

#include "mbase/asm/tmatcher/replace_context.hpp"

namespace mjt::tmatcher {

//============================================================================
//ReplaceStep：声明式替换的原子单元
//============================================================================

///替换 Step 抽象基类。每个 step 通过 emit 向 ReplaceContext.builder 贡献指令。
///与 ReplaceRuleHandler 的关系：ReplaceSequence（一组有序 ReplaceStep）
///可被 SequenceHandler 包装为一个 handler。
class ReplaceStep
{
public:
    virtual ~ReplaceStep() = default;

    ///在 ReplaceContext 上执行本 step 的替换贡献。
    virtual void emit(ReplaceContext& ctx) = 0;

    ///调试标识。
    virtual std::string name() const = 0;
};

///追加一条指定指令节点（builder 接管所有权）。
class EmitInsnStep : public ReplaceStep
{
    std::function<jbytecode::tree::AbstractInsnNode*(ReplaceContext&)> factory_;
    std::string                                                         name_;

public:
    ///factory 返回一个 new 出来的节点，所有权移交 builder。
    explicit EmitInsnStep(std::function<jbytecode::tree::AbstractInsnNode*(ReplaceContext&)> factory,
                          std::string name = "emit")
        : factory_(std::move(factory))
        , name_(std::move(name))
    {}

    void emit(ReplaceContext& ctx) override
    {
        auto* insn = factory_(ctx);
        if (insn) ctx.replace(insn);
    }

    std::string name() const override { return name_; }
};

///克隆命中区间内第 idx 条原始指令追加到 builder（0-based）。
class CloneOriginStep : public ReplaceStep
{
    int idx_;

public:
    explicit CloneOriginStep(int idx)
        : idx_(idx)
    {}

    void emit(ReplaceContext& ctx) override { ctx.origin(idx_); }

    std::string name() const override { return "cloneOrigin(" + std::to_string(idx_) + ")"; }
};

///克隆命中区间 [startIdx, endIdx) 内的原始指令追加到 builder。
class CloneOriginRangeStep : public ReplaceStep
{
    int startIdx_;
    int endIdx_;

public:
    CloneOriginRangeStep(int startIdx, int endIdx)
        : startIdx_(startIdx)
        , endIdx_(endIdx)
    {}

    void emit(ReplaceContext& ctx) override { ctx.originRange(startIdx_, endIdx_); }

    std::string name() const override
    {
        return "cloneOriginRange(" + std::to_string(startIdx_) + "," +
               std::to_string(endIdx_) + ")";
    }
};

///删除命中指令、不追加任何替换（等价 replaceWithPop）。
class RemoveStep : public ReplaceStep
{
public:
    void emit(ReplaceContext& ctx) override { ctx.replaceWithPop(); }

    std::string name() const override { return "remove"; }
};

//============================================================================
//ReplaceSequence：有序 ReplaceStep 列表
//============================================================================

///替换序列：按序执行一组 ReplaceStep，共同构成一次替换的输出。
class ReplaceSequence
{
    std::vector<std::unique_ptr<ReplaceStep>> steps_;

public:
    ReplaceSequence() = default;

    void add(std::unique_ptr<ReplaceStep> step) { steps_.push_back(std::move(step)); }

    ///依次执行所有 step。
    void emit(ReplaceContext& ctx)
    {
        for (auto& s : steps_) s->emit(ctx);
    }

    bool empty() const { return steps_.empty(); }
    size_t size() const { return steps_.size(); }
};

}   //namespace mjt::tmatcher
