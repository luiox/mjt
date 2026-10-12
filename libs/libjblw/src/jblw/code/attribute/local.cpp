#include "local.hpp"

#include <stdexcept>

namespace jblw::code::attribute {

namespace generic {

GenericLocal::GenericLocal(LabelPtr start, LabelPtr end, ca::i32 index, ca::str::Utf8String name,
                           type::ClassTypePtr type, std::optional<ca::str::Utf8String> signature)
    : start_(std::move(start))
    , end_(std::move(end))
    , index_(index)
    , name_(std::move(name))
    , type_(std::move(type))
    , signature_(std::move(signature))
{
    if (!start_ || !end_ || !type_) {
        throw std::invalid_argument("Local labels and type must not be null");
    }
}

bool GenericLocal::operator==(const GenericLocal& other) const noexcept
{
    return index_ == other.index_ && start_->equals(*other.start_) && end_->equals(*other.end_) &&
           name_.ref() == other.name_.ref() && type_->equals(*other.type_) &&
           signature_ == other.signature_;
}

}   //namespace generic

}   //namespace jblw::code::attribute
