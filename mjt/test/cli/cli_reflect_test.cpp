/// @file        cli_reflect_test.cpp
/// @brief       `mjt reflect` 核心测试：sink 三态（resolved / pending / unsupported）、无法判定按 pending 呈现的边界、--unresolved 与 --bindings-of 过滤、文本与 JSON 渲染确定性。
#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "commands.hpp"
#include "cli_fixture.hpp"
#include "shared/output_sink.hpp"

namespace {

using mjt::cli::collectReflect;
using mjt::cli::ReflectError;
using mjt::cli::ReflectReport;

using CapturedSink = cli_fixture::CapturedSink;

/// 按 siteMethod + api 查行：命中返回该行，未命中返回 nullptr（一个方法可承载多个 sink 调用点，api 区分同方法内的不同 sink）。
const mjt::cli::ReflectBindingRow* findByMethod(const ReflectReport& report,
                                                const std::string&  siteMethod,
                                                const std::string&  api)
{
    for (const auto& row : report.bindings) {
        if (row.siteMethod == siteMethod && row.api == api) return &row;
    }
    return nullptr;
}

}   // namespace

TEST(MjtCliReflectTest, ResolvesLiteralForNameAndFailsClosedOnMissing)
{
    const auto jar    = cli_fixture::writeReflectJar();
    auto       report = collectReflect(jar.string(), false, "");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto value = std::move(report).unwrap();

    // 类计数（Target + Caller）；sink 计量：4 调用点 = 3 forName（2 resolved + 1 external pending）+ 1 member-lookup pending。
    EXPECT_EQ(value.classCount, 2);
    EXPECT_EQ(value.summary.total, 4);
    EXPECT_EQ(value.summary.resolved, 2);
    EXPECT_EQ(value.summary.pending, 2);
    EXPECT_EQ(value.summary.unsupported, 0);
    EXPECT_NEAR(value.summary.ratePercent, 50.0, 0.01);
    ASSERT_EQ(value.summary.apiDistribution.size(), 2);
    EXPECT_EQ(value.summary.apiDistribution[0].first, "Class.forName");
    EXPECT_EQ(value.summary.apiDistribution[0].second, 3);
    EXPECT_EQ(value.summary.apiDistribution[1].first, "Class.member-lookup");
    EXPECT_EQ(value.summary.apiDistribution[1].second, 1);

    // byName：字面量 forName 容器内类 → Resolved，目标类验证存在（类级 sink 无成员名：target 列 = owner + "." 空成员拼接形）。
    const auto* byName = findByMethod(value, "byName", "Class.forName");
    ASSERT_NE(byName, nullptr);
    EXPECT_EQ(byName->state, "resolved");
    EXPECT_EQ(byName->reason, "");
    EXPECT_TRUE(byName->resolved);
    EXPECT_EQ(byName->className, "rf/Target");
    EXPECT_EQ(byName->targetOwner, "rf/Target");
    EXPECT_EQ(byName->targetName, "");
    EXPECT_EQ(byName->siteClass, "rf/Caller");

    // external：目标类不在容器内 → Pending no_such_class（不猜测外部类），解析出的类名随行披露。
    const auto* external = findByMethod(value, "external", "Class.forName");
    ASSERT_NE(external, nullptr);
    EXPECT_EQ(external->state, "pending");
    EXPECT_EQ(external->reason, "no_such_class");
    EXPECT_EQ(external->className, "java/lang/String");
    EXPECT_FALSE(external->resolved);
}

TEST(MjtCliReflectTest, PendingLookupCarriesReasonAndParsedName)
{
    const auto jar    = cli_fixture::writeReflectJar();
    auto       report = collectReflect(jar.string(), false, "");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto value = std::move(report).unwrap();

    // lookup 内的 getMethod：接收者可判定（forName 解析出目标）、成员名可判定（"nope"）但不在 Target 声明内 → Pending no_such_member，成员名与参数个数随行披露。
    const auto* lookup = findByMethod(value, "lookup", "Class.member-lookup");
    ASSERT_NE(lookup, nullptr);
    EXPECT_EQ(lookup->state, "pending");
    EXPECT_EQ(lookup->reason, "no_such_member");
    EXPECT_EQ(lookup->memberName, "nope");
    EXPECT_EQ(lookup->paramTypeCount, 0);
    EXPECT_FALSE(lookup->resolved);
}

