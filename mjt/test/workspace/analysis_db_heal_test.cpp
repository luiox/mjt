/// @file        analysis_db_heal_test.cpp
/// @brief       open 期身份迁移自愈：一致库零开销幂等（stats 全零）、类名路径
///              对账回位（阶段二，classId 稳定）、成员覆盖层对账回位（阶段三，
///              memberId 稳定、覆盖行健在）。
///
/// WorkspaceBase 不绑台账（bindLedgerStore 缺省 = 纯内存台账，
/// headIdentitySpecBatches 空态安全）：阶段一（注记补账）的 E2E 面在
/// mcp_shadow_token_test.cpp 的重开自愈用例，本文件聚焦阶段二/三的对账
/// 语义；分歧态一律经现成 API（migrateIdentities / setRenames）直构，
/// 不手拼库字节。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "workspace/analysis_db_heal.hpp"
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
    if (!result.isOk()) throw std::runtime_error("analysis heal fixture did not assemble: " + name);
    return result.value->bytes;
}

///方法描述符类内唯一（()V 与 (I)I 各一）：阶段三按 desc 锚定副本成员、
///同描述符多候选计歧义跳过——被测方法 second 的 (I)I 必须唯一，否则
///对账会被误判为 ambiguousSkipped（ingest fixture 的 value()/virtualValue()
///同为 ()I 正是反例，故本文件另起语料）。
const char* subjectSource()
{
    return R"JASM(.super java/lang/Object
.class public super workspace/Subject {
  .field public counter I
  .method public first ()V {
    code: {
      return
    }
  }
  .method public second (I)I {
    code: {
      iconst_0
      ireturn
    }
  }
}
)JASM";
}

