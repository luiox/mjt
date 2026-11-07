#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "mcp_test_support.hpp"
#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_model.hpp"
#include "jzip/zip_io.hpp"
#include "libca/zip/checksum.hpp"
#include "workspace/export_manifest.hpp"

namespace mjt::mcp {
namespace {

using namespace test_support;

// ── fixture 辅助 ────────────────────────────────────────────────────────

//源 MANIFEST 文本（LF 行尾、无 Multi-Release——issue #74 的源形态）。
const char* kSourceManifestLf = "Manifest-Version: 1.0\nMain-Class: mcp.Subject\n\n";

std::vector<ca::u8> bytesOfText(const std::string& text)
{
    return std::vector<ca::u8>(text.begin(), text.end());
}

std::string textOf(const std::vector<ca::u8>& bytes)
{
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

ca::str::Utf8StringRef utf8View(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

using JarEntries = std::vector<std::pair<std::string, std::vector<ca::u8>>>;

///@brief 物理字节级受控 jar：条目集/顺序/载荷逐字节按给定写出（STORED，
///不走 exportToJar——其目录占位与 Multi-Release 注入正是被测对象）。
std::filesystem::path writeRawJar(const std::string& fileName, const JarEntries& entries)
{
    jzip::ZipArchive archive;
    for (const auto& [name, data] : entries) {
        auto local = std::make_unique<jzip::LocalFileHeader>();
        local->setFileName(name);
        local->setGeneralPurposeBitFlag(0x0800);   // bit11 = UTF-8
        local->setLastModFileTime(0);
        local->setLastModFileDate(0x0021);   // DOS 纪元 1980-01-01
        local->setOffset(0);
        local->setCompressionMethod(0);   // STORED：fixture 精确控制物理字节
        if (data.empty()) {
            local->setCompressedSize(0);
            local->setUncompressedSize(0);
            local->setCrc32(0);
        }
        else {
            ca::zip::Crc32 crc;
            crc.update(data.data(), data.size());
            local->setCompressedSize(static_cast<ca::i64>(data.size()));
            local->setUncompressedSize(static_cast<ca::i64>(data.size()));
            local->setCrc32(crc.value());
            local->setFileData(data);
        }
        archive.addPart(std::move(local));
    }
    const auto bytes = jzip::ZipIO::writeDirect(archive);
    const auto path  = test::getTempOutPath(fileName);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("raw fixture jar not written: " + fileName);
    return path;
}

///@brief 逐条目读回 jar（条目名原始字节 + 内容），保持物理条目集口径。
JarEntries readJarEntries(const std::filesystem::path& path)
{
    JarEntries entries;
    const std::string pathText = path.u8string();
    mjbase::jar::JvmJarReader reader(utf8View(pathText));
    const auto read =
        reader.read([&](const ca::str::Utf8String& name, const std::vector<ca::u8>& data) {
            entries.emplace_back(std::string(reinterpret_cast<const char*>(name.data()),
                                             name.byte_length()),
                                 data);
        });
    if (read.is_err()) throw std::runtime_error("jar unreadable: " + pathText);
    return entries;
}

const std::vector<ca::u8>* findEntry(const JarEntries& entries, const std::string& name)
{
    const auto it = std::find_if(entries.begin(), entries.end(),
                                 [&name](const auto& entry) { return entry.first == name; });
    return it == entries.end() ? nullptr : &it->second;
}

std::vector<std::string> entryNames(const JarEntries& entries)
{
    std::vector<std::string> names;
    for (const auto& entry : entries) names.push_back(entry.first);
    std::sort(names.begin(), names.end());
    return names;
}

///@brief 开 jar → jasm 编辑一个类 → project_export 到新路径。
///editedJasmSource 须与 editedUri 对应的内部名一致（类名/URI 校验门）。
///非 void 返回值函数不能用 ASSERT_*（其 return; 是 void 退出），失败一律抛。
std::filesystem::path editAndExport(const std::filesystem::path& source,
                                    const std::string&           editedUri,
                                    const std::string&           editedJasmSource,
                                    const std::string&           exportFileName)
{
    McpSessionHarness server;
    const auto opened = call(server, "project_open", {{"path", source.string()}});
    if (opened.at("result").at("isError").get<bool>())
        throw std::runtime_error("project_open failed: " + payload(opened).dump());
    const auto projectId = payload(opened).at("project_id").get<std::string>();

    const auto edited = call(server,
                             "edit_apply",
                             {{"project_id", projectId},
                              {"kind", "jasm"},
                              {"uri", editedUri},
                              {"source", editedJasmSource},
                              {"expected_revision", 0}});
    if (edited.at("result").at("isError").get<bool>())
        throw std::runtime_error("edit_apply failed: " + payload(edited).dump());

    const auto outputPath = test::getTempOutPath(exportFileName);
    std::error_code ec;
    std::filesystem::remove(outputPath, ec);
    const auto exported =
        call(server, "project_export", {{"project_id", projectId}, {"path", outputPath.string()}});
    if (exported.at("result").at("isError").get<bool>())
        throw std::runtime_error("project_export failed: " + payload(exported).dump());
    EXPECT_TRUE(payload(exported).at("exported").get<bool>());
    EXPECT_TRUE(std::filesystem::exists(outputPath));
    return outputPath;
}

// ── (a) versions/ jar + LF MANIFEST 无 Multi-Release：编辑无关类后导出， ──
// ── MANIFEST 逐字节等于源（行尾不动、无注入），versions/ 条目原样保留。  ──
TEST(McpExportFidelityTest, KeepsSourceManifestBytesForUneditedVersionedJar)
{
    const auto subject  = assembleFixtureClass("Subject.jasm", classSource("1"));
    const auto versioned =
        assembleFixtureClass("Versioned.jasm", classSourceFor("mcp/Versioned", "9"));
    const auto sourceManifest = bytesOfText(kSourceManifestLf);
    const auto source         = writeRawJar("mjt_export_fidelity_versioned.jar",
                                    {{"META-INF/MANIFEST.MF", sourceManifest},
                                     {"META-INF/versions/9/mcp/Versioned.class", versioned},
                                     {"mcp/Subject.class", subject}});

    const auto exportedJar =
        editAndExport(source, "mcp/Subject.class", classSourceFor("mcp/Subject", "7"),
                      "mjt_export_fidelity_versioned_out.jar");
    const auto exported    = readJarEntries(exportedJar);

    //条目集与源一致：不新增 META-INF/、META-INF/versions/ 等目录占位。
    EXPECT_EQ((std::vector<std::string>{"META-INF/MANIFEST.MF",
                                        "META-INF/versions/9/mcp/Versioned.class",
                                        "mcp/Subject.class"}),
              entryNames(exported));

    //MANIFEST 逐字节等于源：58B 形态保真（LF 行尾、无 \r、无 Multi-Release）。
    const auto* exportedManifest = findEntry(exported, "META-INF/MANIFEST.MF");
    ASSERT_NE(exportedManifest, nullptr);
    EXPECT_EQ(sourceManifest, *exportedManifest);
    EXPECT_EQ(kSourceManifestLf, textOf(*exportedManifest));
    EXPECT_EQ(std::string::npos, textOf(*exportedManifest).find("Multi-Release"));

    //未触及的 versions/ 条目字节原样（惰性覆盖保持惰性——#74 语义面）。
    const auto* exportedVersioned = findEntry(exported, "META-INF/versions/9/mcp/Versioned.class");
    ASSERT_NE(exportedVersioned, nullptr);
    EXPECT_EQ(versioned, *exportedVersioned);

    //编辑过的无关类物化为新字节（导出确是编辑态）。
    const auto* exportedSubject = findEntry(exported, "mcp/Subject.class");
    ASSERT_NE(exportedSubject, nullptr);
    EXPECT_NE(subject, *exportedSubject);
}

// ── (b) 源含目录条目：导出条目集与源一致（源目录条目保留、不新增占位）。 ──
TEST(McpExportFidelityTest, KeepsEntrySetWithSourceDirectoryEntries)
{
    const auto classBytes = assembleFixtureClass("A.jasm", classSourceFor("mcp/A", "1"));
    const auto sourceManifest = bytesOfText(kSourceManifestLf);
    const auto payloadBytes   = std::vector<ca::u8>{0x00, 0xFE, 0x7F, 0x20, 0xFF};
    const auto source         = writeRawJar("mjt_export_fidelity_dirs.jar",
                                    {{"META-INF/MANIFEST.MF", sourceManifest},
                                     {"mcp/", {}},
                                     {"mcp/A.class", classBytes},
                                     {"assets/", {}},
                                     {"assets/data.bin", payloadBytes}});

    const auto exportedJar =
        editAndExport(source, "mcp/A.class", classSourceFor("mcp/A", "7"),
                      "mjt_export_fidelity_dirs_out.jar");
    const auto exported    = readJarEntries(exportedJar);

    //源目录条目 mcp/、assets/ 原样保留；不为文件条目补 META-INF/ 等父级占位。
    EXPECT_EQ((std::vector<std::string>{"META-INF/MANIFEST.MF", "assets/", "assets/data.bin",
                                        "mcp/", "mcp/A.class"}),
              entryNames(exported));

    const auto* exportedDir = findEntry(exported, "assets/");
    ASSERT_NE(exportedDir, nullptr);
    EXPECT_TRUE(exportedDir->empty());
    const auto* exportedPayload = findEntry(exported, "assets/data.bin");
    ASSERT_NE(exportedPayload, nullptr);
    EXPECT_EQ(payloadBytes, *exportedPayload);

    const auto* exportedManifest = findEntry(exported, "META-INF/MANIFEST.MF");
    ASSERT_NE(exportedManifest, nullptr);
    EXPECT_EQ(sourceManifest, *exportedManifest);
}

// ── (c) 真有 MANIFEST 编辑：属性变更物化（插入序保序、无注入）。        ──
// 决策面是纯函数：直接以容器 + 基线 + 源字节驱动 resolveExportManifestBytes。
TEST(McpExportFidelityTest, ResolveManifestCopiesSourceBytesWhenModelUnchanged)
{
    const auto sourceManifest = bytesOfText(kSourceManifestLf);
    mjbase::model::ResourceContainer baseline;
    baseline.addResource("META-INF/MANIFEST.MF",
                         std::make_unique<mjbase::model::ManifestResource>(
                             mjbase::jar::Manifest::fromBytes(sourceManifest)));
    // ResourceContainer 不可拷贝（arena 语义）：工作副本同字节独立构建。
    mjbase::model::ResourceContainer working;
    working.addResource("META-INF/MANIFEST.MF",
                        std::make_unique<mjbase::model::ManifestResource>(
                            mjbase::jar::Manifest::fromBytes(sourceManifest)));

    const auto resolved =
        mjt::workspace::resolveExportManifestBytes(working, &baseline, sourceManifest);
    ASSERT_FALSE(resolved.is_err());
    ASSERT_TRUE(resolved.unwrap().has_value());
    EXPECT_EQ(sourceManifest, resolved.unwrap().value());
}

TEST(McpExportFidelityTest, ResolveManifestMaterializesEditsWithoutInjection)
{
    const auto sourceManifest = bytesOfText(kSourceManifestLf);
    mjbase::model::ResourceContainer baseline;
    baseline.addResource("META-INF/MANIFEST.MF",
                         std::make_unique<mjbase::model::ManifestResource>(
                             mjbase::jar::Manifest::fromBytes(sourceManifest)));

    //工作副本真改了属性：追加 Implementation-Version（既有键不动的插入序）。
    auto editedManifest = mjbase::jar::Manifest::fromBytes(sourceManifest);
    editedManifest.setMainAttribute(utf8View("Implementation-Version"), utf8View("2.0"));
    mjbase::model::ResourceContainer working;
    working.addResource("META-INF/MANIFEST.MF",
                        std::make_unique<mjbase::model::ManifestResource>(
                            std::move(editedManifest)));

    const auto resolved =
        mjt::workspace::resolveExportManifestBytes(working, &baseline, sourceManifest);
    ASSERT_FALSE(resolved.is_err());
    ASSERT_TRUE(resolved.unwrap().has_value());
    const auto materialized = textOf(resolved.unwrap().value());

    //既有属性保插入序在前，编辑属性物化在后；不做 Multi-Release 推断注入。
    EXPECT_EQ(0U, materialized.find("Manifest-Version: 1.0\r\n"));
    EXPECT_NE(std::string::npos, materialized.find("Implementation-Version: 2.0\r\n"));
    EXPECT_EQ(std::string::npos, materialized.find("Multi-Release"));
    EXPECT_TRUE(materialized.size() > 4 && materialized.substr(materialized.size() - 2) == "\r\n");
}

TEST(McpExportFidelityTest, ResolveManifestAddsMinimalManifestOnlyForVersionedEntries)
{
    //无 MANIFEST + JEP 238 版本化类（≥9）：现状收口契约——补最小 MANIFEST
    //（缺属性 JVM 9+ 忽略整棵 versions 树），字节同 mjbase 无 Manifest 分支。
    mjbase::model::ResourceContainer versioned;
    versioned.addResource("META-INF/versions/9/mcp/Versioned.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "META-INF/versions/9/mcp/Versioned.class",
                              assembleFixtureClass("Versioned.jasm",
                                                   classSourceFor("mcp/Versioned", "9"))));
    const auto resolved =
        mjt::workspace::resolveExportManifestBytes(versioned, nullptr, std::nullopt);
    ASSERT_FALSE(resolved.is_err());
    ASSERT_TRUE(resolved.unwrap().has_value());
    EXPECT_EQ(std::string("Manifest-Version: 1.0\r\nMulti-Release: true\r\n\r\n"),
              textOf(resolved.unwrap().value()));

    //版本段 < 9 不构成版本化族：不补 MANIFEST（产物与源一致不带该条目）。
    mjbase::model::ResourceContainer legacy;
    legacy.addResource("META-INF/versions/8/mcp/Legacy.class",
                       std::make_unique<mjbase::model::ClassResource>(
                           "META-INF/versions/8/mcp/Legacy.class",
                           assembleFixtureClass("Legacy.jasm",
                                                classSourceFor("mcp/Legacy", "8"))));
    const auto unresolved =
        mjt::workspace::resolveExportManifestBytes(legacy, nullptr, std::nullopt);
    ASSERT_FALSE(unresolved.is_err());
    EXPECT_FALSE(unresolved.unwrap().has_value());
}

// ── (d) 常规导出回归：无 versions/、无 MANIFEST 的 jar，条目集与源一致， ──
// ── 产物不带 MANIFEST（现状契约：该形态 mjbase 原本也不生成）。          ──
TEST(McpExportFidelityTest, ExportsManifestlessPlainJarWithSourceEntrySet)
{
    const auto classBytes    = assembleFixtureClass("B.jasm", classSourceFor("mcp/B", "1"));
    const auto notesBytes    = bytesOfText("plain fixture");
    const auto source        = writeRawJar("mjt_export_fidelity_plain.jar",
                                    {{"mcp/B.class", classBytes}, {"notes/readme.txt", notesBytes}});

    const auto exportedJar =
        editAndExport(source, "mcp/B.class", classSourceFor("mcp/B", "7"),
                      "mjt_export_fidelity_plain_out.jar");
    const auto exported    = readJarEntries(exportedJar);

    EXPECT_EQ((std::vector<std::string>{"mcp/B.class", "notes/readme.txt"}),
              entryNames(exported));
    EXPECT_EQ(nullptr, findEntry(exported, "META-INF/MANIFEST.MF"));
    const auto* exportedNotes = findEntry(exported, "notes/readme.txt");
    ASSERT_NE(exportedNotes, nullptr);
    EXPECT_EQ(notesBytes, *exportedNotes);
}

}   //namespace
}   //namespace mjt::mcp
