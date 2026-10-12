#include "builders.hpp"

#include <stdexcept>

namespace jblw::annotation {

namespace {
DeferredElement fixed(ElementPtr element)
{
    if (!element) throw std::invalid_argument("Annotation element cannot be null");
    return [element = std::move(element)] { return element; };
}

void validate(const DeferredElement& element)
{
    if (!element) throw std::invalid_argument("Deferred annotation element cannot be empty");
}

template<typename Entries>
void putEntry(Entries& entries, ca::str::Utf8String name, DeferredElement element)
{
    validate(element);
    for (auto& entry : entries) {
        if (entry.first.equals(name)) {
            entry.second = std::move(element);
            return;
        }
    }
    entries.emplace_back(std::move(name), std::move(element));
}

template<typename Entries>
std::vector<AnnotationEntry> materialize(const Entries& entries)
{
    std::vector<AnnotationEntry> result;
    result.reserve(entries.size());
    for (const auto& entry : entries) {
        ElementPtr value = entry.second();
        if (!value) throw std::invalid_argument("Annotation builder produced null element");
        const auto name = entry.first.ref();
        result.emplace_back(ca::str::Utf8String::from_data(name.data(), name.byte_length()),
                            std::move(value));
    }
    return result;
}
}   //namespace

GenericArrayBuilder& GenericArrayBuilder::element(ElementPtr element)
{
    return this->element(fixed(std::move(element)));
}

GenericArrayBuilder& GenericArrayBuilder::element(DeferredElement element)
{
    validate(element);
    values_.push_back(std::move(element));
    return *this;
}

GenericArrayBuilder& GenericArrayBuilder::elements(const std::vector<ElementPtr>& elements)
{
    for (const auto& elementValue : elements) element(elementValue);
    return *this;
}

std::shared_ptr<GenericAnnotationBuilder> GenericArrayBuilder::annotation(
    type::InstanceTypePtr type)
{
    auto child = std::make_shared<GenericAnnotationBuilder>(std::move(type));
    element([child] { return child->build(); });
    return child;
}

std::shared_ptr<GenericArrayBuilder> GenericArrayBuilder::array()
{
    auto child = std::make_shared<GenericArrayBuilder>();
    element([child] { return child->build(); });
    return child;
}

std::shared_ptr<const ElementArray> GenericArrayBuilder::build() const
{
    std::vector<ElementPtr> elements;
    elements.reserve(values_.size());
    for (const auto& value : values_) {
        ElementPtr elementValue = value();
        if (!elementValue) throw std::invalid_argument("Array builder produced null element");
        elements.push_back(std::move(elementValue));
    }
    return std::make_shared<const ElementArray>(std::move(elements));
}

GenericAnnotationBuilder::GenericAnnotationBuilder(type::InstanceTypePtr type)
    : type_(std::move(type))
{
    if (!type_) throw std::invalid_argument("Annotation builder type cannot be null");
}

GenericAnnotationBuilder& GenericAnnotationBuilder::element(ca::str::Utf8String name,
                                                            ElementPtr          element)
{
    return this->element(std::move(name), fixed(std::move(element)));
}

GenericAnnotationBuilder& GenericAnnotationBuilder::element(ca::str::Utf8String name,
                                                            DeferredElement     element)
{
    put(std::move(name), std::move(element));
    return *this;
}

std::shared_ptr<GenericAnnotationBuilder> GenericAnnotationBuilder::annotation(
    ca::str::Utf8String name, type::InstanceTypePtr type)
{
    auto child = std::make_shared<GenericAnnotationBuilder>(std::move(type));
    element(std::move(name), [child] { return child->build(); });
    return child;
}

std::shared_ptr<GenericArrayBuilder> GenericAnnotationBuilder::array(ca::str::Utf8String name)
{
    auto child = std::make_shared<GenericArrayBuilder>();
    element(std::move(name), [child] { return child->build(); });
    return child;
}

GenericAnnotationBuilder& GenericAnnotationBuilder::type(type::InstanceTypePtr type)
{
    if (!type) throw std::invalid_argument("Annotation builder type cannot be null");
    type_ = std::move(type);
    return *this;
}

AnnotationPtr GenericAnnotationBuilder::build() const
{
    return std::make_shared<const MapAnnotation>(type_, buildEntries());
}

void GenericAnnotationBuilder::put(ca::str::Utf8String name, DeferredElement element)
{
    putEntry(entries_, std::move(name), std::move(element));
}

std::vector<AnnotationEntry> GenericAnnotationBuilder::buildEntries() const
{
    return materialize(entries_);
}

GenericTypeAnnotationBuilder::GenericTypeAnnotationBuilder(type::InstanceTypePtr   type,
                                                           ca::i32                 typeRef,
                                                           std::optional<TypePath> typePath)
    : type_(std::move(type))
    , typeRef_(typeRef)
    , typePath_(std::move(typePath))
{
    if (!type_) throw std::invalid_argument("Type annotation builder type cannot be null");
}

GenericTypeAnnotationBuilder& GenericTypeAnnotationBuilder::element(ca::str::Utf8String name,
                                                                    ElementPtr          element)
{
    return this->element(std::move(name), fixed(std::move(element)));
}

GenericTypeAnnotationBuilder& GenericTypeAnnotationBuilder::element(ca::str::Utf8String name,
                                                                    DeferredElement     element)
{
    put(std::move(name), std::move(element));
    return *this;
}

GenericTypeAnnotationBuilder& GenericTypeAnnotationBuilder::type(type::InstanceTypePtr type)
{
    if (!type) throw std::invalid_argument("Type annotation builder type cannot be null");
    type_ = std::move(type);
    return *this;
}

GenericTypeAnnotationBuilder& GenericTypeAnnotationBuilder::typeRef(ca::i32 value) noexcept
{
    typeRef_ = value;
    return *this;
}

GenericTypeAnnotationBuilder& GenericTypeAnnotationBuilder::typePath(std::optional<TypePath> value)
{
    typePath_ = std::move(value);
    return *this;
}

AnnotationPtr GenericTypeAnnotationBuilder::build() const
{
    return buildType();
}

TypeAnnotationPtr GenericTypeAnnotationBuilder::buildType() const
{
    return std::make_shared<const MapTypeAnnotation>(type_, buildEntries(), typeRef_, typePath_);
}

void GenericTypeAnnotationBuilder::put(ca::str::Utf8String name, DeferredElement element)
{
    putEntry(entries_, std::move(name), std::move(element));
}

std::vector<AnnotationEntry> GenericTypeAnnotationBuilder::buildEntries() const
{
    return materialize(entries_);
}

}   //namespace jblw::annotation
