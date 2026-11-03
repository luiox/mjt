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
    return ".super java/lang/Object\n"
           ".class public super workspace/Subject {\n"
           "  .field public counter I\n"
           "  .method public static value ()I {\n"
           "    code: {\n"
           "      bipush 7\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "  .method public virtualValue ()I {\n"
           "    code: {\n"
           "      iconst_0\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

const char* callerSource()
{
    return ".super java/lang/Object\n"
           ".class public super workspace/Caller {\n"
           "  .method public static call ()I {\n"
           "    code: {\n"
           "      getstatic workspace/Subject.counter I\n"
           "      invokestatic workspace/Subject.value ()I\n"
           "      iadd\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

const char* childSource()
{
    return ".super workspace/Subject\n"
           ".implements java/lang/Runnable\n"
           ".class public super workspace/Child {\n"
           "  .method public virtualValue ()I {\n"
           "    code: {\n"
           "      iconst_1\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "  .method public run ()V {\n"
           "    code: {\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
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
    const auto path = test::getTempOutPath("mjt_analysis_ingest_input.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path freshDbPath()
{
    const auto path = test::getTempOutPath("mjt_analysis_ingest.mjdb");
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::remove_all(path.string() + "-wal", ec);
    std::filesystem::remove_all(path.string() + "-shm", ec);
    return path;
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
        workspace.openJar(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

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
        workspace.openJar(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    {
        auto db    = openOrDie(dbPath);
        auto first = ingestIntoAnalysisDb(workspace, db);
        ASSERT_TRUE(first.is_ok());
        const auto firstStats = std::move(first).unwrap();
        // 三个 class 条目 + jar 写入器自动生成的父目录占位条目。
        EXPECT_EQ(4U, firstStats.entries);
        EXPECT_EQ(4U, firstStats.createdEntries);
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

        // 条目登记：class 条目 kind/形态旗标/内容锚形状齐备；目录占位条目
        // （尾随 '/'、零字节）按非常规形态落库。
        auto entriesListed = db.listEntries(inputId);
        ASSERT_TRUE(entriesListed.is_ok());
        auto entries = std::move(entriesListed).unwrap();
        ASSERT_EQ(4U, entries.size());
        ASSERT_EQ("workspace/", entries[0].path.to_std_string());
        EXPECT_FALSE(entries[0].regular);
        EXPECT_EQ(0, entries[0].size);
        EXPECT_EQ("resource", entries[0].kind.to_std_string());
        std::map<std::string, ca::i64> entryIds;
        for (const mjbase::project::DbEntryInfo& row : entries) {
            if (row.path.to_std_string() == "workspace/") continue;
            EXPECT_EQ("class", row.kind.to_std_string());
            EXPECT_TRUE(row.regular);
            EXPECT_FALSE(row.tombstoned);
            EXPECT_EQ(64U, row.sha256.to_std_string().size());
            EXPECT_GT(row.size, 0);
            entryIds[row.path.to_std_string()] = row.entryId;
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
        EXPECT_EQ(4U, secondStats.entries);
        EXPECT_EQ(0U, secondStats.createdEntries);
        EXPECT_EQ(0U, secondStats.tombstonedEntries);

        auto relisted = db.listEntries(inputId);
        ASSERT_TRUE(relisted.is_ok());
        auto rowsAgain = std::move(relisted).unwrap();
        ASSERT_EQ(4U, rowsAgain.size());
        for (const mjbase::project::DbEntryInfo& row : rowsAgain) {
            const auto id = entryIds.find(row.path.to_std_string());
            if (id == entryIds.end()) {
                EXPECT_EQ("workspace/", row.path.to_std_string());
                continue;
            }
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
    // regular=false；jar 写入器会为双斜杠路径补两级目录占位条目（同样按
    // 非常规落库）；常规 class 条目不受影响。
    mjbase::model::ResourceContainer container;
    container.addResource("workspace/Subject.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "workspace/Subject.class",
                              assemble("workspace/Subject", subjectSource())));
    container.addResource("bad//double.bin",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "bad//double.bin", std::vector<ca::u8>{1, 2, 3, 4}));
    const auto jarPath = test::getTempOutPath("mjt_analysis_ingest_irregular.jar");
    ASSERT_TRUE(mjbase::model::ResourceHelper::exportToJar(container, jarPath.string()).is_ok());

    const auto dbPath = test::getTempOutPath("mjt_analysis_ingest_irregular.mjdb");
    std::error_code ec;
    std::filesystem::remove_all(dbPath, ec);
    std::filesystem::remove_all(dbPath.string() + "-wal", ec);
    std::filesystem::remove_all(dbPath.string() + "-shm", ec);

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.openJar(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db    = openOrDie(dbPath);
    auto stats = ingestIntoAnalysisDb(workspace, db);
    ASSERT_TRUE(stats.is_ok());
    EXPECT_EQ(5U, std::move(stats).unwrap().entries);

    auto inputsListed = db.listInputs();
    ASSERT_TRUE(inputsListed.is_ok());
    auto inputs = std::move(inputsListed).unwrap();
    ASSERT_EQ(1U, inputs.size());

    auto entriesListed = db.listEntries(inputs[0].inputId);
    ASSERT_TRUE(entriesListed.is_ok());
    auto entries = std::move(entriesListed).unwrap();
    ASSERT_EQ(5U, entries.size());
    // listEntries 按 path 字节序：bad/ < bad// < bad//double.bin < workspace/ < ...class
    ASSERT_EQ("bad/", entries[0].path.to_std_string());
    ASSERT_EQ("bad//", entries[1].path.to_std_string());
    ASSERT_EQ("bad//double.bin", entries[2].path.to_std_string());
    ASSERT_EQ("workspace/", entries[3].path.to_std_string());
    ASSERT_EQ("workspace/Subject.class", entries[4].path.to_std_string());
    EXPECT_FALSE(entries[0].regular);
    EXPECT_FALSE(entries[1].regular);
    EXPECT_FALSE(entries[2].regular);
    EXPECT_EQ("resource", entries[2].kind.to_std_string());
    EXPECT_EQ(4, entries[2].size);
    EXPECT_FALSE(entries[3].regular);
    EXPECT_TRUE(entries[4].regular);
    EXPECT_EQ("class", entries[4].kind.to_std_string());

    ASSERT_TRUE(db.save().is_ok());
}

}   // namespace mjt::workspace
