#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <libca/http/client.hpp>
#include <libca/http/url.hpp>

#include <jbc/jbc.hpp>
#include <mcp/json_rpc.hpp>

#include "mcp_test_support.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "tool/json.hpp"
#include "test_helper.hpp"

namespace mjt::mcp::test_support {

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string encodeBase64(const std::vector<ca::u8>& value)
{
    //复用生产侧 base64，避免与 json.cpp 的实现逐字重复（M2 技术债）。
    return mjt::mcp::detail::base64(value);
}

std::string classSourceFor(const std::string& name, const std::string& value)
{
    // jasm 文本 raw string 定版（用户定 fixture 规范，同 mjbase
    // doc/test-fixtures.rst）：大段 R"JASM 定版，仅插值点最小段拼接，
    // 禁逐行 ".x\n" 拼接与转义引号。拼接顺序与历史模板逐字节一致。
    return std::string{R"JASM(.super java/lang/Object
.class public super )JASM"} +
           name +
           R"JASM( {
  .field public counter I
  .method public static value ()I {
    code: {
      bipush )JASM" +
           value +
           R"JASM(
      ireturn
    }
  }
}
)JASM";
}

std::string classSource(const std::string& value)
{
    return classSourceFor("mcp/Subject", value);
}

/// 类名 + 类级签名改写（RFC 0002 迁移批 B5：tree 面 → libjbc 会话面）。
/// 只动 this_class 与 Signature 属性，方法体零触碰——帧/栈沿用装配器产物
///（fixture 模板无分支无 handler，本就不产 StackMapTable）；写回走
/// copy-through，未触及的属性逐字节保真，不再整类重序列化。
std::vector<ca::u8> withClassNameAndSignature(const std::vector<ca::u8>& bytes,
                                              const std::string&         internalName,
                                              const std::string&         signature)
{
    auto opened = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (!opened.is_ok()) throw std::runtime_error("fixture session failed");
    // shared_ptr 按值接住（寿命接管根）：引用临时 unwrap 值会悬垂。
    auto session = std::move(opened).unwrap();

    jbc::ir::ClassUnit& unit = session->unit();
    unit.thisClass = unit.symbols().intern(internalName);
    if (!session->commitClassMetadata().is_ok())
        throw std::runtime_error("fixture class rename failed: " + internalName);

    // 旧 tree 写面口径：空签名 = 不落 Signature 属性（writer 按串长判空跳过）。
    if (!signature.empty()) {
        auto body          = std::make_unique<jbc::core::SignatureAttr>();
        body->signatureCp  = session->internUtf8(signature);
        const auto mounted = session->attachClassAttr("Signature", std::move(body));
        if (!mounted.is_ok()) throw std::runtime_error("fixture signature mount failed");
    }

    auto written = jbc::core::writeClass(session->backing());
    if (!written.is_ok()) throw std::runtime_error("fixture write failed");
    return std::move(written).unwrap();
}

