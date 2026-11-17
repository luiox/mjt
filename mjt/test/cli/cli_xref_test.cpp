/// @file        cli_xref_test.cpp
/// @brief       `mjt xref` 核心面测试：to/from 方向语义 / 成员过滤 / limit 截断
///              / both 去重 / 错误分支 / 文本与 JSON 渲染确定性。
///
/// 语料（writeXrefJar）已知引用边：Caller.call 发出 Node.run 方法引用与
/// Node.flag 字段访问，Caller.pick 发出 checkcast 类型引用，NodeImpl 类头 super
/// 声明命中 Node，Node.run 自引用（both 方向去重观察点）。
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "commands.hpp"
#include "shared/output_sink.hpp"
#include "cli_fixture.hpp"

namespace {

using mjt::cli::collectXref;
using mjt::cli::XrefError;

/// 边查找：kind/sourceClass/sourceMethod（空 = 类头）/target 全匹配。
bool findReference(const mjt::cli::XrefReport& report, const std::string& kind,
                   const std::string& sourceClass, const std::string& sourceMethod,
                   const std::string& target)
{
    for (const auto& reference : report.references) {
        if (reference.kind.to_std_string() != kind) continue;
        if (reference.sourceClass.to_std_string() != sourceClass) continue;
        if (reference.sourceMethod.to_std_string() != sourceMethod) continue;
        if (reference.target.to_std_string() != target) continue;
        return true;
    }
    return false;
}

using CapturedSink = cli_fixture::CapturedSink;

}   // namespace

TEST(MjtCliXrefTest, ToDirectionFindsClassLevelReferenceEdges)
{
    const auto jar    = cli_fixture::writeXrefJar();
    auto       report = collectXref(jar.string(), "fixture/Node", "", "to", 200);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();

    EXPECT_EQ(info.direction, "to");
    EXPECT_EQ(info.className, "fixture/Node");
    EXPECT_GT(info.total, 0);
    EXPECT_FALSE(info.truncated);
    EXPECT_EQ(static_cast<ca::i64>(info.references.size()), info.total);

    // 方法边：Caller.call -> Node.run（invokestatic）；自引用边：Node.run。
    EXPECT_TRUE(findReference(info, "method", "fixture/Caller", "call", "fixture/Node.run()V"));
    EXPECT_TRUE(findReference(info, "method", "fixture/Node", "run", "fixture/Node.run()V"));
    // 字段边：Caller.call -> Node.flag（getstatic）。
    EXPECT_TRUE(findReference(info, "field", "fixture/Caller", "call", "fixture/Node.flagZ"));
    // 类型边：Caller.pick checkcast + NodeImpl 类头 super 声明（insnIndex = -1）。
    EXPECT_TRUE(findReference(info, "type", "fixture/Caller", "pick", "fixture/Node"));
    EXPECT_TRUE(findReference(info, "type", "fixture/NodeImpl", "", "fixture/Node"));
    for (const auto& reference : info.references) {
        if (reference.sourceMethod.to_std_string().empty())
            EXPECT_EQ(reference.instructionIndex, -1);
    }
}

TEST(MjtCliXrefTest, FindsDynamicCallsBootstrapAndHandleReferencesInDex)
{
    const auto dex = test::getTopProjectPath() / "test_resource" / "dex" / "invoke_custom.dex";
    const std::string owner = "com/mjt/dexfix/Dynamic";
    auto report = collectXref(dex.string(), owner, "run(I)I", "from", 200);
    ASSERT_TRUE(report.is_ok()) << report.unwrap_err().message;
    const auto info = std::move(report).unwrap();
    EXPECT_TRUE(findReference(info, "bootstrap", owner, "run",
        owner + ".bootstrap(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;"
                "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;)"
                "Ljava/lang/invoke/CallSite;"));
    EXPECT_TRUE(findReference(info, "method_handle", owner, "run", owner + ".identity(I)I"));
    EXPECT_TRUE(findReference(info, "method_handle", owner, "run", owner + ".valueI"));
    std::vector<ca::i32> dynamicSites;
    for (const auto& reference : info.references) {
        if (reference.kind != "dynamic") continue;
        EXPECT_EQ(reference.target, "apply(I)I");
        dynamicSites.push_back(reference.instructionIndex);
    }
    EXPECT_EQ(dynamicSites, (std::vector<ca::i32>{1, 3}));

    auto incoming = collectXref(dex.string(), owner, "", "to", 200);
    ASSERT_TRUE(incoming.is_ok()) << incoming.unwrap_err().message;
    const auto incomingInfo = std::move(incoming).unwrap();
    for (const auto& reference : incomingInfo.references)
        EXPECT_NE(reference.kind, "dynamic");
}

