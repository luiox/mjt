#include "jblw/annotation/builders.hpp"
#include "jblw/annotation/element_traits.hpp"
#include "jblw/type/types.hpp"

#include <gtest/gtest.h>

#include "libca/core/tag_cast.hpp"

#include <limits>

using namespace jblw::annotation;
using namespace jblw::type;

TEST(BlwAnnotationTest, ScalarElementsUseJavaRecordSemantics)
{
    ElementFloat positiveZero(0.0F);
    ElementFloat negativeZero(-0.0F);
    EXPECT_NE(positiveZero, negativeZero);

    ElementDouble nan(std::numeric_limits<ca::f64>::quiet_NaN());
    ElementDouble anotherNan(std::numeric_limits<ca::f64>::signaling_NaN());
    EXPECT_EQ(nan, anotherNan);
    EXPECT_EQ(nan.hashCode(), anotherNan.hashCode());

    EXPECT_EQ(booleanElement(true), booleanElement(true));
    EXPECT_EQ(1231, booleanElement(true)->hashCode());
    EXPECT_EQ(ElementInt(7), ElementInt(7));
}

TEST(BlwAnnotationTest, TypePathParsesAndPrintsAsmForm)
{
    auto path = TypePath::fromString(ca::str::Utf8StringRef::from_cstr("[.*12;"));
    ASSERT_TRUE(path.has_value());
    ASSERT_EQ(4U, path->length());
    EXPECT_EQ(TypePath::ARRAY_ELEMENT, path->step(0));
    EXPECT_EQ(TypePath::TYPE_ARGUMENT, path->step(3));
    EXPECT_EQ(12, path->stepArgument(3));
    EXPECT_EQ("[.*12;", path->toString().to_std_string());
    EXPECT_FALSE(TypePath::fromString(ca::str::Utf8StringRef()).has_value());
    EXPECT_THROW(TypePath::fromString(ca::str::Utf8StringRef::from_cstr("1")),
                 std::invalid_argument);
}

TEST(BlwAnnotationTest, BuildersPreserveOrderReplaceKeysAndBuildNestedValues)
{
    auto annotationType =
        Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("example/Marker"));
    auto nestedType =
        Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("example/Nested"));
    auto builder = std::make_shared<GenericAnnotationBuilder>(annotationType);
    builder->element(ca::str::Utf8String("number"), std::make_shared<const ElementInt>(1));
    auto array = builder->array(ca::str::Utf8String("values"));
    array->element(std::make_shared<const ElementString>(ca::str::Utf8String("a")));
    array->element(std::make_shared<const ElementString>(ca::str::Utf8String("b")));
    auto nested = builder->annotation(ca::str::Utf8String("nested"), nestedType);
    nested->element(ca::str::Utf8String("enabled"), booleanElement(true));
    builder->element(ca::str::Utf8String("number"), std::make_shared<const ElementInt>(2));

    AnnotationPtr result = builder->build();
    ASSERT_EQ(3U, result->entries().size());
    EXPECT_EQ("number", result->entries()[0].first.to_std_string());
    const ElementPtr* number = result->get(ca::str::Utf8StringRef::from_cstr("number"));
    ASSERT_NE(nullptr, number);
    EXPECT_EQ(2, ca::core::tag_cast::cast<const ElementInt>(number->get())->value());
    const ElementPtr* values = result->get(ca::str::Utf8StringRef::from_cstr("values"));
    ASSERT_NE(nullptr, values);
    EXPECT_EQ(2U, ca::core::tag_cast::cast<const ElementArray>(values->get())->count());
    EXPECT_TRUE(
        ca::core::tag_cast::isa<Annotation>(result->get(ca::str::Utf8StringRef::from_cstr("nested"))->get()));
}

TEST(BlwAnnotationTest, TypeAnnotationsCarryReferencePathAndStructuralValues)
{
    auto annotationType =
        Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr("example/TypeUse"));
    auto path = TypePath::fromString(ca::str::Utf8StringRef::from_cstr("[0;"));
    GenericTypeAnnotationBuilder builder(annotationType, 0x14000000, path);
    builder.element(ca::str::Utf8String("name"),
                    std::make_shared<const ElementString>(ca::str::Utf8String("x")));

    TypeAnnotationPtr first  = builder.buildType();
    TypeAnnotationPtr second = builder.buildType();
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(first->hashCode(), second->hashCode());
    EXPECT_EQ(0x14000000, first->typeRef());
    ASSERT_TRUE(first->typePath().has_value());
    EXPECT_EQ("[0;", first->typePath()->toString().to_std_string());
}
