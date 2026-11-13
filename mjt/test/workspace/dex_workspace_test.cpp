/// @file        dex_workspace_test.cpp
/// @brief       apk/dex 输入的引擎面测试：分派装载、类事实、引用/层次、
///              常量事实、smali 视图与守卫面。
///
/// fixture：test_resource/dex/ 的 smalict 产物（smali 源同目录可再生）——
/// hello_refs.dex（Base/Hook/Subject：invoke/field/type/const-string 全覆盖
/// + super/interfaces 层次 + 静态字符串初值）与 hello_extra.dex（const-wide/
/// const-class/instance-of）。apk 形态由 test::writeRawZip 包 dex + 资源条目。

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

#include "workspace/dex_domain.hpp"
#include "raw_zip_fixture.hpp"
#include "test_helper.hpp"
#include "workspace/analysis_ingest.hpp"
#include "workspace/analysis_ops.hpp"
#include "workspace/container_io.hpp"
#include "workspace/constant_scan.hpp"
#include "workspace/edit_command.hpp"
#include "workspace/jar_ops.hpp"
#include "workspace/workspace.hpp"

namespace {

using namespace mjt::workspace;
using ca::str::Utf8String;
using ca::str::Utf8StringRef;

Utf8StringRef refOf(const std::string& value)
{
    return Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                    value.size());
}

///std::string → owning Utf16String（EditCommand::Jasm 载荷字段）。
ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::filesystem::path dexResource(const std::string& name)
{
    // 基线资源在仓顶层 test_resource/（mjt 项目标记映射到 mjt/ 子目录，
    // 与 asm-test 语料的 top-project 定位同形）。
    return test::getTopProjectPath() / "test_resource" / "dex" / name;
}

std::vector<ca::u8> readBytes(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    return std::vector<ca::u8>((std::istreambuf_iterator<char>(input)),
                               std::istreambuf_iterator<char>());
}

std::vector<ca::u8> binaryManifest()
{
    // Android ResXMLTree header followed by a UTF-16 ResStringPool containing U+5E94.
    return {
        3, 0, 8, 0, 48, 0, 0, 0,
        1, 0, 28, 0, 40, 0, 0, 0,
        1, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 32, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 1, 0, 0x94, 0x5e, 0, 0, 0, 0,
    };
}

/// @brief APK fixture with two DEX units, a binary manifest and a text resource.
std::filesystem::path writeFixtureApk()
{
    const auto path = test::getTempOutPath("mjt_dex_fixture.apk");
    test::writeRawZip(path, {
        {"classes.dex", readBytes(dexResource("hello_refs.dex"))},
        {"classes2.dex", readBytes(dexResource("hello_extra.dex"))},
        {"AndroidManifest.xml", binaryManifest()},
        {"res/values.txt", std::vector<ca::u8>({'m', 'j', 't', '-', 'd', 'e', 'x', '-'})},
    });
    return path;
}

std::filesystem::path freshDbPath()
{
    const auto path = test::getTempOutPath("mjt_dex_workspace.mjdb");
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::remove_all(path.string() + "-wal", ec);
    std::filesystem::remove_all(path.string() + "-shm", ec);
    return path;
}

mjbase::project::AnalysisDb openOrDie(const std::filesystem::path& path)
{
    auto result = mjbase::project::AnalysisDb::open(refOf(path.string()),
                                                    mjbase::project::AnalysisDbOpenMode::ReadWrite);
    if (result.is_err()) throw std::runtime_error("analysis db did not open");
    return std::move(result).unwrap();
}

}   // namespace

TEST(MjDexWorkspaceTest, ProbesDexAndApkForms)
{
    EXPECT_EQ(DexArtifactForm::Dex, probeDexArtifact(dexResource("hello_refs.dex").string()));
    EXPECT_EQ(DexArtifactForm::Apk, probeDexArtifact(writeFixtureApk().string()));
    // jar 形态不受 sniff 影响：.jar 扩展名 + zip magic → None（走 jar 流程）。
    const auto jarPath = test::getTempOutPath("mjt_dex_probe.jar");
    test::writeRawZip(jarPath, {{"A.txt", std::vector<ca::u8>({'x'})}});
    EXPECT_EQ(DexArtifactForm::None, probeDexArtifact(jarPath.string()));
}

