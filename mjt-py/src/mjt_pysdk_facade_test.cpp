// mjt_pysdk_facade_test.cpp — v3 facade C++ 直测（契约 §8 验收项的 B1 子集）。
//
// 覆盖：索引装载 / loose class 导入（v2 缺口：新增类全链）/ 子集与单类导出 /
// first-wins 重复名单 / 条目字节往返 / 句柄失效 fail-closed。
// 全程合成样本（writeSyntheticDemoJar），零样本字节入库。

#include <gtest/gtest.h>

#include <chrono>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "jbc/jbc.hpp"

#include "pysdk_facade.hpp"
#include "pysdk_jar.hpp"
#include "pysdk_jasm.hpp"
#include "pysdk_limits.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"

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

ca::str::Utf16String utf16Text(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

/// #72 复现形态罐：两份 module-info.class（多版本打包残留，issue 实测样本
/// 形状）。类字节经 jasm 文本装配（fixture 纪律：R"JASM 定版，禁手拼字节）。
std::string makeAmbiguousModuleJar(const char* name)
{
    mjbase::jasm::JvmJasmService assembler;
    const auto sourceName = utf16Text("module-info.jasm");
    const auto source     = utf16Text(std::string{R"JASM(.class module module-info {
}
)JASM"});
    const auto assembled = assembler.assemble(sourceName.ref(), source.ref());
    if (!assembled.isOk()) throw std::runtime_error("module-info fixture did not assemble");

    mjbase::model::ResourceContainer container;
    const auto bytes = assembled.value->bytes;
    container.addResource(
        "module-info.class",
        std::make_unique<mjbase::model::ClassResource>("module-info.class", bytes));
    container.addResource(
        "META-INF/versions/9/module-info.class",
        std::make_unique<mjbase::model::ClassResource>(
            "META-INF/versions/9/module-info.class", bytes));
    const auto path = tmpPath(name);
    const auto exported = mjbase::model::ResourceHelper::exportToJar(container, path);
    if (exported.is_err()) throw std::runtime_error("module-info fixture export failed");
    return path;
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

// #72 复现形态：jar 内两份 module-info.class（多版本打包残留）。重名歧义
// 异常不再只报单条 name——四个重名抛点（class_bytes / set_class_bytes /
// 树解析 / jasm）共用同一文案：全部候选 URI（winner + 落选者）+ 决策建议
// （排除多余条目或按显式条目 URI 寻址）。决策建议可落地：排除落选条目后
// winner 晋级，类名恢复可寻址。
TEST(FacadeContainer, AmbiguousDuplicateErrorListsCandidateUris)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = reg.create();
    facadeImportJar(s, makeAmbiguousModuleJar("mjt_facade_ambig.jar"));
    ASSERT_EQ(facadeClassNames(s), (std::vector<std::string>{"module-info"}));
    ASSERT_EQ(facadeDuplicateNames(s), (std::vector<std::string>{"module-info"}));

    const std::string winner = "module-info.class";
    const std::string loser  = "META-INF/versions/9/module-info.class";

    const auto expectAmbiguity = [&](const char* what) {
        const std::string message = what;
        EXPECT_NE(message.find("ambiguous duplicate class name: module-info"),
                  std::string::npos);
        EXPECT_NE(message.find(winner), std::string::npos);
        EXPECT_NE(message.find(loser), std::string::npos);
        EXPECT_NE(message.find("remove_entry"), std::string::npos);
    };

    try {
        (void)facadeClassBytes(s, "module-info");
        FAIL() << "class_bytes must stay ambiguous";
    }
    catch (const StaleRefError& error) {
        expectAmbiguity(error.what());
    }
    try {
        (void)facadeSetClassBytes(s, "module-info", facadeGetEntryBytes(s, winner));
        FAIL() << "set_class_bytes must stay ambiguous";
    }
    catch (const StaleRefError& error) {
        expectAmbiguity(error.what());
    }
    try {
        facadeParseClass(s, "module-info");
        FAIL() << "tree resolve must stay ambiguous";
    }
    catch (const StaleRefError& error) {
        expectAmbiguity(error.what());
    }
    try {
        (void)facadeClassJasm(s, "module-info");
        FAIL() << "class jasm must stay ambiguous";
    }
    catch (const StaleRefError& error) {
        expectAmbiguity(error.what());
    }

    // 显式条目 URI 寻址不受重名歧义影响（entry 面按 URI 直取）。
    EXPECT_FALSE(facadeGetEntryBytes(s, loser).empty());
    EXPECT_FALSE(facadeGetEntryBytes(s, winner).empty());

    // 决策建议落地：排除落选条目 → winner 晋级，类名恢复可寻址。
    facadeRemoveEntry(s, loser);
    EXPECT_TRUE(facadeDuplicateNames(s).empty());
    EXPECT_EQ(facadeClassUri(s, "module-info"), winner);
    EXPECT_FALSE(facadeClassBytes(s, "module-info").empty());
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
    EXPECT_TRUE((mi.access & 0x0008 /*ACC_STATIC*/) != 0);
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
    // branchy: iload 0; ifeq L; iconst_1; goto Lend; L: iconst_0; Lend: ireturn。
    // 跳转目标 label 必须先在（uid 无前向引用），真目标以 set_target 改指：
    // 占位 label 落点不进边，帧计算按改指后的真目标求深度（Lend 两前驱
    // 深度均 1，L 前驱深度均 0——迁移期版本把 L 发射在 iload 之后，L 的
    // 落入边与直落边深度 0/1 相撞，write_back 恒炸且从未被 CI 拦下）。
    const auto load = facadeEmitVar(s, "me/demo/Util", "branchy", "(I)I", 0x15 /*iload*/, 0, std::nullopt);
    const auto lDum = facadeEmitLabel(s, "me/demo/Util", "branchy", "(I)I", std::nullopt);
    const auto cond = facadeEmitJump(s, "me/demo/Util", "branchy", "(I)I", 0x99 /*ifeq*/, lDum, std::nullopt);
    facadeEmitInsn(s, "me/demo/Util", "branchy", "(I)I", 0x04 /*iconst_1*/, std::nullopt);
    const auto gDum = facadeEmitLabel(s, "me/demo/Util", "branchy", "(I)I", std::nullopt);
    const auto go   = facadeEmitJump(s, "me/demo/Util", "branchy", "(I)I", 0xA7 /*goto*/, gDum, std::nullopt);
    const auto l1   = facadeEmitLabel(s, "me/demo/Util", "branchy", "(I)I", std::nullopt);
    facadeEmitInsn(s, "me/demo/Util", "branchy", "(I)I", 0x03 /*iconst_0*/, std::nullopt);
    const auto lend = facadeEmitLabel(s, "me/demo/Util", "branchy", "(I)I", std::nullopt);
    facadeEmitInsn(s, "me/demo/Util", "branchy", "(I)I", 0xAC /*ireturn*/, std::nullopt);
    // 前向跳转目标 uid 可改指（set_target）：两跳各自改指真 label。
    facadeInsnSetTarget(s, "me/demo/Util", "branchy", "(I)I", cond, l1);
    facadeInsnSetTarget(s, "me/demo/Util", "branchy", "(I)I", go, lend);
    EXPECT_NO_THROW(facadeWriteBack(s, "me/demo/Util", true));
    (void)load;
    (void)lDum;
    (void)gDum;
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
    EXPECT_EQ(d.kind, insn_kind::kTableSwitch);
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
    EXPECT_EQ(d.kind, insn_kind::kLookupSwitch);
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

TEST(FacadeTree, LdcStringPayloadPreservesEmbeddedNul)
{
    // MUTF-8 边界串回归：MUTF-8→UTF-8 转换产物可含内嵌 NUL（U+0000 的 C0 80
    // 形转回真实 NUL，增补平面转四字节 UTF-8）。载荷入池必须按 byte_length
    // 复制——internIn 曾走 from_cstr 的 strlen 舞步，ldc "A\0B😊X" 写出为 "A"。
    // 发射 → 读侧回读 → save → 重开回读，四级断言全链保真。
    const std::string payload = std::string("A\x00" "B\xF0\x9F\x98\x80" "X", 8);

    const auto out = tmpPath("mjt_facade_ldc_nul.jar");
    {
        const auto h = openDemo("mjt_facade_ldc_nul_src.jar");
        auto& s      = FacadeRegistry::instance().get(h);
        facadeParseClass(s, "me/demo/Util");
        facadeAddMethod(s, "me/demo/Util", "msg", "()Ljava/lang/String;", 0x0009, 1, 0);
        const auto ld = facadeEmitLdc(s, "me/demo/Util", "msg", "()Ljava/lang/String;",
                                      facadeInternedStr(s, payload), std::nullopt);
        facadeEmitInsn(s, "me/demo/Util", "msg", "()Ljava/lang/String;",
                       0xB0 /*areturn*/, std::nullopt);
        const auto d = facadeInsnDetail(s, "me/demo/Util", "msg", "()Ljava/lang/String;", ld);
        EXPECT_EQ(d.cstTag, 1);
        EXPECT_EQ(d.cstStr, payload);
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
        EXPECT_EQ(d.cstStr, payload);
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
    EXPECT_NE(facadeContainer(s).getResource("me/demo/Renamed.class"), nullptr);
    EXPECT_EQ(facadeContainer(s).getResource("me/demo/Util.class"), nullptr);
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
        // jar 写出不合成目录占位条目（mjbase#167）：全 class 条目，nonClass=0。
        EXPECT_EQ(rep.nonClass, 0u);
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
    namespace ir  = jbc::ir;
    namespace jb  = jbc::core;
    const auto h = openDemo("mjt_facade_lvt.jar");
    auto& s      = FacadeRegistry::instance().get(h);
    // 干净基线：demo jar 无 LVT 条目。
    {
        const auto rep = facadeSanitizeLvt(s);
        EXPECT_EQ(rep.dropped, 0u);
        EXPECT_EQ(rep.deduped, 0u);
        EXPECT_EQ(rep.classes, 5u);
    }
    // 合成带 LVT 的类：nop(pc0); iconst_1; istore_0; iload_0; ireturn
    // + LVT slot0 [pc0, codeLen)。首行锚 = nop（G1 场景里被删的锚指令）。
    ir::NewClassSeed seed;
    seed.access    = 0x0021;   // PUBLIC|SUPER
    seed.thisName  = "me/demo/LvtDemo";
    seed.superName = "java/lang/Object";
    auto made      = ir::makeSession(seed);
    ASSERT_TRUE(made.is_ok());
    auto session = std::move(made).unwrap();
    ir::MethodSeed mseed;
    mseed.name      = "lv";
    mseed.desc      = "()V";
    mseed.access    = 0x0009;   // PUBLIC|STATIC
    mseed.withCode  = true;
    mseed.maxStack  = 1;
    mseed.maxLocals = 1;
    auto added      = session->addMethod(mseed);
    ASSERT_TRUE(added.is_ok());
    // withCode 只落空 Code 属性：promote 后 IR CodeUnit 才非空（MethodUnit::code 契约）。
    ASSERT_TRUE(ir::promoteMethod(*session, added.unwrap()).is_ok());
    auto* cu  = session->unit().method(added.unwrap())->code.get();
    auto  mk  = [&](jb::Op op) {
        ir::Insn in;
        in.op    = op;
        in.srcPc = ir::kNoPc;
        return in;
    };
    cu->dom.append(mk(jb::Op::NOP));
    cu->dom.append(mk(jb::Op::ICONST_1));
    cu->dom.append(mk(jb::Op::ISTORE_0));
    cu->dom.append(mk(jb::Op::ILOAD_0));
    cu->dom.append(mk(jb::Op::IRETURN));
    ASSERT_TRUE(ir::lowerMethod(*session, added.unwrap()).is_ok());
    // LVT 嵌套槽直挂（Code 内属性：libjbc 槽级手术形态）。
    auto& mi = session->backing().methods[session->unit().method(added.unwrap())->backingIndex];
    auto  ca = session->backing().decodeCode(mi);
    ASSERT_TRUE(ca.is_ok());
    auto lvtBody = std::make_unique<jb::LocalVariableTableAttr>();
    jb::LocalVariableTableAttr::Row row;
    row.startPc = 0;
    row.length  = static_cast<ca::u32>(ca.unwrap()->code.size());
    row.nameCp  = session->internUtf8("v");
    row.descCp  = session->internUtf8("I");
    row.slot    = 0;
    lvtBody->rows.push_back(row);
    ca.unwrap()->attrs.push_back(
        jb::AttrSlot::makeNew(session->internUtf8("LocalVariableTable"), std::move(lvtBody)));
    const auto written = jb::writeClass(session->backing());
    ASSERT_TRUE(written.is_ok());
    const auto bytes = std::move(written).unwrap();
    ASSERT_FALSE(bytes.empty());
    const auto lvtPath = tmpPath("mjt_facade_lvt.class");
    writeFileBytes(lvtPath, bytes);
    facadeLoadClassFile(s, lvtPath, std::nullopt, false);
    {
        const auto rep = facadeSanitizeLvt(s);
        EXPECT_EQ(rep.classes, 6u);
        EXPECT_EQ(rep.dropped, 0u);
    }
    // G1 形态：LVT start 锚所在指令被删 → start_pc 脱链 → 清洗剔除该条目，
    // 之后 COMPUTE_FRAMES 写出可过（未清洗前 lower 的 LVT 重定位会失败）。
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
    // 资源门显式关：类条目 URI 仍随类改名迁移（mjbase #144 口径——类 URI 换名
    // 门 = bytecode，旧 URI 挂新字节是坏 jar；resources 门只管 services/
    // manifest/文本资源面，类条目逃生门不复存在）。
    auto& s2 = reg.create();
    facadeImportJar(s2, makeManifestJar("mjt_facade_token_uri_off.jar"));
    facadeRenameBegin(s2);
    facadeTokenRenameClass(s2, "me/demo/App", "me/demo/Moved");
    facadeTokenCommit(s2, false, false, false, /*resources=*/false);
    EXPECT_EQ(facadeClassUri(s2, "me/demo/Moved"), "me/demo/Moved.class");
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

// ═══════════════════════════════════════════════════════════════════════
// 保留原帧档（mjbase#190）：apply 族 frame 档透传——坏帧类 validation=off
// 的逃生门。夹具例外①申报，两条构造路线（与 mjbase frame_preserve_test
// 同口径）：
//   1. jsr/ret 子程序类（preserve 档目标面）：jsr 文本 v49 直装（分析干净、
//      无帧参与），DOM 单点抬 major 61——v61 + jsr/ret 类数据流重算可过、
//      StackMapTable 序列化必败。头条用例钉：Regenerate 如实失败零落盘、
//      preserve 落盘且版本回填。
//   2. branchy 类 1 字节 opcode 偏差（aconst_null→iconst_2，int vs ref 合并
//      Bottom 槽）：jhasm 对数据流 Strict 拒绝一律硬失败，与版本和 frame 档
//      无关——只用于钉边界申报用例（指令面改动 fail-closed），不是 preserve
//      可救面。
// 边界申报：preserve 档豁免的是帧序列化，不是数据流分析。
// ═══════════════════════════════════════════════════════════════════════

namespace {

ca::str::Utf16String frameTestUtf16(const std::string& text)
{
    return ca::str::Utf16String::from_utf8_string(
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(text.data()),
                                          text.size()));
}

/// v61 分支基类源（join 合并两侧同为 ref——装配即生成 StackMapTable）。
/// 指令流全流仅两个 0x01 字节，即两条 aconst_null（偏差定位锚）。
std::string branchClassSource(const std::string& internalName)
{
    return ".super java/lang/Object\n"
           ".class public super " + internalName + " {\n"
           "  .method public static pick (Z)Ljava/lang/Object; {\n"
           "    parameters: { flag },\n"
           "    code: {\n"
           "      iload flag\n"
           "      ifeq right\n"
           "      aconst_null\n"
           "      goto join\n"
           "    right:\n"
           "      aconst_null\n"
           "    join:\n"
           "      areturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::vector<ca::u8> assembleBranchClass(const std::string& internalName)
{
    mjbase::jasm::JvmJasmService jasm;
    mjbase::jasm::JvmJasmContext context;
    context.classMajorVersion = 61;
    const auto sourceName = frameTestUtf16(internalName + ".jasm");
    const auto sourceText = frameTestUtf16(branchClassSource(internalName));
    const auto assembled  = jasm.assemble(sourceName.ref(), sourceText.ref(), context);
    EXPECT_TRUE(assembled.isOk());
    if (!assembled.isOk() || !assembled.value.has_value()) return {};
    return assembled.value->bytes;
}

/// 单点最小偏差：首支 aconst_null(0x01) → iconst_2(0x05)。AttrSlot 置脏
/// 纪律：Decoded 态写回走源 span copy，必须 mutate() 偏差才真正落盘。
std::vector<ca::u8> deviateFirstAconstNull(const std::vector<ca::u8>& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView{bytes.data(), bytes.size()});
    EXPECT_TRUE(read.is_ok());
    if (read.is_err()) return {};
    auto                  parsed = std::move(read).unwrap();
    jbc::core::ClassFile& cf     = *parsed.file;
    bool                  patched = false;
    for (auto& method : cf.methods) {
        for (auto& slot : method.attrs) {
            if (cf.utf8(slot.nameCp) != "Code") continue;
            auto decoded = cf.decodeAttr(slot);
            EXPECT_TRUE(decoded.is_ok());
            if (decoded.is_err()) return {};
            auto* code = static_cast<jbc::core::CodeAttr*>(decoded.unwrap());
            for (auto& byte : code->code) {
                if (byte == 0x01) {
                    byte = 0x05;
                    patched = true;
                    break;
                }
            }
            (void)slot.mutate();
            break;
        }
        if (patched) break;
    }
    EXPECT_TRUE(patched) << "夹具指令流无 aconst_null";
    if (!patched) return {};
    auto written = jbc::core::writeClass(cf);
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

/// 首个方法的 Code 嵌套 StackMapTable 内容字节（span 原样副本）；无则空。
std::vector<ca::u8> firstStackMapTableOf(const std::vector<ca::u8>& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView{bytes.data(), bytes.size()});
    if (read.is_err()) return {};
    auto                  parsed = std::move(read).unwrap();
    jbc::core::ClassFile& cf     = *parsed.file;
    for (auto& method : cf.methods) {
        for (auto& slot : method.attrs) {
            if (cf.utf8(slot.nameCp) != "Code") continue;
            auto decoded = cf.decodeAttr(slot);
            if (decoded.is_err()) return {};
            auto* code = static_cast<jbc::core::CodeAttr*>(decoded.unwrap());
            for (const auto& nested : code->attrs) {
                if (cf.utf8(nested.nameCp) != "StackMapTable") continue;
                const auto raw = cf.source().spanOrEmpty(nested.data);
                return std::vector<ca::u8>(raw.data(), raw.data() + raw.size());
            }
            return {};
        }
    }
    return {};
}

/// jsr/ret 子程序类源（preserve 档目标面，构造路线①）。
std::string subroutineClassSource(const std::string& internalName)
{
    return ".super java/lang/Object\n"
           ".class public super " + internalName + " {\n"
           "  .method public static pick (Z)Ljava/lang/Object; {\n"
           "    code: {\n"
           "      jsr sub\n"
           "      aconst_null\n"
           "      areturn\n"
           "    sub:\n"
           "      astore 1\n"
           "      ret 1\n"
           "    }\n"
           "  }\n"
           "}\n";
}

/// 构造路线①装配：jsr 文本 v49 直装（分析干净、无帧参与）→ DOM 抬
/// major 61——v61 + jsr/ret 类：数据流重算可过、SMT 序列化必败。
std::vector<ca::u8> assembleSubroutineClassAt61(const std::string& internalName)
{
    mjbase::jasm::JvmJasmService jasm;
    mjbase::jasm::JvmJasmContext context;
    context.classMajorVersion = 49;   // jsr 形态按夹具版本口径 ≤49 直装
    const auto sourceName     = frameTestUtf16(internalName + ".jasm");
    const auto sourceText     = frameTestUtf16(subroutineClassSource(internalName));
    const auto assembled      = jasm.assemble(sourceName.ref(), sourceText.ref(), context);
    EXPECT_TRUE(assembled.isOk());
    if (!assembled.isOk() || !assembled.value.has_value()) return {};
    auto read = jbc::core::readClass(
        jbc::core::ByteView{assembled.value->bytes.data(), assembled.value->bytes.size()});
    EXPECT_TRUE(read.is_ok());
    if (read.is_err()) return {};
    auto parsed = std::move(read).unwrap();
    parsed.file->major = 61;
    auto written = jbc::core::writeClass(*parsed.file);
    EXPECT_TRUE(written.is_ok());
    if (written.is_err()) return {};
    return std::move(written).unwrap();
}

/// 类文件 major 版本号（读失败返回 0）。
ca::u16 classMajorOf(const std::vector<ca::u8>& bytes)
{
    auto read = jbc::core::readClass(jbc::core::ByteView{bytes.data(), bytes.size()});
    if (read.is_err()) return 0;
    auto parsed = std::move(read).unwrap();
    return parsed.file->major;
}

}   // namespace

TEST(FacadeJasm, FramePreserveLandsBadFrameClassOffGate)
{
    const auto h = openDemo("mjt_facade_jasm_frame.jar");
    auto&      s = FacadeRegistry::instance().get(h);

    const auto badFrame = assembleSubroutineClassAt61("me/demo/Framed");
    ASSERT_FALSE(badFrame.empty());
    ASSERT_TRUE(firstStackMapTableOf(badFrame).empty());   // v49 直装产物无帧
    facadeSetClassBytes(s, "me/demo/Framed", badFrame);
    const auto text = facadeClassJasm(s, "me/demo/Framed").source;

    // Regenerate（缺省档）+ off：v61 + jsr/ret 的帧序列化失败如实上报，
    // 事务零落盘。
    const auto regen = facadeApplyJasm(s, "me/demo/Framed", "", text, "off");
    EXPECT_FALSE(regen.ok());
    EXPECT_FALSE(regen.diagnostics.empty());
    EXPECT_EQ(facadeClassBytes(s, "me/demo/Framed"), badFrame);

    // preserve + off：v49 杠杆跳过帧序列化后装配通过，落盘且版本回填原类；
    // 原类无帧（v49 直装产物），preserve 保持无帧不凭空发帧；指令面 = 提交
    // 文本往返。
    const auto kept = facadeApplyJasm(s, "me/demo/Framed", "", text, "off", "preserve");
    ASSERT_TRUE(kept.ok()) << (kept.diagnostics.empty() ? "" :
                               kept.diagnostics.front().message);
    EXPECT_GT(kept.written, ca::usize{0});
    const auto landed = facadeClassBytes(s, "me/demo/Framed");
    EXPECT_EQ(classMajorOf(landed), 61);
    EXPECT_TRUE(firstStackMapTableOf(landed).empty());
    EXPECT_NE(facadeClassJasm(s, "me/demo/Framed").source.find("jsr"),
              std::string::npos);
    FacadeRegistry::instance().close(h);
}

TEST(FacadeJasm, FramePreserveRejectsInstructionChangeAndBadValue)
{
    const auto h = openDemo("mjt_facade_jasm_frame2.jar");
    auto&      s = FacadeRegistry::instance().get(h);
    const auto deviated = deviateFirstAconstNull(assembleBranchClass("me/demo/Framed"));
    ASSERT_FALSE(deviated.empty());
    facadeSetClassBytes(s, "me/demo/Framed", deviated);

    // 提交合法基类文本：code[] 与容器内偏差类不一致——原帧对指令流的描述
    // 失效，保留即产坏类，fail-closed 逐方法申报、事务不落改动。
    const auto report = facadeApplyJasm(s, "me/demo/Framed", "",
                                        branchClassSource("me/demo/Framed"), "off",
                                        "preserve");
    EXPECT_FALSE(report.ok());
    bool boundaryReported = false;
    for (const auto& diagnostic : report.diagnostics) {
        if (diagnostic.severity == 0
            && diagnostic.message.find("instructions changed") != std::string::npos)
            boundaryReported = true;
    }
    EXPECT_TRUE(boundaryReported);
    EXPECT_EQ(facadeClassBytes(s, "me/demo/Framed"), deviated);

    // frame 非法值 → 参数错误（绑定层映射 ValueError）。
    EXPECT_THROW(facadeApplyJasm(s, "me/demo/Framed", "",
                                 branchClassSource("me/demo/Framed"), "off", "bogus"),
                 std::invalid_argument);
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

// ── class_census 文件面（#39 S0 量化基线） ───────────────────────────────

namespace {

/// 单行 TSV 切列（转义契约保证列内无 \t，见 workspace/analysis_census）。
std::vector<std::string> splitTabs(const std::string& line)
{
    std::vector<std::string>        fields;
    std::string::size_type          start = 0;
    while (start <= line.size()) {
        const auto tab = line.find('\t', start);
        if (tab == std::string::npos) {
            fields.emplace_back(line.substr(start));
            break;
        }
        fields.emplace_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    return fields;
}

}   // namespace

TEST(FacadeCensus, ExportsVersionedDeterministicTables)
{
    const auto jar    = makeDemoJar("mjt_facade_census.jar");
    const auto outDir = tmpPath("mjt_facade_census_out");

    const auto report = facadeClassCensusFile(jar, outDir);
    EXPECT_EQ(report.classes, 5U);
    // 成员面（#148 批 2）：11 方法 + 1 字段（$VALUES），对账不变式
    // members = member_census 行数 = Σ 类行 method_count + field_count。
    EXPECT_EQ(report.members, 12U);
    EXPECT_EQ(report.members, report.methods + report.fields);
    // jar 写出不合成目录占位条目（mjbase#167）：条目面 = 5 class 条目全常规。
    EXPECT_EQ(report.entriesActive, 5U);
    EXPECT_EQ(report.entriesIrregular, 0U);
    ASSERT_TRUE(std::filesystem::is_regular_file(report.classCensusFile));
    ASSERT_TRUE(std::filesystem::is_regular_file(report.containerCensusFile));
    ASSERT_TRUE(std::filesystem::is_regular_file(report.memberCensusFile));

    const auto classTsv = readFileBytes(report.classCensusFile);
    const std::string classText(classTsv.begin(), classTsv.end());
    // 版本化表头 + 列名行（schema v3 契约；LF 行尾、无 BOM）。
    EXPECT_EQ(classText.substr(0, classText.find('\n') + 1), "# class-census v3\n");
    EXPECT_NE(classText.find("internal_name\tkind\taccess\tparent\tiface_count"
                             "\tmethod_count\tfield_count\tnative_method_count"
                             "\tbyte_size\tzip_entry\torigin\n"),
              std::string::npos);

    // 一行一类、行序 = internal_name BINARY 升序（"Api" < "App"：'i' < 'p'）。
    const auto lines = [&] {
        std::vector<std::string> rows;
        std::string::size_type   start = 0;
        while (start < classText.size()) {
            const auto end = classText.find('\n', start);
            rows.emplace_back(classText.substr(start, end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return rows;
    }();
    ASSERT_EQ(lines.size(), 7U);   // 版本行 + 列名行 + 5 类
    const std::array<const char*, 5> expectedNames = {
        "me/demo/Api", "me/demo/App", "me/demo/Color", "me/demo/Crypto", "me/demo/Util"};
    for (std::size_t i = 0; i < expectedNames.size(); ++i) {
        const auto fields = splitTabs(lines[i + 2]);
        ASSERT_EQ(fields.size(), 11U) << lines[i + 2];
        EXPECT_EQ(fields[0], expectedNames[i]) << lines[i + 2];
        EXPECT_GT(fields[8].size(), 0U) << lines[i + 2];   // byte_size 非空
        EXPECT_EQ(fields[9], "regular") << lines[i + 2];   // 合成罐无伪装条目
        EXPECT_EQ(fields[10], "unknown") << lines[i + 2];  // me/demo 不落前缀
    }
    // 接口行全列（Api：PUBLIC|INTERFACE|ABSTRACT → 位序命名；1 抽象方法）。
    const auto apiFields = splitTabs(lines[2]);
    EXPECT_EQ(apiFields[1], "interface");
    EXPECT_EQ(apiFields[2], "public interface abstract");
    EXPECT_EQ(apiFields[3], "java/lang/Object");
    EXPECT_EQ(apiFields[4], "0");
    EXPECT_EQ(apiFields[5], "1");
    EXPECT_EQ(apiFields[6], "0");
    EXPECT_EQ(apiFields[7], "0");

    // container_census.tsv：条目面（regular/irregular 二分 + 形态 facet）与
    // 事实面计数，数字与报告一致。
    const auto containerTsv = readFileBytes(report.containerCensusFile);
    const std::string containerText(containerTsv.begin(), containerTsv.end());
    EXPECT_NE(containerText.find("# container-census v2\n"), std::string::npos);
    EXPECT_NE(containerText.find("entries_active\t5\n"), std::string::npos);
    EXPECT_NE(containerText.find("entries_regular\t5\n"), std::string::npos);
    EXPECT_NE(containerText.find("entries_irregular\t0\n"), std::string::npos);
    // 无目录占位（mjbase#167）：trailing_slash 与 resource 形态行零计数不落盘，
    // 条目形态由 entry_shape_regular 全覆盖。
    EXPECT_NE(containerText.find("entry_shape_regular\t5\n"), std::string::npos);
    EXPECT_NE(containerText.find("entry_kind_class\t5\n"), std::string::npos);
    EXPECT_EQ(containerText.find("entry_shape_trailing_slash"), std::string::npos);
    EXPECT_EQ(containerText.find("entry_kind_resource"), std::string::npos);
    EXPECT_NE(containerText.find("classes\t5\n"), std::string::npos);
    // 汇总 sha256 行 = 64 位 hex 内容锚形状。
    const auto shaLine = containerText.find("input_sha256\t");
    ASSERT_NE(shaLine, std::string::npos);
    const auto shaValue = containerText.substr(
        shaLine + std::strlen("input_sha256\t"),
        containerText.find('\n', shaLine) - (shaLine + std::strlen("input_sha256\t")));
    EXPECT_EQ(shaValue.size(), 64U);

    // 同输入两次导出逐字节一致（确定性契约，#39 设计要点 2）。
    const auto outDirAgain = tmpPath("mjt_facade_census_out_again");
    const auto again = facadeClassCensusFile(jar, outDirAgain);
    EXPECT_EQ(readFileBytes(again.classCensusFile), classTsv);
    EXPECT_EQ(readFileBytes(again.containerCensusFile), containerTsv);

    // member_census.tsv：版本化表头 + 复合键行序直接落盘（owner → kind
    // Field 在前 → 名字 → 描述符）；Api 的抽象方法 id 首行，Color 的
    // $VALUES 字段（Field 先于 Method）带 26 = PRIVATE|STATIC|FINAL。
    const auto memberTsv = readFileBytes(report.memberCensusFile);
    const std::string memberText(memberTsv.begin(), memberTsv.end());
    EXPECT_EQ(memberText.substr(0, memberText.find('\n') + 1), "# member-census v1\n");
    EXPECT_NE(memberText.find("member_class\tkind\tname\tdescriptor\taccess"
                             "\tnative\tentry\n"),
              std::string::npos);
    // 行数对账：文件数据行 = 版本行 + 列名行 + report.members（转义契约
    // 保证列内无裸换行，换行数即行数）。
    const auto memberLineCount =
        static_cast<std::size_t>(std::count(memberText.begin(), memberText.end(), '\n'));
    EXPECT_EQ(memberLineCount, report.members + 2);
    EXPECT_EQ(memberLineCount, 14U);   // 版本行 + 列名行 + 12 成员
    EXPECT_NE(memberText.find("me/demo/Api\tmethod\tid"
                             "\t(Ljava/lang/Object;)Ljava/lang/Object;\t1025\t0\t"),
              std::string::npos);
    EXPECT_NE(memberText.find("me/demo/Color\tfield\t$VALUES"
                             "\t[Lme/demo/Color;\t26\t0\t"),
              std::string::npos);
    EXPECT_NE(memberText.find("me/demo/Color\tmethod\tvalues"), std::string::npos);
    // access 列十进制（mjbase 无成员旗标命名面，v1 口径）。
    EXPECT_EQ(memberText.find("me/demo/Util\tmethod\tadd\tpublic"), std::string::npos);
    const auto memberAgain = readFileBytes(again.memberCensusFile);
    EXPECT_EQ(memberAgain, memberTsv);
}

TEST(FacadeCensus, MissingJarFailsClosed)
{
    EXPECT_THROW(facadeClassCensusFile(tmpPath("mjt_facade_census_missing.jar"), ""),
                 std::runtime_error);
}

// ── 查询面（analysis.db DB-first，语义随 mjt #94；MCP search/xref 等位物） ──

namespace {

/// 查询面夹具罐（search/xref DB-first 端到端）：Subject.value = ldc String
/// 字面量 + ldc Integer 常量池行 + `bipush 1`（汇编器整型装载糖——落到 ldc
/// CP 行或真立即数行均可能，断言按两源并集写）+ ireturn；Second 对照
///（bipush 2 + 重载对照）；Caller 直调 Subject 方法/字段（xref 探针）；
/// notice.txt 资源串（strings 并集面探针）。jasm 文本 raw string 定版规则
/// 同 makeAmbiguousModuleJar。
std::string makeQueryFacesJar(const char* name)
{
    mjbase::jasm::JvmJasmService assembler;
    const auto assemble = [&](const char* sourceName, const std::string& source) {
        const auto nameText   = utf16Text(sourceName);
        const auto sourceText = utf16Text(source);
        const auto assembled  = assembler.assemble(nameText.ref(), sourceText.ref());
        if (!assembled.isOk()) throw std::runtime_error("query faces fixture did not assemble");
        return assembled.value->bytes;
    };

    const auto subject = assemble("Subject.jasm", R"JASM(.super java/lang/Object
.class public super pysdk/Subject {
  .field public counter I
  .method public static value ()I {
    code: {
      ldc "hello pysdk-fixture world"
      pop
      ldc 100000
      pop
      bipush 1
      ireturn
    }
  }
}
)JASM");
    const auto second = assemble("Second.jasm", R"JASM(.super java/lang/Object
.class public super pysdk/Second {
  .field public counter I
  .method public static value ()I {
    code: {
      bipush 2
      ireturn
    }
  }
  .method public static value (I)I {
    code: {
      iload 0
      ireturn
    }
  }
}
)JASM");
    const auto caller = assemble("Caller.jasm", R"JASM(.super java/lang/Object
.class public super pysdk/Caller {
  .method public static call ()I {
    code: {
      invokestatic pysdk/Subject.value ()I
      ireturn
    }
  }
  .method public static read ()I {
    code: {
      getstatic pysdk/Subject.counter I
      ireturn
    }
  }
}
)JASM");

    mjbase::model::ResourceContainer container;
    container.addResource("pysdk/Subject.class",
                          std::make_unique<mjbase::model::ClassResource>("pysdk/Subject.class",
                                                                         subject));
    container.addResource("pysdk/Second.class",
                          std::make_unique<mjbase::model::ClassResource>("pysdk/Second.class",
                                                                         second));
    container.addResource("pysdk/Caller.class",
                          std::make_unique<mjbase::model::ClassResource>("pysdk/Caller.class",
                                                                         caller));
    container.addResource("notice.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "notice.txt",
                              std::vector<ca::u8>{'p', 'y', 's', 'd', 'k', '-', 'f', 'i', 'x',
                                                  't', 'u', 'r', 'e'}));
    const auto path = tmpPath(name);
    const auto exported = mjbase::model::ResourceHelper::exportToJar(container, path);
    if (exported.is_err()) throw std::runtime_error("query faces fixture export failed");
    return path;
}

/// 开查询面夹具会话（句柄随注册表；用例自行 close）。
FacadeSession& openQueryFacesSession(const char* jarName)
{
    auto& s = FacadeRegistry::instance().create();
    facadeImportJar(s, makeQueryFacesJar(jarName));
    return s;
}

SearchRequest makeSearchRequest(const char* kind, const char* query)
{
    SearchRequest request;
    request.kind      = kind;
    request.query     = query;
    request.matchMode = "contains";
    return request;
}

}   // namespace

TEST(FacadeQuery, NumbersValueParityAndScopes)
{
    auto&  reg = FacadeRegistry::instance();
    auto&  s   = openQueryFacesSession("mjt_facade_query_numbers.jar");
    auto ownerNameInsn = [](const SearchRow& row) {
        return std::make_tuple(row.owner, row.name, row.descriptor, row.instructionIndex);
    };
    auto collect = [&](const char* query, const char* owner = "") {
        auto request = makeSearchRequest("numbers", query);
        request.owner = owner;
        std::vector<std::tuple<std::string, std::string, std::string, ca::i32>> hits;
        for (const auto& row : facadeSearch(s, request)) hits.push_back(ownerNameInsn(row));
        std::sort(hits.begin(), hits.end());
        return hits;
    };

    // bipush 1 命中 Subject.value 方法行（ldc CP 行或真立即数行两源并集）：
    // owner=Subject、value 文本 "1"、kind ∈ 常量池/指令两源。
    const auto ones = collect("1");
    ASSERT_FALSE(ones.empty());
    bool subjectOne = false;
    for (const auto& row : facadeSearch(s, makeSearchRequest("numbers", "1"))) {
        EXPECT_TRUE(row.kind == "constant_pool_number" || row.kind == "instruction_number") << row.kind;
        if (row.owner == "pysdk/Subject" && row.value == "1") subjectOne = true;
    }
    EXPECT_TRUE(subjectOne);

    // "1.0" 与 "1" 数值等值 parity：同一命中行集（值文本存的是入库原文本 "1"）。
    EXPECT_EQ(collect("1.0"), ones);

    // Second（bipush 2）对照："2" 命中 Second、"1" 不命中。
    const auto twos = collect("2");
    ASSERT_FALSE(twos.empty());
    EXPECT_TRUE(std::any_of(twos.begin(), twos.end(), [](const auto& hit) {
        return std::get<0>(hit) == "pysdk/Second";
    }));
    EXPECT_TRUE(std::none_of(ones.begin(), ones.end(), [](const auto& hit) {
        return std::get<0>(hit) == "pysdk/Second";
    }));

    // 常量池行（ldc 100000 → CP Integer）：memberName 空 → kind=
    // constant_pool_number；ldc 指令本体另落方法体行（两源并集，断言按
    // 两源写——bipush 装载糖是否折 CP 同理）。
    bool sawCpNumber = false;
    for (const auto& row : facadeSearch(s, makeSearchRequest("numbers", "100000"))) {
        EXPECT_EQ(row.owner, "pysdk/Subject");
        EXPECT_EQ(row.value, "100000");
        if (row.kind == "constant_pool_number") {
            sawCpNumber = true;
            EXPECT_EQ(row.name, "");
            EXPECT_EQ(row.instructionIndex, -1);
        }
    }
    EXPECT_TRUE(sawCpNumber);

    // owner 作用域收窄：Subject 的 "1" 用 Second 作用域查 → 空集。
    EXPECT_TRUE(collect("1", "pysdk/Second").empty());

    reg.close(s.handle);
}

TEST(FacadeQuery, StringsUnionClassPoolAndResource)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openQueryFacesSession("mjt_facade_query_strings.jar");

    // strings "pysdk-fixture"：类常量池 String 行 + notice.txt 资源串行（并集）。
    const auto rows = facadeSearch(s, makeSearchRequest("strings", "pysdk-fixture"));
    bool hasCpString = false, hasResource = false;
    for (const auto& row : rows) {
        if (row.kind == "constant_pool_string") {
            hasCpString = true;
            EXPECT_EQ(row.value, "hello pysdk-fixture world");
            EXPECT_EQ(row.owner, "pysdk/Subject");
            EXPECT_EQ(row.stringKind, "string_literal");
        }
        if (row.kind == "resource_string") {
            hasResource = true;
            EXPECT_EQ(row.value, "pysdk-fixture");
            EXPECT_EQ(row.resourceUri, "notice.txt");
            EXPECT_EQ(row.stringKind, "string_literal");
        }
    }
    EXPECT_TRUE(hasCpString);
    EXPECT_TRUE(hasResource);

    // string_kind 过滤：string_literal 两路全留；utf8_name 只剩裸 UTF8 名行。
    auto literalRequest = makeSearchRequest("strings", "pysdk-fixture");
    literalRequest.stringKind = "string_literal";
    const auto literals = facadeSearch(s, literalRequest);
    EXPECT_EQ(literals.size(), rows.size());
    auto nameRequest = makeSearchRequest("strings", "value");
    nameRequest.stringKind = "utf8_name";
    bool hasUtf8Name = false;
    for (const auto& row : facadeSearch(s, nameRequest)) {
        EXPECT_EQ(row.kind, "constant_pool_utf8");
        EXPECT_EQ(row.stringKind, "utf8_name");
        if (row.value == "value" && row.owner == "pysdk/Subject") hasUtf8Name = true;
    }
    EXPECT_TRUE(hasUtf8Name);

    // 大小写：缺省 ASCII 折叠命中，case_sensitive 失配零命中。
    auto insensitive = makeSearchRequest("strings", "PYSDK-FIXTURE");
    EXPECT_FALSE(facadeSearch(s, insensitive).empty());
    insensitive.caseSensitive = true;
    EXPECT_TRUE(facadeSearch(s, insensitive).empty());

    // owner 作用域：DB-only（引擎资源串半边在作用域场景本就不出）。
    auto scoped = makeSearchRequest("strings", "pysdk-fixture");
    scoped.owner = "pysdk/Subject";
    const auto scopedRows = facadeSearch(s, scoped);
    EXPECT_EQ(scopedRows.size(), std::size_t{1});
    EXPECT_EQ(scopedRows.front().kind, "constant_pool_string");

    // regex 全量拉取客户端过滤：资源串行照常命中（SQL 无正则等价面，
    // resources 表行随全量拉取参与同面过滤）。
    auto regex = makeSearchRequest("strings", "^pysdk-fixture$");
    regex.matchMode = "regex";
    bool regexResource = false;
    for (const auto& row : facadeSearch(s, regex)) {
        if (row.kind == "resource_string" && row.value == "pysdk-fixture") regexResource = true;
    }
    EXPECT_TRUE(regexResource);

    reg.close(s.handle);
}

TEST(FacadeQuery, ClassesAndMembersMatchModes)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openQueryFacesSession("mjt_facade_query_classes.jar");

    // classes prefix 三类全中（行形：owner = value = 内部名）。
    auto prefix = makeSearchRequest("classes", "pysdk/");
    prefix.matchMode = "prefix";
    const auto prefixed = facadeSearch(s, prefix);
    ASSERT_EQ(prefixed.size(), std::size_t{3});
    for (const auto& row : prefixed) {
        EXPECT_EQ(row.kind, "class");
        EXPECT_EQ(row.owner, row.value);
        EXPECT_EQ(row.resourceUri, row.owner + ".class");
    }

    // exact 单中；大小写缺省折叠 / 敏感失配。
    auto exact = makeSearchRequest("classes", "pysdk/Subject");
    exact.matchMode = "exact";
    const auto exactRows = facadeSearch(s, exact);
    ASSERT_EQ(exactRows.size(), std::size_t{1});
    EXPECT_EQ(exactRows.front().owner, "pysdk/Subject");
    auto insensitive = makeSearchRequest("classes", "SUBJECT");
    EXPECT_EQ(facadeSearch(s, insensitive).size(), std::size_t{1});
    insensitive.caseSensitive = true;
    EXPECT_TRUE(facadeSearch(s, insensitive).empty());

    // regex 全量拉取客户端过滤：单中 Subject（DB 面无正则等价）。
    auto regex = makeSearchRequest("classes", "^pysdk/Sub.*$");
    regex.matchMode = "regex";
    const auto regexRows = facadeSearch(s, regex);
    ASSERT_EQ(regexRows.size(), std::size_t{1});
    EXPECT_EQ(regexRows.front().owner, "pysdk/Subject");

    // members：方法行（Second 重载一并命中，value = name+desc 拼接）。
    const auto methods = facadeSearch(s, makeSearchRequest("members", "value"));
    ASSERT_EQ(methods.size(), std::size_t{3});   // Subject.value + Second.value × 2
    for (const auto& row : methods) EXPECT_EQ(row.kind, "method");
    // member_kind 过滤：字段行 counter（value = "counterI"）；method 档零命中。
    auto fields = makeSearchRequest("members", "counter");
    fields.memberKind = "field";
    const auto fieldRows = facadeSearch(s, fields);
    ASSERT_EQ(fieldRows.size(), std::size_t{2});   // Subject.counter + Second.counter
    EXPECT_EQ(fieldRows.front().kind, "field");
    EXPECT_EQ(fieldRows.front().descriptor, "I");
    auto wrongKind = makeSearchRequest("members", "value");
    wrongKind.memberKind = "field";
    EXPECT_TRUE(facadeSearch(s, wrongKind).empty());

    reg.close(s.handle);
}

TEST(FacadeQuery, ParamGuardsFailClosed)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openQueryFacesSession("mjt_facade_query_guards.jar");

    // kind / match_mode 词表外。
    EXPECT_THROW(facadeSearch(s, makeSearchRequest("instructions", "x")), std::invalid_argument);
    auto badMode = makeSearchRequest("classes", "x");
    badMode.matchMode = "glob";
    EXPECT_THROW(facadeSearch(s, badMode), std::invalid_argument);
    // numbers 无 match_mode（值等值语义）。
    auto numbersMode = makeSearchRequest("numbers", "1");
    numbersMode.matchMode = "prefix";
    EXPECT_THROW(facadeSearch(s, numbersMode), std::invalid_argument);
    // 过滤位错位：classes 不吃 owner；strings 不吃 member_kind；members 词表外。
    auto scopedClasses = makeSearchRequest("classes", "x");
    scopedClasses.owner = "pysdk/Subject";
    EXPECT_THROW(facadeSearch(s, scopedClasses), std::invalid_argument);
    auto stringsMember = makeSearchRequest("strings", "x");
    stringsMember.memberKind = "field";
    EXPECT_THROW(facadeSearch(s, stringsMember), std::invalid_argument);
    auto badMemberKind = makeSearchRequest("members", "x");
    badMemberKind.memberKind = "constructor";
    EXPECT_THROW(facadeSearch(s, badMemberKind), std::invalid_argument);
    auto badStringKind = makeSearchRequest("strings", "x");
    badStringKind.stringKind = "utf8_other";
    EXPECT_THROW(facadeSearch(s, badStringKind), std::invalid_argument);
    auto badOpcode = makeSearchRequest("numbers", "1");
    badOpcode.opcode = 250;
    EXPECT_THROW(facadeSearch(s, badOpcode), std::invalid_argument);
    // query 边界态走引擎校验（空 query 报错，不静默全量）。
    EXPECT_THROW(facadeSearch(s, makeSearchRequest("classes", "")), std::runtime_error);

    reg.close(s.handle);
}

TEST(FacadeXref, ReferencesToAndFrom)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openQueryFacesSession("mjt_facade_xref.jar");

    // to-method（显式 descriptor）：Caller.call 指令级直调行。
    XrefRequest toMethod;
    toMethod.target     = "method";
    toMethod.direction  = "to";
    toMethod.owner      = "pysdk/Subject";
    toMethod.name       = "value";
    toMethod.descriptor = "()I";
    const auto methodRefs = facadeXref(s, toMethod);
    ASSERT_EQ(methodRefs.size(), std::size_t{1});
    EXPECT_EQ(methodRefs.front().sourceClass, "pysdk/Caller");
    EXPECT_EQ(methodRefs.front().sourceMethod, "call");
    EXPECT_EQ(methodRefs.front().target, "pysdk/Subject.value()I");
    EXPECT_EQ(methodRefs.front().kind, "direct");
    EXPECT_EQ(methodRefs.front().instructionIndex, 0);
    EXPECT_FALSE(methodRefs.front().supporting);

    // descriptor 省略回填（宿主类名称唯一命中）：同 1 行。
    XrefRequest backfilled = toMethod;
    backfilled.descriptor.clear();
    const auto backfilledRefs = facadeXref(s, backfilled);
    ASSERT_EQ(backfilledRefs.size(), std::size_t{1});
    EXPECT_EQ(backfilledRefs.front().target, "pysdk/Subject.value()I");

    // to-field：Caller.read 的 getstatic 行。
    XrefRequest toField;
    toField.target     = "field";
    toField.owner      = "pysdk/Subject";
    toField.name       = "counter";
    toField.descriptor = "I";
    const auto fieldRefs = facadeXref(s, toField);
    ASSERT_EQ(fieldRefs.size(), std::size_t{1});
    EXPECT_EQ(fieldRefs.front().sourceMethod, "read");
    EXPECT_EQ(fieldRefs.front().target, "pysdk/Subject.counterI");
    EXPECT_EQ(fieldRefs.front().kind, "direct");

    // to-class：invoke/getstatic 的 owner 类引用双行（Type 切片，target=内部名）。
    XrefRequest toClass;
    toClass.target = "class";
    toClass.owner  = "pysdk/Subject";
    const auto classRefs = facadeXref(s, toClass);
    ASSERT_EQ(classRefs.size(), std::size_t{2});
    bool sawCall = false, sawRead = false;
    for (const auto& row : classRefs) {
        EXPECT_EQ(row.kind, "type");
        EXPECT_EQ(row.target, "pysdk/Subject");
        if (row.sourceMethod == "call") sawCall = true;
        if (row.sourceMethod == "read") sawRead = true;
    }
    EXPECT_TRUE(sawCall && sawRead);

    // from-method：callees 切片面（只取 Method 行）——Caller.call 发出的
    // Subject.value 直调。
    XrefRequest fromMethod;
    fromMethod.target     = "method";
    fromMethod.direction  = "from";
    fromMethod.owner      = "pysdk/Caller";
    fromMethod.name       = "call";
    fromMethod.descriptor = "()I";
    const auto callees = facadeXref(s, fromMethod);
    ASSERT_EQ(callees.size(), std::size_t{1});
    EXPECT_EQ(callees.front().sourceClass, "pysdk/Caller");
    EXPECT_EQ(callees.front().target, "pysdk/Subject.value()I");
    EXPECT_EQ(callees.front().kind, "direct");

    reg.close(s.handle);
}

TEST(FacadeXref, SelectorAndParamGuards)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openQueryFacesSession("mjt_facade_xref_guards.jar");

    // target / direction 词表外；from 只对 method 开放。
    XrefRequest badTarget;
    badTarget.target = "package";
    badTarget.owner  = "pysdk/Subject";
    EXPECT_THROW(facadeXref(s, badTarget), std::invalid_argument);
    XrefRequest badDirection = badTarget;
    badDirection.target    = "method";
    badDirection.direction = "into";
    EXPECT_THROW(facadeXref(s, badDirection), std::invalid_argument);
    XrefRequest fromField;
    fromField.target    = "field";
    fromField.direction = "from";
    fromField.owner     = "pysdk/Subject";
    fromField.name      = "counter";
    EXPECT_THROW(facadeXref(s, fromField), std::invalid_argument);
    // selector 缺失。
    XrefRequest noOwner;
    noOwner.target = "class";
    EXPECT_THROW(facadeXref(s, noOwner), std::invalid_argument);
    // 名字不存在（宿主类在索引）→ invalid_argument；宿主类未索引 → StaleRefError
    //（direction 走结构体缺省 "to"：合法方向 + 表外宿主类必须落到键失效路径，
    // 而不是被方向守卫拦下——CI 实证回归位）。
    XrefRequest missing;
    missing.target = "method";
    missing.owner  = "pysdk/Subject";
    missing.name   = "missing";
    EXPECT_THROW(facadeXref(s, missing), std::invalid_argument);
    XrefRequest foreign;
    foreign.target    = "method";
    foreign.direction = "to";
    foreign.owner     = "java/lang/String";
    foreign.name      = "length";
    EXPECT_THROW(facadeXref(s, foreign), StaleRefError);
    // 直构缺省方向 = to：同表外目标不显式给 direction 也走 StaleRefError。
    XrefRequest foreignDefaultDirection;
    foreignDefaultDirection.target = "method";
    foreignDefaultDirection.owner  = "java/lang/String";
    foreignDefaultDirection.name   = "length";
    EXPECT_THROW(facadeXref(s, foreignDefaultDirection), StaleRefError);
    // 重载歧义：Second.value × 2 无 descriptor → 列候选报错。
    XrefRequest ambiguous;
    ambiguous.target = "method";
    ambiguous.owner  = "pysdk/Second";
    ambiguous.name   = "value";
    EXPECT_THROW(facadeXref(s, ambiguous), std::invalid_argument);
    // 显式 descriptor 的表外目标直通（引用查询零漂移）。
    XrefRequest foreignExplicit = foreign;
    foreignExplicit.descriptor = "()I";
    const auto foreignRefs = facadeXref(s, foreignExplicit);
    EXPECT_TRUE(foreignRefs.empty());   // 容器内无指向 JDK 的该引用 → 空集非错

    reg.close(s.handle);
}

// ── 层次闭包面（analysis.db DB-only，无引擎回落；mjbase AnalysisDb 等位物） ──

namespace {

/// 层次闭包夹具罐（hierarchy DB-first 端到端）：Base←Mid←Leaf 传递链 + 兄弟
/// Other（direct_subclasses 排序对照）；IFoo 接口 + 子接口 IBar（接口闭包经
/// implements 边展开）+ Impl implements IFoo + SubImpl extends Impl implements
/// IBar（implementors 后代展开）。全部空体类——层次查询只吃类头事实（super/
/// interfaces），方法体零成本。jasm 文本 raw string 定版规则同 makeQueryFacesJar。
std::string makeHierarchyJar(const char* name)
{
    mjbase::jasm::JvmJasmService assembler;
    const auto assemble = [&](const char* sourceName, const std::string& source) {
        const auto nameText   = utf16Text(sourceName);
        const auto sourceText = utf16Text(source);
        const auto assembled  = assembler.assemble(nameText.ref(), sourceText.ref());
        if (!assembled.isOk()) throw std::runtime_error("hierarchy fixture did not assemble");
        return assembled.value->bytes;
    };

    const std::pair<const char*, const char*> sources[] = {
        {"Base.jasm",
         ".super java/lang/Object\n"
         ".class public super pysdk/hier/Base {\n"
         "}\n"},
        {"Mid.jasm",
         ".super pysdk/hier/Base\n"
         ".class public super pysdk/hier/Mid {\n"
         "}\n"},
        {"Leaf.jasm",
         ".super pysdk/hier/Mid\n"
         ".class public super pysdk/hier/Leaf {\n"
         "}\n"},
        {"Other.jasm",
         ".super pysdk/hier/Base\n"
         ".class public super pysdk/hier/Other {\n"
         "}\n"},
        {"IFoo.jasm",
         ".super java/lang/Object\n"
         ".class public interface abstract pysdk/hier/IFoo {\n"
         "}\n"},
        {"IBar.jasm",
         ".super java/lang/Object\n"
         ".implements pysdk/hier/IFoo\n"
         ".class public interface abstract pysdk/hier/IBar {\n"
         "}\n"},
        {"Impl.jasm",
         ".super java/lang/Object\n"
         ".implements pysdk/hier/IFoo\n"
         ".class public super pysdk/hier/Impl {\n"
         "}\n"},
        {"SubImpl.jasm",
         ".super pysdk/hier/Impl\n"
         ".implements pysdk/hier/IBar\n"
         ".class public super pysdk/hier/SubImpl {\n"
         "}\n"},
    };

    mjbase::model::ResourceContainer container;
    constexpr std::string_view kSourceSuffix = ".jasm";
    for (const auto& [sourceName, source] : sources) {
        const std::string stem(sourceName,
                               std::strlen(sourceName) - kSourceSuffix.size());
        const std::string entry = "pysdk/hier/" + stem + ".class";
        container.addResource(entry,
                              std::make_unique<mjbase::model::ClassResource>(
                                  entry, assemble(sourceName, source)));
    }
    const auto path = tmpPath(name);
    const auto exported = mjbase::model::ResourceHelper::exportToJar(container, path);
    if (exported.is_err()) throw std::runtime_error("hierarchy fixture export failed");
    return path;
}

/// 开层次夹具会话（句柄随注册表；用例自行 close）。
FacadeSession& openHierarchySession(const char* jarName)
{
    auto& s = FacadeRegistry::instance().create();
    facadeImportJar(s, makeHierarchyJar(jarName));
    return s;
}

HierarchyRequest makeHierarchyRequest(const char* kind, const char* query)
{
    HierarchyRequest request;
    request.kind  = kind;
    request.query = query;
    return request;
}

/// 行类名列表（命中集合断言辅助）。
std::vector<std::string> hierarchyNames(const std::vector<HierarchyRow>& rows)
{
    std::vector<std::string> names;
    names.reserve(rows.size());
    for (const auto& row : rows) names.push_back(row.className);
    return names;
}

}   // namespace

TEST(FacadeHierarchy, ChainSubclassesAndImplementors)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openHierarchySession("mjt_facade_hierarchy.jar");

    // super_chain：含自身、最近在前；未登记父类名（java/lang/Object）计入链尾
    // 后停止——链行 uri 逐行回填，链尾名不在库 → uri 空串。
    const auto chain = facadeHierarchy(s, makeHierarchyRequest("super_chain", "pysdk/hier/Leaf"));
    ASSERT_EQ(chain.size(), std::size_t{4});
    EXPECT_EQ(hierarchyNames(chain),
              (std::vector<std::string>{"pysdk/hier/Leaf", "pysdk/hier/Mid", "pysdk/hier/Base",
                                        "java/lang/Object"}));
    for (std::size_t i = 0; i < 3; ++i)
        EXPECT_EQ(chain[i].resourceUri, chain[i].className + ".class");
    EXPECT_EQ(chain.back().resourceUri, "");

    // 短链对照：registered 起点恒有自身行（链首）。
    const auto shortChain =
        facadeHierarchy(s, makeHierarchyRequest("super_chain", "pysdk/hier/Base"));
    ASSERT_EQ(shortChain.size(), std::size_t{2});
    EXPECT_EQ(shortChain.front().className, "pysdk/hier/Base");
    EXPECT_EQ(shortChain.back().className, "java/lang/Object");

    // direct_subclasses：仅 extends 边 + internal_name 升序（Mid < Other）。
    const auto direct =
        facadeHierarchy(s, makeHierarchyRequest("direct_subclasses", "pysdk/hier/Base"));
    EXPECT_EQ(hierarchyNames(direct),
              (std::vector<std::string>{"pysdk/hier/Mid", "pysdk/hier/Other"}));
    for (const auto& row : direct) EXPECT_EQ(row.resourceUri, row.className + ".class");
    // 叶子无子类 → 空集非错。
    EXPECT_TRUE(facadeHierarchy(s, makeHierarchyRequest("direct_subclasses", "pysdk/hier/Leaf"))
                    .empty());

    // all_subclasses：传递闭包（不含自身）升序。
    EXPECT_EQ(hierarchyNames(facadeHierarchy(s, makeHierarchyRequest("all_subclasses",
                                                                     "pysdk/hier/Base"))),
              (std::vector<std::string>{"pysdk/hier/Leaf", "pysdk/hier/Mid", "pysdk/hier/Other"}));
    EXPECT_TRUE(facadeHierarchy(s, makeHierarchyRequest("all_subclasses", "pysdk/hier/Leaf"))
                    .empty());

    // implementors：接口自身闭包（含子接口 IBar 经 implements 边入列）+ 实现
    // 者全部后代（SubImpl extends Impl）。
    EXPECT_EQ(hierarchyNames(facadeHierarchy(s, makeHierarchyRequest("implementors",
                                                                     "pysdk/hier/IFoo"))),
              (std::vector<std::string>{"pysdk/hier/IBar", "pysdk/hier/Impl",
                                        "pysdk/hier/SubImpl"}));
    // 子接口视角（mjbase implements-边闭包契约，CI 实证钉死）：ifaces 闭包 =
    // {IBar} ∪ IBar 声明的接口 = {IBar, IFoo}；impls = 声明任一闭包接口的类
    // ∪ 其后代——IBar 自己声明 IFoo → IBar 入列，Impl 声明 IFoo → 同样入列，
    // 加 SubImpl（声明 IBar）→ 三类齐全，与 IFoo 视角同集。这不是引擎
    // classRelations 的 JVM 子型语义（那边 Impl 不是 IBar 子型，只有
    // SubImpl）——DB 面原样透出 mjbase kImplementorsSql 闭包，门面零重排。
    EXPECT_EQ(hierarchyNames(facadeHierarchy(s, makeHierarchyRequest("implementors",
                                                                     "pysdk/hier/IBar"))),
              (std::vector<std::string>{"pysdk/hier/IBar", "pysdk/hier/Impl",
                                        "pysdk/hier/SubImpl"}));

    reg.close(s.handle);
}

TEST(FacadeHierarchy, PagedWindowsConcatenateToFull)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openHierarchySession("mjt_facade_hierarchy_paged.jar");

    const auto full = facadeHierarchy(s, makeHierarchyRequest("all_subclasses", "pysdk/hier/Base"));
    ASSERT_EQ(full.size(), std::size_t{3});

    // 各页按 offset 升序拼接 = 全量结果（跨页不重不漏）。
    std::vector<std::string> concatenated;
    for (ca::i64 offset = 0; offset < 3; ++offset) {
        auto request  = makeHierarchyRequest("all_subclasses", "pysdk/hier/Base");
        request.limit = 1;
        request.offset = offset;
        auto page     = facadeHierarchy(s, request);
        ASSERT_EQ(page.size(), std::size_t{1});
        EXPECT_EQ(page.front().resourceUri, page.front().className + ".class");
        concatenated.push_back(page.front().className);
    }
    EXPECT_EQ(concatenated, hierarchyNames(full));

    // 多行页 + 尾页取剩余；offset 超闭包 → 空页；limit=0 → 空页。
    auto tail        = makeHierarchyRequest("all_subclasses", "pysdk/hier/Base");
    tail.limit       = 2;
    tail.offset      = 1;
    EXPECT_EQ(hierarchyNames(facadeHierarchy(s, tail)),
              (std::vector<std::string>{"pysdk/hier/Mid", "pysdk/hier/Other"}));
    auto beyond = tail;
    beyond.offset = 7;
    EXPECT_TRUE(facadeHierarchy(s, beyond).empty());
    auto zero = tail;
    zero.limit = 0;
    EXPECT_TRUE(facadeHierarchy(s, zero).empty());

    // implementors 同一窗口契约（3 行闭包逐页拼回）。
    std::vector<std::string> implPages;
    for (ca::i64 offset = 0; offset < 3; ++offset) {
        auto request   = makeHierarchyRequest("implementors", "pysdk/hier/IFoo");
        request.limit  = 1;
        request.offset = offset;
        auto page      = facadeHierarchy(s, request);
        ASSERT_EQ(page.size(), std::size_t{1});
        implPages.push_back(page.front().className);
    }
    EXPECT_EQ(implPages, hierarchyNames(facadeHierarchy(s, makeHierarchyRequest(
                                                   "implementors", "pysdk/hier/IFoo"))));

    // 负值窗口走库面 Invalid（不重复口径）→ RuntimeError。
    auto negative = makeHierarchyRequest("all_subclasses", "pysdk/hier/Base");
    negative.limit = -1;
    negative.offset = 0;
    EXPECT_THROW(facadeHierarchy(s, negative), std::runtime_error);
    auto negativeOffset = negative;
    negativeOffset.limit  = 1;
    negativeOffset.offset = -2;
    EXPECT_THROW(facadeHierarchy(s, negativeOffset), std::runtime_error);

    reg.close(s.handle);
}

