#include "jblw/type/primitive_kind.hpp"
#include "jblw/type/type_reader.hpp"
#include "jblw/type/type_traits.hpp"
#include "jblw/type/types.hpp"

#include <gtest/gtest.h>

#include "libca/core/tag_cast.hpp"

#include <string_view>

using namespace jblw::type;

namespace {
std::string text(ca::str::Utf8StringRef value)
{
    return value.to_std_string();
}
}   //namespace

TEST(BlwTypeTest, PrimitiveSingletonsExposeUpstreamValues)
{
    EXPECT_EQ("V", text(Types::VOID->descriptor()));
    EXPECT_EQ("int", text(Types::INT->name()));
    EXPECT_EQ(PrimitiveKind::T_INT, Types::INT->kind());
    EXPECT_EQ(PrimitiveKind::T_DOUBLE, Types::DOUBLE->hashCode());
    EXPECT_TRUE(*Types::INT == *Types::primitiveFromChar('I').unwrap());
    EXPECT_TRUE(Types::isPrimitive(ca::str::Utf8StringRef::from_cstr("Z")));
    EXPECT_FALSE(Types::isPrimitive(ca::str::Utf8StringRef::from_cstr("Ljava/lang/Object;")));
}

TEST(BlwTypeTest, PrimitiveBoxAndWidenFollowBlwTables)
{
    auto boxed = Types::INT->box();
    ASSERT_TRUE(boxed.is_ok());
    EXPECT_EQ("Ljava/lang/Integer;", text(boxed.unwrap()->descriptor()));

    auto voidBox = Types::VOID->box();
    ASSERT_TRUE(voidBox.is_err());
    EXPECT_EQ(TypeErrorCode::CANNOT_BOX_VOID, voidBox.unwrap_err().code);

    EXPECT_EQ(PrimitiveKind::T_DOUBLE, Types::BOOLEAN->widen(*Types::DOUBLE).unwrap()->kind());
    EXPECT_EQ(PrimitiveKind::T_CHAR, Types::BYTE->widen(*Types::CHAR).unwrap()->kind());
    EXPECT_EQ(PrimitiveKind::T_FLOAT, Types::INT->widen(*Types::FLOAT).unwrap()->kind());
    EXPECT_EQ(PrimitiveKind::T_LONG, Types::LONG->widen(*Types::DOUBLE).unwrap()->kind());
    EXPECT_EQ(PrimitiveKind::T_DOUBLE, Types::DOUBLE->widen(*Types::BYTE).unwrap()->kind());
}

TEST(BlwTypeTest, InstanceNamesAndEqualityOwnTheirStrings)
{
    auto first =
        Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("java/lang/String"));
    auto second =
        Types::instanceTypeFromExternalName(ca::str::Utf8StringRef::from_cstr("java.lang.String"));

    EXPECT_EQ("Ljava/lang/String;", text(first->descriptor()));
    EXPECT_EQ("java/lang/String", text(first->internalName()));
    EXPECT_EQ("java.lang.String", text(first->externalName()));
    EXPECT_TRUE(*first == *second);
    EXPECT_EQ(first->hashCode(), second->hashCode());
}

TEST(BlwTypeTest, ArrayTypeKeepsComponentGraphAlive)
{
    auto parsed =
        Types::arrayTypeFromDescriptor(ca::str::Utf8StringRef::from_cstr("[[Ljava/lang/String;"));
    ASSERT_TRUE(parsed.is_ok());
    ArrayTypePtr array = parsed.unwrap();

    EXPECT_EQ(2, array->dimensions());
    EXPECT_EQ("[[Ljava/lang/String;", text(array->descriptor()));
    EXPECT_EQ("[[Ljava.lang.String;", text(array->externalName()));
    ASSERT_EQ(TypeKind::INSTANCE, array->rootComponentType()->getType());
    EXPECT_EQ("java/lang/String",
              text(static_cast<const InstanceType*>(array->rootComponentType())->internalName()));
}

TEST(BlwTypeTest, ParsesAndBuildsMethodDescriptors)
{
    auto parsed = Types::methodType(ca::str::Utf8StringRef::from_cstr("(I[Ljava/lang/String;)J"));
    ASSERT_TRUE(parsed.is_ok());
    MethodTypePtr method = parsed.unwrap();

    EXPECT_EQ("(I[Ljava/lang/String;)J", text(method->descriptor()));
    ASSERT_EQ(2U, method->parameterTypes().size());
    EXPECT_EQ(TypeKind::PRIMITIVE, method->parameterTypes()[0]->getType());
    EXPECT_EQ(TypeKind::ARRAY, method->parameterTypes()[1]->getType());
    EXPECT_EQ(Types::CATEGORY2, Types::category(*method->returnType()));
    EXPECT_EQ(Types::CATEGORY1 + Types::CATEGORY1, Types::parametersSize(*method));

    auto rebuilt = Types::methodType(method->returnType(), method->parameterTypes());
    EXPECT_TRUE(*method == *rebuilt);
    EXPECT_EQ(method->hashCode(), rebuilt->hashCode());
}

