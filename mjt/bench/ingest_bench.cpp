/// @file       ingest_bench.cpp
/// @brief      analysis-db 整库导入基准（B6，#121）：分相计时 + 热查询电池。
///
/// 用法：`xmake run mjt_ingest_bench -- <container> [db-path]`。container 是
/// workspace 可开的 jar；db 省略时取 `<container>.bench.mjdb`，已存在则先删。
///
/// 分相计时复刻 analysis_ingest.cpp 的公开 API 流水（条目身份 → 类事实 →
/// 直接引用），落库状态与桥完全一致；总相为各相之和。
/// 热查询电池在导入完成后同一连接上跑：6 个索引查询 × N 轮取均值，
/// 全库级递归查询（allSubclasses(Object)）单跑一次报总量。
/// 输出全部走 stdout `[bench] name = value unit` 行，便于脚本抓取。

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "libca/crypto/sha256.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "workspace/reference_scan.hpp"
#include "workspace/workspace.hpp"

namespace {

using Clock = std::chrono::steady_clock;

/// @brief 简单计时器：构造起走表，ms() / us() 取已耗时长。
struct Stopwatch
{
    Clock::time_point start = Clock::now();

    double ms() const
    {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }
    double us() const
    {
        return std::chrono::duration<double, std::micro>(Clock::now() - start).count();
    }
};

void die(const std::string& what)
{
    std::fprintf(stderr, "bench fatal: %s\n", what.c_str());
    std::exit(1);
}

using ca::str::Utf8String;

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

Utf8String entryKind(const std::string& type)
{
    if (type == "class") return Utf8String::from_cstr("class");
    if (type == "manifest") return Utf8String::from_cstr("manifest");
    return Utf8String::from_cstr("resource");
}

/// @brief 报告行：`[bench] <name>=<value><unit>  (<note>)`。
void report(const std::string& name, double value, const char* unit,
            const std::string& note = {})
{
    std::printf("[bench] %-24s = %10.1f %-3s", name.c_str(), value, unit);
    if (!note.empty()) std::printf("  (%s)", note.c_str());
    std::printf("\n");
}

}   // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: mjt_ingest_bench <container.jar> [db.mjdb]\n");
        return 2;
    }
    const std::string containerPath = argv[1];
    std::string       dbPath        = containerPath + ".bench.mjdb";
    if (argc >= 3) dbPath = argv[2];
    std::error_code ec;
    std::filesystem::remove(dbPath, ec);

    //-- 打开容器 ----------------------------------------------------------

    Stopwatch openWatch;
    mjt::workspace::WorkspaceBase workspace;
    if (workspace.openJar(containerPath).is_err()) die("openJar failed: " + containerPath);
    const double openMs = openWatch.ms();

    //-- 相 1：条目遍历 + 内容锚（含容器 canonical 序列化） ------------------

    Stopwatch entryWatch;
    auto resources = workspace.listResources(true);
    if (resources.is_err()) die("listResources failed");
    const auto resourceList = std::move(resources).unwrap();

    std::vector<mjbase::project::DbEntryInput> entryInputs;
    entryInputs.reserve(resourceList.size());
    ca::crypto::SHA256 containerHasher;
    ca::usize          canonicalSize = 0;
    const auto         feedU64       = [&containerHasher](ca::u64 value) {
        ca::u8 raw[8];
        for (ca::usize i = 0; i < 8; ++i) {
            raw[i] = static_cast<ca::u8>((value >> (8 * i)) & 0xff);
        }
        containerHasher.add(raw, sizeof(raw));
    };
    ca::usize containerBytes = 0;
    for (const mjt::workspace::ResourceSummary& resource : resourceList) {
        auto content = workspace.readResource(resource.uri);
        if (content.is_err()) die("readResource failed: " + resource.uri);
        auto bytes = std::move(content).unwrap().bytes;
        containerBytes += bytes.size();
        feedU64(resource.uri.size());
        containerHasher.add(resource.uri.data(), resource.uri.size());
        feedU64(bytes.size());
        containerHasher.add(bytes.data(), bytes.size());
        canonicalSize += 2 * sizeof(ca::u64) + resource.uri.size() + bytes.size();

        mjbase::project::DbEntryInput input;
        input.path    = utf8(resource.uri);
        input.kind    = entryKind(resource.type);
        input.size    = static_cast<ca::i64>(bytes.size());
        input.sha256 =
            utf8(ca::crypto::sha256_hex(ca::core::ByteSlice(bytes.data(), bytes.size())));
        input.regular = true;
        entryInputs.push_back(std::move(input));
    }
    const double entryScanMs = entryWatch.ms();

    //-- 相 2：输入/条目登记 ------------------------------------------------

    auto dbResult = mjbase::project::AnalysisDb::open(
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(dbPath.data()),
                                          dbPath.size()));
    if (dbResult.is_err()) die("AnalysisDb::open failed");
    auto db = std::move(dbResult).unwrap();

    Stopwatch ingestEntryWatch;
    auto      inputId = db.ensureInput(utf8Ref(workspace.sourcePath()),
                                       utf8Ref(containerHasher.get_hash()),
                                       static_cast<ca::i64>(canonicalSize));
    if (inputId.is_err()) die("ensureInput failed");
    auto entryStats = db.ingestEntries(inputId.unwrap(), std::move(entryInputs));
    if (entryStats.is_err()) die("ingestEntries failed");
    const double ingestEntryMs = ingestEntryWatch.ms();
    const auto   entryCounts   = entryStats.unwrap();

    //-- 相 3：类事实 -------------------------------------------------------

    Stopwatch classScanWatch;
    auto      classInfos = workspace.listClasses();
    if (classInfos.is_err()) die("listClasses failed");
    const auto classList = std::move(classInfos).unwrap();

    auto listedEntries = db.listEntries(inputId.unwrap());
    if (listedEntries.is_err()) die("listEntries failed");
    auto entryRows = std::move(listedEntries).unwrap();
    std::unordered_map<std::string, ca::i64> entryIdByPath;
    for (const mjbase::project::DbEntryInfo& row : entryRows) {
        if (!row.tombstoned) entryIdByPath.emplace(row.path.to_std_string(), row.entryId);
    }

    std::unordered_set<std::string> primaryClasses;
    std::vector<mjbase::project::DbClassInput> classes;
    classes.reserve(classList.size());
    ca::usize memberCount = 0;
    for (const mjt::workspace::ClassInfo& info : classList) {
        mjbase::project::DbClassInput input;
        input.internalName = utf8(info.internalName);
        input.superName    = utf8(info.superName);
        input.access       = info.access;
        input.version      = info.version;
        const auto entry   = entryIdByPath.find(info.resourceUri);
        input.entryId      = entry != entryIdByPath.end() ? entry->second : 0;
        input.interfaces.reserve(info.interfaces.size());
        for (const std::string& name : info.interfaces) input.interfaces.push_back(utf8(name));
        input.members.reserve(info.fields.size() + info.methods.size());
        for (const mjt::workspace::FieldInfo& field : info.fields) {
            mjbase::project::DbMemberInput member;
            member.kind       = mjbase::project::DbMemberKind::Field;
            member.name       = utf8(field.name);
            member.descriptor = utf8(field.descriptor);
            member.access     = field.access;
            input.members.push_back(std::move(member));
        }
        for (const mjt::workspace::MethodInfo& method : info.methods) {
            mjbase::project::DbMemberInput member;
            member.kind       = mjbase::project::DbMemberKind::Method;
            member.name       = utf8(method.name);
            member.descriptor = utf8(method.descriptor);
            member.access     = method.access;
            input.members.push_back(std::move(member));
        }
        memberCount += input.members.size();
        primaryClasses.insert(info.internalName);
        classes.push_back(std::move(input));
    }
    const double classScanMs = classScanWatch.ms();

    Stopwatch ingestClassWatch;
    auto      classIngest = db.ingestClasses(std::move(classes));
    if (classIngest.is_err()) {
        die("ingestClasses failed: " +
            std::move(classIngest).unwrap_err().message.to_std_string());
    }
    const double ingestClassMs = ingestClassWatch.ms();

    //-- 相 4：直接引用 -----------------------------------------------------

    Stopwatch refScanWatch;
    auto      scan = workspace.referenceScan();
    if (scan.is_err()) die("referenceScan failed");
    // ReferenceScan 持 Utf8String 域切片（禁拷贝）：unwrap 走移动重载。
    const auto references = std::move(scan).unwrap();

    std::vector<mjbase::project::DbReferenceRecord> records;
    records.reserve(references.methodReferences().size() +
                    references.fieldReferences().size() +
                    references.typeReferences().size());
    const auto emitSite = [&records](mjbase::project::DbReferenceRecord record) {
        records.push_back(std::move(record));
    };
    for (const mjt::workspace::MethodReference& ref : references.methodReferences()) {
        if (primaryClasses.find(ref.site.sourceClass.to_std_string()) == primaryClasses.end())
            continue;
        mjbase::project::DbReferenceRecord record;
        record.sourceClass      = utf8(ref.site.sourceClass.ref());
        record.sourceMethod     = utf8(ref.site.sourceMethod.ref());
        record.sourceDescriptor = utf8(ref.site.sourceDescriptor.ref());
        record.insnIndex        = ref.site.insnIndex;
        record.kind             = mjbase::project::DbReferenceKind::Method;
        record.targetClass      = utf8(ref.target.owner.ref());
        record.targetName       = utf8(ref.target.name.ref());
        record.targetDescriptor = utf8(ref.target.descriptor.ref());
        emitSite(std::move(record));
    }
    for (const mjt::workspace::FieldReference& ref : references.fieldReferences()) {
        if (primaryClasses.find(ref.site.sourceClass.to_std_string()) == primaryClasses.end())
            continue;
        mjbase::project::DbReferenceRecord record;
        record.sourceClass      = utf8(ref.site.sourceClass.ref());
        record.sourceMethod     = utf8(ref.site.sourceMethod.ref());
        record.sourceDescriptor = utf8(ref.site.sourceDescriptor.ref());
        record.insnIndex        = ref.site.insnIndex;
        record.kind             = mjbase::project::DbReferenceKind::Field;
        record.targetClass      = utf8(ref.target.owner.ref());
        record.targetName       = utf8(ref.target.name.ref());
        record.targetDescriptor = utf8(ref.target.descriptor.ref());
        emitSite(std::move(record));
    }
    for (const mjt::workspace::TypeReference& ref : references.typeReferences()) {
        if (primaryClasses.find(ref.site.sourceClass.to_std_string()) == primaryClasses.end())
            continue;
        mjbase::project::DbReferenceRecord record;
        record.sourceClass      = utf8(ref.site.sourceClass.ref());
        record.sourceMethod     = utf8(ref.site.sourceMethod.ref());
        record.sourceDescriptor = utf8(ref.site.sourceDescriptor.ref());
        record.insnIndex        = ref.site.insnIndex;
        record.kind             = mjbase::project::DbReferenceKind::Type;
        record.targetClass      = utf8(ref.targetType.ref());
        emitSite(std::move(record));
    }
    const double       refScanMs  = refScanWatch.ms();
    const ca::usize    refCount   = records.size();

    Stopwatch ingestRefWatch;
    if (db.replaceReferences(std::move(records)).is_err()) die("replaceReferences failed");
    const double ingestRefMs = ingestRefWatch.ms();

    //-- 报告：导入相 -------------------------------------------------------

    std::printf("[bench] container = %s\n", containerPath.c_str());
    report("classes", static_cast<double>(classList.size()), "", "registered");
    report("members", static_cast<double>(memberCount), "", "registered");
    report("references", static_cast<double>(refCount), "", "rows");
    report("entries", static_cast<double>(entryCounts.total), "", "rows");
    report("container_bytes", static_cast<double>(containerBytes), "B");
    report("open_jar", openMs, "ms");
    report("entry_scan", entryScanMs, "ms", "listResources+sha256");
    report("entry_ingest", ingestEntryMs, "ms", "ensureInput+ingestEntries");
    report("class_scan", classScanMs, "ms", "listClasses+build");
    report("class_ingest", ingestClassMs, "ms", "ingestClasses txn");
    report("ref_scan", refScanMs, "ms", "referenceScan+build");
    report("ref_ingest", ingestRefMs, "ms", "replaceReferences txn");
    const double totalMs = openMs + entryScanMs + ingestEntryMs + classScanMs +
                           ingestClassMs + refScanMs + ingestRefMs;
    report("ingest_total", totalMs, "ms");
    if (totalMs > 0 && classList.size() > 0) {
        report("throughput_class_s", static_cast<double>(classList.size()) / (totalMs / 1000.0),
               "c/s");
    }

    //-- 热查询电池（同连接，索引路径） --------------------------------------

    const ca::str::Utf8String strClass    = Utf8String::from_cstr("java/lang/String");
    const ca::str::Utf8String objClass    = Utf8String::from_cstr("java/lang/Object");
    const ca::str::Utf8String hashMap     = Utf8String::from_cstr("java/util/HashMap");
    const ca::str::Utf8String collection  = Utf8String::from_cstr("java/util/Collection");

    double      qStrLenUs = 0, qObjClassUs = 0, qSuperUs = 0, qImplUs = 0, qFromUs = 0, qCountUs = 0;
    ca::usize   nStrLen = 0, nObjClass = 0, nSuper = 0, nImpl = 0, nFrom = 0;
    const int   kRounds = 50;
    for (int round = 0; round < kRounds; ++round) {
        {
            Stopwatch w;
            auto      r = db.referencesToMethod(strClass, Utf8String::from_cstr("length"),
                                                Utf8String::from_cstr("()I"));
            if (r.is_err()) die("referencesToMethod failed");
            nStrLen = std::move(r).unwrap().size();
            qStrLenUs += w.us();
        }
        {
            Stopwatch w;
            auto      r = db.referencesToClass(objClass);
            if (r.is_err()) die("referencesToClass failed");
            nObjClass = std::move(r).unwrap().size();
            qObjClassUs += w.us();
        }
        {
            Stopwatch w;
            auto      r = db.superChain(hashMap);
            if (r.is_err()) die("superChain failed");
            nSuper = std::move(r).unwrap().size();
            qSuperUs += w.us();
        }
        {
            Stopwatch w;
            auto      r = db.implementors(collection);
            if (r.is_err()) die("implementors failed");
            nImpl = std::move(r).unwrap().size();
            qImplUs += w.us();
        }
        {
            Stopwatch w;
            auto      r = db.referencesFromClass(strClass);
            if (r.is_err()) die("referencesFromClass failed");
            nFrom = std::move(r).unwrap().size();
            qFromUs += w.us();
        }
        {
            Stopwatch w;
            auto      r = db.referenceCount();
            if (r.is_err()) die("referenceCount failed");
            qCountUs += w.us();
        }
    }
    report("q_ref_to_method", qStrLenUs / kRounds, "us", "avg; java/lang/String.length ()I");
    report("q_ref_to_class", qObjClassUs / kRounds, "us", "avg; java/lang/Object");
    report("q_super_chain", qSuperUs / kRounds, "us", "avg; java/util/HashMap");
    report("q_implementors", qImplUs / kRounds, "us", "avg; java/util/Collection");
    report("q_refs_from_class", qFromUs / kRounds, "us", "avg; java/lang/String");
    report("q_ref_count", qCountUs / kRounds, "us", "avg");
    std::printf("[bench]   rows: str.length=%zu objClass=%zu superChain=%zu impl=%zu "
                "fromString=%zu\n",
                static_cast<size_t>(nStrLen), static_cast<size_t>(nObjClass),
                static_cast<size_t>(nSuper), static_cast<size_t>(nImpl),
                static_cast<size_t>(nFrom));

    //-- 全库级递归查询（单跑，报总量） --------------------------------------

    Stopwatch allWatch;
    auto      all = db.allSubclasses(objClass);
    if (all.is_err()) die("allSubclasses failed");
    // 左值 unwrap() 按值返回，vector<Utf8String> 拷不动——走 rvalue 重载。
    const auto allSubs = std::move(all).unwrap();
    report("q_all_subclasses_obj", allWatch.ms(), "ms", "full hierarchy walk");
    std::printf("[bench]   rows: %zu\n", static_cast<size_t>(allSubs.size()));

    //-- 库体积 -------------------------------------------------------------

    auto size = std::filesystem::file_size(dbPath, ec);
    if (!ec) report("db_size", static_cast<double>(size), "B", dbPath.c_str());

    if (db.close().is_err()) die("close failed");
    return 0;
}