TEST(FacadeHierarchy, ParamGuardsFailClosed)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openHierarchySession("mjt_facade_hierarchy_guards.jar");

    // kind 词表外 / 空 query。
    EXPECT_THROW(facadeHierarchy(s, makeHierarchyRequest("ancestors", "pysdk/hier/Base")),
                 std::invalid_argument);
    EXPECT_THROW(facadeHierarchy(s, makeHierarchyRequest("super_chain", "")),
                 std::invalid_argument);
    // 起点未登记 → 明确寻址错误（mjbase 面未登记起点返回空表，pysdk 升格）。
    try {
        facadeHierarchy(s, makeHierarchyRequest("super_chain", "pysdk/hier/Nope"));
        FAIL() << "unregistered start class must fail closed";
    }
    catch (const std::invalid_argument& error) {
        EXPECT_NE(std::string(error.what()).find("not registered"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("pysdk/hier/Nope"), std::string::npos);
    }
    // 分页位错位：limit/offset 缺一不成对；super_chain/direct_subclasses 不吃分页。
    auto halfWindow           = makeHierarchyRequest("all_subclasses", "pysdk/hier/Base");
    halfWindow.limit          = 1;
    EXPECT_THROW(facadeHierarchy(s, halfWindow), std::invalid_argument);
    auto chainPaged           = makeHierarchyRequest("super_chain", "pysdk/hier/Leaf");
    chainPaged.limit          = 1;
    chainPaged.offset         = 0;
    EXPECT_THROW(facadeHierarchy(s, chainPaged), std::invalid_argument);
    auto directPaged          = makeHierarchyRequest("direct_subclasses", "pysdk/hier/Base");
    directPaged.limit         = 1;
    directPaged.offset        = 0;
    EXPECT_THROW(facadeHierarchy(s, directPaged), std::invalid_argument);

    reg.close(s.handle);
}

