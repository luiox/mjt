/// @file        cli_fixture.hpp
/// @brief       CLI 测试语料：运行时生成的 jar（jasm 汇编 class 字节）与
///              mapping 文本。语料名全部中性构造，只称「混淆 jar」语义的
///              坏条目/坏帧语料，不出现任何产品家族名。
#pragma once

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "jbc/jbc.hpp"
#include "jzip/zip_io.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/zip/checksum.hpp"

#include "shared/decompiler.hpp"
#include "shared/output_sink.hpp"

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

/// 写原始 zip（条目名原样落盘）。JarWriter 会丢弃尾斜杠条目名（目录条目
/// 语义），OS2.9 形 `.class/` 混淆语料必需绕开——同一 jzip 写面直写，
/// 全 STORED 条目，读回侧（listJarEntries / 容器导入）无感。
inline void writeRawZip(const std::filesystem::path& path,
                        const std::vector<std::pair<std::string, std::vector<ca::u8>>>& entries)
{
    jzip::ZipArchive archive;
    for (const auto& [name, data] : entries) {
        auto local = std::make_unique<jzip::LocalFileHeader>();
        ca::zip::Crc32 crcCalc;
        crcCalc.update(data.data(), data.size());
        local->setFileName(name);
        local->setCompressionMethod(0);   // STORED
        local->setCompressedSize(static_cast<ca::u32>(data.size()));
        local->setUncompressedSize(static_cast<ca::u32>(data.size()));
        local->setFileData(data);
        local->setCrc32(crcCalc.value());
        local->setGeneralPurposeBitFlag(0x0800);   // bit11 = UTF-8
        archive.addPart(std::move(local));
    }
    const auto bytes = jzip::ZipIO::writeDirect(archive);
    std::ofstream    out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) throw std::runtime_error("failed to write raw zip fixture: " + path.string());
}

/// OutputSink 捕获：临时文件 FILE* 读写往返（各命令测试的渲染断言共用）。
class CapturedSink
{
public:
    CapturedSink()
        : file_(std::tmpfile())
        , sink_(file_)
    {}
    ~CapturedSink()
    {
        if (file_ != nullptr) std::fclose(file_);
    }
    CapturedSink(const CapturedSink&)            = delete;
    CapturedSink& operator=(const CapturedSink&) = delete;

    //返回引用而非值：OutputSink 非 const 左值引用形参不能绑右值（MSVC 扩展
    //放行、g++ 拒绝——linux CI 实证）。
    mjt::OutputSink& sink() { return sink_; }

    std::string text()
    {
        std::fflush(file_);
        std::rewind(file_);
        std::string out;
        char        buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file_)) > 0) out.append(buffer, read);
        return out;
    }

private:
    std::FILE*      file_ = nullptr;
    mjt::OutputSink sink_;
};

// ── 类文件字节手术（disasm/subs 失败分支语料）────────────────────

/// u16 大端读取（语料改造专用，语料是测试前提，不做越界防护）。
inline ca::usize readU2(const std::vector<ca::u8>& bytes, std::size_t offset)
{
    return (static_cast<ca::usize>(bytes[offset]) << 8) | bytes[offset + 1];
}

