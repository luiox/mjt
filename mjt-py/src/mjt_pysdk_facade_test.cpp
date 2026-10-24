// mjt_pysdk_facade_test.cpp — v3 facade C++ 直测（契约 §8 验收项的 B1 子集）。
//
// 覆盖：索引装载 / loose class 导入（v2 缺口：新增类全链）/ 子集与单类导出 /
// first-wins 重复名单 / 条目字节往返 / 句柄失效 fail-closed。
// 全程合成样本（writeSyntheticDemoJar），零样本字节入库。

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

#include "pysdk_facade.hpp"
#include "pysdk_jar.hpp"
#include "pysdk_jasm.hpp"
#include "pysdk_limits.hpp"

namespace fs = std::filesystem;
using namespace mjt::pysdk;

namespace {

std::string tmpPath(const char* name)
{
    return (fs::temp_directory_path() / name).u8string();
}

std::string makeDemoJar(const char* name)
{
    auto path = tmpPath(name);
    writeSyntheticDemoJar(path);
    return path;
}

std::vector<ca::u8> readFileBytes(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<ca::u8>((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
}

void writeFileBytes(const std::string& path, const std::vector<ca::u8>& bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

}   // namespace

TEST(FacadeContainer, OpenBuildsSortedIndex)
{
    auto& reg   = FacadeRegistry::instance();
    auto& s     = reg.create();
    facadeImportJar(s, makeDemoJar("mjt_facade_open.jar"));

    EXPECT_EQ(facadeClassNames(s),
              (std::vector<std::string>{"me/demo/Api", "me/demo/App", "me/demo/Color",
                                        "me/demo/Crypto", "me/demo/Util"}));
    EXPECT_TRUE(facadeDuplicateNames(s).empty());
    EXPECT_EQ(facadeClassUri(s, "me/demo/App"), "me/demo/App.class");

    auto bytes = facadeClassBytes(s, "me/demo/App");
    ASSERT_GE(bytes.size(), std::size_t{4});
    EXPECT_EQ(bytes[0], 0xCA);
    EXPECT_EQ(bytes[1], 0xFE);
    EXPECT_EQ(bytes[2], 0xBA);
    EXPECT_EQ(bytes[3], 0xBE);
    reg.close(s.handle);
}

TEST(FacadeContainer, LoadClassFileAddsClass)
{
    // v2 缺口回归位：loose .class 入容器 → 新增类全链（导出 → 重开可见）。
    // 目标罐只含 Crypto/Util（子集导出），App 以 loose 文件装入。
    auto& reg = FacadeRegistry::instance();
    auto& src = reg.create();
    facadeImportJar(src, makeDemoJar("mjt_facade_load_src.jar"));
    const auto loose = tmpPath("mjt_facade_loose_App.class");
    writeFileBytes(loose, facadeClassBytes(src, "me/demo/App"));

    const auto base = tmpPath("mjt_facade_load_dst.jar");
    {
        auto& full = reg.create();
        facadeImportJar(full, makeDemoJar("mjt_facade_load_full.jar"));
        facadeExportJar(full, base, {"me/demo/Crypto.class", "me/demo/Util.class"});
        reg.close(full.handle);
    }

    auto& s = reg.create();
    facadeImportJar(s, base);
    ASSERT_EQ(facadeClassNames(s).size(), std::size_t{2});
    facadeLoadClassFile(s, loose, std::nullopt, false);
    EXPECT_EQ(facadeClassNames(s).size(), std::size_t{3});
    EXPECT_EQ(facadeClassUri(s, "me/demo/App"), "mjt_facade_loose_App.class");

    // 同类再装入第二 URI → first-wins 落选，不新增类名。
    facadeLoadClassFile(s, loose, std::string{"shadow_App.class"}, false);
    EXPECT_EQ(facadeClassNames(s).size(), std::size_t{3});
    EXPECT_EQ(facadeDuplicateNames(s), std::vector<std::string>{"me/demo/App"});

    const auto out = tmpPath("mjt_facade_load_out.jar");
    facadeExportJar(s, out, {});
    auto& reopen = reg.create();
    facadeImportJar(reopen, out);
    EXPECT_EQ(facadeClassNames(reopen).size(), std::size_t{3});
    reg.close(src.handle);
    reg.close(s.handle);
    reg.close(reopen.handle);
}

TEST(FacadeContainer, SetClassBytesAddsNewClass)
{
    // v2 缺口回归位：set_class_bytes 对容器中不存在的类 = 新增
    // （字节内部名必须与目标名一致——fail-closed 防索引错位）。
    auto& reg = FacadeRegistry::instance();
    auto& src = reg.create();
    facadeImportJar(src, makeDemoJar("mjt_facade_add_src.jar"));
    const auto appBytes = facadeClassBytes(src, "me/demo/App");

    const auto base = tmpPath("mjt_facade_add_dst.jar");
    {
        auto& full = reg.create();
        facadeImportJar(full, makeDemoJar("mjt_facade_add_full.jar"));
        facadeExportJar(full, base, {"me/demo/Crypto.class", "me/demo/Util.class"});
        reg.close(full.handle);
    }

    auto& s = reg.create();
    facadeImportJar(s, base);
    ASSERT_EQ(facadeClassNames(s).size(), std::size_t{2});
    facadeSetClassBytes(s, "me/demo/App", appBytes);
    EXPECT_EQ(facadeClassNames(s).size(), std::size_t{3});
    EXPECT_EQ(facadeClassUri(s, "me/demo/App"), "me/demo/App.class");

    // 写非该类字节 → fail-closed。
    EXPECT_THROW(facadeSetClassBytes(s, "me/demo/Other", appBytes), std::runtime_error);
    reg.close(src.handle);
    reg.close(s.handle);
}

TEST(FacadeContainer, RemoveEntryAndPromoteLoser)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = reg.create();
    facadeImportJar(s, makeDemoJar("mjt_facade_rm.jar"));
    const auto utilBytes = facadeClassBytes(s, "me/demo/Util");

    // 同名第二 URI → first-wins 落选。
    facadeAddEntry(s, "shadow/me/demo/Util.class", utilBytes);
    EXPECT_EQ(facadeClassNames(s).size(), std::size_t{5});
    ASSERT_EQ(facadeDuplicateNames(s), std::vector<std::string>{"me/demo/Util"});
    EXPECT_EQ(facadeClassUri(s, "me/demo/Util"), "me/demo/Util.class");

    // winner 移除 → 落选者晋升，类名仍可寻址。
    facadeRemoveEntry(s, "me/demo/Util.class");
    EXPECT_TRUE(facadeDuplicateNames(s).empty());
    EXPECT_EQ(facadeClassUri(s, "me/demo/Util"), "shadow/me/demo/Util.class");
    EXPECT_FALSE(facadeClassBytes(s, "me/demo/Util").empty());
    reg.close(s.handle);
}

TEST(FacadeContainer, EntryBytesRoundtripAndTypes)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = reg.create();
    facadeImportJar(s, makeDemoJar("mjt_facade_entries.jar"));

    // 非类条目：add → list 可见 → 字节往返。
    const std::vector<ca::u8> payload{'h', 'e', 'l', 'l', 'o'};
    facadeAddEntry(s, "META-INF/notes.txt", payload);
    bool sawUnknown = false;
    for (const auto& [uri, type] : facadeListEntries(s)) {
        if (uri == "META-INF/notes.txt") {
            EXPECT_EQ(type, "Unknown");
            sawUnknown = true;
        }
    }
    EXPECT_TRUE(sawUnknown);
    EXPECT_EQ(facadeGetEntryBytes(s, "META-INF/notes.txt"), payload);
    const std::vector<ca::u8> patched{'w', 'o', 'r', 'l', 'd'};
    facadeSetEntryBytes(s, "META-INF/notes.txt", patched);
    EXPECT_EQ(facadeGetEntryBytes(s, "META-INF/notes.txt"), patched);

    EXPECT_THROW(facadeGetEntryBytes(s, "no/such/entry"), StaleRefError);
    EXPECT_THROW(facadeRemoveEntry(s, "no/such/entry"), StaleRefError);
    reg.close(s.handle);
}

TEST(FacadeContainer, ExportSubsetAndSingleClass)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = reg.create();
    facadeImportJar(s, makeDemoJar("mjt_facade_subset.jar"));

    const auto subset = tmpPath("mjt_facade_subset_out.jar");
    facadeExportJar(s, subset, {"me/demo/App.class"});
    auto& reopen = reg.create();
    facadeImportJar(reopen, subset);
    EXPECT_EQ(facadeClassNames(reopen), std::vector<std::string>{"me/demo/App"});
    reg.close(reopen.handle);

