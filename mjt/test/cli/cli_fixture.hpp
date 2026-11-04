/// @file        cli_fixture.hpp
/// @brief       CLI 测试语料：运行时生成的 jar（jasm 汇编 class 字节）与
///              mapping 文本。语料名全部中性构造，只称「混淆 jar」语义的
///              坏条目/坏帧语料，不出现任何产品家族名。
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "jbc/jbc.hpp"
#include "libca/str/utf8_string.hpp"

#include "shared/decompiler.hpp"

#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/jar/jar_writer.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "test_helper.hpp"

namespace cli_fixture {

/// UTF-8 文本 → UTF-16（jasm 源入口口径）。
inline ca::str::Utf16String utf16(const std::string& value)
{
    const auto ref = ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size());
    return ca::str::Utf16String::from_utf8_string(ref);
}

/// jasm 汇编单段源；失败抛出（语料是测试前提，失败即测试自身错误）。
inline std::vector<ca::u8> assembleClass(const std::string& source)
{
    mjbase::jasm::JvmJasmService assembler;
    const auto assembled = assembler.assemble(utf16("fixture.jasm").ref(), utf16(source).ref());
    if (!assembled.isOk()) throw std::runtime_error("fixture jasm did not assemble");
    return assembled.value->bytes;
}

/// 写一个小文本文件到测试输出目录（mapping 语料等）。
inline std::filesystem::path writeTextFile(const std::string& name, const std::string& content)
{
    const auto path = test::getTempOutPath(name);
    std::ofstream out(path, std::ios::binary);
    out << content;
    out.close();
    if (!out) throw std::runtime_error("failed to write fixture file: " + name);
    return path;
}

namespace {

const std::string kAlphaSource =
    ".super java/lang/Object\n"
    ".class public super fixture/Alpha {\n"
    "  .field private state I\n"
    "  .method public static act (I)V {\n"
    "    code: {\n"
    "      return\n"
    "    }\n"
    "  }\n"
    "}\n";

const std::string kApiSource =
    ".class public abstract interface fixture/Api {\n"
    "  .method public abstract act (I)V {}\n"
    "}\n";

const std::string kColorSource =
    ".super java/lang/Enum\n"
    ".class public final enum fixture/Color {\n"
    "  .method public static values ()[Lfixture/Color; {\n"
    "    code: {\n"
    "      aconst_null\n"
    "      areturn\n"
    "    }\n"
    "  }\n"
    "}\n";

}   // namespace

