/// @file        cli_dataflow_test.cpp
/// @brief       `mjt dataflow du|live` 核心测试：slot 级 def-use（入参播种 / 双定义汇合 / 栈与局部使用点 / 字段访问事实 / ok+truncated 状态字段）与局部槽活跃（entry live / store 杀 / 点级前后集）+ 文本与 JSON 渲染 + 方法选择三态与退出码分支。
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "commands.hpp"
#include "cli_fixture.hpp"

namespace {

using mjt::cli::collectDataflowDu;
using mjt::cli::collectDataflowLive;
using mjt::cli::DataflowError;
using mjt::cli::DuReport;
using mjt::cli::LiveReport;

using CapturedSink = cli_fixture::CapturedSink;

/// 采集便捷入口：断言成功并返回报告。
DuReport duOk(const std::string& input, const std::string& className,
              const std::string& methodSpec)
{
    auto report = collectDataflowDu(input, className, methodSpec);
    if (report.is_err()) {
        ADD_FAILURE() << "collectDataflowDu unexpectedly failed: "
                      << report.unwrap_err().message;
        return DuReport{};
    }
    return std::move(report).unwrap();
}

LiveReport liveOk(const std::string& input, const std::string& className,
                  const std::string& methodSpec)
{
    auto report = collectDataflowLive(input, className, methodSpec);
    if (report.is_err()) {
        ADD_FAILURE() << "collectDataflowLive unexpectedly failed: "
                      << report.unwrap_err().message;
        return LiveReport{};
    }
    return std::move(report).unwrap();
}

template <typename Report>
std::string renderTo(void (*render)(const Report&, mjt::OutputSink&), const Report& report)
{
    CapturedSink capture;
    render(report, capture.sink());
    return capture.text();
}

/// 参数定义行（含 this 注记预期由渲染层判断，结构层只认槽位）。
const mjt::cli::DuChain* findParamChain(const DuReport& report, ca::u32 slot)
{
    for (const auto& chain : report.chains) {
        if (chain.def.kindParameter && chain.def.slot == slot) return &chain;
    }
    return nullptr;
}

/// 双定义汇合观察：某使用点被 ≥2 个不同定义指到。
bool hasMergedUse(const DuReport& report)
{
    for (const auto& chain : report.chains) {
        for (const auto& use : chain.uses) {
            std::size_t owners = 0;
            for (const auto& other : report.chains) {
                for (const auto& otherUse : other.uses) {
                    if (otherUse.insnSlot == use.insnSlot &&
                        otherUse.stack == use.stack && otherUse.index == use.index) {
                        owners += 1;
                        break;
                    }
                }
            }
            if (owners >= 2) return true;
        }
    }
    return false;
}

}   // namespace

TEST(MjtCliDataflowDuTest, CollectsSeededParamsAndInstructionDefs)
{
    const auto jar = cli_fixture::writeDfFixtureJar();
    const auto report = duOk(jar.string(), "fixture/DfSample", "add");

    EXPECT_TRUE(report.ok);
    EXPECT_FALSE(report.truncated);
    EXPECT_FALSE(report.unresolvedFieldRefs);
    EXPECT_TRUE(report.instance == false);
    // 入参按签名播种：param[0] / param[1] 两行。slot 级模型：参数定义由局部槽读（load 边）消费，iload_0 的 def 是 param[0]。
    const auto* param0 = findParamChain(report, 0);
    const auto* param1 = findParamChain(report, 1);
    ASSERT_NE(param0, nullptr);
    ASSERT_NE(param1, nullptr);
    ASSERT_FALSE(param0->uses.empty());
    ASSERT_FALSE(param1->uses.empty());
    EXPECT_EQ(param0->uses.front().opcode, "iload_0");
    EXPECT_FALSE(param0->uses.front().stack);
    EXPECT_EQ(param0->uses.front().index, 0u);
    // 指令定义链在参数行之后（DefId 全序：Parameter 先）。
    ASSERT_GE(report.chains.size(), std::size_t{3});
    EXPECT_FALSE(report.chains.back().def.kindParameter);
    EXPECT_FALSE(report.chains.back().def.opcode.empty());

    // 槽位布局回归：(II)I 静态两个基类型参数各占一槽，param[0] / param[1] 各恰一条链（曾因基类型不推进槽计数把两参数都播到槽 0）。
    ca::usize paramChains = 0;
    for (const auto& chain : report.chains) paramChains += chain.def.kindParameter ? 1 : 0;
    EXPECT_EQ(paramChains, std::size_t{2});
}

