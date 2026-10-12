#include "code_model.hpp"

#include <algorithm>
#include <iterator>
#include <stdexcept>

namespace jblw::code {

CodeElementList::CodeElementList(std::vector<CodeElementPtr> elements)
    : elements_(std::move(elements))
{
    for (const auto& element : elements_) validate(element);
    indexFrom(0);
}

CodeElementList::CodeElementList(std::initializer_list<CodeElementPtr> elements)
    : CodeElementList(std::vector<CodeElementPtr>(elements))
{}

void CodeElementList::validate(const CodeElementPtr& element) const
{
    if (!element) throw std::invalid_argument("Code element must not be null");
}

void CodeElementList::indexFrom(ca::usize index) noexcept
{
    for (; index < elements_.size(); ++index) {
        auto& element = elements_[index];
        if (element->getType() == CodeElementKind::LABEL) {
            static_cast<Label&>(*element).setIndex(static_cast<ca::i32>(index));
        }
    }
}

void CodeElementList::add(CodeElementPtr element)
{
    validate(element);
    if (element->getType() == CodeElementKind::LABEL) {
        static_cast<Label&>(*element).setIndex(static_cast<ca::i32>(elements_.size()));
    }
    elements_.push_back(std::move(element));
}

void CodeElementList::insert(ca::usize index, CodeElementPtr element)
{
    validate(element);
    if (index > elements_.size()) throw std::out_of_range("Code element insertion index");
    elements_.insert(elements_.begin() + static_cast<std::ptrdiff_t>(index), std::move(element));
    indexFrom(index);
}

void CodeElementList::addAll(std::vector<CodeElementPtr> elements)
{
    for (const auto& element : elements) validate(element);
    const ca::usize first = elements_.size();
    elements_.insert(elements_.end(),
                     std::make_move_iterator(elements.begin()),
                     std::make_move_iterator(elements.end()));
    indexFrom(first);
}

CodeElementPtr CodeElementList::set(ca::usize index, CodeElementPtr element)
{
    validate(element);
    CodeElementPtr previous = elements_.at(index);
    if (previous->getType() == CodeElementKind::LABEL) {
        static_cast<Label&>(*previous).setIndex(Label::UNSET);
    }
    elements_[index] = std::move(element);
    indexFrom(index);
    return previous;
}

CodeElementPtr CodeElementList::remove(ca::usize index)
{
    if (index >= elements_.size()) throw std::out_of_range("Code element removal index");
    CodeElementPtr previous = elements_[index];
    if (previous->getType() == CodeElementKind::LABEL) {
        static_cast<Label&>(*previous).setIndex(Label::UNSET);
    }
    elements_.erase(elements_.begin() + static_cast<std::ptrdiff_t>(index));
    indexFrom(index);
    return previous;
}

void CodeElementList::clear() noexcept
{
    for (auto& element : elements_) {
        if (element->getType() == CodeElementKind::LABEL) {
            static_cast<Label&>(*element).setIndex(Label::UNSET);
        }
    }
    elements_.clear();
}

ca::i32 CodeElementList::indexOf(const CodeElement* element) const noexcept
{
    for (ca::usize i = 0; i < elements_.size(); ++i) {
        if (elements_[i].get() == element) return static_cast<ca::i32>(i);
    }
    return -1;
}

bool GenericLabel::equals(const CodeElement& other) const noexcept
{
    if (other.getType() != CodeElementKind::LABEL) return false;
    const auto& label = static_cast<const Label&>(other);
    return lineNumber_ == label.getLineNumber() && index_ == label.getIndex();
}

namespace {
class GenericCodeWalker final : public CodeWalker
{
public:
    explicit GenericCodeWalker(CodeElementList& elements)
        : elements_(elements)
    {}

    ca::i32      index() const noexcept override { return index_; }
    CodeElement* element() const noexcept override { return element_; }

    void advance() noexcept override
    {
        const ca::i32 next = index_ + 1;
        if (next < 0 || static_cast<ca::usize>(next) >= elements_.size()) {
            element_ = nullptr;
            return;
        }
        index_   = next;
        element_ = elements_[static_cast<ca::usize>(next)].get();
    }