TEST(MjDexWorkspaceTest, OpensRawDexAndListsClassFacts)
{
    WorkspaceBase workspace;
    ASSERT_FALSE(workspace.open(refOf(dexResource("hello_refs.dex").string())).is_err());
    EXPECT_EQ(ContainerFormat::Dex, workspace.format());
    EXPECT_TRUE(workspace.isDexBacked());
    ASSERT_NE(workspace.dexDomain(), nullptr);
    EXPECT_EQ(35u, workspace.dexDomain()->dexVersion());

    auto classes = workspace.dexDomain()->listClasses();
    ASSERT_FALSE(classes.is_err());
    const auto list = std::move(classes).unwrap();
    ASSERT_EQ(3u, list.size());
    // class_def 序：Base/Hook/Subject。
    EXPECT_EQ("com/mjt/dexfix/Base", list[0].internalName.to_std_string());
    EXPECT_EQ("java/lang/Object", list[0].superName.to_std_string());
    // 裸 dex 单容器：条目 uri = 容器路径本身（apk 形态才是 classesN.dex）。
    EXPECT_NE(std::string::npos,
              list[0].resourceUri.to_std_string().find("hello_refs.dex"));
    EXPECT_EQ("com/mjt/dexfix/Hook", list[1].internalName.to_std_string());
    // Hook 是接口（接口自身无 implements 行）；implements 事实在 Subject 上。
    EXPECT_EQ(0u, list[1].interfaces.size());
    EXPECT_EQ("com/mjt/dexfix/Subject", list[2].internalName.to_std_string());
    EXPECT_EQ("com/mjt/dexfix/Base", list[2].superName.to_std_string());
    EXPECT_EQ(1u, list[2].interfaces.size());
    // 成员事实：Subject 的 describe()Ljava/lang/String; 与 Base 的静态初值。
    const ClassInfo& subject = list[2];
    bool hasDescribe = false;
    for (const MethodInfo& method : subject.methods) {
        if (method.name.to_std_string() == "describe") {
            hasDescribe = true;
            EXPECT_EQ("()Ljava/lang/String;", method.descriptor.to_std_string());
        }
    }
    EXPECT_TRUE(hasDescribe);
    const ClassInfo& base = list[0];
    bool hasStaticValue = false;
    for (const FieldInfo& field : base.fields) {
        if (field.name.to_std_string() == "sTag") {
            ASSERT_TRUE(field.constantValue.has_value());
            EXPECT_EQ(ConstantValueInfo::Kind::String, field.constantValue->kind);
            EXPECT_EQ("dexfix-base", field.constantValue->text.to_std_string());
            hasStaticValue = true;
        }
    }
    EXPECT_TRUE(hasStaticValue);
}

TEST(MjDexWorkspaceTest, ReadsUtf16AndroidManifestStringPool)
{
    auto xml = binaryManifest();
    auto strings = androidManifestStringRecords(xml);
    ASSERT_EQ(1u, strings.size());
    EXPECT_EQ("\xe5\xba\x94", strings.front().to_std_string());
    xml.pop_back();
    EXPECT_TRUE(androidManifestStringRecords(xml).empty());
}

