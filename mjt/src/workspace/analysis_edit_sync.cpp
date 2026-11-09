#include "analysis_edit_sync.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "libca/crypto/sha256.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "mjbase/project/entry_shape.hpp"
#include "reference_scan.hpp"
#include "workspace.hpp"

namespace mjt::workspace {
namespace {

using ca::str::Utf8String;

/// 方法身份键：name + descriptor（库成员行与 DOM 方法表的公共键形）。
using MethodKey = std::pair<std::string, std::string>;

/// @brief 规范形 UTF-8 名 → 借用视图（字节边界直通，与 analysis_edit 同口径）。
ca::str::Utf8StringRef utf8RefOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// @brief 底层文本（string_view）→ 拥有型 Utf8String（与导入桥 utf8 同口径）。
Utf8String utf8(std::string_view value)
{
    return Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(value.data()),
                                           value.size());
}

/// @brief 容器资源类型串 → 条目 kind（与 analysis_ingest 的 entryKind 同口径；
///        重复实现是为保持两桥各自独立的小工具面，词表漂移由 census 投影对账）。
Utf8String entryKindOf(const std::string& type)
{
    if (type == "class") return Utf8String::from_cstr("class");
    if (type == "manifest") return Utf8String::from_cstr("manifest");
    return Utf8String::from_cstr("resource");
}

/// @brief 编辑后类的引用切片重扫结果 → DbReferenceRecord 分桶。
///
/// 与导入桥同形转换（method→field→type 三段），按来源方法分桶：方法体切片
/// 逐方法替换，source method 为空的行是类头切片。site 过滤只收 @p owner
/// 名下的事实（buildFor 按条目过滤，这里再按类名收敛——换名重汇编的条目
/// DOM 类名即新名，扫描行天然落在新名桶里）。
std::map<MethodKey, std::vector<mjbase::project::DbReferenceRecord>> collectMethodSlices(
    const ReferenceScan& scan, const std::string& owner,
    std::vector<mjbase::project::DbReferenceRecord>& header)
{
    std::map<MethodKey, std::vector<mjbase::project::DbReferenceRecord>> slices;
    const auto push = [&](const ca::str::Utf8String& sourceClass,
                          const ca::str::Utf8String& sourceMethod,
                          const ca::str::Utf8String& sourceDescriptor, ca::i32 insnIndex,
                          mjbase::project::DbReferenceKind kind,
                          const ca::str::Utf8String& targetClass,
                          const ca::str::Utf8String& targetName,
                          const ca::str::Utf8String& targetDescriptor) {
        if (sourceClass.to_std_string() != owner) return;
        mjbase::project::DbReferenceRecord record;
        record.sourceClass      = sourceClass.clone();
        record.sourceMethod     = sourceMethod.clone();
        record.sourceDescriptor = sourceDescriptor.clone();
        record.insnIndex        = insnIndex;
        record.kind             = kind;
        record.targetClass      = targetClass.clone();
        record.targetName       = targetName.clone();
        record.targetDescriptor = targetDescriptor.clone();
        if (sourceMethod.is_empty()) header.push_back(std::move(record));
        else
            slices[{sourceMethod.to_std_string(), sourceDescriptor.to_std_string()}].push_back(
                std::move(record));
    };
    for (const MethodReference& reference : scan.methodReferences()) {
        push(reference.site.sourceClass, reference.site.sourceMethod,
             reference.site.sourceDescriptor, reference.site.insnIndex,
             mjbase::project::DbReferenceKind::Method, reference.target.owner,
             reference.target.name, reference.target.descriptor);
    }
    for (const FieldReference& reference : scan.fieldReferences()) {
        push(reference.site.sourceClass, reference.site.sourceMethod,
             reference.site.sourceDescriptor, reference.site.insnIndex,
             mjbase::project::DbReferenceKind::Field, reference.target.owner,
             reference.target.name, reference.target.descriptor);
    }
    for (const TypeReference& reference : scan.typeReferences()) {
        push(reference.site.sourceClass, reference.site.sourceMethod,
             reference.site.sourceDescriptor, reference.site.insnIndex,
             mjbase::project::DbReferenceKind::Type, reference.targetType, Utf8String{},
             Utf8String{});
    }
    return slices;
}

}   // namespace

