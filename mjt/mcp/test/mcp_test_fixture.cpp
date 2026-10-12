#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
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

#include <jbytecode/jbytecode.hpp>
#include <mcp/json_rpc.hpp>

#include "mcp_test_support.hpp"
#include "mbase/jasm/jvm_jasm_service.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/model/resource_helper.hpp"
#include "mbase/model/resource_model.hpp"
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
    return ".super java/lang/Object\n"
           ".class public super " +
           name +
           " {\n"
           "  .field public counter I\n"
           "  .method public static value ()I {\n"
           "    code: {\n"
           "      bipush " +
           value +
           "\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::string classSource(const std::string& value)
{
    return classSourceFor("mcp/Subject", value);
}

std::vector<ca::u8> withClassNameAndSignature(const std::vector<ca::u8>& bytes,
                                              const std::string&         internalName,
                                              const std::string&         signature)
{
    ca::str::Utf8StringArena     arena;
    jbytecode::tree::ClassNode   node;
    jbytecode::core::ClassReader reader(arena, bytes);
    reader.accept(&node, 0);
    node.name      = arena.intern(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(internalName.data()), internalName.size()));
    node.signature = arena.intern(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(signature.data()), signature.size()));
    jbytecode::core::ClassWriter writer(
        arena,
        jbytecode::core::ClassWriter::COMPUTE_MAXS | jbytecode::core::ClassWriter::COMPUTE_FRAMES);
    node.accept(&writer);
    return writer.toByteArray();
}