TEST(MjDexWorkspaceTest, ReassemblesOneClassWithoutChangingTheOriginalDex)
{
    WorkspaceBase workspace;
    ASSERT_FALSE(workspace.open(refOf(dexResource("hello_refs.dex").string())).is_err());
    const auto* dex = workspace.dexDomain();
    ASSERT_NE(nullptr, dex);
    const std::string name = "com/mjt/dexfix/Base";
    auto disassembled = dex->smaliText(name);
    ASSERT_FALSE(disassembled.is_err());
    const auto original = std::move(disassembled).unwrap();
    auto edited = original;
    const auto pos = edited.find("dexfix-base");
    ASSERT_NE(std::string::npos, pos);
    edited.replace(pos, std::string_view("dexfix-base").size(), "edited-base");
    auto rebuilt = dex->assembleSmali(name, edited);
    ASSERT_FALSE(rebuilt.is_err()) << rebuilt.unwrap_err().message;
    const auto bytes = std::move(rebuilt).unwrap();
    const auto path = test::getTempOutPath("mjt_reassembled.dex");
    std::error_code cleanup;
    std::filesystem::remove(path, cleanup);
    const auto written = writeDerivedDex(workspace.sourcePath().to_std_string(), path.u8string(),
                                         bytes);
    ASSERT_FALSE(written.is_err()) << written.message;
    EXPECT_TRUE(writeDerivedDex(workspace.sourcePath().to_std_string(), path.u8string(),
                                bytes).is_err());
    WorkspaceBase reopened;
    ASSERT_FALSE(reopened.open(refOf(path.u8string())).is_err());
    const auto* output = reopened.dexDomain();
    ASSERT_NE(nullptr, output);
    EXPECT_EQ(dex->classCount(), output->classCount());
    EXPECT_EQ(edited, output->smaliText(name).unwrap());
    EXPECT_EQ(original, dex->smaliText(name).unwrap());
    EXPECT_TRUE(dex->assembleSmali("com/mjt/dexfix/Subject", edited).is_err());
}

TEST(MjDexWorkspaceTest, OpensApkAndIngestsIntoAnalysisDb)
{
    const auto apkPath = writeFixtureApk();
    WorkspaceBase workspace;
    ASSERT_FALSE(workspace.open(refOf(apkPath.string())).is_err());
    EXPECT_EQ(ContainerFormat::Apk, workspace.format());
    ASSERT_NE(workspace.dexDomain(), nullptr);
    ASSERT_EQ(2u, workspace.dexDomain()->dexUris().size());
    // 跨 dex 类合计：hello_refs(3) + hello_extra(1)。
    ASSERT_EQ(4u, workspace.dexDomain()->classCount());

    auto dbResult = openOrDie(freshDbPath());
    mjbase::project::AnalysisDb& db = dbResult;
    auto ingested = ingestIntoAnalysisDb(workspace, db);
    ASSERT_FALSE(ingested.is_err()) << ingested.unwrap_err().message;
    const auto stats = std::move(ingested).unwrap();
    EXPECT_EQ(4u, stats.classes);
    auto manifestStrings = db.searchResourceStringsPaged(
        {Utf8String::from_cstr("\xe5\xba\x94"), mjbase::project::DbTextMatchKind::Exact, true},
        10, 0);
    ASSERT_FALSE(manifestStrings.is_err());
    const auto manifestPage = std::move(manifestStrings).unwrap();
    ASSERT_EQ(1u, manifestPage.total);
    EXPECT_EQ("AndroidManifest.xml", manifestPage.rows.front().entryPath.to_std_string());

    // 类事实：层次闭包（Subject → Base → Object；Hook 实现者含 Subject）。
    auto found = db.findClass(refOf("com/mjt/dexfix/Subject"));
    ASSERT_FALSE(found.is_err());
    ASSERT_TRUE(std::move(found).unwrap().has_value());
    auto chain = db.superChain(refOf("com/mjt/dexfix/Subject"));
    ASSERT_FALSE(chain.is_err());
    const auto chainRows = std::move(chain).unwrap();
    ASSERT_GE(chainRows.size(), 2u);
    EXPECT_EQ("com/mjt/dexfix/Base", chainRows[1].to_std_string());
    auto implementors = db.implementors(refOf("com/mjt/dexfix/Hook"));
    ASSERT_FALSE(implementors.is_err());
    bool subjectImplements = false;
    for (const Utf8String& row : std::move(implementors).unwrap()) {
        if (row.to_std_string() == "com/mjt/dexfix/Subject") subjectImplements = true;
    }
    EXPECT_TRUE(subjectImplements);

    // 引用事实：invoke-static Base.tag() 的调用方是 Subject.onEvent。
    auto refs = db.referencesToMethod(refOf("com/mjt/dexfix/Base"), refOf("tag"),
                                      refOf("()Ljava/lang/String;"));
    ASSERT_FALSE(refs.is_err());
    const auto refRows = std::move(refs).unwrap();
    ASSERT_FALSE(refRows.empty());
    bool onEventCallsTag = false;
    for (const auto& row : refRows) {
        if (row.sourceClass.to_std_string() == "com/mjt/dexfix/Subject" &&
            row.sourceMethod.to_std_string() == "onEvent") {
            onEventCallsTag = true;
        }
    }
    EXPECT_TRUE(onEventCallsTag);

    // 字符串事实：const-string "hook-fired" 落 String 行。
    auto strings = db.searchStringsPaged(
        mjbase::project::DbStringSearchQuery{
            Utf8String::from_cstr("hook-fired"),
            mjbase::project::DbTextMatchKind::Contains, false, {}, {}},
        50, 0);
    ASSERT_FALSE(strings.is_err());
    EXPECT_GE(std::move(strings).unwrap().total, 1u);

    // 数值事实：Extra.wide 的 const-wide 字面量（方法体切片）。
    auto numbers = db.searchNumbers(mjbase::project::DbNumberQuery{
        Utf8String::from_cstr("1234605616436508552")});
    ASSERT_FALSE(numbers.is_err());
    EXPECT_FALSE(std::move(numbers).unwrap().empty());
}