// ── 会话级分析库（编辑边界增量同步 + 结构变更重建；新鲜度契约钉死面） ──────
//
// 查询面底座从「每命令临时库全量 ingest」换成「会话持库 + 编辑边界增量同
// 步」：内容编辑（write_back / jasm apply / set_class_bytes）按脏类追平库，
// 结构变更（add/remove/import/rename/token 提交）标 stale 下次查询全量重
// 建。本组钉死新鲜度契约：边界同步后查询必须看到编辑后状态；脏树不参与
// 口径保持（未 write_back 的树面编辑不可见）。

/// 末条指令 uid（单插入点探针；kInsn kind 过滤）。
static std::optional<ca::u32> lastInsnUid(FacadeSession& s, const std::string& cls,
                                          const std::string& name, const std::string& desc)
{
    const auto briefs = facadeInsns(s, cls, name, desc);
    for (auto it = briefs.rbegin(); it != briefs.rend(); ++it) {
        if (it->kind == insn_kind::kInsn) return it->uid;
    }
    return std::nullopt;
}

TEST(FacadeSessionDb, NumbersFreshAfterInsnInsertAndRemove)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openQueryFacesSession("mjt_facade_session_numbers.jar");

    // 基线：42 无命中（夹具常量为 1/2/100000）。
    auto numbers = [&](const char* query) {
        return facadeSearch(s, makeSearchRequest("numbers", query));
    };
    ASSERT_TRUE(numbers("42").empty());

    // 内容编辑：Subject.value 末尾 ireturn 前插 `sipush 42; pop`（emit 是
    // raw 形态，无整型装载糖——立即数恒落指令行，探针确定；pop 保持栈形
    // 中性，compute_frames=false 的写回纪律不被探针牵连）。写回落账 → 会话
    // 库增量同步（成员键不变，无幽灵 → 走增量路径）→ 查询即见。
    const auto ret = lastInsnUid(s, "pysdk/Subject", "value", "()I");
    ASSERT_TRUE(ret.has_value());
    const auto sipush = facadeEmitInsn(s, "pysdk/Subject", "value", "()I", 17 /*SIPUSH*/, *ret);
    facadeInsnSetOperand(s, "pysdk/Subject", "value", "()I", sipush, 42);
    facadeEmitInsn(s, "pysdk/Subject", "value", "()I", 87 /*POP*/, *ret);
    facadeWriteBack(s, "pysdk/Subject", false);

    const auto rows = numbers("42");
    ASSERT_EQ(rows.size(), std::size_t{1});
    EXPECT_EQ(rows.front().kind, "instruction_number");
    EXPECT_EQ(rows.front().owner, "pysdk/Subject");
    EXPECT_EQ(rows.front().name, "value");
    EXPECT_EQ(rows.front().value, "42");
    EXPECT_GE(rows.front().instructionIndex, 0);

    // 反向：摘掉 sipush 再写回 → 数值切片随编辑消失（空切片替换清旧行）。
    facadeRemoveInsn(s, "pysdk/Subject", "value", "()I", sipush);
    facadeWriteBack(s, "pysdk/Subject", false);
    EXPECT_TRUE(numbers("42").empty());

    reg.close(s.handle);
}