std::filesystem::path writeLargeClassListFixture(const std::string& signature)
{
    base::jasm::JvmJasmService assembler;
    const auto                 sourceName = utf16("LargeClassTemplate.jasm");
    const auto                 source     = utf16(classSourceFor("mcp/large/Template", "1"));
    const auto                 assembled  = assembler.assemble(sourceName.ref(), source.ref());
    if (!assembled.isOk()) throw std::runtime_error("large MCP fixture did not assemble");

    base::ResourceContainer container;
    for (ca::usize index = 0; index < 210; ++index) {
        const auto internalName = "mcp/large/Subject" + std::to_string(index);
        const auto uri          = internalName + ".class";
        container.addResource(
            uri,
            std::make_unique<base::ClassResource>(
                uri, withClassNameAndSignature(assembled.value->bytes, internalName, signature)));
    }
    const auto path = test::getTempOutPath("mjt_mcp_large_class_list_fixture.jar");
    base::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path writeFixture()
{
    base::jasm::JvmJasmService assembler;
    auto                       sourceName = utf16("Subject.jasm");
    auto                       source = utf16(classSource("1"));
    const auto assembled = assembler.assemble(sourceName.ref(), source.ref());
    if (!assembled.isOk()) throw std::runtime_error("MCP fixture did not assemble");

    base::ResourceContainer container;
    container.addResource("mcp/Subject.class",
                          std::make_unique<base::ClassResource>("mcp/Subject.class",
                                                                assembled.value->bytes));
    auto secondName = utf16("Second.jasm");
    auto secondSource = utf16(classSourceFor("mcp/Second", "2"));
    const auto secondAssembled = assembler.assemble(secondName.ref(), secondSource.ref());
    if (!secondAssembled.isOk()) throw std::runtime_error("MCP second fixture did not assemble");
    container.addResource("mcp/Second.class",
                          std::make_unique<base::ClassResource>("mcp/Second.class",
                                                                secondAssembled.value->bytes));
    container.addResource("notice.txt",
                          std::make_unique<base::UnknownResource>(
                              "notice.txt", std::vector<ca::u8>{'m', 'c', 'p', '-', 'f', 'i', 'x', 't', 'u', 'r', 'e'}));
    container.addResource("broken/Invalid.class",
                          std::make_unique<base::UnknownResource>(
                              "broken/Invalid.class", std::vector<ca::u8>{0xCA, 0xFE, 0xBA}));
    const auto path = test::getTempOutPath("mjt_mcp_fixture.jar");
    base::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path writeQuerySupportingFixture()
{
    const auto source = ".super mcp/Subject\n"
                        ".class public super support/McpLibrary {\n"
                        "  .method public static call ()I {\n"
                        "    code: {\n"
                        "      invokestatic mcp/Subject.value ()I\n"
                        "      ireturn\n"
                        "    }\n"
                        "  }\n"
                        "}\n";
    base::jasm::JvmJasmService assembler;
    auto sourceName = utf16("support/McpLibrary.jasm");
    auto sourceText = utf16(source);
    const auto assembled = assembler.assemble(sourceName.ref(), sourceText.ref());
    if (!assembled.isOk()) throw std::runtime_error("MCP supporting fixture did not assemble");
    base::ResourceContainer container;
    container.addResource(
        "support/McpLibrary.class",
        std::make_unique<base::ClassResource>(
            "support/McpLibrary.class",
            assembled.value->bytes));
    const auto path = test::getTempOutPath("mjt_mcp_query_support.jar");
    base::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::vector<ca::u8> assembleFixtureClass(const std::string& sourceName, const std::string& source)
{
    base::jasm::JvmJasmService assembler;
    auto                       name = utf16(sourceName + ".jasm");
    auto                       text = utf16(source);
    const auto assembled = assembler.assemble(name.ref(), text.ref());
    if (!assembled.isOk()) throw std::runtime_error("MCP parity fixture did not assemble: " + sourceName);
    return assembled.value->bytes;
}

std::string annotationSource()
{
    return ".visible-annotation mcp/parity/TestAnnotation {\n"
           "  number: 15,\n"
           "  nested: .annotation mcp/parity/NestedAnnotation { value: \"fixture\" }\n"
           "}\n"
           ".invisible-annotation mcp/parity/TestAnnotation { value: \"hidden\" }\n"
           ".super java/lang/Object\n"
           ".class public super mcp/parity/Annotated {\n"
           "  .method public static value ()I {\n"
           "    code: {\n"
           "      bipush 7\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::string invokeDynamicSource()
{
    return ".super java/lang/Object\n"
           ".class public super mcp/parity/Indy {\n"
           "  .method public static run ()V {\n"
           "    code: {\n"
           "      new java/util/ArrayList\n"
           "      dup\n"
           "      invokespecial java/util/ArrayList.<init> ()V\n"
           "      astore values\n"
           "      aload values\n"
           "      invokedynamic accept ()Ljava/util/function/Consumer; LambdaMetafactory.metafactory { (Ljava/lang/Object;)V, { invokestatic, mcp/parity/Indy.lambda$run$0, (Ljava/lang/String;)V }, (Ljava/lang/String;)V }\n"
           "      invokevirtual java/util/ArrayList.forEach (Ljava/util/function/Consumer;)V\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "  .method private static synthetic lambda$run$0 (Ljava/lang/String;)V {\n"
           "    code: {\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::string constantDynamicSource()
{
    return ".super java/lang/Object\n"
           ".class public super mcp/parity/Condy {\n"
           "  .method public static value ()V {\n"
           "    code: {\n"
           "      ldc { fixtureConstant, Ljava/lang/String;, { invokestatic, mcp/parity/Bootstrap.get, (II)Ljava/lang/String; }, { 1, 2 } }\n"
           "      pop\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::string reflectionSource()
{
    return ".super java/lang/Object\n"
           ".class public super mcp/parity/ReflectionUser {\n"
           "  .method public static load ()V {\n"
           "    code: {\n"
           "      ldc \"mcp.parity.ReflectionTarget\"\n"
           "      invokestatic java/lang/Class.forName (Ljava/lang/String;)Ljava/lang/Class;\n"
           "      pop\n"
           "      return\n"
           "    }\n"
           "  }\n"
           "}\n";
}

std::string complexFrameSource()
{
    return ".super java/lang/Object\n"
           ".class public super mcp/parity/FrameSubject {\n"
           "  .method public static choose (Z)I {\n"
           "    parameters: { flag },\n"
           "    code: {\n"
           "      iload flag\n"
           "      ifeq right\n"
           "      bipush 1\n"
           "      goto join\n"
           "    right:\n"
           "      bipush 2\n"
           "    join:\n"
           "      ireturn\n"
           "    }\n"
           "  }\n"
           "}\n";
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
    base::jasm::JvmJasmService assembler;
    const auto templateAssembled =
        assembler.assemble(utf16("Template.jasm"), utf16(classSourceFor("mcp/Template", "1")));
    if (!templateAssembled.isOk())
        throw std::runtime_error("long name fixture template did not assemble");
    const auto normalAssembled =
        assembler.assemble(utf16("Normal.jasm"), utf16(classSourceFor("mcp/Normal", "2")));
    if (!normalAssembled.isOk())
        throw std::runtime_error("long name fixture control class did not assemble");

    base::ResourceContainer container;
    container.addResource("mcp/Normal.class",
                          std::make_unique<base::ClassResource>("mcp/Normal.class",
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
        container.addResource(uri, std::make_unique<base::ClassResource>(uri, longBytes));
    }

    const auto path = test::getTempOutPath("mjt_mcp_long_name_" + std::to_string(count) + "x" +
                                           std::to_string(totalLength) + ".jar");
    base::ResourceHelper::exportToJar(container, path.string());
    return path;
}

///一个 jar 两个类：普通名对照类 mcp/Normal + 内部名总长 totalLength 的长名类。
std::filesystem::path writeLongNameFixture(std::size_t totalLength)
{
    return writeBulkLongNameFixture(1, totalLength);
}

std::filesystem::path writeParityFixture()
{
    base::ResourceContainer container;
    const auto addClass = [&](const std::string& uri, const std::string& source) {
        container.addResource(uri,
                              std::make_unique<base::ClassResource>(
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
        std::make_unique<base::ManifestResource>(base::Manifest::fromBytes(
            {'M', 'a', 'n', 'i', 'f', 'e', 's', 't', '-', 'V', 'e', 'r', 's', 'i', 'o', 'n', ':', ' ',
             '1', '.', '0', '\r', '\n', 'M', 'u', 'l', 't', 'i', '-', 'R', 'e', 'l', 'e', 'a', 's', 'e',
             ':', ' ', 't', 'r', 'u', 'e', '\r', '\n', '\r', '\n'})));
    container.addResource("assets/payload.bin",
                          std::make_unique<base::UnknownResource>(
                              "assets/payload.bin", std::vector<ca::u8>{0x00, 0xFE, 0x7F, 0x20, 0xFF}));
    container.addResource(
        "notes/fixture.txt",
        std::make_unique<base::UnknownResource>(
            "notes/fixture.txt",
            std::vector<ca::u8>{'p', 'a', 'r', 'i', 't', 'y', ' ', 'f', 'i', 'x', 't', 'u', 'r', 'e'}));
    container.addResource("mcp/parity/Broken.class",
                          std::make_unique<base::UnknownResource>(
                              "mcp/parity/Broken.class", std::vector<ca::u8>{0xCA, 0xFE, 0xBA}));

    const auto path = test::getTempOutPath("mjt_mcp_parity_fixture.jar");
    base::ResourceHelper::exportToJar(container, path.string());
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
        decompilerName != "fake-mcp-decompiler-fail.jar") {
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