/// u32 大端读取。
inline ca::u32 readU4(const std::vector<ca::u8>& bytes, std::size_t offset)
{
    return (static_cast<ca::u32>(bytes[offset]) << 24) |
           (static_cast<ca::u32>(bytes[offset + 1]) << 16) |
           (static_cast<ca::u32>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

/// 常量池单条目步进：返回 {下一偏移, 槽位步进}（long/double 占两槽）。
inline std::pair<std::size_t, ca::usize> cpEntryStep(const std::vector<ca::u8>& bytes,
                                                     std::size_t                offset)
{
    switch (bytes[offset]) {
    case 1: return {offset + 3 + readU2(bytes, offset + 1), 1};   // Utf8
    case 3:
    case 4: return {offset + 5, 1};   // Integer / Float
    case 5:
    case 6: return {offset + 9, 2};   // Long / Double（双槽）
    case 15: return {offset + 3, 1};  // MethodHandle（u1 kind + u2 index）
    case 7:
    case 8:
    case 16:
    case 19:
    case 20: return {offset + 3, 1};  // Class / String / MethodType / Module / Package
    default: return {offset + 5, 1};  // 9-12 引用族 / 13 NameAndType / 17-18 Dynamic 族
    }
}

/// 在属性表内按名字找首个属性载荷起点（utf8ByIndex 供 name_index 解析）；
/// 命中 Code 时下钻其嵌套属性表。未命中返回 false。
inline bool findAttributePayload(const std::vector<ca::u8>& bytes,
                                 const std::map<ca::usize, std::string>& utf8ByIndex,
                                 std::size_t tableOffset, std::string_view name,
                                 std::size_t& payloadOffset)
{
    const ca::usize count    = readU2(bytes, tableOffset);
    std::size_t     offset   = tableOffset + 2;
    for (ca::usize index = 0; index < count; ++index) {
        const ca::usize nameIndex = readU2(bytes, offset);
        const ca::u32   length    = readU4(bytes, offset + 2);
        const auto      it        = utf8ByIndex.find(nameIndex);
        if (it != utf8ByIndex.end() && it->second == name) {
            payloadOffset = offset + 6;
            return true;
        }
        if (it != utf8ByIndex.end() && it->second == "Code") {
            // Code 载荷：u2 max_stack + u2 max_locals + u4 code_length + code +
            // u2 exception_table_length + 8*n + u2 嵌套属性表计数。
            const std::size_t payload    = offset + 6;
            const ca::u32     codeLength = readU4(bytes, payload + 4);
            const std::size_t exTable    = payload + 8 + codeLength;
            const std::size_t nested     = exTable + 2 + readU2(bytes, exTable) * 8;
            if (findAttributePayload(bytes, utf8ByIndex, nested, name, payloadOffset))
                return true;
        }
        offset += 6 + length;
    }
    return false;
}

/// 在成员表（字段或方法）内按名搜索属性载荷：命中时写 payloadOffset。
/// 无论命中与否都走完整张表并返回表尾偏移——fields / methods / 类级
/// 三张表要顺序消费，前两张的表尾就是下一张表的起点。
inline std::pair<std::size_t, bool> findMemberAttributePayload(
    const std::vector<ca::u8>& bytes, const std::map<ca::usize, std::string>& utf8ByIndex,
    std::size_t tableOffset, std::string_view name, std::size_t& payloadOffset)
{
    const ca::usize count  = readU2(bytes, tableOffset);
    std::size_t     cursor = tableOffset + 2;
    bool            found  = false;
    for (ca::usize index = 0; index < count; ++index) {
        cursor += 6;   // access_flags / name_index / descriptor_index
        if (findAttributePayload(bytes, utf8ByIndex, cursor, name, payloadOffset)) found = true;
        const ca::usize attrs = readU2(bytes, cursor);
        cursor += 2;
        for (ca::usize attr = 0; attr < attrs; ++attr) cursor += 6 + readU4(bytes, cursor + 2);
    }
    return {cursor, found};
}

/// 把类字节内首个命中名字的属性载荷头两字节改写为 0xFFFF（u2 行数计数
/// 越界，解码必失败）。disasm 失败分支语料专用：workspace 侧对 LNT 解码
/// 失败容错跳过（classInfo 照常成功），jasm 反汇编按名强制解码即抛。
inline std::vector<ca::u8> withCorruptAttribute(const std::vector<ca::u8>& classBytes,
                                                std::string_view           name)
{
    std::map<ca::usize, std::string> utf8ByIndex;
    std::size_t                      offset  = 8;
    const ca::usize                  cpCount = readU2(classBytes, offset);
    offset += 2;
    for (ca::usize slot = 1; slot < cpCount;) {
        const auto [next, advance] = cpEntryStep(classBytes, offset);
        if (classBytes[offset] == 1) {
            const ca::usize length = readU2(classBytes, offset + 1);
            utf8ByIndex.emplace(slot, std::string(reinterpret_cast<const char*>(
                                                          classBytes.data() + offset + 3),
                                                      length));
        }
        offset = next;
        slot += advance;
    }
    // access + this + super 之后是接口表，再 fields / methods / 类级属性表；
    // 方法属性（LNT 等）藏在 methods 表的 Code 嵌套里，三张表顺序搜。
    const std::size_t interfaces = readU2(classBytes, offset + 6);
    std::size_t       cursor     = offset + 8 + interfaces * 2;
    std::size_t       payload    = 0;
    bool              found      = false;
    auto [afterFields, fieldsHit] =
        findMemberAttributePayload(classBytes, utf8ByIndex, cursor, name, payload);
    cursor = afterFields;
    found  = found || fieldsHit;
    auto [afterMethods, methodsHit] =
        findMemberAttributePayload(classBytes, utf8ByIndex, cursor, name, payload);
    cursor = afterMethods;
    found  = found || methodsHit;
    if (!found) found = findAttributePayload(classBytes, utf8ByIndex, cursor, name, payload);
    if (!found) throw std::runtime_error("fixture attribute not found: " + std::string(name));
    std::vector<ca::u8> patched = classBytes;
    patched[payload]            = 0xFF;
    patched[payload + 1]        = 0xFF;
    return patched;
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
    R"JASM(.super java/lang/Object
.class public super fixture/Alpha {
  .field private state I
  .method public static act (I)V {
    code: {
      return
    }
  }
}
)JASM";

const std::string kApiSource =
    R"JASM(.class public abstract interface fixture/Api {
  .method public abstract act (I)V {}
}
)JASM";