TEST(FacadeSessionDb, XrefFreshAfterCallRetarget)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openQueryFacesSession("mjt_facade_session_xref.jar");

    const auto xrefTo = [&](const char* name, const char* desc) {
        XrefRequest request;
        request.target     = "method";
        request.direction  = "to";
        request.owner      = "pysdk/Subject";
        request.name       = name;
        request.descriptor = desc;
        return facadeXref(s, request);
    };

    // 基线：Caller.call 直调 Subject.value；read 无调用方。
    ASSERT_EQ(xrefTo("value", "()I").size(), std::size_t{1});
    ASSERT_TRUE(xrefTo("read", "()I").empty());

    // 内容编辑：call 体的 invokestatic 改靶 value → read（栈形不变）。写回
    // 落账 → 引用切片换新（旧靶行清、新靶行入）→ 双向查询即见。invoke 家族
    // kind 外显是 kMethod（非 kInsn），opcode 定位即可（label/line 节点
    // opcode 为 -1，不会混入）。
    const auto briefs = facadeInsns(s, "pysdk/Caller", "call", "()I");
    ca::u32 invokeUid = 0;
    for (const auto& brief : briefs) {
        if (brief.opcode == 184 /*INVOKESTATIC*/) {
            invokeUid = brief.uid;
        }
    }
    ASSERT_NE(invokeUid, ca::u32{0});
    facadeInsnSetMethodRef(s, "pysdk/Caller", "call", "()I", invokeUid, "pysdk/Subject",
                           "read", "()I", false);
    facadeWriteBack(s, "pysdk/Caller", false);

    EXPECT_TRUE(xrefTo("value", "()I").empty());
    const auto retargeted = xrefTo("read", "()I");
    ASSERT_EQ(retargeted.size(), std::size_t{1});
    EXPECT_EQ(retargeted.front().sourceClass, "pysdk/Caller");
    EXPECT_EQ(retargeted.front().sourceMethod, "call");
    EXPECT_EQ(retargeted.front().target, "pysdk/Subject.read()I");

    // from 面（callees 切片）同步跟随：call 现在只发 read 一条。
    XrefRequest from;
    from.target    = "method";
    from.direction = "from";
    from.owner     = "pysdk/Caller";
    from.name      = "call";
    from.descriptor = "()I";
    const auto callees = facadeXref(s, from);
    ASSERT_EQ(callees.size(), std::size_t{1});
    EXPECT_EQ(callees.front().target, "pysdk/Subject.read()I");

    reg.close(s.handle);
}

