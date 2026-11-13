#include "analysis_ingest.hpp"

#include "analysis_ops.hpp"

#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "libca/crypto/sha256.hpp"
#include "jbc/ir/units.hpp"
#include "mjbase/project/entry_shape.hpp"
#include "constant_scan.hpp"
#include "dex_domain.hpp"
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
///        dex 条目（apk 根级 *.dex / 裸 dex）单列 "dex"——二进制字节码容器，
///        不参与资源串抽取（可打印 run 扫描会产生噪声行）。
Utf8String entryKind(const std::string& type)
{
    if (type == "class") return Utf8String::from_cstr("class");
    if (type == "manifest") return Utf8String::from_cstr("manifest");
    if (type == "dex") return Utf8String::from_cstr("dex");
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

    auto resources = listResources(*workspace.workingContainer(), true);
    if (resources.is_err()) {
        return ca::core::Err(workspaceError(std::move(resources).unwrap_err()));
    }
    // listResources 按 uri 字典序返回，满足 canonical 序列化的确定性要求。
    const auto resourceList = std::move(resources).unwrap();

    std::vector<mjbase::project::DbEntryInput> entryInputs;
    entryInputs.reserve(resourceList.size());
    // 非 class 条目的资源串切片（path 键控；entry_id 回填在条目登记后）。
    std::map<std::string, std::vector<Utf8String>> resourceValues;
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
        auto content = readResource(*workspace.workingContainer(), resource.uri);
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

        // 资源串抽取（schema v13）：非 class 条目的可打印 run——与引擎
        // resourceStrings 活扫描同一事实源（readResource 的 bytesOf 序列化
        // 同形），抽取规则在 constant_scan 单源。dex 条目跳过（二进制字节码
        // 容器，run 扫描是噪声）。entry_id 在登记后回填。
        if (resource.type.to_std_string() != "class" &&
            resource.type.to_std_string() != "dex") {
            auto uri = resource.uri.to_std_string();
            auto strings = workspace.format() == ContainerFormat::Apk && uri == "AndroidManifest.xml"
                               ? androidManifestStringRecords(bytes)
                               : resourceStringRecords(bytes);
            resourceValues.emplace(std::move(uri), std::move(strings));
        }
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

    // dex 会话：类事实直接出自 dex 域（容器自由函数只认 jbc class 条目）。
    WorkspaceResult<std::vector<ClassInfo>> classInfos =
        workspace.isDexBacked() && workspace.dexDomain() != nullptr
            ? workspace.dexDomain()->listClasses()
            : listClasses(*workspace.workingContainer(), workspace.lvtJarFallbackFlag());
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
            member.signature  = utf8(field.signature.ref());
            member.access     = field.access;
            input.members.push_back(std::move(member));
        }
        for (const MethodInfo& method : info.methods) {
            mjbase::project::DbMemberInput member;
            member.kind       = mjbase::project::DbMemberKind::Method;
            member.name       = utf8(method.name.ref());
            member.descriptor = utf8(method.descriptor.ref());
            member.signature  = utf8(method.signature.ref());
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
        // DB Method rows are used as direct call edges by referencesFromMember.
        if (reference.kind != MethodReferenceKind::Direct) continue;
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
        if (reference.kind != FieldReferenceKind::Direct) continue;
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

    //-- 常量事实面（cp_strings / number_literals，mjbase schema v12）-----------
    // dex 会话：DexDomain 常量面直接出切片（const-string→strings、静态初值/
    // const 字面量→numbers），jbc 会话段整段跳过。jar 会话：逐类重读字节建
    // jbc 会话（引擎 sessionOf 同形输入），抽取层与引擎搜索/编辑同步同一
    // 事实源。两路径同构攒 DbClassConstantFaces，走查收尾后单次
    // replaceConstantFaces 整库单事务重建（mjbase 0.0.29 整库入口）——
    // 逐类/逐方法切片调用各自开一事务，windows 上每事务 WAL fsync（fat 夹具
    // ≈ 4.4 万次调用）实证悬崖由此消除。批次锚与 replaceReferences 同批：
    // 每类 (2, class_id)、每个出现的方法 (3, member_id)（mjbase 整库入口
    // 合批落锚，INSERT OR IGNORE 幂等）；增量编辑仍走逐切片入口（edit_sync）。
    const bool dexBacked = workspace.isDexBacked();
    const DexDomain* dexDomain = workspace.dexDomain();
    std::vector<mjbase::project::DbClassConstantFaces> classFaces;
    for (const ClassInfo& info : classList) {
        const auto entry = entryIdByPath.find(info.resourceUri.to_std_string());
        if (entry == entryIdByPath.end()) continue;
        auto found = db.findClass(utf8Ref(info.internalName.to_std_string()));
        if (found.is_err()) {
            return ca::core::Err(databaseError(std::move(found).unwrap_err()));
        }
        auto classRow = std::move(found).unwrap();
        if (!classRow.has_value()) continue;   // 未登记类（防御）：开期导入幂等重跑兜底

        mjbase::project::DbClassConstantFaces faces;
        faces.classId    = classRow->classId;
        const std::string internalName = info.internalName.to_std_string();

        std::vector<MethodNumberSlice> methodSlices;
        if (dexBacked && dexDomain != nullptr) {
            // dex 域常量面（引用集口径见 dex_domain.hpp；入库失败 fail-closed
            // ——类事实已随批登记，缺席即状态不一致，不静默丢面）。
            auto dexStrings = dexDomain->cpStringRecords(internalName);
            if (dexStrings.is_err()) {
                return ca::core::Err(workspaceError(std::move(dexStrings).unwrap_err()));
            }
            auto dexNumbers = dexDomain->cpNumberRecords(internalName);
            if (dexNumbers.is_err()) {
                return ca::core::Err(workspaceError(std::move(dexNumbers).unwrap_err()));
            }
            auto dexSlices = dexDomain->methodNumberSlices(internalName);
            if (dexSlices.is_err()) {
                return ca::core::Err(workspaceError(std::move(dexSlices).unwrap_err()));
            }
            faces.cpStrings = std::move(dexStrings).unwrap();
            faces.cpNumbers = std::move(dexNumbers).unwrap();
            methodSlices    = std::move(dexSlices).unwrap();
        }
        else {
            auto content = readResource(*workspace.workingContainer(), info.resourceUri.ref());
            if (content.is_err()) {
                return ca::core::Err(workspaceError(std::move(content).unwrap_err()));
            }
            auto bytes = std::move(content).unwrap().bytes;
            auto opened = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
            if (opened.is_err()) continue;   // 引擎搜索同款宽容：畸形 class 跳过
            auto held = std::move(opened).unwrap();

            faces.cpStrings = cpStringRecords(held->backing());
            faces.cpNumbers = cpNumberRecords(held->backing());
            methodSlices    = methodNumberSlices(*held);
        }
        stats.constantStrings += faces.cpStrings.size();
        stats.constantNumbers += faces.cpNumbers.size();

        auto members = db.listMembers(utf8Ref(internalName));
        if (members.is_err()) {
            return ca::core::Err(databaseError(std::move(members).unwrap_err()));
        }
        std::map<std::pair<std::string, std::string>, ca::i64> methodIds;
        for (const mjbase::project::DbMemberInfo& row : std::move(members).unwrap()) {
            if (row.kind != mjbase::project::DbMemberKind::Method) continue;
            methodIds[{row.name.to_std_string(), row.descriptor.to_std_string()}] = row.memberId;
        }
        for (auto& slice : methodSlices) {
            const auto id = methodIds.find({slice.name, slice.descriptor});
            if (id == methodIds.end()) continue;   // 成员行缺席（防御）：类事实已随批登记，重跑自愈
            if (slice.records.empty()) continue;   // 无数值行不落成员切片（mjbase 逐切片锚行只携带真实覆盖）
            mjbase::project::DbMemberNumberSlices memberNumbers;
            memberNumbers.memberId = id->second;
            memberNumbers.records  = std::move(slice.records);
            stats.constantNumbers += memberNumbers.records.size();
            faces.memberNumbers.push_back(std::move(memberNumbers));
        }
        classFaces.push_back(std::move(faces));
    }

    auto replacedFaces = db.replaceConstantFaces(batchId, std::move(classFaces));
    if (replacedFaces.is_err()) {
        return ca::core::Err(databaseError(std::move(replacedFaces).unwrap_err()));
    }

    //-- 资源串事实面（resources，mjbase schema v13）---------------------------
    // 条目走查攒的切片按活跃条目 id 回填后单次 replaceResources（整库单事务，
    // batchId 同现行批次——批次锚复用 input 作用域）。资源变更 = 结构变更走
    // 重建（#97 分工），无逐条目增量入口。
    std::vector<mjbase::project::DbResourceEntryInput> resourceInputs;
    for (auto& [path, values] : resourceValues) {
        const auto entry = entryIdByPath.find(path);
        if (entry == entryIdByPath.end()) continue;   // 防御：活跃行缺席不写
        mjbase::project::DbResourceEntryInput resourceInput;
        resourceInput.entryId = entry->second;
        resourceInput.values  = std::move(values);
        stats.resourceStrings += resourceInput.values.size();
        resourceInputs.push_back(std::move(resourceInput));
    }
    auto replacedResources = db.replaceResources(batchId, std::move(resourceInputs));
    if (replacedResources.is_err()) {
        return ca::core::Err(databaseError(std::move(replacedResources).unwrap_err()));
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
