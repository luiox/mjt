#include "analysis_census.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <utility>

#include "libca/crypto/sha256.hpp"
#include "mjbase/mapping/identifier_util.hpp"
#include "workspace_types.hpp"

namespace mjt::workspace {
namespace {

using mjbase::project::DbClassCensusRow;
using mjbase::project::DbContainerCensus;
using mjbase::project::DbMemberCensusRow;

CensusExportError databaseError(mjbase::project::AnalysisDbError error)
{
    CensusExportError output;
    output.kind    = CensusExportErrorKind::Database;
    output.message = error.message.to_std_string();
    return output;
}

CensusExportError invalidError(std::string message)
{
    CensusExportError output;
    output.kind    = CensusExportErrorKind::Invalid;
    output.message = std::move(message);
    return output;
}

CensusExportError ioError(std::string message)
{
    CensusExportError output;
    output.kind    = CensusExportErrorKind::Io;
    output.message = std::move(message);
    return output;
}

/// @brief 文本列转义：与 rename mapping 格式同一套 \uXXXX 转义（#39 设计
///        要点 1，全链路一套），出列文本不含制表/换行/空白字节。
std::string escapeCell(const ca::str::Utf8StringRef& value)
{
    return mjbase::mapping::IdentifierUtil::escapeUnicode(value).to_std_string();
}

/// @brief 文本列渲染出口：转义 + 超长折叠（v2 契约，见 analysis_census.hpp）。
///
/// 转义后超 kCensusCellFoldBytes 的单元格渲染为
/// `#folded:len=<原始字节数>:sha256=<原始字节 SHA-256>` 确定形——保身份
/// 丢字面，全名仍在库内事实层；len/摘要对原始字节计算，与 entries.sha256
/// 的内容锚约定一致。
std::string censusCell(const ca::str::Utf8StringRef& value)
{
    const std::string escaped = escapeCell(value);
    if (escaped.size() <= kCensusCellFoldBytes) return escaped;
    const std::string digest =
        ca::crypto::sha256_hex(ca::core::ByteSlice(value.data(), value.byte_length()));
    return "#folded:len=" + std::to_string(value.byte_length()) + ":sha256=" + digest;
}

/// @brief class 级 access 旗标摘要：JVMS 位序命名（bit 升序，序恒定），
///        未知名按 0x hex token 收尾——"flags 摘要"列的确定性口径。
std::string renderAccessFlags(ca::i32 access)
{
    struct FlagName
    {
        ca::i32     bit;
        const char* name;
    };
    // 类级词表（JVMS 4.1 + 工具链伪位 record）；命名风格与 parser.cpp 的
    // access_flags 词表一致（小写旗标名）。
    constexpr FlagName kClassFlags[] = {
        {kAccPublic, "public"},         {kAccFinal, "final"},
        {kAccSuper, "super"},           {kAccInterface, "interface"},
        {kAccAbstract, "abstract"},     {kAccSynthetic, "synthetic"},
        {kAccAnnotation, "annotation"}, {kAccEnum, "enum"},
        {kAccModule, "module"},         {kAccRecord, "record"},
    };
    ca::i32 named = access;
    std::string out;
    for (const FlagName& flag : kClassFlags) {
        if ((access & flag.bit) == 0) continue;
        named &= ~flag.bit;
        if (!out.empty()) out.push_back(' ');
        out += flag.name;
    }
    // 剩余未知名按位升序补 0x hex token（u32 扫描，i32 最高位不进符号域）。
    auto unknown = static_cast<ca::u32>(named);
    for (ca::u32 bit = 1; unknown != 0 && bit != 0; bit <<= 1) {
        if ((unknown & bit) == 0) continue;
        unknown &= ~bit;
        char hex[16];
        std::snprintf(hex, sizeof(hex), " 0x%04x", static_cast<unsigned>(bit));
        out += hex;
    }
    return out;
}

const char* zipEntryCell(bool regular)
{
    return regular ? "regular" : "irregular";
}

/// @brief member kind 列 token：field/method 小写单 token——classKindName
///        同款风格（mjbase 0.0.15 无 memberKindName 命名面，枚举编码序
///        Field < Method 与投影键序一致）。
const char* memberKindName(mjbase::project::DbMemberKind kind)
{
    return kind == mjbase::project::DbMemberKind::Field ? "field" : "method";
}

/// @brief binary 写出（禁 CRT 文本模式翻译，LF 原样落盘）。
ca::Result<void, CensusExportError> writeBytes(const std::filesystem::path& path,
                                               const std::string&           bytes)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file.is_open()) {
        return ca::Err(ioError("cannot open census output: " + path.string()));
    }
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.flush();
    if (!file.good()) {
        return ca::Err(ioError("census output write failed: " + path.string()));
    }
    return ca::Ok();
}

}   // namespace

