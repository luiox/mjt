/// @file        analysis_census_test.cpp
/// @brief       census 普查导出 E2E（#121 §5 B4 / #39）：工作区 → 导入桥 →
///              AnalysisDb 投影 → 版本化表头 TSV 落盘；验收 = 同输入两次
///              导出逐字节一致 + 列契约（#39 首期列）+ 转义与 escapeUnicode
///              （rename mapping 格式）同源。
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "workspace/analysis_census.hpp"
#include "workspace/analysis_ingest.hpp"
#include "libca/crypto/sha256.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/mapping/identifier_util.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "test_helper.hpp"
#include "workspace/workspace.hpp"

using namespace mjt::workspace;
using mjbase::project::AnalysisDb;
using ca::str::Utf8String;
using ca::str::Utf8StringRef;

namespace {

Utf8StringRef refOf(const std::string& value)
{
    return Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(refOf(value));
}

std::vector<ca::u8> assembleFixtureClass(const std::string& source)
{
    mjbase::jasm::JvmJasmService assembler;
    const auto                    assembled = assembler.assemble(utf16("census.jasm").ref(),
                                                                utf16(source).ref());
    if (!assembled.isOk()) throw std::runtime_error("census fixture did not assemble");
    return assembled.value->bytes;
}

/// census 语料：常规类（接口 + 字段 + native 方法）、接口、enum 形态位、
/// 常规资源与一条非常规路径资源（空段）。语料名全部中性构造。
std::filesystem::path writeCensusFixture()
{
    const std::string alphaSource =
        ".super java/lang/Object\n"
        ".implements census/Api\n"
        ".class public super census/Alpha {\n"
        "  .field private state I\n"
        "  .method public static act ()V {\n"
        "    code: {\n"
        "      return\n"
        "    }\n"
        "  }\n"
        "  .method public static native probe ()J {}\n"
        "}\n";
    const std::string apiSource =
        ".class public abstract interface census/Api {\n"
        "  .method public abstract act ()V {}\n"
        "}\n";
    const std::string colorSource =
        ".super java/lang/Enum\n"
        ".class public final enum census/Color {\n"
        "  .method public static values ()[Lcensus/Color; {\n"
        "    code: {\n"
        "      aconst_null\n"
        "      areturn\n"
        "    }\n"
        "  }\n"
        "}\n";

    mjbase::model::ResourceContainer container;
    container.addResource("census/Alpha.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "census/Alpha.class", assembleFixtureClass(alphaSource)));
    container.addResource("census/Api.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "census/Api.class", assembleFixtureClass(apiSource)));
    container.addResource("census/Color.class",
                          std::make_unique<mjbase::model::ClassResource>(
                              "census/Color.class", assembleFixtureClass(colorSource)));
    container.addResource("notes/data.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "notes/data.txt",
                              std::vector<ca::u8>{'c', 'e', 'n', 's', 'u', 's'}));
    container.addResource("bad//name.txt",
                          std::make_unique<mjbase::model::UnknownResource>(
                              "bad//name.txt", std::vector<ca::u8>{'x'}));

    const auto path = test::getTempOutPath("mjt_census_fixture.jar");
    mjbase::model::ResourceHelper::exportToJar(container, path.string());
    return path;
}

std::filesystem::path freshDbPath()
{
    const auto path = test::getTempOutPath("mjt_census.mjdb");
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::remove_all(path.string() + "-wal", ec);
    std::filesystem::remove_all(path.string() + "-shm", ec);
    std::filesystem::create_directories(path.parent_path(), ec);
    return path;
}

std::string readFileBytes(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::vector<std::string> splitLines(const std::string& bytes)
{
    std::vector<std::string> lines;
    std::istringstream       stream(bytes);
    std::string              line;
    while (std::getline(stream, line)) lines.push_back(line);
    return lines;
}

/// 导入桥 → db 的公共前缀：打开 fixture 工作区并整库入库，返回 {db, inputId}。
struct IngestedCensus
{
    AnalysisDb db;
    ca::i64    inputId = 0;
};

IngestedCensus ingestFixture(const std::filesystem::path& fixture)
{
    WorkspaceBase workspace;
    auto          opened = workspace.openJar(fixture.string());
    if (!opened.is_ok()) {
        ADD_FAILURE() << "open jar failed: " << opened.unwrap_err().message;
        return {AnalysisDb::open(refOf(freshDbPath().string())).unwrap(), 0};
    }
    auto dbResult = AnalysisDb::open(refOf(freshDbPath().string()));
    EXPECT_TRUE(dbResult.is_ok());
    auto db = std::move(dbResult).unwrap();

    auto ingested = ingestIntoAnalysisDb(workspace, db);
    if (ingested.is_err()) {
        ADD_FAILURE() << "ingest failed: " << ingested.unwrap_err().message;
        return {std::move(db), 0};
    }
    auto inputId = resolveCensusInput(db, refOf(workspace.sourcePath()));
    EXPECT_TRUE(inputId.is_ok());
    return {std::move(db), inputId.is_ok() ? inputId.unwrap() : 0};
}

}   // namespace

TEST(MjtAnalysisCensusTest, ExportsDeterministicVersionedClassCensusTsv)
{
    const auto fixture = writeCensusFixture();
    auto       session = ingestFixture(fixture);
    ASSERT_GT(session.inputId, 0);

    const auto outDir = test::getTempOutPath("mjt_census_export");
    std::error_code ec;
    std::filesystem::remove_all(outDir, ec);

    auto first = exportCensus(session.db, session.inputId, outDir.string());
    ASSERT_TRUE(first.is_ok()) << first.unwrap_err().message;
    // 目录不存在时自动创建，两表落盘。
    ASSERT_TRUE(std::filesystem::is_regular_file(first.unwrap().classCensus));
    ASSERT_TRUE(std::filesystem::is_regular_file(first.unwrap().containerCensus));

    // 验收（#121 §5 B4）：同输入两次导出逐字节一致。
    auto second = exportCensus(session.db, session.inputId, outDir.string());
    ASSERT_TRUE(second.is_ok());
    const auto firstBytes  = readFileBytes(first.unwrap().classCensus);
    const auto secondBytes = readFileBytes(second.unwrap().classCensus);
    EXPECT_EQ(firstBytes, secondBytes);

    // 字节面：UTF-8 无 BOM、LF 行尾（无 CRT 翻译）。
    ASSERT_FALSE(firstBytes.empty());
    EXPECT_EQ(static_cast<unsigned char>(firstBytes[0]), static_cast<unsigned char>('#'));
    EXPECT_EQ(firstBytes.find('\r'), std::string::npos);

    // 版本化表头 + 列序（#39 首期列契约）。
    const auto lines = splitLines(firstBytes);
    ASSERT_GE(lines.size(), 4U);
    EXPECT_EQ(lines[0], "# class-census v2");
    EXPECT_EQ(lines[1],
              "internal_name\tkind\taccess\tparent\tiface_count\tmethod_count"
              "\tfield_count\tnative_method_count\tbyte_size\tzip_entry");

    // 行序 = internal_name 升序（BINARY 字节序）：census/Alpha < census/Api
    // < census/Color（"Alpha" vs "Api" 在 'l' 0x6C < 'p' 0x70 分出先后）。
    EXPECT_EQ(lines[2].substr(0, lines[2].find('\t')), "census/Alpha");
    EXPECT_EQ(lines[3].substr(0, lines[3].find('\t')), "census/Api");
    EXPECT_EQ(lines[4].substr(0, lines[4].find('\t')), "census/Color");

    // Alpha：class 形态、public super、父类、1 接口、2 方法（1 native）、
    // 1 字段、常规条目；byte_size = 条目字节 > 0。
    {
        std::vector<std::string> cells;
        std::istringstream       row(lines[2]);
        std::string              cell;
        while (std::getline(row, cell, '\t')) cells.push_back(cell);
        ASSERT_EQ(cells.size(), 10U);
        EXPECT_EQ(cells[1], "class");
        EXPECT_EQ(cells[2], "public super");
        EXPECT_EQ(cells[3], "java/lang/Object");
        EXPECT_EQ(cells[4], "1");
        EXPECT_EQ(cells[5], "2");
        EXPECT_EQ(cells[6], "1");
        EXPECT_EQ(cells[7], "1");
        EXPECT_GT(std::stoll(cells[8]), 0);
        EXPECT_EQ(cells[9], "regular");
    }
    // Api：interface 形态、无父类（空列）、abstract 方法、常规条目。
    {
        std::vector<std::string> cells;
        std::istringstream       row(lines[3]);
        std::string              cell;
        while (std::getline(row, cell, '\t')) cells.push_back(cell);
        ASSERT_EQ(cells.size(), 10U);
        EXPECT_EQ(cells[1], "interface");
        EXPECT_EQ(cells[2], "public interface abstract");
        EXPECT_EQ(cells[3], "");
        EXPECT_EQ(cells[4], "0");
        EXPECT_EQ(cells[5], "1");
        EXPECT_EQ(cells[7], "0");
        EXPECT_EQ(cells[9], "regular");
    }
    // Color：enum 形态位（jasm enum 修饰 → ACC_ENUM）。
    {
        std::vector<std::string> cells;
        std::istringstream       row(lines[4]);
        std::string              cell;
        while (std::getline(row, cell, '\t')) cells.push_back(cell);
        ASSERT_EQ(cells.size(), 10U);
        EXPECT_EQ(cells[1], "enum");
        EXPECT_EQ(cells[2], "public final enum");
    }
}

TEST(MjtAnalysisCensusTest, ContainerSummaryReflectsEntryShapeAndFacts)
{
    const auto fixture = writeCensusFixture();
    auto       session = ingestFixture(fixture);
    ASSERT_GT(session.inputId, 0);

    auto text = renderCensus(session.db, session.inputId);
    ASSERT_TRUE(text.is_ok()) << text.unwrap_err().message;
    const auto rendered = std::move(text).unwrap();

    const auto lines = splitLines(rendered.containerCensusTsv);
    ASSERT_GE(lines.size(), 3U);
    EXPECT_EQ(lines[0], "# container-census v2");
    EXPECT_EQ(lines[1], "metric\tvalue");

    // 条目面：5 条真实条目（3 class + 2 resource）+ jar 写入器为逐级路径
    // 生成的 4 个目录占位条目（census/、notes/、bad/、bad//，尾随 '/'
    // 与空段均非常规），共 9 活跃；bad//name.txt 与 bad// 同计空段形态。
    const auto find = [&lines](const std::string& metric) -> std::optional<std::string> {
        for (const auto& line : lines) {
            const auto tab = line.find('\t');
            if (tab != std::string::npos && line.substr(0, tab) == metric) {
                return line.substr(tab + 1);
            }
        }
        return std::nullopt;
    };
    EXPECT_EQ(find("entries_active"), std::optional<std::string>("9"));
    EXPECT_EQ(find("entries_tombstoned"), "0");
    EXPECT_EQ(find("entries_regular"), "4");
    EXPECT_EQ(find("entries_irregular"), "5");
    EXPECT_EQ(find("entry_kind_class"), "3");
    EXPECT_EQ(find("entry_kind_resource"), "6");
    EXPECT_EQ(find("entry_shape_regular"), "4");
    EXPECT_EQ(find("entry_shape_trailing_slash"), "3");
    EXPECT_EQ(find("entry_shape_empty_segment"), "2");
    EXPECT_EQ(find("classes"), "3");
    EXPECT_EQ(find("methods"), "4");
    EXPECT_EQ(find("fields"), "1");
    EXPECT_EQ(find("native_methods"), "1");
    // 容器身份：来源路径与 fixture 一致（导入桥身份键）。
    const auto pathRow = find("input_path");
    ASSERT_TRUE(pathRow.has_value());
    EXPECT_NE(pathRow->find("mjt_census_fixture"), std::string::npos);
}

TEST(MjtAnalysisCensusTest, TextCellsEscapeWithRenameMappingConvention)
{
    // 列文本统一 \uXXXX 转义（与 rename mapping 同一套）：控制字符、制表、
    // 反斜杠、高位字节都出列转义，行内制表数恒定（TSV 结构不被语料破坏）。
    std::vector<mjbase::project::DbClassCensusRow> rows;
    mjbase::project::DbClassCensusRow row;
    row.internalName = Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>("weird\tname\x01"), 11);
    row.superName = Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>("back\\slash"), 10);
    row.access  = 0x0021;
    row.kind    = mjbase::project::DbClassKind::Class;
    row.methodCount = 1;
    rows.push_back(std::move(row));