TEST(BlwTypeTest, TypeReaderCanReadASequenceWithoutBorrowingResults)
{
    ca::str::Utf8String descriptor("ILjava/lang/String;");
    TypeReader          reader(descriptor.ref());

    auto first = reader.read();
    ASSERT_TRUE(first.is_ok());
    ASSERT_TRUE(first.unwrap().has_value());
    EXPECT_EQ("I", text(first.unwrap().value()->descriptor()));

    auto second = reader.read();
    ASSERT_TRUE(second.is_ok());
    ASSERT_TRUE(second.unwrap().has_value());
    EXPECT_EQ("Ljava/lang/String;", text(second.unwrap().value()->descriptor()));

    auto end = reader.read();
    ASSERT_TRUE(end.is_ok());
    EXPECT_FALSE(end.unwrap().has_value());
}

TEST(BlwTypeTest, RejectsMalformedDescriptorsWithStableErrors)
{
    auto empty = Types::typeFromDescriptor(ca::str::Utf8StringRef());
    ASSERT_TRUE(empty.is_err());
    EXPECT_EQ(TypeErrorCode::EMPTY_DESCRIPTOR, empty.unwrap_err().code);

    auto unterminated =
        Types::typeFromDescriptor(ca::str::Utf8StringRef::from_cstr("Ljava/lang/String"));
    ASSERT_TRUE(unterminated.is_err());
    EXPECT_EQ(TypeErrorCode::UNTERMINATED_INSTANCE, unterminated.unwrap_err().code);

    auto trailing = Types::typeFromDescriptor(ca::str::Utf8StringRef::from_cstr("II"));
    ASSERT_TRUE(trailing.is_err());
    EXPECT_EQ(TypeErrorCode::TRAILING_CHARACTERS, trailing.unwrap_err().code);

    auto voidParameter = Types::typeFromDescriptor(ca::str::Utf8StringRef::from_cstr("(V)V"));
    ASSERT_TRUE(voidParameter.is_err());
    EXPECT_EQ(TypeErrorCode::INVALID_VOID_USE, voidParameter.unwrap_err().code);

    auto voidArray = Types::typeFromDescriptor(ca::str::Utf8StringRef::from_cstr("[V"));
    ASSERT_TRUE(voidArray.is_err());
    EXPECT_EQ(TypeErrorCode::INVALID_VOID_USE, voidArray.unwrap_err().code);
}

TEST(BlwTypeTest, TypeTagsSupportRttiFreeCasts)
{
    TypePtr primitive = Types::INT;
    TypePtr instance  = Types::OBJECT;
    TypePtr array     = Types::arrayType(Types::INT);
    TypePtr method    = Types::methodType(Types::VOID);

    EXPECT_TRUE(ca::core::tag_cast::isa<PrimitiveType>(primitive.get()));
    EXPECT_TRUE(ca::core::tag_cast::isa<InstanceType>(instance.get()));
    EXPECT_TRUE(ca::core::tag_cast::isa<ArrayType>(array.get()));
    EXPECT_TRUE(ca::core::tag_cast::isa<MethodType>(method.get()));
    EXPECT_EQ(PrimitiveKind::T_INT, ca::core::tag_cast::cast<const PrimitiveType>(primitive.get())->kind());
    EXPECT_EQ(nullptr, ca::core::tag_cast::dyn_cast<MethodType>(instance.get()));
}

TEST(BlwTypeTest, ReportsPrimitiveCategoriesAndStorageSizes)
{
    EXPECT_EQ(Types::CATEGORY1, Types::category(*Types::INT));
    EXPECT_EQ(Types::CATEGORY2, Types::category(*Types::LONG));
    EXPECT_EQ(1, Types::sizeOf(*Types::BOOLEAN).unwrap());
    EXPECT_EQ(2, Types::sizeOf(*Types::CHAR).unwrap());
    EXPECT_EQ(4, Types::sizeOf(*Types::FLOAT).unwrap());
    EXPECT_EQ(8, Types::sizeOf(*Types::DOUBLE).unwrap());
    EXPECT_TRUE(Types::sizeOf(*Types::VOID).is_err());
}
