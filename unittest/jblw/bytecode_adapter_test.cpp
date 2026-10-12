#include "jblw/asm/bytecode_library.hpp"
#include "jblw/classfile/generic/builders.hpp"
#include "jblw/constant/of_dynamic.hpp"
#include "jblw/constant/of_int.hpp"
#include "jblw/constantpool/constant_pool.hpp"
#include "jblw/type/types.hpp"
#include "jbytecode/core/frame.hpp"
#include "test_helper.hpp"

#include <gtest/gtest.h>
#include <libca/str/utf8_string_arena.hpp>

#include <filesystem>
#include <fstream>

using namespace jblw::adapter;
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

ClassFileViewPtr buildStructuralClass()
{
    GenericClassBuilder builder;
    auto                owner = instance("example/AdapterSubject");
    builder.version(JavaVersion::V17)
        .accessFlags(AccessFlag::ACC_PUBLIC | AccessFlag::ACC_SUPER | AccessFlag::ACC_RECORD)
        .type(owner)
        .superClass(Types::OBJECT)
        .sourceFile(ca::str::Utf8String("AdapterSubject.java"))
        .nestMember(instance("example/AdapterSubject$Nested"))
        .permittedSubclass(instance("example/AdapterChild"));
    builder.innerClass(
        std::make_shared<const GenericInnerClass>(AccessFlag::ACC_PUBLIC | AccessFlag::ACC_STATIC,
                                                  instance("example/AdapterSubject$Nested"),
                                                  owner,
                                                  ca::str::Utf8String("Nested")));

    auto annotation = builder.annotations().addVisibleRuntimeAnnotation(instance("example/Marker"));
    annotation->element(ca::str::Utf8String("value"),
                        std::make_shared<const ElementString>(ca::str::Utf8String("class")));

    auto field =
        builder.putField(AccessFlag::ACC_PUBLIC | AccessFlag::ACC_STATIC | AccessFlag::ACC_FINAL,
                         ca::str::Utf8String("ANSWER"),
                         Types::INT);
    field->defaultValue(std::make_shared<const OfInt>(42));
    field->annotations()
        .addInvisibleRuntimeAnnotation(instance("example/FieldMarker"))
        ->element(ca::str::Utf8String("number"), std::make_shared<const ElementInt>(7));

    auto record = builder.putRecordComponent(ca::str::Utf8String("value"), Types::INT);
    record->annotations()
        .addVisibleRuntimeAnnotation(instance("example/RecordMarker"))
        ->element(ca::str::Utf8String("name"),
                  std::make_shared<const ElementString>(ca::str::Utf8String("value")));

    auto method = builder.putMethod(AccessFlag::ACC_PUBLIC | AccessFlag::ACC_STATIC,
                                    ca::str::Utf8String("choose"),
                                    Types::methodType(Types::INT, {Types::INT}));
    method->parameter(std::make_shared<const GenericParameter>(AccessFlag::ACC_FINAL,
                                                               ca::str::Utf8String("input")));
    method->annotations()
        .addVisibleRuntimeAnnotation(instance("example/MethodMarker"))
        ->element(ca::str::Utf8String("enabled"), booleanElement(true));
    auto zero = std::make_shared<GenericLabel>();
    zero->setLineNumber(12);
    method->code()
        .maxStack(1)
        .maxLocals(1)
        .codeList()
        .addInstruction(std::make_shared<VarInstruction>(JavaOpcodes::ILOAD, 0))
        .addInstruction(std::make_shared<ConditionalJumpInstruction>(JavaOpcodes::IFEQ, zero))
        .addInstruction(std::make_shared<IntConstantInstruction>(std::make_shared<const OfInt>(1)))
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::IRETURN))
        .addLabel(zero)
        .addInstruction(std::make_shared<IntConstantInstruction>(std::make_shared<const OfInt>(0)))
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::IRETURN));

    auto guarded = builder.putMethod(AccessFlag::ACC_PUBLIC | AccessFlag::ACC_STATIC,
                                     ca::str::Utf8String("guarded"),
                                     Types::methodType(Types::VOID));
    auto start   = std::make_shared<GenericLabel>();
    auto end     = std::make_shared<GenericLabel>();
    auto handler = std::make_shared<GenericLabel>();
    guarded->code().maxStack(1).maxLocals(0).tryCatchBlock(
        {start, end, handler, instance("java/lang/Exception")});
    guarded->code()
        .codeList()
        .addLabel(start)
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::NOP))
        .addLabel(end)
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::RETURN))
        .addLabel(handler)
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::POP))
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::RETURN));

    auto main =
        builder.putMethod(AccessFlag::ACC_PUBLIC | AccessFlag::ACC_STATIC,
                          ca::str::Utf8String("main"),
                          Types::methodType(Types::VOID, {Types::arrayType(Types::STRING)}));
    main->code().maxStack(0).maxLocals(1).codeList().addInstruction(
        std::make_shared<SimpleInstruction>(JavaOpcodes::RETURN));
    return builder.build();
}

