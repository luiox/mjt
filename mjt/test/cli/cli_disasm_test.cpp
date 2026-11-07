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

using CapturedSink = cli_fixture::CapturedSink;

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

TEST(MjtCliDisasmTest, UnparsableClassBytesIsRuntimeWithInfoFailureMessage)
{
    // 魔数合法 + 载荷缺失：容器按 ClassResource 登记（JarUtil 只查 4 字节
    // 魔数），URI 形选择器直命中容器键，jbc 会话解析失败 → DataLoss（非
    // NotFound）→ class_info_failed 文案 + Runtime；区别于类缺失的用法 1。
    mjbase::model::ResourceContainer container;
    container.addResource("fixture/Stub.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Stub.class",
                              std::vector<ca::u8>{0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0x00}));
    const auto path = test::getTempOutPath("mjt_cli_disasm_unparsable_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());

    auto report = collectDisasm(path.string(), "fixture/Stub.class");
    ASSERT_TRUE(report.is_err());
    auto error = std::move(report).unwrap_err();
    EXPECT_EQ(error.kind, mjt::cli::DisasmError::Kind::Runtime);
    EXPECT_NE(error.message.find("cannot read class info"), std::string::npos);
}

TEST(MjtCliDisasmTest, RuntimeWithFirstErrorDiagnosticWhenDisassemblyFails)
{
    // 语料：合法类字节 + LineNumberTable 载荷行数计数改写为越界值。jasm
    // 源带 line 指令——code_compiler 只在行表非空时写 LNT。workspace 侧
    // classInfo 对 LNT 解码失败容错跳过（照常成功），jasm 打印机 findAttr
    // 按名强取 LNT、解码失败抛异常 → getJasm err——打到「反汇编失败退出
    // 码 2 + 诊断取首条 Error」聚合分支（getJasm 失败面此前零覆盖）。
    const auto bytes = cli_fixture::withCorruptAttribute(
        cli_fixture::assembleClass(
            R"JASM(.super java/lang/Object
.class public super fixture/DisasmFail {
  .method public static act (I)I {
    code: {
    L0:
      line 10
      iload 0
      line 11
      ifeq L1
      iconst_1
      ireturn
    L1:
      iconst_2
      ireturn
    }
  }
}
)JASM"),
        "LineNumberTable");
    mjbase::model::ResourceContainer container;
    container.addResource("fixture/DisasmFail.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/DisasmFail.class", bytes));
    const auto path = test::getTempOutPath("mjt_cli_disasm_corrupt_lnt_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());

    auto report = collectDisasm(path.string(), "fixture/DisasmFail");
    ASSERT_TRUE(report.is_err());
    auto error = std::move(report).unwrap_err();
    EXPECT_EQ(error.kind, mjt::cli::DisasmError::Kind::Runtime);
    // disasm_failed 文案 + 首条 Error 诊断非空（聚合逻辑的真断言）。
    EXPECT_NE(error.message.find("disassembly failed: "), std::string::npos);
    EXPECT_GT(error.message.size(), std::string("disassembly failed: ").size());
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
