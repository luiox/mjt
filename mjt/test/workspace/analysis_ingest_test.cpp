/// @file        analysis_ingest_test.cpp
/// @brief       Workspace → AnalysisDb 整库导入：类头/成员/继承/直接引用落库
///              与查询；条目身份登记、重导入 id 稳定与非常规条目旗标。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "raw_zip_fixture.hpp"
#include "workspace/analysis_ingest.hpp"
#include "workspace/workspace.hpp"

namespace mjt::workspace {
namespace {

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::vector<ca::u8> assemble(const std::string& name, const std::string& sourceText)
{
    mjbase::jasm::JvmJasmService service;
    auto                 sourceName = utf16(name + ".jasm");
    auto                 source     = utf16(sourceText);
    auto                 result     = service.assemble(sourceName.ref(), source.ref());
    if (!result.isOk()) throw std::runtime_error("analysis ingest fixture did not assemble: " + name);
    return result.value->bytes;
}

const char* subjectSource()
{
    return R"JASM(.super java/lang/Object
.class public super workspace/Subject {
  .field public counter I
  .method public static value ()I {
    code: {
      bipush 7
      ireturn
    }
  }
  .method public virtualValue ()I {
    code: {
      iconst_0
      ireturn
    }
  }
}
)JASM";
}

const char* callerSource()
{
    return R"JASM(.super java/lang/Object
.class public super workspace/Caller {
  .method public static call ()I {
    code: {
      getstatic workspace/Subject.counter I
      invokestatic workspace/Subject.value ()I
      iadd
      ireturn
    }
  }
}
)JASM";
}

const char* childSource()
{
    return R"JASM(.super workspace/Subject
.implements java/lang/Runnable
.class public super workspace/Child {
  .method public virtualValue ()I {
    code: {
      iconst_1
      ireturn
    }
  }
  .method public run ()V {
    code: {
      return
    }
  }
}
)JASM";
}

std::filesystem::path writeFixtureJar()
{
    mjbase::model::ResourceContainer container;
    container.addResource("workspace/Subject.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "workspace/Subject.class",
                              assemble("workspace/Subject", subjectSource())));
    container.addResource("workspace/Caller.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "workspace/Caller.class",
                              assemble("workspace/Caller", callerSource())));
    container.addResource("workspace/Child.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "workspace/Child.class",
                              assemble("workspace/Child", childSource())));
    // 非 class 条目（资源串事实面 fixture）：两段可打印 run，0x01 分隔
    //（run 边界非可打印字节）。
    const std::string noticeText = "notice-marker-alpha-text\x01notice-marker-beta";
    std::vector<ca::u8> noticeBytes(noticeText.begin(), noticeText.end());
    container.addResource("workspace/notice.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "workspace/notice.txt", std::move(noticeBytes)));
    // manifest 条目（run 扫描含 manifest 面）：两行属性，CRLF 分隔。
    const std::string manifestText =
        "Manifest-Version: 1.0\r\nImplementation-Title: mjt-ingest-manifest-run\r\n\r\n";
    std::vector<ca::u8> manifestBytes(manifestText.begin(), manifestText.end());
    container.addResource("META-INF/MANIFEST.MF",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "META-INF/MANIFEST.MF", std::move(manifestBytes)));
    const auto path = test::getTempOutPath("mjt_analysis_ingest_input.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

// 清场逻辑用 test_helper 单源（test::freshDbPath：主/-wal/-shm 三连清 + 父目录
// 兜底），本文件只钉文件名。
std::filesystem::path freshDbPath()
{
    return test::freshDbPath("mjt_analysis_ingest.mjdb");
}

mjbase::project::AnalysisDb openOrDie(const std::filesystem::path& path,
                                      mjbase::project::AnalysisDbOpenMode mode =
                                          mjbase::project::AnalysisDbOpenMode::ReadWrite)
{
    auto result = mjbase::project::AnalysisDb::open(
        ca::str::Utf8StringRef::from_string_view(path.string()), mode);
    if (result.is_err()) throw std::runtime_error("analysis db did not open");
    return std::move(result).unwrap();
}

}   // namespace