MethodHandlePtr dynamicBootstrapHandle()
{
    return std::make_shared<const MethodHandle>(6,
                                                instance("example/Bootstrap"),
                                                ca::str::Utf8String("bootstrap"),
                                                Types::methodType(Types::OBJECT, {Types::OBJECT}),
                                                false);
}

ClassFileViewPtr buildDynamicClass()
{
    GenericClassBuilder builder;
    builder.version(JavaVersion::V17)
        .accessFlags(AccessFlag::ACC_PUBLIC | AccessFlag::ACC_SUPER)
        .type(instance("example/DynamicSubject"))
        .superClass(Types::OBJECT);

    MethodHandlePtr handle  = dynamicBootstrapHandle();
    auto            dynamic = std::make_shared<const ConstantDynamic>(
        ca::str::Utf8String("value"),
        Types::STRING,
        handle,
        std::vector<ConstantPtr>{std::make_shared<const OfInt>(7)});
    auto method = builder.putMethod(AccessFlag::ACC_PUBLIC | AccessFlag::ACC_STATIC,
                                    ca::str::Utf8String("dynamic"),
                                    Types::methodType(Types::VOID));
    method->code()
        .maxStack(1)
        .maxLocals(0)
        .codeList()
        .addInstruction(std::make_shared<DynamicConstantInstruction>(
            std::make_shared<const OfDynamic>(dynamic)))
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::POP))
        .addInstruction(std::make_shared<InvokeDynamicInstruction>(
            ca::str::Utf8String("call"),
            Types::methodType(Types::VOID),
            handle,
            std::vector<ConstantPtr>{std::make_shared<const OfInt>(9)}))
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::RETURN));
    return builder.build();
}

ClassFileViewPtr buildReferenceMergeClass()
{
    GenericClassBuilder builder;
    builder.version(JavaVersion::V17)
        .accessFlags(AccessFlag::ACC_PUBLIC | AccessFlag::ACC_SUPER)
        .type(instance("example/MergeSubject"))
        .superClass(Types::OBJECT);

    auto method          = builder.putMethod(AccessFlag::ACC_PUBLIC | AccessFlag::ACC_STATIC,
                                    ca::str::Utf8String("choose"),
                                    Types::methodType(Types::OBJECT, {Types::BOOLEAN}));
    auto right           = std::make_shared<GenericLabel>();
    auto join            = std::make_shared<GenericLabel>();
    auto leftType        = instance("example/Left");
    auto rightType       = instance("example/Right");
    auto constructorType = Types::methodType(Types::VOID);
    method->code()
        .maxStack(2)
        .maxLocals(1)
        .codeList()
        .addInstruction(std::make_shared<VarInstruction>(JavaOpcodes::ILOAD, 0))
        .addInstruction(std::make_shared<ConditionalJumpInstruction>(JavaOpcodes::IFEQ, right))
        .addInstruction(std::make_shared<AllocateInstruction>(leftType))
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::DUP))
        .addInstruction(std::make_shared<MethodInstruction>(JavaOpcodes::INVOKESPECIAL,
                                                            leftType,
                                                            ca::str::Utf8String("<init>"),
                                                            constructorType,
                                                            false))
        .addInstruction(std::make_shared<ImmediateJumpInstruction>(JavaOpcodes::GOTO, join))
        .addLabel(right)
        .addInstruction(std::make_shared<AllocateInstruction>(rightType))
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::DUP))
        .addInstruction(std::make_shared<MethodInstruction>(JavaOpcodes::INVOKESPECIAL,
                                                            rightType,
                                                            ca::str::Utf8String("<init>"),
                                                            constructorType,
                                                            false))
        .addLabel(join)
        .addInstruction(std::make_shared<SimpleInstruction>(JavaOpcodes::ARETURN));
    return builder.build();
}

