#include "jhasm/ast/specific.hpp"
#include "jhasm/parser/ast_processor.hpp"
#include "jhasm/parser/declaration_parser.hpp"
#include "jhasm/parser/tokenizer.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace jhasm::ast;
using namespace jhasm::parser;

namespace {
ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

jhasm::error::Result<std::vector<ElementPtr>> process(const std::string& source)
{
    const auto name   = utf16("<stdin>");
    const auto input  = utf16(source);
    auto       tokens = Tokenizer{}.tokenize(name.ref(), input.ref());
    EXPECT_TRUE(tokens.has_value());
    EXPECT_FALSE(tokens.has_err());
    if (!tokens.get()) return jhasm::error::Result<std::vector<ElementPtr>>::err(tokens.errors());
    auto declarations = DeclarationParser{}.parseAny(*tokens.get());
    for (const auto& diagnostic : declarations.errors()) {
        ADD_FAILURE() << diagnostic.to_string().to_utf8_string().to_std_string();
    }
    return ASTProcessor{}.process(declarations.get());
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}
}   //namespace

TEST(JhasmASTProcessorTest, ProcessesClassFieldAndMethod)
{
    auto result = process(".super java/lang/Object\n"
                          ".class public super HelloWorld {\n"
                          "  .field public static final answer I {value: 42}\n"
                          "  .method public static main ([Ljava/lang/String;)V {\n"
                          "    parameters: {args}, code: { return }\n"
                          "  }\n"
                          "}\n");
    ASSERT_FALSE(result.has_err());
    ASSERT_NE(nullptr, result.get());
    ASSERT_EQ(1U, result.get()->size());
    auto clazz = elementAs<Class>(result.get()->front(), ElementKind::CLASS);
    ASSERT_NE(nullptr, clazz);
    EXPECT_EQ("HelloWorld", clazz->name()->content().to_utf8_string().to_std_string());
    ASSERT_NE(nullptr, clazz->superName());
    EXPECT_EQ("java/lang/Object", clazz->superName()->content().to_utf8_string().to_std_string());
    ASSERT_EQ(2U, clazz->contents().size());

    auto field = elementAs<Field>(clazz->contents()[0], ElementKind::FIELD);
    ASSERT_NE(nullptr, field);
    EXPECT_TRUE(field->modifiers().has(utf16("static").ref()));
    EXPECT_EQ("I", field->descriptor()->content().to_utf8_string().to_std_string());
    ASSERT_NE(nullptr, field->fieldValue());

    auto method = elementAs<Method>(clazz->contents()[1], ElementKind::METHOD);
    ASSERT_NE(nullptr, method);
    ASSERT_EQ(1U, method->parameters().size());
    ASSERT_NE(nullptr, method->code());
    ASSERT_EQ(1U, method->code()->instructions().size());
}

TEST(JhasmASTProcessorTest, ProcessesNestedAnnotationValues)
{
    auto result = process(".annotation java/lang/annotation/Retention {\n"
                          "  value: .enum java/lang/annotation/RetentionPolicy RUNTIME\n"
                          "}\n");
    ASSERT_FALSE(result.has_err());
    ASSERT_NE(nullptr, result.get());
    ASSERT_EQ(1U, result.get()->size());
    auto annotation = elementAs<Annotation>(result.get()->front(), ElementKind::ANNOTATION);
    ASSERT_NE(nullptr, annotation);
    auto enumValue =
        elementAs<EnumValue>(annotation->value(utf16("value").ref()), ElementKind::ENUM_VALUE);
    ASSERT_NE(nullptr, enumValue);
    EXPECT_EQ("RUNTIME", enumValue->enumValue()->content().to_utf8_string().to_std_string());
}

TEST(JhasmASTProcessorTest, ReportsInvalidModifiersAndInstructions)
{
    auto modifier = process(".field privlick name I\n");
    EXPECT_TRUE(modifier.has_err());

    auto instruction =
        process(".class Example { .method run ()V { code: { definitely_not_an_opcode } } }\n");
    EXPECT_TRUE(instruction.has_err());
}

TEST(JhasmASTProcessorTest, ProcessesAllUpstreamLegalJvmFixtures)
{
    const std::filesystem::path root = "libs/libjhasm/test_resource/jasm";
    ASSERT_TRUE(std::filesystem::exists(root));
    ca::usize count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".jasm") continue;
        SCOPED_TRACE(entry.path().filename().string());
        auto result = process(readFile(entry.path()));
        EXPECT_FALSE(result.has_err());
        ASSERT_NE(nullptr, result.get());
        EXPECT_FALSE(result.get()->empty());
        ++count;
    }
    EXPECT_EQ(44U, count);
}
