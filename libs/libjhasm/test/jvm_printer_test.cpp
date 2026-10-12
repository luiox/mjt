#include "jhasm/compile/jvm_compiler.hpp"
#include "jhasm/parser/ast_processor.hpp"
#include "jhasm/parser/declaration_parser.hpp"
#include "jhasm/parser/tokenizer.hpp"
#include "jhasm/printer/jvm_printer.hpp"

#include <jblw/asm/bytecode_library.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

jhasm::error::Result<jhasm::compile::JvmCompileResult> compile(const std::string& source)
{
    auto name   = utf16("<printer>");
    auto input  = utf16(source);
    auto tokens = jhasm::parser::Tokenizer{}.tokenize(name.ref(), input.ref());
    if (!tokens.is_ok())
        return jhasm::error::Result<jhasm::compile::JvmCompileResult>::err(tokens.errors());
    auto declarations = jhasm::parser::DeclarationParser{}.parseAny(*tokens.get());
    if (declarations.has_err())
        return jhasm::error::Result<jhasm::compile::JvmCompileResult>::err(declarations.errors());
    auto processed = jhasm::parser::ASTProcessor{}.process(declarations.get());
    if (!processed.is_ok())
        return jhasm::error::Result<jhasm::compile::JvmCompileResult>::err(processed.errors());
    return jhasm::compile::JvmCompiler{}.compile(*processed.get());
}

std::string firstError(const std::vector<jhasm::error::Error>& errors)
{
    return errors.empty() ? std::string{} : errors[0].to_string().to_utf8_string().to_std_string();
}
}   //namespace

TEST(JhasmJvmPrinterTest, RoundTripsAllUpstreamLegalJvmFixtures)
{
    const std::filesystem::path root  = "libs/libjhasm/test_resource/jasm";
    ca::usize                   count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".jasm") continue;
        SCOPED_TRACE(entry.path().filename().string());
        auto first = compile(readFile(entry.path()));
        ASSERT_TRUE(first.is_ok()) << firstError(first.errors());
        auto printed = jhasm::printer::JvmPrinter{}.print(first.get()->bytes);
        ASSERT_TRUE(printed.is_ok()) << firstError(printed.errors());
        const auto normalized = printed.get()->to_utf8_string().to_std_string();
        auto       second     = compile(normalized);
        ASSERT_TRUE(second.is_ok()) << firstError(second.errors()) << "\n" << normalized;

        const auto& left  = first.get()->classFile;
        const auto& right = second.get()->classFile;
        EXPECT_EQ(left->type()->internalName(), right->type()->internalName());
        ASSERT_EQ(left->fields().size(), right->fields().size());
        ASSERT_EQ(left->methods().size(), right->methods().size());
        EXPECT_EQ(left->visibleRuntimeAnnotations().size(),
                  right->visibleRuntimeAnnotations().size());
        EXPECT_EQ(left->invisibleRuntimeAnnotations().size(),
                  right->invisibleRuntimeAnnotations().size());
        for (ca::usize index = 0; index < left->fields().size(); ++index) {
            EXPECT_EQ(left->fields()[index]->name(), right->fields()[index]->name());
            EXPECT_EQ(left->fields()[index]->type()->descriptor(),
                      right->fields()[index]->type()->descriptor());
        }
        for (ca::usize index = 0; index < left->methods().size(); ++index) {
            EXPECT_EQ(left->methods()[index]->name(), right->methods()[index]->name());
            EXPECT_EQ(left->methods()[index]->type()->descriptor(),
                      right->methods()[index]->type()->descriptor());
        }
        ++count;
    }
    EXPECT_EQ(44U, count);
}

TEST(JhasmJvmPrinterTest, ReattachesLineNumbersToTheirOriginalLabels)
{
    auto first = compile(".super java/lang/Object\n"
                         ".class public super LineNumbers {\n"
                         "  .method public static value ()I { code: {\n"
                         "    L0:\n"
                         "    line 10\n"
                         "    bipush 41\n"
                         "    L1:\n"
                         "    line 11\n"
                         "    ireturn\n"
                         "  } }\n"
                         "}\n");
    ASSERT_TRUE(first.is_ok()) << firstError(first.errors());

    auto printed = jhasm::printer::JvmPrinter{}.print(first.get()->bytes);
    ASSERT_TRUE(printed.is_ok()) << firstError(printed.errors());
    const auto source = printed.get()->to_utf8_string().to_std_string();
    EXPECT_NE(std::string::npos, source.find("line 10\n            ldc 41"));
    EXPECT_NE(std::string::npos, source.find("line 11\n            ireturn"));

    auto second = compile(source);
    ASSERT_TRUE(second.is_ok()) << firstError(second.errors()) << "\n" << source;
    EXPECT_NE(nullptr, jblw::adapter::BytecodeLibrary::read(second.get()->bytes));
}