class RecordingCommonSuperClassProvider final : public jbytecode::core::ICommonSuperClassProvider
{
public:
    ca::str::Utf8StringRef getCommonSuperClass(const ca::str::Utf8StringRef& first,
                                               const ca::str::Utf8StringRef& second) override
    {
        calls.emplace_back(first.to_std_string(), second.to_std_string());
        return arena.intern(ca::str::Utf8StringRef::from_cstr("example/Base"));
    }

    ca::str::Utf8StringArena                         arena;
    std::vector<std::pair<std::string, std::string>> calls;
};
}   //namespace

TEST(BlwBytecodeAdapterTest, ClassModelWritesReadsAndRewritesStructurally)
{
    ClassFileViewPtr          original   = buildStructuralClass();
    const std::vector<ca::u8> firstBytes = BytecodeLibrary::write(*original);
    ASSERT_FALSE(firstBytes.empty());
    ClassFileViewPtr first = BytecodeLibrary::read(firstBytes);

    EXPECT_EQ("example/AdapterSubject", first->type()->internalName().to_std_string());
    EXPECT_EQ(JavaVersion::V17, first->version());
    ASSERT_TRUE(first->constantPool());
    EXPECT_FALSE(first->constantPool()->indices().empty());
    ASSERT_EQ(1U, first->fields().size());
    ASSERT_TRUE(first->fields()[0]->defaultValue());
    EXPECT_EQ(42, static_cast<const OfInt&>(*first->fields()[0]->defaultValue()).value());
    ASSERT_EQ(3U, first->methods().size());
    ASSERT_TRUE(first->methods()[0]->code());
    EXPECT_GE(first->methods()[0]->code()->elements().size(), 7U);
    ASSERT_TRUE(first->methods()[1]->code());
    ASSERT_EQ(1U, first->methods()[1]->code()->tryCatchBlocks().size());
    EXPECT_EQ(1U, first->recordComponents().size());
    EXPECT_EQ(1U, first->innerClasses().size());
    EXPECT_EQ(1U, first->nestMembers().size());
    EXPECT_EQ(1U, first->permittedSubclasses().size());
    ASSERT_EQ(1U, first->visibleRuntimeAnnotations().size());
    EXPECT_EQ(
        "class",
        static_cast<const ElementString&>(*first->visibleRuntimeAnnotations()[0]
                                               ->get(ca::str::Utf8StringRef::from_cstr("value"))
                                               ->get())
            .value()
            .to_std_string());

    const std::vector<ca::u8> secondBytes = BytecodeLibrary::write(*first);
    ASSERT_FALSE(secondBytes.empty());
    ClassFileViewPtr second = BytecodeLibrary::read(secondBytes);
    EXPECT_EQ(first->type()->internalName().to_std_string(),
              second->type()->internalName().to_std_string());
    EXPECT_EQ(first->methods().size(), second->methods().size());
    EXPECT_EQ(first->fields().size(), second->fields().size());
    EXPECT_EQ(first->recordComponents().size(), second->recordComponents().size());

    const std::filesystem::path output = test::getOutPath("example/AdapterSubject.class");
    std::filesystem::create_directories(output.parent_path());
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.good());
    stream.write(reinterpret_cast<const char*>(secondBytes.data()),
                 static_cast<std::streamsize>(secondBytes.size()));
    ASSERT_TRUE(stream.good());
}