TEST(MjDexWorkspaceTest, ScansOnlyTheVisibleDefinitionOfADuplicateDexClass)
{
    const auto source = dexResource("hello_extra.dex");
    WorkspaceBase primary;
    ASSERT_FALSE(primary.open(refOf(source.u8string())).is_err());
    auto firstBytes = primary.dexDomain()->assembleSmali(
        "com/mjt/dexfix/Extra",
        ".class public Lcom/mjt/dexfix/Extra;\n.super Ljava/lang/Object;\n");
    ASSERT_FALSE(firstBytes.is_err());
    const auto apk = test::getTempOutPath("mjt_duplicate_dex_class.apk");
    test::writeRawZip(apk, {{"classes.dex", std::move(firstBytes).unwrap()},
                            {"classes2.dex", readBytes(source)}});

    WorkspaceBase workspace;
    ASSERT_FALSE(workspace.open(refOf(apk.string())).is_err());
    const auto* domain = workspace.dexDomain();
    ASSERT_NE(domain, nullptr);
    ASSERT_EQ(domain->classCount(), 1U);
    auto visibleClass = domain->classInfo("com/mjt/dexfix/Extra");
    ASSERT_FALSE(visibleClass.is_err());
    EXPECT_EQ(std::move(visibleClass).unwrap().resourceUri, "classes.dex");

    const auto refs = domain->buildReferenceScan();
    EXPECT_TRUE(refs.methodReferences().empty());
    auto hits = domain->searchInstructions("const-wide", {});
    ASSERT_FALSE(hits.is_err());
    EXPECT_TRUE(std::move(hits).unwrap().empty());

    auto db = openOrDie(freshDbPath());
    auto ingested = ingestIntoAnalysisDb(workspace, db);
    ASSERT_FALSE(ingested.is_err()) << ingested.unwrap_err().message;
    EXPECT_EQ(std::move(ingested).unwrap().classes, 1U);
}