/// info 语料 jar：3 个 class（class/interface/enum 形态）+ 1 个常规资源 +
/// 1 个空段路径资源，无 manifest。条目数 = 5。
inline std::filesystem::path writeInfoJar()
{
    mjbase::model::ResourceContainer container;
    container.addResource("fixture/Alpha.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Alpha.class", assembleClass(kAlphaSource)));
    container.addResource("fixture/Api.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Api.class", assembleClass(kApiSource)));
    container.addResource("fixture/Color.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Color.class", assembleClass(kColorSource)));
    container.addResource("notes/data.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "notes/data.txt",
                              std::vector<ca::u8>{'f', 'i', 'x', 't', 'u', 'r', 'e'}));
    container.addResource("bad//name.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "bad//name.txt", std::vector<ca::u8>{'x'}));

    const auto path = test::getTempOutPath("mjt_cli_info_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

/// info 语料 jar 的 manifest 变体：在 info 语料之上补 META-INF/MANIFEST.MF。
inline std::filesystem::path writeInfoJarWithManifest()
{
    mjbase::model::ResourceContainer container;
    container.addResource("fixture/Alpha.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Alpha.class", assembleClass(kAlphaSource)));
    container.addResource(
        "META-INF/MANIFEST.MF",
        std::make_unique<mjbase::model::ManifestResource>(
            mjbase::model::ManifestResource::fromBytes(
                std::vector<ca::u8>{'M', 'a', 'n', 'i', 'f', 'e', 's', 't', '-', 'V', 'e', 'r',
                                    's', 'i', 'o', 'n', ':', ' ', '1', '.', '0', '\n', '\n'})));

    const auto path = test::getTempOutPath("mjt_cli_info_manifest_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

/// jar fix 语料：1 个合法 class + 1 个坏字节 .class 条目 + 1 个损坏输出
/// 标记资源 + 1 个常规资源。修复后应只剩合法 class 与常规资源。
inline std::filesystem::path writeBrokenJar()
{
    mjbase::model::ResourceContainer container;
    container.addResource("fixture/Alpha.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Alpha.class", assembleClass(kAlphaSource)));
    // 非 class 字节伪装的 .class 条目（坏字节语料）。
    container.addResource("fixture/Broken.class",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "fixture/Broken.class",
                              std::vector<ca::u8>{'n', 'o', 't', '-', 'a', '-', 'c', 'l', 'a',
                                                  's', 's'}));
    // 损坏输出标记资源（名字命中 workspace 扫描器的已知标记）。
    container.addResource("corrupt_output_stream.dat",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "corrupt_output_stream.dat", std::vector<ca::u8>{0x00, 0x01}));
    container.addResource("data/notes.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "data/notes.txt", std::vector<ca::u8>{'o', 'k'}));

    const auto path = test::getTempOutPath("mjt_cli_fix_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

/// mapping 三族互转语料（SRG 形态；一类一字段一方法，desc 全带，proguard
/// 往返无损的前提）。
inline const char* srgMappingText()
{
    return "CL: fixture/Alpha fixture/RenamedAlpha\n"
           "FD: fixture/Alpha/state I fixture/RenamedAlpha/renamedState I\n"
           "MD: fixture/Alpha/act (I)V fixture/RenamedAlpha/doAct (I)V\n";
}

/// 读取文件全部文本（binary 模式）。
inline std::string readFileText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

/// 打开 jar 统计条目（测试断言输出 jar 内容用）。
inline std::vector<std::string> jarEntryUris(const std::filesystem::path& jar)
{
    mjbase::model::ResourceContainer container;
    const auto imported =
        mjbase::model::ResourceHelper::importFromJar(container, jar.string());
    if (imported.is_err()) throw std::runtime_error("failed to reimport fixture jar");
    std::vector<std::string> uris;
    container.visitResources([&uris](std::string_view uri, const mjbase::model::IResource&) {
        uris.emplace_back(uri);
    });
    std::sort(uris.begin(), uris.end());
    return uris;
}

// ── strings 语料（批三：常量池字符串抽取）───────────────────────────

/// strings 语料源 A：两个字面量（一个跨类复用，一个本类独有）。
inline std::vector<ca::u8> assembleWordsClass()
{
    return assembleClass(
        ".super java/lang/Object\n"
        ".class public super fixture/Words {\n"
        "  .method public static first ()Ljava/lang/String; {\n"
        "    code: {\n"
        "      ldc \"hello world\"\n"
        "      areturn\n"
        "    }\n"
        "  }\n"
        "  .method public static second ()Ljava/lang/String; {\n"
        "    code: {\n"
        "      ldc \"zeta_payload_xyz\"\n"
        "      areturn\n"
        "    }\n"
        "  }\n"
        "}\n");
}

/// strings 语料源 B：复用 "hello world" + 一个长密文形字面量（--min-len 观察点）。
inline std::vector<ca::u8> assembleEchoClass()
{
    return assembleClass(
        ".super java/lang/Object\n"
        ".class public super fixture/Echo {\n"
        "  .method public static echo ()Ljava/lang/String; {\n"
        "    code: {\n"
        "      ldc \"hello world\"\n"
        "      areturn\n"
        "    }\n"
        "  }\n"
        "  .method public static blob ()Ljava/lang/String; {\n"
        "    code: {\n"
        "      ldc \"ENCRYPTED_BLOB_0123456789abcdef\"\n"
        "      areturn\n"
        "    }\n"
        "  }\n"
        "}\n");
}

/// strings 语料 jar：Words + Echo 两类（无资源条目）。
inline std::filesystem::path writeStringsJar()
{
    mjbase::model::ResourceContainer container;
    container.addResource("fixture/Words.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Words.class", assembleWordsClass()));
    container.addResource("fixture/Echo.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Echo.class", assembleEchoClass()));
    const auto path = test::getTempOutPath("mjt_cli_strings_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

// ── xref 语料（批三：已知引用边）────────────────────────────────────

/// xref 语料源 Node：被反查目标（静态字段 + 递归自引用方法）。
inline std::vector<ca::u8> assembleNodeClass()
{
    return assembleClass(
        ".super java/lang/Object\n"
        ".class public super fixture/Node {\n"
        "  .field public static flag Z\n"
        "  .method public static run ()V {\n"
        "    code: {\n"
        "      invokestatic fixture/Node.run ()V\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "}\n");
}

/// xref 语料源 Caller：call 发出方法/字段引用，pick 发出 checkcast 类型引用。
inline std::vector<ca::u8> assembleCallerClass()
{
    return assembleClass(
        ".super java/lang/Object\n"
        ".class public super fixture/Caller {\n"
        "  .method public static call ()V {\n"
        "    code: {\n"
        "      invokestatic fixture/Node.run ()V\n"
        "      getstatic fixture/Node.flag Z\n"
        "      pop\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public static pick (Ljava/lang/Object;)Ljava/lang/Object; {\n"
        "    code: {\n"
        "      aload_0\n"
        "      checkcast fixture/Node\n"
        "      areturn\n"
        "    }\n"
        "  }\n"
        "}\n");
}

/// xref 语料源 NodeImpl：super 头声明命中 Node（类头类型引用观察点）。
inline std::vector<ca::u8> assembleNodeImplClass()
{
    return assembleClass(
        ".super fixture/Node\n"
        ".class public super fixture/NodeImpl {\n"
        "}\n");
}

/// xref 语料 jar：Node（目标）/ Caller（调用方）/ NodeImpl（子类）三类。
inline std::filesystem::path writeXrefJar()
{
    mjbase::model::ResourceContainer container;
    container.addResource("fixture/Node.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Node.class", assembleNodeClass()));
    container.addResource("fixture/Caller.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Caller.class", assembleCallerClass()));
    container.addResource("fixture/NodeImpl.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/NodeImpl.class", assembleNodeImplClass()));
    const auto path = test::getTempOutPath("mjt_cli_xref_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

// ── classes / disasm / subs 语料（批四：类清单 / 单类 jasm / 成员清单）───

/// classes 语料源 Native：native 方法（subs native 标记观察点）+ 常规方法。
inline std::vector<ca::u8> assembleNativeHostClass()
{
    return assembleClass(
        ".super java/lang/Object\n"
        ".class public super fixture/Native {\n"
        "  .field private token J\n"
        "  .method public native probe (I)V {}\n"
        "  .method public static calc (II)I {\n"
        "    code: {\n"
        "      iload_0\n"
        "      iload_1\n"
        "      iadd\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n");
}

/// classes/subs/disasm 语料 jar：九类覆盖六形态（class / interface / enum /
/// record / annotation / module）与三来源（jdk = sun/misc、known =
/// com/google/gson、unknown = fixture/ + module-info），外加一个常规资源。
/// fixture/Native 兼作 subs 的 native 观察点；fixture/Alpha（字段 + 常量
/// 运算方法体）兼作 disasm 的指令形状观察点。
inline std::filesystem::path writeClassesJar()
{
    mjbase::model::ResourceContainer container;
    const std::string alphaSource =
        ".super java/lang/Object\n"
        ".class public super fixture/Alpha {\n"
        "  .field private state I\n"
        "  .method public static act (I)I {\n"
        "    code: {\n"
        "      iload_0\n"
        "      iconst_2\n"
        "      imul\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
    container.addResource("fixture/Alpha.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Alpha.class", assembleClass(alphaSource)));
    container.addResource("fixture/Api.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Api.class", assembleClass(kApiSource)));
    container.addResource("fixture/Color.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Color.class", assembleClass(kColorSource)));
    container.addResource("fixture/Anno.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Anno.class",
                              assembleClass(".class public abstract interface annotation "
                                            "fixture/Anno {\n"
                                            "  .method public abstract value ()I {}\n"
                                            "}\n")));
    container.addResource("fixture/Pair.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Pair.class",
                              assembleClass(".super java/lang/Record\n"
                                            ".class public final record fixture/Pair {\n"
                                            "  .field private final a I\n"
                                            "}\n")));
    container.addResource("fixture/Native.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Native.class", assembleNativeHostClass()));
    container.addResource("com/google/gson/Stub.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "com/google/gson/Stub.class",
                              assembleClass(".super java/lang/Object\n"
                                            ".class public super com/google/gson/Stub {\n"
                                            "}\n")));
    container.addResource("sun/misc/Stub.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "sun/misc/Stub.class",
                              assembleClass(".super java/lang/Object\n"
                                            ".class public super sun/misc/Stub {\n"
                                            "}\n")));
    container.addResource("module-info.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "module-info.class",
                              assembleClass(".class module module-info {\n"
                                            "}\n")));
    container.addResource("data/notes.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "data/notes.txt", std::vector<ca::u8>{'o', 'k'}));
    const auto path = test::getTempOutPath("mjt_cli_classes_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

// ── decompile 语料（#63 敌意名 + perf 大 jar）───────────────────────

/// 敌意类名长度（单段 UTF-16 单元数）：Windows 单段文件名上限的 ~85 倍，
/// 落盘必然失败（#63 样本形态：单段全空格 + ".class"）。
inline constexpr std::size_t kHostileNameLength = 21845;

/// 敌意类内部名：kHostileNameLength 个空格（合法 JVM 名——不含 '/' 与 '.'）。
inline std::string hostileInternalName() { return std::string(kHostileNameLength, ' '); }

/// 模板类（jasm 汇编一次，改名产多类）。方法体带常量加载 + 返回（有内容）。
inline std::vector<ca::u8> assembleRenamedClass(const std::string& internalName)
{
    const std::string source =
        ".super java/lang/Object\n"
        ".class public super fixture/Template {\n"
        "  .field public counter I\n"
        "  .method public static act (I)I {\n"
        "    code: {\n"
        "      iload_0\n"
        "      iconst_1\n"
        "      iadd\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
    const auto templateBytes = assembleClass(source);
    //this_class 改写（libjbc 会话面；模板方法体零触碰，与 MCP fixture 同款）。
    auto opened = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(templateBytes));
    if (!opened.is_ok()) throw std::runtime_error("fixture jbc session failed");
    auto session = std::move(opened).unwrap();
    jbc::ir::ClassUnit& unit = session->unit();
    unit.thisClass = unit.symbols().intern(internalName);
    if (!session->commitClassMetadata().is_ok())
        throw std::runtime_error("fixture class rename failed");
    auto written = jbc::core::writeClass(session->backing());
    if (!written.is_ok()) throw std::runtime_error("fixture class write failed");
    return std::move(written).unwrap();
}

/// decompile 单类语料 jar：fixture/Alpha（有字段有方法体）+ fixture/Api 接口 +
/// 1 个常规资源。Alpha 引用 Api（跨类型上下文/classpath 语义的观察点）。
inline std::filesystem::path writeDecompileJar()
{
    mjbase::model::ResourceContainer container;
    const std::string alphaSource =
        ".super java/lang/Object\n"
        ".class public super fixture/Alpha {\n"
        "  .field private partner Lfixture/Api;\n"
        "  .method public static act (I)I {\n"
        "    code: {\n"
        "      iload_0\n"
        "      iconst_2\n"
        "      imul\n"
        "      ireturn\n"
        "    }\n"
        "  }\n"
        "}\n";
    container.addResource("fixture/Alpha.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Alpha.class", assembleClass(alphaSource)));
    container.addResource("fixture/Api.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Api.class", assembleClass(kApiSource)));
    container.addResource("data/notes.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "data/notes.txt", std::vector<ca::u8>{'o', 'k'}));
    const auto path = test::getTempOutPath("mjt_cli_decompile_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

/// #63 敌意名语料 jar：正常类 fixture/Alpha + 敌意名类（kHostileNameLength
/// 个空格 + ".class" 条目，内部名同为该空格名）。条目经 JarWriter 原始字节
/// 直写（不经模型层校验——敌意条目必须原样存活到 workspace/后端边界）。
inline std::filesystem::path writeHostileNameJar()
{
    const auto alphaBytes = assembleRenamedClass("fixture/Alpha");
    const auto hostileBytes = assembleRenamedClass(hostileInternalName());
    const auto path = test::getTempOutPath("mjt_cli_decompile_hostile_fixture.jar");
    mjbase::jar::JarWriter writer(
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(path.string().data()),
                                          path.string().size()));
    auto written = writer.write([&](mjbase::jar::IEntryWriter& entries) {
        const std::string alphaEntry = "fixture/Alpha.class";
        entries.writeEntry(
            ca::str::Utf8StringRef::from_data(
                reinterpret_cast<const ca::u8*>(alphaEntry.data()), alphaEntry.size()),
            alphaBytes);
        const std::string hostileEntry = hostileInternalName() + ".class";
        entries.writeEntry(
            ca::str::Utf8StringRef::from_data(
                reinterpret_cast<const ca::u8*>(hostileEntry.data()), hostileEntry.size()),
            hostileBytes);
        const std::string notes = "data/notes.txt";
        entries.writeEntry(
            ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(notes.data()),
                                              notes.size()),
            std::vector<ca::u8>{'o', 'k'});
    });
    if (written.is_err()) throw std::runtime_error("failed to write hostile fixture jar");
    return path;
}