std::string renderClassCensusTsv(const std::vector<DbClassCensusRow>& rows)
{
    // 表头：版本行 + 列名行（列序变更递增 kClassCensusSchemaVersion）。
    std::string out = "# class-census v" + std::to_string(kClassCensusSchemaVersion) + "\n"
                      "internal_name\tkind\taccess\tparent\tiface_count\tmethod_count"
                      "\tfield_count\tnative_method_count\tbyte_size\tzip_entry"
                      "\torigin\n";
    for (const DbClassCensusRow& row : rows) {
        out += censusCell(row.internalName.ref());
        out += '\t';
        out += mjbase::project::classKindName(row.kind);
        out += '\t';
        out += renderAccessFlags(row.access);
        out += '\t';
        out += censusCell(row.superName.ref());
        out += '\t';
        out += std::to_string(row.ifaceCount);
        out += '\t';
        out += std::to_string(row.methodCount);
        out += '\t';
        out += std::to_string(row.fieldCount);
        out += '\t';
        out += std::to_string(row.nativeMethodCount);
        out += '\t';
        out += std::to_string(row.byteSize);
        out += '\t';
        out += zipEntryCell(row.entryRegular);
        out += '\t';
        // origin（v3 列）：前缀分类派生来源（mjbase library_origin，规则版本
        // kLibraryOriginRuleVersion），token = jdk/known/unknown。
        out += mjbase::project::libraryOriginName(row.origin);
        out += '\n';
    }
    return out;
}

std::string renderContainerCensusTsv(const DbContainerCensus& summary)
{
    std::string out = "# container-census v" +
                      std::to_string(kContainerCensusSchemaVersion) + "\n"
                      "metric\tvalue\n";
    const auto emit = [&out](const std::string& metric, const std::string& value) {
        out += metric;
        out += '\t';
        out += value;
        out += '\n';
    };

    // 容器身份。
    emit("input_path", censusCell(summary.path.ref()));
    emit("input_size", std::to_string(summary.size));
    emit("input_sha256", censusCell(summary.sha256.ref()));
    // 条目面（活跃/墓碑、形态二分）。
    emit("entries_active", std::to_string(summary.entriesActive));
    emit("entries_tombstoned", std::to_string(summary.entriesTombstoned));
    emit("entries_regular", std::to_string(summary.entriesRegular));
    emit("entries_irregular", std::to_string(summary.entriesIrregular));
    // kind 分布（存储值，名称升序；投影面已排序，导出面保序）。
    for (const auto& facet : summary.entryKinds) {
        emit("entry_kind_" + escapeCell(facet.facet.ref()), std::to_string(facet.count));
    }
    // 路径形态分布（entry_shape facet，枚举声明序）。
    for (const auto& facet : summary.entryShapes) {
        emit("entry_shape_" + escapeCell(facet.facet.ref()), std::to_string(facet.count));
    }
    // 分析事实面。
    emit("classes", std::to_string(summary.classes));
    emit("methods", std::to_string(summary.methods));
    emit("fields", std::to_string(summary.fields));
    emit("native_methods", std::to_string(summary.nativeMethods));
    emit("references", std::to_string(summary.references));
    return out;
}

std::string renderMemberCensusTsv(const std::vector<DbMemberCensusRow>& rows)
{
    // 表头：版本行 + 列名行（列序变更递增 kMemberCensusSchemaVersion）。
    // 列契约（#148 批 2，schema v1）：member_class / kind / name /
    // descriptor / access / native / entry。
    std::string out = "# member-census v" +
                      std::to_string(kMemberCensusSchemaVersion) + "\n"
                      "member_class\tkind\tname\tdescriptor\taccess\tnative\tentry\n";
    for (const DbMemberCensusRow& row : rows) {
        out += censusCell(row.ownerInternalName.ref());
        out += '\t';
        out += memberKindName(row.kind);
        out += '\t';
        out += censusCell(row.name.ref());
        out += '\t';
        out += censusCell(row.descriptor.ref());
        out += '\t';
        // access：原始访问旗标十进制。mjbase 0.0.15 无成员旗标命名面
        // （类级 renderAccessFlags 词表不含成员位 static/private/volatile/
        // bridge/varargs 等），v1 不在 mjt 侧另起词表——口径披露进 PR。
        out += std::to_string(row.access);
        out += '\t';
        out += row.native ? "1" : "0";
        out += '\t';
        // entry：所属类条目身份（censusMembers 关联事实；成员行不携带条目
        // 形态旗标，类级 regular/irregular 不外推，故出 entryId 十进制）。
        out += std::to_string(row.entryId);
        out += '\n';
    }
    return out;
}