TEST(BlwBytecodeAdapterTest, ModuleStructureRoundTrips)
{
    GenericClassBuilder builder;
    builder.version(JavaVersion::V17)
        .accessFlags(AccessFlag::ACC_MODULE)
        .type(instance("module-info"));
    auto module =
        builder.addModule(ca::str::Utf8String("example.module"), 0, ca::str::Utf8String("1.0"));
    module->mainClass(ca::str::Utf8String("example/Main"))
        .package(ca::str::Utf8String("example"))
        .require(std::make_shared<const GenericModuleRequire>(
            AccessFlag::ACC_MANDATED, ca::str::Utf8String("java.base"), std::nullopt))
        .exportPackage(std::make_shared<const GenericModuleExport>(
            0, std::vector<ca::str::Utf8String>{}, ca::str::Utf8String("example")))
        .use(ca::str::Utf8String("example/Service"));

    const auto       bytes  = BytecodeLibrary::write(*builder.build());
    ClassFileViewPtr result = BytecodeLibrary::read(bytes);
    ASSERT_EQ(1U, result->modules().size());
    EXPECT_EQ("example.module", result->modules()[0]->name().to_std_string());
    EXPECT_EQ("example/Main", result->modules()[0]->mainClass()->to_std_string());
    EXPECT_EQ(1U, result->modules()[0]->requires().size());
    EXPECT_EQ(1U, result->modules()[0]->exports().size());
    EXPECT_EQ(1U, result->modules()[0]->uses().size());
}

TEST(BlwBytecodeAdapterTest, DynamicConstantsAndInvocationsRoundTrip)
{
    const auto       firstBytes = BytecodeLibrary::write(*buildDynamicClass());
    ClassFileViewPtr first      = BytecodeLibrary::read(firstBytes);
    ASSERT_TRUE(first->constantPool());

    bool hasDynamic       = false;
    bool hasInvokeDynamic = false;
    for (ca::i32 index : first->constantPool()->indices()) {
        const auto& entry = first->constantPool()->get(index);
        hasDynamic |= entry->getType() == jblw::constantpool::EntryKind::DYNAMIC;
        hasInvokeDynamic |= entry->getType() == jblw::constantpool::EntryKind::INVOKE_DYNAMIC;
    }
    EXPECT_TRUE(hasDynamic);
    EXPECT_TRUE(hasInvokeDynamic);

    ASSERT_EQ(1U, first->methods().size());
    ASSERT_TRUE(first->methods()[0]->code());
    bool hasDynamicInstruction       = false;
    bool hasInvokeDynamicInstruction = false;
    for (const auto& element : first->methods()[0]->code()->elements()) {
        hasDynamicInstruction |= element->getType() == CodeElementKind::CONSTANT_DYNAMIC;
        hasInvokeDynamicInstruction |= element->getType() == CodeElementKind::INVOKE_DYNAMIC;
    }
    EXPECT_TRUE(hasDynamicInstruction);
    EXPECT_TRUE(hasInvokeDynamicInstruction);

    const auto       secondBytes = BytecodeLibrary::write(*first);
    ClassFileViewPtr second      = BytecodeLibrary::read(secondBytes);
    ASSERT_EQ(1U, second->methods().size());
    EXPECT_EQ(first->methods()[0]->code()->elements().size(),
              second->methods()[0]->code()->elements().size());
}

TEST(BlwBytecodeAdapterTest, CustomCommonSuperClassProviderParticipatesInFrameWriting)
{
    RecordingCommonSuperClassProvider provider;
    const auto bytes = BytecodeLibrary::write(*buildReferenceMergeClass(), provider);

    ASSERT_FALSE(bytes.empty());
    ASSERT_FALSE(provider.calls.empty());
    const auto& [first, second] = provider.calls.front();
    EXPECT_TRUE((first == "example/Left" && second == "example/Right") ||
                (first == "example/Right" && second == "example/Left"));
    EXPECT_NO_THROW(BytecodeLibrary::read(bytes));
}
