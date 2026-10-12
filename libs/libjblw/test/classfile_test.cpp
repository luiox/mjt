#include "jblw/classfile/generic/builders.hpp"
#include "jblw/constant/of_int.hpp"
#include "jblw/type/types.hpp"

#include <gtest/gtest.h>

using namespace jblw::annotation;
using namespace jblw::classfile;
using namespace jblw::classfile::attribute::generic;
using namespace jblw::classfile::generic;
using namespace jblw::code;
using namespace jblw::constant;
using namespace jblw::type;
using namespace jblw::version;

namespace {
InstanceTypePtr instance(const char* name)
{
    return Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr(name));
}
}   //namespace

TEST(BlwClassfileTest, GenericBuildersConstructCompleteOrderedClassView)
{
    GenericClassBuilder builder;
    auto                owner = instance("example/Example");
    builder.version(JavaVersion::V17)
        .accessFlags(AccessFlag::ACC_PUBLIC | AccessFlag::ACC_SUPER)
        .type(owner)
        .superClass(Types::OBJECT)
        .addInterface(instance("java/io/Serializable"))
        .sourceFile(ca::str::Utf8String("Example.java"));

    auto field =
        builder.putField(AccessFlag::ACC_PRIVATE, ca::str::Utf8String("value"), Types::INT);
    field->defaultValue(std::make_shared<const OfInt>(7));

    auto method = builder.putMethod(
        AccessFlag::ACC_PUBLIC, ca::str::Utf8String("getValue"), Types::methodType(Types::INT));
    method->parameter(std::make_shared<const GenericParameter>(AccessFlag::ACC_FINAL,
                                                               ca::str::Utf8String("ignored")));
    method->code()
        .maxStack(1)
        .maxLocals(1)
        .codeList()
        .addLabel(std::make_shared<GenericLabel>())
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::IRETURN));

    builder.putRecordComponent(ca::str::Utf8String("component"), Types::STRING);
    builder.innerClass(std::make_shared<const GenericInnerClass>(AccessFlag::ACC_PUBLIC,
                                                                 instance("example/Example$Inner"),
                                                                 owner,
                                                                 ca::str::Utf8String("Inner")));
    builder.permittedSubclass(instance("example/Child"));
    builder.nestHost(owner).nestMember(instance("example/Example$Inner"));

    ClassFileViewPtr result = builder.build();
    EXPECT_EQ(JavaVersion::V17, result->version());
    EXPECT_EQ(owner, result->type());
    EXPECT_EQ(1U, result->fields().size());
    EXPECT_EQ(1U, result->methods().size());
    EXPECT_EQ(1U, result->recordComponents().size());
    EXPECT_EQ(1U, result->innerClasses().size());
    EXPECT_EQ(7, static_cast<const OfInt&>(*result->fields()[0]->defaultValue()).value());
    EXPECT_NE(nullptr,
              result->field(ca::str::Utf8StringRef::from_cstr("value"),
                            ca::str::Utf8StringRef::from_cstr("I")));
    EXPECT_NE(nullptr,
              result->method(ca::str::Utf8StringRef::from_cstr("getValue"),
                             ca::str::Utf8StringRef::from_cstr("()I")));
    ASSERT_TRUE(result->sourceFile().has_value());
    EXPECT_EQ("Example.java", result->sourceFile()->to_std_string());
}

TEST(BlwClassfileTest, MemberReplacementKeepsOriginalInsertionSlot)
{
    GenericClassBuilder builder;
    builder.version(JavaVersion::V17).type(instance("example/Order"));
    builder.putField(0, ca::str::Utf8String("first"), Types::INT);
    builder.putField(0, ca::str::Utf8String("second"), Types::LONG);
    auto replacement =
        builder.putField(AccessFlag::ACC_PUBLIC, ca::str::Utf8String("first"), Types::INT);
    replacement->defaultValue(std::make_shared<const OfInt>(1));

    auto result = builder.build();
    ASSERT_EQ(2U, result->fields().size());
    EXPECT_EQ("first", result->fields()[0]->name().to_std_string());
    EXPECT_EQ(AccessFlag::ACC_PUBLIC, result->fields()[0]->accessFlags());
    EXPECT_EQ("second", result->fields()[1]->name().to_std_string());
}

TEST(BlwClassfileTest, AnnotationAndModuleBuildersPreserveNestedState)
{
    GenericClassBuilder builder;
    builder.version(JavaVersion::V17).type(instance("module-info"));
    auto annotation = builder.annotations().addVisibleRuntimeAnnotation(instance("example/Marker"));
    annotation->element(ca::str::Utf8String("value"),
                        std::make_shared<const ElementString>(ca::str::Utf8String("ok")));

    auto module = builder.addModule(
        ca::str::Utf8String("example.module"), AccessFlag::ACC_OPEN, ca::str::Utf8String("1.0"));
    std::vector<ca::str::Utf8String> providers;
    providers.emplace_back("example/ServiceImpl");
    module->mainClass(ca::str::Utf8String("example/Main"))
        .package(ca::str::Utf8String("example"))
        .require(std::make_shared<const GenericModuleRequire>(
            AccessFlag::ACC_TRANSITIVE, ca::str::Utf8String("java.base"), std::nullopt))
        .provide(std::make_shared<const GenericModuleProvide>(
            ca::str::Utf8String("example/Service"), std::move(providers)));

    auto result = builder.build();
    ASSERT_EQ(1U, result->visibleRuntimeAnnotations().size());
    EXPECT_EQ(
        "ok",
        static_cast<const ElementString&>(*result->visibleRuntimeAnnotations()[0]
                                               ->get(ca::str::Utf8StringRef::from_cstr("value"))
                                               ->get())
            .value()
            .to_std_string());
    ASSERT_EQ(1U, result->modules().size());
    EXPECT_EQ("example.module", result->modules()[0]->name().to_std_string());
    EXPECT_EQ(1U, result->modules()[0]->requires().size());
    EXPECT_EQ(1U, result->modules()[0]->provides().size());
}