ca::Result<CensusExportText, CensusExportError> renderCensus(
    const mjbase::project::AnalysisDb& db, ca::i64 inputId)
{
    // 契约：inputId 未登记 → Invalid（未登记输入的普查投影是调用方错误，
    // 不落 Database 通道——mjbase censusGate 对缺失输入报 Schema 类错误，
    // 投影面在渲染前以登记面显式判定，不靠错误串匹配）。
    auto listed = db.listInputs();
    if (listed.is_err()) {
        return ca::Err(databaseError(std::move(listed).unwrap_err()));
    }
    const auto& inputs = std::move(listed).unwrap();
    const bool registered =
        std::any_of(inputs.begin(), inputs.end(),
                    [inputId](const mjbase::project::DbInputInfo& input) {
                        return input.inputId == inputId;
                    });
    if (!registered) {
        return ca::Err(invalidError("census input is not registered: id " +
                                    std::to_string(inputId)));
    }

    auto classes = db.censusClasses(inputId);
    if (classes.is_err()) {
        return ca::Err(databaseError(std::move(classes).unwrap_err()));
    }
    // 成员普查（#148 批 2）：与 censusClasses 同门禁（上方登记判定）同错误
    // 面（查询失败走 Database 通道）。行向量 move-out（行含 Utf8String，
    // 不可拷贝），渲染与计数共用同一份。
    auto members = db.censusMembers(inputId);
    if (members.is_err()) {
        return ca::Err(databaseError(std::move(members).unwrap_err()));
    }
    auto memberRows = std::move(members).unwrap();
    auto summary = db.censusContainer(inputId);
    if (summary.is_err()) {
        return ca::Err(databaseError(std::move(summary).unwrap_err()));
    }

    CensusExportText text;
    text.classCensusTsv     = renderClassCensusTsv(std::move(classes).unwrap());
    text.memberCensusTsv    = renderMemberCensusTsv(memberRows);
    text.containerCensusTsv = renderContainerCensusTsv(std::move(summary).unwrap());
    text.members            = memberRows.size();
    return ca::Ok(std::move(text));
}

ca::Result<CensusExportPaths, CensusExportError> exportCensus(
    const mjbase::project::AnalysisDb& db, ca::i64 inputId, const std::string& directory)
{
    auto text = renderCensus(db, inputId);
    if (text.is_err()) {
        return ca::Err(std::move(text).unwrap_err());
    }

    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (!std::filesystem::is_directory(directory, ec)) {
        return ca::Err(ioError("cannot create census output directory: " + directory));
    }

    const std::filesystem::path dir(directory);
    CensusExportPaths paths;
    paths.classCensus     = (dir / "class_census.tsv").string();
    paths.containerCensus = (dir / "container_census.tsv").string();
    paths.memberCensus    = (dir / "member_census.tsv").string();

    auto rendered = std::move(text).unwrap();
    if (auto written = writeBytes(paths.classCensus, rendered.classCensusTsv);
        written.is_err()) {
        return ca::Err(std::move(written).unwrap_err());
    }
    if (auto written = writeBytes(paths.containerCensus, rendered.containerCensusTsv);
        written.is_err()) {
        return ca::Err(std::move(written).unwrap_err());
    }
    // member_census.tsv 与 class_census.tsv 配套（#148 设计稿推荐口径）：
    // 同一次导出、同临时库生命周期，不存在独立入口。
    if (auto written = writeBytes(paths.memberCensus, rendered.memberCensusTsv);
        written.is_err()) {
        return ca::Err(std::move(written).unwrap_err());
    }
    paths.members = rendered.members;
    return ca::Ok(std::move(paths));
}

ca::Result<ca::i64, CensusExportError> resolveCensusInput(
    const mjbase::project::AnalysisDb& db, const ca::str::Utf8StringRef& sourcePath)
{
    auto listed = db.listInputs();
    if (listed.is_err()) {
        return ca::Err(databaseError(std::move(listed).unwrap_err()));
    }
    for (const mjbase::project::DbInputInfo& input : std::move(listed).unwrap()) {
        if (input.path.ref() == sourcePath) return ca::Ok(input.inputId);
    }
    return ca::Err(invalidError("census input is not registered: " +
                                sourcePath.to_std_string()));
}

}   // namespace mjt::workspace
