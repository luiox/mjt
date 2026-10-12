#include "jblw/constant/constant_sink.hpp"
#include "jblw/constant/constant_traits.hpp"
#include "jblw/type/dynamic_traits.hpp"
#include "jblw/type/types.hpp"

#include <gtest/gtest.h>

#include "libca/core/tag_cast.hpp"

#include <limits>
#include <memory>

using namespace jblw::constant;
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

class RecordingSink final : public ConstantSink
{
public:
    void acceptString(const OfString&) override { seen = ConstantKind::STRING; }
    void acceptMethodHandle(const OfMethodHandle&) override { seen = ConstantKind::METHOD_HANDLE; }
    void acceptType(const OfType&) override { seen = ConstantKind::TYPE; }
    void acceptDynamic(const OfDynamic&) override { seen = ConstantKind::DYNAMIC; }
    void acceptLong(const OfLong&) override { seen = ConstantKind::LONG; }
    void acceptDouble(const OfDouble&) override { seen = ConstantKind::DOUBLE; }
    void acceptInt(const OfInt&) override { seen = ConstantKind::INT; }
    void acceptFloat(const OfFloat&) override { seen = ConstantKind::FLOAT; }

    ConstantKind seen = ConstantKind::STRING;
};
}   //namespace

TEST(BlwConstantTest, PrimitiveConstantsFollowBlwEqualityAndJavaHashes)
{
    OfInt  intValue(42);
    OfLong longValue(0x123456789abcdef0LL);
    EXPECT_EQ(42, intValue.hashCode());
    EXPECT_EQ(static_cast<ca::i32>(0x88888888U), longValue.hashCode());
    EXPECT_EQ(intValue, OfInt(42));
    EXPECT_NE(intValue, OfInt(41));

    OfFloat positiveZero(0.0F);
    OfFloat negativeZero(-0.0F);
    EXPECT_EQ(positiveZero, negativeZero);
    EXPECT_NE(positiveZero.hashCode(), negativeZero.hashCode());

    OfDouble nan(std::numeric_limits<ca::f64>::quiet_NaN());
    OfDouble anotherNan(std::numeric_limits<ca::f64>::quiet_NaN());
    EXPECT_EQ(nan, nan);
    EXPECT_NE(nan, anotherNan);
    EXPECT_EQ(static_cast<ca::i32>(0x7ff80000U), nan.hashCode());
}

TEST(BlwConstantTest, ReferenceConstantsOwnValuesAndUseStructuralEquality)
{
    OfString first(ca::str::Utf8String("hello"));
    OfString second(ca::str::Utf8String("hello"));
    EXPECT_EQ(first, second);
    EXPECT_EQ(99162322, first.hashCode());

    OfType intType(Types::INT);
    OfType parsedInt(Types::typeFromDescriptor(ca::str::Utf8StringRef::from_cstr("I")).unwrap());
    EXPECT_EQ(intType, parsedInt);

    auto firstHandle  = bootstrapHandle();
    auto secondHandle = bootstrapHandle();
    EXPECT_EQ(*firstHandle, *secondHandle);
    EXPECT_EQ(firstHandle->hashCode(), secondHandle->hashCode());
    EXPECT_EQ(OfMethodHandle(firstHandle), OfMethodHandle(secondHandle));
}

TEST(BlwConstantTest, DynamicObjectsKeepIdentityAndOwnBootstrapArguments)
{
    auto                     handle = bootstrapHandle();
    std::vector<ConstantPtr> args{std::make_shared<const OfString>(ca::str::Utf8String("arg")),
                                  std::make_shared<const OfInt>(7)};
    auto                     first = std::make_shared<const ConstantDynamic>(
        ca::str::Utf8String("value"), Types::STRING, handle, args);
    auto sameShape = std::make_shared<const ConstantDynamic>(
        ca::str::Utf8String("value"), Types::STRING, handle, args);

    EXPECT_EQ("value", first->name().to_std_string());
    EXPECT_EQ(2U, first->args().size());
    EXPECT_TRUE(ca::core::tag_cast::isa<ConstantDynamic>(first.get()));
    EXPECT_FALSE(ca::core::tag_cast::isa<InvokeDynamic>(first.get()));
    EXPECT_EQ(OfDynamic(first), OfDynamic(first));
    EXPECT_NE(OfDynamic(first), OfDynamic(sameShape));

    auto invoke = std::make_shared<const InvokeDynamic>(
        ca::str::Utf8String("call"), Types::methodType(Types::VOID), handle, args);
    EXPECT_TRUE(ca::core::tag_cast::isa<InvokeDynamic>(invoke.get()));
    EXPECT_EQ("()V", invoke->type().descriptor().to_std_string());
}

TEST(BlwConstantTest, ConstantTagsAndSinkSupportRttiFreeDispatch)
{
    std::vector<ConstantPtr> constants{
        std::make_shared<const OfString>(ca::str::Utf8String("text")),
        std::make_shared<const OfMethodHandle>(bootstrapHandle()),
        std::make_shared<const OfType>(Types::STRING),
        std::make_shared<const OfLong>(1),
        std::make_shared<const OfDouble>(2.0),
        std::make_shared<const OfInt>(3),
        std::make_shared<const OfFloat>(4.0F),
    };
    RecordingSink sink;

    for (const auto& constant : constants) {
        constant->accept(sink);
        EXPECT_EQ(constant->getType(), sink.seen);
    }
    EXPECT_TRUE(ca::core::tag_cast::isa<OfString>(constants[0].get()));
    EXPECT_EQ(nullptr, ca::core::tag_cast::dyn_cast<OfInt>(constants[0].get()));
    EXPECT_EQ(3, ca::core::tag_cast::cast<const OfInt>(constants[5].get())->value());
}