    const auto single = tmpPath("mjt_facade_single_App.class");
    facadeExportClass(s, "me/demo/App", single);
    auto probed = facadeProbeClassName(readFileBytes(single));
    ASSERT_TRUE(probed.has_value());
    EXPECT_EQ(*probed, "me/demo/App");
    reg.close(s.handle);
}

TEST(FacadeContainer, StaleHandleFailClosed)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = reg.create();
    const auto h = s.handle;
    reg.close(h);
    EXPECT_THROW(reg.get(h), StaleRefError);

    // 未知句柄同型失败。
    EXPECT_THROW(reg.get(999999), StaleRefError);
}

TEST(FacadeContainer, RemoveUnknownUriFailsClosed)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = reg.create();
    facadeImportJar(s, makeDemoJar("mjt_facade_rm2.jar"));
    EXPECT_THROW(facadeRemoveEntry(s, "nope.class"), StaleRefError);
    reg.close(s.handle);
}

// ═══════════════════════════════════════════════════════════════════════
// 树面（B2）：uid 稳定性 / 墓地 fail-closed / 脏纪律 / rename 迁移。
// ═══════════════════════════════════════════════════════════════════════

namespace {

/// 打开 demo jar 的便捷：返回句柄（注册表持有，用例不复用句柄名冲突即可）。
ca::u64 openDemo(const char* name)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = reg.create();
    facadeImportJar(s, makeDemoJar(name));
    return s.handle;
}

}   // namespace