TEST(MjAnalysisIngestTest, ImportsHierarchyMembersAndReferences)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.open(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    {
        auto db    = openOrDie(dbPath);
        auto stats = ingestIntoAnalysisDb(workspace, db);
        ASSERT_TRUE(stats.is_ok());
        const auto ingested = std::move(stats).unwrap();
        EXPECT_EQ(3U, ingested.classes);
        EXPECT_EQ(6U, ingested.members);
        EXPECT_GT(ingested.references, 0U);

        auto child = db.findClass(ca::str::Utf8StringRef::from_cstr("workspace/Child"));
        ASSERT_TRUE(child.is_ok());
        auto childInfo = std::move(child).unwrap();
        ASSERT_TRUE(childInfo.has_value());
        EXPECT_EQ("workspace/Subject", childInfo->superName.to_std_string());
        ASSERT_EQ(1U, childInfo->interfaces.size());
        EXPECT_EQ("java/lang/Runnable", childInfo->interfaces[0].to_std_string());

        auto chain = db.superChain(ca::str::Utf8StringRef::from_cstr("workspace/Child"));
        ASSERT_TRUE(chain.is_ok());
        auto chainNames = std::move(chain).unwrap();
        ASSERT_EQ(3U, chainNames.size());
        EXPECT_EQ("workspace/Child", chainNames[0].to_std_string());
        EXPECT_EQ("workspace/Subject", chainNames[1].to_std_string());
        EXPECT_EQ("java/lang/Object", chainNames[2].to_std_string());

        auto subclasses = db.directSubclasses(ca::str::Utf8StringRef::from_cstr("workspace/Subject"));
        ASSERT_TRUE(subclasses.is_ok());
        auto subclassNames = std::move(subclasses).unwrap();
        ASSERT_EQ(1U, subclassNames.size());
        EXPECT_EQ("workspace/Child", subclassNames[0].to_std_string());

        auto implementors = db.implementors(ca::str::Utf8StringRef::from_cstr("java/lang/Runnable"));
        ASSERT_TRUE(implementors.is_ok());
        auto implementorNames = std::move(implementors).unwrap();
        ASSERT_EQ(1U, implementorNames.size());
        EXPECT_EQ("workspace/Child", implementorNames[0].to_std_string());

        auto calls = db.referencesToMethod(ca::str::Utf8StringRef::from_cstr("workspace/Subject"),
                                           ca::str::Utf8StringRef::from_cstr("value"),
                                           ca::str::Utf8StringRef::from_cstr("()I"));
        ASSERT_TRUE(calls.is_ok());
        auto callRecords = std::move(calls).unwrap();
        ASSERT_EQ(1U, callRecords.size());
        EXPECT_EQ("workspace/Caller", callRecords[0].sourceClass.to_std_string());
        EXPECT_EQ("call", callRecords[0].sourceMethod.to_std_string());
        EXPECT_EQ(1, callRecords[0].insnIndex);

        auto fields = db.referencesToField(ca::str::Utf8StringRef::from_cstr("workspace/Subject"),
                                           ca::str::Utf8StringRef::from_cstr("counter"),
                                           ca::str::Utf8StringRef::from_cstr("I"));
        ASSERT_TRUE(fields.is_ok());
        auto fieldRecords = std::move(fields).unwrap();
        ASSERT_EQ(1U, fieldRecords.size());
        EXPECT_EQ(0, fieldRecords[0].insnIndex);

        // 两条指令级引用 + 两条成员 owner 类型引用 + super 类型引用。
        auto callerRefs = db.referencesFromClass(ca::str::Utf8StringRef::from_cstr("workspace/Caller"));
        ASSERT_TRUE(callerRefs.is_ok());
        EXPECT_EQ(5U, std::move(callerRefs).unwrap().size());

        // Caller 的字段/方法目标 + 两个成员 owner 类型引用 + Child 的 super 类型引用。
        auto subjectRefs = db.referencesToClass(ca::str::Utf8StringRef::from_cstr("workspace/Subject"));
        ASSERT_TRUE(subjectRefs.is_ok());
        EXPECT_EQ(5U, std::move(subjectRefs).unwrap().size());

        ASSERT_TRUE(db.save().is_ok());
    }

    auto readOnly = openOrDie(dbPath, mjbase::project::AnalysisDbOpenMode::ReadOnly);
    auto count    = readOnly.referenceCount();
    ASSERT_TRUE(count.is_ok());
    EXPECT_GT(count.unwrap(), 0U);

    auto base = readOnly.findClass(ca::str::Utf8StringRef::from_cstr("workspace/Subject"));
    ASSERT_TRUE(base.is_ok());
    ASSERT_TRUE(std::move(base).unwrap().has_value());
}

