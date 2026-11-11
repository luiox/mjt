/// @file        analysis_edit_sync_test.cpp
/// @brief       编辑流 → 分析库类事实增量同步（查询面 DB-first 配套）：
///              条目 sha 重锚（编辑换新锚、undo 锚命中复活 id 稳定）、类事实
///              upsert 换锚保 searchClassesPaged 可见域、引用切片随方法体编辑
///              换新/清空（消失方法）、未导入库的整体跳过。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "workspace/analysis_edit_sync.hpp"
#include "workspace/analysis_ingest.hpp"
#include "workspace/workspace.hpp"

namespace mjt::workspace {
namespace {

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

ca::str::Utf8StringRef utf8Ref(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

std::vector<ca::u8> assemble(const std::string& name, const std::string& sourceText)
{
    mjbase::jasm::JvmJasmService service;
    auto                 sourceName = utf16(name + ".jasm");
    auto                 source     = utf16(sourceText);
    auto                 result     = service.assemble(sourceName.ref(), source.ref());
    if (!result.isOk()) throw std::runtime_error("analysis edit sync fixture did not assemble: " + name);
    return result.value->bytes;
}

/// @brief 基线 Subject：value 载 iconst_0，virtualValue 载 iconst_1。
const char* subjectSource()
{
    return R"JASM(.super java/lang/Object
.class public super workspace/Subject {
  .field public counter I
  .method public static value ()I {
    code: {
      iconst_0
      ireturn
    }
  }
  .method public static virtualValue ()I {
    code: {
      iconst_1
      ireturn
    }
  }
}
)JASM";
}

/// @brief 编辑后 Subject：两方法常量换档（字节变、类名/成员集不变）。
const char* replacedSubjectSource()
{
    return R"JASM(.super java/lang/Object
.class public super workspace/Subject {
  .field public counter I
  .method public static value ()I {
    code: {
      iconst_2
      ireturn
    }
  }
  .method public static virtualValue ()I {
    code: {
      iconst_3
      ireturn
    }
  }
}
)JASM";
}

/// @brief 基线 Caller：call 调 Subject.value。
const char* callerSource()
{
    return R"JASM(.super java/lang/Object
.class public super workspace/Caller {
  .method public static call ()I {
    code: {
      invokestatic workspace/Subject.value ()I
      ireturn
    }
  }
}
)JASM";
}

/// @brief Caller 变体一：call 改调 virtualValue（引用目标换新）。
const char* callerVirtualCallSource()
{
    return R"JASM(.super java/lang/Object
.class public super workspace/Caller {
  .method public static call ()I {
    code: {
      invokestatic workspace/Subject.virtualValue ()I
      ireturn
    }
  }
}
)JASM";
}

/// @brief Caller 变体二：call 无引用（方法在、切片清空）。
const char* callerNoRefSource()
{
    return R"JASM(.super java/lang/Object
.class public super workspace/Caller {
  .method public static call ()I {
    code: {
      iconst_5
      ireturn
    }
  }
}
)JASM";
}

