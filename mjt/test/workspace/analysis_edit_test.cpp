/// @file        analysis_edit_test.cpp
/// @brief       编辑流 → method_versions 指纹版本接线（#132「怎么接进数据库」）：
///              提交锚点单方法面/整类批量面落库、幂等跳过（字节变指纹不变）、
///              未登记身份跳过、revert 与 undo/redo 恢复路径。
///
/// #147 教训：成员身份先有——各用例先 ingestIntoAnalysisDb 登记 class/member
/// 身份，再绑定分析库做编辑；指纹期望值一律从字节直算（同一引擎独立调用），
/// 不硬编码哈希。
#include <gtest/gtest.h>
#include <test_helper.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "jbc/core/reader.hpp"

#include "mjbase/analysis/semantic_fp.hpp"
#include "mjbase/jasm/classfile_utf8.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "workspace/analysis_edit.hpp"
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
    if (!result.isOk()) throw std::runtime_error("analysis edit fixture did not assemble: " + name);
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
  .method public virtualValue ()I {
    code: {
      iconst_1
      ireturn
    }
  }
}
)JASM";
}

/// @brief 整类替换用 Subject：value 载 iconst_2，virtualValue 载 iconst_3
///        （两个方法语义都变）。
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
  .method public virtualValue ()I {
    code: {
      iconst_3
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
      invokestatic workspace/Subject.value ()I
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
    const auto path = test::getTempOutPath("mjt_analysis_edit_input.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path freshDbPath()
{
    const auto path = test::getTempOutPath("mjt_analysis_edit.mjdb");
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

/// @brief 库内已登记方法身份（#147：先 ingest 才有成员 id 可查）。
ca::i64 memberIdOrDie(mjbase::project::AnalysisDb& db, const char* owner, const char* name,
                      const char* descriptor)
{
    auto found = db.findMember(mjbase::project::DbMemberKind::Method,
                               ca::str::Utf8StringRef::from_cstr(owner),
                               ca::str::Utf8StringRef::from_cstr(name),
                               ca::str::Utf8StringRef::from_cstr(descriptor));
    // unwrap() 左值重载按值返回拷贝，DbMemberInfo 携 move-only Utf8String
    // 拷不动：一律走 rvalue 重载把行移出来再判。
    if (found.is_err()) throw std::runtime_error("member lookup failed in db");
    auto info = std::move(found).unwrap();
    if (!info.has_value()) throw std::runtime_error("member identity missing (ingest first)");
    return info->memberId;
}

/// @brief class 字节直算方法指纹（与桥同一引擎、独立调用，作期望值）。
mjbase::analysis::SemanticFp fingerprintOfBytes(const std::vector<ca::u8>& bytes,
                                               const std::string&         methodName)
{
    auto read = jbc::core::readClass(jbc::core::ByteView(bytes));
    if (read.is_err()) throw std::runtime_error("expected-fp fixture did not parse");
    auto parsed = std::move(read).unwrap();
    for (jbc::core::MemberInfo& member : parsed.file->methods) {
        if (mjbase::jasm::classFileUtf8ToString(parsed.file->nameOf(member)) == methodName) {
            auto fp = mjbase::analysis::semanticFp(*parsed.file, member);
            if (fp.is_err()) throw std::runtime_error("expected-fp fixture failed to fingerprint");
            return std::move(fp).unwrap();
        }
    }
    throw std::runtime_error("expected-fp method not found: " + methodName);
}

/// @brief 工作副本中该 class 的当前字节。
std::vector<ca::u8> workingBytesOf(const WorkspaceBase& workspace, const std::string& uri)
{
    auto info = workspace.readClassBytes(ca::str::Utf8StringRef::from_string_view(uri));
    if (info.is_err()) throw std::runtime_error("working class bytes unreadable: " + uri);
    return std::move(info).unwrap().bytes;
}

/// @brief 指令级单方法编辑请求（ReplaceInstruction @ index 0）。
ClassMutation replaceInsnZero(InstructionKind kind, ca::i32 opcode, ca::i32 operand)
{
    ClassMutation mutation;
    mutation.classUri            = ca::str::Utf8String::from_cstr("workspace/Subject.class");
    mutation.kind                = ClassMutationKind::ReplaceInstruction;
    mutation.member.name         = ca::str::Utf8String::from_cstr("value");
    mutation.member.descriptor   = ca::str::Utf8String::from_cstr("()I");
    mutation.instructionIndex    = 0;
    mutation.instruction.kind    = kind;
    mutation.instruction.opcode  = opcode;
    mutation.instruction.operand = operand;
    return mutation;
}

}   // namespace

TEST(MjAnalysisEditTest, MethodEditLandsVersionRowAndUpdatesCurrent)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.openJar(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db = openOrDie(dbPath);
    ASSERT_TRUE(ingestIntoAnalysisDb(workspace, db).is_ok());
    workspace.bindAnalysisDb(&db);

    const ca::i64 valueId = memberIdOrDie(db, "workspace/Subject", "value", "()I");
    const ca::i64 virtualId = memberIdOrDie(db, "workspace/Subject", "virtualValue", "()I");
    // 导入桥只登记身份，不落指纹：编辑前无当前版本。
    auto baseline = db.currentMethodFingerprint(valueId);
    ASSERT_TRUE(baseline.is_ok());
    EXPECT_FALSE(std::move(baseline).unwrap().has_value());

    // 单方法面：iconst_0 → iconst_1（ReplaceInstruction，聚焦成员只落 value）。
    auto result = workspace.applyClassMutation(
        replaceInsnZero(InstructionKind::Simple, 0x04, 0));
    ASSERT_TRUE(result.is_ok()) << result.status.message;
    ASSERT_TRUE(result.committed);
    EXPECT_EQ(1U, result.methodVersions.recorded);
    EXPECT_EQ(0U, result.methodVersions.unchanged);
    EXPECT_EQ(0U, result.methodVersions.skipped);
    EXPECT_FALSE(result.methodVersions.failed);

    auto versions = db.listMethodVersions(valueId);
    ASSERT_TRUE(versions.is_ok());
    auto rows = std::move(versions).unwrap();
    ASSERT_EQ(1U, rows.size());
    EXPECT_TRUE(rows[0].isCurrent);
    EXPECT_EQ(mjbase::analysis::kSemanticFpVersion, rows[0].fpVersion);
    EXPECT_EQ(mjbase::analysis::kSemanticFpHashBytes, rows[0].fingerprint.bytes.size());
    EXPECT_EQ(64U, rows[0].fingerprint.hex.to_std_string().size());

    // 当前指纹 = 编辑后字节的直算指纹（同一引擎独立调用对账）。
    auto current = db.currentMethodFingerprint(valueId);
    ASSERT_TRUE(current.is_ok());
    auto currentFp = std::move(current).unwrap();
    ASSERT_TRUE(currentFp.has_value());
    const std::vector<ca::u8> editedBytes = workingBytesOf(workspace, "workspace/Subject.class");
    EXPECT_TRUE(*currentFp == fingerprintOfBytes(editedBytes, "value"));

    // 非聚焦成员不落行（单方法面口径）。
    auto untouched = db.currentMethodFingerprint(virtualId);
    ASSERT_TRUE(untouched.is_ok());
    EXPECT_FALSE(std::move(untouched).unwrap().has_value());
}

TEST(MjAnalysisEditTest, SemanticsPreservingEditSkipsUnchanged)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.openJar(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db = openOrDie(dbPath);
    ASSERT_TRUE(ingestIntoAnalysisDb(workspace, db).is_ok());
    workspace.bindAnalysisDb(&db);

    const ca::i64 valueId = memberIdOrDie(db, "workspace/Subject", "value", "()I");

    // 第一次编辑：iconst_0 → iconst_1，语义变化落新版本。
    auto first = workspace.applyClassMutation(
        replaceInsnZero(InstructionKind::Simple, 0x04, 0));
    ASSERT_TRUE(first.is_ok()) << first.status.message;
    EXPECT_EQ(1U, first.methodVersions.recorded);

    // 第二次编辑：iconst_1 → bipush 1。字节变了，int 装载族归一化后语义没变
    // ——提交成功（新 revision）但指纹幂等跳过，不落伪版本行。
    auto second = workspace.applyClassMutation(
        replaceInsnZero(InstructionKind::Int, 0x10, 1));
    ASSERT_TRUE(second.is_ok()) << second.status.message;
    ASSERT_TRUE(second.committed);
    EXPECT_EQ(0U, second.methodVersions.recorded);
    EXPECT_EQ(1U, second.methodVersions.unchanged);
    EXPECT_EQ(0U, second.methodVersions.skipped);

    auto versions = db.listMethodVersions(valueId);
    ASSERT_TRUE(versions.is_ok());
    EXPECT_EQ(1U, std::move(versions).unwrap().size());
}

TEST(MjAnalysisEditTest, WholeClassReplacementRecordsBatchAndReconciles)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.openJar(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db = openOrDie(dbPath);
    ASSERT_TRUE(ingestIntoAnalysisDb(workspace, db).is_ok());
    workspace.bindAnalysisDb(&db);

    const ca::i64 valueId   = memberIdOrDie(db, "workspace/Subject", "value", "()I");
    const ca::i64 virtualId = memberIdOrDie(db, "workspace/Subject", "virtualValue", "()I");

    // 整类替换（patch_replace_class_bytes 同款路径）：两个方法语义都变，
    // 批量面逐方法落版本。
    auto result = workspace.replaceClassBytes(
        ca::str::Utf8StringRef::from_cstr("workspace/Subject.class"),
        assemble("workspace/Subject", replacedSubjectSource()));
    ASSERT_TRUE(result.is_ok()) << result.status.message;
    ASSERT_TRUE(result.committed);
    EXPECT_EQ(2U, result.methodVersions.recorded);
    EXPECT_EQ(0U, result.methodVersions.unchanged);
    EXPECT_EQ(0U, result.methodVersions.skipped);
    EXPECT_FALSE(result.methodVersions.failed);

    // 批量 stats 与方法数对账：recorded + unchanged + skipped == 方法数（2）。
    EXPECT_EQ(2U, result.methodVersions.recorded + result.methodVersions.unchanged +
                      result.methodVersions.skipped);

    auto valueVersions = db.listMethodVersions(valueId);
    ASSERT_TRUE(valueVersions.is_ok());
    EXPECT_EQ(1U, std::move(valueVersions).unwrap().size());
    auto virtualVersions = db.listMethodVersions(virtualId);
    ASSERT_TRUE(virtualVersions.is_ok());
    EXPECT_EQ(1U, std::move(virtualVersions).unwrap().size());

    auto current = db.currentMethodFingerprint(valueId);
    ASSERT_TRUE(current.is_ok());
    auto currentFp = std::move(current).unwrap();
    ASSERT_TRUE(currentFp.has_value());
    const std::vector<ca::u8> replacedBytes = workingBytesOf(workspace, "workspace/Subject.class");
    EXPECT_TRUE(*currentFp == fingerprintOfBytes(replacedBytes, "value"));
}

TEST(MjAnalysisEditTest, ClassJasmReassemblyUsesBatchFace)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.openJar(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db = openOrDie(dbPath);
    ASSERT_TRUE(ingestIntoAnalysisDb(workspace, db).is_ok());
    workspace.bindAnalysisDb(&db);

    // 整类 Jasm 重汇编（patch_apply_jasm 同款路径）：无聚焦成员，批量面
    // 兜底。导入桥未落基线版本，首个绑定编辑对受影响类全方法落当前版本
    // （value 语义变了、virtualValue 是首记）。
    auto result = workspace.applyJasm(ca::str::Utf8StringRef::from_cstr("workspace/Subject.class"),
                                      utf16("Subject.jasm").ref(),
                                      utf16(replacedSubjectSource()).ref());
    ASSERT_TRUE(result.is_ok()) << result.status.message;
    ASSERT_TRUE(result.committed);
    EXPECT_EQ(2U, result.methodVersions.recorded);
    EXPECT_EQ(0U, result.methodVersions.unchanged);
    EXPECT_EQ(0U, result.methodVersions.skipped);
}

TEST(MjAnalysisEditTest, UnregisteredIdentitySkipsRecording)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.openJar(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    // 空库（未 ingest）：类/成员身份都不在，编辑成功但落库全跳过，不报错。
    auto db = openOrDie(dbPath);
    workspace.bindAnalysisDb(&db);

    auto batch = workspace.applyJasm(ca::str::Utf8StringRef::from_cstr("workspace/Subject.class"),
                                     utf16("Subject.jasm").ref(),
                                     utf16(replacedSubjectSource()).ref());
    ASSERT_TRUE(batch.is_ok()) << batch.status.message;
    ASSERT_TRUE(batch.committed);
    EXPECT_EQ(0U, batch.methodVersions.recorded);
    EXPECT_EQ(0U, batch.methodVersions.unchanged);
    EXPECT_EQ(2U, batch.methodVersions.skipped);   // Subject 的 2 个方法无身份
    EXPECT_FALSE(batch.methodVersions.failed);

    auto revert = workspace.revertClass(ca::str::Utf8StringRef::from_cstr("workspace/Subject.class"));
    ASSERT_TRUE(revert.is_ok()) << revert.status.message;
    EXPECT_EQ(2U, revert.methodVersions.skipped);

    // 单方法面同口径：聚焦成员无身份 → 跳过 1。
    auto focused = workspace.applyClassMutation(
        replaceInsnZero(InstructionKind::Simple, 0x04, 0));
    ASSERT_TRUE(focused.is_ok()) << focused.status.message;
    EXPECT_EQ(1U, focused.methodVersions.skipped);
}

TEST(MjAnalysisEditTest, UnboundDbKeepsEditsInert)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.openJar(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    auto db = openOrDie(dbPath);
    ASSERT_TRUE(ingestIntoAnalysisDb(workspace, db).is_ok());
    // 不绑定：编辑行为与旧版完全一致，侧账全零，库零写入。

    auto result = workspace.applyClassMutation(
        replaceInsnZero(InstructionKind::Simple, 0x04, 0));
    ASSERT_TRUE(result.is_ok()) << result.status.message;
    ASSERT_TRUE(result.committed);
    EXPECT_EQ(0U, result.methodVersions.recorded);
    EXPECT_EQ(0U, result.methodVersions.unchanged);
    EXPECT_EQ(0U, result.methodVersions.skipped);
    EXPECT_FALSE(result.methodVersions.failed);

    const ca::i64 valueId = memberIdOrDie(db, "workspace/Subject", "value", "()I");
    auto current = db.currentMethodFingerprint(valueId);
    ASSERT_TRUE(current.is_ok());
    EXPECT_FALSE(std::move(current).unwrap().has_value());
}

TEST(MjAnalysisEditTest, RevertClassRecordsRestoredFingerprint)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.openJar(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    const std::vector<ca::u8> originalBytes =
        assemble("workspace/Subject", subjectSource());

    auto db = openOrDie(dbPath);
    ASSERT_TRUE(ingestIntoAnalysisDb(workspace, db).is_ok());
    workspace.bindAnalysisDb(&db);

    const ca::i64 valueId = memberIdOrDie(db, "workspace/Subject", "value", "()I");

    // 编辑（单方法面）→ revert（恢复原字节，批量面）。
    auto edited = workspace.applyClassMutation(
        replaceInsnZero(InstructionKind::Simple, 0x04, 0));
    ASSERT_TRUE(edited.is_ok()) << edited.status.message;
    EXPECT_EQ(1U, edited.methodVersions.recorded);

    // revert 走批量面：value 指纹回到原字形（recorded），virtualValue 首记
    // （导入桥未落基线）。
    auto reverted = workspace.revertClass(
        ca::str::Utf8StringRef::from_cstr("workspace/Subject.class"));
    ASSERT_TRUE(reverted.is_ok()) << reverted.status.message;
    ASSERT_TRUE(reverted.committed);
    EXPECT_EQ(2U, reverted.methodVersions.recorded);
    EXPECT_EQ(0U, reverted.methodVersions.unchanged);
    EXPECT_FALSE(reverted.methodVersions.failed);

    auto versions = db.listMethodVersions(valueId);
    ASSERT_TRUE(versions.is_ok());
    EXPECT_EQ(2U, std::move(versions).unwrap().size());

    auto current = db.currentMethodFingerprint(valueId);
    ASSERT_TRUE(current.is_ok());
    auto currentFp = std::move(current).unwrap();
    ASSERT_TRUE(currentFp.has_value());
    EXPECT_TRUE(*currentFp == fingerprintOfBytes(originalBytes, "value"));
}

TEST(MjAnalysisEditTest, UndoRedoRestoresAndReappliesFingerprints)
{
    const auto jarPath = writeFixtureJar();
    const auto dbPath  = freshDbPath();

    WorkspaceBase workspace;
    ASSERT_TRUE(
        workspace.openJar(ca::str::Utf8StringRef::from_string_view(jarPath.string())).is_ok());

    const std::vector<ca::u8> originalBytes =
        assemble("workspace/Subject", subjectSource());

    auto db = openOrDie(dbPath);
    ASSERT_TRUE(ingestIntoAnalysisDb(workspace, db).is_ok());
    workspace.bindAnalysisDb(&db);

    const ca::i64 valueId = memberIdOrDie(db, "workspace/Subject", "value", "()I");

    auto edited = workspace.applyClassMutation(
        replaceInsnZero(InstructionKind::Simple, 0x04, 0));
    ASSERT_TRUE(edited.is_ok()) << edited.status.message;
    EXPECT_EQ(1U, edited.methodVersions.recorded);

    // undo（不走 commit 锚，独立挂点）：恢复原字节，value 落第 2 行。
    auto undone = workspace.undo();
    ASSERT_TRUE(undone.is_ok()) << undone.status.message;
    ASSERT_TRUE(undone.committed);
    EXPECT_EQ(2U, undone.methodVersions.recorded);   // value 恢复 + virtualValue 首记
    EXPECT_FALSE(undone.methodVersions.failed);

    auto currentAfterUndo = db.currentMethodFingerprint(valueId);
    ASSERT_TRUE(currentAfterUndo.is_ok());
    auto undoFp = std::move(currentAfterUndo).unwrap();
    ASSERT_TRUE(undoFp.has_value());
    EXPECT_TRUE(*undoFp == fingerprintOfBytes(originalBytes, "value"));

    // redo：重放编辑字节，value 第 3 行 + virtualValue 幂等跳过。
    auto redone = workspace.redo();
    ASSERT_TRUE(redone.is_ok()) << redone.status.message;
    ASSERT_TRUE(redone.committed);
    EXPECT_EQ(1U, redone.methodVersions.recorded);
    EXPECT_EQ(1U, redone.methodVersions.unchanged);
    EXPECT_FALSE(redone.methodVersions.failed);

    auto valueVersions = db.listMethodVersions(valueId);
    ASSERT_TRUE(valueVersions.is_ok());
    EXPECT_EQ(3U, std::move(valueVersions).unwrap().size());

    const std::vector<ca::u8> editedBytes = workingBytesOf(workspace, "workspace/Subject.class");
    auto currentAfterRedo = db.currentMethodFingerprint(valueId);
    ASSERT_TRUE(currentAfterRedo.is_ok());
    auto redoFp = std::move(currentAfterRedo).unwrap();
    ASSERT_TRUE(redoFp.has_value());
    EXPECT_TRUE(*redoFp == fingerprintOfBytes(editedBytes, "value"));
}

}   // namespace mjt::workspace
