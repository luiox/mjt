#include "annotation.hpp"

#include <libca/str/utf16_string.hpp>

#include <stdexcept>

namespace jblw::annotation {

namespace {
ca::i32 javaStringHash(ca::str::Utf8StringRef value)
{
    return ca::str::Utf16String::from_utf8_string(value).hash_code();
}

void validate(type::InstanceTypePtr& type, const std::vector<AnnotationEntry>& entries)
{
    if (!type) throw std::invalid_argument("Annotation type cannot be null");
    for (const auto& entry : entries) {
        if (!entry.second) throw std::invalid_argument("Annotation element cannot be null");
    }
}

const ElementPtr* find(const std::vector<AnnotationEntry>& entries,
                       ca::str::Utf8StringRef              name) noexcept
{
    for (const auto& entry : entries) {
        if (entry.first.equals(name)) return &entry.second;
    }
    return nullptr;
}

bool entriesEqual(const std::vector<AnnotationEntry>& lhs,
                  const std::vector<AnnotationEntry>& rhs) noexcept
{
    if (lhs.size() != rhs.size()) return false;
    for (const auto& entry : lhs) {
        const ElementPtr* value = find(rhs, entry.first.ref());
        if (!value || *entry.second != **value) return false;
    }
    return true;
}

ca::i32 entriesHash(const std::vector<AnnotationEntry>& entries) noexcept
{
    ca::u32 hash = 0;
    for (const auto& entry : entries) {
        hash += static_cast<ca::u32>(javaStringHash(entry.first.ref())) ^
                static_cast<ca::u32>(entry.second->hashCode());
    }
    return static_cast<ca::i32>(hash);
}

bool annotationFieldsEqual(const type::InstanceTypePtr&        type,
                           const std::vector<AnnotationEntry>& entries,
                           const Annotation&                   other) noexcept
{
    return *type == *other.type() && entriesEqual(entries, other.entries());
}

ca::i32 annotationHash(const type::InstanceTypePtr&        type,
                       const std::vector<AnnotationEntry>& entries) noexcept
{
    return static_cast<ca::i32>(static_cast<ca::u32>(type->hashCode()) * 31U +
                                static_cast<ca::u32>(entriesHash(entries)));
}
}   //namespace

MapAnnotation::MapAnnotation(type::InstanceTypePtr type, std::vector<AnnotationEntry> entries)
    : type_(std::move(type))
    , entries_(std::move(entries))
    , hashCode_(0)
{
    validate(type_, entries_);
    hashCode_ = annotationHash(type_, entries_);
}

const ElementPtr* MapAnnotation::get(ca::str::Utf8StringRef name) const noexcept
{
    return find(entries_, name);
}

bool MapAnnotation::equals(const Element& other) const noexcept
{
    return this == &other ||
           (other.getType() == ElementKind::ANNOTATION &&
            annotationFieldsEqual(type_, entries_, static_cast<const Annotation&>(other)));
}

MapTypeAnnotation::MapTypeAnnotation(type::InstanceTypePtr        type,
                                     std::vector<AnnotationEntry> entries, ca::i32 typeRef,
                                     std::optional<TypePath> typePath)
    : type_(std::move(type))
    , entries_(std::move(entries))
    , typeRef_(typeRef)
    , typePath_(std::move(typePath))
    , hashCode_(0)
{
    validate(type_, entries_);
    ca::u32 hash = static_cast<ca::u32>(annotationHash(type_, entries_));
    hash         = hash * 31U + static_cast<ca::u32>(typeRef_);
    hash         = hash * 31U + (typePath_ ? static_cast<ca::u32>(typePath_->hashCode()) : 0U);
    hashCode_    = static_cast<ca::i32>(hash);
}

const ElementPtr* MapTypeAnnotation::get(ca::str::Utf8StringRef name) const noexcept
{
    return find(entries_, name);
}

bool MapTypeAnnotation::equals(const Element& other) const noexcept
{
    if (this == &other) return true;
    if (other.getType() != ElementKind::ANNOTATION) return false;
    const auto& annotation = static_cast<const Annotation&>(other);
    if (annotation.annotationKind() != AnnotationKind::TYPE) return false;
    const auto& typed = static_cast<const TypeAnnotation&>(annotation);
    return annotationFieldsEqual(type_, entries_, annotation) && typeRef_ == typed.typeRef() &&
           typePath_ == typed.typePath();
}

}   //namespace jblw::annotation
