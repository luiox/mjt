/// @file        cli_cfg_test.cpp
/// @brief       `mjt cfg` 核心测试：块级 CFG 采集（分支 / 循环 / try-catch 异常边 / RPO / 支配树）、dot 渲染形状与确定性、.class 直读、方法选择三态（默认首个带体 / 多义 / 未命中）与退出码分支。
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "commands.hpp"
#include "cli_fixture.hpp"

namespace {

using mjt::cli::collectCfg;
using mjt::cli::CfgBlock;
using mjt::cli::CfgError;
using mjt::cli::CfgReport;

using CapturedSink = cli_fixture::CapturedSink;

/// 采集便捷入口：断言成功并返回报告。
CfgReport cfgOk(const std::string& input, const std::string& className,
                const std::string& methodSpec)
{
    auto report = collectCfg(input, className, methodSpec);
    if (report.is_err()) {
        ADD_FAILURE() << "collectCfg unexpectedly failed: " << report.unwrap_err().message;
        return CfgReport{};
    }
    return std::move(report).unwrap();
}

/// 渲染便捷入口：报告 → 捕获文本。
std::string renderDot(const CfgReport& report)
{
    CapturedSink capture;
    mjt::cli::renderCfgDot(report, capture.sink());
    return capture.text();
}

/// Flow 语料上的异常边（detail = catch 类型）。
const mjt::cli::CfgEdge* findExceptionEdge(const CfgReport& report)
{
    for (const auto& edge : report.edges) {
        if (std::string{edge.kind} == "exception") return &edge;
    }
    return nullptr;
}

}   // namespace

TEST(MjtCliCfgTest, CollectsBlockGraphWithBranchLoopAndExceptionEdges)
{
    const auto jar = cli_fixture::writeDfFixtureJar();
    const auto report = cfgOk(jar.string(), "fixture/Flow", "flow");

    EXPECT_TRUE(report.hasBody);
    EXPECT_TRUE(report.instance == false);
    // 循环 + 条件分支 + try-catch 至少切成多个块。
    ASSERT_GT(report.blocks.size(), std::size_t{4});
    ASSERT_FALSE(report.blocks.empty());
    EXPECT_EQ(report.blocks.front().id, 0u);
    EXPECT_TRUE(report.blocks.front().entry);

    // 边种类齐全：条件 taken 边 + 循环回边（branch）+ 异常边。
    bool hasCondition = false;
    bool hasBranch    = false;
    for (const auto& edge : report.edges) {
        hasCondition |= std::string{edge.kind} == "condition";
        hasBranch    |= std::string{edge.kind} == "branch";
    }
    EXPECT_TRUE(hasCondition);
    EXPECT_TRUE(hasBranch);

    const auto* exception = findExceptionEdge(report);
    ASSERT_NE(exception, nullptr);
    EXPECT_EQ(exception->detail, "java/lang/Exception");

    // 边计数对账列：各 kind 计数之和 == 边行数。
    ca::usize counted = 0;
    for (const auto& [kind, count] : report.edgeCounts) counted += count;
    EXPECT_EQ(counted, report.edges.size());

    // RPO 从入口开始且覆盖可达块；入口块自支配。
    ASSERT_FALSE(report.reversePostorder.empty());
    EXPECT_EQ(report.reversePostorder.front(), 0u);
    ASSERT_FALSE(report.dominators.empty());
    EXPECT_TRUE(report.dominators.front().reachable);
    EXPECT_TRUE(report.dominators.front().entrySelfDom);
    EXPECT_EQ(report.dominators.front().idom, 0);
    // try 体某块直接支配链存在（可达块的 idom 均为合法块号）。
    for (const auto& dominator : report.dominators) {
        if (!dominator.reachable) continue;
        EXPECT_TRUE(dominator.entrySelfDom || dominator.idom >= 0);
    }
}

TEST(MjtCliCfgTest, CollectsFromRawClassFileWithInferredClassName)
{
    const auto classFile = cli_fixture::writeFlowClassFile();
    const auto report    = cfgOk(classFile.string(), "", "flow");
    // 类名从文件名推断（剥 .class 后缀）。
    EXPECT_EQ(report.className, "mjt_cli_df_flow");
    EXPECT_EQ(report.methodName, "flow");
    EXPECT_TRUE(report.hasBody);
    EXPECT_NE(findExceptionEdge(report), nullptr);
}

TEST(MjtCliCfgTest, DefaultsToFirstMethodWithBody)
{
    const auto jar    = cli_fixture::writeDfFixtureJar();
    const auto report = cfgOk(jar.string(), "fixture/DfSample", "");
    // DfSample 声明序首个带方法体的方法 = add。
    EXPECT_EQ(report.methodName, "add");
    EXPECT_EQ(report.descriptor, "(II)I");
}