const char* mateSource()
{
    return R"JASM(.super java/lang/Object
.class public super workspace/Mate {
  .method public ping ()V {
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
    container.addResource("workspace/Mate.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "workspace/Mate.class",
                              assemble("workspace/Mate", mateSource())));
    const auto path = test::getTempOutPath("mjt_analysis_heal_input.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path freshDbPath()
{
    const auto path = test::getTempOutPath("mjt_analysis_heal.mjdb");
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

///类身份快照（classId + internalName）：heal 前后逐行比对用。
struct ClassRowSnapshot
{
    ca::i64    classId;
    std::string internalName;
};

std::vector<ClassRowSnapshot> snapshotClasses(mjbase::project::AnalysisDb& db)
{
    auto listed = db.listClasses();
    if (listed.is_err()) throw std::runtime_error("list classes failed");
    //先物化再遍历（range-for 只延长最外层临时，不链式取中间对象）。
    auto classRows = std::move(listed).unwrap();
    std::vector<ClassRowSnapshot> rows;
    for (const auto& row : classRows)
        rows.push_back({row.classId, row.internalName.to_std_string()});
    return rows;
}

ca::i64 classIdOrDie(mjbase::project::AnalysisDb& db, const char* internalName)
{
    auto found = db.findClass(ca::str::Utf8StringRef::from_cstr(internalName));
    if (found.is_err()) throw std::runtime_error("find class failed");
    auto info = std::move(found).unwrap();
    if (!info.has_value()) throw std::runtime_error(std::string("class missing: ") + internalName);
    return info->classId;
}

///单条类身份迁移（现成 API 直构分歧态；owner/desc 对 class 规格忽略）。
void migrateClassOrDie(mjbase::project::AnalysisDb& db, const char* from, const char* to)
{
    std::vector<mjbase::project::DbIdentityRename> specs;
    mjbase::project::DbIdentityRename              spec;
    spec.target  = mjbase::project::RenameTargetKind::Class;
    spec.name    = ca::str::Utf8String::from_cstr(from);
    spec.newName = ca::str::Utf8String::from_cstr(to);
    specs.push_back(std::move(spec));
    auto migrated = db.migrateIdentities(std::move(specs));
    if (migrated.is_err()) throw std::runtime_error("migrate identities failed");
    if (std::move(migrated).unwrap().migratedClasses != 1U)
        throw std::runtime_error("class migration did not land");
}

}   // namespace

TEST(MjAnalysisHealTest, HealIsNoOpOnConsistentDb)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.open(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db    = openOrDie(dbPath);
    auto stats = ingestIntoAnalysisDb(workspace, db);
    ASSERT_TRUE(stats.is_ok());
    EXPECT_EQ(2U, std::move(stats).unwrap().classes);

    //一致库 + 纯内存台账（无注记批）：全字段零开销幂等空转。
    const auto before = snapshotClasses(db);
    auto        healed = healAnalysisDbIdentities(workspace, db);
    ASSERT_TRUE(healed.is_ok());
    const auto result = std::move(healed).unwrap();
    EXPECT_EQ(0U, result.annotationBatches);
    EXPECT_EQ(0U, result.migratedClasses);
    EXPECT_EQ(0U, result.migratedMembers);
    EXPECT_EQ(0U, result.skipped);
    EXPECT_EQ(0U, result.collisionBatches);
    EXPECT_EQ(0U, result.reconciledClasses);
    EXPECT_EQ(0U, result.reconciledMembers);
    EXPECT_EQ(0U, result.ambiguousSkipped);

    //类行逐行不变（classId + internalName）。
    const auto after = snapshotClasses(db);
    ASSERT_EQ(before.size(), after.size());
    for (ca::usize index = 0; index < before.size(); ++index) {
        EXPECT_EQ(before[index].classId, after[index].classId);
        EXPECT_EQ(before[index].internalName, after[index].internalName);
    }

    ASSERT_TRUE(db.save().is_ok());
}

TEST(MjAnalysisHealTest, HealReconcilesClassNameDivergence)
{
    //undo 方向分歧模拟：库行名经迁移滞留新名（workspace/Diverged），工作
    //副本仍是原类——无注记可补（纯内存台账），阶段二经 entry_id → 条目
    //path 锚定副本同类，行名 ≠ 副本名即补一条回位迁移，classId 不漂。
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.open(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db    = openOrDie(dbPath);
    auto stats = ingestIntoAnalysisDb(workspace, db);
    ASSERT_TRUE(stats.is_ok());

    const ca::i64 subjectId = classIdOrDie(db, "workspace/Subject");
    const ca::i64 mateId    = classIdOrDie(db, "workspace/Mate");

    //分歧态直构：库行名改走新名（模拟反迁移失败的滞后差）。
    migrateClassOrDie(db, "workspace/Subject", "workspace/Diverged");
    ASSERT_EQ(subjectId, classIdOrDie(db, "workspace/Diverged"));

    auto healed = healAnalysisDbIdentities(workspace, db);
    ASSERT_TRUE(healed.is_ok());
    const auto result = std::move(healed).unwrap();
    EXPECT_EQ(1U, result.reconciledClasses);
    EXPECT_EQ(0U, result.reconciledMembers);
    EXPECT_EQ(0U, result.ambiguousSkipped);

    //行名回副本名、classId 稳定；滞留名 miss；对照类零漂移。
    ASSERT_EQ(subjectId, classIdOrDie(db, "workspace/Subject"));
    auto diverged = db.findClass(ca::str::Utf8StringRef::from_cstr("workspace/Diverged"));
    ASSERT_TRUE(diverged.is_ok());
    EXPECT_FALSE(std::move(diverged).unwrap().has_value());
    ASSERT_EQ(mateId, classIdOrDie(db, "workspace/Mate"));

    ASSERT_TRUE(db.save().is_ok());
}

TEST(MjAnalysisHealTest, HealReconcilesMemberOverlayDivergence)
{
    //阶段三：成员现行名 ≠ 副本成员名、且该成员有覆盖层行。构造走现成
    //写入面：setRenames 落一条成员覆盖行（renames 行按 member_id 键控），
    //再 migrateIdentities 把成员行名改走——覆盖行零触碰、listRenames 经
    //成员联表显示滞留新名，副本名仍锚在 desc 唯一的方法上。
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.open(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db    = openOrDie(dbPath);
    auto stats = ingestIntoAnalysisDb(workspace, db);
    ASSERT_TRUE(stats.is_ok());

    //覆盖层行：second (I)I（desc 类内唯一——见 subjectSource 注记）。
    std::vector<mjbase::project::DbRenameRecord> overlays;
    mjbase::project::DbRenameRecord              overlay;
    overlay.target     = mjbase::project::RenameTargetKind::Member;
    overlay.memberKind = mjbase::project::DbMemberKind::Method;
    overlay.owner      = ca::str::Utf8String::from_cstr("workspace/Subject");
    overlay.name       = ca::str::Utf8String::from_cstr("second");
    overlay.desc       = ca::str::Utf8String::from_cstr("(I)I");
    overlay.shadow     = ca::str::Utf8String::from_cstr("Method_second");
    overlays.push_back(std::move(overlay));
    ASSERT_TRUE(db.setRenames(std::move(overlays)).is_ok());

    auto member = db.findMember(mjbase::project::DbMemberKind::Method,
                                ca::str::Utf8StringRef::from_cstr("workspace/Subject"),
                                ca::str::Utf8StringRef::from_cstr("second"),
                                ca::str::Utf8StringRef::from_cstr("(I)I"));
    ASSERT_TRUE(member.is_ok());
    auto memberInfo = std::move(member).unwrap();
    ASSERT_TRUE(memberInfo.has_value());
    const ca::i64 memberId = memberInfo->memberId;

    //分歧态直构：成员行名改走（覆盖行按稳定 ID 键控不受触碰）。
    {
        std::vector<mjbase::project::DbIdentityRename> specs;
        mjbase::project::DbIdentityRename              spec;
        spec.target     = mjbase::project::RenameTargetKind::Member;
        spec.memberKind = mjbase::project::DbMemberKind::Method;
        spec.owner      = ca::str::Utf8String::from_cstr("workspace/Subject");
        spec.name       = ca::str::Utf8String::from_cstr("second");
        spec.desc       = ca::str::Utf8String::from_cstr("(I)I");
        spec.newName    = ca::str::Utf8String::from_cstr("secondMoved");
        specs.push_back(std::move(spec));
        auto migrated = db.migrateIdentities(std::move(specs));
        ASSERT_TRUE(migrated.is_ok());
        EXPECT_EQ(1U, std::move(migrated).unwrap().migratedMembers);
    }
    auto moved = db.findMember(mjbase::project::DbMemberKind::Method,
                               ca::str::Utf8StringRef::from_cstr("workspace/Subject"),
                               ca::str::Utf8StringRef::from_cstr("secondMoved"),
                               ca::str::Utf8StringRef::from_cstr("(I)I"));
    ASSERT_TRUE(moved.is_ok());
    auto movedInfo = std::move(moved).unwrap();
    ASSERT_TRUE(movedInfo.has_value());
    EXPECT_EQ(memberId, movedInfo->memberId);

    auto healed = healAnalysisDbIdentities(workspace, db);
    ASSERT_TRUE(healed.is_ok());
    const auto result = std::move(healed).unwrap();
    EXPECT_EQ(1U, result.reconciledMembers);
    EXPECT_EQ(0U, result.reconciledClasses);
    EXPECT_EQ(0U, result.ambiguousSkipped);

    //成员行名回副本名、memberId 不变；滞留名 miss；覆盖层行健在（行名随
    //联表回到副本名）。
    auto restored = db.findMember(mjbase::project::DbMemberKind::Method,
                                  ca::str::Utf8StringRef::from_cstr("workspace/Subject"),
                                  ca::str::Utf8StringRef::from_cstr("second"),
                                  ca::str::Utf8StringRef::from_cstr("(I)I"));
    ASSERT_TRUE(restored.is_ok());
    auto restoredInfo = std::move(restored).unwrap();
    ASSERT_TRUE(restoredInfo.has_value());
    EXPECT_EQ(memberId, restoredInfo->memberId);
    auto stale = db.findMember(mjbase::project::DbMemberKind::Method,
                               ca::str::Utf8StringRef::from_cstr("workspace/Subject"),
                               ca::str::Utf8StringRef::from_cstr("secondMoved"),
                               ca::str::Utf8StringRef::from_cstr("(I)I"));
    ASSERT_TRUE(stale.is_ok());
    EXPECT_FALSE(std::move(stale).unwrap().has_value());

    auto renames = db.listRenames();
    ASSERT_TRUE(renames.is_ok());
    auto rows = std::move(renames).unwrap();
    ASSERT_EQ(1U, rows.size());
    EXPECT_EQ(mjbase::project::RenameTargetKind::Member, rows[0].target);
    EXPECT_EQ("workspace/Subject", rows[0].owner.to_std_string());
    EXPECT_EQ("second", rows[0].name.to_std_string());
    EXPECT_EQ("(I)I", rows[0].desc.to_std_string());
    EXPECT_EQ("Method_second", rows[0].shadow.to_std_string());

    ASSERT_TRUE(db.save().is_ok());
}

}   // namespace mjt::workspace