TEST(FacadeSessionDb, MemberRenameLeavesNoGhost)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openQueryFacesSession("mjt_facade_session_member_rename.jar");

    // 基线：value 三行（Subject.value + Second.value 两个重载）。
    const auto baseline = facadeSearch(s, makeSearchRequest("members", "value"));
    ASSERT_EQ(baseline.size(), std::size_t{3});

    // 成员改名 = 内容编辑 + 成员键漂移：mjbase 成员身份只增不删，旧键行是
    // 幽灵——同步的幽灵检查标 stale，下次查询全量重建（无遮蔽层的唯一出口）。
    facadeRenameMethod(s, "pysdk/Second", "value", "()I", "value2");
    facadeWriteBack(s, "pysdk/Second", false);

    const auto after = facadeSearch(s, makeSearchRequest("members", "value"));
    // contains 语义：needle "value" 同时命中改名后的 value2——行数不降，
    // 幽灵判定看键：旧键 (Second, value, ()I) 必须消失。
    ASSERT_EQ(after.size(), std::size_t{3});
    for (const auto& row : after) {
        EXPECT_FALSE(row.owner == "pysdk/Second" && row.descriptor == "()I" &&
                     row.name == "value");
    }
    const auto renamed = facadeSearch(s, makeSearchRequest("members", "value2"));
    ASSERT_EQ(renamed.size(), std::size_t{1});
    EXPECT_EQ(renamed.front().owner, "pysdk/Second");
    EXPECT_EQ(renamed.front().descriptor, "()I");
    EXPECT_EQ(renamed.front().value, "value2()I");

    reg.close(s.handle);
}