/// perf 语料 jar：count 个有内容方法体的类（模板一次汇编，逐类改名），
/// 计数含对照资源条目。生成走模型层（ClassResource + exportToJar）。
inline std::filesystem::path writePerfClassListJar(ca::usize count)
{
    mjbase::model::ResourceContainer container;
    for (ca::usize index = 0; index < count; ++index) {
        const auto internalName = "perf/Subject" + std::to_string(index);
        const auto uri          = internalName + ".class";
        container.addResource(uri,
                              std::make_unique<mjbase::model::ClassResource>(
                                  uri, assembleRenamedClass(internalName)));
    }
    container.addResource("data/notes.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "data/notes.txt", std::vector<ca::u8>{'o', 'k'}));
    const auto path = test::getTempOutPath("mjt_cli_perf_class_list.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

/// perf mapping 语料（SRG 形态）：count 组 CL/FD/MD 行。
inline std::string writePerfSrgMapping(ca::usize count)
{
    std::string text;
    text.reserve(count * 128);
    for (ca::usize index = 0; index < count; ++index) {
        const auto name = "perf/Subject" + std::to_string(index);
        text += "CL: " + name + " perf/Renamed" + std::to_string(index) + "\n";
        text += "FD: " + name + "/counter I perf/Renamed" + std::to_string(index) +
                "/renamedCounter I\n";
        text += "MD: " + name + "/act (I)I perf/Renamed" + std::to_string(index) +
                "/doAct (I)I\n";
    }
    return text;
}

/// 后端可用性门（真后端执行用例的 SKIP 条款）：decompiler JAR 与 java 均按
/// 与产品相同的发现序解析；env（MJT_CFR_JAR / MJT_VINEFLOWER_JAR）可在本机
/// 注入真 jar，CI 无注入时整组用例跳过（与 probeClang 先例同口径）。
struct BackendProbe
{
    bool        available = false;
    std::string decompilerJar;
    std::string reason;
};

inline BackendProbe probeBackend(mjt::DecompilerKind kind)
{
    BackendProbe probe;
    const auto inspect = mjt::inspectDecompiler(kind);
    if (!inspect.artifactAvailable) probe.reason = "decompiler jar not found (set MJT_CFR_JAR / MJT_VINEFLOWER_JAR)";
    else if (!inspect.javaAvailable) probe.reason = "java executable not found";
    else {
        probe.available      = true;
        probe.decompilerJar  = inspect.decompilerJar;
    }
    return probe;
}

}   // namespace cli_fixture
