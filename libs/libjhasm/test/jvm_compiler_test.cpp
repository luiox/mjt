#include "jhasm/compile/jvm_compiler.hpp"
#include "jhasm/parser/ast_processor.hpp"
#include "jhasm/parser/declaration_parser.hpp"
#include "jhasm/parser/tokenizer.hpp"

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
    auto name   = utf16("<stdin>");
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
}   //namespace

TEST(JhasmJvmCompilerTest, WritesReadableClassBytes)
{
    auto result = compile(".super java/lang/Object\n"
                          ".class public super Example {\n"
                          "  .method public static value ()I { code: { bipush 42\n ireturn } }\n"
                          "}\n");
    ASSERT_TRUE(result.is_ok())
        << (result.errors().empty()
                ? ""
                : result.errors()[0].to_string().to_utf8_string().to_std_string());
    ASSERT_NE(nullptr, result.get());
    EXPECT_GT(result.get()->bytes.size(), 4U);
    auto reread = jblw::adapter::BytecodeLibrary::read(result.get()->bytes);
    ASSERT_NE(nullptr, reread);
    EXPECT_EQ("Example", reread->type()->internalName().to_std_string());
    EXPECT_EQ(1U, reread->methods().size());
}

TEST(JhasmJvmCompilerTest, CompilesAllUpstreamLegalJvmFixtures)
{
    const std::filesystem::path root  = "libs/libjhasm/test_resource/jasm";
    ca::usize                   count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".jasm") continue;
        SCOPED_TRACE(entry.path().filename().string());
        auto result = compile(readFile(entry.path()));
        if (!result.errors().empty())
            ADD_FAILURE() << result.errors()[0].to_string().to_utf8_string().to_std_string();
        ASSERT_TRUE(result.is_ok());
        ASSERT_NE(nullptr, result.get());
        EXPECT_GT(result.get()->bytes.size(), 4U);
        EXPECT_NE(nullptr, jblw::adapter::BytecodeLibrary::read(result.get()->bytes));
        ++count;
    }
    EXPECT_EQ(44U, count);
}
