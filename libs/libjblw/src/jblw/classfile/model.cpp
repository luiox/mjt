#include "model.hpp"

#include <stdexcept>

namespace jblw::classfile {

MemberIdentifier::MemberIdentifier(ca::str::Utf8String name, ca::str::Utf8String descriptor)
    : name_(std::move(name))
    , descriptor_(std::move(descriptor))
{}

MemberIdentifier::MemberIdentifier(ca::str::Utf8String name, const type::Type& type)
    : MemberIdentifier(
          std::move(name),
          ca::str::Utf8String::from_data(type.descriptor().data(), type.descriptor().byte_length()))
{}

bool MemberIdentifier::operator==(const MemberIdentifier& other) const noexcept
{
    return name_.ref() == other.name_.ref() && descriptor_.ref() == other.descriptor_.ref();
}

namespace attribute::generic {
GenericInnerClass::GenericInnerClass(ca::i32 accessFlags, type::InstanceTypePtr type,
                                     type::InstanceTypePtr              outerType,
                                     std::optional<ca::str::Utf8String> innerName)
    : accessFlags_(accessFlags)
    , type_(std::move(type))
    , outerType_(std::move(outerType))
    , innerName_(std::move(innerName))
{
    if (!type_) throw std::invalid_argument("Inner class type must not be null");
}
}   //namespace attribute::generic

MethodPtr ClassFileView::method(ca::str::Utf8StringRef name,
                                ca::str::Utf8StringRef descriptor) const noexcept
{
    for (const auto& method : methods()) {
        if (method && method->name() == name && method->type()->descriptor() == descriptor) {
            return method;
        }
    }
    return nullptr;
}

FieldPtr ClassFileView::field(ca::str::Utf8StringRef name,
                              ca::str::Utf8StringRef descriptor) const noexcept
{
    for (const auto& field : fields()) {
        if (field && field->name() == name && field->type()->descriptor() == descriptor) {
            return field;
        }
    }
    return nullptr;
}

}   //namespace jblw::classfile