/// @brief Caller 变体三：call 方法消失（库内成员行保留、切片清空计数）。
const char* callerNoMethodSource()
{
    return R"JASM(.super java/lang/Object
.class public super workspace/Caller {
  .method public static other ()I {
    code: {
      iconst_4
      ireturn
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
    const auto path = test::getTempOutPath("mjt_analysis_edit_sync_input.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path freshDbPath(const char* stem)
{
    const auto path = test::getTempOutPath(stem);
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::remove_all(path.string() + "-wal", ec);
    std::filesystem::remove_all(path.string() + "-shm", ec);
    return path;
}

mjbase::project::AnalysisDb openOrDie(const std::filesystem::path& path)
{
    auto result = mjbase::project::AnalysisDb::open(
        ca::str::Utf8StringRef::from_string_view(path.string()),
        mjbase::project::AnalysisDbOpenMode::ReadWrite);
    if (result.is_err()) throw std::runtime_error("analysis db did not open");
    return std::move(result).unwrap();
}

/// @brief 唯一输入行的 input_id（导入桥每次 open 建一行）。
ca::i64 inputIdOrDie(mjbase::project::AnalysisDb& db)
{
    auto listed = db.listInputs();
    if (listed.is_err()) throw std::runtime_error("listInputs failed");
    auto rows = std::move(listed).unwrap();
    if (rows.size() != 1) throw std::runtime_error("expected exactly one input row");
    return rows[0].inputId;
}

/// @brief 该路径当前活跃条目的 entry_id；无活跃行抛错，active 数 >1 也抛。
ca::i64 activeEntryIdOrDie(mjbase::project::AnalysisDb& db, ca::i64 inputId,
                           const char* path)
{
    auto listed = db.listEntries(inputId);
    if (listed.is_err()) throw std::runtime_error("listEntries failed");
    ca::i64 found  = 0;
    ca::usize count = 0;
    for (const mjbase::project::DbEntryInfo& row : std::move(listed).unwrap()) {
        if (row.tombstoned || row.path.to_std_string() != path) continue;
        found = row.entryId;
        ++count;
    }
    if (count != 1) throw std::runtime_error("expected exactly one active entry: " + std::string(path));
    return found;
}

/// @brief 类文本搜索命中行数（exact 档；可见域 = 活跃锚类）。
ca::usize searchHitCount(mjbase::project::AnalysisDb& db, const char* needle)
{
    mjbase::project::DbClassSearchQuery query;
    query.needle = ca::str::Utf8String::from_cstr(needle);
    query.match  = mjbase::project::DbTextMatchKind::Exact;
    auto searched = db.searchClassesPaged(query, 16, 0);
    if (searched.is_err()) throw std::runtime_error("searchClassesPaged failed");
    return std::move(searched).unwrap().rows.size();
}

}   // namespace

TEST(MjAnalysisEditSyncTest, EditReanchorsEntryAndUndoResurrectsOriginalAnchor)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath("mjt_analysis_edit_sync_anchor.mjdb");

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.open(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db = openOrDie(dbPath);
    ASSERT_TRUE(ingestIntoAnalysisDb(workspace, db).is_ok());
    workspace.bindAnalysisDb(&db);
    const ca::i64 inputId = inputIdOrDie(db);

    const ca::i64 originalSubject = activeEntryIdOrDie(db, inputId, "workspace/Subject.class");
    const ca::i64 originalCaller  = activeEntryIdOrDie(db, inputId, "workspace/Caller.class");

    // 编辑（整类重汇编）：条目换锚（同 path 新 sha → 新 entry_id），类事实
    // upsert 换锚，搜索可见域不丢类。
    auto edited = applyEdit(workspace,
                                       EditCommand{EditCommand::Jasm{
                                           ca::str::Utf8String::from_cstr("workspace/Subject.class"),
                                           utf16("Subject.jasm"), utf16(replacedSubjectSource())}},
                                       MutationRequest{});
    ASSERT_TRUE(edited.is_ok()) << edited.status.message;
    ASSERT_TRUE(edited.committed);
    EXPECT_FALSE(edited.analysisSync.failed);
    EXPECT_EQ(1U, edited.analysisSync.classes);
    EXPECT_EQ(3U, edited.analysisSync.members);

    const ca::i64 editedSubject = activeEntryIdOrDie(db, inputId, "workspace/Subject.class");
    EXPECT_NE(originalSubject, editedSubject);
    auto found = db.findClass(utf8Ref("workspace/Subject"));
    ASSERT_TRUE(found.is_ok());
    auto classRow = std::move(found).unwrap();
    ASSERT_TRUE(classRow.has_value());
    EXPECT_EQ(editedSubject, classRow->entryId);
    // 未触碰条目锚不动（避免全容器无谓换锚）。
    EXPECT_EQ(originalCaller, activeEntryIdOrDie(db, inputId, "workspace/Caller.class"));
    EXPECT_EQ(1U, searchHitCount(db, "workspace/Subject"));

    // undo：字节回到原内容 → sha 锚命中原行原地复活（entry id 不漂移），
    // 类事实换回原锚。
    auto undone = workspace.undo();
    ASSERT_TRUE(undone.is_ok()) << undone.status.message;
    ASSERT_TRUE(undone.committed);
    EXPECT_FALSE(undone.analysisSync.failed);
    EXPECT_EQ(originalSubject, activeEntryIdOrDie(db, inputId, "workspace/Subject.class"));
    found = db.findClass(utf8Ref("workspace/Subject"));
    ASSERT_TRUE(found.is_ok());
    classRow = std::move(found).unwrap();
    ASSERT_TRUE(classRow.has_value());
    EXPECT_EQ(originalSubject, classRow->entryId);
    EXPECT_EQ(1U, searchHitCount(db, "workspace/Subject"));

    // redo：换回编辑后字节 → 新锚回归。
    auto redone = workspace.redo(MutationRequest{});
    ASSERT_TRUE(redone.is_ok()) << redone.status.message;
    ASSERT_TRUE(redone.committed);
    EXPECT_FALSE(redone.analysisSync.failed);
    EXPECT_EQ(editedSubject, activeEntryIdOrDie(db, inputId, "workspace/Subject.class"));
    EXPECT_EQ(1U, searchHitCount(db, "workspace/Subject"));
}

TEST(MjAnalysisEditSyncTest, RefSlicesFollowMethodBodyEditsAndVanishingMethods)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath("mjt_analysis_edit_sync_refs.mjdb");

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.open(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db = openOrDie(dbPath);
    ASSERT_TRUE(ingestIntoAnalysisDb(workspace, db).is_ok());
    workspace.bindAnalysisDb(&db);

    // 编辑一：call 改调 virtualValue——方法体切片换新（方法引用 + owner
    // 类型引用两行，目标换新）。
    auto retargeted = applyEdit(workspace,
                                           EditCommand{EditCommand::Jasm{
                                               ca::str::Utf8String::from_cstr("workspace/Caller.class"),
                                               utf16("Caller.jasm"), utf16(callerVirtualCallSource())}},
                                           MutationRequest{});
    ASSERT_TRUE(retargeted.is_ok()) << retargeted.status.message;
    EXPECT_FALSE(retargeted.analysisSync.failed);

    auto slice = db.referencesFromMember(utf8Ref("workspace/Caller"), utf8Ref("call"),
                                         utf8Ref("()I"));
    ASSERT_TRUE(slice.is_ok());
    auto rows = std::move(slice).unwrap();
    ASSERT_EQ(2U, rows.size());
    bool sawMethodRef = false;
    bool sawOwnerType = false;
    for (const auto& row : rows) {
        if (row.kind == mjbase::project::DbReferenceKind::Method) {
            sawMethodRef = true;
            EXPECT_EQ("workspace/Subject", row.targetClass.to_std_string());
            EXPECT_EQ("virtualValue", row.targetName.to_std_string());
        }
        else {
            sawOwnerType = true;
            EXPECT_EQ("workspace/Subject", row.targetClass.to_std_string());
        }
    }
    EXPECT_TRUE(sawMethodRef);
    EXPECT_TRUE(sawOwnerType);

    // 编辑二：call 去引用——方法还在，切片写空。
    auto dereferenced = applyEdit(workspace,
                                             EditCommand{EditCommand::Jasm{
                                                 ca::str::Utf8String::from_cstr("workspace/Caller.class"),
                                                 utf16("Caller.jasm"), utf16(callerNoRefSource())}},
                                             MutationRequest{});
    ASSERT_TRUE(dereferenced.is_ok()) << dereferenced.status.message;
    EXPECT_FALSE(dereferenced.analysisSync.failed);
    EXPECT_EQ(0U, dereferenced.analysisSync.slicesCleared);
    slice = db.referencesFromMember(utf8Ref("workspace/Caller"), utf8Ref("call"), utf8Ref("()I"));
    ASSERT_TRUE(slice.is_ok());
    EXPECT_TRUE(std::move(slice).unwrap().empty());
    // 类头引用（super Object）不受方法体编辑影响。
    auto header = db.referencesFromClass(utf8Ref("workspace/Caller"));
    ASSERT_TRUE(header.is_ok());
    auto headerRows = std::move(header).unwrap();
    ASSERT_EQ(1U, headerRows.size());
    EXPECT_EQ(-1, headerRows[0].insnIndex);

    // 编辑三：call 方法整个消失——库内成员行保留（身份只增不删），切片
    // 清空并计数。
    auto vanished = applyEdit(workspace,
                                         EditCommand{EditCommand::Jasm{
                                             ca::str::Utf8String::from_cstr("workspace/Caller.class"),
                                             utf16("Caller.jasm"), utf16(callerNoMethodSource())}},
                                         MutationRequest{});
    ASSERT_TRUE(vanished.is_ok()) << vanished.status.message;
    EXPECT_FALSE(vanished.analysisSync.failed);
    EXPECT_EQ(1U, vanished.analysisSync.slicesCleared);
    slice = db.referencesFromMember(utf8Ref("workspace/Caller"), utf8Ref("call"), utf8Ref("()I"));
    ASSERT_TRUE(slice.is_ok());
    EXPECT_TRUE(std::move(slice).unwrap().empty());
}

TEST(MjAnalysisEditSyncTest, LibraryWithoutImportSkipsWholesale)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath("mjt_analysis_edit_sync_skip.mjdb");

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.open(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    // 绑库但不导入：无输入行可挂锚——编辑成功，同步整体跳过（开期导入建行）。
    auto db = openOrDie(dbPath);
    workspace.bindAnalysisDb(&db);

    auto edited = applyEdit(workspace,
                                       EditCommand{EditCommand::Jasm{
                                           ca::str::Utf8String::from_cstr("workspace/Subject.class"),
                                           utf16("Subject.jasm"), utf16(replacedSubjectSource())}},
                                       MutationRequest{});
    ASSERT_TRUE(edited.is_ok()) << edited.status.message;
    ASSERT_TRUE(edited.committed);
    EXPECT_FALSE(edited.analysisSync.failed);
    EXPECT_EQ(0U, edited.analysisSync.entries);
    EXPECT_EQ(0U, edited.analysisSync.classes);
    EXPECT_LE(1U, edited.analysisSync.skipped);
}

}   // namespace mjt::workspace
