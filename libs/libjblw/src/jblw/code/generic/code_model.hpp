#pragma once

#include "jblw/code/attribute/local.hpp"
#include "jblw/code/instructions.hpp"

#include <initializer_list>

namespace jblw::code {

///@brief 自动维护所有 label index 的 code 元素序列。
///
///通过本类型的修改 API 插入、替换或删除元素时，受影响 label 会立即重编号；被移除
///的 label 会恢复为 `Label::UNSET`。
class CodeElementList final
{
public:
    CodeElementList() = default;
    explicit CodeElementList(std::vector<CodeElementPtr> elements);
    CodeElementList(std::initializer_list<CodeElementPtr> elements);

    ca::usize             size() const noexcept { return elements_.size(); }
    bool                  empty() const noexcept { return elements_.empty(); }
    const CodeElementPtr& at(ca::usize index) const { return elements_.at(index); }
    const CodeElementPtr& operator[](ca::usize index) const noexcept { return elements_[index]; }
    const std::vector<CodeElementPtr>& values() const noexcept { return elements_; }

    auto begin() const noexcept { return elements_.begin(); }
    auto end() const noexcept { return elements_.end(); }

    void           add(CodeElementPtr element);
    void           insert(ca::usize index, CodeElementPtr element);
    void           addAll(std::vector<CodeElementPtr> elements);
    CodeElementPtr set(ca::usize index, CodeElementPtr element);
    CodeElementPtr remove(ca::usize index);
    void           clear() noexcept;
    ca::i32        indexOf(const CodeElement* element) const noexcept;

private:
    void validate(const CodeElementPtr& element) const;
    void indexFrom(ca::usize index) noexcept;

    std::vector<CodeElementPtr> elements_;
};

///@brief BLW 默认 label 实现；相等性由 line/index 两个可变字段决定。
class GenericLabel final : public Label
{
public:
    ca::i32 getIndex() const noexcept override { return index_; }
    void    setIndex(ca::i32 index) noexcept override { index_ = index; }
    ca::i32 getLineNumber() const noexcept override { return lineNumber_; }
    void    setLineNumber(ca::i32 lineNumber) noexcept override { lineNumber_ = lineNumber; }
    bool    equals(const CodeElement& other) const noexcept override;

private:
    ca::i32 lineNumber_ = UNSET;
    ca::i32 index_      = UNSET;
};

///@brief GenericCode 的 owning 实现。
class GenericCode final : public Code
{
public:
    GenericCode(ca::i32 maxStack, ca::i32 maxLocals, CodeElementList elements,
                std::vector<TryCatchBlock>       tryCatchBlocks,
                std::vector<attribute::LocalPtr> localVariables);

    ca::i32                           maxStack() const noexcept override { return maxStack_; }
    ca::i32                           maxLocals() const noexcept override { return maxLocals_; }
    std::unique_ptr<CodeWalker>       walker() override;
    CodeElementList&                  elements() noexcept override { return elements_; }
    const CodeElementList&            elements() const noexcept override { return elements_; }
    const std::vector<TryCatchBlock>& tryCatchBlocks() const noexcept override
    {
        return tryCatchBlocks_;
    }
    const std::vector<attribute::LocalPtr>& localVariables() const noexcept override
    {
        return localVariables_;
    }

private:
    ca::i32                          maxStack_;
    ca::i32                          maxLocals_;
    CodeElementList                  elements_;
    std::vector<TryCatchBlock>       tryCatchBlocks_;
    std::vector<attribute::LocalPtr> localVariables_;
};

///@brief code 元素列表的可复用 builder。
class GenericCodeListBuilder final
{
public:
    GenericCodeListBuilder& addInstruction(InstructionPtr instruction);
    GenericCodeListBuilder& addInstruction(ca::usize index, InstructionPtr instruction);
    GenericCodeListBuilder& addLabel(LabelPtr label);
    GenericCodeListBuilder& addLabel(ca::usize index, LabelPtr label);
    GenericCodeListBuilder& setElements(std::vector<CodeElementPtr> elements);

    CodeElement* firstElement() const noexcept;
    CodeElement* lastElement() const noexcept;
    Label*       firstLabel() const noexcept;
    Label*       lastLabel() const noexcept;
    ca::i32      indexOf(const CodeElement* element) const noexcept
    {
        return elements_.indexOf(element);
    }
    const CodeElementList& elements() const noexcept { return elements_; }
    CodeElementList        build() const { return CodeElementList(elements_.values()); }

private:
    CodeElementList elements_;
};

///@brief GenericCode 的 fluent builder。
class GenericCodeBuilder final
{
public:
    ca::i32                 maxStack() const noexcept { return maxStack_; }
    GenericCodeBuilder&     maxStack(ca::i32 value) noexcept;
    ca::i32                 maxLocals() const noexcept { return maxLocals_; }
    GenericCodeBuilder&     maxLocals(ca::i32 value) noexcept;
    GenericCodeBuilder&     tryCatchBlocks(std::vector<TryCatchBlock> values);
    GenericCodeBuilder&     tryCatchBlock(TryCatchBlock value);
    GenericCodeBuilder&     localVariables(std::vector<attribute::LocalPtr> values);
    GenericCodeBuilder&     localVariable(attribute::LocalPtr value);
    GenericCodeListBuilder& codeList() noexcept { return content_; }
    CodePtr                 build() const;

private:
    ca::i32                          maxStack_  = 0;
    ca::i32                          maxLocals_ = 0;
    std::vector<TryCatchBlock>       tryCatchBlocks_;
    GenericCodeListBuilder           content_;
    std::vector<attribute::LocalPtr> localVariables_;
};

}   //namespace jblw::code

///@brief 与上游 `dev.xdark.blw.code.generic` 对齐的命名空间视图。
namespace jblw::code::generic {
using ::jblw::code::CodeElementList;
using ::jblw::code::GenericCode;
using ::jblw::code::GenericCodeBuilder;
using ::jblw::code::GenericCodeListBuilder;
using ::jblw::code::GenericLabel;
}   //namespace jblw::code::generic