TEST(FacadeSessionDb, ClassRenameLeavesNoGhost)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openQueryFacesSession("mjt_facade_session_class_rename.jar");

    auto exact = [&](const char* query) {
        auto request = makeSearchRequest("classes", query);
        request.matchMode = "exact";
        return facadeSearch(s, request);
    };
    ASSERT_EQ(exact("pysdk/Second").size(), std::size_t{1});

    // 类改名 = 结构变更面（条目 URI 迁移 + mjbase 类身份只增不删的旧名幽灵
    // 行）→ 库标 stale，下次查询全量重建后新名可见、旧名无幽灵。
    EXPECT_EQ(facadeRenameClass(s, "pysdk/Second", "pysdk/Second2"), "pysdk/Second2");
    facadeWriteBack(s, "pysdk/Second2", false);   // 字节落账 + 条目搬移（sync_entries）

    EXPECT_TRUE(exact("pysdk/Second").empty());
    const auto renamed = exact("pysdk/Second2");
    ASSERT_EQ(renamed.size(), std::size_t{1});
    EXPECT_EQ(renamed.front().resourceUri, "pysdk/Second2.class");
    EXPECT_EQ(renamed.front().owner, "pysdk/Second2");
    // 成员随类改名整体重建后仍在（宿主名跟到新名）。
    const auto values = facadeSearch(s, makeSearchRequest("members", "value"));
    ASSERT_EQ(values.size(), std::size_t{3});
    bool second2Value = false, subjectValue = false;
    for (const auto& row : values) {
        if (row.owner == "pysdk/Second2") second2Value = true;
        if (row.owner == "pysdk/Subject") subjectValue = true;
    }
    EXPECT_TRUE(second2Value);
    EXPECT_TRUE(subjectValue);

    reg.close(s.handle);
}