    const auto rendered = renderClassCensusTsv(rows);
    const auto lines    = splitLines(rendered);
    ASSERT_EQ(lines.size(), 3U);   // 版本行 + 列名行 + 1 数据行
    EXPECT_NE(lines[2].find("weird\\u0009name\\u0001"), std::string::npos);
    EXPECT_NE(lines[2].find("back\\u005Cslash"), std::string::npos);
    // 表格结构完好：数据行仍为 10 列（9 个制表位）。
    EXPECT_EQ(std::count(lines[2].begin(), lines[2].end(), '\t'), 9);

    // 补充平面类名（规范形 4 字节 UTF-8，decodeClass 出库形态）：代理对
    // 出码（U+1F600 → \uD83D\uDE00），转义→反转义逐字节闭合，列内仍无
    // 裸控制字节。
    std::vector<mjbase::project::DbClassCensusRow> supplementaryRows;
    mjbase::project::DbClassCensusRow supplementary;
    supplementary.internalName = Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>("emo\xF0\x9F\x98\x80ji"), 9);
    supplementary.kind = mjbase::project::DbClassKind::Class;
    supplementaryRows.push_back(std::move(supplementary));
    const auto escaped = renderClassCensusTsv(supplementaryRows);
    EXPECT_NE(escaped.find("emo\\uD83D\\uDE00ji"), std::string::npos);
    const auto roundtrip = mjbase::mapping::IdentifierUtil::unescapeUnicode(
        mjbase::mapping::IdentifierUtil::escapeUnicode(
            ca::str::Utf8StringRef::from_data(
                reinterpret_cast<const ca::u8*>("emo\xF0\x9F\x98\x80ji"), 9))
            .to_std_string());
    EXPECT_EQ(roundtrip, "emo\xF0\x9F\x98\x80ji");
}