TEST(MjAnalysisIngestTest, RegistersEntryIdentityAndKeepsIdsAcrossReingest)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.open(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    {
        auto db    = openOrDie(dbPath);
        auto first = ingestIntoAnalysisDb(workspace, db);
        ASSERT_TRUE(first.is_ok());
        const auto firstStats = std::move(first).unwrap();
        // 五个条目：3 class + notice + manifest（资源面 fixture 条目，
        // LandsResourceStringsForNonClassEntries 专项覆盖；mjbase#167 起写出
        // 不再合成父目录占位条目）。
        EXPECT_EQ(5U, firstStats.entries);
        EXPECT_EQ(5U, firstStats.createdEntries);
        EXPECT_EQ(0U, firstStats.tombstonedEntries);

        // 输入登记：主容器一行，path 为打开路径，sha256 为容器内容锚形状。
        auto inputsListed = db.listInputs();
        ASSERT_TRUE(inputsListed.is_ok());
        auto inputs = std::move(inputsListed).unwrap();
        ASSERT_EQ(1U, inputs.size());
        EXPECT_EQ(jarPath.string(), inputs[0].path.to_std_string());
        EXPECT_EQ(64U, inputs[0].sha256.to_std_string().size());
        EXPECT_GT(inputs[0].size, 0);
        const ca::i64 inputId = inputs[0].inputId;

        // 条目登记：class 条目 kind/形态旗标/内容锚形状齐备（无目录占位行）；
        // 非 class 条目只核形态旗标与内容锚。
        auto entriesListed = db.listEntries(inputId);
        ASSERT_TRUE(entriesListed.is_ok());
        auto entries = std::move(entriesListed).unwrap();
        ASSERT_EQ(5U, entries.size());
        std::map<std::string, ca::i64> entryIds;
        for (const mjbase::project::DbEntryInfo& row : entries) {
            EXPECT_TRUE(row.regular);
            EXPECT_FALSE(row.tombstoned);
            EXPECT_EQ(64U, row.sha256.to_std_string().size());
            EXPECT_GT(row.size, 0);
            if (row.kind.to_std_string() == "class") {
                entryIds[row.path.to_std_string()] = row.entryId;
            }
        }
        ASSERT_EQ(3U, entryIds.size());
        ASSERT_EQ(1U, entryIds.count("workspace/Subject.class"));

        // class 条目与 classes.entry_id 关联。
        auto subject = db.findClass(ca::str::Utf8StringRef::from_cstr("workspace/Subject"));
        ASSERT_TRUE(subject.is_ok());
        auto subjectInfo = std::move(subject).unwrap();
        ASSERT_TRUE(subjectInfo.has_value());
        EXPECT_EQ(entryIds["workspace/Subject.class"], subjectInfo->entryId);

        // 二次 ingest：同内容重导入 entry_id / class_id 都不漂。
        auto second = ingestIntoAnalysisDb(workspace, db);
        ASSERT_TRUE(second.is_ok());
        const auto secondStats = std::move(second).unwrap();
        EXPECT_EQ(5U, secondStats.entries);
        EXPECT_EQ(0U, secondStats.createdEntries);
        EXPECT_EQ(0U, secondStats.tombstonedEntries);

        auto relisted = db.listEntries(inputId);
        ASSERT_TRUE(relisted.is_ok());
        auto rowsAgain = std::move(relisted).unwrap();
        ASSERT_EQ(5U, rowsAgain.size());
        for (const mjbase::project::DbEntryInfo& row : rowsAgain) {
            if (row.kind.to_std_string() != "class") continue;
            const auto id = entryIds.find(row.path.to_std_string());
            ASSERT_NE(id, entryIds.end());
            EXPECT_EQ(id->second, row.entryId);
            EXPECT_FALSE(row.tombstoned);
        }

        auto subjectAgain = db.findClass(ca::str::Utf8StringRef::from_cstr("workspace/Subject"));
        ASSERT_TRUE(subjectAgain.is_ok());
        auto subjectInfoAgain = std::move(subjectAgain).unwrap();
        ASSERT_TRUE(subjectInfoAgain.has_value());
        EXPECT_EQ(subjectInfo->classId, subjectInfoAgain->classId);
        EXPECT_EQ(subjectInfo->entryId, subjectInfoAgain->entryId);

        ASSERT_TRUE(db.save().is_ok());
    }
}

