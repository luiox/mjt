#include "jhasm/jhasm.hpp"

#include <jblw/asm/bytecode_library.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {
class RecordingInheritanceChecker final : public jhasm::analysis::InheritanceChecker
{
public:
    bool isSubclassOf(ca::str::Utf8StringRef child, ca::str::Utf8StringRef parent) const override
    {
        return child == parent;
    }

    ca::str::Utf8String commonSuperclass(ca::str::Utf8StringRef,
                                         ca::str::Utf8StringRef) const override
    {
        ++calls;
        return ca::str::Utf8String("example/Base");
    }

    mutable int calls = 0;
};
}   //namespace

TEST(LibJhasmEntryTest, ReportsPinnedUpstreamRevision)
{
    EXPECT_EQ(std::string_view(jhasm::get_upstream_revision()),
              "aacebfa8b06a5c478adf13d9ce8e0e9f896ba840");
    EXPECT_FALSE(std::string_view(jhasm::get_lib_version()).empty());
}

TEST(LibJhasmEntryTest, RunsPublicJvmPipeline)
{
    auto sourceName = ca::str::Utf16String::value_of("entry.jasm");
    auto source     = ca::str::Utf16String::value_of(
        ".super java/lang/Object\n"
            ".class public super Entry {\n"
            "  .method public static value ()I { code: { bipush 7\n ireturn } }\n"
            "}\n");
    auto compiled = jhasm::assemble(sourceName.ref(), source.ref());
    ASSERT_TRUE(compiled.is_ok());
    auto analysis = jhasm::analyze(compiled.get()->bytes);
    ASSERT_TRUE(analysis.is_ok());
    ASSERT_EQ(1U, analysis.get()->methods.size());
    auto printed = jhasm::disassemble(compiled.get()->bytes);
    ASSERT_TRUE(printed.is_ok());
    EXPECT_NE(std::string::npos,
              printed.get()->to_utf8_string().to_std_string().find(".class public super Entry"));
}

TEST(LibJhasmEntryTest, DisassemblesOneMethodByExactSelector)
{
    auto sourceName = ca::str::Utf16String::value_of("method.jasm");
    auto source     = ca::str::Utf16String::value_of(
        ".super java/lang/Object\n"
        ".class public super MethodSubject {\n"
        "  .method public static value ()I { code: { bipush 7\n ireturn } }\n"
        "  .method public static value (I)I { code: { iload 0\n ireturn } }\n"
        "}\n");
    auto compiled = jhasm::assemble(sourceName.ref(), source.ref());
    ASSERT_TRUE(compiled.is_ok());

    const auto name = ca::str::Utf8StringRef::from_cstr("value");
    const auto desc = ca::str::Utf8StringRef::from_cstr("(I)I");
    auto method = jhasm::disassembleMethod(compiled.get()->bytes, name, desc);

    ASSERT_TRUE(method.is_ok());
    const auto text = method.get()->to_utf8_string().to_std_string();
    EXPECT_NE(std::string::npos, text.find(".method public static value (I)I"));
    EXPECT_EQ(std::string::npos, text.find("bipush 7"));

    auto missing = jhasm::disassembleMethod(
        compiled.get()->bytes, name, ca::str::Utf8StringRef::from_cstr("()V"));
    EXPECT_FALSE(missing.is_ok());
}

TEST(LibJhasmEntryTest, CompilerUsesInjectedInheritanceForFrameWriting)
{
    auto sourceName = ca::str::Utf16String::value_of("merge.jasm");
    auto source =
        ca::str::Utf16String::value_of(".super java/lang/Object\n"
                                       ".class public super MergeSubject {\n"
                                       "  .method public static choose (Z)Ljava/lang/Object; {\n"
                                       "    parameters: { flag },\n"
                                       "    code: {\n"
                                       "      iload flag\n"
                                       "      ifeq right\n"
                                       "      new example/Left\n"
                                       "      dup\n"
                                       "      invokespecial example/Left.<init> ()V\n"
                                       "      goto join\n"
                                       "    right:\n"
                                       "      new example/Right\n"
                                       "      dup\n"
                                       "      invokespecial example/Right.<init> ()V\n"
                                       "    join:\n"
                                       "      areturn\n"
                                       "    }\n"
                                       "  }\n"
                                       "}\n");
    RecordingInheritanceChecker        checker;
    jhasm::compile::JvmCompilerOptions options;
    options.inheritance = &checker;

    auto compiled = jhasm::assemble(sourceName.ref(), source.ref(), options);
    ASSERT_TRUE(compiled.is_ok())
        << (compiled.errors().empty()
                ? ""
                : compiled.errors()[0].to_string().to_utf8_string().to_std_string());
    EXPECT_GT(checker.calls, 0);
    EXPECT_NE(nullptr, jblw::adapter::BytecodeLibrary::read(compiled.get()->bytes));
}