TEST(FacadeTree, ParseNavigateAndMembers)
{
    const auto h = openDemo("mjt_facade_tree_nav.jar");
    facadeParseClass(FacadeRegistry::instance().get(h), "me/demo/App");
    auto& s = FacadeRegistry::instance().get(h);

    const auto info = facadeClassInfo(s, "me/demo/App");
    EXPECT_EQ(info.name, "me/demo/App");
    EXPECT_EQ(info.superName, "java/lang/Object");

    const auto methods = facadeMembers(s, "me/demo/App", true);
    ASSERT_EQ(methods.size(), std::size_t{3});
    const auto mi = facadeMethodInfo(s, "me/demo/App", "main", "([Ljava/lang/String;)V");
    EXPECT_TRUE((mi.access & jbytecode::core::Opcodes::ACC_STATIC) != 0);
    EXPECT_THROW(facadeMethodInfo(s, "me/demo/App", "nope", "()V"), StaleRefError);
    EXPECT_THROW(facadeParseClass(s, "no/such/Class"), StaleRefError);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, InsnUidStableAcrossStructureEdit)
{
    const auto h = openDemo("mjt_facade_tree_uid.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    const auto count0 = facadeInsnCount(s, "me/demo/Util", "add", "(II)I");
    ASSERT_GT(count0, ca::u32{0});

    const auto insns0 = facadeInsns(s, "me/demo/Util", "add", "(II)I");
    const auto firstUid = facadeFirstInsn(s, "me/demo/Util", "add", "(II)I");
    ASSERT_TRUE(firstUid.has_value());

    // 结构编辑（首指令前插入）：既有 uid 仍解析正确（契约 §8.2）。
    const auto newUid =
        facadeEmitInsn(s, "me/demo/Util", "add", "(II)I", 0 /*nop*/, firstUid);
    EXPECT_EQ(facadeInsnCount(s, "me/demo/Util", "add", "(II)I"), count0 + 1);
    const auto detail0 = facadeInsnDetail(s, "me/demo/Util", "add", "(II)I", *firstUid);
    EXPECT_EQ(detail0.uid, *firstUid);
    EXPECT_EQ(facadeInsnIndex(s, "me/demo/Util", "add", "(II)I", *firstUid), ca::u32{1});

    // 移除新发 uid：墓地生效；邻居 uid 不受影响。
    facadeRemoveInsn(s, "me/demo/Util", "add", "(II)I", newUid);
    EXPECT_THROW(facadeInsnDetail(s, "me/demo/Util", "add", "(II)I", newUid), StaleRefError);
    EXPECT_NO_THROW(facadeInsnDetail(s, "me/demo/Util", "add", "(II)I", *firstUid));
    EXPECT_EQ(facadeInsnCount(s, "me/demo/Util", "add", "(II)I"), count0);
    (void)insns0;
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, RenameClassMigratesIndexAndFailsClosed)
{
    const auto h = openDemo("mjt_facade_tree_rc.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    const auto oldBytes = facadeClassBytes(s, "me/demo/Util");

    const auto newName = facadeRenameClass(s, "me/demo/Util", "me/demo/Helper");
    EXPECT_EQ(newName, "me/demo/Helper");
    // URI 不变（条目地址与类名解耦）、索引键迁移。
    EXPECT_EQ(facadeClassUri(s, "me/demo/Helper"), "me/demo/Util.class");
    EXPECT_FALSE(facadeClassBytes(s, "me/demo/Helper").empty());
    // 旧键 fail-closed（契约 §8.3）。
    EXPECT_THROW(facadeParseClass(s, "me/demo/Util"), StaleRefError);
    EXPECT_THROW(facadeClassBytes(s, "me/demo/Util"), StaleRefError);
    // 冲突 fail-closed。
    EXPECT_THROW(facadeRenameClass(s, "me/demo/Helper", "me/demo/App"), StaleRefError);
    // 树随条目迁移（uid 状态存活）：改名后脏树可写回（字节自然含新类名，大小可变）。
    EXPECT_NO_THROW(facadeWriteBack(s, "me/demo/Helper", false));
    EXPECT_FALSE(facadeClassBytes(s, "me/demo/Helper").empty());
    (void)oldBytes;
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, RenameMethodReturnsNewKeyOldFails)
{
    const auto h = openDemo("mjt_facade_tree_rm.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    facadeRenameMethod(s, "me/demo/Util", "add", "(II)I", "plus");
    EXPECT_THROW(facadeMethodInfo(s, "me/demo/Util", "add", "(II)I"), StaleRefError);
    const auto mi = facadeMethodInfo(s, "me/demo/Util", "plus", "(II)I");
    EXPECT_EQ(mi.name, "plus");
    // 冲突 fail-closed：新增 (II)I 的 x 后，plus→x 改名被拒。
    facadeAddMethod(s, "me/demo/Util", "x", "(II)I", 0x0008, 2, 2);
    EXPECT_THROW(facadeRenameMethod(s, "me/demo/Util", "plus", "(II)I", "x"), StaleRefError);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, DirtyDisciplineSaveAndReopen)
{
    const auto h  = openDemo("mjt_facade_tree_save.jar");
    auto& s       = FacadeRegistry::instance().get(h);
    const auto cryptoBytesBefore = facadeClassBytes(s, "me/demo/Crypto");

    facadeParseClass(s, "me/demo/Util");
    facadeRenameMethod(s, "me/demo/Util", "add", "(II)I", "plus");

    const auto out = tmpPath("mjt_facade_tree_save_out.jar");
    facadeSave(s, out, true);
    // 未触碰类字节原样导出（零写出成本的可观测面）。
    EXPECT_EQ(facadeClassBytes(s, "me/demo/Crypto"), cryptoBytesBefore);

    auto& reopen = FacadeRegistry::instance().create();
    facadeImportJar(reopen, out);
    const auto methods = facadeMembers(reopen, "me/demo/Util", true);
    bool hasPlus = false, hasAdd = false;
    for (const auto& [n, d, a] : methods) {
        if (n == "plus") hasPlus = true;
        if (n == "add") hasAdd = true;
    }
    EXPECT_TRUE(hasPlus);
    EXPECT_FALSE(hasAdd);
    FacadeRegistry::instance().close(h);
    FacadeRegistry::instance().close(reopen.handle);
}

TEST(FacadeTree, AddMethodEmitBodyWriteBackFullChain)
{
    const auto h = openDemo("mjt_facade_tree_add.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    facadeAddMethod(s, "me/demo/Util", "noise", "()V", 0x0009 /*PUBLIC|STATIC*/, 0, 0);
    const auto retUid = facadeEmitInsn(s, "me/demo/Util", "noise", "()V", 0xB1 /*return*/, std::nullopt);
    EXPECT_NE(retUid, ca::u32{0});

    const auto out = tmpPath("mjt_facade_tree_add_out.jar");
    facadeSave(s, out, true);
    auto& reopen = FacadeRegistry::instance().create();
    facadeImportJar(reopen, out);
    facadeParseClass(reopen, "me/demo/Util");
    EXPECT_EQ(facadeInsnCount(reopen, "me/demo/Util", "noise", "()V"), ca::u32{1});
    FacadeRegistry::instance().close(h);
    FacadeRegistry::instance().close(reopen.handle);
}

TEST(FacadeTree, TryCatchRowsRoundtrip)
{
    const auto h = openDemo("mjt_facade_tree_tc.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/App");
    auto rows = facadeTryCatches(s, "me/demo/App", "guarded", "()V");
    ASSERT_EQ(rows.size(), std::size_t{1});
    const auto [s0, e0, h0, type] = rows[0];
    EXPECT_EQ(type, "java/io/IOException");
    EXPECT_NE(s0, ca::u32{0});
    EXPECT_NE(e0, ca::u32{0});
    EXPECT_NE(h0, ca::u32{0});

    // 删行 → 重行（label 节点仍在）→ 行数复原 + COMPUTE_FRAMES 可写。
    facadeRemoveTryCatch(s, "me/demo/App", "guarded", "()V", 0);
    EXPECT_TRUE(facadeTryCatches(s, "me/demo/App", "guarded", "()V").empty());
    facadeAddTryCatch(s, "me/demo/App", "guarded", "()V", s0, e0, h0, "java/io/IOException");
    EXPECT_EQ(facadeTryCatches(s, "me/demo/App", "guarded", "()V").size(), std::size_t{1});
    EXPECT_NO_THROW(facadeWriteBack(s, "me/demo/App", true));
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, EmitLabelJumpLdcChain)
{
    const auto h = openDemo("mjt_facade_tree_emit.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    facadeAddMethod(s, "me/demo/Util", "branchy", "(I)I", 0x0009, 2, 1);
    // branchy: iload 0; ifeq L; iconst_1; goto Lend; L: iconst_0; Lend: ireturn
    const auto load  = facadeEmitVar(s, "me/demo/Util", "branchy", "(I)I", 0x15 /*iload*/, 0, std::nullopt);
    const auto label = facadeEmitLabel(s, "me/demo/Util", "branchy", "(I)I", std::nullopt);
    facadeEmitJump(s, "me/demo/Util", "branchy", "(I)I", 0x99 /*ifeq*/, label, std::nullopt);
    facadeEmitInsn(s, "me/demo/Util", "branchy", "(I)I", 0x04 /*iconst_1*/, std::nullopt);
    const auto lend = facadeEmitLabel(s, "me/demo/Util", "branchy", "(I)I", std::nullopt);
    const auto go   = facadeEmitJump(s, "me/demo/Util", "branchy", "(I)I", 0xA7 /*goto*/, lend, std::nullopt);
    const auto l1   = facadeEmitInsn(s, "me/demo/Util", "branchy", "(I)I", 0x03 /*iconst_0*/, lend);
    facadeEmitInsn(s, "me/demo/Util", "branchy", "(I)I", 0xAC /*ireturn*/, std::nullopt);
    // 前向跳转目标 uid 可改指（set_target）。
    facadeInsnSetTarget(s, "me/demo/Util", "branchy", "(I)I", go, lend);
    EXPECT_NO_THROW(facadeWriteBack(s, "me/demo/Util", true));
    (void)load;
    (void)l1;
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, EmitTableSwitchFullChain)
{
    const auto h = openDemo("mjt_facade_tree_tswitch.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    facadeAddMethod(s, "me/demo/Util", "pick", "(I)I", 0x0009, 1, 1);
    // pick: iload 0; tableswitch(0..3 -> B0..B3 default Bd); Bd: iconst_m1;
    // B0..B3: iconst_N; goto Lend; Lend: ireturn
    const auto load = facadeEmitVar(s, "me/demo/Util", "pick", "(I)I", 0x15 /*iload*/, 0, std::nullopt);
    const auto bd   = facadeEmitLabel(s, "me/demo/Util", "pick", "(I)I", std::nullopt);
    const auto b0   = facadeEmitLabel(s, "me/demo/Util", "pick", "(I)I", std::nullopt);
    const auto b1   = facadeEmitLabel(s, "me/demo/Util", "pick", "(I)I", std::nullopt);
    const auto b2   = facadeEmitLabel(s, "me/demo/Util", "pick", "(I)I", std::nullopt);
    const auto b3   = facadeEmitLabel(s, "me/demo/Util", "pick", "(I)I", std::nullopt);
    const auto lend = facadeEmitLabel(s, "me/demo/Util", "pick", "(I)I", std::nullopt);
    const auto sw =
        facadeEmitTableSwitch(s, "me/demo/Util", "pick", "(I)I", 0, 3, bd, {b0, b1, b2, b3}, bd);
    // 各 label 体 = 插在「下一 label 之前」（label 之间即上一个体的领地）。
    facadeEmitInsn(s, "me/demo/Util", "pick", "(I)I", 0x02 /*iconst_m1*/, b0);
    facadeEmitJump(s, "me/demo/Util", "pick", "(I)I", 0xA7 /*goto*/, lend, b0);
    facadeEmitInsn(s, "me/demo/Util", "pick", "(I)I", 0x03 /*iconst_0*/, b1);
    facadeEmitJump(s, "me/demo/Util", "pick", "(I)I", 0xA7, lend, b1);
    facadeEmitInsn(s, "me/demo/Util", "pick", "(I)I", 0x04 /*iconst_1*/, b2);
    facadeEmitJump(s, "me/demo/Util", "pick", "(I)I", 0xA7, lend, b2);
    facadeEmitInsn(s, "me/demo/Util", "pick", "(I)I", 0x05 /*iconst_2*/, b3);
    facadeEmitJump(s, "me/demo/Util", "pick", "(I)I", 0xA7, lend, b3);
    facadeEmitInsn(s, "me/demo/Util", "pick", "(I)I", 0x06 /*iconst_3*/, lend);
    facadeEmitJump(s, "me/demo/Util", "pick", "(I)I", 0xA7, lend, lend);
    facadeEmitInsn(s, "me/demo/Util", "pick", "(I)I", 0xAC /*ireturn*/, std::nullopt);

    // 读侧展开：min/max/default/targets 全数可读（契约 §2.4 switch 字段）。
    const auto d = facadeInsnDetail(s, "me/demo/Util", "pick", "(I)I", sw);
    EXPECT_EQ(d.opcode, 0xAA);
    EXPECT_EQ(d.kind, jbytecode::tree::AbstractInsnNode::TABLESWITCH_INSN);
    EXPECT_EQ(std::string(facadeInsnKindName(d.kind)), "tableswitch");
    EXPECT_EQ(d.switchMin, 0);
    EXPECT_EQ(d.switchMax, 3);
    EXPECT_EQ(d.defaultUid, bd);
    EXPECT_EQ(d.targetUids, (std::vector<ca::u32>{b0, b1, b2, b3}));

    // uid 稳定性：头插新指令后 switch 及其目标的 uid 解析不变（契约 §2.4）。
    facadeEmitInsn(s, "me/demo/Util", "pick", "(I)I", 0x00 /*nop*/, load);
    const auto d2 = facadeInsnDetail(s, "me/demo/Util", "pick", "(I)I", sw);
    EXPECT_EQ(d2.uid, sw);
    EXPECT_EQ(d2.targetUids, d.targetUids);
    EXPECT_EQ(d2.defaultUid, bd);

    // 写回 + 重开：switch 形态穿越 ClassWriter 往返保真。
    EXPECT_NO_THROW(facadeWriteBack(s, "me/demo/Util", true));
    const auto out = tmpPath("mjt_facade_tree_tswitch_out.jar");
    facadeSave(s, out, true);
    auto& reg    = FacadeRegistry::instance();
    auto& reopen = reg.create();
    facadeImportJar(reopen, out);
    facadeParseClass(reopen, "me/demo/Util");
    ca::u32 reopenSwitch = 0;
    for (const auto& b : facadeInsns(reopen, "me/demo/Util", "pick", "(I)I")) {
        if (b.opcode == 0xAA) reopenSwitch = b.uid;
    }
    ASSERT_NE(reopenSwitch, ca::u32{0});
    const auto dr = facadeInsnDetail(reopen, "me/demo/Util", "pick", "(I)I", reopenSwitch);
    EXPECT_EQ(dr.switchMin, 0);
    EXPECT_EQ(dr.switchMax, 3);
    EXPECT_EQ(dr.targetUids.size(), std::size_t{4});
    EXPECT_NE(dr.defaultUid, ca::u32{0});
    for (const auto t : dr.targetUids) {
        EXPECT_NE(t, ca::u32{0});
        EXPECT_NE(t, dr.defaultUid);
    }
    reg.close(reopen.handle);

    // 墓地：remove 后 uid 解析 fail-closed，链数回落。
    facadeRemoveInsn(s, "me/demo/Util", "pick", "(I)I", sw);
    EXPECT_THROW(facadeInsnDetail(s, "me/demo/Util", "pick", "(I)I", sw), StaleRefError);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, EmitLookupSwitchFullChain)
{
    const auto h = openDemo("mjt_facade_tree_lswitch.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    facadeAddMethod(s, "me/demo/Util", "pick2", "(I)I", 0x0009, 1, 1);
    // pick2: iload 0; lookupswitch{10->C0, 20->C1 default Cd}; C0: iconst_0;
    // goto Lend; C1: iconst_1; goto Lend; Cd: iconst_m1; Lend: ireturn
    facadeEmitVar(s, "me/demo/Util", "pick2", "(I)I", 0x15, 0, std::nullopt);
    const auto cd   = facadeEmitLabel(s, "me/demo/Util", "pick2", "(I)I", std::nullopt);
    const auto c0   = facadeEmitLabel(s, "me/demo/Util", "pick2", "(I)I", std::nullopt);
    const auto c1   = facadeEmitLabel(s, "me/demo/Util", "pick2", "(I)I", std::nullopt);
    const auto lend = facadeEmitLabel(s, "me/demo/Util", "pick2", "(I)I", std::nullopt);
    const auto sw =
        facadeEmitLookupSwitch(s, "me/demo/Util", "pick2", "(I)I", {10, 20}, cd, {c0, c1}, cd);
    facadeEmitInsn(s, "me/demo/Util", "pick2", "(I)I", 0x02 /*iconst_m1*/, c0);
    facadeEmitJump(s, "me/demo/Util", "pick2", "(I)I", 0xA7, lend, c0);
    facadeEmitInsn(s, "me/demo/Util", "pick2", "(I)I", 0x03 /*iconst_0*/, c1);
    facadeEmitJump(s, "me/demo/Util", "pick2", "(I)I", 0xA7, lend, c1);
    facadeEmitInsn(s, "me/demo/Util", "pick2", "(I)I", 0x04 /*iconst_1*/, lend);
    facadeEmitJump(s, "me/demo/Util", "pick2", "(I)I", 0xA7, lend, lend);
    facadeEmitInsn(s, "me/demo/Util", "pick2", "(I)I", 0xAC /*ireturn*/, std::nullopt);

    const auto d = facadeInsnDetail(s, "me/demo/Util", "pick2", "(I)I", sw);
    EXPECT_EQ(d.opcode, 0xAB);
    EXPECT_EQ(d.kind, jbytecode::tree::AbstractInsnNode::LOOKUPSWITCH_INSN);
    EXPECT_EQ(std::string(facadeInsnKindName(d.kind)), "lookupswitch");
    EXPECT_EQ(d.switchKeys, (std::vector<int>{10, 20}));
    EXPECT_EQ(d.defaultUid, cd);
    EXPECT_EQ(d.targetUids, (std::vector<ca::u32>{c0, c1}));

    // 与 emit_jump 同方法组合 + COMPUTE_FRAMES 写出（表尾直落 default 体）。
    EXPECT_NO_THROW(facadeWriteBack(s, "me/demo/Util", true));
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, EmitSwitchFailsClosedOnStructuralErrors)
{
    const auto h = openDemo("mjt_facade_tswitch_bad.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    facadeAddMethod(s, "me/demo/Util", "sw", "(I)I", 0x0009, 1, 1);
    const auto load = facadeEmitVar(s, "me/demo/Util", "sw", "(I)I", 0x15, 0, std::nullopt);
    const auto bd   = facadeEmitLabel(s, "me/demo/Util", "sw", "(I)I", std::nullopt);
    const auto b0   = facadeEmitLabel(s, "me/demo/Util", "sw", "(I)I", std::nullopt);
    const auto b1   = facadeEmitLabel(s, "me/demo/Util", "sw", "(I)I", std::nullopt);

    // table：表项数 ≠ max-min+1。
    EXPECT_THROW(facadeEmitTableSwitch(s, "me/demo/Util", "sw", "(I)I", 0, 3, bd, {b0, b1},
                                       std::nullopt),
                 std::runtime_error);
    // lookup：keys/targets 不成对。
    EXPECT_THROW(facadeEmitLookupSwitch(s, "me/demo/Util", "sw", "(I)I", {1, 2}, bd, {b0},
                                        std::nullopt),
                 std::runtime_error);
    // lookup：keys 非严格升序（乱序/重复都拒）。
    EXPECT_THROW(facadeEmitLookupSwitch(s, "me/demo/Util", "sw", "(I)I", {2, 1}, bd, {b0, b1},
                                        std::nullopt),
                 std::runtime_error);
    EXPECT_THROW(facadeEmitLookupSwitch(s, "me/demo/Util", "sw", "(I)I", {1, 1}, bd, {b0, b1},
                                        std::nullopt),
                 std::runtime_error);
    // 目标非 label 节点（default 与 targets 双向校验）。
    EXPECT_THROW(facadeEmitTableSwitch(s, "me/demo/Util", "sw", "(I)I", 0, 0, load, {b0},
                                       std::nullopt),
                 std::runtime_error);
    EXPECT_THROW(facadeEmitTableSwitch(s, "me/demo/Util", "sw", "(I)I", 0, 0, bd, {load},
                                       std::nullopt),
                 std::runtime_error);
    // 校验全部发生在发射前：链长不变、无半发射残留。
    EXPECT_EQ(facadeInsnCount(s, "me/demo/Util", "sw", "(I)I"), ca::u32{4});
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, LdcStringPayloadInternedAndDeterministic)
{
    // 悬垂视图防线：字符串 ldc 载荷必须指向会话寿命的存储（绑定层曾以临时
    // std::string 的 c_str 悬垂视图构造 Any——写出的类字节随堆残留物漂移）。
    // 回归位：发射 → 读侧 cst 回读 → 写回；两个独立会话同程序字节恒等。
    auto build = [](const char* jarName) {
        const auto h   = openDemo(jarName);
        auto&      s   = FacadeRegistry::instance().get(h);
        facadeParseClass(s, "me/demo/Util");
        facadeAddMethod(s, "me/demo/Util", "msg", "()Ljava/lang/String;", 0x0009, 1, 0);
        const auto ld =
            facadeEmitLdc(s, "me/demo/Util", "msg", "()Ljava/lang/String;",
                          facadeInternedStr(s, "deterministic-payload"), std::nullopt);
        facadeEmitInsn(s, "me/demo/Util", "msg", "()Ljava/lang/String;",
                       0xB0 /*areturn*/, std::nullopt);
        const auto d = facadeInsnDetail(s, "me/demo/Util", "msg", "()Ljava/lang/String;", ld);
        EXPECT_EQ(d.cstTag, 1);
        EXPECT_EQ(d.cstStr, "deterministic-payload");
        EXPECT_NO_THROW(facadeWriteBack(s, "me/demo/Util", true));
        const auto bytes = facadeClassBytes(s, "me/demo/Util");
        FacadeRegistry::instance().close(h);
        return bytes;
    };
    const auto a = build("mjt_facade_ldc_a.jar");
    const auto b = build("mjt_facade_ldc_b.jar");
    EXPECT_EQ(a, b);

    // 全链：save → 重开 → insn_detail 读回 cst 逐字节等值（COMPUTE_FRAMES 写出）。
    const auto out = tmpPath("mjt_facade_ldc_round.jar");
    {
        const auto h = openDemo("mjt_facade_ldc_c.jar");
        auto& s      = FacadeRegistry::instance().get(h);
        facadeParseClass(s, "me/demo/Util");
        facadeAddMethod(s, "me/demo/Util", "msg", "()Ljava/lang/String;", 0x0009, 1, 0);
        facadeEmitLdc(s, "me/demo/Util", "msg", "()Ljava/lang/String;",
                      facadeInternedStr(s, "deterministic-payload"), std::nullopt);
        facadeEmitInsn(s, "me/demo/Util", "msg", "()Ljava/lang/String;",
                       0xB0 /*areturn*/, std::nullopt);
        facadeSave(s, out, true);
        FacadeRegistry::instance().close(h);
    }
    {
        auto& reg    = FacadeRegistry::instance();
        auto& reopen = reg.create();
        facadeImportJar(reopen, out);
        facadeParseClass(reopen, "me/demo/Util");
        ca::u32 ldcUid = 0;
        for (const auto& it : facadeInsns(reopen, "me/demo/Util", "msg", "()Ljava/lang/String;")) {
            if (std::string(facadeInsnKindName(it.kind)) == "ldc") ldcUid = it.uid;
        }
        ASSERT_NE(ldcUid, ca::u32{0});
        const auto d =
            facadeInsnDetail(reopen, "me/demo/Util", "msg", "()Ljava/lang/String;", ldcUid);
        EXPECT_EQ(d.cstTag, 1);
        EXPECT_EQ(d.cstStr, "deterministic-payload");
        reg.close(reopen.handle);
    }
}

TEST(FacadeTree, SetClassBytesInvalidatesCache)
{
    const auto h = openDemo("mjt_facade_tree_inv.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    facadeRenameMethod(s, "me/demo/Util", "add", "(II)I", "plus");
    EXPECT_NO_THROW(facadeMethodInfo(s, "me/demo/Util", "plus", "(II)I"));
    // 外部改字节 → 缓存作废（契约 §3.4）。
    facadeSetClassBytes(s, "me/demo/Util", facadeClassBytes(s, "me/demo/Util"));
    // 重解析后树来自新字节——此前未 write_back 的脏改动不复存在（fail-closed 语义，
    // 这里名字回到原样即为作废证据——原字节里是 add）。
    bool hasAdd = false;
    for (const auto& [n, d, a] : facadeMembers(s, "me/demo/Util", true)) {
        if (n == "add") hasAdd = true;
    }
    EXPECT_TRUE(hasAdd);
    FacadeRegistry::instance().close(h);
}

// ═══════════════════════════════════════════════════════════════════════
// 实测补刀批：rename 条目落账（G2）/ verify_container / sanitize_lvt（G1）/
// 批量改名（CLI rename --suggestions 等位）/ shadow 显示层（#1072）。
// ═══════════════════════════════════════════════════════════════════════

TEST(FacadeTree, RenameSyncsEntryOnWriteBack)
{
    const auto h = openDemo("mjt_facade_sync_wb.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeRenameClass(s, "me/demo/Util", "me/demo/Renamed");
    // 挂账未落：索引已迁移、条目未动。
    EXPECT_EQ(facadeClassUri(s, "me/demo/Renamed"), "me/demo/Util.class");
    facadeWriteBack(s, "me/demo/Renamed", false);
    // 写回落账：条目搬移 + 字节声明名 = 新名。
    EXPECT_EQ(facadeClassUri(s, "me/demo/Renamed"), "me/demo/Renamed.class");
    EXPECT_THROW(facadeClassUri(s, "me/demo/Util"), StaleRefError);
    const auto probed = facadeProbeClassName(facadeClassBytes(s, "me/demo/Renamed"));
    ASSERT_TRUE(probed);
    EXPECT_EQ(*probed, "me/demo/Renamed");
    EXPECT_NE(s.container->getResource("me/demo/Renamed.class"), nullptr);
    EXPECT_EQ(s.container->getResource("me/demo/Util.class"), nullptr);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeContainer, ExportAutoFlushesRenameSync)
{
    const auto h = openDemo("mjt_facade_sync_export.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeRenameClass(s, "me/demo/Util", "me/demo/Renamed");
    // 不 flush 直接 export——脏树与挂账改名必须被自动落账（G2 回归位）。
    facadeExportJar(s, tmpPath("mjt_facade_sync_out.jar"), {});
    auto& reg = FacadeRegistry::instance();
    auto& s2  = reg.create();
    facadeImportJar(s2, tmpPath("mjt_facade_sync_out.jar"));
    EXPECT_EQ(facadeClassNames(s2),
              (std::vector<std::string>{"me/demo/Api", "me/demo/App", "me/demo/Color",
                                        "me/demo/Crypto", "me/demo/Renamed"}));
    EXPECT_EQ(facadeClassUri(s2, "me/demo/Renamed"), "me/demo/Renamed.class");
    const auto rep = facadeVerifyContainer(s2);
    EXPECT_EQ(rep.match, 5u);
    EXPECT_EQ(rep.mismatch, 0u);
    EXPECT_EQ(rep.malformed, 0u);
    reg.close(s2.handle);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeContainer, VerifyContainerCountsMismatchMalformedDeps)
{
    const auto h = openDemo("mjt_facade_verify.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    {
        const auto rep = facadeVerifyContainer(s);
        EXPECT_EQ(rep.match, 5u);
        EXPECT_EQ(rep.mismatch, 0u);
        EXPECT_EQ(rep.malformed, 0u);
        EXPECT_GE(rep.nonClass, std::size_t{1});   // 目录条目（+/MANIFEST）
    }
    // mismatch：条目字节声明的类名与 entry 路径不符。
    facadeSetEntryBytes(s, "me/demo/Util.class", facadeClassBytes(s, "me/demo/App"));
    // malformed：坏字节。
    facadeSetEntryBytes(s, "me/demo/Crypto.class", {0xCA, 0xFE, 0xBA, 0xBE, 0x00, 0xFF});
    const auto rep = facadeVerifyContainer(s);
    EXPECT_EQ(rep.match, 3u);
    EXPECT_EQ(rep.mismatch, 1u);
    EXPECT_EQ(rep.malformed, 1u);
    // dep_missing：条目移除后引用悬空（App 引用 Util/Crypto）。
    facadeRemoveEntry(s, "me/demo/Util.class");
    const auto rep2 = facadeVerifyContainer(s);
    EXPECT_GE(rep2.depMissing, std::size_t{1});
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, SanitizeLvtDropsUnlinkedAndRoundtrips)
{
    namespace jt = jbytecode::tree;
    const auto h = openDemo("mjt_facade_lvt.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    // 干净基线：demo jar 无 LVT 条目。
    {
        const auto rep = facadeSanitizeLvt(s);
        EXPECT_EQ(rep.dropped, 0u);
        EXPECT_EQ(rep.deduped, 0u);
        EXPECT_EQ(rep.classes, 5u);
    }
    // 合成带 LVT 的类：L_start; iconst_1; istore_0; iload_0; ireturn; L_end
    // + LVT slot0 [L_start, L_end)。
    ca::str::Utf8StringArena arena;
    auto       node = std::make_unique<jt::ClassNode>();
    node->access    = 0x0021;   // PUBLIC|SUPER
    node->version   = 52;
    node->name      = arena.intern(ca::str::Utf8StringRef::from_cstr("me/demo/LvtDemo"));
    node->superName = arena.intern(ca::str::Utf8StringRef::from_cstr("java/lang/Object"));
    auto* m         = new jt::MethodNode(
        0x0009, arena.intern(ca::str::Utf8StringRef::from_cstr("lv")),
        arena.intern(ca::str::Utf8StringRef::from_cstr("()V")), ca::str::Utf8StringRef{}, {});
    m->maxStack  = 1;
    m->maxLocals = 1;
    auto* label  = m->arena().create<jbytecode::core::Label>();
    label->setOwner(&m->arena());
    m->instructions.add(m->create<jt::LabelNode>(label));
    m->instructions.add(m->create<jt::InsnNode>(0x04));    // iconst_1
    m->instructions.add(m->create<jt::VarInsnNode>(0x3C, 0)); // istore_0
    m->instructions.add(m->create<jt::VarInsnNode>(0x1A, 0)); // iload_0
    m->instructions.add(m->create<jt::InsnNode>(0xAC));    // ireturn
    m->instructions.add(m->create<jt::LabelNode>(m->arena().create<jbytecode::core::Label>()));
    auto* lv = m->create<jt::LocalVariableNode>(
        arena.intern(ca::str::Utf8StringRef::from_cstr("v")),
        arena.intern(ca::str::Utf8StringRef::from_cstr("I")), ca::str::Utf8StringRef{}, label,
        static_cast<jt::LabelNode*>(m->instructions.getLast())->label, 0);
    m->localVariables.push_back(lv);
    node->methods.push_back(m);
    jbytecode::core::ClassWriter writer(arena, 0);
    node->accept(&writer);
    const auto bytes = writer.toByteArray();
    ASSERT_FALSE(bytes.empty());
    const auto lvtPath = tmpPath("mjt_facade_lvt.class");
    writeFileBytes(lvtPath, bytes);
    facadeLoadClassFile(s, lvtPath, std::nullopt, false);
    {
        const auto rep = facadeSanitizeLvt(s);
        EXPECT_EQ(rep.classes, 6u);
        EXPECT_EQ(rep.dropped, 0u);
    }
    // G1 形态：LVT start 标签所在指令被删 → 标签脱链 → 清洗剔除该条目，
    // 之后 COMPUTE_FRAMES 写出可过（未清洗前 writer 会生成坏 start_pc）。
    const auto firstUid = facadeFirstInsn(s, "me/demo/LvtDemo", "lv", "()V");
    ASSERT_TRUE(firstUid);
    facadeRemoveInsn(s, "me/demo/LvtDemo", "lv", "()V", *firstUid);
    {
        const auto rep = facadeSanitizeLvt(s);
        EXPECT_EQ(rep.dropped, 1u);
    }
    EXPECT_NO_THROW(facadeWriteBack(s, "me/demo/LvtDemo", true));
    facadeExportClass(s, "me/demo/LvtDemo", tmpPath("mjt_facade_lvt_out.class"));
    const auto probed = facadeProbeClassName(readFileBytes(tmpPath("mjt_facade_lvt_out.class")));
    ASSERT_TRUE(probed);
    EXPECT_EQ(*probed, "me/demo/LvtDemo");
    FacadeRegistry::instance().close(h);
}

TEST(FacadeContainer, RenameApplySuggestionsFlow)
{
    const auto h = openDemo("mjt_facade_sugg.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    const std::string json = R"({
        "version": 1,
        "source": "ai:test",
        "proposals": [
            {"kind": "class", "name": "me/demo/Util",
             "newName": "me/demo/Helper", "confidence": 1.0}
        ]
    })";
    const auto path = tmpPath("mjt_facade_sugg.json");
    writeFileBytes(path, {json.begin(), json.end()});
    const auto rep = facadeRenameApplySuggestions(s, path, "all");
    EXPECT_EQ(rep.proposed, std::size_t{1});
    EXPECT_EQ(rep.accepted, std::size_t{1});
    EXPECT_EQ(facadeClassNames(s),
              (std::vector<std::string>{"me/demo/Api", "me/demo/App", "me/demo/Color",
                                        "me/demo/Crypto", "me/demo/Helper"}));
    EXPECT_EQ(facadeClassUri(s, "me/demo/Helper"), "me/demo/Helper.class");
    const auto [c, m, f] = facadeMappingCounts(s);
    EXPECT_EQ(c, std::size_t{1});
    FacadeRegistry::instance().close(h);
}

TEST(FacadeContainer, RenameApplySuggestionsRejectedEntriesCarryCollision)
{
    // fail-closed 报错面：跨类撞名拒收时被拒流水自含两侧坐标
    // （owner/old/new + 撞名对象定位），脚本可直接读，不再只拿计数。
    const auto h = openDemo("mjt_facade_sugg_reject.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    const std::string json = R"({
        "version": 1,
        "source": "ai:test",
        "proposals": [
            {"kind": "class", "name": "me/demo/Util",
             "newName": "me/demo/Taken", "confidence": 1.0},
            {"kind": "class", "name": "me/demo/Api",
             "newName": "me/demo/Taken", "confidence": 1.0}
        ]
    })";
    const auto path = tmpPath("mjt_facade_sugg_reject.json");
    writeFileBytes(path, {json.begin(), json.end()});
    const auto rep = facadeRenameApplySuggestions(s, path, "bytecode-only");
    EXPECT_EQ(rep.proposed, std::size_t{2});
    EXPECT_EQ(rep.accepted, std::size_t{1});
    EXPECT_EQ(rep.rejected, std::size_t{1});
    ASSERT_EQ(rep.rejectedEntries.size(), std::size_t{1});
    const auto& entry = rep.rejectedEntries.front();
    EXPECT_EQ(entry.kind, "class");
    EXPECT_EQ(entry.name, "me/demo/Api");
    EXPECT_EQ(entry.newName, "me/demo/Taken");
    EXPECT_EQ(entry.reason, "conflict");
    // detail 自含两侧：撞名对象（在位者）与被拒建议的坐标都在文案里。
    EXPECT_NE(entry.detail.find("me/demo/Util"), std::string::npos);
    EXPECT_NE(entry.detail.find("me/demo/Taken"), std::string::npos);
    EXPECT_NE(entry.detail.find("target already claimed"), std::string::npos);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, LabelIndexGraveyardAndReemit)
{
    // Label*→uid 反查缓存（insn_detail 跳转/switch 读侧定点优化）的失效纪律：
    // 目标 label 摘链进墓地 → 该目标读侧回 0、其余目标/default 不受影响；
    // 之后新发射的 label 必须立即可解析（emit 置脏重建路径）。
    const auto h = openDemo("mjt_facade_labelidx.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    facadeAddMethod(s, "me/demo/Util", "lx", "(I)I", 0x0009, 1, 1);
    facadeEmitVar(s, "me/demo/Util", "lx", "(I)I", 0x15, 0, std::nullopt);
    const auto bd = facadeEmitLabel(s, "me/demo/Util", "lx", "(I)I", std::nullopt);
    const auto b0 = facadeEmitLabel(s, "me/demo/Util", "lx", "(I)I", std::nullopt);
    const auto b1 = facadeEmitLabel(s, "me/demo/Util", "lx", "(I)I", std::nullopt);
    const auto sw = facadeEmitTableSwitch(s, "me/demo/Util", "lx", "(I)I", 0, 1, bd, {b0, b1}, bd);

    // 预读一次：消费掉 emit 残留的缓存脏标，确保下方 remove 的置脏独立受检
    // （否则陈旧缓存会被 emit 的脏标掩盖，remove 置脏行失去回归保护）。
    {
        const auto d = facadeInsnDetail(s, "me/demo/Util", "lx", "(I)I", sw);
        ASSERT_EQ(d.targetUids[0], b0);
    }

    // 摘链 b0（墓地）：switch 读侧该目标回 0，default 与 b1 原样。
    facadeRemoveInsn(s, "me/demo/Util", "lx", "(I)I", b0);
    {
        const auto d = facadeInsnDetail(s, "me/demo/Util", "lx", "(I)I", sw);
        EXPECT_EQ(d.defaultUid, bd);
        ASSERT_EQ(d.targetUids.size(), std::size_t{2});
        EXPECT_EQ(d.targetUids[0], ca::u32{0});
        EXPECT_EQ(d.targetUids[1], b1);
    }
    // 墓地目标 uid 自身解析 fail-closed（既有纪律不因缓存而松动）。
    EXPECT_THROW(facadeInsnDetail(s, "me/demo/Util", "lx", "(I)I", b0), StaleRefError);

    // 新发射 label + goto：缓存置脏后新 label 立即可解析。
    const auto l2 = facadeEmitLabel(s, "me/demo/Util", "lx", "(I)I", std::nullopt);
    const auto go = facadeEmitJump(s, "me/demo/Util", "lx", "(I)I", 0xA7, l2, std::nullopt);
    {
        const auto d = facadeInsnDetail(s, "me/demo/Util", "lx", "(I)I", go);
        EXPECT_EQ(d.targetUid, l2);
    }
    FacadeRegistry::instance().close(h);
}

TEST(FacadeTree, ClassHierarchyClosure)
{
    // 继承闭包查询：super 链 + 全部接口传递闭包 + 沿途声明成员集
    // （源码树建继承成员集不必再外跑 javap 反射的 SDK 内建等位物）。
    const auto h = openDemo("mjt_facade_hier.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    {
        const auto rep = facadeClassHierarchy(s, "me/demo/Color");
        ASSERT_EQ(rep.classes.size(), std::size_t{2});   // Color → Api；Object 容器外不入闭包
        EXPECT_EQ(rep.classes[0].name, "me/demo/Color");
        EXPECT_EQ(rep.classes[0].superName, "java/lang/Object");
        EXPECT_EQ(rep.classes[0].interfaces, (std::vector<std::string>{"me/demo/Api"}));
        EXPECT_EQ(rep.classes[1].name, "me/demo/Api");
        EXPECT_EQ(rep.classes[1].superName, "java/lang/Object");
        bool hasValues = false;
        bool hasApiId  = false;
        for (const auto& [owner, name, desc, access] : rep.methods) {
            (void)desc;
            (void)access;
            if (owner == "me/demo/Color" && name == "values") hasValues = true;
            if (owner == "me/demo/Api" && name == "id") hasApiId = true;
        }
        EXPECT_TRUE(hasValues);
        EXPECT_TRUE(hasApiId);
        bool hasValuesField = false;
        for (const auto& [owner, name, desc, access] : rep.fields) {
            if (owner == "me/demo/Color" && name == "$VALUES" &&
                desc == "[Lme/demo/Color;") {
                hasValuesField = access != -1;
            }
        }
        EXPECT_TRUE(hasValuesField);
    }
    {
        const auto rep = facadeClassHierarchy(s, "me/demo/Crypto");
        ASSERT_EQ(rep.classes.size(), std::size_t{1});   // super/接口全在容器外 → 闭包只含自身
        EXPECT_EQ(rep.classes[0].name, "me/demo/Crypto");
    }
    EXPECT_THROW(facadeClassHierarchy(s, "me/demo/Nope"), StaleRefError);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeContainer, ShadowGenerateDumpLoadRoundtrip)
{
    const auto h = openDemo("mjt_facade_shadow.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    const auto gen = facadeShadowGenerate(s);
    EXPECT_EQ(gen.classes, std::size_t{5});
    EXPECT_GT(gen.methods + gen.fields, std::size_t{0});
    const auto total = gen.classes + gen.methods + gen.fields;
    EXPECT_EQ(facadeShadowEntries(s).size(), total);
    // token 补号非空（#1156）。
    for (const auto& entry : facadeShadowEntries(s)) {
        EXPECT_FALSE(std::get<5>(entry).empty());
    }
    // dump -> 清表 -> load 往返恒等。
    facadeShadowDumpFile(s, tmpPath("mjt_facade_shadow.json"));
    s.shadowTable.reset();
    EXPECT_TRUE(facadeShadowEntries(s).empty());
    EXPECT_EQ(facadeShadowLoadFile(s, tmpPath("mjt_facade_shadow.json")), total);
    EXPECT_EQ(facadeShadowEntries(s).size(), total);
    FacadeRegistry::instance().close(h);
}

// ── 批 A 回归：W5 set_entry_bytes 类型混淆 / W4 token 改名资源面 / 脏树保存 ──

namespace {

std::string makeManifestJar(const char* name)
{
    auto path = tmpPath(name);
    writeSyntheticManifestJar(path);
    return path;
}

}   // namespace

/// W5 回归：set_entry_bytes 落 MANIFEST 条目必须走 ManifestResource 分支。
/// 旧代码用不受检 cast 做分支判定（永远非空），Manifest 条目误入
/// ClassResource 分支类型混淆写内存 → SEH。修复后三资源类型分支均可达。
TEST(FacadeContainer, SetEntryBytesManifestRoundtrip)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = reg.create();
    facadeImportJar(s, makeManifestJar("mjt_facade_manifest_set.jar"));

    const auto before = facadeGetEntryBytes(s, "META-INF/MANIFEST.MF");
    const std::string beforeText(before.begin(), before.end());
    EXPECT_NE(beforeText.find("Main-Class"), std::string::npos);

    // 同字往返（set 时 manifest 解析/再序列化必须不炸）。
    facadeSetEntryBytes(s, "META-INF/MANIFEST.MF", before);
    EXPECT_EQ(facadeGetEntryBytes(s, "META-INF/MANIFEST.MF"), before);

    // 改写主属性后再读回。
    std::string edited(before.begin(), before.end());
    edited.replace(edited.find("me.demo.App"), std::strlen("me.demo.App"), "me.demo.Launched");
    const std::vector<ca::u8> editedBytes(edited.begin(), edited.end());
    facadeSetEntryBytes(s, "META-INF/MANIFEST.MF", editedBytes);
    EXPECT_EQ(facadeGetEntryBytes(s, "META-INF/MANIFEST.MF"), editedBytes);

    // 类条目分支同测（同型 cast 误用面）。
    const auto cls = facadeClassBytes(s, "me/demo/App");
    facadeSetEntryBytes(s, "me/demo/App.class", cls);
    EXPECT_EQ(facadeClassBytes(s, "me/demo/App"), cls);
    // 非法 manifest 字节不得崩：Manifest 模型解析后规范化为空主属性段。
    const std::vector<ca::u8> garbage{'o', 'k'};
    facadeSetEntryBytes(s, "META-INF/MANIFEST.MF", garbage);
    // 空主属性段的规范化产物：单个 CRLF。
    EXPECT_EQ(facadeGetEntryBytes(s, "META-INF/MANIFEST.MF"),
              (std::vector<ca::u8>{0x0D, 0x0A}));
    reg.close(s.handle);
}

/// W4 回归：token 改名 commit 必须迁移条目 URI 并重写 MANIFEST 主属性
/// （resources 门默认开）。旧代码 resources 缺省 false，导出 jar 新类挂旧 URI。
TEST(FacadeContainer, TokenRenameMigratesEntryUriAndManifest)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = reg.create();
    facadeImportJar(s, makeManifestJar("mjt_facade_token_uri.jar"));

    facadeRenameBegin(s);
    facadeTokenRenameClass(s, "me/demo/App", "me/demo/Launched");
    facadeTokenCommit(s, /*reflectionStrings=*/true, /*annotations=*/false,
                      /*signatures=*/false, /*resources=*/true);

    // 类索引 + 条目 URI 双迁移。
    EXPECT_EQ(facadeClassUri(s, "me/demo/Launched"), "me/demo/Launched.class");
    EXPECT_THROW(facadeClassUri(s, "me/demo/App"), StaleRefError);
    // MANIFEST 的 class 形态主属性随改写。
    const auto mf = facadeGetEntryBytes(s, "META-INF/MANIFEST.MF");
    EXPECT_NE(std::string(mf.begin(), mf.end()).find("me.demo.Launched"),
              std::string::npos);
    EXPECT_EQ(std::string(mf.begin(), mf.end()).find("me.demo.App"), std::string::npos);
    // 资源门显式关：URI 保持旧名（调用方自管条目地址的逃生门仍在）。
    auto& s2 = reg.create();
    facadeImportJar(s2, makeManifestJar("mjt_facade_token_uri_off.jar"));
    facadeRenameBegin(s2);
    facadeTokenRenameClass(s2, "me/demo/App", "me/demo/Moved");
    facadeTokenCommit(s2, false, false, false, /*resources=*/false);
    EXPECT_EQ(facadeClassUri(s2, "me/demo/Moved"), "me/demo/App.class");
    reg.close(s.handle);
    reg.close(s2.handle);
}

/// 脏树保存回归：token commit 前必须自动 flush 脏树。旧代码 remapper 从
/// 容器字节重写并 clearTrees，未落盘编辑被静默丢弃（实测 41 折叠整批蒸发）。
TEST(FacadeTree, TokenCommitPreservesDirtyTreeEdits)
{
    const auto h = openDemo("mjt_facade_token_dirty.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    const auto before = facadeInsnCount(s, "me/demo/Util", "add", "(II)I");
    facadeEmitInsn(s, "me/demo/Util", "add", "(II)I", 0x04 /*iconst_1*/, std::nullopt);
    EXPECT_EQ(facadeInsnCount(s, "me/demo/Util", "add", "(II)I"), before + 1);

    // 不 flush 直接 commit（另一类的改名），编辑必须存活。
    facadeRenameBegin(s);
    facadeTokenRenameClass(s, "me/demo/Crypto", "me/demo/Decoder");
    facadeTokenCommit(s, false, false, false, true);

    EXPECT_EQ(facadeClassUri(s, "me/demo/Decoder"), "me/demo/Decoder.class");
    facadeParseClass(s, "me/demo/Util");
    // 编辑存活 = iconst_1 恰好一个，且总数 = 原值 +1（auto-flush 为保帧路径，
    // 不引入 frame/label 伪指令；COMPUTE_FRAMES 写回对尾插死代码形状会产出
    // 损坏体——独立挂账，见 dev_plan）。
    int iconst1 = 0;
    for (const auto& i : facadeInsns(s, "me/demo/Util", "add", "(II)I")) {
        if (i.opcode == 0x04) ++iconst1;
    }
    EXPECT_EQ(iconst1, 1);
    EXPECT_EQ(facadeInsnCount(s, "me/demo/Util", "add", "(II)I"), before + 1);
    FacadeRegistry::instance().close(h);
}

// ═══════════════════════════════════════════════════════════════════════
// 脚本资源上限（协作层）：deadline 检查点与 watchdog 触发逻辑。
// 进程级退出码契约（watchdog _Exit(3) 等）在 mjt_pysdk_embedtest。
// ═══════════════════════════════════════════════════════════════════════

TEST(FacadeLimits, DeadlineCheckpointFailsLongOpsClosed)
{
    const auto h = openDemo("mjt_facade_limits.jar");
    auto&      s = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    facadeRenameMethod(s, "me/demo/Util", "add", "(II)I", "plus");   // 造脏树：flush 才有活干
    EXPECT_EQ(deadlineRemainingMs(), -1);   // 缺省无上限

    setDeadlineMs(-1);                       // 测试位：立即过期
    EXPECT_EQ(deadlineRemainingMs(), 0);
    EXPECT_THROW(facadeFlush(s, true), DeadlineExceeded);
    EXPECT_THROW(facadeVerifyContainer(s), DeadlineExceeded);
    EXPECT_THROW(facadeSanitizeLvt(s), DeadlineExceeded);

    // 清除后必须完全复原（不得毒化同进程的后续用例）。
    setDeadlineMs(0);
    EXPECT_EQ(deadlineRemainingMs(), -1);
    EXPECT_NO_THROW(facadeFlush(s, true));
    FacadeRegistry::instance().close(h);
}

TEST(FacadeLimits, WatchdogFiresHookOnceAndStopsClean)
{
    int              fired   = 0;
    std::int64_t firedWith = 0;
    startDeadlineWatchdog(80, [&](std::int64_t ms) {
        ++fired;
        firedWith = ms;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(fired, 1);
    EXPECT_EQ(firedWith, 80);
    stopDeadlineWatchdog();   // hook 已执行过：join 立即返回

    // ms <= 0 为空操作路径；stop 幂等。
    startDeadlineWatchdog(0, [](std::int64_t) { FAIL() << "must not fire"; });
    stopDeadlineWatchdog();
    stopDeadlineWatchdog();

    // 未到点即停：hook 不得再触发。
    int unfired = 0;
    startDeadlineWatchdog(10000, [&](std::int64_t) { ++unfired; });
    stopDeadlineWatchdog();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(unfired, 0);
}

// ═══════════════════════════════════════════════════════════════════════
// Jasm 文本面（libjhasm 绑定）：读 / patch / diff / fail-closed。
// 恒等锚是规范化文本（disassemble→assemble 的字节不保证逐字节恒等——
// 常量池排布可漂），字节级断言只做语义面（insns 操作码）。
// ═══════════════════════════════════════════════════════════════════════

TEST(FacadeJasm, ClassRoundtripAndValidateZeroCommit)
{
    const auto h = openDemo("mjt_facade_jasm_rt.jar");
    auto&      s = FacadeRegistry::instance().get(h);
    const auto text = facadeClassJasm(s, "me/demo/Util");
    EXPECT_EQ(text.internalName, "me/demo/Util");
    EXPECT_NE(text.source.find(".class public super me/demo/Util"), std::string::npos);
    EXPECT_NE(text.source.find(".method public static add (II)I"), std::string::npos);
    EXPECT_NE(text.source.find("iadd"), std::string::npos);

    // validate 全绿且零诊断。
    const auto okReport = facadeValidateJasm(s, "me/demo/Util", "", text.source);
    EXPECT_TRUE(okReport.ok());
    EXPECT_TRUE(okReport.diagnostics.empty());
    EXPECT_EQ(okReport.internalName, "me/demo/Util");

    // validate 绝不落容器：字节恒等。
    const auto before = facadeClassBytes(s, "me/demo/Util");
    EXPECT_EQ(facadeClassBytes(s, "me/demo/Util"), before);

    // apply 同文往返：报告 ok；重读文本恒等（恒等锚）。
    const auto applied = facadeApplyJasm(s, "me/demo/Util", "", text.source);
    EXPECT_TRUE(applied.ok());
    EXPECT_EQ(applied.internalName, "me/demo/Util");
    EXPECT_GT(applied.written, ca::usize{0});
    EXPECT_EQ(facadeClassJasm(s, "me/demo/Util").source, text.source);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeJasm, ApplyInvalidatesDirtyTree)
{
    const auto h = openDemo("mjt_facade_jasm_inv.jar");
    auto&      s = FacadeRegistry::instance().get(h);
    facadeParseClass(s, "me/demo/Util");
    facadeRenameMethod(s, "me/demo/Util", "add", "(II)I", "plus");
    EXPECT_NO_THROW(facadeMethodInfo(s, "me/demo/Util", "plus", "(II)I"));

    // jasm apply = 外部改字节：未 write_back 的脏树让位（set_class_bytes 同款）。
    const auto text = facadeClassJasm(s, "me/demo/Util").source;
    EXPECT_TRUE(facadeApplyJasm(s, "me/demo/Util", "", text).ok());
    bool hasAdd = false;
    for (const auto& [n, d, a] : facadeMembers(s, "me/demo/Util", true)) {
        if (n == "add") hasAdd = true;
    }
    EXPECT_TRUE(hasAdd);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeJasm, MethodJasmDiffApplyChangesOpcode)
{
    const auto h = openDemo("mjt_facade_jasm_diff.jar");
    auto&      s = FacadeRegistry::instance().get(h);
    const auto method = facadeMethodJasm(s, "me/demo/Util", "add", "(II)I");
    EXPECT_EQ(method.descriptor, "(II)I");
    EXPECT_NE(method.source.find("iadd"), std::string::npos);

    // 全量替换（原样）→ ok。
    EXPECT_TRUE(facadeApplyMethodJasm(s, "me/demo/Util", "add", "(II)I", "", method.source).ok());

    // diff：iadd → isub（方法源内行号 5）。
    const std::string diff =
        "@@ -5,1 +5,1 @@\n"
        "-            iadd\n"
        "+            isub\n";
    const auto report = facadeApplyMethodJasmDiff(s, "me/demo/Util", "add", "(II)I", diff);
    ASSERT_TRUE(report.ok()) << (report.diagnostics.empty() ? "" :
                                 report.diagnostics.front().message);

    // 语义面核验：操作码 0x60(iadd) → 0x64(isub)，ireturn 保留。
    bool sawIsub = false;
    for (const auto& insn : facadeInsns(s, "me/demo/Util", "add", "(II)I")) {
        if (insn.opcode == 0x64) sawIsub = true;
        else EXPECT_NE(insn.opcode, 0x60);
    }
    EXPECT_TRUE(sawIsub);
    // 类全文里的方法段同步更新（method_jasm 与类内方法段逐字节一致的契约面）。
    EXPECT_NE(facadeMethodJasm(s, "me/demo/Util", "add", "(II)I").source.find("isub"),
              std::string::npos);
    EXPECT_EQ(facadeMethodJasm(s, "me/demo/Util", "add", "(II)I").source.find("iadd"),
              std::string::npos);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeJasm, DiffContextMismatchCarriesHunkNumber)
{
    const auto h = openDemo("mjt_facade_jasm_badctx.jar");
    auto&      s = FacadeRegistry::instance().get(h);
    const std::string diff =
        "@@ -4,1 +4,1 @@\n"
        "-            iadd\n"
        "+            isub\n";
    const auto report = facadeApplyJasmDiff(s, "me/demo/Util", diff);
    EXPECT_FALSE(report.ok());
    ASSERT_EQ(report.diagnostics.size(), std::size_t{1});
    EXPECT_EQ(report.diagnostics.front().severity, 0);
    EXPECT_NE(report.diagnostics.front().message.find("hunk #1"), std::string::npos);
    EXPECT_NE(report.diagnostics.front().message.find("context mismatch"), std::string::npos);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeJasm, ValidateRejectsBrokenAndNameMismatch)
{
    const auto h = openDemo("mjt_facade_jasm_reject.jar");
    auto&      s = FacadeRegistry::instance().get(h);
    const auto before = facadeClassBytes(s, "me/demo/Util");

    // 语法坏源：error 诊断，字节不动。
    const auto broken = facadeValidateJasm(s, "me/demo/Util", "", "this is not jasm");
    EXPECT_FALSE(broken.ok());
    EXPECT_FALSE(broken.diagnostics.empty());
    EXPECT_EQ(facadeClassBytes(s, "me/demo/Util"), before);

    // 名不符：expected/got 全在文案里（fail-closed 防索引错位）。
    const auto mismatch =
        facadeValidateJasm(s, "me/demo/Util", "",
                           ".super java/lang/Object\n.class public me/demo/Other {\n}\n");
    EXPECT_FALSE(mismatch.ok());
    EXPECT_NE(mismatch.diagnostics.front().message.find("me/demo/Other"), std::string::npos);
    EXPECT_EQ(facadeClassBytes(s, "me/demo/Util"), before);

    // apply 坏源同样不落容器。
    EXPECT_FALSE(facadeApplyJasm(s, "me/demo/Util", "", "garbage").ok());
    EXPECT_EQ(facadeClassBytes(s, "me/demo/Util"), before);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeJasm, UnknownClassAndMethodFailClosed)
{
    const auto h = openDemo("mjt_facade_jasm_stale.jar");
    auto&      s = FacadeRegistry::instance().get(h);
    EXPECT_THROW(facadeClassJasm(s, "me/demo/Nope"), StaleRefError);
    EXPECT_THROW(facadeMethodJasm(s, "me/demo/Util", "nope", "()V"), StaleRefError);
    EXPECT_THROW(facadeMethodJasm(s, "me/demo/Util", "add", "(JJ)J"), StaleRefError);
    EXPECT_THROW(facadeValidateJasm(s, "me/demo/Nope", "", "x"), StaleRefError);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeJasm, ValidationModeAcceptsThreeTiersAndRejectsUnknown)
{
    const auto h = openDemo("mjt_facade_jasm_validation.jar");
    auto&      s = FacadeRegistry::instance().get(h);
    const auto text = facadeClassJasm(s, "me/demo/Util").source;

    // 三档词表齐收（默认 relaxed 等位）；off 同样走全管线（汇编后校验跳过）。
    for (const auto* mode : {"relaxed", "strict", "off"}) {
        const auto report = facadeValidateJasm(s, "me/demo/Util", "", text, mode);
        EXPECT_TRUE(report.ok()) << mode;
    }

    // 词表外值在参数层拒绝（对齐 MCP schema enum 文案）。
    EXPECT_THROW(facadeValidateJasm(s, "me/demo/Util", "", text, "lenient"),
                 std::invalid_argument);
    FacadeRegistry::instance().close(h);
}