TEST(FacadeSessionDb, AddRemoveEntrySeesNewClass)
{
    auto& reg = FacadeRegistry::instance();

    // 源会话：树面造一个新名类（Util → Extra），写回落账后取字节。
    auto& src = reg.create();
    facadeImportJar(src, makeDemoJar("mjt_facade_session_entry_src.jar"));
    ASSERT_EQ(facadeRenameClass(src, "me/demo/Util", "me/demo/Extra"), "me/demo/Extra");
    facadeWriteBack(src, "me/demo/Extra", false);
    const auto extraBytes = facadeClassBytes(src, "me/demo/Extra");
    reg.close(src.handle);

    auto& s = reg.create();
    facadeImportJar(s, makeDemoJar("mjt_facade_session_entry.jar"));

    auto exact = [&](const char* query) {
        auto request = makeSearchRequest("classes", query);
        request.matchMode = "exact";
        return facadeSearch(s, request);
    };
    // 先建库（查询一次），再走结构变更面。
    ASSERT_TRUE(exact("me/demo/Extra").empty());

    // add_entry：新条目 = 可见域变更 → 重建 → 新类即时可见。
    facadeAddEntry(s, "me/demo/Extra.class", extraBytes);
    const auto added = exact("me/demo/Extra");
    ASSERT_EQ(added.size(), std::size_t{1});
    EXPECT_EQ(added.front().resourceUri, "me/demo/Extra.class");

    // remove_entry：条目消失 → 重建 → 查询不可见。
    facadeRemoveEntry(s, "me/demo/Extra.class");
    EXPECT_TRUE(exact("me/demo/Extra").empty());

    reg.close(s.handle);
}

