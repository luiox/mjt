/// @file        cli_eval_test.cpp
/// @brief       `mjt eval` 核心测试：优先级求值、变量绑定、unbound 的 S-表达式通道、-s 化简恒等式、绑定串与解析错误、渲染确定性。
#include <gtest/gtest.h>

#include <string>

#include "commands.hpp"
#include "cli_fixture.hpp"
#include "shared/output_sink.hpp"

namespace {

using mjt::cli::evaluateExpression;
using mjt::cli::EvalError;
using mjt::cli::EvalOutcome;

using CapturedSink = cli_fixture::CapturedSink;

/// 求值便捷入口：断言成功并返回 outcome。
EvalOutcome evalOk(const std::string& expr, const std::string& vars = "", bool simplify = false)
{
    auto outcome = evaluateExpression(expr, vars, simplify);
    if (!outcome.is_ok()) {
        const auto message = std::move(outcome).unwrap_err().message;
        ADD_FAILURE() << expr << " unexpectedly failed: " << message;
        return EvalOutcome{};
    }
    return std::move(outcome).unwrap();
}

}   // namespace

TEST(MjtCliEvalTest, EvaluatesArithmeticWithPrecedence)
{
    const auto outcome = evalOk("1 + 2 * 3");
    EXPECT_TRUE(outcome.evaluated);
    EXPECT_EQ(outcome.value, 7);
}

TEST(MjtCliEvalTest, EvaluatesHexLiteralsAndShifts)
{
    EXPECT_EQ(evalOk("0xFF").value, 255);
    EXPECT_EQ(evalOk("1 << 4").value, 16);
    EXPECT_EQ(evalOk("(0xF0 & 0x3C) | 0x0F").value, 0x3F);
}

TEST(MjtCliEvalTest, VarBindingsSubstitute)
{
    const auto outcome = evalOk("x + y", "x=10,y=20");
    EXPECT_TRUE(outcome.evaluated);
    EXPECT_EQ(outcome.value, 30);
}

TEST(MjtCliEvalTest, UnboundPrintsSExprAndCountsAsSuccess)
{
    const auto outcome = evalOk("x + 0");
    EXPECT_FALSE(outcome.evaluated);
    // 断言 S-表达式同时含 (var x 与 (const 0 两段。
    EXPECT_NE(outcome.symbolic.find("(var x"), std::string::npos);
    EXPECT_NE(outcome.symbolic.find("(const 0"), std::string::npos);
}

TEST(MjtCliEvalTest, SimplifyAppliesIdentity)
{
    // x + 0 化简后只剩变量本身（const 0 被恒等式吸收）。
    const auto outcome = evalOk("x + 0", "", true);
    EXPECT_FALSE(outcome.evaluated);
    EXPECT_NE(outcome.symbolic.find("(var x"), std::string::npos);
    EXPECT_EQ(outcome.symbolic.find("(const 0"), std::string::npos);
}

TEST(MjtCliEvalTest, SimplifyXorAndMaskIdentity)
{
    // (x ^ y) + 2*(x & y) == x + y（MBA 恒等式）。
    const auto outcome = evalOk("(x ^ y) + 2*(x & y)", "", true);
    EXPECT_FALSE(outcome.evaluated);
    EXPECT_NE(outcome.symbolic.find("(var x"), std::string::npos);
    EXPECT_NE(outcome.symbolic.find("(var y"), std::string::npos);
}

TEST(MjtCliEvalTest, SimplifiedConstantsStillEvaluate)
{
    // 化简把常量表达式折成单值后照常求值。
    const auto outcome = evalOk("1 + 2 * 3", "", true);
    EXPECT_TRUE(outcome.evaluated);
    EXPECT_EQ(outcome.value, 7);
}

TEST(MjtCliEvalTest, MissingExpressionIsUsageError)
{
    auto missing = evaluateExpression("", "", false);
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(std::move(missing).unwrap_err().kind, EvalError::Kind::MissingExpression);
}

TEST(MjtCliEvalTest, ParseFailureIsUsageError)
{
    auto broken = evaluateExpression("1 +", "", false);
    ASSERT_TRUE(broken.is_err());
    const auto error = std::move(broken).unwrap_err();
    EXPECT_EQ(error.kind, EvalError::Kind::ParseFailed);
    EXPECT_FALSE(error.message.empty());
}

TEST(MjtCliEvalTest, InvalidVarSpecsAreUsageErrors)
{
    const char* badSpecs[] = {"x", "=5", "x=", "x=abc", "x=99999999999999999999999999"};
    for (const char* spec : badSpecs) {
        auto outcome = evaluateExpression("x + 1", spec, false);
        ASSERT_TRUE(outcome.is_err()) << "spec: " << spec;
        EXPECT_EQ(std::move(outcome).unwrap_err().kind, EvalError::Kind::InvalidVarSpec)
            << "spec: " << spec;
    }
}

TEST(MjtCliEvalTest, RendersValueAndSymbolicLines)
{
    CapturedSink valueCapture;
    mjt::cli::renderEvalOutcome(EvalOutcome{true, 30, ""}, valueCapture.sink());
    EXPECT_EQ(valueCapture.text(), "30\n");

    CapturedSink symbolicCapture;
    mjt::cli::renderEvalOutcome(EvalOutcome{false, 0, "(var x)"}, symbolicCapture.sink());
    EXPECT_EQ(symbolicCapture.text(), "(var x)\n");
}

TEST(MjtCliEvalTest, RenderUsageListsOperatorsAndExamples)
{
    CapturedSink capture;
    mjt::cli::renderEvalUsage(capture.sink());
    const auto text = capture.text();
    EXPECT_NE(text.find("mjt eval"), std::string::npos);
    EXPECT_NE(text.find("Operators:"), std::string::npos);
    EXPECT_NE(text.find("--simplify"), std::string::npos);
    // 用法文自带尾部换行（write 不再加倍）：恰以单个换行收尾。
    EXPECT_TRUE(text.size() >= 2 && text[text.size() - 1] == '\n' && text[text.size() - 2] != '\n');
}