TEST(MjDexWorkspaceTest, AnswersEngineXrefAndHierarchySurfaces)
{
    WorkspaceBase workspace;
    ASSERT_FALSE(workspace.open(refOf(dexResource("hello_refs.dex").string())).is_err());

    auto refs = workspace.xrefsToMethod(refOf("com/mjt/dexfix/Base"), refOf("tag"),
                                        refOf("()Ljava/lang/String;"));
    ASSERT_FALSE(refs.is_err());
    const auto rows = std::move(refs).unwrap();
    ASSERT_FALSE(rows.empty());
    bool subjectOnEvent = false;
    for (const ReferenceInfo& row : rows) {
        EXPECT_EQ("direct", row.kind.to_std_string());
        if (row.sourceClass.to_std_string() == "com/mjt/dexfix/Subject" &&
            row.sourceMethod.to_std_string() == "onEvent") {
            subjectOnEvent = true;
            EXPECT_EQ("com/mjt/dexfix/Base.tag()Ljava/lang/String;",
                      row.target.to_std_string());
        }
    }
    EXPECT_TRUE(subjectOnEvent);

    auto typeRefs = workspace.xrefsToClass(refOf("com/mjt/dexfix/Hook"));
    ASSERT_FALSE(typeRefs.is_err());
    EXPECT_FALSE(std::move(typeRefs).unwrap().empty());

    auto relations = workspace.classRelations(refOf("com/mjt/dexfix/Subject"));
    ASSERT_FALSE(relations.is_err());
    const auto closure = std::move(relations).unwrap();
    ASSERT_EQ(2u, closure.superclasses.size());
    EXPECT_EQ("com/mjt/dexfix/Base", closure.superclasses[0].to_std_string());
    EXPECT_EQ("java/lang/Object", closure.superclasses[1].to_std_string());
    ASSERT_EQ(1u, closure.interfaces.size());
    EXPECT_EQ("com/mjt/dexfix/Hook", closure.interfaces[0].to_std_string());
    // Subject 是叶：无子类、无人实现它。
    EXPECT_TRUE(closure.directSubclasses.empty());
    EXPECT_TRUE(closure.allSubclasses.empty());
    EXPECT_TRUE(closure.implementors.empty());

    // Base 的子类闭包：Subject（direct 与 all 同收）。
    auto baseRelations = workspace.classRelations(refOf("com/mjt/dexfix/Base"));
    ASSERT_FALSE(baseRelations.is_err());
    const auto baseClosure = std::move(baseRelations).unwrap();
    ASSERT_EQ(1u, baseClosure.directSubclasses.size());
    EXPECT_EQ("com/mjt/dexfix/Subject", baseClosure.directSubclasses[0].to_std_string());
    ASSERT_EQ(1u, baseClosure.allSubclasses.size());
    EXPECT_EQ("com/mjt/dexfix/Subject", baseClosure.allSubclasses[0].to_std_string());

    // Hook 的实现者闭包：Subject（implements 直连，无后代可并）。
    auto hookRelations = workspace.classRelations(refOf("com/mjt/dexfix/Hook"));
    ASSERT_FALSE(hookRelations.is_err());
    bool subjectInImplementors = false;
    for (const Utf8String& row : std::move(hookRelations).unwrap().implementors) {
        if (row.to_std_string() == "com/mjt/dexfix/Subject") subjectInImplementors = true;
    }
    EXPECT_TRUE(subjectInImplementors);

    // callees resolved：factory() 的 invoke-direct 不展开，Subject 构造器
    // 直连；CHA 展开在域内子类出现时收敛（Extra 无关类不混入）。
    auto callees = workspace.calleesOfMethod(refOf("com/mjt/dexfix/Subject"),
                                             refOf("factory"), refOf("()Lcom/mjt/dexfix/Base;"),
                                             false);
    ASSERT_FALSE(callees.is_err());
    bool ctorDirect = false;
    for (const ReferenceInfo& row : std::move(callees).unwrap()) {
        if (row.target.to_std_string().find("com/mjt/dexfix/Subject.<init>()V") !=
            std::string::npos)
            ctorDirect = true;
    }
    EXPECT_TRUE(ctorDirect);
}

