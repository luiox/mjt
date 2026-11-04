/// @file        cli_disasm_test.cpp
/// @brief       `mjt disasm` 核心面测试：单类 jasm 文本（指令形状）/ 类选择器
///              无命中分支 / -o 落盘与 stdout 同文 / 渲染确定性。
#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "cli_fixture.hpp"
#include "commands.hpp"
#include "shared/output_sink.hpp"
#include "test_helper.hpp"

namespace {

using mjt::cli::collectDisasm;

/// OutputSink 捕获（FILE* 往返；strings 测试同款构造）。
class CapturedSink
{
public:
    CapturedSink()
        : file_(std::tmpfile())
        , sink_(file_)
    {}
    ~CapturedSink()
    {
        if (file_ != nullptr) std::fclose(file_);
    }
    CapturedSink(const CapturedSink&)            = delete;
    CapturedSink& operator=(const CapturedSink&) = delete;

    mjt::OutputSink& sink() { return sink_; }

    std::string text()
    {
        std::fflush(file_);
        std::rewind(file_);
        std::string out;
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file_)) > 0) out.append(buffer, read);
        return out;
    }

private:
    std::FILE*      file_ = nullptr;
    mjt::OutputSink sink_;
};

}   // namespace

TEST(MjtCliDisasmTest, EmitsJasmTextWithExpectedInstructionShapes)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectDisasm(jar.string(), "fixture/Alpha");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    // 回显契约：请求类名原样。
    EXPECT_EQ(info.internalName, "fixture/Alpha");
    EXPECT_EQ(info.jarPath, jar.string());

    // 指令形状：方法体常量加载 + 运算 + 返回的规范化 jasm 文本可断言。
    EXPECT_NE(info.jasm.find("fixture/Alpha"), std::string::npos);
    EXPECT_NE(info.jasm.find("act"), std::string::npos);
    EXPECT_NE(info.jasm.find("(I)I"), std::string::npos);
    EXPECT_NE(info.jasm.find("iload_0"), std::string::npos);
    EXPECT_NE(info.jasm.find("iconst_2"), std::string::npos);
    EXPECT_NE(info.jasm.find("imul"), std::string::npos);
    EXPECT_NE(info.jasm.find("ireturn"), std::string::npos);
    // 字段声明在 jasm 全文中可达。
    EXPECT_NE(info.jasm.find("state"), std::string::npos);

    // 规范化产物可回汇编（jasm 面自洽；assemble-disassemble 闭环）。
    auto reparsed = cli_fixture::assembleClass(info.jasm);
    EXPECT_FALSE(reparsed.empty());
}

TEST(MjtCliDisasmTest, InterfaceClassDisassemblesWithoutCodeBody)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectDisasm(jar.string(), "fixture/Api");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();
    EXPECT_NE(info.jasm.find("fixture/Api"), std::string::npos);
    // 抽象方法无 code 块，jasm 文本仍带方法声明。
    EXPECT_NE(info.jasm.find("act"), std::string::npos);
    EXPECT_NE(info.jasm.find("(I)V"), std::string::npos);
}

TEST(MjtCliDisasmTest, UnknownClassIsUsageError)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectDisasm(jar.string(), "fixture/NoSuch");
    ASSERT_TRUE(report.is_err());
    EXPECT_EQ(report.unwrap_err().kind, mjt::cli::DisasmError::Kind::ClassNotFound);
}

TEST(MjtCliDisasmTest, UnopenableJarIsRuntimeError)
{
    auto report = collectDisasm("mjt_cli_disasm_missing.jar", "fixture/Alpha");
    ASSERT_TRUE(report.is_err());
    EXPECT_EQ(report.unwrap_err().kind, mjt::cli::DisasmError::Kind::Runtime);
}

TEST(MjtCliDisasmTest, OutputFileMatchesStdoutText)
{
    const auto jar    = cli_fixture::writeClassesJar();
    auto       report = collectDisasm(jar.string(), "fixture/Alpha");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    CapturedSink capture;
    mjt::cli::renderDisasmText(info, capture.sink());
    const auto text = capture.text();
    // stdout 渲染 = jasm 全文原样（无头部摘要混入，重定向取文件语义）。
    EXPECT_EQ(text, info.jasm);

    // -o 落盘同文（main.cpp 以同一渲染函数写文件）。
    const auto outPath = test::getTempOutPath("mjt_cli_disasm_out.jasm");
    std::FILE* file    = std::fopen(outPath.string().c_str(), "wb");
    ASSERT_NE(file, nullptr);
    {
        mjt::OutputSink sink(file);
        mjt::cli::renderDisasmText(info, sink);
    }
    std::fclose(file);
    EXPECT_EQ(cli_fixture::readFileText(outPath), info.jasm);
    std::error_code ec;
    std::filesystem::remove(outPath, ec);
}
