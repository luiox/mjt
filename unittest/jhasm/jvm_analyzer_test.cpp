#include "jhasm/analysis/jvm_analyzer.hpp"
#include "jhasm/compile/jvm_compiler.hpp"
#include "jhasm/parser/ast_processor.hpp"
#include "jhasm/parser/declaration_parser.hpp"
#include "jhasm/parser/tokenizer.hpp"

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
    auto name   = utf16("<analysis>");
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

TEST(JhasmJvmAnalyzerTest, ReturnsOwningFramesForValidMethod)
{
    auto compiled = compile(
        ".super java/lang/Object\n"
        ".class public super Example {\n"
        "  .method public static value (I)I { code: { iload 0\n iconst_1\n iadd\n ireturn } }\n"
        "}\n");
    ASSERT_TRUE(compiled.is_ok());
    auto analysis = jhasm::analysis::JvmAnalyzer{}.analyze(compiled.get()->bytes);
    ASSERT_TRUE(analysis.is_ok())
        << (analysis.errors().empty()
                ? ""
                : analysis.errors()[0].to_string().to_utf8_string().to_std_string());
    ASSERT_EQ(1U, analysis.get()->methods.size());
    const auto& frames = analysis.get()->methods[0].frames;
    ASSERT_GE(frames.size(), 4U);
    ASSERT_TRUE(frames[0].has_value());
    ASSERT_FALSE(frames[0]->locals.empty());
    EXPECT_EQ("I", frames[0]->locals[0].descriptor().to_std_string());
}

TEST(JhasmJvmAnalyzerTest, KeepsArrayParameterDescriptorsAliveInFrames)
{
    auto compiled = compile(".super java/lang/Object\n"
                            ".class public super Example {\n"
                            "  .method public static main ([Ljava/lang/String;)V {\n"
                            "    code: { aload 0\n pop\n return }\n"
                            "  }\n"
                            "}\n");
    ASSERT_TRUE(compiled.is_ok());
    auto analysis = jhasm::analysis::JvmAnalyzer{}.analyze(compiled.get()->bytes);
    ASSERT_TRUE(analysis.is_ok())
        << (analysis.errors().empty()
                ? ""
                : analysis.errors()[0].to_string().to_utf8_string().to_std_string());
    ASSERT_EQ(1U, analysis.get()->methods.size());
    const auto& frames = analysis.get()->methods[0].frames;
    ASSERT_FALSE(frames.empty());
    ASSERT_TRUE(frames[0].has_value());
    ASSERT_EQ(1U, frames[0]->locals.size());
    EXPECT_EQ("[Ljava/lang/String;", frames[0]->locals[0].descriptor().to_std_string());
}

TEST(JhasmJvmAnalyzerTest, RejectsAllUpstreamIllegalJvmFixtures)
{
    const std::filesystem::path root     = "unittest/jhasm/test_resource/jasm-illegal";
    ca::usize                   count    = 0;
    ca::usize                   rejected = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".jasm") continue;
        SCOPED_TRACE(entry.path().filename().string());
        auto compiled = compile(readFile(entry.path()));
        ASSERT_TRUE(compiled.has_value());
        auto analysis = jhasm::analysis::JvmAnalyzer{}.analyze(compiled.get()->bytes);
        if (analysis.has_err())
            ++rejected;
        else
            ADD_FAILURE() << "No analysis diagnostic for " << entry.path().filename().string();
        ++count;
    }
    EXPECT_EQ(44U, count);
    EXPECT_EQ(count, rejected);
}