TEST(MjDexWorkspaceTest, ServesSmaliViewAndConstantFaces)
{
    // apk 形态：两个 dex 都在域内（Extra 在 classes2.dex，常量切片跨 dex 验证）。
    const auto apkPath = writeFixtureApk();
    WorkspaceBase workspace;
    ASSERT_FALSE(workspace.open(refOf(apkPath.string())).is_err());

    auto smali = workspace.dexDomain()->smaliText("com/mjt/dexfix/Subject");
    ASSERT_FALSE(smali.is_err());
    const std::string text = std::move(smali).unwrap();
    EXPECT_NE(text.find(".class public Lcom/mjt/dexfix/Subject;"), std::string::npos);
    EXPECT_NE(text.find(".super Lcom/mjt/dexfix/Base;"), std::string::npos);
    EXPECT_NE(text.find("const-string"), std::string::npos);
    EXPECT_NE(text.find(".end method"), std::string::npos);

    // 指令搜索：const-string 行命中（含值文本）。
    SearchOptions options;
    auto hits = workspace.searchInstructions(refOf("hook-fired"), options);
    ASSERT_FALSE(hits.is_err());
    const auto rows = std::move(hits).unwrap();
    ASSERT_FALSE(rows.empty());
    EXPECT_EQ("instruction", rows[0].kind.to_std_string());
    EXPECT_EQ("com/mjt/dexfix/Subject", rows[0].owner.to_std_string());

    // 常量事实面（ingest 桥同源）。
    auto strings = workspace.dexDomain()->cpStringRecords("com/mjt/dexfix/Subject");
    ASSERT_FALSE(strings.is_err());
    bool hookFired = false;
    bool selfName = false;
    for (const auto& record : std::move(strings).unwrap()) {
        if (record.value.to_std_string() == "hook-fired" &&
            record.kind == mjbase::project::DbStringKind::String) hookFired = true;
        if (record.value.to_std_string() == "com/mjt/dexfix/Subject" &&
            record.kind == mjbase::project::DbStringKind::Utf8) selfName = true;
    }
    EXPECT_TRUE(hookFired);
    EXPECT_TRUE(selfName);

    auto numbers = workspace.dexDomain()->methodNumberSlices("com/mjt/dexfix/Extra");
    ASSERT_FALSE(numbers.is_err());
    bool wideLiteral = false;
    for (const MethodNumberSlice& slice : std::move(numbers).unwrap()) {
        for (const auto& record : slice.records) {
            if (record.valueText.to_std_string() == "1234605616436508552") wideLiteral = true;
        }
    }
    EXPECT_TRUE(wideLiteral);
}

TEST(MjDexWorkspaceTest, GuardsWriteAndUnsupportedSurfaces)
{
    const auto apkPath = writeFixtureApk();
    WorkspaceBase workspace;
    ASSERT_FALSE(workspace.open(refOf(apkPath.string())).is_err());

    EXPECT_FALSE(workspace.canExport());
    auto exported = workspace.exportArtifact(refOf(test::getTempOutPath("mjt_dex_out.apk").string()));
    EXPECT_TRUE(exported.is_err());
    EXPECT_FALSE(exported.unwrap_err().message.empty());
    // APK support mounts retain their DEX domain. The primary class wins on duplicates.
    auto supporting = workspace.addSupportingRole(refOf(apkPath.string()), refOf("support"));
    ASSERT_FALSE(supporting.is_err()) << supporting.unwrap_err().message;
    EXPECT_EQ(4u, std::move(supporting).unwrap().classCount);
    EXPECT_EQ(1u, workspace.supportingResources().size());
    EXPECT_EQ(4u, workspace.dexDomain()->classCount());
    //readClassBytes 只认 .class 条目：apk 容器的 dex 条目寻址自然 NotFound。
    auto classBytes = readClassBytes(*workspace.workingContainer(), refOf("com/mjt/dexfix/Subject"));
    EXPECT_TRUE(classBytes.is_err());
    //manifest 面走 jar_ops 自由函数（成员已随 openJar 命名面退役）：apk 容器
    //无 MANIFEST.MF 条目 → NotFound（空清单语义在新架构下是错误码）。
    auto manifest = manifestAttributes(*workspace.workingContainer());
    EXPECT_TRUE(manifest.is_err());
    // 引擎编辑面在 dex 类名上 NotFound（MCP 层另有产品级守卫）。
    auto edited = applyEdit(workspace,
                            EditCommand{EditCommand::Jasm{
                                Utf8String::from_cstr("com/mjt/dexfix/Subject"),
                                utf16("Subject.jasm"),
                                utf16("")}},
                            MutationRequest{});
    EXPECT_TRUE(edited.status.is_err());
}
