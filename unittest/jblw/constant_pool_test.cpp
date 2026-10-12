#include "jblw/constantpool/constant_pool.hpp"
#include "jblw/constantpool/entry_traits.hpp"
#include "jblw/type/types.hpp"

#include <gtest/gtest.h>

using namespace jblw::constant;
using namespace jblw::constantpool;
using namespace jblw::type;

namespace {
MethodHandlePtr bootstrapHandle()
{
    return std::make_shared<const MethodHandle>(
        6,
        Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("example/Bootstrap")),
        ca::str::Utf8String("bootstrap"),
        Types::methodType(Types::OBJECT, {Types::OBJECT}),
        false);
}
}   //namespace

TEST(BlwConstantPoolTest, EntryFamilyExposesJvmTagsAndPayloads)
{
    EntryUtf8               utf8(OfString(ca::str::Utf8String("name")));
    EntryInteger            integer(OfInt(42));
    EntryFloat              floating(OfFloat(1.5F));
    EntryLong               wideInteger(OfLong(7));
    EntryDouble             wideFloat(OfDouble(2.5));
    EntryClass              classEntry(3);
    EntryString             stringEntry(4);
    EntryFieldRef           field(5, 6);
    EntryMethodRef          method(7, 8);
    EntryInterfaceMethodRef interfaceMethod(9, 10);
    EntryNameAndType        nameAndType(11, 12);
    EntryMethodHandle       handle(6, 13);
    EntryMethodType         methodType(14);
    EntryModule             module(15);
    EntryPackage            package(16);

    EXPECT_EQ(Tag::Utf8, utf8.tag());
    EXPECT_EQ("name", utf8.value().value().to_std_string());
    EXPECT_EQ(42, integer.value().value());
    EXPECT_FLOAT_EQ(1.5F, floating.value().value());
    EXPECT_EQ(7, wideInteger.value().value());
    EXPECT_DOUBLE_EQ(2.5, wideFloat.value().value());
    EXPECT_EQ(3, classEntry.nameIndex());
    EXPECT_EQ(4, stringEntry.utf8Index());
    EXPECT_EQ(5, field.classIndex());
    EXPECT_EQ(8, method.nameAndTypeIndex());
    EXPECT_EQ(Tag::InterfaceMethodref, interfaceMethod.tag());
    EXPECT_EQ(11, nameAndType.nameIndex());
    EXPECT_EQ(12, nameAndType.typeIndex());
    EXPECT_EQ(6, handle.referenceKind());
    EXPECT_EQ(13, handle.referenceIndex());
    EXPECT_EQ(14, methodType.descriptorIndex());
    EXPECT_EQ(15, module.nameIndex());
    EXPECT_EQ(16, package.nameIndex());
}

TEST(BlwConstantPoolTest, TypedCastsUseExplicitEntryKinds)
{
    EntryPtr entry = std::make_shared<const EntryMethodRef>(2, 3);
    ASSERT_NE(nullptr, ca::core::tag_cast::dyn_cast<EntryMethodRef>(entry.get()));
    EXPECT_EQ(nullptr, ca::core::tag_cast::dyn_cast<EntryFieldRef>(entry.get()));
}

TEST(BlwConstantPoolTest, DynamicEntriesKeepResolvedBlwValues)
{
    auto                     handle = bootstrapHandle();
    std::vector<ConstantPtr> arguments{std::make_shared<const OfInt>(7)};
    auto                     dynamic = std::make_shared<const ConstantDynamic>(
        ca::str::Utf8String("value"), Types::STRING, handle, arguments);
    auto invoke = std::make_shared<const InvokeDynamic>(
        ca::str::Utf8String("call"), Types::methodType(Types::VOID), handle, arguments);

    EntryDynamic       dynamicEntry{OfDynamic(dynamic)};
    EntryInvokeDynamic invokeEntry(invoke);
    EXPECT_EQ(Tag::Dynamic, dynamicEntry.tag());
    EXPECT_EQ(dynamic, dynamicEntry.value().value());
    EXPECT_EQ(Tag::InvokeDynamic, invokeEntry.tag());
    EXPECT_EQ(invoke, invokeEntry.value());
}

TEST(BlwConstantPoolTest, ListPoolPreservesWideReservedSlots)
{
    std::vector<EntryPtr> entries;
    entries.push_back(std::make_shared<const EntryInteger>(OfInt(1)));
    entries.push_back(std::make_shared<const EntryLong>(OfLong(2)));
    entries.push_back(nullptr);
    entries.push_back(std::make_shared<const EntryDouble>(OfDouble(3.0)));
    entries.push_back(nullptr);
    entries.push_back(std::make_shared<const EntryClass>(0));
    ListConstantPool pool(std::move(entries));

    EXPECT_EQ(6U, pool.size());
    EXPECT_EQ(nullptr, pool.get(2));
    EXPECT_EQ((std::vector<ca::i32>{0, 1, 3, 5}), pool.indices());
    EXPECT_THROW(pool.get(-1), std::out_of_range);
    EXPECT_THROW(pool.get(6), std::out_of_range);
}

TEST(BlwConstantPoolTest, ListPoolRejectsMalformedWideLayouts)
{
    std::vector<EntryPtr> missingSlot;
    missingSlot.push_back(std::make_shared<const EntryLong>(OfLong(1)));
    EXPECT_THROW(ListConstantPool(std::move(missingSlot)), std::invalid_argument);

    std::vector<EntryPtr> occupiedSlot;
    occupiedSlot.push_back(std::make_shared<const EntryDouble>(OfDouble(1.0)));
    occupiedSlot.push_back(std::make_shared<const EntryInteger>(OfInt(2)));
    EXPECT_THROW(ListConstantPool(std::move(occupiedSlot)), std::invalid_argument);

    std::vector<EntryPtr> straySlot;
    straySlot.push_back(nullptr);
    EXPECT_THROW(ListConstantPool(std::move(straySlot)), std::invalid_argument);
}