TEST(MjtCliXrefTest, ToDirectionMemberFilterShrinksToExactTarget)
{
    const auto jar = cli_fixture::writeXrefJar();

    auto run = collectXref(jar.string(), "fixture/Node", "run", "to", 200);
    ASSERT_TRUE(run.is_ok()) << run.unwrap_err().message;
    {
        const auto info = std::move(run).unwrap();
        ASSERT_GT(info.total, 0);
        EXPECT_TRUE(findReference(info, "method", "fixture/Caller", "call", "fixture/Node.run()V"));
        for (const auto& reference : info.references)
            EXPECT_EQ(reference.kind.to_std_string(), "method");
        // 描述符精度：写错的描述符零命中。
        auto wrongDesc = collectXref(jar.string(), "fixture/Node", "run(I)V", "to", 200);
        ASSERT_TRUE(wrongDesc.is_ok());
        EXPECT_EQ(std::move(wrongDesc).unwrap().total, 0);
    }

    auto flag = collectXref(jar.string(), "fixture/Node", "flag", "to", 200);
    ASSERT_TRUE(flag.is_ok()) << flag.unwrap_err().message;
    {
        const auto info = std::move(flag).unwrap();
        ASSERT_GT(info.total, 0);
        EXPECT_TRUE(findReference(info, "field", "fixture/Caller", "call", "fixture/Node.flagZ"));
        for (const auto& reference : info.references)
            EXPECT_EQ(reference.kind.to_std_string(), "field");
    }
}

TEST(MjtCliXrefTest, FromDirectionFindsEmittedReferences)
{
    const auto jar = cli_fixture::writeXrefJar();

    auto caller = collectXref(jar.string(), "fixture/Caller", "", "from", 200);
    ASSERT_TRUE(caller.is_ok()) << caller.unwrap_err().message;
    {
        const auto info = std::move(caller).unwrap();
        ASSERT_GT(info.total, 0);
        for (const auto& reference : info.references)
            EXPECT_EQ(reference.sourceClass.to_std_string(), "fixture/Caller");
        EXPECT_TRUE(findReference(info, "method", "fixture/Caller", "call", "fixture/Node.run()V"));
        EXPECT_TRUE(findReference(info, "field", "fixture/Caller", "call", "fixture/Node.flagZ"));
        EXPECT_TRUE(findReference(info, "type", "fixture/Caller", "pick", "fixture/Node"));
    }

    // 成员过滤（from）：只保留 sourceMethod 精确命中的一面。
    auto pick = collectXref(jar.string(), "fixture/Caller", "pick", "from", 200);
    ASSERT_TRUE(pick.is_ok()) << pick.unwrap_err().message;
    {
        const auto info = std::move(pick).unwrap();
        ASSERT_GT(info.total, 0);
        for (const auto& reference : info.references) {
            EXPECT_EQ(reference.sourceMethod.to_std_string(), "pick");
            EXPECT_EQ(reference.kind.to_std_string(), "type");
        }
    }

    // NodeImpl 无方法体 → from 只含自己的类头 super 声明（1 条）。
    auto leaf = collectXref(jar.string(), "fixture/NodeImpl", "", "from", 200);
    ASSERT_TRUE(leaf.is_ok());
    {
        const auto info = std::move(leaf).unwrap();
        EXPECT_EQ(info.total, 1);
        EXPECT_TRUE(findReference(info, "type", "fixture/NodeImpl", "", "fixture/Node"));
    }
}

TEST(MjtCliXrefTest, BothDirectionUnionsAndDeduplicatesSelfEdges)
{
    const auto jar = cli_fixture::writeXrefJar();

    auto to   = collectXref(jar.string(), "fixture/Node", "", "to", 200000);
    auto from = collectXref(jar.string(), "fixture/Node", "", "from", 200000);
    auto both = collectXref(jar.string(), "fixture/Node", "", "both", 200000);
    ASSERT_TRUE(to.is_ok());
    ASSERT_TRUE(from.is_ok());
    ASSERT_TRUE(both.is_ok());
    const auto toValue   = std::move(to).unwrap();
    const auto fromValue = std::move(from).unwrap();
    const auto bothValue = std::move(both).unwrap();

    // 并集有界：total(both) ≤ total(to) + total(from)；自引用边（Node.run 的
    // 方法/owner 类型边）只出现一次。
    EXPECT_LE(bothValue.total, toValue.total + fromValue.total);
    int selfEdges = 0;
    for (const auto& reference : bothValue.references) {
        if (reference.kind.to_std_string() == "method" &&
            reference.sourceClass.to_std_string() == "fixture/Node" &&
            reference.target.to_std_string() == "fixture/Node.run()V")
            ++selfEdges;
    }
    EXPECT_EQ(selfEdges, 1);

    // 稳定序 + 全键无重复（排序后相邻不全等）。
    for (std::size_t index = 1; index < bothValue.references.size(); ++index) {
        const auto& previous = bothValue.references[index - 1];
        const auto& current  = bothValue.references[index];
        const bool  equal    = previous.sourceClass == current.sourceClass &&
                            previous.sourceMethod == current.sourceMethod &&
                            previous.sourceDescriptor == current.sourceDescriptor &&
                            previous.instructionIndex == current.instructionIndex &&
                            previous.kind == current.kind && previous.target == current.target;
        EXPECT_FALSE(equal) << "duplicate reference at index " << index;
    }
}

