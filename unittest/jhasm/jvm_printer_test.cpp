#include "jhasm/compile/jvm_compiler.hpp"
#include "jhasm/parser/ast_processor.hpp"
#include "jhasm/parser/declaration_parser.hpp"
#include "jhasm/parser/tokenizer.hpp"
#include "jhasm/printer/jvm_printer.hpp"

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
    const std::filesystem::path root  = "unittest/jhasm/test_resource/jasm";
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

        // 0.3.x JvmCompileResult 只剩 bytes（libjblw 模型视图退役）：roundtrip
        // 对读经 libjbc readClass 重建。unwrap()&& 按值返回，命名局部保寿命
        // （字节视图随模型走，先拷出再断言）。
        auto leftRead  = jbc::core::readClass(jbc::core::ByteView{first.get()->bytes});
        auto rightRead = jbc::core::readClass(jbc::core::ByteView{second.get()->bytes});
        ASSERT_TRUE(leftRead.is_ok());
        ASSERT_TRUE(rightRead.is_ok());
        auto left  = std::move(leftRead).unwrap();
        auto right = std::move(rightRead).unwrap();
        EXPECT_EQ(std::string(left.file->thisName()), std::string(right.file->thisName()));
        ASSERT_EQ(left.file->fields.size(), right.file->fields.size());
        ASSERT_EQ(left.file->methods.size(), right.file->methods.size());
        EXPECT_EQ(left.file->findClassAttr("RuntimeVisibleAnnotations") != nullptr,
                  right.file->findClassAttr("RuntimeVisibleAnnotations") != nullptr);
        EXPECT_EQ(left.file->findClassAttr("RuntimeInvisibleAnnotations") != nullptr,
                  right.file->findClassAttr("RuntimeInvisibleAnnotations") != nullptr);
        for (ca::usize index = 0; index < left.file->fields.size(); ++index) {
            EXPECT_EQ(std::string(left.file->nameOf(left.file->fields[index])),
                      std::string(right.file->nameOf(right.file->fields[index])));
            EXPECT_EQ(std::string(left.file->descOf(left.file->fields[index])),
                      std::string(right.file->descOf(right.file->fields[index])));
        }
        for (ca::usize index = 0; index < left.file->methods.size(); ++index) {
            EXPECT_EQ(std::string(left.file->nameOf(left.file->methods[index])),
                      std::string(right.file->nameOf(right.file->methods[index])));
            EXPECT_EQ(std::string(left.file->descOf(left.file->methods[index])),
                      std::string(right.file->descOf(right.file->methods[index])));
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
    EXPECT_TRUE(jbc::core::readClass(jbc::core::ByteView{second.get()->bytes}).is_ok());
}