const std::string kColorSource =
    R"JASM(.super java/lang/Enum
.class public final enum fixture/Color {
  .method public static values ()[Lfixture/Color; {
    code: {
      aconst_null
      areturn
    }
  }
}
)JASM";

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
        R"JASM(.super java/lang/Object
.class public super fixture/Words {
  .method public static first ()Ljava/lang/String; {
    code: {
      ldc "hello world"
      areturn
    }
  }
  .method public static second ()Ljava/lang/String; {
    code: {
      ldc "zeta_payload_xyz"
      areturn
    }
  }
}
)JASM");
}

/// strings 语料源 B：复用 "hello world" + 一个长密文形字面量（--min-len 观察点）。
inline std::vector<ca::u8> assembleEchoClass()
{
    return assembleClass(
        R"JASM(.super java/lang/Object
.class public super fixture/Echo {
  .method public static echo ()Ljava/lang/String; {
    code: {
      ldc "hello world"
      areturn
    }
  }
  .method public static blob ()Ljava/lang/String; {
    code: {
      ldc "ENCRYPTED_BLOB_0123456789abcdef"
      areturn
    }
  }
}
)JASM");
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
        R"JASM(.super java/lang/Object
.class public super fixture/Node {
  .field public static flag Z
  .method public static run ()V {
    code: {
      invokestatic fixture/Node.run ()V
      return
    }
  }
}
)JASM");
}

/// xref 语料源 Caller：call 发出方法/字段引用，pick 发出 checkcast 类型引用。
inline std::vector<ca::u8> assembleCallerClass()
{
    return assembleClass(
        R"JASM(.super java/lang/Object
.class public super fixture/Caller {
  .method public static call ()V {
    code: {
      invokestatic fixture/Node.run ()V
      getstatic fixture/Node.flag Z
      pop
      return
    }
  }
  .method public static pick (Ljava/lang/Object;)Ljava/lang/Object; {
    code: {
      aload_0
      checkcast fixture/Node
      areturn
    }
  }
}
)JASM");
}

/// xref 语料源 NodeImpl：super 头声明命中 Node（类头类型引用观察点）。
inline std::vector<ca::u8> assembleNodeImplClass()
{
    return assembleClass(
        R"JASM(.super fixture/Node
.class public super fixture/NodeImpl {
}
)JASM");
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
        R"JASM(.super java/lang/Object
.class public super fixture/Native {
  .field private token J
  .method public native probe (I)V {}
  .method public static calc (II)I {
    code: {
      iload_0
      iload_1
      iadd
      ireturn
    }
  }
}
)JASM");
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
        R"JASM(.super java/lang/Object
.class public super fixture/Alpha {
  .field private state I
  .method public static act (I)I {
    code: {
      iload_0
      iconst_2
      imul
      ireturn
    }
  }
}
)JASM";
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
                              assembleClass(R"JASM(.class public abstract interface annotation fixture/Anno {
  .method public abstract value ()I {}
}
)JASM")));
    container.addResource("fixture/Pair.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Pair.class",
                              assembleClass(R"JASM(.super java/lang/Record
.class public final record fixture/Pair {
  .field private final a I
}
)JASM")));
    container.addResource("fixture/Native.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "fixture/Native.class", assembleNativeHostClass()));
    container.addResource("com/google/gson/Stub.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "com/google/gson/Stub.class",
                              assembleClass(R"JASM(.super java/lang/Object
.class public super com/google/gson/Stub {
}
)JASM")));
    container.addResource("sun/misc/Stub.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "sun/misc/Stub.class",
                              assembleClass(R"JASM(.super java/lang/Object
.class public super sun/misc/Stub {
}
)JASM")));
    container.addResource("module-info.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "module-info.class",
                              assembleClass(R"JASM(.class module module-info {
}
)JASM")));
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
        R"JASM(.super java/lang/Object
.class public super fixture/Template {
  .field public counter I
  .method public static act (I)I {
    code: {
      iload_0
      iconst_1
      iadd
      ireturn
    }
  }
}
)JASM";
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
        R"JASM(.super java/lang/Object
.class public super fixture/Alpha {
  .field private partner Lfixture/Api;
  .method public static act (I)I {
    code: {
      iload_0
      iconst_2
      imul
      ireturn
    }
  }
}
)JASM";
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
