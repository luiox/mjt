#include "jhasm/ast/primitive.hpp"
#include "jhasm/parser/declaration_parser.hpp"
#include "jhasm/parser/tokenizer.hpp"

#include <gtest/gtest.h>

using namespace jhasm::ast;
using namespace jhasm::parser;

namespace {
ca::str::Utf16String utf16(const char* value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_cstr(value));
}

ParsingResult parse(const char* source)
{
    const auto input = utf16(source);
    const auto name  = utf16("<stdin>");
    Tokenizer  tokenizer;
    auto       tokens = tokenizer.tokenize(name.ref(), input.ref());
    EXPECT_FALSE(tokens.has_err());
    EXPECT_TRUE(tokens.has_value());
    return DeclarationParser().parseAny(*tokens.get());
}
}   //namespace

TEST(JhasmDeclarationParserTest, ParsesPrimitiveObjectAndArrayValues)
{
    ParsingResult result = parse("10 0x10 test \"value\" {key: {1, two, \"three\"}}");
    ASSERT_FALSE(result.has_err());
    ASSERT_EQ(5U, result.get().size());
    EXPECT_EQ(ElementKind::NUMBER, result.get()[0]->getType());
    EXPECT_EQ(ElementKind::NUMBER, result.get()[1]->getType());
    EXPECT_EQ(ElementKind::IDENTIFIER, result.get()[2]->getType());
    EXPECT_EQ(ElementKind::STRING, result.get()[3]->getType());

    auto object = elementAs<Object>(result.get()[4], ElementKind::OBJECT);
    ASSERT_TRUE(object);
    auto array = elementAs<Array>(object->value(utf16("key").ref()), ElementKind::ARRAY);
    ASSERT_TRUE(array);
    EXPECT_EQ(3U, array->values().size());
}

TEST(JhasmDeclarationParserTest, ParsesNestedClassDeclarations)
{
    ParsingResult result =
        parse(".class public final HelloWorld { .field public name Ljava/lang/String; "
              ".field public output I }");
    ASSERT_FALSE(result.has_err());
    ASSERT_EQ(1U, result.get().size());
    auto declaration = elementAs<Declaration>(result.get()[0], ElementKind::DECLARATION);
    ASSERT_TRUE(declaration);
    ASSERT_EQ(4U, declaration->elements().size());
    auto body = elementAs<Declaration>(declaration->element(3), ElementKind::DECLARATION);
    ASSERT_TRUE(body);
    ASSERT_EQ(2U, body->elements().size());
    auto field = elementAs<Declaration>(body->element(0), ElementKind::DECLARATION);
    ASSERT_TRUE(field);
    EXPECT_EQ(".field", field->keyword()->content().to_utf8_string().to_std_string());
}

TEST(JhasmDeclarationParserTest, ParsesCodeLabelsAndInstructionArgumentsByLine)
{
    ParsingResult result = parse(".method public static main ()V { parameters: {}, code: {\n"
                                 "A:\n"
                                 "  bipush 10\n"
                                 "  istore value\n"
                                 "  return\n"
                                 "B:\n"
                                 "} }");
    ASSERT_FALSE(result.has_err());
    auto method = elementAs<Declaration>(result.get()[0], ElementKind::DECLARATION);
    ASSERT_TRUE(method);
    auto attributes = elementAs<Object>(method->elements().back(), ElementKind::OBJECT);
    ASSERT_TRUE(attributes);
    auto code = elementAs<Code>(attributes->value(utf16("code").ref()), ElementKind::CODE);
    ASSERT_TRUE(code);
    ASSERT_EQ(5U, code->instructions().size());
    EXPECT_EQ(ElementKind::LABEL, code->instructions()[0]->getType());
    EXPECT_EQ(ElementKind::INSTRUCTION, code->instructions()[1]->getType());
    ASSERT_EQ(1U, code->instructions()[1]->arguments().size());
    EXPECT_EQ(ElementKind::NUMBER, code->instructions()[1]->arguments()[0]->getType());
}