AnalysisEditSyncStats syncAnalysisDbAfterEdit(
    const WorkspaceBase& workspace, mjbase::project::AnalysisDb& db,
    const model::ResourceContainer& container, const std::vector<ca::str::Utf8String>& changedUris)
{
    AnalysisEditSyncStats stats;

    // 输入行：sourcePath 是导入桥的输入身份键；库内没有该行说明从未整库导入
    // （或库是临建的），编辑同步无从挂锚——整体跳过，开期导入会建行。
    auto inputs = db.listInputs();
    if (inputs.is_err()) {
        stats.failed = true;
        return stats;
    }
    ca::i64 inputId = 0;
    for (const mjbase::project::DbInputInfo& row : std::move(inputs).unwrap()) {
        if (row.path.ref() == workspace.sourcePath().ref()) {
            inputId = row.inputId;
            break;
        }
    }
    if (inputId == 0) {
        stats.skipped += changedUris.size();
        return stats;
    }

    // 条目合并清单：容器活跃资源全量（清单口径与导入桥一致——消失条目要
    // 墓碑、新条目要建行），字节只对变更/无锚条目重读。
    auto listed = workspace.listResources(true);
    if (listed.is_err()) {
        stats.failed = true;
        return stats;
    }
    const auto resourceList = std::move(listed).unwrap();

    std::unordered_set<std::string_view> changed;
    changed.reserve(changedUris.size());
    for (const ca::str::Utf8String& uri : changedUris) {
        changed.emplace(reinterpret_cast<const char*>(uri.data()), uri.byte_length());
    }

    auto entryListed = db.listEntries(inputId);
    if (entryListed.is_err()) {
        stats.failed = true;
        return stats;
    }
    auto entryRows = std::move(entryListed).unwrap();
    std::unordered_map<std::string, const mjbase::project::DbEntryInfo*> activeByPath;
    for (const mjbase::project::DbEntryInfo& row : entryRows) {
        if (!row.tombstoned) activeByPath.emplace(row.path.to_std_string(), &row);
    }

    std::vector<mjbase::project::DbEntryInput> entryInputs;
    entryInputs.reserve(resourceList.size());
    for (const ResourceSummary& resource : resourceList) {
        const std::string path     = resource.uri.to_std_string();
        const auto        anchored = activeByPath.find(path);
        if (changed.find(path) == changed.end() && anchored != activeByPath.end()) {
            mjbase::project::DbEntryInput input;
            input.path    = resource.uri.clone();
            input.kind    = anchored->second->kind.clone();
            input.size    = anchored->second->size;
            input.sha256  = anchored->second->sha256.clone();
            input.regular = anchored->second->regular;
            entryInputs.push_back(std::move(input));
            continue;
        }
        auto content = workspace.readResource(resource.uri.ref());
        if (content.is_err()) {
            // 活跃清单内的资源读不出字节属工作副本态异常：fail-closed 整体停
            // （把该条目静默剔除清单会让 ingestEntries 打墓碑——错动作）。
            stats.failed = true;
            return stats;
        }
        auto bytes = std::move(content).unwrap().bytes;
        mjbase::project::DbEntryInput input;
        input.path    = resource.uri.clone();
        input.kind    = entryKindOf(resource.type.to_std_string());
        input.size    = static_cast<ca::i64>(bytes.size());
        input.sha256  = utf8(ca::crypto::sha256_hex(ca::core::ByteSlice(bytes.data(), bytes.size())));
        input.regular = mjbase::project::isRegularEntryPath(resource.uri.ref());
        entryInputs.push_back(std::move(input));
    }
    auto entryStats = db.ingestEntries(0, inputId, std::move(entryInputs));
    if (entryStats.is_err()) {
        stats.failed = true;
        return stats;
    }
    stats.entries = resourceList.size();

    // 重锚后的活跃条目 id 索引（类事实换锚用）。
    auto relisted = db.listEntries(inputId);
    if (relisted.is_err()) {
        stats.failed = true;
        return stats;
    }
    std::unordered_map<std::string, ca::i64> entryIdByPath;
    for (const mjbase::project::DbEntryInfo& row : std::move(relisted).unwrap()) {
        if (!row.tombstoned) entryIdByPath.emplace(row.path.to_std_string(), row.entryId);
    }

    // 受影响 uri 去序折叠 + 受影响类引用重扫（只扫这些条目）。
    std::vector<ca::str::Utf8String> uris;
    uris.reserve(changedUris.size());
    std::unordered_set<std::string_view> seen;
    for (const ca::str::Utf8String& uri : changedUris) {
        if (seen.emplace(reinterpret_cast<const char*>(uri.data()), uri.byte_length()).second)
            uris.push_back(uri.clone());
    }
    ReferenceScan scan;
    scan.buildFor(container, uris);

    // 类事实：classInfo(uri) 现算（workspace 同一事实源，与导入桥构造同形），
    // 单批 upsert（新类/新成员随批登记，为引用切片提供稳定 id）。
    struct ClassPlan
    {
        ca::str::Utf8String           internalName;
        std::vector<MethodKey> methods;   ///< 现方法集（name, descriptor）
        ca::usize                     members = 0;
    };
    std::vector<mjbase::project::DbClassInput> classInputs;
    std::vector<ClassPlan>                      plans;
    for (const ca::str::Utf8String& uri : uris) {
        auto info = workspace.classInfo(uri.ref());
        if (info.is_err()) {
            // 条目已非 class（重汇编成别的形态）或解码失败：条目锚已同步，
            // 类事实交由可见域遮蔽/开期导入兜底。
            ++stats.skipped;
            continue;
        }
        ClassInfo clazz = std::move(info).unwrap();
        mjbase::project::DbClassInput input;
        input.internalName = clazz.internalName.clone();
        input.superName    = clazz.superName.clone();
        input.access       = clazz.access;
        input.version      = clazz.version;
        const auto entry   = entryIdByPath.find(uri.to_std_string());
        input.entryId      = entry != entryIdByPath.end() ? entry->second : 0;
        input.interfaces.reserve(clazz.interfaces.size());
        for (const ca::str::Utf8String& interfaceName : clazz.interfaces) {
            input.interfaces.push_back(interfaceName.clone());
        }
        input.members.reserve(clazz.fields.size() + clazz.methods.size());
        for (const FieldInfo& field : clazz.fields) {
            mjbase::project::DbMemberInput member;
            member.kind       = mjbase::project::DbMemberKind::Field;
            member.name       = field.name.clone();
            member.descriptor = field.descriptor.clone();
            member.signature  = field.signature.clone();
            member.access     = field.access;
            input.members.push_back(std::move(member));
        }
        ClassPlan plan;
        plan.internalName = clazz.internalName.clone();
        for (const MethodInfo& method : clazz.methods) {
            mjbase::project::DbMemberInput member;
            member.kind       = mjbase::project::DbMemberKind::Method;
            member.name       = method.name.clone();
            member.descriptor = method.descriptor.clone();
            member.signature  = method.signature.clone();
            member.access     = method.access;
            input.members.push_back(std::move(member));
            plan.methods.emplace_back(method.name.to_std_string(),
                                      method.descriptor.to_std_string());
        }
        plan.members = input.members.size();
        classInputs.push_back(std::move(input));
        plans.push_back(std::move(plan));
    }
    if (!classInputs.empty()) {
        auto ingested = db.ingestClasses(0, std::move(classInputs));
        if (ingested.is_err()) {
            stats.failed = true;
            return stats;
        }
        for (const ClassPlan& plan : plans) {
            ++stats.classes;
            stats.members += plan.members;
        }
    }

    // 引用切片：逐类换新（类头 + 逐方法——现存方法即使零引用也写空切片，
    // 清掉编辑前的旧行），库内有而 DOM 现方法集没有的方法清切片。
    for (const ClassPlan& plan : plans) {
        const std::string owner = plan.internalName.to_std_string();
        auto found = db.findClass(utf8RefOf(owner));
        if (found.is_err()) {
            stats.failed = true;
            return stats;
        }
        auto classRow = std::move(found).unwrap();
        if (!classRow.has_value()) {
            // upsert 刚写即 miss 属库态异常：防御性跳过，开期导入兜底。
            stats.skipped += plan.members;
            continue;
        }
        const ca::i64 classId = classRow->classId;

        std::vector<mjbase::project::DbReferenceRecord> header;
        auto                                           slices = collectMethodSlices(scan, owner, header);
        auto replacedHeader = db.replaceReferencesFromClass(0, classId, std::move(header));
        if (replacedHeader.is_err()) {
            stats.failed = true;
            return stats;
        }
        stats.references += replacedHeader.unwrap();

        auto members = db.listMembers(utf8RefOf(owner));
        if (members.is_err()) {
            stats.failed = true;
            return stats;
        }
        std::map<MethodKey, ca::i64> methodIds;
        for (const mjbase::project::DbMemberInfo& row : std::move(members).unwrap()) {
            if (row.kind != mjbase::project::DbMemberKind::Method) continue;
            methodIds[{row.name.to_std_string(), row.descriptor.to_std_string()}] = row.memberId;
        }
        for (const MethodKey& key : plan.methods) {
            const auto id = methodIds.find(key);
            if (id == methodIds.end()) {
                const auto slice = slices.find(key);
                stats.skipped += slice == slices.end() ? 0 : slice->second.size();
                continue;
            }
            std::vector<mjbase::project::DbReferenceRecord> records;
            const auto slice = slices.find(key);
            if (slice != slices.end()) records = std::move(slice->second);
            auto replaced = db.replaceReferencesFromMember(0, id->second, std::move(records));
            if (replaced.is_err()) {
                stats.failed = true;
                return stats;
            }
            stats.references += replaced.unwrap();
        }
        for (const auto& [key, memberId] : methodIds) {
            const auto present = std::find(plan.methods.begin(), plan.methods.end(), key);
            if (present != plan.methods.end()) continue;
            auto cleared = db.replaceReferencesFromMember(0, memberId, {});
            if (cleared.is_err()) {
                stats.failed = true;
                return stats;
            }
            ++stats.slicesCleared;
        }
    }
    return stats;
}

}   // namespace mjt::workspace