TEST(MjtCliReflectTest, UnresolvedFilterKeepsOnlyPendingRows)
{
    const auto jar    = cli_fixture::writeReflectJar();
    auto       report = collectReflect(jar.string(), true, "");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto value = std::move(report).unwrap();

    ASSERT_EQ(value.bindings.size(), 2);
    for (const auto& row : value.bindings) {
        EXPECT_EQ(row.state, "pending");
        EXPECT_FALSE(row.resolved);
    }
    // 过滤后行集（两条 resolved forName 被 --unresolved 滤除）。
    EXPECT_EQ(findByMethod(value, "byName", "Class.forName"), nullptr);
    EXPECT_NE(findByMethod(value, "lookup", "Class.member-lookup"), nullptr);
    EXPECT_NE(findByMethod(value, "external", "Class.forName"), nullptr);
}

TEST(MjtCliReflectTest, BindingsOfClassRestrictsBySiteClass)
{
    const auto jar    = cli_fixture::writeReflectJar();
    auto       report = collectReflect(jar.string(), false, "rf/Caller");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto value = std::move(report).unwrap();

    // 四个 sink 调用点全部落在 Caller。
    ASSERT_EQ(value.bindings.size(), 4);
    for (const auto& row : value.bindings) EXPECT_EQ(row.siteClass, "rf/Caller");
    EXPECT_EQ(value.classFilter, "rf/Caller");
}

TEST(MjtCliReflectTest, SinklessJarReportsNoneLine)
{
    const auto jar    = cli_fixture::writeInfoJar();
    auto       report = collectReflect(jar.string(), false, "");
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto value = std::move(report).unwrap();

    EXPECT_EQ(value.summary.total, 0);
    EXPECT_NEAR(value.summary.ratePercent, 0.0, 0.0);

    CapturedSink capture;
    mjt::cli::renderReflectText(value, capture.sink());
    EXPECT_NE(capture.text().find("No matching bindings."), std::string::npos);
}

TEST(MjtCliReflectTest, UnopenableJarFailsAsRuntimeError)
{
    auto missing = collectReflect("mjt_cli_reflect_no_such.jar", false, "");
    ASSERT_TRUE(missing.is_err());
    const auto error = std::move(missing).unwrap_err();
    EXPECT_EQ(error.kind, ReflectError::Kind::Runtime);
    EXPECT_FALSE(error.message.empty());
}

TEST(MjtCliReflectTest, RendersDeterministicTextAndJson)
{
    const auto jar    = cli_fixture::writeReflectJar();
    auto       report = collectReflect(jar.string(), false, "");
    ASSERT_TRUE(report.is_ok());
    const auto value = std::move(report).unwrap();

    CapturedSink textCapture;
    mjt::cli::renderReflectText(value, textCapture.sink());
    const auto text = textCapture.text();
    // 摘要三行（en_US 共享表文案）。
    EXPECT_NE(text.find("reflect: " + jar.string() + " (2 classes)"), std::string::npos);
    EXPECT_NE(text.find("4 sink call sites: resolved 2 / pending 2 / unsupported 0, "
                        "resolution rate 50.0%"),
              std::string::npos);
    EXPECT_NE(text.find("API distribution: Class.forName=3, Class.member-lookup=1"),
              std::string::npos);
    // 绑定行：状态 / api / reason / 目标 / 调用点（类级 sink 目标列 = owner + "." 空成员拼接形）。
    EXPECT_NE(text.find("[resolved] Class.forName"), std::string::npos);
    EXPECT_NE(text.find("[pending] Class.member-lookup"), std::string::npos);
    EXPECT_NE(text.find("reason=no_such_class"), std::string::npos);
    EXPECT_NE(text.find("reason=no_such_member"), std::string::npos);
    EXPECT_NE(text.find("target=rf/Target."), std::string::npos);
    EXPECT_NE(text.find("<- rf/Caller.byName()Ljava/lang/Class; insn=1"), std::string::npos);

    CapturedSink jsonCapture;
    mjt::cli::renderReflectJson(value, jsonCapture.sink());
    const auto json = jsonCapture.text();
    EXPECT_EQ(json.front(), '[');
    EXPECT_NE(json.find("\"state\":\"resolved\""), std::string::npos);
    EXPECT_NE(json.find("\"state\":\"pending\""), std::string::npos);
    EXPECT_NE(json.find("\"api\":\"Class.forName\""), std::string::npos);
    EXPECT_NE(json.find("\"target\":{\"owner\":\"rf/Target\""), std::string::npos);
    EXPECT_NE(json.find("\"site\":{\"class\":\"rf/Caller\""), std::string::npos);
    EXPECT_NE(json.back(), ']');   // JsonArrayWriter 闭合 + 尾随换行

    // 渲染纯函数：同数据两次渲染逐字节一致。
    CapturedSink again;
    mjt::cli::renderReflectJson(value, again.sink());
    EXPECT_EQ(json, again.text());
}