TEST(MjtAnalysisCensusTest, OversizedCellsFoldToDeterministicIdentityForm)
{
    // v2 折叠契约：转义后超 kCensusCellFoldBytes 的单元格渲染为
    // #folded:len=<原始字节数>:sha256=<原始字节摘要>——保身份丢字面，
    // 行结构完好（10 列不动），确定性（两次渲染逐字节一致）。
    const std::string fatName(5000, 'a');
    const std::string expectedDigest = ca::crypto::sha256_hex(
        ca::core::ByteSlice(reinterpret_cast<const ca::u8*>(fatName.data()), fatName.size()));
    const std::string expectedCell =
        "#folded:len=5000:sha256=" + expectedDigest;

    std::vector<mjbase::project::DbClassCensusRow> rows;
    mjbase::project::DbClassCensusRow fat;
    fat.internalName = Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(fatName.data()), fatName.size());
    fat.superName = Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>("java/lang/Object"), 16);
    fat.kind = mjbase::project::DbClassKind::Class;
    rows.push_back(std::move(fat));

    const auto first  = renderClassCensusTsv(rows);
    const auto second = renderClassCensusTsv(rows);
    EXPECT_EQ(first, second);
    const auto lines = splitLines(first);
    ASSERT_EQ(lines.size(), 3U);
    EXPECT_NE(lines[2].find(expectedCell), std::string::npos);
    EXPECT_EQ(std::count(lines[2].begin(), lines[2].end(), '\t'), 9);

    // 边界：转义后恰 4096（≤ 阈值）保字面，4097 折叠。ASCII 名转义不变长。
    const std::string edgeKept(4096, 'b');
    const std::string edgeFolded(4097, 'b');
    std::vector<mjbase::project::DbClassCensusRow> keptRows;
    mjbase::project::DbClassCensusRow kept;
    kept.internalName = Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(edgeKept.data()), edgeKept.size());
    kept.kind = mjbase::project::DbClassKind::Class;
    keptRows.push_back(std::move(kept));
    EXPECT_NE(renderClassCensusTsv(keptRows).find(edgeKept), std::string::npos);
    std::vector<mjbase::project::DbClassCensusRow> foldedRows;
    mjbase::project::DbClassCensusRow folded;
    folded.internalName = Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(edgeFolded.data()), edgeFolded.size());
    folded.kind = mjbase::project::DbClassKind::Class;
    foldedRows.push_back(std::move(folded));
    const auto foldedOut = renderClassCensusTsv(foldedRows);
    EXPECT_EQ(foldedOut.find(edgeFolded), std::string::npos);
    EXPECT_NE(foldedOut.find("#folded:len=4097:sha256="), std::string::npos);
}

TEST(MjtAnalysisCensusTest, UnregisteredInputFailsInvalid)
{
    const auto fixture = writeCensusFixture();
    auto       session = ingestFixture(fixture);
    ASSERT_GT(session.inputId, 0);

    auto missing = resolveCensusInput(session.db, refOf("no/such/container.jar"));
    ASSERT_TRUE(missing.is_err());
    EXPECT_EQ(missing.unwrap_err().kind, CensusExportErrorKind::Invalid);

    auto exported = exportCensus(session.db, 424242,
                                 test::getTempOutPath("mjt_census_bad").string());
    ASSERT_TRUE(exported.is_err());
    EXPECT_EQ(exported.unwrap_err().kind, CensusExportErrorKind::Invalid);
}