TEST(MjtCliDataflowDuTest, SeedsCat2ParameterSlotsByDescriptor)
{
    const auto jar = cli_fixture::writeDfFixtureJar();
    // over(J)J：long 占双槽，单参数、播种在槽 0（体 lconst_1; lreturn 不读参数：seed 行 uses 为空即它没有使用者）。
    const auto report = duOk(jar.string(), "fixture/DfSample", "over:(J)J");
    EXPECT_EQ(report.methodName, "over");
    EXPECT_EQ(report.descriptor, "(J)J");
    ASSERT_EQ(report.chains.size(), std::size_t{2});
    EXPECT_TRUE(report.chains.front().def.kindParameter);
    EXPECT_EQ(report.chains.front().def.slot, 0u);
    EXPECT_TRUE(report.chains.front().uses.empty());
    EXPECT_FALSE(report.chains.back().def.kindParameter);
    ASSERT_FALSE(report.chains.back().uses.empty());
    EXPECT_EQ(report.chains.back().uses.front().opcode, "lreturn");
}

TEST(MjtCliDataflowDuTest, CollectsMergedDefsAndFieldFactsOnFlowCorpus)
{
    const auto jar = cli_fixture::writeDfFixtureJar();
    const auto report = duOk(jar.string(), "fixture/DfSample", "");
    ASSERT_TRUE(report.methodName == "add");
    const auto flow = duOk(jar.string(), "fixture/Flow", "flow");

    // 槽 2 双定义在 end 后的 iload_2 汇合：某使用点被 ≥2 个定义指到。
    EXPECT_TRUE(hasMergedUse(flow));

    // 字段访问事实：getstatic fixture/Flow.counter I（读、静态）。
    ASSERT_EQ(flow.fieldAccesses.size(), std::size_t{1});
    const auto& fact = flow.fieldAccesses.front();
    EXPECT_EQ(fact.opcode, "getstatic");
    EXPECT_EQ(fact.owner, "fixture/Flow");
    EXPECT_EQ(fact.name, "counter");
    EXPECT_EQ(fact.descriptor, "I");
    EXPECT_FALSE(fact.write);
    EXPECT_TRUE(fact.isStatic);

    // 溢出位与字段引用解析健康。
    EXPECT_FALSE(flow.truncated);
    EXPECT_FALSE(flow.unresolvedFieldRefs);
    EXPECT_TRUE(flow.ok);
}

TEST(MjtCliDataflowDuTest, CollectsFromRawClassFileWithInferredClassName)
{
    const auto classFile = cli_fixture::writeFlowClassFile();
    const auto report    = duOk(classFile.string(), "", "flow");
    EXPECT_EQ(report.className, "mjt_cli_df_flow");
    EXPECT_TRUE(report.ok);
}

TEST(MjtCliDataflowDuTest, RendersTextTable)
{
    const auto jar = cli_fixture::writeDfFixtureJar();
    const auto report = duOk(jar.string(), "fixture/Flow", "flow");
    const auto text = renderTo(&mjt::cli::renderDuText, report);

    EXPECT_NE(text.find("class:  fixture/Flow"), std::string::npos);
    EXPECT_NE(text.find("method: flow (II)I"), std::string::npos);
    EXPECT_NE(text.find("ok: yes  truncated: no"), std::string::npos);
    // 表头：%-18s 列宽 + 单空格分隔。
    EXPECT_NE(text.find("def" + std::string(16, ' ') + "uses"), std::string::npos);
    EXPECT_NE(text.find("param[0]"), std::string::npos);
    EXPECT_NE(text.find("param[1]"), std::string::npos);
    // 使用点形 i<slot>(<mnemonic>)@stack<k> / @local<k>。
    EXPECT_NE(text.find("@stack"), std::string::npos);
    EXPECT_NE(text.find("@local"), std::string::npos);
}