    void set(const Label& label) override
    {
        if (label.getIndex() == Label::UNSET) {
            throw std::logic_error("Label index is not set");
        }
        index_   = label.getIndex() - 1;
        element_ = nullptr;
    }

private:
    CodeElementList& elements_;
    CodeElement*     element_ = nullptr;
    ca::i32          index_   = -1;
};
}   //namespace

GenericCode::GenericCode(ca::i32 maxStack, ca::i32 maxLocals, CodeElementList elements,
                         std::vector<TryCatchBlock>       tryCatchBlocks,
                         std::vector<attribute::LocalPtr> localVariables)
    : maxStack_(maxStack)
    , maxLocals_(maxLocals)
    , elements_(std::move(elements))
    , tryCatchBlocks_(std::move(tryCatchBlocks))
    , localVariables_(std::move(localVariables))
{}

std::unique_ptr<CodeWalker> GenericCode::walker()
{
    return std::make_unique<GenericCodeWalker>(elements_);
}

GenericCodeListBuilder& GenericCodeListBuilder::addInstruction(InstructionPtr instruction)
{
    elements_.add(std::move(instruction));
    return *this;
}

GenericCodeListBuilder& GenericCodeListBuilder::addInstruction(ca::usize      index,
                                                               InstructionPtr instruction)
{
    elements_.insert(index, std::move(instruction));
    return *this;
}

GenericCodeListBuilder& GenericCodeListBuilder::addLabel(LabelPtr label)
{
    elements_.add(std::move(label));
    return *this;
}

GenericCodeListBuilder& GenericCodeListBuilder::addLabel(ca::usize index, LabelPtr label)
{
    elements_.insert(index, std::move(label));
    return *this;
}

GenericCodeListBuilder& GenericCodeListBuilder::setElements(std::vector<CodeElementPtr> elements)
{
    elements_ = CodeElementList(std::move(elements));
    return *this;
}

CodeElement* GenericCodeListBuilder::firstElement() const noexcept
{
    return elements_.empty() ? nullptr : elements_[0].get();
}

CodeElement* GenericCodeListBuilder::lastElement() const noexcept
{
    return elements_.empty() ? nullptr : elements_[elements_.size() - 1].get();
}

Label* GenericCodeListBuilder::firstLabel() const noexcept
{
    for (const auto& element : elements_) {
        if (element->getType() == CodeElementKind::LABEL) return static_cast<Label*>(element.get());
    }
    return nullptr;
}

Label* GenericCodeListBuilder::lastLabel() const noexcept
{
    for (auto it = elements_.values().rbegin(); it != elements_.values().rend(); ++it) {
        if ((*it)->getType() == CodeElementKind::LABEL) return static_cast<Label*>(it->get());
    }
    return nullptr;
}

GenericCodeBuilder& GenericCodeBuilder::maxStack(ca::i32 value) noexcept
{
    maxStack_ = value;
    return *this;
}

GenericCodeBuilder& GenericCodeBuilder::maxLocals(ca::i32 value) noexcept
{
    maxLocals_ = value;
    return *this;
}

GenericCodeBuilder& GenericCodeBuilder::tryCatchBlocks(std::vector<TryCatchBlock> values)
{
    tryCatchBlocks_ = std::move(values);
    return *this;
}

GenericCodeBuilder& GenericCodeBuilder::tryCatchBlock(TryCatchBlock value)
{
    tryCatchBlocks_.push_back(std::move(value));
    return *this;
}

GenericCodeBuilder& GenericCodeBuilder::localVariables(std::vector<attribute::LocalPtr> values)
{
    localVariables_ = std::move(values);
    return *this;
}

GenericCodeBuilder& GenericCodeBuilder::localVariable(attribute::LocalPtr value)
{
    if (!value) throw std::invalid_argument("Local variable must not be null");
    localVariables_.push_back(std::move(value));
    return *this;
}

CodePtr GenericCodeBuilder::build() const
{
    return std::make_shared<GenericCode>(
        maxStack_, maxLocals_, content_.build(), tryCatchBlocks_, localVariables_);
}

}   //namespace jblw::code
