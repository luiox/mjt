#pragma once

#include "jblw/constant/of_double.hpp"
#include "jblw/constant/of_dynamic.hpp"
#include "jblw/constant/of_float.hpp"
#include "jblw/constant/of_int.hpp"
#include "jblw/constant/of_long.hpp"
#include "jblw/constant/of_string.hpp"
#include "jblw/type/invoke_dynamic.hpp"

#include <libca/core/datatype.hpp>

#include <memory>
#include <stdexcept>
#include <utility>

namespace jblw::constantpool {

///@brief JVM constant_pool_info tag values.
struct Tag final
{
    static constexpr ca::i32 Utf8               = 1;
    static constexpr ca::i32 Integer            = 3;
    static constexpr ca::i32 Float              = 4;
    static constexpr ca::i32 Long               = 5;
    static constexpr ca::i32 Double             = 6;
    static constexpr ca::i32 Class              = 7;
    static constexpr ca::i32 String             = 8;
    static constexpr ca::i32 Fieldref           = 9;
    static constexpr ca::i32 Methodref          = 10;
    static constexpr ca::i32 InterfaceMethodref = 11;
    static constexpr ca::i32 NameAndType        = 12;
    static constexpr ca::i32 MethodHandle       = 15;
    static constexpr ca::i32 MethodType         = 16;
    static constexpr ca::i32 Dynamic            = 17;
    static constexpr ca::i32 InvokeDynamic      = 18;
    static constexpr ca::i32 Module             = 19;
    static constexpr ca::i32 Package            = 20;

