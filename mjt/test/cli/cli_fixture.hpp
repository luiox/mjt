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

#include "libca/str/utf8_string.hpp"

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

}   // namespace cli_fixture
