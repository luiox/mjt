#pragma once

#include "entries.hpp"

#include <vector>

namespace jblw::constantpool {

///@brief 原始常量池槽视图。
///
///索引从零开始，对应 class 文件常量池索引减一；Long/Double 后的保留槽返回空指针。
class ConstantPool
{
public:
    virtual ~ConstantPool()                               = default;
    virtual const EntryPtr&      get(ca::i32 index) const = 0;
    virtual std::vector<ca::i32> indices() const          = 0;
    virtual ca::usize            size() const noexcept    = 0;
};
using ConstantPoolPtr = std::shared_ptr<const ConstantPool>;

class ListConstantPool final : public ConstantPool
{
public:
    ///@throws std::invalid_argument 槽布局不符合 JVM 双槽规则时抛出。
    explicit ListConstantPool(std::vector<EntryPtr> entries);

    const EntryPtr&              get(ca::i32 index) const override;
    std::vector<ca::i32>         indices() const override;
    ca::usize                    size() const noexcept override { return entries_.size(); }
    const std::vector<EntryPtr>& entries() const noexcept { return entries_; }

private:
    std::vector<EntryPtr> entries_;
};

}   //namespace jblw::constantpool