std::filesystem::path writeLargeClassListFixture(const std::string& signature)
{
    mjbase::jasm::JvmJasmService assembler;
    const auto                 sourceName = utf16("LargeClassTemplate.jasm");
    const auto                 source     = utf16(classSourceFor("mcp/large/Template", "1"));
    const auto                 assembled  = assembler.assemble(sourceName.ref(), source.ref());
    if (!assembled.isOk()) throw std::runtime_error("large MCP fixture did not assemble");

    mjbase::model::ResourceContainer container;
    for (ca::usize index = 0; index < 210; ++index) {
        const auto internalName = "mcp/large/Subject" + std::to_string(index);
        const auto uri          = internalName + ".class";
        container.addResource(
            uri,
            std::make_unique<mjbase::model::ClassResource>(
                uri, withClassNameAndSignature(assembled.value->bytes, internalName, signature)));
    }
    const auto path = test::getTempOutPath("mjt_mcp_large_class_list_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

/// 给指定方法挂 Signature 属性（tree attachAttr 面；其余字节 copy-through
/// 保真）——members.signature 面（search kind=members 的 signature 命中）
/// 端到端 fixture 用。
std::vector<ca::u8> withMethodSignature(const std::vector<ca::u8>& bytes,
                                        const std::string&         methodName,
                                        const std::string&         methodDesc,
                                        const std::string&         signature)
{
    auto opened = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (!opened.is_ok()) throw std::runtime_error("fixture session failed");
    auto session = std::move(opened).unwrap();

    const auto methodId = session->unit().findMethodId(methodName, methodDesc);
    if (methodId == jbc::ir::kNoMethodId)
        throw std::runtime_error("fixture method not found: " + methodName);
    auto body         = std::make_unique<jbc::core::SignatureAttr>();
    body->signatureCp = session->internUtf8(signature);
    if (!session->attachAttr(methodId, "Signature", std::move(body)).is_ok())
        throw std::runtime_error("fixture method signature mount failed");

    auto written = jbc::core::writeClass(session->backing());
    if (!written.is_ok()) throw std::runtime_error("fixture write failed");
    return std::move(written).unwrap();
}

/// Subject.value 方法带泛型 Signature 的 fixture——search kind=members
/// signature 面端到端（Second 无签名对照，二类其余成员同 writeFixture）。
std::filesystem::path writeSignaturedMemberFixture()
{
    mjbase::jasm::JvmJasmService assembler;
    auto                       sourceName = utf16("Subject.jasm");
    auto                       source     = utf16(classSource("1"));
    const auto                 assembled  = assembler.assemble(sourceName.ref(), source.ref());
    if (!assembled.isOk()) throw std::runtime_error("signatured member fixture did not assemble");

    mjbase::model::ResourceContainer container;
    container.addResource(
        "mcp/Subject.class",
        std::make_unique<mjbase::model::ClassResource>(
            "mcp/Subject.class",
            withMethodSignature(assembled.value->bytes, "value", "()I",
                                "(Ljava/util/List<Ljava/lang/String;>;)I")));
    auto secondName   = utf16("Second.jasm");
    auto secondSource = utf16(classSourceFor("mcp/Second", "2"));
    const auto secondAssembled =
        assembler.assemble(secondName.ref(), secondSource.ref());
    if (!secondAssembled.isOk())
        throw std::runtime_error("signatured member second fixture did not assemble");
    container.addResource("mcp/Second.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "mcp/Second.class", secondAssembled.value->bytes));
    const auto path = test::getTempOutPath("mjt_mcp_signatured_member_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path writeFixture()
{
    mjbase::jasm::JvmJasmService assembler;
    auto                       sourceName = utf16("Subject.jasm");
    auto                       source = utf16(classSource("1"));
    const auto assembled = assembler.assemble(sourceName.ref(), source.ref());
    if (!assembled.isOk()) throw std::runtime_error("MCP fixture did not assemble");

    mjbase::model::ResourceContainer container;
    container.addResource("mcp/Subject.class",
                          std::make_unique<mjbase::model::ClassResource>("mcp/Subject.class",
                                                                assembled.value->bytes));
    auto secondName = utf16("Second.jasm");
    auto secondSource = utf16(classSourceFor("mcp/Second", "2"));
    const auto secondAssembled = assembler.assemble(secondName.ref(), secondSource.ref());
    if (!secondAssembled.isOk()) throw std::runtime_error("MCP second fixture did not assemble");
    container.addResource("mcp/Second.class",
                          std::make_unique<mjbase::model::ClassResource>("mcp/Second.class",
                                                                secondAssembled.value->bytes));
    container.addResource("notice.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "notice.txt", std::vector<ca::u8>{'m', 'c', 'p', '-', 'f', 'i', 'x', 't', 'u', 'r', 'e'}));
    container.addResource("broken/Invalid.class",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "broken/Invalid.class", std::vector<ca::u8>{0xCA, 0xFE, 0xBA}));
    const auto path = test::getTempOutPath("mjt_mcp_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path writeQuerySupportingFixture()
{
    const auto source = R"JASM(.super mcp/Subject
.class public super support/McpLibrary {
  .method public static call ()I {
    code: {
      invokestatic mcp/Subject.value ()I
      ireturn
    }
  }
}
)JASM";
    mjbase::jasm::JvmJasmService assembler;
    auto sourceName = utf16("support/McpLibrary.jasm");
    auto sourceText = utf16(source);
    const auto assembled = assembler.assemble(sourceName.ref(), sourceText.ref());
    if (!assembled.isOk()) throw std::runtime_error("MCP supporting fixture did not assemble");
    mjbase::model::ResourceContainer container;
    container.addResource(
        "support/McpLibrary.class",
        std::make_unique<mjbase::model::ClassResource>(
            "support/McpLibrary.class",
            assembled.value->bytes));
    const auto path = test::getTempOutPath("mjt_mcp_query_support.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::vector<ca::u8> assembleFixtureClass(const std::string& sourceName, const std::string& source)
{
    mjbase::jasm::JvmJasmService assembler;
    auto                       name = utf16(sourceName + ".jasm");
    auto                       text = utf16(source);
    const auto assembled = assembler.assemble(name.ref(), text.ref());
    if (!assembled.isOk()) throw std::runtime_error("MCP parity fixture did not assemble: " + sourceName);
    return assembled.value->bytes;
}

std::string annotationSource()
{
    return R"JASM(.visible-annotation mcp/parity/TestAnnotation {
  number: 15,
  nested: .annotation mcp/parity/NestedAnnotation { value: "fixture" }
}
.invisible-annotation mcp/parity/TestAnnotation { value: "hidden" }
.super java/lang/Object
.class public super mcp/parity/Annotated {
  .method public static value ()I {
    code: {
      bipush 7
      ireturn
    }
  }
}
)JASM";
}

std::string invokeDynamicSource()
{
    return R"JASM(.super java/lang/Object
.class public super mcp/parity/Indy {
  .method public static run ()V {
    code: {
      new java/util/ArrayList
      dup
      invokespecial java/util/ArrayList.<init> ()V
      astore values
      aload values
      invokedynamic accept ()Ljava/util/function/Consumer; LambdaMetafactory.metafactory { (Ljava/lang/Object;)V, { invokestatic, mcp/parity/Indy.lambda$run$0, (Ljava/lang/String;)V }, (Ljava/lang/String;)V }
      invokevirtual java/util/ArrayList.forEach (Ljava/util/function/Consumer;)V
      return
    }
  }
  .method private static synthetic lambda$run$0 (Ljava/lang/String;)V {
    code: {
      return
    }
  }
}
)JASM";
}

std::string constantDynamicSource()
{
    return R"JASM(.super java/lang/Object
.class public super mcp/parity/Condy {
  .method public static value ()V {
    code: {
      ldc { fixtureConstant, Ljava/lang/String;, { invokestatic, mcp/parity/Bootstrap.get, (II)Ljava/lang/String; }, { 1, 2 } }
      pop
      return
    }
  }
}
)JASM";
}

std::string reflectionSource()
{
    return R"JASM(.super java/lang/Object
.class public super mcp/parity/ReflectionUser {
  .method public static load ()V {
    code: {
      ldc "mcp.parity.ReflectionTarget"
      invokestatic java/lang/Class.forName (Ljava/lang/String;)Ljava/lang/Class;
      pop
      return
    }
  }
}
)JASM";
}

std::string complexFrameSource()
{
    return R"JASM(.super java/lang/Object
.class public super mcp/parity/FrameSubject {
  .method public static choose (Z)I {
    parameters: { flag },
    code: {
      iload flag
      ifeq right
      bipush 1
      goto join
    right:
      bipush 2
    join:
      ireturn
    }
  }
}
)JASM";
}

//==== 长名 fixture ====

///内部名总长恰为 totalLength 的合法 ASCII 内部名：mcp/ 前缀 + 'x' 填充 + suffix。
std::string longInternalNameWithSuffix(std::size_t totalLength, const std::string& suffix)
{
    const std::string prefix = "mcp/L";
    if (totalLength < prefix.size() + suffix.size())
        throw std::runtime_error("length too small for long name fixture");
    std::string name = prefix;
    name.append(totalLength - prefix.size() - suffix.size(), 'x');
    name += suffix;
    return name;
}

///单类档的内部名：后缀 "_<len>" 同时充当 search_classes 的唯一命中探针。
std::string longInternalName(std::size_t totalLength)
{
    return longInternalNameWithSuffix(totalLength, "_" + std::to_string(totalLength));
}

///count 个类、内部名总长 totalLength 的 jar（另有普通名对照类 mcp/Normal）。
///长名类经 ClassWriter 重写内部名生成（装配器只装配普通模板，避免 jasm
///文本行长度等装配器自身限制混入测量）。totalLength 取 0 时全部用普通名
///（mcp/Bulk0..N），作同规模对照基线。
std::filesystem::path writeBulkLongNameFixture(std::size_t count, std::size_t totalLength)
{
    mjbase::jasm::JvmJasmService assembler;
    const auto templateAssembled =
        assembler.assemble(utf16("Template.jasm"), utf16(classSourceFor("mcp/Template", "1")));
    if (!templateAssembled.isOk())
        throw std::runtime_error("long name fixture template did not assemble");
    const auto normalAssembled =
        assembler.assemble(utf16("Normal.jasm"), utf16(classSourceFor("mcp/Normal", "2")));
    if (!normalAssembled.isOk())
        throw std::runtime_error("long name fixture control class did not assemble");

    mjbase::model::ResourceContainer container;
    container.addResource("mcp/Normal.class",
                          std::make_unique<mjbase::model::ClassResource>("mcp/Normal.class",
                                                                normalAssembled.value->bytes));
    for (std::size_t index = 0; index < count; ++index) {
        const std::string tag = count > 1 ? "_" + std::to_string(totalLength) + "_" +
                                                std::to_string(index)
                                          : "_" + std::to_string(totalLength);
        const auto longName = totalLength == 0
                                  ? "mcp/Bulk" + std::to_string(index)
                                  : longInternalNameWithSuffix(totalLength, tag);
        const auto longBytes =
            withClassNameAndSignature(templateAssembled.value->bytes, longName, "");
        const auto uri = longName + ".class";
        container.addResource(uri, std::make_unique<mjbase::model::ClassResource>(uri, longBytes));
    }

    const auto path = test::getTempOutPath("mjt_mcp_long_name_" + std::to_string(count) + "x" +
                                           std::to_string(totalLength) + ".jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

///一个 jar 两个类：普通名对照类 mcp/Normal + 内部名总长 totalLength 的长名类。
std::filesystem::path writeLongNameFixture(std::size_t totalLength)
{
    return writeBulkLongNameFixture(1, totalLength);
}

std::filesystem::path writeParityFixture()
{
    mjbase::model::ResourceContainer container;
    const auto addClass = [&](const std::string& uri, const std::string& source) {
        container.addResource(uri,
                              std::make_unique<mjbase::model::ClassResource>(
                                  uri, assembleFixtureClass(uri, source)));
    };

    addClass("mcp/Subject.class", classSource("1"));
    addClass("mcp/parity/Annotated.class", annotationSource());
    addClass("mcp/parity/Indy.class", invokeDynamicSource());
    addClass("mcp/parity/Condy.class", constantDynamicSource());
    addClass("mcp/parity/ReflectionUser.class", reflectionSource());
    addClass("mcp/parity/ReflectionTarget.class", classSourceFor("mcp/parity/ReflectionTarget", "3"));
    addClass("mcp/parity/Left.class", classSourceFor("mcp/parity/Left", "4"));
    addClass("mcp/parity/Right.class", classSourceFor("mcp/parity/Right", "5"));
    addClass("mcp/parity/FrameSubject.class", complexFrameSource());
    addClass("META-INF/versions/9/mcp/parity/Versioned.class",
             classSourceFor("mcp/parity/Versioned", "9"));

    container.addResource(
        "META-INF/MANIFEST.MF",
        std::make_unique<mjbase::model::ManifestResource>(mjbase::jar::Manifest::fromBytes(
            {'M', 'a', 'n', 'i', 'f', 'e', 's', 't', '-', 'V', 'e', 'r', 's', 'i', 'o', 'n', ':', ' ',
             '1', '.', '0', '\r', '\n', 'M', 'u', 'l', 't', 'i', '-', 'R', 'e', 'l', 'e', 'a', 's', 'e',
             ':', ' ', 't', 'r', 'u', 'e', '\r', '\n', '\r', '\n'})));
    container.addResource("assets/payload.bin",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "assets/payload.bin", std::vector<ca::u8>{0x00, 0xFE, 0x7F, 0x20, 0xFF}));
    container.addResource(
        "notes/fixture.txt",
        std::make_unique<mjbase::model::UnknownResource>(
            "notes/fixture.txt",
            std::vector<ca::u8>{'p', 'a', 'r', 'i', 't', 'y', ' ', 'f', 'i', 'x', 't', 'u', 'r', 'e'}));
    container.addResource("mcp/parity/Broken.class",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "mcp/parity/Broken.class", std::vector<ca::u8>{0xCA, 0xFE, 0xBA}));

    const auto path = test::getTempOutPath("mjt_mcp_parity_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

int runFakeDecompiler(int argc, char** argv)
{
    int jarIndex = -1;
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "-jar") {
            jarIndex = index + 1;
            break;
        }
    }
    if (jarIndex < 0) return -1;
    const auto decompilerName = std::filesystem::path(argv[jarIndex]).filename().string();
    if (decompilerName != "fake-mcp-decompiler.jar" &&
        decompilerName != "fake-mcp-decompiler-slow.jar" &&
        decompilerName != "fake-mcp-decompiler-fail.jar" &&
        decompilerName != "fake-mcp-decompiler-partial.jar") {
        return -1;
    }
    if (decompilerName == "fake-mcp-decompiler-fail.jar") return 9;
    if (decompilerName == "fake-mcp-decompiler-slow.jar")
        std::this_thread::sleep_for(std::chrono::seconds(1));

    std::filesystem::path output;
    for (int index = jarIndex + 1; index + 1 < argc; ++index) {
        if (std::string(argv[index]) == "--outputdir") {
            output = argv[index + 1];
            break;
        }
    }
    if (output.empty() && jarIndex + 1 < argc) output = argv[argc - 1];
    if (output.empty()) return 7;

    //partial 注入：rc=0 但目标类缺席（后端类级异常只上 stderr、整体照常退出）
    //——decompile 工具必须把 stderr 根因摘要带进产物缺失错误消息。
    if (decompilerName == "fake-mcp-decompiler-partial.jar") {
        std::error_code partialError;
        std::filesystem::create_directories(output / "mcp", partialError);
        if (partialError) return 7;
        std::ofstream other(output / "mcp" / "Other.java",
                            std::ios::binary | std::ios::trunc);
        other << "package mcp;\npublic class Other { }\n";
        if (!other) return 7;
        other.close();
        std::cerr << "WARNING: skipping class\n"
                  << "org.benf.cfr.reader.util.ConfusedCFRException: Cannot decompile mcp/Subject\n"
                  << "\tat org.benf.cfr.reader.State.getOrFallback(State.java)\n"
                  << "Caused by: java.lang.IllegalStateException: deep cause for mcp/Subject\n";
        return 0;
    }

    std::error_code error;
    std::filesystem::create_directories(output / "mcp", error);
    if (error) return 7;
    const auto writeSource = [&](const char* name) {
        std::ofstream source(output / "mcp" / (std::string(name) + ".java"),
                             std::ios::binary | std::ios::trunc);
        source << "package mcp;\npublic class " << name
               << " { public static int value() { return 42; } }\n";
        return static_cast<bool>(source);
    };
    if (!writeSource("Subject") || !writeSource("WorkflowSubject") || !writeSource("Second")) return 7;
    std::cout << "fake MCP decompiler ok\n";
    return 0;
}

}   //namespace mjt::mcp::test_support

namespace mjt::mcp {

int runFakeDecompilerProcess(int argc, char** argv)
{
    return test_support::runFakeDecompiler(argc, argv);
}

}   //namespace mjt::mcp