TEST(MjtCliDataflowDuTest, RendersJsonSingleObject)
{
    const auto jar = cli_fixture::writeDfFixtureJar();
    const auto report = duOk(jar.string(), "fixture/Flow", "flow");
    const auto json = renderTo(&mjt::cli::renderDuJson, report);

    // 结构完整性回归：对象/数组定界符配平（曾因链对象缺 '{' 产出非法 JSON），且每条链都是 {"def":{...},"uses":[...]} 包装形态。
    const auto braces =
        static_cast<std::size_t>(std::count(json.begin(), json.end(), '{'));
    const auto closeBraces =
        static_cast<std::size_t>(std::count(json.begin(), json.end(), '}'));
    const auto brackets =
        static_cast<std::size_t>(std::count(json.begin(), json.end(), '['));
    const auto closeBrackets =
        static_cast<std::size_t>(std::count(json.begin(), json.end(), ']'));
    ASSERT_EQ(braces, closeBraces);
    ASSERT_EQ(brackets, closeBrackets);
    EXPECT_NE(json.find("\"chains\":[{\"def\":{\"kind\":\"parameter\""),
              std::string::npos);

    EXPECT_EQ(json.find("{\"class\":\"fixture/Flow\",\"method\":\"flow\""), 0u);
    EXPECT_NE(json.find("\"descriptor\":\"(II)I\""), std::string::npos);
    EXPECT_NE(json.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(json.find("\"truncated\":false"), std::string::npos);
    EXPECT_NE(json.find("\"unresolved_field_refs\":false"), std::string::npos);
    EXPECT_NE(json.find("\"edges\":"), std::string::npos);
    // 参数定义（静态方法 0 号不是 this）与指令定义两种形态。
    EXPECT_NE(json.find("{\"def\":{\"kind\":\"parameter\",\"slot\":0}"), std::string::npos);
    EXPECT_NE(json.find("\"kind\":\"instruction\""), std::string::npos);
    EXPECT_NE(json.find("\"opcode\":\"iadd\""), std::string::npos);
    // 使用点字段。
    EXPECT_NE(json.find("\"slot\":\"stack\""), std::string::npos);
    EXPECT_NE(json.find("\"slot\":\"local\""), std::string::npos);
    // 字段访问事实。
    EXPECT_NE(json.find("\"field_accesses\":[{\"insn\":"), std::string::npos);
    EXPECT_NE(json.find("\"owner\":\"fixture/Flow\",\"name\":\"counter\""), std::string::npos);
    // 单对象收尾（非数组；JSON 文本自带尾部换行）。
    EXPECT_EQ(json.substr(json.size() - 3), std::string("]}\n"));
}

TEST(MjtCliDataflowLiveTest, CollectsPointLevelSlotLiveness)
{
    const auto jar = cli_fixture::writeDfFixtureJar();
    const auto report = liveOk(jar.string(), "fixture/Flow", "flow");

    // 槽位上界：最高局部操作数 astore 4（非 cat2）→ 5。
    EXPECT_EQ(report.slotCount, 5u);
    // 入口活跃：两参数沿循环路径回读 → 0、1 均活跃。
    EXPECT_NE(std::find(report.entryLive.begin(), report.entryLive.end(), 0u),
              report.entryLive.end());
    EXPECT_NE(std::find(report.entryLive.begin(), report.entryLive.end(), 1u),
              report.entryLive.end());
    // 点级：每条指令一行。
    EXPECT_EQ(report.points.size(), std::size_t{27});

    // 首个 istore_2（index 1）：store 杀 slot 2 → before 不含 2、after 含 2。
    const auto* firstStore = &report.points[1];
    EXPECT_EQ(firstStore->opcode, "istore_2");
    EXPECT_EQ(std::find(firstStore->liveBefore.begin(), firstStore->liveBefore.end(), 2u),
              firstStore->liveBefore.end());
    EXPECT_NE(std::find(firstStore->liveAfter.begin(), firstStore->liveAfter.end(), 2u),
              firstStore->liveAfter.end());
}

TEST(MjtCliDataflowLiveTest, RendersTextTableWithEntryLive)
{
    const auto jar = cli_fixture::writeDfFixtureJar();
    const auto report = liveOk(jar.string(), "fixture/Flow", "flow");
    const auto text = renderTo(&mjt::cli::renderLiveText, report);

    EXPECT_NE(text.find("slots: 5  entry live:"), std::string::npos);
    EXPECT_NE(text.find("insn     pc     opcode"), std::string::npos);
    EXPECT_NE(text.find("istore_2"), std::string::npos);
    EXPECT_NE(text.find("athrow"), std::string::npos);
}

TEST(MjtCliDataflowLiveTest, RendersJsonSingleObject)
{
    const auto jar = cli_fixture::writeDfFixtureJar();
    const auto report = liveOk(jar.string(), "fixture/Flow", "flow");
    const auto json = renderTo(&mjt::cli::renderLiveJson, report);

    // 结构完整性回归：对象/数组定界符配平（du 侧曾因缺 '{' 产出非法 JSON）。
    ASSERT_EQ(std::count(json.begin(), json.end(), '{'),
              std::count(json.begin(), json.end(), '}'));
    ASSERT_EQ(std::count(json.begin(), json.end(), '['),
              std::count(json.begin(), json.end(), ']'));

    EXPECT_EQ(json.find("{\"class\":\"fixture/Flow\",\"method\":\"flow\""), 0u);
    EXPECT_NE(json.find("\"slots\":5"), std::string::npos);
    EXPECT_NE(json.find("\"entry_live\":[0,"), std::string::npos);
    EXPECT_NE(json.find("\"points\":[{\"insn\":"), std::string::npos);
    EXPECT_NE(json.find("\"opcode\":\"iconst_0\""), std::string::npos);
    EXPECT_NE(json.find("\"live_before\":["), std::string::npos);
    EXPECT_NE(json.find("\"live_after\":["), std::string::npos);
    EXPECT_EQ(json.substr(json.size() - 3), std::string("]}\n"));
}

TEST(MjtCliDataflowLiveTest, RenderIsDeterministic)
{
    const auto jar = cli_fixture::writeDfFixtureJar();
    const auto duReport = duOk(jar.string(), "fixture/Flow", "flow");
    const auto liveReport = liveOk(jar.string(), "fixture/Flow", "flow");
    EXPECT_EQ(renderTo(&mjt::cli::renderDuText, duReport),
              renderTo(&mjt::cli::renderDuText, duReport));
    EXPECT_EQ(renderTo(&mjt::cli::renderLiveJson, liveReport),
              renderTo(&mjt::cli::renderLiveJson, liveReport));
}

TEST(MjtCliDataflowTest, MethodWithoutBodyFailsAsUsageError)
{
    const auto jar = cli_fixture::writeDfFixtureJar();
    auto noBody = collectDataflowDu(jar.string(), "fixture/DfAbstract", "api");
    ASSERT_TRUE(noBody.is_err());
    EXPECT_EQ(std::move(noBody).unwrap_err().kind, DataflowError::Kind::Usage);

    auto liveNoBody = collectDataflowLive(jar.string(), "fixture/DfAbstract", "api");
    ASSERT_TRUE(liveNoBody.is_err());
    EXPECT_EQ(std::move(liveNoBody).unwrap_err().kind, DataflowError::Kind::Usage);
}

TEST(MjtCliDataflowTest, SelectorMissesFailAsUsageErrors)
{
    const auto jar = cli_fixture::writeDfFixtureJar();

    auto ambiguous = collectDataflowDu(jar.string(), "fixture/DfSample", "over");
    ASSERT_TRUE(ambiguous.is_err());
    EXPECT_EQ(std::move(ambiguous).unwrap_err().kind, DataflowError::Kind::Usage);

    auto methodMiss = collectDataflowLive(jar.string(), "fixture/DfSample", "nope");
    ASSERT_TRUE(methodMiss.is_err());
    EXPECT_EQ(std::move(methodMiss).unwrap_err().kind, DataflowError::Kind::Usage);

    auto classMiss = collectDataflowDu(jar.string(), "fixture/NoSuch", "add");
    ASSERT_TRUE(classMiss.is_err());
    EXPECT_EQ(std::move(classMiss).unwrap_err().kind, DataflowError::Kind::Usage);

    auto missingClass = collectDataflowDu(jar.string(), "", "add");
    ASSERT_TRUE(missingClass.is_err());
    EXPECT_EQ(std::move(missingClass).unwrap_err().kind, DataflowError::Kind::Usage);
}

TEST(MjtCliDataflowTest, CorruptClassBytesFailAsRuntimeError)
{
    const auto garbage =
        cli_fixture::writeTextFile("mjt_cli_dataflow_garbage.class", "still not a class");
    auto broken = collectDataflowDu(garbage.string(), "", "");
    ASSERT_TRUE(broken.is_err());
    EXPECT_EQ(std::move(broken).unwrap_err().kind, DataflowError::Kind::Runtime);
}
