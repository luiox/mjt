#include "jhasm/compile/jvm_compiler.hpp"
#include "jhasm/parser/ast_processor.hpp"
#include "jhasm/parser/declaration_parser.hpp"
#include "jhasm/parser/tokenizer.hpp"

#include <jbc/core/reader.hpp>

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
    // 产物回读验证（B7：libjblw 退役，readClass 同位替入）。unwrap()&& 按
    // 值返回，先落命名局部再取视图（模型随临时量析构会带走字节视图）。
    auto reread = jbc::core::readClass(jbc::core::ByteView{result.get()->bytes});
    ASSERT_TRUE(reread.is_ok());
    auto opened = std::move(reread).unwrap();
    EXPECT_EQ("Example", std::string(opened.file->thisName()));
    EXPECT_EQ(1U, opened.file->methods.size());
}

TEST(JhasmJvmCompilerTest, CompilesAllUpstreamLegalJvmFixtures)
{
    const std::filesystem::path root  = "unittest/jhasm/test_resource/jasm";
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
        EXPECT_TRUE(jbc::core::readClass(jbc::core::ByteView{result.get()->bytes}).is_ok());
        ++count;
    }
    EXPECT_EQ(44U, count);
}