TEST(MjtCliCfgTest, RendersGraphvizDotWithMetadataCommentsAndLabeledEdges)
{
    const auto jar    = cli_fixture::writeDfFixtureJar();
    const auto report = cfgOk(jar.string(), "fixture/Flow", "flow");
    const auto dot    = renderDot(report);

    // digraph 头 + 注释元数据。
    EXPECT_EQ(dot.find("digraph \"fixture/Flow.flow(II)I\" {"), 0u);
    EXPECT_NE(dot.find("  // class: fixture/Flow"), std::string::npos);
    EXPECT_NE(dot.find("  // method: flow (II)I (static)"), std::string::npos);
    EXPECT_NE(dot.find("  // entry: B0"), std::string::npos);
    EXPECT_NE(dot.find("  // rpo: B0,"), std::string::npos);
    EXPECT_NE(dot.find("  // idom: B0 <- (entry)"), std::string::npos);

    // 块节点：块号 + 指令概览（jbc 助记符；常量池解析成文本）。
    EXPECT_NE(dot.find("B0 (entry)"), std::string::npos);
    EXPECT_NE(dot.find("if_icmpge -> B"), std::string::npos);
    EXPECT_NE(dot.find("goto -> B"), std::string::npos);
    EXPECT_NE(dot.find("getstatic fixture/Flow.counter I"), std::string::npos);

    // 边按 kind 标注；异常边带 catch 类型 + 虚线样式。
    EXPECT_NE(dot.find("[label=\"fallthrough\"]"), std::string::npos);
    EXPECT_NE(dot.find("[label=\"condition\"]"), std::string::npos);
    EXPECT_NE(dot.find("[label=\"exception: java/lang/Exception\" style=dashed]"),
              std::string::npos);

    // dot 语法收尾。
    EXPECT_EQ(dot.back(), '\n');
    EXPECT_NE(dot.find("}\n"), std::string::npos);
}

TEST(MjtCliCfgTest, RenderIsDeterministic)
{
    const auto jar    = cli_fixture::writeDfFixtureJar();
    const auto report = cfgOk(jar.string(), "fixture/Flow", "flow");
    EXPECT_EQ(renderDot(report), renderDot(report));
}

TEST(MjtCliCfgTest, MethodWithoutBodyRendersExplicitEmptyGraph)
{
    const auto jar    = cli_fixture::writeDfFixtureJar();
    const auto report = cfgOk(jar.string(), "fixture/DfAbstract", "api");
    EXPECT_FALSE(report.hasBody);
    EXPECT_TRUE(report.blocks.empty());
    EXPECT_TRUE(report.edges.empty());

    const auto dot = renderDot(report);
    EXPECT_EQ(dot.find("digraph \"fixture/DfAbstract.api(I)V\" {"), 0u);
    EXPECT_NE(dot.find("no body"), std::string::npos);
}

TEST(MjtCliCfgTest, SelectorMissesFailAsUsageErrors)
{
    const auto jar = cli_fixture::writeDfFixtureJar();

    // 同名重载多命中。
    auto ambiguous = collectCfg(jar.string(), "fixture/DfSample", "over");
    ASSERT_TRUE(ambiguous.is_err());
    EXPECT_EQ(std::move(ambiguous).unwrap_err().kind, CfgError::Kind::Usage);

    // 方法未命中。
    auto methodMiss = collectCfg(jar.string(), "fixture/DfSample", "nope");
    ASSERT_TRUE(methodMiss.is_err());
    EXPECT_EQ(std::move(methodMiss).unwrap_err().kind, CfgError::Kind::Usage);

    // 类未命中。
    auto classMiss = collectCfg(jar.string(), "fixture/NoSuch", "flow");
    ASSERT_TRUE(classMiss.is_err());
    EXPECT_EQ(std::move(classMiss).unwrap_err().kind, CfgError::Kind::Usage);

    // jar 输入缺 -c。
    auto missingClass = collectCfg(jar.string(), "", "flow");
    ASSERT_TRUE(missingClass.is_err());
    EXPECT_EQ(std::move(missingClass).unwrap_err().kind, CfgError::Kind::Usage);
}

TEST(MjtCliCfgTest, CorruptClassBytesFailAsRuntimeError)
{
    const auto garbage =
        cli_fixture::writeTextFile("mjt_cli_cfg_garbage.class", "not a class file at all");
    auto broken = collectCfg(garbage.string(), "", "");
    ASSERT_TRUE(broken.is_err());
    EXPECT_EQ(std::move(broken).unwrap_err().kind, CfgError::Kind::Runtime);
}

TEST(MjtCliCfgTest, UnopenableJarFailsAsRuntimeError)
{
    auto missing = collectCfg("mjt_cli_cfg_no_such.jar", "fixture/Flow", "flow");
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(std::move(missing).unwrap_err().kind, CfgError::Kind::Runtime);
}