TEST(FacadeSessionDb, HierarchyFreshAfterSuperEditAndDirtyTreeExcluded)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = reg.create();
    facadeImportJar(s, makeDemoJar("mjt_facade_session_hier.jar"));

    const auto chain = [&] {
        std::vector<std::string> names;
        for (const auto& row :
             facadeHierarchy(s, makeHierarchyRequest("super_chain", "me/demo/App"))) {
            names.push_back(row.className);
        }
        return names;
    };

    // 基线链：App →（未登记）java/lang/Object。
    ASSERT_EQ(chain(), (std::vector<std::string>{"me/demo/App", "java/lang/Object"}));

    // 类头编辑（super 改写）→ flush 落账 → 会话库类事实 upsert + 类头引用
    // 切片换新 → 层次闭包即见新链。
    facadeClassSetSuper(s, "me/demo/App", "me/demo/Crypto");
    facadeFlush(s, false);
    EXPECT_EQ(chain(),
              (std::vector<std::string>{"me/demo/App", "me/demo/Crypto", "java/lang/Object"}));

    // 脏树不参与口径保持：未 flush 的 super 改写不进查询口径。
    facadeClassSetSuper(s, "me/demo/App", "me/demo/Util");
    EXPECT_EQ(chain(),
              (std::vector<std::string>{"me/demo/App", "me/demo/Crypto", "java/lang/Object"}));

    // flush 后再查：新 super 即见。
    facadeFlush(s, false);
    EXPECT_EQ(chain(),
              (std::vector<std::string>{"me/demo/App", "me/demo/Util", "java/lang/Object"}));

    reg.close(s.handle);
}

TEST(FacadeSessionDb, SetClassBytesFreshNumbers)
{
    auto& reg = FacadeRegistry::instance();

    // 源会话：树面造 sipush 42 探针字节（Subject.value），写回落账后取字节。
    auto& src = reg.create();
    facadeImportJar(src, makeQueryFacesJar("mjt_facade_session_setbytes_src.jar"));
    const auto ret = lastInsnUid(src, "pysdk/Subject", "value", "()I");
    ASSERT_TRUE(ret.has_value());
    const auto sipush = facadeEmitInsn(src, "pysdk/Subject", "value", "()I", 17, *ret);
    facadeInsnSetOperand(src, "pysdk/Subject", "value", "()I", sipush, 42);
    facadeEmitInsn(src, "pysdk/Subject", "value", "()I", 87, *ret);
    facadeWriteBack(src, "pysdk/Subject", false);
    const auto editedBytes = facadeClassBytes(src, "pysdk/Subject");
    reg.close(src.handle);

    // 被测会话：先建库（基线无 42），再走 set_class_bytes 内容编辑边界
    //（与 jasm apply 同一同步入口）→ numbers 即见探针。
    auto& s = reg.create();
    facadeImportJar(s, makeQueryFacesJar("mjt_facade_session_setbytes.jar"));
    ASSERT_TRUE(facadeSearch(s, makeSearchRequest("numbers", "42")).empty());
    facadeSetClassBytes(s, "pysdk/Subject", editedBytes);
    const auto rows = facadeSearch(s, makeSearchRequest("numbers", "42"));
    ASSERT_EQ(rows.size(), std::size_t{1});
    EXPECT_EQ(rows.front().owner, "pysdk/Subject");
    EXPECT_EQ(rows.front().value, "42");

    reg.close(s.handle);
}

TEST(FacadeSessionDb, SyncUnknownClassIsSilentAndInvalidateRebuildsClean)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openQueryFacesSession("mjt_facade_session_syncedge.jar");

    // 同步入口对未知名静默（fail-open 纪律：同步永远不炸调用方）。
    EXPECT_NO_THROW(facadeSyncAnalysisDbAfterEdit(s, "pysdk/Nope"));

    auto prefix = makeSearchRequest("classes", "pysdk/");
    prefix.matchMode = "prefix";
    ASSERT_EQ(facadeSearch(s, prefix).size(), std::size_t{3});

    // 显式失效（结构变更面共用入口）→ 下次查询全量重建，口径不变。
    facadeInvalidateAnalysisDb(s);
    EXPECT_EQ(facadeSearch(s, prefix).size(), std::size_t{3});

    reg.close(s.handle);
}

TEST(FacadeSessionDb, SessionTempDirCleanedOnClose)
{
    auto& reg = FacadeRegistry::instance();
    auto& s   = openQueryFacesSession("mjt_facade_session_tmpdir.jar");

    // 首查触发建库：临时目录按句柄命名落位。
    ASSERT_FALSE(facadeSearch(s, makeSearchRequest("classes", "pysdk")).empty());
    const auto root = fs::temp_directory_path() /
                      ("mjt_pysdk_session_" + std::to_string(s.handle));
    EXPECT_TRUE(fs::exists(root));

    // 会话关闭：先关库连接再删目录（Windows 句柄纪律），目录即清。
    reg.close(s.handle);
    EXPECT_FALSE(fs::exists(root));
}

