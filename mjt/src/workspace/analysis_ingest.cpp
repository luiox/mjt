#include "analysis_ingest.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "libca/crypto/sha256.hpp"
#include "mjbase/project/entry_shape.hpp"
#include "reference_scan.hpp"
#include "workspace.hpp"

namespace mjt::workspace {
namespace {

using ca::str::Utf8String;

/// @brief 整库导入桥的注册身份（#143 §3.6）：稳定 key 幂等注册，trusted
///        档（产品内置管线）。
constexpr const char* kIngestProducerKey  = "mjt.workspace.ingest";
constexpr const char* kIngestProducerName = "mjt workspace ingest";

#if defined(MJT_VERSION)
/// @brief 自报工具指纹（工具名 + 语义版本；版本字面量单源根 xmake.lua 的
///        MJT_VERSION，与 set_version 同源）。
constexpr const char* kIngestToolFingerprint = "mjt " MJT_VERSION;
#else
constexpr const char* kIngestToolFingerprint = "mjt";
#endif

Utf8String utf8(const std::string& value)
{
    return Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(value.data()),
                                           value.size());
}

/// @brief Utf8String 域文本 → 拥有型 Utf8String（reference_scan 切片字段直通）。
Utf8String utf8(const ca::str::Utf8StringRef& value)
{
    return Utf8String::from_data_unchecked(value.data(), value.byte_length());
}

ca::str::Utf8StringRef utf8Ref(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

AnalysisIngestError workspaceError(const WorkspaceErrorInfo& error)
{
    AnalysisIngestError output;
    output.kind    = AnalysisIngestErrorKind::Workspace;
    output.message = error.message;
    return output;
}

AnalysisIngestError databaseError(mjbase::project::AnalysisDbError error)
{
    AnalysisIngestError output;
    output.kind    = AnalysisIngestErrorKind::Database;
    output.message = error.message.to_std_string();
    return output;
}

/// @brief 容器资源类型串 → 条目 kind（开放文本；workspace 面是小写口径
///        "class"/"manifest"/"resource"，jar/unknown 按不透明 resource
///        记——登记语义 B4 起保持不变，更细的形态切面（路径 facet、jar
///        伪装形态）由 census 投影/导出层细化，见 analysis_census）。
Utf8String entryKind(const std::string& type)
{
    if (type == "class") return Utf8String::from_cstr("class");
    if (type == "manifest") return Utf8String::from_cstr("manifest");
    return Utf8String::from_cstr("resource");
}

}   // namespace

ca::Result<AnalysisIngestStats, AnalysisIngestError> ingestIntoAnalysisDb(
    const WorkspaceBase& workspace, mjbase::project::AnalysisDb& db)
{
    //-- producer 注册表身份（#143 §3.6：导入桥自报 trusted 身份，key 幂等） --

    mjbase::project::DbProducerInput producer;
    producer.key             = Utf8String::from_cstr(kIngestProducerKey);
    producer.name            = Utf8String::from_cstr(kIngestProducerName);
    producer.toolFingerprint = Utf8String::from_cstr(kIngestToolFingerprint);
    producer.trustLevel      = mjbase::project::DbProducerTrustLevel::Trusted;
    auto producerId          = db.registerProducer(producer);
    if (producerId.is_err()) {
        return ca::core::Err(databaseError(std::move(producerId).unwrap_err()));
    }

    //-- 批次开启（一次导入一个 full_import 批；中途失败留残批不 close——
    //    finished_at 空串是合法审计状态，与设计稿「批次与事务的关系」一致） --

    auto batch = db.openBatch(producerId.unwrap(), mjbase::project::DbBatchKind::FullImport,
                              workspace.sourcePath().ref());
    if (batch.is_err()) {
        return ca::core::Err(databaseError(std::move(batch).unwrap_err()));
    }
    const ca::i64 batchId = batch.unwrap();

    //-- 条目身份：容器面遍历 + 内容锚 + 输入/条目登记 ------------------------

    auto resources = workspace.listResources(true);
    if (resources.is_err()) {
        return ca::core::Err(workspaceError(std::move(resources).unwrap_err()));
    }
    // listResources 按 uri 字典序返回，满足 canonical 序列化的确定性要求。
    const auto resourceList = std::move(resources).unwrap();

    std::vector<mjbase::project::DbEntryInput> entryInputs;
    entryInputs.reserve(resourceList.size());
    ca::crypto::SHA256 containerHasher;
    ca::usize          canonicalSize = 0;
    const auto         feedU64       = [&containerHasher](ca::u64 value) {
        ca::u8 raw[8];
        for (ca::usize i = 0; i < 8; ++i) {
            raw[i] = static_cast<ca::u8>((value >> (8 * i)) & 0xff);   // 小端
        }
        containerHasher.add(raw, sizeof(raw));
    };

    for (const ResourceSummary& resource : resourceList) {
        auto content = workspace.readResource(resource.uri);
        if (content.is_err()) {
            return ca::core::Err(workspaceError(std::move(content).unwrap_err()));
        }
        auto bytes = std::move(content).unwrap().bytes;

        // 容器 canonical 序列化（analysis-project-schema-v2 §1.6）：
        // len(uri, u64 小端) + uri + len(payload, u64 小端) + payload。
        feedU64(resource.uri.byte_length());
        containerHasher.add(resource.uri.data(), resource.uri.byte_length());
        feedU64(bytes.size());
        containerHasher.add(bytes.data(), bytes.size());
        canonicalSize += 2 * sizeof(ca::u64) + resource.uri.byte_length() + bytes.size();

        mjbase::project::DbEntryInput input;
        input.path    = utf8(resource.uri.ref());
        input.kind    = entryKind(resource.type.to_std_string());
        input.size    = static_cast<ca::i64>(bytes.size());
        input.sha256 =
            utf8(ca::crypto::sha256_hex(ca::core::ByteSlice(bytes.data(), bytes.size())));
        input.regular = mjbase::project::isRegularEntryPath(resource.uri.ref());
        entryInputs.push_back(std::move(input));
    }

    const std::string containerDigest = containerHasher.get_hash();
    auto inputId = db.ensureInput(workspace.sourcePath(), utf8Ref(containerDigest),
                                  static_cast<ca::i64>(canonicalSize));
    if (inputId.is_err()) {
        return ca::core::Err(databaseError(std::move(inputId).unwrap_err()));
    }
    const ca::i64 primaryInputId = inputId.unwrap();

    auto entryStats = db.ingestEntries(batchId, primaryInputId, std::move(entryInputs));
    if (entryStats.is_err()) {
        return ca::core::Err(databaseError(std::move(entryStats).unwrap_err()));
    }
    const auto entryCounts = entryStats.unwrap();

    // 条目 id 索引（活跃行）：class 条目 → classes.entry_id 关联用。
    auto listedEntries = db.listEntries(primaryInputId);
    if (listedEntries.is_err()) {
        return ca::core::Err(databaseError(std::move(listedEntries).unwrap_err()));
    }
    // unwrap() 的左值重载按值返回（拷贝），DbEntryInfo 携 move-only 的
    // Utf8String 拷不动——必须走 rvalue 重载把行表移动出来。
    auto entryRows = std::move(listedEntries).unwrap();
    std::unordered_map<std::string, ca::i64> entryIdByPath;
    for (const mjbase::project::DbEntryInfo& row : entryRows) {
        if (!row.tombstoned) {
            entryIdByPath.emplace(row.path.to_std_string(), row.entryId);
        }
    }

    //-- 类事实（类头 + 成员清单，class 条目关联） ----------------------------

    auto classInfos = workspace.listClasses();
    if (classInfos.is_err()) {
        return ca::core::Err(workspaceError(std::move(classInfos).unwrap_err()));
    }

    std::unordered_set<std::string> primaryClasses;
    std::vector<mjbase::project::DbClassInput> classes;
    const auto classList = std::move(classInfos).unwrap();
    classes.reserve(classList.size());
    for (const ClassInfo& info : classList) {
        mjbase::project::DbClassInput input;
        input.internalName = utf8(info.internalName.ref());
        input.superName    = utf8(info.superName.ref());
        input.access       = info.access;
        input.version      = info.version;
        const auto entry   = entryIdByPath.find(info.resourceUri.to_std_string());
        input.entryId      = entry != entryIdByPath.end() ? entry->second : 0;
        input.interfaces.reserve(info.interfaces.size());
        for (const ca::str::Utf8String& interfaceName : info.interfaces) {
            input.interfaces.push_back(utf8(interfaceName.ref()));
        }
        input.members.reserve(info.fields.size() + info.methods.size());
        for (const FieldInfo& field : info.fields) {
            mjbase::project::DbMemberInput member;
            member.kind       = mjbase::project::DbMemberKind::Field;
            member.name       = utf8(field.name.ref());
            member.descriptor = utf8(field.descriptor.ref());
            member.access     = field.access;
            input.members.push_back(std::move(member));
        }
        for (const MethodInfo& method : info.methods) {
            mjbase::project::DbMemberInput member;
            member.kind       = mjbase::project::DbMemberKind::Method;
            member.name       = utf8(method.name.ref());
            member.descriptor = utf8(method.descriptor.ref());
            member.access     = method.access;
            input.members.push_back(std::move(member));
        }
        primaryClasses.insert(info.internalName.to_std_string());
        classes.push_back(std::move(input));
    }

    AnalysisIngestStats stats;
    stats.classes          = classes.size();
    stats.entries          = entryCounts.total;
    stats.createdEntries    = entryCounts.created;
    stats.tombstonedEntries = entryCounts.tombstoned;
    for (const mjbase::project::DbClassInput& input : classes) stats.members += input.members.size();

    auto ingested = db.ingestClasses(batchId, std::move(classes));
    if (ingested.is_err()) {
        return ca::core::Err(databaseError(std::move(ingested).unwrap_err()));
    }

    //-- 直接引用（现状不变） --------------------------------------------------

    auto scan = workspace.referenceScan();
    if (scan.is_err()) {
        return ca::core::Err(workspaceError(std::move(scan).unwrap_err()));
    }
    const auto references = std::move(scan).unwrap();

    std::vector<mjbase::project::DbReferenceRecord> records;
    records.reserve(references.methodReferences().size() + references.fieldReferences().size() +
                    references.typeReferences().size());

    for (const MethodReference& reference : references.methodReferences()) {
        if (primaryClasses.find(reference.site.sourceClass.to_std_string()) ==
            primaryClasses.end())
            continue;
        mjbase::project::DbReferenceRecord record;
        record.sourceClass      = utf8(reference.site.sourceClass.ref());
        record.sourceMethod     = utf8(reference.site.sourceMethod.ref());
        record.sourceDescriptor = utf8(reference.site.sourceDescriptor.ref());
        record.insnIndex        = reference.site.insnIndex;
        record.kind             = mjbase::project::DbReferenceKind::Method;
        record.targetClass      = utf8(reference.target.owner.ref());
        record.targetName       = utf8(reference.target.name.ref());
        record.targetDescriptor = utf8(reference.target.descriptor.ref());
        records.push_back(std::move(record));
    }
    for (const FieldReference& reference : references.fieldReferences()) {
        if (primaryClasses.find(reference.site.sourceClass.to_std_string()) ==
            primaryClasses.end())
            continue;
        mjbase::project::DbReferenceRecord record;
        record.sourceClass      = utf8(reference.site.sourceClass.ref());
        record.sourceMethod     = utf8(reference.site.sourceMethod.ref());
        record.sourceDescriptor = utf8(reference.site.sourceDescriptor.ref());
        record.insnIndex        = reference.site.insnIndex;
        record.kind             = mjbase::project::DbReferenceKind::Field;
        record.targetClass      = utf8(reference.target.owner.ref());
        record.targetName       = utf8(reference.target.name.ref());
        record.targetDescriptor = utf8(reference.target.descriptor.ref());
        records.push_back(std::move(record));
    }
    for (const TypeReference& reference : references.typeReferences()) {
        if (primaryClasses.find(reference.site.sourceClass.to_std_string()) ==
            primaryClasses.end())
            continue;
        mjbase::project::DbReferenceRecord record;
        record.sourceClass      = utf8(reference.site.sourceClass.ref());
        record.sourceMethod     = utf8(reference.site.sourceMethod.ref());
        record.sourceDescriptor = utf8(reference.site.sourceDescriptor.ref());
        record.insnIndex        = reference.site.insnIndex;
        record.kind             = mjbase::project::DbReferenceKind::Type;
        record.targetClass      = utf8(reference.targetType.ref());
        records.push_back(std::move(record));
    }

    stats.references = records.size();
    auto replaced    = db.replaceReferences(batchId, std::move(records));
    if (replaced.is_err()) {
        return ca::core::Err(databaseError(std::move(replaced).unwrap_err()));
    }

    //-- 批次收尾：计数从导入统计回填（注册表不核对，对账由消费方负责） ------

    mjbase::project::DbBatchCounts counts;
    counts.classCount  = static_cast<ca::i64>(stats.classes);
    counts.entryCount  = static_cast<ca::i64>(stats.entries);
    counts.memberCount = static_cast<ca::i64>(stats.members);
    counts.refCount    = static_cast<ca::i64>(stats.references);
    auto closed        = db.closeBatch(batchId, counts);
    if (closed.is_err()) {
        return ca::core::Err(databaseError(std::move(closed).unwrap_err()));
    }
    return ca::core::Ok(stats);
}

}   // namespace mjt::workspace