TEST(MjAnalysisIngestTest, FlagsIrregularEntryPathsAsNonRegular)
{
    // 非常规条目形态（双斜杠路径，#39 硬壳容器形态）：kind 归 resource、
    // regular=false；常规 class 条目不受影响。写面已不产出这种形态
    // （mjbase#167：exportToJar 折叠双斜杠、不再合成目录占位），本用例经
    // raw zip 直写让 bad//double.bin 原样落盘，保住读侧/ingest 分类面覆盖。
    const auto jarPath = test::getTempOutPath("mjt_analysis_ingest_irregular.jar");
    test::writeRawZip(jarPath,
                      {{"workspace/Subject.class",
                        assemble("workspace/Subject", subjectSource())},
                       {"bad//double.bin", std::vector<ca::u8>{1, 2, 3, 4}}});

    const auto dbPath = test::freshDbPath("mjt_analysis_ingest_irregular.mjdb");

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.open(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db    = openOrDie(dbPath);
    auto stats = ingestIntoAnalysisDb(workspace, db);
    ASSERT_TRUE(stats.is_ok());
    EXPECT_EQ(2U, std::move(stats).unwrap().entries);

    auto inputsListed = db.listInputs();
    ASSERT_TRUE(inputsListed.is_ok());
    auto inputs = std::move(inputsListed).unwrap();
    ASSERT_EQ(1U, inputs.size());

    auto entriesListed = db.listEntries(inputs[0].inputId);
    ASSERT_TRUE(entriesListed.is_ok());
    auto entries = std::move(entriesListed).unwrap();
    ASSERT_EQ(2U, entries.size());
    // listEntries 按 path 字节序：bad//double.bin < workspace/Subject.class。
    ASSERT_EQ("bad//double.bin", entries[0].path.to_std_string());
    EXPECT_FALSE(entries[0].regular);
    EXPECT_EQ("resource", entries[0].kind.to_std_string());
    EXPECT_EQ(4, entries[0].size);
    ASSERT_EQ("workspace/Subject.class", entries[1].path.to_std_string());
    EXPECT_TRUE(entries[1].regular);
    EXPECT_EQ("class", entries[1].kind.to_std_string());

    // class 条目与 classes.entry_id 关联（非常规条目在场时关联不漂）。
    auto subject = db.findClass(ca::str::Utf8StringRef::from_cstr("workspace/Subject"));
    ASSERT_TRUE(subject.is_ok());
    auto subjectInfo = std::move(subject).unwrap();
    ASSERT_TRUE(subjectInfo.has_value());
    EXPECT_EQ(entries[1].entryId, subjectInfo->entryId);

    ASSERT_TRUE(db.save().is_ok());
}

TEST(MjAnalysisIngestTest, LandsProducerRegistryAndBatchOwnership)
{
    // 注册表接线（#143 §3.6 批 4）：导入桥自报 trusted 身份、一次导入一个
    // full_import 批、计数与 AnalysisIngestStats 对账、作用域锚
    // （refs_all + input + 逐类）与归属解析（classOwnership /
    // refSliceOwnership）落到整库批；二次导入出新批、注册幂等。
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.open(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db    = openOrDie(dbPath);
    auto stats = ingestIntoAnalysisDb(workspace, db);
    ASSERT_TRUE(stats.is_ok());
    const auto first = std::move(stats).unwrap();

    // producers 非空：唯一一行 = 导入桥身份（trusted，指纹 mjt 前缀）。
    auto producersListed = db.listProducers();
    ASSERT_TRUE(producersListed.is_ok());
    auto producers = std::move(producersListed).unwrap();
    ASSERT_EQ(1U, producers.size());
    EXPECT_EQ("mjt.workspace.ingest", producers[0].key.to_std_string());
    EXPECT_EQ("mjt workspace ingest", producers[0].name.to_std_string());
    EXPECT_EQ(0, producers[0].toolFingerprint.to_std_string().rfind("mjt ", 0));
    EXPECT_EQ(mjbase::project::DbProducerTrustLevel::Trusted, producers[0].trustLevel);
    EXPECT_FALSE(producers[0].createdAt.to_std_string().empty());

    // batches 非空：一个已收尾的 full_import 批，note 自报源容器路径。
    auto batchesListed = db.listBatches(std::nullopt);
    ASSERT_TRUE(batchesListed.is_ok());
    auto batches = std::move(batchesListed).unwrap();
    ASSERT_EQ(1U, batches.size());
    EXPECT_EQ(producers[0].producerId, batches[0].producerId);
    EXPECT_EQ("mjt.workspace.ingest", batches[0].producerKey.to_std_string());
    EXPECT_EQ(mjbase::project::DbBatchKind::FullImport, batches[0].kind);
    EXPECT_FALSE(batches[0].finishedAt.to_std_string().empty());
    EXPECT_EQ(jarPath.string(), batches[0].note.to_std_string());
    EXPECT_FALSE(batches[0].startedAt.to_std_string().empty());

    // 计数与 AnalysisIngestStats 对账（同一导入运行的两侧账本）。
    EXPECT_EQ(static_cast<ca::i64>(first.classes), batches[0].classCount);
    EXPECT_EQ(static_cast<ca::i64>(first.entries), batches[0].entryCount);
    EXPECT_EQ(static_cast<ca::i64>(first.members), batches[0].memberCount);
    EXPECT_EQ(static_cast<ca::i64>(first.references), batches[0].refCount);

    // 作用域锚：refs_all (0,0) + input (1,input_id) + 逐类 (2,class_id)×3 +
    // 逐方法数值切片 (3,member_id)×3（Subject.value / Subject.virtualValue /
    // Child.virtualValue——带数值字面量的方法；常量面整库入口合批落锚，
    // Caller.call 与 Child.run 无数值行不落）。
    auto inputsListed = db.listInputs();
    ASSERT_TRUE(inputsListed.is_ok());
    auto inputs = std::move(inputsListed).unwrap();
    ASSERT_EQ(1U, inputs.size());
    const ca::i64 inputId = inputs[0].inputId;

    auto scopesListed = db.batchScopes(batches[0].batchId);
    ASSERT_TRUE(scopesListed.is_ok());
    auto scopes = std::move(scopesListed).unwrap();
    ASSERT_EQ(8U, scopes.size());
    EXPECT_EQ(mjbase::project::DbBatchScopeKind::RefsAll, scopes[0].kind);
    EXPECT_EQ(0, scopes[0].scopeId);
    EXPECT_EQ(mjbase::project::DbBatchScopeKind::Input, scopes[1].kind);
    EXPECT_EQ(inputId, scopes[1].scopeId);
    ca::usize scopeIndex = 2;
    for (; scopeIndex < 5; ++scopeIndex) {
        EXPECT_EQ(mjbase::project::DbBatchScopeKind::Class, scopes[scopeIndex].kind);
        EXPECT_GT(scopes[scopeIndex].scopeId, 0);
    }
    for (; scopeIndex < scopes.size(); ++scopeIndex) {
        EXPECT_EQ(mjbase::project::DbBatchScopeKind::Member, scopes[scopeIndex].kind);
        EXPECT_GT(scopes[scopeIndex].scopeId, 0);
    }

    // 类事实归属解析：逐类作用域行让 classOwnership 可答（整库批）。
    auto subject = db.findClass(ca::str::Utf8StringRef::from_cstr("workspace/Subject"));
    ASSERT_TRUE(subject.is_ok());
    auto subjectInfo = std::move(subject).unwrap();
    ASSERT_TRUE(subjectInfo.has_value());
    auto classOwned = db.classOwnership(subjectInfo->classId);
    ASSERT_TRUE(classOwned.is_ok());
    auto classOwnership = std::move(classOwned).unwrap();
    ASSERT_TRUE(classOwnership.has_value());
    EXPECT_EQ(batches[0].batchId, classOwnership->batchId);
    EXPECT_EQ("mjt.workspace.ingest", classOwnership->producerKey.to_std_string());
    EXPECT_EQ(mjbase::project::DbBatchKind::FullImport, classOwnership->kind);

    // 引用切片归属解析：refs_all 锚覆盖全部成员切片（#147 教训：成员身份
    // 来自导入桥的 ingestClasses，先有成员才有切片可查）。
    auto member = db.findMember(mjbase::project::DbMemberKind::Method,
                                ca::str::Utf8StringRef::from_cstr("workspace/Subject"),
                                ca::str::Utf8StringRef::from_cstr("value"),
                                ca::str::Utf8StringRef::from_cstr("()I"));
    ASSERT_TRUE(member.is_ok());
    auto memberInfo = std::move(member).unwrap();
    ASSERT_TRUE(memberInfo.has_value());
    auto sliceOwned = db.refSliceOwnership(mjbase::project::DbMemberKind::Method,
                                           memberInfo->memberId);
    ASSERT_TRUE(sliceOwned.is_ok());
    auto sliceOwnership = std::move(sliceOwned).unwrap();
    ASSERT_TRUE(sliceOwnership.has_value());
    EXPECT_EQ(batches[0].batchId, sliceOwnership->batchId);
    EXPECT_EQ("mjt.workspace.ingest", sliceOwnership->producerKey.to_std_string());

    // 二次导入：第二个批（计数同量），producer 注册幂等仍一行。
    auto again = ingestIntoAnalysisDb(workspace, db);
    ASSERT_TRUE(again.is_ok());
    const auto second = std::move(again).unwrap();
    EXPECT_EQ(first.classes, second.classes);
    EXPECT_EQ(0U, second.createdEntries);

    auto batchesAgainListed = db.listBatches(std::nullopt);
    ASSERT_TRUE(batchesAgainListed.is_ok());
    auto batchesAgain = std::move(batchesAgainListed).unwrap();
    ASSERT_EQ(2U, batchesAgain.size());
    EXPECT_NE(batchesAgain[0].batchId, batchesAgain[1].batchId);
    EXPECT_EQ(batches[0].batchId, batchesAgain[0].batchId);
    EXPECT_EQ(mjbase::project::DbBatchKind::FullImport, batchesAgain[1].kind);
    EXPECT_FALSE(batchesAgain[1].finishedAt.to_std_string().empty());
    EXPECT_EQ(static_cast<ca::i64>(second.entries), batchesAgain[1].entryCount);

    auto producersAgainListed = db.listProducers();
    ASSERT_TRUE(producersAgainListed.is_ok());
    EXPECT_EQ(1U, std::move(producersAgainListed).unwrap().size());

    // 生产者聚合统计：两批求和（批次数与各计数）。
    auto statsListed = db.producerStats();
    ASSERT_TRUE(statsListed.is_ok());
    auto producerStats = std::move(statsListed).unwrap();
    ASSERT_EQ(1U, producerStats.size());
    EXPECT_EQ(2, producerStats[0].batchCount);
    EXPECT_EQ(static_cast<ca::i64>(first.classes + second.classes),
              producerStats[0].classCount);
    EXPECT_EQ(static_cast<ca::i64>(first.references + second.references),
              producerStats[0].refCount);
    EXPECT_FALSE(producerStats[0].firstBatchAt.to_std_string().empty());
    EXPECT_FALSE(producerStats[0].lastBatchAt.to_std_string().empty());

    ASSERT_TRUE(db.save().is_ok());
}

/// @brief 资源串事实面（schema v13）：非 class 条目的可打印 run 经抽取层
///        （constant_scan::resourceStringRecords，引擎 resourceStrings 逐字节
///        同形）落 resources 表，整库单事务（replaceResources）；manifest 与
///        fixture 自带资源条目的 run 均可经 searchResourceStringsPaged 查回。
// 真修在场（manifest fixture + 空成员切片不落锚）：资源串/批次锚两断言
// 重跑（PR 99 曾以 DISABLED 收敛 main 上从未编译通过的旧断言）。
TEST(MjAnalysisIngestTest, LandsResourceStringsForNonClassEntries)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.open(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    {
        auto db    = openOrDie(dbPath);
        auto stats = ingestIntoAnalysisDb(workspace, db);
        ASSERT_TRUE(stats.is_ok());
        // manifest run + fixture notice 两段 run：资源行非空且含确定性内容。
        EXPECT_GT(std::move(stats).unwrap().resourceStrings, 2U);

        // contains 不敏感：两段 notice run 全中（(path, value) 全序）。
        mjbase::project::DbResourceSearchQuery query;
        query.needle = ca::str::Utf8String::from_cstr("NOTICE-MARKER");
        query.match  = mjbase::project::DbTextMatchKind::Contains;
        auto page    = db.searchResourceStringsPaged(query, 100, 0);
        ASSERT_TRUE(page.is_ok());
        const auto result = std::move(page).unwrap();
        ASSERT_EQ(2U, result.rows.size());
        EXPECT_EQ("workspace/notice.txt", result.rows[0].entryPath.to_std_string());
        EXPECT_EQ("notice-marker-alpha-text", result.rows[0].value.to_std_string());
        EXPECT_EQ("workspace/notice.txt", result.rows[1].entryPath.to_std_string());
        EXPECT_EQ("notice-marker-beta", result.rows[1].value.to_std_string());

        // manifest 机器 run 落库（空 needle 全量 > notice 两行）。
        mjbase::project::DbResourceSearchQuery all;
        auto allPage = db.searchResourceStringsPaged(all, 1000, 0);
        ASSERT_TRUE(allPage.is_ok());
        EXPECT_GT(std::move(allPage).unwrap().total, 2U);

        ASSERT_TRUE(db.save().is_ok());
    }

    // 只读重开：资源行持久（save 后 checkpoint 收敛）。
    auto readOnly = openOrDie(dbPath, mjbase::project::AnalysisDbOpenMode::ReadOnly);
    mjbase::project::DbResourceSearchQuery again;
    again.needle = ca::str::Utf8String::from_cstr("notice-marker-beta");
    auto reread  = readOnly.searchResourceStringsPaged(again, 100, 0);
    ASSERT_TRUE(reread.is_ok());
    const auto rows = std::move(reread).unwrap();
    ASSERT_EQ(1U, rows.rows.size());
    EXPECT_EQ("notice-marker-beta", rows.rows[0].value.to_std_string());
}

}   // namespace mjt::workspace