    Tag() = delete;
};

enum class EntryKind : ca::i32
{
    UTF8                 = Tag::Utf8,
    INTEGER              = Tag::Integer,
    FLOAT                = Tag::Float,
    LONG                 = Tag::Long,
    DOUBLE               = Tag::Double,
    CLASS                = Tag::Class,
    STRING               = Tag::String,
    FIELD_REF            = Tag::Fieldref,
    METHOD_REF           = Tag::Methodref,
    INTERFACE_METHOD_REF = Tag::InterfaceMethodref,
    NAME_AND_TYPE        = Tag::NameAndType,
    METHOD_HANDLE        = Tag::MethodHandle,
    METHOD_TYPE          = Tag::MethodType,
    DYNAMIC              = Tag::Dynamic,
    INVOKE_DYNAMIC       = Tag::InvokeDynamic,
    MODULE               = Tag::Module,
    PACKAGE              = Tag::Package,
};

class Entry
{
public:
    virtual ~Entry()                           = default;
    virtual EntryKind getType() const noexcept = 0;
    ca::i32           tag() const noexcept { return static_cast<ca::i32>(getType()); }
};
using EntryPtr = std::shared_ptr<const Entry>;

template<typename Value, EntryKind Kind>
class ValueEntry : public Entry
{
public:
    explicit ValueEntry(Value value)
        : value_(std::move(value))
    {}
    const Value& value() const noexcept { return value_; }
    EntryKind    getType() const noexcept final { return Kind; }

private:
    Value value_;
};

class EntryUtf8 final : public ValueEntry<constant::OfString, EntryKind::UTF8>
{
public:
    using ValueEntry::ValueEntry;
};

class EntryInteger final : public ValueEntry<constant::OfInt, EntryKind::INTEGER>
{
public:
    using ValueEntry::ValueEntry;
};

class EntryFloat final : public ValueEntry<constant::OfFloat, EntryKind::FLOAT>
{
public:
    using ValueEntry::ValueEntry;
};

class EntryLong final : public ValueEntry<constant::OfLong, EntryKind::LONG>
{
public:
    using ValueEntry::ValueEntry;
};

class EntryDouble final : public ValueEntry<constant::OfDouble, EntryKind::DOUBLE>
{
public:
    using ValueEntry::ValueEntry;
};

class EntryDynamic final : public ValueEntry<constant::OfDynamic, EntryKind::DYNAMIC>
{
public:
    using ValueEntry::ValueEntry;
};

class EntryInvokeDynamic final
    : public ValueEntry<type::InvokeDynamicPtr, EntryKind::INVOKE_DYNAMIC>
{
public:
    explicit EntryInvokeDynamic(type::InvokeDynamicPtr value)
        : ValueEntry(requireValue(std::move(value)))
    {}

private:
    static type::InvokeDynamicPtr requireValue(type::InvokeDynamicPtr value)
    {
        if (!value) throw std::invalid_argument("InvokeDynamic entry value must not be null");
        return value;
    }
};

template<EntryKind Kind>
class SingleIndexEntry : public Entry
{
public:
    explicit SingleIndexEntry(ca::i32 index) noexcept
        : index_(index)
    {}
    ca::i32   index() const noexcept { return index_; }
    EntryKind getType() const noexcept final { return Kind; }

private:
    ca::i32 index_;
};

class EntryClass final : public SingleIndexEntry<EntryKind::CLASS>
{
public:
    using SingleIndexEntry::SingleIndexEntry;
    ca::i32 nameIndex() const noexcept { return index(); }
};

class EntryString final : public SingleIndexEntry<EntryKind::STRING>
{
public:
    using SingleIndexEntry::SingleIndexEntry;
    ca::i32 utf8Index() const noexcept { return index(); }
};

class EntryMethodType final : public SingleIndexEntry<EntryKind::METHOD_TYPE>
{
public:
    using SingleIndexEntry::SingleIndexEntry;
    ca::i32 descriptorIndex() const noexcept { return index(); }
};

class EntryModule final : public SingleIndexEntry<EntryKind::MODULE>
{
public:
    using SingleIndexEntry::SingleIndexEntry;
    ca::i32 nameIndex() const noexcept { return index(); }
};

class EntryPackage final : public SingleIndexEntry<EntryKind::PACKAGE>
{
public:
    using SingleIndexEntry::SingleIndexEntry;
    ca::i32 nameIndex() const noexcept { return index(); }
};

class EntryNameAndType final : public Entry
{
public:
    EntryNameAndType(ca::i32 nameIndex, ca::i32 typeIndex) noexcept
        : nameIndex_(nameIndex)
        , typeIndex_(typeIndex)
    {}
    ca::i32   nameIndex() const noexcept { return nameIndex_; }
    ca::i32   typeIndex() const noexcept { return typeIndex_; }
    EntryKind getType() const noexcept override { return EntryKind::NAME_AND_TYPE; }

private:
    ca::i32 nameIndex_;
    ca::i32 typeIndex_;
};

class EntryMethodHandle final : public Entry
{
public:
    EntryMethodHandle(ca::i32 referenceKind, ca::i32 referenceIndex) noexcept
        : referenceKind_(referenceKind)
        , referenceIndex_(referenceIndex)
    {}
    ca::i32   referenceKind() const noexcept { return referenceKind_; }
    ca::i32   referenceIndex() const noexcept { return referenceIndex_; }
    EntryKind getType() const noexcept override { return EntryKind::METHOD_HANDLE; }

private:
    ca::i32 referenceKind_;
    ca::i32 referenceIndex_;
};

class EntryMemberRef : public Entry
{
public:
    ca::i32 classIndex() const noexcept { return classIndex_; }
    ca::i32 nameAndTypeIndex() const noexcept { return nameAndTypeIndex_; }

protected:
    EntryMemberRef(ca::i32 classIndex, ca::i32 nameAndTypeIndex) noexcept
        : classIndex_(classIndex)
        , nameAndTypeIndex_(nameAndTypeIndex)
    {}

private:
    ca::i32 classIndex_;
    ca::i32 nameAndTypeIndex_;
};

class EntryFieldRef final : public EntryMemberRef
{
public:
    EntryFieldRef(ca::i32 classIndex, ca::i32 nameAndTypeIndex) noexcept
        : EntryMemberRef(classIndex, nameAndTypeIndex)
    {}
    EntryKind getType() const noexcept override { return EntryKind::FIELD_REF; }
};

class EntryMethodRef final : public EntryMemberRef
{
public:
    EntryMethodRef(ca::i32 classIndex, ca::i32 nameAndTypeIndex) noexcept
        : EntryMemberRef(classIndex, nameAndTypeIndex)
    {}
    EntryKind getType() const noexcept override { return EntryKind::METHOD_REF; }
};

class EntryInterfaceMethodRef final : public EntryMemberRef
{
public:
    EntryInterfaceMethodRef(ca::i32 classIndex, ca::i32 nameAndTypeIndex) noexcept
        : EntryMemberRef(classIndex, nameAndTypeIndex)
    {}
    EntryKind getType() const noexcept override { return EntryKind::INTERFACE_METHOD_REF; }
};

}   //namespace jblw::constantpool