TEST(MjtCliXrefTest, LimitBoundsOutputAndReportsTruncation)
{
    const auto jar    = cli_fixture::writeXrefJar();
    auto       uncut  = collectXref(jar.string(), "fixture/Node", "", "to", 200);
    ASSERT_TRUE(uncut.is_ok());
    const auto total = std::move(uncut).unwrap().total;
    ASSERT_GT(total, 1);

    auto cut = collectXref(jar.string(), "fixture/Node", "", "to", 1);
    ASSERT_TRUE(cut.is_ok()) << cut.unwrap_err().message;
    {
        const auto info = std::move(cut).unwrap();
        EXPECT_EQ(info.limit, 1);
        EXPECT_EQ(info.total, total);
        EXPECT_TRUE(info.truncated);
        EXPECT_EQ(static_cast<ca::i64>(info.references.size()), 1);
    }

    // limit ≥ total：无截断。
    auto full = collectXref(jar.string(), "fixture/Node", "", "to", total);
    ASSERT_TRUE(full.is_ok());
    {
        const auto info = std::move(full).unwrap();
        EXPECT_FALSE(info.truncated);
        EXPECT_EQ(static_cast<ca::i64>(info.references.size()), total);
    }
}

TEST(MjtCliXrefTest, FailsOnMissingClassAndBadArguments)
{
    const auto jar = cli_fixture::writeXrefJar();

    auto missing = collectXref(jar.string(), "fixture/NoSuch", "", "to", 200);
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(std::move(missing).unwrap_err().kind, XrefError::Kind::ClassNotFound);

    auto direction = collectXref(jar.string(), "fixture/Node", "", "sideways", 200);
    ASSERT_TRUE(direction.is_err());
    EXPECT_EQ(std::move(direction).unwrap_err().kind, XrefError::Kind::InvalidArgument);

    auto limit = collectXref(jar.string(), "fixture/Node", "", "to", 0);
    ASSERT_TRUE(limit.is_err());
    EXPECT_EQ(std::move(limit).unwrap_err().kind, XrefError::Kind::InvalidArgument);

    auto bareParen = collectXref(jar.string(), "fixture/Node", "(I)V", "to", 200);
    ASSERT_TRUE(bareParen.is_err());
    EXPECT_EQ(std::move(bareParen).unwrap_err().kind, XrefError::Kind::InvalidArgument);

    auto openParen = collectXref(jar.string(), "fixture/Node", "run(", "to", 200);
    ASSERT_TRUE(openParen.is_err());
    EXPECT_EQ(std::move(openParen).unwrap_err().kind, XrefError::Kind::InvalidArgument);
}

TEST(MjtCliXrefTest, RendersDeterministicTextAndJson)
{
    const auto jar    = cli_fixture::writeXrefJar();
    auto       report = collectXref(jar.string(), "fixture/Node", "", "to", 200);
    ASSERT_TRUE(report.is_ok());
    const auto info = std::move(report).unwrap();

    CapturedSink textCapture;
    mjt::cli::renderXrefText(info, textCapture.sink());
    const auto text = textCapture.text();
    EXPECT_NE(text.find("xref: " + jar.string()), std::string::npos);
    EXPECT_NE(text.find("class: fixture/Node"), std::string::npos);
    EXPECT_NE(text.find("direction: to"), std::string::npos);
    EXPECT_NE(text.find("references: " + std::to_string(info.total) + " of " +
                        std::to_string(info.total)),
              std::string::npos);
    EXPECT_NE(text.find("  [method] fixture/Caller.call()V@"), std::string::npos);
    EXPECT_NE(text.find("  [field] fixture/Caller.call()V@"), std::string::npos);
    EXPECT_NE(text.find("  [type] fixture/NodeImpl (class header) -> fixture/Node"),
              std::string::npos);

    CapturedSink jsonCapture;
    mjt::cli::renderXrefJson(info, jsonCapture.sink());
    const auto json = jsonCapture.text();
    EXPECT_NE(json.find("\"class\": \"fixture/Node\""), std::string::npos);
    EXPECT_NE(json.find("\"direction\": \"to\""), std::string::npos);
    EXPECT_NE(json.find("\"total\": " + std::to_string(info.total)), std::string::npos);
    EXPECT_NE(json.find("\"truncated\": false"), std::string::npos);
    EXPECT_NE(json.find("\"kind\": \"method\""), std::string::npos);
    EXPECT_NE(json.find("\"source_method\": \"\""), std::string::npos);   // 类头声明行

    // 渲染纯函数：同数据两次渲染逐字节一致。
    CapturedSink again;
    mjt::cli::renderXrefJson(info, again.sink());
    EXPECT_EQ(json, again.text());
}
