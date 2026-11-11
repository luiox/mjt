#include "workspace.hpp"
#include "search_match.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <tuple>
#include <type_traits>
#include <unordered_set>

#include "jbc/core/attr_codec.hpp"
#include "jbc/core/insn.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/core/reader.hpp"
#include "jbc/ir/units.hpp"

#include "mjbase/hierarchy/class_hierarchy.hpp"
#include "mjbase/hierarchy/summary.hpp"
#include "mjbase/analysis/class_scan.hpp"
#include "mjbase/analysis/lvt_display_policy.hpp"
#include "mjbase/asm/remapper/member_remap_plan.hpp"
#include "mjbase/asm/remapper/resource_remapper.hpp"
#include "mjbase/jar/jar_reader.hpp"
#include "mjbase/jar/jar_util.hpp"
#include "mjbase/jasm/classfile_utf8.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/jasm/resource_jasm_service.hpp"
#include "mjbase/keys.hpp"
#include "mjbase/mapping/identifier_util.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "mjbase/project/edit_ledger.hpp"
#include "mjbase/project/edit_ledger_store.hpp"
#include "mjbase/project/project_store.hpp"
#include "mjbase/spi/class_file_sanitize.hpp"
#include "mjbase/spi/common_super_resolver.hpp"
#include "mjbase/spi/job_context.hpp"
#include "mjbase/spi/pass_manager.hpp"
#include "mjbase/spi/pass_name_registry.hpp"
#include "mjbase/spi/pass_pipeline.hpp"
#include "mjbase/spi/pass_registry.hpp"
#include "mjbase/spi/pipeline_builder.hpp"
#include "mjbase/spi/strict_type_resolver.hpp"
#include "jzip/decompressor.hpp"
#include "jzip/zip_io.hpp"
#include "libca/zip/checksum.hpp"
#include "analysis_edit.hpp"
#include "container_io.hpp"
#include "edit_support.hpp"
#include "jar_ops.hpp"
#include "analysis_ops.hpp"
#include "analysis_edit_sync.hpp"
#include "constant_scan.hpp"
#include "search_match.hpp"
#include "export_manifest.hpp"
#include "class_mutation_service.hpp"
#include "classfile_support.hpp"
#include "reference_scan.hpp"
#include "summary_scan.hpp"
#include "dex_domain.hpp"

namespace mjt::workspace {

namespace {

using support::utf8Text;

// ── 基础助手 ─────────────────────────────────────────────────────

/// @brief 资源类型短名（小写口径："class"/"manifest"/"jar"/"resource"）。

// ── class 解码（libjbc 会话；字段口径照旧）──────────────────────────────


/// @brief Code 嵌套 LNT 行 → {line, 代码指令序号}；pc 无对应指令时 index 为 -1。


/// @brief 把 LVT 显示名策略应用到查询输出的 locals（B3，#121 §4.4）。
///
/// 只改呈现名：行数、槽位、描述符原样——LVT 原文在字节码里照存，判定
/// 结果也不入库（v1 决策）。呈现核心在 mjbase::analysis（纯函数面）。


/// @brief InnerClasses 属性的内部类名（去重排序；inner_name_index=0 的匿名形态跳过）。
///
/// typed InnerClassesAttr 体经公开解码入口读出（B3 分派接线前后同形，不触碰
/// 槽状态）；解码失败的槽跳过（查询面对畸形输入宽容）。


// ── 搜索 ────────────────────────────────────────────────────────

// 搜索匹配原语收口在共享层（search_match：lowercase/matchesText/
// validateSearchOptions/compileSearchRegex/matchesSearchScope/
// validateNumberQuery——引擎活扫描与 MCP/pysdk 的 analysis.db 直查路径
// 共用，regex 档实现单源）。

// ── 指令渲染（libjbc 形状分派；node_type 按指令语义命名）────────────────

/// @brief libjbc 无 ASM 节点类型 → 按指令语义命名（invoke/field/ldc/cp_ref/
///        jump/var/const/array/iinc/switch 族）；标签/行号不是指令，操作数
///        与跳转以 jasm 全文为权威。旧「按操作数布局」命名把全部 CP 形态
///        （getstatic/invoke*/checkcast/new…）标成 ldc，搜索结果的类型标签
///        失去判别力，改为助记符归类。

/// @brief 成员引用 CP 索引 → "owner.name:desc"（operandComment 渲染口径）。
std::string memberRefColonText(const jbc::core::ClassFile& cf, ca::u32 cpIndex)
{
    if (!cf.cp.valid(cpIndex)) return {};
    const jbc::core::ConstantPool::Entry& entry = cf.cp.at(cpIndex);
    if (entry.tag != jbc::core::CpTag::Fieldref && entry.tag != jbc::core::CpTag::Methodref &&
        entry.tag != jbc::core::CpTag::InterfaceMethodref)
        return {};
    std::string owner;
    std::string name;
    std::string desc;
    if (cf.cp.valid(entry.a)) {
        const auto& classEntry = cf.cp.at(entry.a);
        if (classEntry.tag == jbc::core::CpTag::Class) owner = utf8Text(cf, classEntry.a);
    }
    if (cf.cp.valid(entry.b)) {
        const auto& nt = cf.cp.at(entry.b);
        if (nt.tag == jbc::core::CpTag::NameAndType) {
            name = utf8Text(cf, nt.a);
            desc = utf8Text(cf, nt.b);
        }
    }
    return owner + "." + name + ":" + desc;
}

/// @brief 成员引用 CP 索引 → 无冒号拼接（指令搜索文本口径：workspace/Subject.value()I）。
std::string memberRefFlatText(const jbc::core::ClassFile& cf, ca::u32 cpIndex)
{
    const std::string resolved = memberRefColonText(cf, cpIndex);
    if (resolved.empty()) return {};
    const auto colon = resolved.rfind(':');
    return resolved.substr(0, colon) + resolved.substr(colon + 1);
}

/// @brief LDC 常量与成员引用的可读注释（字符串加引号、数值无后缀、已过
///        IdentifierUtil 可读化——InstructionDetailInfo::operandComment 契约）。
std::string ldcComment(const jbc::core::ClassFile& cf, ca::u32 cpIndex)
{
    if (!cf.cp.valid(cpIndex)) return {};
    const jbc::core::ConstantPool::Entry& entry = cf.cp.at(cpIndex);
    char                                  tmp[64];
    switch (entry.tag) {
    case jbc::core::CpTag::String:
        return "\"" +
               mjbase::mapping::IdentifierUtil::printReadable(utf8Text(cf, entry.a)) + "\"";
    case jbc::core::CpTag::Class:
        return mjbase::mapping::IdentifierUtil::printReadable(utf8Text(cf, entry.a));
    case jbc::core::CpTag::Integer:
        std::snprintf(tmp, sizeof(tmp), "%d", entry.ival);
        return tmp;
    case jbc::core::CpTag::Float:
        std::snprintf(tmp, sizeof(tmp), "%g",
                      static_cast<double>(support::bitsToFloat(static_cast<ca::u32>(entry.ival))));
        return tmp;
    case jbc::core::CpTag::Long:
        std::snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(entry.lval));
        return tmp;
    case jbc::core::CpTag::Double:
        std::snprintf(tmp, sizeof(tmp), "%g",
                      support::bitsToDouble(static_cast<ca::u64>(entry.lval)));
        return tmp;
    case jbc::core::CpTag::Fieldref:
    case jbc::core::CpTag::Methodref:
    case jbc::core::CpTag::InterfaceMethodref:
    {
        // owner.name:desc 各段过 IdentifierUtil 可读化。
        std::string owner;
        std::string name;
        std::string desc;
        if (cf.cp.valid(entry.a)) {
            const auto& classEntry = cf.cp.at(entry.a);
            if (classEntry.tag == jbc::core::CpTag::Class)
                owner = mjbase::mapping::IdentifierUtil::printReadable(utf8Text(cf, classEntry.a));
        }
        if (cf.cp.valid(entry.b)) {
            const auto& nt = cf.cp.at(entry.b);
            if (nt.tag == jbc::core::CpTag::NameAndType) {
                name = mjbase::mapping::IdentifierUtil::printReadable(utf8Text(cf, nt.a));
                desc = mjbase::mapping::IdentifierUtil::printReadable(utf8Text(cf, nt.b));
            }
        }
        return owner + "." + name + ":" + desc;
    }
    default: return {};
    }
}

/// @brief 一条指令的完整渲染（保留 CP 索引与字节 PC；switch 给绝对目标 PC）。
InstructionDetailInfo renderInstruction(const jbc::core::ClassFile& cf,
                                        const jbc::core::RawInsn&   insn)
{
    InstructionDetailInfo info;
    info.pc     = static_cast<ca::i32>(insn.pc);
    info.opcode = static_cast<ca::i32>(insn.op);
    const char* mnemonic = jbc::core::mnemonicOf(insn.op);
    info.mnemonic        = ca::str::Utf8String::from_cstr(mnemonic != nullptr ? mnemonic : "?");
    char tmp[64];
    switch (jbc::core::shapeOf(insn.op)) {
    case jbc::core::Shape::CP1:
    case jbc::core::Shape::CP2:
        std::snprintf(tmp, sizeof(tmp), "#%u", insn.cp);
        info.operandText = ca::str::Utf8String::from_cstr(tmp);
        info.cpIndex     = static_cast<ca::i32>(insn.cp);
        break;
    case jbc::core::Shape::Imm1:
    case jbc::core::Shape::Imm2:
    case jbc::core::Shape::Atype1:
    case jbc::core::Shape::Local1:
        std::snprintf(tmp, sizeof(tmp), "%d", insn.a);
        info.operandText = ca::str::Utf8String::from_cstr(tmp);
        break;
    case jbc::core::Shape::Iinc1:
        std::snprintf(tmp, sizeof(tmp), "%d, %d", insn.a, insn.b);
        info.operandText = ca::str::Utf8String::from_cstr(tmp);
        break;
    case jbc::core::Shape::CP2Dims:
        std::snprintf(tmp, sizeof(tmp), "#%u dims=%d", insn.cp, insn.a);
        info.operandText = ca::str::Utf8String::from_cstr(tmp);
        info.cpIndex     = static_cast<ca::i32>(insn.cp);
        break;
    case jbc::core::Shape::CP2Count:
    case jbc::core::Shape::CP2Zero2:
        std::snprintf(tmp, sizeof(tmp), "#%u", insn.cp);
        info.operandText = ca::str::Utf8String::from_cstr(tmp);
        info.cpIndex     = static_cast<ca::i32>(insn.cp);
        break;
    case jbc::core::Shape::TableSwitch:
    {
        info.isSwitch        = true;
        info.switchDefaultPc = static_cast<ca::i32>(insn.pc + static_cast<ca::u32>(insn.t));
        for (ca::usize k = 0; k < insn.offs.size(); ++k) {
            info.switchCases.push_back(
                {insn.a + static_cast<ca::i32>(k),
                 static_cast<ca::i32>(insn.pc + static_cast<ca::u32>(insn.offs[k]))});
        }
        break;
    }
    case jbc::core::Shape::LookupSwitch:
    {
        info.isSwitch        = true;
        info.switchDefaultPc = static_cast<ca::i32>(insn.pc + static_cast<ca::u32>(insn.t));
        for (const auto& kase : insn.cases) {
            info.switchCases.push_back(
                {kase.key, static_cast<ca::i32>(insn.pc + static_cast<ca::u32>(kase.off))});
        }
        break;
    }
    case jbc::core::Shape::Branch2:
    case jbc::core::Shape::Branch4:
        info.branchTargetPc = static_cast<ca::i32>(insn.pc + static_cast<ca::u32>(insn.t));
        info.operandText = utf8(std::to_string(info.branchTargetPc));
        break;
    case jbc::core::Shape::None: break;
    }
    return info;
}

// ── 字节 diff / 修复 / 指纹 ──────────────────────────────────────

// 内名推导（模型层族视图原语，Multi-Release 版本化 URI 剥离口径见
// model/resource_container.hpp）：本文件不再持有副本。

// ── 指纹（fnv1a64:16hex；长度按小端逐字节入串）─────────────────


WorkspaceResult<std::string> normalizedRegularFile(const std::string& path,
                                                   const std::string& operation)
{
    namespace fs = std::filesystem;
    if (path.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, operation, "path is empty"));
    std::error_code filesystemError;
    const auto      absolute = fs::absolute(fsPath(path), filesystemError).lexically_normal();
    if (filesystemError)
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, operation, filesystemError.message()));
    const auto status = fs::status(absolute, filesystemError);
    if (filesystemError || !fs::exists(status))
        return ca::core::Err(error(WorkspaceError::NotFound, operation, pathToUtf8(absolute)));
    if (!fs::is_regular_file(status))
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, operation, "path is not a regular file"));
    return ca::core::Ok(pathToUtf8(absolute));
}


template <typename T>
void sortUnique(std::vector<T>& values)
{
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
}

}   // namespace

void sortAndDeduplicateSearchHits(std::vector<SearchHit>& hits)
{
    std::sort(hits.begin(), hits.end(), [](const SearchHit& left, const SearchHit& right) {
        return std::tie(left.resourceUri,
                        left.owner,
                        left.name,
                        left.descriptor,
                        left.instructionIndex,
                        left.kind,
                        left.value) <
               std::tie(right.resourceUri,
                        right.owner,
                        right.name,
                        right.descriptor,
                        right.instructionIndex,
                        right.kind,
                        right.value);
    });
    hits.erase(
        std::unique(hits.begin(), hits.end(), [](const SearchHit& left, const SearchHit& right) {
            return left.resourceUri == right.resourceUri && left.kind == right.kind &&
                   left.owner == right.owner && left.name == right.name &&
                   left.descriptor == right.descriptor &&
                   left.instructionIndex == right.instructionIndex && left.value == right.value;
        }),
        hits.end());
}

// ── 错误码短名 ──────────────────────────────────────────────────────────

const char* to_string(WorkspaceError errorCode) noexcept
{
    switch (errorCode) {
    case WorkspaceError::Ok: return "Ok";
    case WorkspaceError::InvalidArgument: return "InvalidArgument";
    case WorkspaceError::NotFound: return "NotFound";
    case WorkspaceError::AlreadyExists: return "AlreadyExists";
    case WorkspaceError::FailedPrecondition: return "FailedPrecondition";
    case WorkspaceError::PermissionDenied: return "PermissionDenied";
    case WorkspaceError::Aborted: return "Aborted";
    case WorkspaceError::DataLoss: return "DataLoss";
    case WorkspaceError::Internal: return "Internal";
    case WorkspaceError::Unimplemented: return "Unimplemented";
    }
    return "?";
}

// ── WorkspaceBase 内部结构 ──────────────────────────────────────────────

/// @brief 层次/覆盖分析图：KeyPool + ClassHierarchy + 每类声明摘要。
///
/// 未登记的超类型（java/lang/Object 等）物化为合成节点（无成员、access 0），
/// 使 superclass 链/接口闭包可统一遍历。
struct WorkspaceBase::AnalysisGraph
{
    std::shared_ptr<mjbase::KeyPool>           pool      = std::make_shared<mjbase::KeyPool>();
    std::unique_ptr<hierarchy::ClassHierarchy> hierarchy = nullptr;
    std::unordered_map<ca::u32, hierarchy::ClassSummary> summaries;   ///< key: ClassKey.id

    AnalysisGraph()
        : hierarchy(std::make_unique<hierarchy::ClassHierarchy>(*pool))
    {}

    bool declaresMethod(ca::u32 classKeyId, const std::string& name,
                        const std::string& descriptor) const
    {
        const auto found = summaries.find(classKeyId);
        if (found == summaries.end()) return false;
        for (const auto& method : found->second.methods) {
            if (pool->str(method.name) == name && pool->str(method.desc) == descriptor) return true;
        }
        return false;
    }
};

WorkspaceBase::WorkspaceBase() = default;
WorkspaceBase::~WorkspaceBase() = default;

// ── 打开 / supporting ───────────────────────────────────────────────────

/// @brief 源 jar MANIFEST 原始字节捕获装饰器（#74 导出保真）。
///
/// 包住本次打开的读取器，在同一个读取遍里把 META-INF/MANIFEST.MF 条目的
/// 原始字节旁路写入 @p sink（ManifestResource 解析物不含行尾/续行/尾随
/// 内容，导出逐字节复制需要源字节）。转发 read/readWithProgress 两个入口，
/// 进度变体也捕获（两个读取入口都可能被消费面走到）。捕获名 = 解码后
/// 规范名，与容器内 ManifestResource 的 URI 同口径。
WorkspaceResult<void> WorkspaceBase::open(ca::str::Utf8StringRef pathRef)
{
    const std::string path = pathRef.to_std_string();
    // 缺省空 decoder = 原始字节直通（#1110 族缺省零漂移，见 decoder 重载）。
    return open(utf8Ref(path), {});
}

WorkspaceResult<void> WorkspaceBase::open(
    ca::str::Utf8StringRef pathRef, const mjbase::jar::IJarReader::EntryNameDecoder& decoder)
{
    const std::string path   = pathRef.to_std_string();
    const auto        format = formatForPath(path);
    if (format == ContainerFormat::None)
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "open",
                                   "unsupported container extension: " + path));
    const ContainerIoEntry* entry = ContainerIoRegistry::instance().find(format);
    if (entry == nullptr || entry->import == nullptr)
        return ca::core::Err(
            error(WorkspaceError::Unimplemented, "open", "no importer for this format"));

    try {
        auto importedArtifact = entry->import(path, pathRef, decoder);
        if (importedArtifact.is_err())
            return ca::core::Err(std::move(importedArtifact).unwrap_err());
        auto artifact = std::move(importedArtifact).unwrap();

        // classfile 族导入行上抛打开期门禁作用域（沿旧 openClass 口径：
        // relaxed 单类门；容器型格式无打开期门禁——沿旧 openJar 口径）。
        if (!artifact.gateUris.empty()) {
            std::vector<jasm::Diagnostic> diagnostics;
            const WorkspaceErrorInfo      validation = validateCandidate(
                *artifact.working, diagnostics, jasm::ValidationMode::Relaxed, artifact.gateUris);
            if (validation.is_err()) {
                return ca::core::Err(error(WorkspaceError::DataLoss, "open",
                                           "class did not pass validation"));
            }
        }

        original_            = std::move(artifact.original);
        working_             = std::move(artifact.working);
        format_              = format;
        sourcePath_          = utf8(path);
        primaryClassUri_     = std::move(artifact.primaryClassUri);
        sourceManifestBytes_ = std::move(artifact.sourceManifestBytes);
        dexDomain_           = std::move(artifact.dexDomain);
        auto reset = resetSessionState("open");
        if (reset.is_err()) return reset;
        return ca::core::Ok();
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::DataLoss, "open", exception.what()));
    }
}

WorkspaceResult<void> WorkspaceBase::openEmpty()
{
    // 空容器出生（pysdk new_container 契约）：双空快照、无源路径、格式 None
    // ——会话自出生即 loaded（编辑面 ensureLoaded 直接可用），纯 add_entry
    // 填充；后续 open(path) 原样接管（自带全套重置）。
    try {
        original_            = std::make_unique<model::ResourceContainer>();
        working_             = std::make_unique<model::ResourceContainer>();
        format_              = ContainerFormat::None;
        sourcePath_          = ca::str::Utf8String{};
        primaryClassUri_     = ca::str::Utf8String{};
        sourceManifestBytes_ = std::nullopt;
        dexDomain_           = nullptr;
        auto reset = resetSessionState("open empty");
        if (reset.is_err()) return reset;
        return ca::core::Ok();
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::DataLoss, "open empty", exception.what()));
    }
}

WorkspaceResult<SupportingResourceInfo> WorkspaceBase::addSupportingRole(
    ca::str::Utf8StringRef pathRef, ca::str::Utf8StringRef roleLabelRef)
{
    const std::string path      = pathRef.to_std_string();
    const std::string roleLabel = roleLabelRef.to_std_string();
    WorkspaceErrorInfo loaded = ensureLoaded("add supporting JAR");
    if (loaded.is_err()) return ca::core::Err(std::move(loaded));
    auto normalized = normalizedRegularFile(path, "add supporting JAR");
    if (normalized.is_err()) return ca::core::Err(std::move(normalized).unwrap_err());
    const auto normalPath = std::move(normalized).unwrap();
    for (const auto& support : registry_->records()) {
        // 路径唯一约束跨角色生效：同一容器换角色挂载不追加第二条记录。
        if (support.path == normalPath) {
            return ca::core::Err(error(WorkspaceError::AlreadyExists, "add supporting JAR",
                                       "supporting path is already attached"));
        }
    }
    try {
        auto imported = importSupportingContainer(normalPath);
        if (imported.is_err()) return ca::core::Err(std::move(imported).unwrap_err());
        auto artifact = std::move(imported).unwrap();
        auto& container = artifact.working;
        auto fingerprint = fingerprintContainer(*container);
        if (fingerprint.is_err()) return ca::core::Err(std::move(fingerprint).unwrap_err());
        const auto fingerprintText = std::move(fingerprint).unwrap();
        for (const auto& support : registry_->records()) {
            if (support.fingerprint == fingerprintText) {
                return ca::core::Err(error(WorkspaceError::AlreadyExists, "add supporting JAR",
                                           "supporting content is already attached"));
            }
        }

        SupportingResourceInfo info;
        info.id            = utf8(registry_->nextSupportId());
        info.path          = utf8(normalPath);
        info.fingerprint   = utf8(fingerprintText);
        info.resourceCount = container->size();
        info.role          = utf8(roleLabel);
        container->visitResources([&](std::string_view, const model::IResource& resource) {
            if (resource.getType() == model::ResourceType::Class) ++info.classCount;
        });
        if (artifact.dexDomain) info.classCount = artifact.dexDomain->classCount();
        ContainerRecord record;
        record.id          = normalPath;
        record.supportId   = info.id.to_std_string();
        record.path        = info.path.to_std_string();
        record.fingerprint = info.fingerprint.to_std_string();
        record.role        = roleLabel == "runtime" ? model::ContainerRole::Runtime
                                                    : model::ContainerRole::Library;
        record.roleLabel   = roleLabel;
        record.container   = std::move(container);
        record.dexDomain   = std::move(artifact.dexDomain);
        registry_->records().push_back(std::move(record));
        return ca::core::Ok(std::move(info));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::DataLoss, "add supporting JAR", exception.what()));
    }
}

WorkspaceResult<SupportingResourceInfo> WorkspaceBase::removeSupporting(
    ca::str::Utf8StringRef idRef)
{
    const std::string id = idRef.to_std_string();
    WorkspaceErrorInfo loaded = ensureLoaded("remove supporting JAR");
    if (loaded.is_err()) return ca::core::Err(std::move(loaded));
    if (id.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "remove supporting JAR",
                                   "support ID is empty"));
    }
    const auto* found = registry_->findBySupportId(id);
    if (found == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "remove supporting JAR", id));

    // 一容器一记录：摘除即清理该容器的全部角色注册，无残留半边。
    SupportingResourceInfo info;
    info.id            = utf8(found->supportId);
    info.path          = utf8(found->path);
    info.fingerprint   = utf8(found->fingerprint);
    info.resourceCount = found->container->size();
    info.role          = utf8(found->roleLabel);
    found->container->visitResources([&](std::string_view, const model::IResource& resource) {
        if (resource.getType() == model::ResourceType::Class) ++info.classCount;
    });
    if (found->dexDomain) info.classCount = found->dexDomain->classCount();
    registry_->detachBySupportId(id);
    return ca::core::Ok(std::move(info));
}

// ── 状态面 ──────────────────────────────────────────────────────────────

bool WorkspaceBase::isLoaded() const noexcept
{
    return working_ != nullptr && original_ != nullptr;
}

const ca::str::Utf8String& WorkspaceBase::sourcePath() const noexcept
{
    return sourcePath_;
}

const ca::str::Utf8String& WorkspaceBase::primaryClassUri() const noexcept
{
    return primaryClassUri_;
}

ca::u64 WorkspaceBase::revision() const noexcept
{
    return revision_;
}

bool WorkspaceBase::lvtJarFallbackFlag() const
{
    if (lvtPolicyComputed_ && lvtPolicyRevision_ == revision_) return lvtJarFallback_;
    // 判定扫描沉 analysis_ops（无状态纯函数）；本成员只留 revision 键控缓存。
    lvtJarFallback_    = working_ != nullptr ? lvtJarFallbackOf(*working_) : false;
    lvtPolicyRevision_ = revision_;
    lvtPolicyComputed_ = true;
    return lvtJarFallback_;
}

bool WorkspaceBase::isDirty() const
{
    if (!isLoaded()) return false;
    return !diffContainers(*original_, *working_).empty();
}

bool WorkspaceBase::canExport() const noexcept
{
    return isLoaded();
}

std::vector<PassDescription> WorkspaceBase::availablePasses()
{
    std::vector<PassDescription> result;
    for (const auto& pass : spi::defaultPassRegistry().list()) {
        // scope 输出小写；mjbase 第四 scope Cf 无默认种子，
        // 仅为穷尽 switch 兜底。
        const std::string scope = pass.scope == spi::PassScope::Method   ? "method"
                                  : pass.scope == spi::PassScope::Class  ? "class"
                                  : pass.scope == spi::PassScope::Global ? "global"
                                                                         : "cf";
        result.push_back({utf8(pass.name), utf8(pass.description), utf8(scope)});
    }
    return result;
}

std::vector<PipelineDescription> WorkspaceBase::availablePipelines()
{
    spi::registerBuiltinPipelines();
    std::vector<PipelineDescription> result;
    //PassPipelineInfo 0.0.28 起携 Utf8String 成员（禁拷贝）：显式 clone 落 DTO。
    for (const auto& pipeline : spi::PassRegistry::instance().listPipelines())
        result.push_back({pipeline.name.clone(), pipeline.description.clone()});
    return result;
}

WorkspaceErrorInfo WorkspaceBase::ensureLoaded(const std::string& operation) const
{
    if (!isLoaded())
        return error(WorkspaceError::FailedPrecondition, operation, "workspace is not loaded");
    return WorkspaceErrorInfo{};
}

bool WorkspaceBase::accepts(const MutationRequest& request) const
{
    return request.expectedRevision == kAnyRevision || request.expectedRevision == revision_;
}

MutationResult WorkspaceBase::revisionConflict() const
{
    MutationResult result;
    result.status = error(WorkspaceError::Aborted, "mutate workspace",
                          "expected revision does not match current revision");
    result.revision   = revision_;
    result.exportable = canExport();
    return result;
}

MutationResult WorkspaceBase::stateFailure(WorkspaceErrorInfo status) const
{
    MutationResult result;
    result.status     = std::move(status);
    result.revision   = revision_;
    result.exportable = canExport();
    return result;
}

model::ResourceContainer* WorkspaceBase::workingContainer() noexcept
{
    return working_.get();
}

const model::ResourceContainer* WorkspaceBase::workingContainer() const noexcept
{
    return working_.get();
}

std::unique_ptr<model::ResourceContainer> WorkspaceBase::cloneWorking() const
{
    auto clone = cloneContainer(*working_);
    if (clone.is_err()) throw std::runtime_error(std::move(clone).unwrap_err().message);
    return std::move(clone).unwrap();
}

std::unique_ptr<model::ResourceContainer> WorkspaceBase::cloneOriginal() const
{
    auto clone = cloneContainer(*original_);
    if (clone.is_err()) throw std::runtime_error(std::move(clone).unwrap_err().message);
    return std::move(clone).unwrap();
}

std::unique_ptr<model::ResourceContainer> WorkspaceBase::analysisScanContainer() const
{
    auto merged = std::make_unique<model::ResourceContainer>();
    if (!dexDomain_) {
        working_->visitResources([&](std::string_view uri, const model::IResource& resource) {
            auto copied = cloneResource(resource);
            if (copied) merged->addResource(uri, std::move(copied));
        });
    }
    for (const auto& support : registry_->records()) {
        if (support.dexDomain) continue;
        support.container->visitResources(
            [&](std::string_view uri, const model::IResource& resource) {
                if (merged->getResource(uri) != nullptr) return;   // 主容器 URI 优先
                auto copied = cloneResource(resource);
                if (copied) merged->addResource(uri, std::move(copied));
            });
    }
    return merged;
}

std::unordered_map<std::string, std::string> WorkspaceBase::supportingClassIds() const
{
    if (registry_->records().empty()) return {};

    std::unordered_set<std::string> primary;
    if (dexDomain_) {
        const auto& names = dexDomain_->classNames();
        primary.insert(names.begin(), names.end());
    }
    else {
        working_->visitResources([&](std::string_view, const model::IResource& resource) {
            const auto* classResource =
                ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
            if (classResource == nullptr) return;
            const auto name = internalNameOfBytes(classResource->get());
            if (!name.empty()) primary.insert(name);
        });
    }

    std::unordered_map<std::string, std::string> result;
    for (const auto& support : registry_->records()) {
        if (support.dexDomain) {
            for (const auto& name : support.dexDomain->classNames()) {
                if (primary.count(name) == 0) result.emplace(name, support.supportId);
            }
            continue;
        }
        support.container->visitResources([&](std::string_view, const model::IResource& resource) {
            const auto* classResource =
                ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
            if (classResource == nullptr) return;
            const auto name = internalNameOfBytes(classResource->get());
            if (!name.empty() && primary.find(name) == primary.end())
                result.emplace(name, support.supportId);
        });
    }
    return result;
}

void WorkspaceBase::annotateSupportingSources(std::vector<ReferenceInfo>& references) const
{
    const auto supportingClasses = supportingClassIds();
    for (auto& reference : references) {
        const auto source = supportingClasses.find(reference.sourceClass.to_std_string());
        if (source == supportingClasses.end()) continue;
        reference.supporting = true;
        reference.supportId  = utf8(source->second);
    }
}

void WorkspaceBase::finalizeReferenceRows(std::vector<ReferenceInfo>& references) const
{
    annotateSupportingSources(references);
    sortReferences(references);
}

WorkspaceResult<WorkspaceBase::AnalysisGraph> WorkspaceBase::analysisGraph() const
{
    WorkspaceErrorInfo status = ensureLoaded("build analysis classpath");
    if (status.is_err()) return ca::core::Err(std::move(status));
    try {
        AnalysisGraph                        graph;
        std::vector<hierarchy::ClassSummary> pending;
        std::unordered_set<ca::u32>          registered;

        const auto appendClasses = [&](const model::ResourceContainer& source, bool primaryOwner) {
            source.visitResources([&](std::string_view, const model::IResource& resource) {
                const auto* classResource =
                    ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
                if (classResource == nullptr) return;
                auto session = sessionOf(classResource->get(), "build analysis classpath");
                if (session.is_err()) return;   // 畸形支撑类不供层次事实（照旧容忍）
                auto summary = hierarchy::fillSummary(*std::move(session).unwrap(), *graph.pool);
                const bool first = registered.insert(summary.self.id).second;
                if (primaryOwner || first)
                    pending.push_back(std::move(summary));
            });
        };
        const auto appendDexClasses = [&](const DexDomain& dex) {
            auto classes = dex.listClasses(false);
            if (classes.is_err())
                return std::move(classes).unwrap_err();
            for (const auto& cls : std::move(classes).unwrap()) {
                hierarchy::ClassSummary summary;
                summary.self = mjbase::classKeyOf(*graph.pool, cls.internalName.to_std_string());
                if (!registered.insert(summary.self.id).second) continue;
                if (!cls.superName.is_empty())
                    summary.super = mjbase::classKeyOf(*graph.pool, cls.superName.to_std_string());
                summary.access = cls.access;
                for (const auto& iface : cls.interfaces)
                    summary.interfaces.push_back(mjbase::classKeyOf(*graph.pool, iface.to_std_string()));
                const auto appendMembers = [&](const auto& members, auto& output) {
                    for (const auto& member : members) {
                        const auto access = static_cast<ca::u32>(member.access);
                        output.push_back({graph.pool->intern(member.name.to_std_string()),
                                          graph.pool->intern(member.descriptor.to_std_string()),
                                          (access & hierarchy::kAccStatic) != 0, access});
                    }
                };
                appendMembers(cls.methods, summary.methods);
                appendMembers(cls.fields, summary.fields);
                pending.push_back(std::move(summary));
            }
            return WorkspaceErrorInfo{};
        };
        if (dexDomain_) {
            auto appended = appendDexClasses(*dexDomain_);
            if (appended.is_err()) return ca::core::Err(std::move(appended));
        }
        else {
            appendClasses(*working_, true);
        }
        for (const auto& support : registry_->records()) {
            if (support.dexDomain) {
                auto appended = appendDexClasses(*support.dexDomain);
                if (appended.is_err()) return ca::core::Err(std::move(appended));
            }
            else {
                appendClasses(*support.container, false);
            }
        }

        // 未登记超类型物化为合成节点（java/lang/Object 等；无成员、access 0）。
        std::unordered_set<ca::u32> known;
        for (const auto& summary : pending) known.insert(summary.self.id);
        std::vector<hierarchy::ClassSummary> synthesized;
        const auto ensureNode = [&](ca::u32 keyId) {
            if (keyId == 0 || known.count(keyId) != 0) return;
            known.insert(keyId);
            hierarchy::ClassSummary stub;
            stub.self = mjbase::ClassKey{keyId};
            synthesized.push_back(std::move(stub));
        };
        for (const auto& summary : pending) {
            ensureNode(summary.super.id);
            for (const auto& interfaceKey : summary.interfaces) ensureNode(interfaceKey.id);
        }

        for (auto& summary : pending) {
            if (graph.hierarchy->addClass(summary).is_ok())
                graph.summaries.emplace(summary.self.id, std::move(summary));
        }
        for (auto& stub : synthesized) {
            if (graph.hierarchy->addClass(stub).is_ok())
                graph.summaries.emplace(stub.self.id, std::move(stub));
        }
        return ca::core::Ok(std::move(graph));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "build analysis classpath", exception.what()));
    }
}

// ── 资源 / class 读取面 ─────────────────────────────────────────────────


WorkspaceResult<FieldInfo> WorkspaceBase::fieldInfo(ca::str::Utf8StringRef ownerRef,
                                                    ca::str::Utf8StringRef nameRef,
                                                    ca::str::Utf8StringRef descriptorRef) const
{
    const std::string owner      = ownerRef.to_std_string();
    const std::string name       = nameRef.to_std_string();
    const std::string descriptor = descriptorRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("get field info");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "get field info",
                                   "owner, name and descriptor are required"));
    }
    // dex 会话：类事实面走 dex 域（jbc 会话族对 dex 容器不可解析）。
    if (dexDomain_ != nullptr) return dexDomain_->fieldInfo(owner, name, descriptor);
    auto uri = resolveClassUri(*working_, owner);
    if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        working_->getResource(uri.unwrap()));
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "get field info", owner));
    auto session = sessionOf(resource->get(), "get field info");
    if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
    auto                  held = std::move(session).unwrap();
    jbc::core::ClassFile& cf   = held->backing();
    for (auto& field : cf.fields) {
        if (utf8Text(cf, field.nameCp) == name && utf8Text(cf, field.descCp) == descriptor)
            return ca::core::Ok(makeFieldInfo(cf, field));
    }
    return ca::core::Err(
        error(WorkspaceError::NotFound, "get field info", owner + "." + name + descriptor));
}

WorkspaceResult<std::vector<ConstantPoolEntryInfo>> WorkspaceBase::constantPool(
    ca::str::Utf8StringRef uriOrInternalNameRef, bool resolve) const
{
    const std::string uriOrInternalName = uriOrInternalNameRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("get constant pool");
    if (status.is_err()) return ca::core::Err(std::move(status));
    // dex 会话：dex 五张 ID 表投影常量池视图（libdex dom）。
    if (dexDomain_ != nullptr)
        return dexDomain_->constantPool(uriOrInternalName, resolve);
    auto uri = resolveClassUri(*working_, uriOrInternalName);
    if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        working_->getResource(uri.unwrap()));
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "get constant pool", uri.unwrap()));
    auto session = sessionOf(resource->get(), "get constant pool");
    if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
    auto                        held = std::move(session).unwrap();
    const jbc::core::ClassFile& cf   = held->backing();
    std::vector<ConstantPoolEntryInfo> result;
    for (ca::u32 index = 1; index < cf.cp.size(); ++index) {
        result.push_back(support::constantPoolEntry(cf, index, resolve));
    }
    return ca::core::Ok(std::move(result));
}

WorkspaceResult<MethodInstructions> WorkspaceBase::methodInstructions(
    ca::str::Utf8StringRef ownerRef, ca::str::Utf8StringRef nameRef,
    ca::str::Utf8StringRef descriptorRef, bool resolve) const
{
    const std::string owner      = ownerRef.to_std_string();
    const std::string name       = nameRef.to_std_string();
    const std::string descriptor = descriptorRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("get method instructions");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "get method instructions",
                                   "owner, name and descriptor are required"));
    }
    // dex 会话：方法指令面走 dex 域（smali 形行渲染）。
    if (dexDomain_ != nullptr)
        return dexDomain_->methodInstructions(owner, name, descriptor, resolve);
    auto uri = resolveClassUri(*working_, owner);
    if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
    const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
        working_->getResource(uri.unwrap()));
    if (resource == nullptr)
        return ca::core::Err(error(WorkspaceError::NotFound, "get method instructions", owner));
    auto session = sessionOf(resource->get(), "get method instructions");
    if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
    auto                  held = std::move(session).unwrap();
    jbc::core::ClassFile& cf   = held->backing();
    for (ca::usize methodIndex = 0; methodIndex < cf.methods.size(); ++methodIndex) {
        auto& method = cf.methods[methodIndex];
        if (utf8Text(cf, method.nameCp) != name || utf8Text(cf, method.descCp) != descriptor)
            continue;
        jbc::core::CodeAttr* code = support::ensureCode(cf, method);
        if (code == nullptr)
            return ca::core::Err(error(WorkspaceError::NotFound, "get method instructions",
                                       "method has no Code attribute"));
        auto promoted = jbc::ir::promoteMethod(*held, methodIndex);
        if (promoted.is_err())
            return ca::core::Err(error(WorkspaceError::DataLoss, "get method instructions",
                                       "method body cannot be promoted"));
        auto raw = promoted.unwrap()->dom.exportRaw();
        if (raw.is_err())
            return ca::core::Err(error(WorkspaceError::DataLoss, "get method instructions",
                                       "method body cannot be exported"));
        const auto         insns = std::move(raw).unwrap();
        MethodInstructions result;
        result.maxStack  = static_cast<ca::i32>(code->maxStack);
        result.maxLocals = static_cast<ca::i32>(code->maxLocals);
        for (const auto& insn : insns) {
            InstructionDetailInfo rendered = renderInstruction(cf, insn);
            if (resolve) rendered.operandComment = utf8(ldcComment(cf, insn.cp));
            result.instructions.push_back(std::move(rendered));
        }
        return ca::core::Ok(std::move(result));
    }
    return ca::core::Err(error(WorkspaceError::NotFound, "get method instructions",
                               owner + "." + name + descriptor));
}


// 引擎文本搜索四分面（searchClasses/searchMembers/searchStrings/searchNumbers）
// 与资源串活扫描面（resourceStrings/searchResourceStrings）已随查询面
// DB-first 收编退役：analysis.db 的 *Paged 查询承接（regex 档全量拉取客户
// 端过滤，search_match 共享层单源）；instructions 是唯一的引擎保留面。

WorkspaceResult<std::vector<SearchHit>> WorkspaceBase::searchInstructions(
    ca::str::Utf8StringRef queryRef, const SearchOptions& options) const
{
    const std::string query = queryRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("search instructions");
    if (status.is_err()) return ca::core::Err(std::move(status));
    // dex 会话：smali 形指令行搜索（smalict 渲染层与 JVM 面同源匹配器）。
    if (dexDomain_ != nullptr) return dexDomain_->searchInstructions(query, options);
    auto compiled = compileSearchRegex(query, options, "search instructions");
    if (compiled.is_err()) return ca::core::Err(std::move(compiled).unwrap_err());
    auto        compiledRegex = std::move(compiled).unwrap();
    const auto* regex         = compiledRegex ? &*compiledRegex : nullptr;

    std::vector<SearchHit> result;
    working_->visitResources([&](std::string_view uriView, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        const std::string uri{uriView};
        auto              session = sessionOf(classResource->get(), "search instructions");
        if (session.is_err()) return;
        auto                  held = std::move(session).unwrap();
        jbc::core::ClassFile& cf   = held->backing();
        const auto            owner = mjbase::jasm::classFileUtf8ToString(cf.thisName());
        if (!matchesSearchScope(owner, options)) return;
        for (auto& visit : jbc::ir::collectCodeUnits(*held)) {
            if (visit.kind != jbc::ir::CodeVisit::Kind::Ok) continue;
            auto raw = visit.unit->dom.exportRaw();
            if (raw.is_err()) continue;
            const auto&       insns = std::move(raw).unwrap();
            auto&             method = cf.methods[visit.methodIdx];
            const std::string methodName = utf8Text(cf, method.nameCp);
            const std::string methodDesc = utf8Text(cf, method.descCp);
            for (ca::i32 index = 0; index < static_cast<ca::i32>(insns.size()); ++index) {
                const auto& insn = insns[static_cast<ca::usize>(index)];
                if (options.opcode >= 0 && static_cast<ca::i32>(insn.op) != options.opcode)
                    continue;
                std::ostringstream output;
                const char*        mnemonic = jbc::core::mnemonicOf(insn.op);
                output << (mnemonic != nullptr ? mnemonic : "?") << ' '
                       << instructionNodeType(insn.op) << ' ' << static_cast<ca::i32>(insn.op);
                switch (jbc::core::shapeOf(insn.op)) {
                case jbc::core::Shape::Imm1:
                case jbc::core::Shape::Imm2:
                case jbc::core::Shape::Atype1:
                case jbc::core::Shape::Local1:
                    output << ' ' << insn.a;
                    break;
                case jbc::core::Shape::Iinc1: output << ' ' << insn.a << ' ' << insn.b; break;
                case jbc::core::Shape::CP2Dims:
                    output << ' ' << support::classNameOf(cf, insn.cp) << ' ' << insn.a;
                    break;
                case jbc::core::Shape::CP2Count:
                case jbc::core::Shape::CP2Zero2:
                    output << ' ' << memberRefFlatText(cf, insn.cp);
                    break;
                case jbc::core::Shape::CP1:
                case jbc::core::Shape::CP2:
                {
                    if (cf.cp.valid(insn.cp)) {
                        const auto& entry = cf.cp.at(insn.cp);
                        if (entry.tag == jbc::core::CpTag::String)
                            output << ' ' << utf8Text(cf, entry.a);
                        else if (entry.tag == jbc::core::CpTag::Class)
                            output << ' ' << support::classNameOf(cf, insn.cp);
                        else if (entry.tag == jbc::core::CpTag::Integer)
                            output << ' ' << entry.ival;
                        else if (entry.tag == jbc::core::CpTag::Long)
                            output << ' ' << entry.lval;
                        else if (entry.tag == jbc::core::CpTag::Float)
                            output << ' '
                                   << support::bitsToFloat(static_cast<ca::u32>(entry.ival));
                        else if (entry.tag == jbc::core::CpTag::Double)
                            output << ' '
                                   << support::bitsToDouble(static_cast<ca::u64>(entry.lval));
                        else
                        {
                            // CP2 形态含 invoke*virtual/static/special 与字段/类访问
                            // 指令。方法引用描述符以 '(' 开头自带分界，保持无冒号
                            // 扁平口径（搜索文本契约）；字段引用的名字与描述符
                            // 连排不可读（评估反馈实证：outLjava/...），冒号分界。
                            if (entry.tag == jbc::core::CpTag::Fieldref)
                            {
                                const auto reference = memberRefColonText(cf, insn.cp);
                                if (!reference.empty()) output << ' ' << reference;
                            }
                            else
                            {
                                const auto reference = memberRefFlatText(cf, insn.cp);
                                if (!reference.empty()) output << ' ' << reference;
                            }
                        }
                    }
                    break;
                }
                default: break;
                }
                const auto value = output.str();
                if (!matchesText(value, query, options, regex)) continue;
                result.push_back({utf8(uri), utf8("instruction"), utf8(owner), utf8(methodName),
                                  utf8(methodDesc), index, utf8(value)});
            }
        }
    });
    sortAndDeduplicateSearchHits(result);
    return ca::core::Ok(std::move(result));
}

// ── 引用 / 层次分析面 ───────────────────────────────────────────────────

namespace {

template <typename Query>
WorkspaceResult<std::vector<ReferenceInfo>> mergeDexXrefs(
    std::vector<ReferenceInfo> result, const DexDomain* primary, const ContainerRegistry& registry,
    const std::unordered_map<std::string, std::string>& owners, Query query)
{
    result.erase(std::remove_if(result.begin(), result.end(), [&](const ReferenceInfo& item) {
        const auto name = item.sourceClass.to_std_string();
        if (primary && primary->containsClass(name)) return true;
        const auto owner = owners.find(name);
        if (owner == owners.end()) return false;
        const auto* support = registry.findBySupportId(owner->second);
        return support && support->dexDomain;
    }), result.end());
    for (auto& item : result) {
        const auto owner = owners.find(item.sourceClass.to_std_string());
        if (owner == owners.end()) continue;
        item.supporting = true;
        item.supportId = utf8(owner->second);
    }

    if (primary) {
        auto references = query(*primary);
        if (references.is_err()) return ca::core::Err(std::move(references).unwrap_err());
        for (auto& item : std::move(references).unwrap())
            result.push_back(std::move(item));
    }
    for (const auto& support : registry.records()) {
        if (!support.dexDomain) continue;
        auto references = query(*support.dexDomain);
        if (references.is_err()) return ca::core::Err(std::move(references).unwrap_err());
        for (auto& item : std::move(references).unwrap()) {
            const auto owner = owners.find(item.sourceClass.to_std_string());
            if (owner == owners.end() || owner->second != support.supportId) continue;
            item.supporting = true;
            item.supportId = utf8(support.supportId);
            result.push_back(std::move(item));
        }
    }
    sortReferences(result);
    return ca::core::Ok(std::move(result));
}

}   // namespace

WorkspaceResult<std::vector<ReferenceInfo>> WorkspaceBase::xrefsToMethod(
    ca::str::Utf8StringRef ownerRef, ca::str::Utf8StringRef nameRef,
    ca::str::Utf8StringRef descriptorRef) const
{
    const std::string owner      = ownerRef.to_std_string();
    const std::string name       = nameRef.to_std_string();
    const std::string descriptor = descriptorRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("xref method");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "xref method",
                                   "owner, name and descriptor are required"));
    }
    if (dexDomain_ != nullptr && registry_->records().empty())
        return dexDomain_->xrefsToMethod(owner, name, descriptor);
    try {
        ReferenceScan scan;
        scan.build(*analysisScanContainer());
        std::vector<ReferenceInfo> result;
        for (const auto& reference : scan.methodReferencesTo(utf8Ref(owner), utf8Ref(name),
                                                             utf8Ref(descriptor))) {
            result.push_back(referenceInfo(
                reference.site,
                reference.target.owner.to_std_string() + "." +
                    reference.target.name.to_std_string() +
                    reference.target.descriptor.to_std_string(),
                "direct"));
        }
        return mergeDexXrefs(std::move(result), dexDomain_.get(), *registry_,
                            supportingClassIds(), [&](const DexDomain& dex) {
            return dex.xrefsToMethod(owner, name, descriptor);
        });
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "xref method", exception.what()));
    }
}

WorkspaceResult<std::vector<ReferenceInfo>> WorkspaceBase::xrefsToField(
    ca::str::Utf8StringRef ownerRef, ca::str::Utf8StringRef nameRef,
    ca::str::Utf8StringRef descriptorRef) const
{
    const std::string owner      = ownerRef.to_std_string();
    const std::string name       = nameRef.to_std_string();
    const std::string descriptor = descriptorRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("xref field");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "xref field",
                                   "owner, name and descriptor are required"));
    }
    if (dexDomain_ != nullptr && registry_->records().empty())
        return dexDomain_->xrefsToField(owner, name, descriptor);
    try {
        ReferenceScan scan;
        scan.build(*analysisScanContainer());
        std::vector<ReferenceInfo> result;
        for (const auto& reference : scan.fieldReferencesTo(utf8Ref(owner), utf8Ref(name),
                                                            utf8Ref(descriptor))) {
            result.push_back(referenceInfo(
                reference.site,
                reference.target.owner.to_std_string() + "." +
                    reference.target.name.to_std_string() +
                    reference.target.descriptor.to_std_string(),
                "direct"));
        }
        return mergeDexXrefs(std::move(result), dexDomain_.get(), *registry_,
                            supportingClassIds(), [&](const DexDomain& dex) {
            return dex.xrefsToField(owner, name, descriptor);
        });
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "xref field", exception.what()));
    }
}

WorkspaceResult<std::vector<ReferenceInfo>> WorkspaceBase::xrefsToClass(
    ca::str::Utf8StringRef internalNameRef) const
{
    const std::string internalName = internalNameRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("xref class");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (internalName.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "xref class", "class is empty"));
    if (dexDomain_ != nullptr && registry_->records().empty())
        return dexDomain_->xrefsToClass(internalName);
    try {
        ReferenceScan scan;
        scan.build(*analysisScanContainer());
        std::vector<ReferenceInfo> result;
        for (const auto& reference : scan.typeReferencesTo(utf8Ref(internalName))) {
            result.push_back(referenceInfo(reference.site, reference.targetType.ref(), "type"));
        }
        return mergeDexXrefs(std::move(result), dexDomain_.get(), *registry_,
                            supportingClassIds(), [&](const DexDomain& dex) {
            return dex.xrefsToClass(internalName);
        });
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "xref class", exception.what()));
    }
}

WorkspaceResult<ReferenceScan> WorkspaceBase::referenceScan() const
{
    WorkspaceErrorInfo status = ensureLoaded("scan references");
    if (status.is_err()) return ca::core::Err(std::move(status));
    // dex 会话：全域引用扫描走 dex 域（ingest 桥喂库同源）。
    if (dexDomain_ != nullptr) return ca::core::Ok(dexDomain_->buildReferenceScan());
    try {
        ReferenceScan scan;
        scan.build(*analysisScanContainer());
        return ca::core::Ok(std::move(scan));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "scan references", exception.what()));
    }
}

WorkspaceResult<std::vector<ReferenceInfo>> WorkspaceBase::calleesOfMethod(
    ca::str::Utf8StringRef ownerRef, ca::str::Utf8StringRef nameRef,
    ca::str::Utf8StringRef descriptorRef, bool resolved) const
{
    const std::string owner      = ownerRef.to_std_string();
    const std::string name       = nameRef.to_std_string();
    const std::string descriptor = descriptorRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("list callees");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "list callees",
                                   "owner, name and descriptor are required"));
    }
    if (dexDomain_ && (registry_->records().empty() || dexDomain_->containsClass(owner)))
        return dexDomain_->calleesOfMethod(owner, name, descriptor, resolved);
    try {
        const auto supportingClasses = supportingClassIds();
        const auto source = supportingClasses.find(owner);
        const auto* mounted = source == supportingClasses.end()
                                  ? nullptr
                                  : registry_->findBySupportId(source->second);
        if (mounted && mounted->dexDomain) {
            auto callees = mounted->dexDomain->calleesOfMethod(owner, name, descriptor, resolved);
            if (callees.is_err()) return ca::core::Err(std::move(callees).unwrap_err());
            auto result = std::move(callees).unwrap();
            for (auto& reference : result) {
                reference.supporting = true;
                reference.supportId = utf8(mounted->supportId);
            }
            return ca::core::Ok(std::move(result));
        }

        std::vector<ReferenceInfo> result;
        if (!resolved) {
            ReferenceScan scan;
            scan.build(*analysisScanContainer());
            for (const auto& reference : scan.methodReferencesFrom(utf8Ref(owner), utf8Ref(name),
                                                                   utf8Ref(descriptor))) {
                result.push_back(referenceInfo(
                    reference.site,
                    reference.target.owner.to_std_string() + "." +
                        reference.target.name.to_std_string() +
                        reference.target.descriptor.to_std_string(),
                    "direct"));
            }
        }
        else {
            // 解析展开（mjbase 面）：虚调用/接口调用沿层次图展开到「静态目标 +
            // 后代中声明同名同描述符方法」的候选集合（虚分派可达面）。本近似
            // 覆盖 direct 与单继承/接口分派的展开语义——完整闭包由主线
            // ResolvedCallGraph 设施接管。
            auto graph = analysisGraph();
            if (graph.is_err()) return ca::core::Err(std::move(graph).unwrap_err());
            auto           analysis = std::move(graph).unwrap();
            const auto& sourceContainer = mounted ? *mounted->container : *working_;
            auto           uri      = resolveClassUri(sourceContainer, owner);
            if (uri.is_err()) return ca::core::Err(std::move(uri).unwrap_err());
            const auto* resource = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
                sourceContainer.getResource(uri.unwrap()));
            if (resource == nullptr)
                return ca::core::Err(error(WorkspaceError::NotFound, "list callees", owner));
            auto session = sessionOf(resource->get(), "list callees");
            if (session.is_err()) return ca::core::Err(std::move(session).unwrap_err());
            auto                  held = std::move(session).unwrap();
            jbc::core::ClassFile& cf   = held->backing();
            for (ca::usize methodIndex = 0; methodIndex < cf.methods.size(); ++methodIndex) {
                auto& method = cf.methods[methodIndex];
                if (utf8Text(cf, method.nameCp) != name ||
                    utf8Text(cf, method.descCp) != descriptor)
                    continue;
                if (support::ensureCode(cf, method) == nullptr) break;
                auto promoted = jbc::ir::promoteMethod(*held, methodIndex);
                if (promoted.is_err()) break;
                const jbc::ir::CodeUnit& code  = *promoted.unwrap();
                const auto&              order = code.dom.order();
                for (ca::usize index = 0; index < order.size(); ++index) {
                    const jbc::ir::Insn* insn = code.dom.peek(order[index]);
                    if (insn == nullptr) continue;
                    if (insn->op != jbc::core::Op::INVOKEVIRTUAL &&
                        insn->op != jbc::core::Op::INVOKEINTERFACE &&
                        insn->op != jbc::core::Op::INVOKESPECIAL &&
                        insn->op != jbc::core::Op::INVOKESTATIC)
                        continue;
                    jbc::ir::ClassSession::MemberSem sem;
                    if (!held->memberOf(*insn, sem) || sem.owner.empty()) continue;
                    const std::string semOwner{sem.owner};
                    const std::string semName{sem.name};
                    const std::string semDesc{sem.desc};
                    const std::string declaredTarget = semOwner + "." + semName + semDesc;
                    bool              expanded       = false;
                    if (insn->op == jbc::core::Op::INVOKEVIRTUAL ||
                        insn->op == jbc::core::Op::INVOKEINTERFACE) {
                        const mjbase::ClassKey targetKey =
                            analysis.hierarchy->findClass(semOwner);
                        if (targetKey.valid()) {
                            std::vector<mjbase::ClassKey> candidates{targetKey};
                            for (const auto descendant :
                                 analysis.hierarchy->descendants(targetKey))
                                candidates.push_back(descendant);
                            for (const auto candidate : candidates) {
                                if (!analysis.declaresMethod(candidate.id, semName, semDesc))
                                    continue;
                                expanded = true;
                                result.push_back(referenceInfo(
                                    {utf8(owner), utf8(name), utf8(descriptor),
                                     static_cast<ca::i32>(index)},
                                    std::string{analysis.hierarchy->nameOf(candidate)} + "." +
                                        semName + semDesc,
                                    "resolved_override"));
                            }
                        }
                    }
                    if (!expanded) {
                        result.push_back(referenceInfo(
                            {utf8(owner), utf8(name), utf8(descriptor),
                             static_cast<ca::i32>(index)},
                            declaredTarget, "direct"));
                    }
                }
                break;
            }
        }
        if (mounted) {
            for (auto& reference : result) {
                reference.supporting = true;
                reference.supportId = utf8(mounted->supportId);
            }
        }
        sortReferences(result);
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "list callees", exception.what()));
    }
}

WorkspaceResult<ClassRelations> WorkspaceBase::classRelations(
    ca::str::Utf8StringRef internalNameRef) const
{
    const std::string internalName = internalNameRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("get class relations");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (internalName.empty()) {
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "get class relations", "class is empty"));
    }
    if (dexDomain_ && registry_->records().empty() && !dexDomain_->containsClass(internalName))
        return ca::core::Err(error(WorkspaceError::NotFound, "get class relations", internalName));
    try {
        auto graph = analysisGraph();
        if (graph.is_err()) return ca::core::Err(std::move(graph).unwrap_err());
        auto analysis = std::move(graph).unwrap();
        const mjbase::ClassKey index    = analysis.hierarchy->findClass(internalName);
        if (!index.valid())
            return ca::core::Err(
                error(WorkspaceError::NotFound, "get class relations", internalName));

        ClassRelations result;
        // super 链：ancestors 含起点（superChainOf 的公开面），跳过自身；合成节点使链以
        // java/lang/Object 收尾。
        const auto chain = analysis.hierarchy->ancestors(index);
        for (ca::usize at = 1; at < chain.size(); ++at) {
            result.superclasses.push_back(utf8(std::string{analysis.hierarchy->nameOf(chain[at])}));
        }
        // 接口闭包：super 链各类的直接接口 + 传递超接口（照旧 ancestors∩interface 语义）。
        for (const auto ancestor : analysis.hierarchy->interfaceClosure(index)) {
            if (ancestor == index || !analysis.hierarchy->isInterface(ancestor)) continue;
            result.interfaces.push_back(utf8(std::string{analysis.hierarchy->nameOf(ancestor)}));
        }
        for (const auto candidate : analysis.hierarchy->registrationOrder()) {
            if (candidate == index) continue;
            if (analysis.hierarchy->superClass(candidate) == index) {
                result.directSubclasses.push_back(
                    utf8(std::string{analysis.hierarchy->nameOf(candidate)}));
            }
            if (analysis.hierarchy->isSubtype(candidate, index)) {
                if (analysis.hierarchy->isInterface(index) &&
                    !analysis.hierarchy->isInterface(candidate)) {
                    result.implementors.push_back(
                        utf8(std::string{analysis.hierarchy->nameOf(candidate)}));
                }
                if (!analysis.hierarchy->isInterface(index)) {
                    result.allSubclasses.push_back(
                        utf8(std::string{analysis.hierarchy->nameOf(candidate)}));
                }
            }
        }
        if (!dexDomain_) sortUnique(result.superclasses);
        sortUnique(result.interfaces);
        sortUnique(result.directSubclasses);
        sortUnique(result.allSubclasses);
        sortUnique(result.implementors);

        std::set<std::string> relatedClasses{internalName};
        const auto collectRelated = [&relatedClasses](
                                        const std::vector<ca::str::Utf8String>& names) {
            for (const auto& name : names) relatedClasses.insert(name.to_std_string());
        };
        collectRelated(result.superclasses);
        collectRelated(result.interfaces);
        collectRelated(result.directSubclasses);
        collectRelated(result.allSubclasses);
        collectRelated(result.implementors);
        const auto supportingClasses = supportingClassIds();
        for (const auto& name : relatedClasses) {
            const auto source = supportingClasses.find(name);
            result.sources.push_back(
                {utf8(name), source != supportingClasses.end(),
                 source == supportingClasses.end() ? ca::str::Utf8String{} : utf8(source->second)});
        }
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "get class relations", exception.what()));
    }
}

WorkspaceResult<std::vector<MethodOverrideInfo>> WorkspaceBase::overridesOfMethod(
    ca::str::Utf8StringRef ownerRef, ca::str::Utf8StringRef nameRef,
    ca::str::Utf8StringRef descriptorRef) const
{
    const std::string owner      = ownerRef.to_std_string();
    const std::string name       = nameRef.to_std_string();
    const std::string descriptor = descriptorRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("get method overrides");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (owner.empty() || name.empty() || descriptor.empty()) {
        return ca::core::Err(error(WorkspaceError::InvalidArgument, "get method overrides",
                                   "owner, name and descriptor are required"));
    }
    // dex 会话：super 链同名同描述符声明走 dex 域。
    if (dexDomain_ != nullptr && registry_->records().empty())
        return dexDomain_->overridesOfMethod(owner, name, descriptor);
    try {
        auto graph = analysisGraph();
        if (graph.is_err()) return ca::core::Err(std::move(graph).unwrap_err());
        auto analysis = std::move(graph).unwrap();
        const mjbase::ClassKey ownerKey = analysis.hierarchy->findClass(owner);
        if (!ownerKey.valid())
            return ca::core::Err(error(WorkspaceError::NotFound, "get method overrides", owner));
        if (!analysis.declaresMethod(ownerKey.id, name, descriptor)) {
            return ca::core::Err(error(WorkspaceError::NotFound, "get method overrides",
                                       owner + "." + name + descriptor));
        }

        // 覆盖组件（mjbase 面直算）：候选 = 全部声明同名同描述符方法的登记类；
        // 两候选之间是子类型关系（任一方向）即连通；并查集闭包给出组件。
        // 与引擎共用的组件解析设施由主线 renamer 接管。
        std::vector<mjbase::ClassKey> candidates;
        for (const auto& [id, summary] : analysis.summaries) {
            (void)summary;
            if (analysis.declaresMethod(id, name, descriptor))
                candidates.push_back(mjbase::ClassKey{id});
        }
        std::sort(candidates.begin(), candidates.end(),
                  [](const mjbase::ClassKey& left, const mjbase::ClassKey& right) {
                      return left.id < right.id;
                  });
        std::unordered_map<ca::u32, ca::usize> slotOf;
        for (ca::usize at = 0; at < candidates.size(); ++at)
            slotOf.emplace(candidates[at].id, at);
        std::vector<ca::usize> parent(candidates.size());
        for (ca::usize at = 0; at < parent.size(); ++at) parent[at] = at;
        const std::function<ca::usize(ca::usize)> find = [&](ca::usize node) -> ca::usize {
            return parent[node] == node ? node : parent[node] = find(parent[node]);
        };
        for (ca::usize i = 0; i < candidates.size(); ++i) {
            for (ca::usize j = i + 1; j < candidates.size(); ++j) {
                if (analysis.hierarchy->isSubtype(candidates[i], candidates[j]) ||
                    analysis.hierarchy->isSubtype(candidates[j], candidates[i])) {
                    parent[find(i)] = find(j);
                }
            }
        }
        const ca::usize ownerRoot = find(slotOf.at(ownerKey.id));

        const auto supportingClasses = supportingClassIds();
        std::vector<MethodOverrideInfo> result;
        for (ca::usize at = 0; at < candidates.size(); ++at) {
            if (find(at) != ownerRoot) continue;
            const auto methodOwner = std::string{analysis.hierarchy->nameOf(candidates[at])};
            const auto support     = supportingClasses.find(methodOwner);
            result.push_back({utf8(methodOwner),
                              utf8(name),
                              utf8(descriptor),
                              methodOwner == owner,
                              support != supportingClasses.end(),
                              support == supportingClasses.end() ? ca::str::Utf8String{}
                                                                 : utf8(support->second)});
        }
        std::sort(result.begin(), result.end(), [](const MethodOverrideInfo& left,
                                                   const MethodOverrideInfo& right) {
            if (left.owner != right.owner) return left.owner < right.owner;
            if (left.name != right.name) return left.name < right.name;
            return left.descriptor < right.descriptor;
        });
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "get method overrides", exception.what()));
    }
}


// ── 变更事务面 ──────────────────────────────────────────────────────────

MutationResult WorkspaceBase::commit(std::unique_ptr<model::ResourceContainer> candidate,
                                     std::vector<jasm::Diagnostic> diagnostics,
                                     const MutationRequest&        request,
                                     const EditedMethodFocus*      focus,
                                     const std::vector<project::LedgerIdentityRename>& identity)
{
    if (!accepts(request)) return revisionConflict();
    auto diffs = diffContainers(*working_, *candidate);

    MutationResult result;
    result.status      = WorkspaceErrorInfo{};
    result.diagnostics = std::move(diagnostics);
    result.exportable  = true;
    result.byteDiffs   = std::move(diffs);
    if (result.byteDiffs.empty()) {
        // 空 diff 不产生新 revision、不进历史（照旧）。
        result.revision = revision_;
        return result;
    }

    //-- 台账事务装配（RFC §6.2：uri 字节级 entries，before/after digest 链）----
    std::set<std::string> uris;
    working_->visitResources(
        [&](std::string_view uri, const model::IResource&) { uris.insert(std::string(uri)); });
    candidate->visitResources(
        [&](std::string_view uri, const model::IResource&) { uris.insert(std::string(uri)); });

    try {
        project::LedgerAppendInput input;
        input.label      = request.label.to_std_string();
        input.producedBy = "mjbase-workspace/1";
        input.createdAt  = project::rfc3339UtcNow();
        //身份注记随事务原子落账（真改名语义的持久旁车；undo/redo/restore
        //消费方反推库迁移的反向/前向规格）。空 = 无名改语义。
        input.identity.reserve(identity.size());
        for (const auto& spec : identity) input.identity.push_back(spec.clone());
        const ca::u64 txnId = ledger_.nextTxnId();
        ca::u32       seq   = 0;
        for (const auto& uri : uris) {
            const model::IResource* beforeResource = working_->getResource(uri);
            const model::IResource* afterResource  = candidate->getResource(uri);
            std::vector<ca::u8>     beforeBytes;
            std::vector<ca::u8>     afterBytes;
            if (beforeResource != nullptr) {
                auto bytes = bytesOf(*beforeResource);
                if (bytes.is_err())
                    return stateFailure(std::move(bytes).unwrap_err());
                beforeBytes = std::move(bytes).unwrap();
            }
            if (afterResource != nullptr) {
                auto bytes = bytesOf(*afterResource);
                if (bytes.is_err()) return stateFailure(std::move(bytes).unwrap_err());
                afterBytes = std::move(bytes).unwrap();
            }
            if (beforeBytes == afterBytes) continue;

            project::LedgerEntry entry;
            entry.uri = project::copyLedgerText(uri);
            if (afterResource == nullptr) {
                entry.op     = project::LedgerEntryOp::Remove;
                entry.before = project::ledgerDigestOfBytes(beforeBytes.data(),
                                                            beforeBytes.size());
            }
            else {
                entry.after = project::ledgerDigestOfBytes(afterBytes.data(), afterBytes.size());
                entry.op    = beforeResource == nullptr ? project::LedgerEntryOp::Add
                                                        : project::LedgerEntryOp::Replace;
                if (beforeResource != nullptr) {
                    entry.before = project::ledgerDigestOfBytes(beforeBytes.data(),
                                                                beforeBytes.size());
                }
                entry.payloadRef = project::overlaySectionName(txnId, seq++);
                input.payloads.push_back({entry.payloadRef.clone(), afterBytes});
            }
            input.entries.push_back(std::move(entry));
        }
        auto appended = ledger_.append(input);
        if (appended.is_err()) return stateFailure(ledgerFailure("commit", std::move(appended).unwrap_err()));

        result.affectedClasses    = utf8List(changedClassNames(*candidate, result.byteDiffs));
        result.affectedReferences = collectImpacts(*candidate, result.byteDiffs, result.affectedClasses);
        redoHint_.clear();
        working_ = std::move(candidate);
        ++revision_;
        result.revision  = revision_;
        result.committed = true;
        // 提交锚点的指纹落库（#132 编辑流）：台账事务先行成功，落库失败不
        // 回滚编辑，计数披露在 methodVersions。
        recordMethodVersionsForEdit(result, focus);
        // 身份迁移（shadow 落分析库）：注记已随事务落账，库迁移失败同样
        // 不回滚编辑（可按注记反推补齐），披露在 identity。
        migrateIdentitiesForEdit(result, identity);
        // 类事实增量同步（查询面 DB-first）：条目锚/类事实/引用切片追平工作
        // 副本——在身份迁移之后（改名先迁行、同步再换锚），失败不回滚。
        syncAnalysisDbForEdit(result);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "commit", exception.what()));
    }
}

void WorkspaceBase::recordMethodVersionsForEdit(MutationResult&          result,
                                                const EditedMethodFocus* focus)
{
    if (analysisDb_ == nullptr || result.byteDiffs.empty()) return;
    std::vector<ca::str::Utf8String> classUris;
    classUris.reserve(result.byteDiffs.size());
    for (const ByteDiff& diff : result.byteDiffs) classUris.push_back(diff.uri.clone());
    result.methodVersions = recordMethodVersionsAfterEdit(*analysisDb_, *working_, classUris, focus);
}

void WorkspaceBase::syncAnalysisDbForEdit(MutationResult& result)
{
    if (analysisDb_ == nullptr || result.byteDiffs.empty()) return;
    std::vector<ca::str::Utf8String> changedUris;
    changedUris.reserve(result.byteDiffs.size());
    for (const ByteDiff& diff : result.byteDiffs) changedUris.push_back(diff.uri.clone());
    result.analysisSync =
        syncAnalysisDbAfterEdit(*this, *analysisDb_, *working_, changedUris);
}

void WorkspaceBase::migrateIdentitiesForEdit(
    MutationResult& result, const std::vector<project::LedgerIdentityRename>& specs)
{
    result.identity.annotated = specs.size();
    if (analysisDb_ == nullptr || specs.empty()) return;
    result.identity.attempted = true;
    std::vector<mjbase::project::DbIdentityRename> renames;
    renames.reserve(specs.size());
    for (const auto& spec : specs) {
        mjbase::project::DbIdentityRename rename;
        rename.target = spec.isMember ? mjbase::project::RenameTargetKind::Member
                                      : mjbase::project::RenameTargetKind::Class;
        rename.memberKind =
            spec.isMethod ? mjbase::project::DbMemberKind::Method
                          : mjbase::project::DbMemberKind::Field;
        rename.owner   = spec.owner.clone();
        rename.name    = spec.name.clone();
        rename.desc    = spec.desc.clone();
        rename.newName = spec.newName.clone();
        rename.newDesc = spec.newDesc.clone();
        renames.push_back(std::move(rename));
    }
    auto migrated = analysisDb_->migrateIdentities(std::move(renames));
    if (migrated.is_err()) {
        result.identity.failed          = true;
        result.identityWarning          = "identity migration failed: ";
        result.identityWarning         += std::move(migrated).unwrap_err().message.to_std_string();
        result.identityWarning         += " (ledger annotation intact; re-running the rename "
                                          "history rebuilds identity rows)";
        return;
    }
    const auto stats  = std::move(migrated).unwrap();
    result.identity.migratedClasses = stats.migratedClasses;
    result.identity.migratedMembers = stats.migratedMembers;
    result.identity.skipped         = stats.skipped;
}

std::vector<project::LedgerIdentityRename> WorkspaceBase::identitySpecsOfRemap(
    const asm_remapper::MemberRemapPlan& plan) const
{
    std::vector<project::LedgerIdentityRename> specs;
    //类规格先行（成员 owner 是类规格先行后的新名）；字典序定序。
    auto mappings = plan.classMappings();
    std::sort(mappings.begin(), mappings.end(),
              [](const auto& lhs, const auto& rhs) { return lhs.first < rhs.first; });
    for (const auto& [from, to] : mappings) {
        if (from == to) continue;
        project::LedgerIdentityRename spec;
        spec.isMember = false;
        spec.name     = utf8(from);
        spec.newName  = utf8(to);
        specs.push_back(std::move(spec));
    }
    //成员规格：规则键/值三段经计划池展开。恒等规则（名与描述符都不变）不产生
    //身份迁移；无描述符规则按工作副本同名成员解析为逐重载规格（owner 不可
    //解析 = 计划悬空，库迁移按未命中跳过语义放行空集）。
    const auto emitMembers = [&](const auto& rules, bool method) {
        for (const auto& rule : rules) {
            const auto oldOwner = plan.pool().str(rule.oldKey.cls);
            const auto oldName  = plan.pool().str(rule.oldKey.name);
            const auto oldDesc  = plan.pool().str(rule.oldKey.desc);
            const auto newOwner = plan.pool().str(rule.newKey.cls);
            const auto newName  = plan.pool().str(rule.newKey.name);
            const auto newDesc  = plan.pool().str(rule.newKey.desc);
            if (!oldDesc.empty()) {
                if (oldName == newName && oldDesc == newDesc) continue;
                project::LedgerIdentityRename spec;
                spec.isMember = true;
                spec.isMethod = method;
                spec.owner    = utf8(newOwner);
                spec.name     = utf8(oldName);
                spec.desc     = utf8(oldDesc);
                spec.newName  = utf8(newName);
                spec.newDesc  = utf8(newDesc);
                specs.push_back(std::move(spec));
                continue;
            }
            const std::string ownerText(oldOwner);
            auto info = classInfo(*working_, utf8Ref(ownerText), lvtJarFallbackFlag());
            if (info.is_err()) continue;
            const auto& clazz = std::move(info).unwrap();
            const auto  memberNameText = std::string(oldName);
            const auto visit = [&](const ca::str::Utf8StringRef& name,
                                   const ca::str::Utf8StringRef& desc) {
                if (name != memberNameText) return;
                project::LedgerIdentityRename spec;
                spec.isMember = true;
                spec.isMethod = method;
                spec.owner    = utf8(newOwner);
                spec.name     = utf8(oldName);
                spec.desc     = utf8(desc.to_std_string());
                spec.newName  = utf8(newName);
                spec.newDesc  = utf8(std::string(
                    method ? plan.mapMethodDescriptor(desc) : plan.mapFieldDescriptor(desc)));
                specs.push_back(std::move(spec));
            };
            if (method)
                for (const auto& item : clazz.methods) visit(item.name.ref(), item.descriptor.ref());
            else
                for (const auto& item : clazz.fields) visit(item.name.ref(), item.descriptor.ref());
        }
    };
    emitMembers(plan.fieldRules(), false);
    emitMembers(plan.methodRules(), true);
    return specs;
}

WorkspaceResult<std::vector<ModifiedClassInfo>> WorkspaceBase::modifiedClasses() const
{
    WorkspaceErrorInfo status = ensureLoaded("list modified classes");
    if (status.is_err()) return ca::core::Err(std::move(status));
    try {
        std::vector<ModifiedClassInfo> result;
        for (auto& diff : diffContainers(*original_, *working_)) {
            const auto* original = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
                original_->getResource(diff.uri.ref()));
            const auto* current = ca::core::tag_cast::dyn_cast<const model::ClassResource>(
                working_->getResource(diff.uri.ref()));
            if (original == nullptr && current == nullptr) continue;

            ModifiedClassInfo item;
            item.resourceUri = std::move(diff.uri);
            if (original != nullptr) {
                item.originalSize         = original->get().size();
                item.originalFingerprint  = utf8(fingerprintBytes(original->get()));
                item.originalInternalName = utf8(internalNameOfBytes(original->get()));
            }
            if (current != nullptr) {
                item.currentSize         = current->get().size();
                item.currentFingerprint  = utf8(fingerprintBytes(current->get()));
                item.currentInternalName = utf8(internalNameOfBytes(current->get()));
            }
            result.push_back(std::move(item));
        }
        return ca::core::Ok(std::move(result));
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "list modified classes", exception.what()));
    }
}

/// @brief 候选容器 → 层次图即时重建（校验门用；resource_jasm_service 同款容
///        错口径）：只读 Class 骨架，畸形/重复/环条目跳过。容器内类可解析走
///        子类型判定；外部/JDK 类型不进图——relaxed 语义按不可解析处置。
void buildValidationHierarchy(const model::ResourceContainer&   container,
                              mjbase::hierarchy::ClassHierarchy& hierarchy, mjbase::KeyPool& pool)
{
    container.visitResources([&](std::string_view, const model::IResource& resource) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(&resource);
        if (classResource == nullptr) return;
        auto read = jbc::core::readClass(jbc::core::ByteView(classResource->get()));
        if (read.is_err()) return;
        auto        parsed = std::move(read).unwrap();
        const auto& file   = *parsed.file;

        mjbase::hierarchy::ClassSummary summary;
        summary.self = mjbase::classKeyOf(pool, mjbase::jasm::classFileUtf8ToString(file.thisName()));
        const std::string_view superName = file.superName();
        summary.super = superName.empty()
                            ? mjbase::ClassKey{}
                            : mjbase::classKeyOf(pool, mjbase::jasm::classFileUtf8ToString(superName));
        for (const ca::u16 ifaceCp : file.interfaces) {
            summary.interfaces.push_back(mjbase::classKeyOf(
                pool, mjbase::jasm::classFileUtf8ToString(file.className(ifaceCp))));
        }
        (void)hierarchy.addClass(summary);
    });
}

WorkspaceErrorInfo WorkspaceBase::validateCandidate(const model::ResourceContainer&  candidate,
                                                    std::vector<jasm::Diagnostic>&  diagnostics,
                                                    jasm::ValidationMode            mode,
                                                    const std::vector<std::string>& scopeUris) const
{
    // off：跳过门禁（直通零诊断）。
    if (mode == jasm::ValidationMode::Off) return WorkspaceErrorInfo{};
    jasm::JvmJasmService validator;
    // 层次图按候选容器即时重建：relaxed 下容器内类走子类型判定，外部
    // /JDK 类型不可解析不判非法。
    mjbase::KeyPool                   pool;
    mjbase::hierarchy::ClassHierarchy hierarchy{pool};
    buildValidationHierarchy(candidate, hierarchy, pool);
    jasm::JvmJasmContext  context;
    context.hierarchy      = &hierarchy;
    context.validationMode = mode;

    // per-class 聚合缓冲：类 uri → 计数 + 截断明细，不再逐方法刷屏。
    struct ClassAggregate
    {
        std::string                     internalName;
        ca::usize                       errors   = 0;
        ca::usize                       warnings = 0;
        std::vector<ca::str::Utf16String> details;   ///< 截断明细（前 3 条）
    };
    std::map<std::string, ClassAggregate> aggregates;

    WorkspaceErrorInfo status;
    for (const auto& uri : scopeUris) {
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const model::ClassResource>(candidate.getResource(uri));
        if (classResource == nullptr) continue;   // 作用域内已删除条目（修复移除等）
        // spi sanitize gate：引用语义损坏的 class 在候选入口前置拒绝（协作面，
        // workspace × sanitize gate 契约落点）。
        const auto issues = spi::sanitizeClassFile(classResource->get());
        if (!issues.empty()) {
            for (const auto& issue : issues) {
                diagnostics.push_back({jasm::DiagnosticSeverity::Error,
                                       jasm::DiagnosticStage::EditGate,
                                       utf16("sanitize: " + issue.message.to_std_string()),
                                       std::nullopt});
            }
            return error(WorkspaceError::InvalidArgument, "validate workspace candidate",
                         "class failed sanitize gate: " + uri);
        }
        auto validated = validator.validate(classResource->get(), context);
        if (validated.diagnostics.empty()) continue;
        auto& aggregate = aggregates[uri];
        aggregate.internalName = internalNameOfBytes(classResource->get());
        for (const auto& diagnostic : validated.diagnostics) {
            if (diagnostic.severity == jasm::DiagnosticSeverity::Error) ++aggregate.errors;
            else
                ++aggregate.warnings;
            if (aggregate.details.size() < 3) aggregate.details.push_back(diagnostic.message.clone());
        }
    }
    for (auto& entry : aggregates) {
        auto&       aggregate = entry.second;
        const bool  failing   = aggregate.errors > 0;
        const auto& label =
            aggregate.internalName.empty() ? entry.first : aggregate.internalName;
        std::string message = "class " + label + ": " + std::to_string(aggregate.errors) +
                              " error(s), " + std::to_string(aggregate.warnings) + " warning(s)";
        if (!aggregate.details.empty()) {
            message +=
                "\n  " + aggregate.details[0].to_utf8_string().to_std_string();
            if (aggregate.details.size() > 1)
                message +=
                    "\n  " + aggregate.details[1].to_utf8_string().to_std_string();
            if (aggregate.details.size() > 2)
                message +=
                    "\n  " + aggregate.details[2].to_utf8_string().to_std_string();
            const ca::usize total = aggregate.errors + aggregate.warnings;
            if (total > aggregate.details.size())
                message += "\n  ... and " + std::to_string(total - aggregate.details.size()) +
                           " more";
        }
        diagnostics.push_back({failing ? jasm::DiagnosticSeverity::Error
                                       : jasm::DiagnosticSeverity::Warning,
                               jasm::DiagnosticStage::EditGate, utf16(message), std::nullopt});
        if (failing && status.is_ok())
            status = error(WorkspaceError::InvalidArgument, "validate workspace candidate",
                           "class did not pass validation: " + entry.first);
    }
    return status;
}

/// @brief 扫描 JDK 归档（.jar/.zip/.jmod，递归）建层次图。
///
/// mbase CommonSuperClassProvider::scan 的 mjbase 对应物：resolver 的数据获取
/// 是调用方的事，管线只消费已建好的图（pass-contract §9）。逐归档逐类
/// fail-open——解析不了的归档/类不贡献层次事实（mbase 扫描口径一致），层次
/// 缺口由 CommonSuperClassResolver 的回退路径兜住。jimage（lib/modules）不是
/// zip 容器，不在扫描集（与 mbase 行为一致）。
void scanJdkHierarchy(const std::string& jdkPath, mjbase::hierarchy::ClassHierarchy& hierarchy,
                      mjbase::KeyPool& pool)
{
    namespace fs = std::filesystem;
    std::error_code filesystemError;
    fs::recursive_directory_iterator it(fsPath(jdkPath), fs::directory_options::skip_permission_denied,
                                        filesystemError);
    if (filesystemError) return;
    for (const fs::directory_entry& entry = *it; it != fs::end(it); it.increment(filesystemError)) {
        if (filesystemError) break;
        if (!entry.is_regular_file(filesystemError) || filesystemError) {
            filesystemError.clear();
            continue;
        }
        std::string extension = entry.path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != ".jar" && extension != ".zip" && extension != ".jmod") continue;

        model::ResourceContainer archive;
        auto imported = model::ResourceHelper::importFromJar(archive, pathToUtf8(entry.path()));
        if (imported.is_err()) continue;   // 单归档 fail-open
        for (const auto& scanned : analysis::scanClassResources(archive)) {
            auto summary = hierarchy::fillSummary(*scanned.session, pool);
            (void)hierarchy.addClass(std::move(summary));   // 单类 fail-open（环检查拒绝等）
        }
    }
}

// ── remap / rename / pipeline / workflow 方法族 ─────────────────────────

WorkspaceErrorInfo WorkspaceBase::runPipeline(model::ResourceContainer& candidate,
                                              const PipelineRequest& request) const
{
    if (!request.pipeline.is_empty() && !request.passNames.empty()) {
        return error(WorkspaceError::InvalidArgument, "run pipeline",
                     "pipeline and passNames are mutually exclusive");
    }

    spi::registerBuiltinPipelines();
    spi::PipelineBuilder builder;
    if (!request.pipeline.is_empty()) {
        if (!spi::PassRegistry::instance().hasPipeline(request.pipeline.to_std_string())) {
            return error(WorkspaceError::NotFound, "run pipeline",
                         request.pipeline.to_std_string());
        }
        builder = spi::PassRegistry::instance().getPipeline(request.pipeline.to_std_string());
    }
    else if (!request.passNames.empty()) {
        for (const auto& name : request.passNames) {
            if (name.is_empty() ||
                spi::applyTo(spi::defaultPassRegistry(), name.ref(), builder).is_err()) {
                return error(WorkspaceError::NotFound, "run pass", name.to_std_string());
            }
        }
    }
    else {
        builder = spi::PassRegistry::instance().getPipeline("default-optimize");
    }

    // jdkPath 非空：扫描 JDK 归档建层次图，Strict resolver 接入帧重算。
    // resolver 链的生命周期必须覆盖
    // buildNestedPipeline + runContainerPipeline 全程，故在 job 之前构造。
    mjbase::KeyPool                        jdkPool;
    mjbase::hierarchy::ClassHierarchy      jdkHierarchy{jdkPool};
    mjbase::spi::CommonSuperClassResolver  superResolver{jdkHierarchy};
    mjbase::spi::StrictTypeResolverAdapter typeResolver{superResolver};
    spi::PassPipelineOptions               options;
    if (!request.jdkPath.is_empty()) {
        std::error_code filesystemError;
        if (!std::filesystem::is_directory(fsPath(request.jdkPath.to_std_string()),
                                           filesystemError) ||
            filesystemError) {
            return error(WorkspaceError::NotFound, "run pipeline", "JDK path does not exist");
        }
        scanJdkHierarchy(request.jdkPath.to_std_string(), jdkHierarchy, jdkPool);
        options.typeResolver = &typeResolver;
    }

    spi::PassJobContext  job;
    spi::GlobalPassManager pipeline = builder.buildNestedPipeline(job, nullptr, nullptr, options);
    auto ran = spi::runContainerPipeline(candidate, pipeline, job);
    if (ran.is_err()) {
        auto pipelineError = std::move(ran).unwrap_err();
        return error(WorkspaceError::Internal, "run pipeline",
                     std::string(spi::to_string(pipelineError.stage)) + " " +
                         pipelineError.target + ": " + pipelineError.detail);
    }
    return WorkspaceErrorInfo{};
}

ImpactAnalysis WorkspaceBase::analyzeRemap(const asm_remapper::MemberRemapPlan& plan,
                                           const asm_remapper::RemapPolicy& policy) const
{
    ImpactAnalysis result;
    auto loaded = ensureLoaded("analyze remap");
    if (loaded.is_err()) {
        result.status = std::move(loaded);
        return result;
    }
    try {
        auto candidate = cloneWorking();
        auto remapped  = asm_remapper::applyResourceRemapPlan(*candidate, plan, policy);
        if (remapped.is_err()) {
            result.status = error(WorkspaceError::Internal, "analyze remap",
                                  std::string("remap failed: ") +
                                      asm_remapper::to_string(std::move(remapped).unwrap_err()));
            return result;
        }
        // 门禁作用域 = remap 改动的类；diff 只算一次，scope 与报告共用。
        // ByteDiff 携 move-only Utf8String：byteDiffs 非常量才能移动移交。
        auto byteDiffs = diffContainers(*working_, *candidate);
        result.status =
            validateCandidate(*candidate, result.diagnostics, jasm::ValidationMode::Relaxed,
                              changedUris(byteDiffs));
        if (result.status.is_err()) return result;
        result.byteDiffs         = std::move(byteDiffs);
        const auto beforeClasses = utf8List(changedClassNames(*working_, result.byteDiffs));
        const auto afterClasses  = utf8List(changedClassNames(*candidate, result.byteDiffs));
        result.beforeReferences  = collectImpacts(*working_, result.byteDiffs, beforeClasses);
        result.afterReferences   = collectImpacts(*candidate, result.byteDiffs, afterClasses);
        for (const auto& diff : result.byteDiffs) {
            const auto uriText = diff.uri.to_std_string();
            if (uriText.find(".class") == std::string::npos)
                result.risks.push_back(utf8("resource changed: " + uriText));
        }
        result.exportable = true;
        return result;
    }
    catch (const std::exception& exception) {
        result.status = error(WorkspaceError::Internal, "analyze remap", exception.what());
        return result;
    }
}

WorkspaceErrorInfo WorkspaceBase::buildRenamePlan(const RenameRequest&   request,
                                                  asm_remapper::MemberRemapPlan&   plan,
                                                  std::vector<RenameMapping>&      mappings,
                                                  std::vector<MethodOverrideInfo>& overrides,
                                                  std::vector<std::string>&        risks) const
{
    if (request.owner.is_empty() || request.newName.is_empty())
        return error(WorkspaceError::InvalidArgument, "build rename plan",
                     "owner and new name are required");
    switch (request.kind) {
    case RenameKind::Class:
    {
        auto source = classInfo(*working_, request.owner, lvtJarFallbackFlag());
        if (source.is_err()) return source.unwrap_err();
        auto existing = classInfo(*working_, request.newName, lvtJarFallbackFlag());
        if (existing.is_ok() && request.newName != request.owner)
            return error(WorkspaceError::AlreadyExists, "rename class",
                         request.newName.to_std_string());
        plan.mapClass(request.owner.ref(), request.newName.ref());
        mappings.push_back({utf8("class"), request.owner.clone(), request.newName.clone()});
        return WorkspaceErrorInfo{};
    }
    case RenameKind::Package:
    {
        auto classes = listClasses(*working_, lvtJarFallbackFlag());
        if (classes.is_err()) return classes.unwrap_err();
        std::string oldPrefix = request.owner.to_std_string();
        std::string newPrefix = request.newName.to_std_string();
        while (!oldPrefix.empty() && oldPrefix.back() == '/') oldPrefix.pop_back();
        while (!newPrefix.empty() && newPrefix.back() == '/') newPrefix.pop_back();
        // ClassInfo 携 move-only Utf8String：unwrap 走移动重载。
        for (const auto& clazz : std::move(classes).unwrap()) {
            const std::string internalName = clazz.internalName.to_std_string();
            if (internalName != oldPrefix && internalName.rfind(oldPrefix + "/", 0) != 0)
                continue;
            const auto mapped = newPrefix + internalName.substr(oldPrefix.size());
            plan.mapClass(clazz.internalName.ref(), utf8Ref(mapped));
            mappings.push_back({utf8("class"), clazz.internalName.clone(), utf8(mapped)});
        }
        if (mappings.empty())
            return error(WorkspaceError::NotFound, "rename package",
                         request.owner.to_std_string());
        return WorkspaceErrorInfo{};
    }
    case RenameKind::Field:
    {
        if (request.name.is_empty() || request.descriptor.is_empty())
            return error(WorkspaceError::InvalidArgument, "rename field",
                         "name and descriptor are required");
        auto field = fieldInfo(request.owner, request.name, request.descriptor);
        if (field.is_err()) return field.unwrap_err();
        plan.mapField(request.owner.ref(), request.name.ref(), request.descriptor.ref(),
                      request.owner.ref(), request.newName.ref(), request.descriptor.ref());
        mappings.push_back(
            {utf8("field"),
             utf8(request.owner.to_std_string() + "." + request.name.to_std_string() +
                  request.descriptor.to_std_string()),
             utf8(request.owner.to_std_string() + "." + request.newName.to_std_string() +
                  request.descriptor.to_std_string())});
        return WorkspaceErrorInfo{};
    }
    case RenameKind::Method:
    {
        if (request.name.is_empty() || request.descriptor.is_empty())
            return error(WorkspaceError::InvalidArgument, "rename method",
                         "name and descriptor are required");
        auto method = methodInfo(*working_, request.owner, request.name, request.descriptor,
                                 lvtJarFallbackFlag());
        if (method.is_err()) return method.unwrap_err();
        auto component = overridesOfMethod(request.owner, request.name, request.descriptor);
        if (component.is_err()) return component.unwrap_err();
        overrides = std::move(component).unwrap();
        const bool hasSupporting =
            std::any_of(overrides.begin(), overrides.end(),
                        [](const auto& item) { return item.supporting; });
        if (overrides.size() > 1 && !request.cascadeOverrides)
            risks.push_back("method belongs to an override component; set cascadeOverrides "
                            "after reviewing mappings");
        if (hasSupporting)
            risks.push_back("override component contains supporting-library declarations that "
                            "cannot be renamed in this workspace");
        for (const auto& item : overrides) {
            if (item.supporting) continue;
            plan.mapMethod(item.owner.ref(), item.name.ref(), item.descriptor.ref(),
                           item.owner.ref(), request.newName.ref(), item.descriptor.ref());
            mappings.push_back(
                {utf8("method"),
                 utf8(item.owner.to_std_string() + "." + item.name.to_std_string() +
                      item.descriptor.to_std_string()),
                 utf8(item.owner.to_std_string() + "." + request.newName.to_std_string() +
                      item.descriptor.to_std_string())});
        }
        return WorkspaceErrorInfo{};
    }
    case RenameKind::Local:
        if (request.methodName.is_empty() || request.name.is_empty() ||
            request.descriptor.is_empty())
            return error(WorkspaceError::InvalidArgument, "rename local",
                         "method name, local name and descriptor are required");
        {
            auto method = methodInfo(*working_, request.owner, request.methodName,
                                     request.descriptor, lvtJarFallbackFlag());
            if (method.is_err()) return method.unwrap_err();
        }
        mappings.push_back({utf8("local"), request.name.clone(), request.newName.clone()});
        return WorkspaceErrorInfo{};
    }
    return error(WorkspaceError::InvalidArgument, "build rename plan", "unknown rename kind");
}

RenameAnalysis WorkspaceBase::analyzeRename(const RenameRequest& request) const
{
    RenameAnalysis result;
    asm_remapper::MemberRemapPlan plan;
    // buildRenamePlan 签名保持 std::string 风险面（workspace.hpp 私有声明不动，
    // B3 才参数化）：DTO risks 列在装配点经 utf8List 收编。
    std::vector<std::string> planRisks;
    result.status = buildRenamePlan(request, plan, result.mappings, result.overrideComponent,
                                    planRisks);
    if (result.status.is_err()) {
        result.impact.status = result.status;
        return result;
    }
    if (request.kind == RenameKind::Local) {
        result.impact.status     = WorkspaceErrorInfo{};
        result.impact.exportable = canExport();
        result.impact.risks      = utf8List(planRisks);
        result.readyToApply      = true;
        return result;
    }
    auto renameRisks = utf8List(planRisks);
    result.impact    = analyzeRemap(plan, request.policy);
    result.impact.risks.insert(result.impact.risks.end(),
                               std::make_move_iterator(renameRisks.begin()),
                               std::make_move_iterator(renameRisks.end()));
    result.status                 = result.impact.status;
    result.requiresOverrideReview = !result.impact.risks.empty();
    result.readyToApply = result.status.is_ok() && !result.requiresOverrideReview;
    return result;
}

MutationResult WorkspaceBase::revertClass(ca::str::Utf8StringRef uriRef,
                                          const MutationRequest& request)
{
    const std::string uri = uriRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("revert class");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    const auto* original =
        ca::core::tag_cast::dyn_cast<const model::ClassResource>(original_->getResource(uri));
    const auto* current =
        ca::core::tag_cast::dyn_cast<const model::ClassResource>(working_->getResource(uri));
    if (original == nullptr || current == nullptr) {
        return stateFailure(error(WorkspaceError::NotFound, "revert class", uri));
    }
    try {
        auto candidate = cloneWorking();
        candidate->replaceResource(uri,
                                   std::make_unique<model::ClassResource>(
                                       locationText(*original), original->get()));
        std::vector<jasm::Diagnostic> diagnostics;
        // 门禁作用域 = 本次还原的类——undo/revert 自救路径不再被容器
        // 基线类的既存形态卡死。
        status = validateCandidate(*candidate, diagnostics, request.validationMode, {uri});
        if (status.is_err()) {
            MutationResult result = stateFailure(std::move(status));
            result.diagnostics    = std::move(diagnostics);
            return result;
        }
        if (!request.dryRun)
            return commit(std::move(candidate), std::move(diagnostics), request);

        MutationResult result;
        result.status             = WorkspaceErrorInfo{};
        result.revision           = revision_;
        result.exportable         = canExport();
        result.diagnostics        = std::move(diagnostics);
        result.byteDiffs          = diffContainers(*working_, *candidate);
        result.affectedClasses    = utf8List(changedClassNames(*candidate, result.byteDiffs));
        result.affectedReferences = collectImpacts(*candidate, result.byteDiffs, result.affectedClasses);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "revert class", exception.what()));
    }
}

WorkspaceResult<void> WorkspaceBase::createSavepoint(ca::str::Utf8StringRef nameRef)
{
    const std::string name = nameRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("create savepoint");
    if (status.is_err()) return ca::core::Err(std::move(status));
    if (name.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "create savepoint", "name is empty"));
    // 台账换底：savepoint = 书签（零字节命名指针，§4）。savepoint 时机查自动
    // 压缩阈值（尽力而为：失败不影响 savepoint 本身）。
    auto bookmarked = ledger_.setBookmark(name, project::rfc3339UtcNow());
    if (bookmarked.is_err())
        return ca::core::Err(ledgerFailure("create savepoint", std::move(bookmarked).unwrap_err()));
    autoCompactLedger();
    return ca::core::Ok();
}

namespace {

///前向身份注记 → 反向规格：全序倒置 + 新旧互换。成员 owner 经批内类改名映射
///（新名→旧名）翻译到反向迁移后的名字（反向批类规格先行的处理序契约）；
///newDesc 空（= 描述符保持）的成员反向同样保持，非空则 desc/newDesc 对调。
std::vector<project::LedgerIdentityRename> reversedIdentitySpecs(
    const std::vector<project::LedgerIdentityRename>& forward)
{
    std::map<std::string, std::string> classBack;
    for (const auto& spec : forward) {
        if (!spec.isMember)
            classBack[spec.newName.to_std_string()] = spec.name.to_std_string();
    }
    const auto translate = [&classBack](const std::string& name) {
        const auto it = classBack.find(name);
        return it != classBack.end() ? it->second : name;
    };
    std::vector<project::LedgerIdentityRename> reversed;
    reversed.reserve(forward.size());
    for (auto it = forward.rbegin(); it != forward.rend(); ++it) {
        project::LedgerIdentityRename spec;
        spec.isMember = it->isMember;
        spec.isMethod = it->isMethod;
        if (!it->isMember) {
            spec.name    = it->newName.clone();
            spec.newName = it->name.clone();
        }
        else {
            spec.owner   = utf8(translate(it->owner.to_std_string()));
            spec.name    = it->newName.clone();
            spec.desc    = it->newDesc.is_empty() ? it->desc.clone() : it->newDesc.clone();
            spec.newName = it->name.clone();
            //前向描述符保持（newDesc 空）→ 反向同样保持；否则新旧对调。
            if (!it->newDesc.is_empty()) spec.newDesc = it->desc.clone();
        }
        reversed.push_back(std::move(spec));
    }
    return reversed;
}

}   //namespace

MutationResult WorkspaceBase::restoreSavepoint(ca::str::Utf8StringRef nameRef,
                                               const MutationRequest& request)
{
    const std::string name = nameRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("restore savepoint");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    const auto* bookmark = ledger_.bookmark(name);
    if (bookmark == nullptr)
        return stateFailure(error(WorkspaceError::NotFound, "restore savepoint", name));
    try {
        // 台账换底：restore = 物化书签事务位状态再走一次普通提交（记一条反向
        // 事务，undo 窗不受损；书签可位于头之前或之后——undo/redo 皆可越过）。
        auto candidate = cloneWorking();
        const auto restored = materializeState(candidate, bookmark->txn, "restore savepoint");
        if (restored.is_err()) return stateFailure(restored);
        //身份规格 = 书签位与头之间的注记差量（方向感知）：书签在头之前 =
        ///活动链 (bookmark, head] 的反向倒置；在头之后 = (head, bookmark] 的
        //前向正序。随 restore 事务落账（undo 该 restore 即整体反向回补）。
        //LedgerIdentityRename move-only：段间拼接走 clone/move，不经
        //vector::insert 的拷贝路径。
        std::vector<project::LedgerIdentityRename> identitySpecs;
        if (bookmark->txn < ledger_.headTxn()) {
            const auto chain = ledger_.chainTo(ledger_.headTxn());
            std::vector<std::vector<project::LedgerIdentityRename>> segments;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
                if (*it <= bookmark->txn) continue;
                const auto* row = ledger_.transaction(*it);
                if (row == nullptr || row->identity.empty()) continue;
                segments.push_back(reversedIdentitySpecs(row->identity));
            }
            for (auto& segment : segments) {
                identitySpecs.reserve(identitySpecs.size() + segment.size());
                for (auto& spec : segment) identitySpecs.push_back(std::move(spec));
            }
        }
        else if (bookmark->txn > ledger_.headTxn()) {
            for (const ca::u64 step : ledger_.chainTo(bookmark->txn)) {
                if (step <= ledger_.headTxn()) continue;
                const auto* row = ledger_.transaction(step);
                if (row == nullptr) continue;
                identitySpecs.reserve(identitySpecs.size() + row->identity.size());
                for (const auto& spec : row->identity)
                    identitySpecs.push_back(spec.clone());
            }
        }
        return commit(std::move(candidate), {}, request, nullptr, identitySpecs);
    }
    catch (const std::exception& exception) {
        return stateFailure(
            error(WorkspaceError::Internal, "restore savepoint", exception.what()));
    }
}

MutationResult WorkspaceBase::undo(const MutationRequest& request)
{
    WorkspaceErrorInfo status = ensureLoaded("undo");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    if (!ledger_.canUndo())
        return stateFailure(
            error(WorkspaceError::FailedPrecondition, "undo", "history is empty"));
    try {
        auto before = cloneWorking();
        const ca::u64       head    = ledger_.headTxn();
        const auto*         txn     = ledger_.transaction(head);
        const WorkspaceErrorInfo applied =
            inverseApplyLedgerTransaction(*before, *txn, "undo");
        if (applied.is_err()) return stateFailure(applied);
        auto moved = ledger_.moveHead(txn->parent);
        if (moved.is_err()) return stateFailure(ledgerFailure("undo", std::move(moved).unwrap_err()));
        redoHint_.push_back(head);
        auto diffs = diffContainers(*working_, *before);
        working_ = std::move(before);
        ++revision_;

        MutationResult result;
        result.status             = WorkspaceErrorInfo{};
        result.revision           = revision_;
        result.committed          = true;
        result.exportable         = true;
        result.byteDiffs          = std::move(diffs);
        result.affectedClasses    = utf8List(changedClassNames(*working_, result.byteDiffs));
        result.affectedReferences = collectImpacts(*working_, result.byteDiffs, result.affectedClasses);
        // undo 也改方法体：指纹按恢复后的字节落库（整类批量面）。
        recordMethodVersionsForEdit(result, nullptr);
        // 身份迁移反向（undo 不走 commit——头指针回退）：被撤销事务的身份注记
        // 整体倒置（类/成员新旧互换，member owner 经批内类映射回译）。
        migrateIdentitiesForEdit(result, reversedIdentitySpecs(txn->identity));
        // 类事实增量同步（undo 对称）：恢复后字节追平库事实（条目锚回滚是
        // sha 锚命中——旧行原地复活，entry id 不漂移）。
        syncAnalysisDbForEdit(result);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "undo", exception.what()));
    }
}

MutationResult WorkspaceBase::redo(const MutationRequest& request)
{
    WorkspaceErrorInfo status = ensureLoaded("redo");
    if (status.is_err()) return stateFailure(std::move(status));
    if (!accepts(request)) return revisionConflict();
    if (redoHint_.empty())
        return stateFailure(
            error(WorkspaceError::FailedPrecondition, "redo", "history is empty"));
    try {
        const ca::u64 target = redoHint_.back();
        const auto*   txn    = ledger_.transaction(target);
        if (txn == nullptr || txn->parent != ledger_.headTxn())
            return stateFailure(
                error(WorkspaceError::Internal, "redo", "redo target is no longer reachable"));
        auto before = cloneWorking();
        const auto  applied = applyLedgerTransaction(*before, *txn, "redo");
        if (applied.is_err()) return stateFailure(applied);
        auto moved = ledger_.moveHead(target);
        if (moved.is_err()) return stateFailure(ledgerFailure("redo", std::move(moved).unwrap_err()));
        redoHint_.pop_back();
        auto diffs = diffContainers(*working_, *before);
        working_ = std::move(before);
        ++revision_;

        MutationResult result;
        result.status             = WorkspaceErrorInfo{};
        result.revision           = revision_;
        result.committed          = true;
        result.exportable         = true;
        result.byteDiffs          = std::move(diffs);
        result.affectedClasses    = utf8List(changedClassNames(*working_, result.byteDiffs));
        result.affectedReferences = collectImpacts(*working_, result.byteDiffs, result.affectedClasses);
        // redo 同 undo：指纹按重放后的字节落库（整类批量面）。
        recordMethodVersionsForEdit(result, nullptr);
        // 身份迁移前向重放（redo 目标事务的注记原序）。
        migrateIdentitiesForEdit(result, txn->identity);
        // 类事实增量同步（redo 对称，与 commit/undo 同一挂点纪律）。
        syncAnalysisDbForEdit(result);
        return result;
    }
    catch (const std::exception& exception) {
        return stateFailure(error(WorkspaceError::Internal, "redo", exception.what()));
    }
}

// ── 导出面 ──────────────────────────────────────────────────────────────

WorkspaceResult<void> WorkspaceBase::exportArtifact(ca::str::Utf8StringRef outputPathRef) const
{
    const std::string outputPath = outputPathRef.to_std_string();
    WorkspaceErrorInfo status = ensureLoaded("export artifact");
    if (status.is_err()) return ca::core::Err(std::move(status));
    const ContainerIoEntry* entry = ContainerIoRegistry::instance().find(format_);
    if (entry == nullptr || entry->exportFn == nullptr) {
        return ca::core::Err(error(WorkspaceError::FailedPrecondition, "export artifact",
                                   "no exporter for the current format"));
    }
    // close/export 前 flush 挂点（§3.2）：导出物离开内存前台账定稿。未绑定 =
    // no-op；flush 失败 fail-closed（不产出一个后台无账的导出物）。
    if (ledger_.bound() && ledger_.hasPending()) {
        auto flushed = ledger_.flush(project::rfc3339UtcNow());
        if (flushed.is_err())
            return ca::core::Err(
                ledgerFailure("export artifact", std::move(flushed).unwrap_err()));
    }
    ExportSpec spec;
    spec.working             = working_.get();
    spec.original            = original_.get();
    spec.sourceManifestBytes = &sourceManifestBytes_;
    spec.sourcePath          = sourcePath_.to_std_string();
    spec.primaryClassUri     = primaryClassUri_.to_std_string();
    spec.revision            = revision_;
    const WorkspaceErrorInfo exported = entry->exportFn(spec, outputPath);
    if (exported.is_err()) return ca::core::Err(std::move(exported));
    return ca::core::Ok();
}

void WorkspaceBase::bindContainerRegistry(ContainerRegistry* registry) noexcept
{
    registry_ = registry != nullptr ? registry : &ownedRegistry_;
}

std::vector<SupportingResourceInfo> WorkspaceBase::supportingResources() const
{
    std::vector<SupportingResourceInfo> result;
    for (const auto& support : registry_->records()) {
        SupportingResourceInfo info;
        info.id            = utf8(support.supportId);
        info.path          = utf8(support.path);
        info.fingerprint   = utf8(support.fingerprint);
        info.resourceCount = support.container->size();
        info.role          = utf8(support.roleLabel);
        support.container->visitResources([&](std::string_view, const model::IResource& resource) {
            if (resource.getType() == model::ResourceType::Class) ++info.classCount;
        });
        if (support.dexDomain) info.classCount = support.dexDomain->classCount();
        result.push_back(std::move(info));
    }
    return result;
}

// ── 台账换底：引擎接线与重放辅助 ──────────────────────────────────

WorkspaceResult<void> WorkspaceBase::resetSessionState(const std::string& operation)
{
    registry_->clear();
    // 会话史清零（旧语义：打开新主资源丢弃 undo/redo/savepoint）。绑定态
    // 先落盘旧容器台账再重置链（新容器台账随下次 flush 落盘；旧容器已落盘
    // 历史入待释放清单随之清理——台账作用域 = 容器）。
    auto digest = ledgerBaseDigest();
    if (digest.is_err()) return ca::core::Err(std::move(digest).unwrap_err());
    if (ledger_.bound()) {
        if (ledger_.hasPending()) {
            auto flushed = ledger_.flush(project::rfc3339UtcNow());
            if (flushed.is_err())
                return ca::core::Err(
                    ledgerFailure(operation, std::move(flushed).unwrap_err()));
        }
        ledger_.resetSession(project::copyLedgerText(digest.unwrap()));
    }
    else {
        auto fresh = project::EditLedgerStore::createInMemory(project::copyLedgerText(digest.unwrap()));
        if (fresh.is_err())
            return ca::core::Err(ledgerFailure(operation, std::move(fresh).unwrap_err()));
        ledger_ = std::move(fresh).unwrap();
    }
    redoHint_.clear();
    revision_          = 0;
    // LVT 显示名策略旗标缓存失效：新主资源的形态基线重算（revision 复位
    // 不能单独作键——新旧会话 revision 可能同为 0）。
    lvtPolicyComputed_ = false;
    return ca::core::Ok();
}

WorkspaceErrorInfo WorkspaceBase::ledgerFailure(const std::string& operation,
                                                const project::ProjectStoreError& cause) const
{
    WorkspaceErrorInfo failure;
    switch (cause.kind) {
    case project::ProjectStoreErrorKind::Open: failure.code = WorkspaceError::NotFound; break;
    case project::ProjectStoreErrorKind::Lock:
        failure.code = WorkspaceError::PermissionDenied;
        break;
    case project::ProjectStoreErrorKind::Section:
    case project::ProjectStoreErrorKind::Schema:
        failure.code = WorkspaceError::DataLoss;
        break;
    case project::ProjectStoreErrorKind::Io: failure.code = WorkspaceError::Internal; break;
    }
    failure.message = operation + ": " + cause.message.to_std_string();
    return failure;
}

WorkspaceResult<std::string> WorkspaceBase::ledgerBaseDigest() const
{
    if (original_ == nullptr) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "ledger digest", "no container loaded"));
    }
    try {
        std::vector<std::string> uris;
        original_->visitResources(
            [&](std::string_view uri, const model::IResource&) { uris.push_back(std::string(uri)); });
        std::sort(uris.begin(), uris.end());
        project::ContainerDigestBuilder builder;
        for (const auto& uri : uris) {
            const auto* resource = original_->getResource(uri);
            if (resource == nullptr) continue;
            auto bytes = bytesOf(*resource);
            if (bytes.is_err()) return ca::core::Err(std::move(bytes).unwrap_err());
            const auto& content = bytes.unwrap();
            builder.add(uri, content.data(), content.size());
        }
        return ca::core::Ok(builder.finish().to_std_string());
    }
    catch (const std::exception& exception) {
        return ca::core::Err(
            error(WorkspaceError::Internal, "ledger digest", exception.what()));
    }
}

std::unique_ptr<model::IResource> WorkspaceBase::resourceFromBytes(const std::string& uri,
                                                                   std::vector<ca::u8> bytes)
{
    if (uri == std::string(mjbase::jar::JarUtil::kManifestFileName)) {
        return std::make_unique<model::ManifestResource>(model::ManifestResource::fromBytes(bytes));
    }
    if (hasClassExtension(uri)) {
        return std::make_unique<model::ClassResource>(uri, std::move(bytes));
    }
    return std::make_unique<model::UnknownResource>(uri, std::move(bytes));
}

WorkspaceResult<std::unique_ptr<model::ResourceContainer>> WorkspaceBase::replayBaseline() const
{
    if (!ledger_.hasBaselineSection()) return cloneContainer(*original_);
    auto bytes = ledger_.baselineBytes();
    if (bytes.is_err())
        return ca::core::Err(ledgerFailure("replay baseline", std::move(bytes).unwrap_err()));
    auto baseline = std::move(bytes).unwrap();
    auto container = std::make_unique<model::ResourceContainer>();
    WorkspaceErrorInfo failure;
    auto scanned = project::forEachContainerEntry(
        reinterpret_cast<const ca::u8*>(baseline.data()), baseline.size(),
        [&](std::string_view uri, const ca::u8* payload, ca::usize payloadSize) {
            container->addResource(uri, resourceFromBytes(
                                            std::string(uri),
                                            std::vector<ca::u8>(payload, payload + payloadSize)));
            return true;
        });
    if (scanned.is_err()) {
        failure = ledgerFailure("replay baseline", std::move(scanned).unwrap_err());
        return ca::core::Err(std::move(failure));
    }
    return ca::core::Ok(std::move(container));
}

WorkspaceResult<std::vector<ca::u8>> WorkspaceBase::resolveBeforeBytes(
    const project::LedgerTransaction& txn, const project::LedgerEntry& entry) const
{
    const std::string uri = entry.uri.to_std_string();
    std::vector<ca::u8> bytes;
    const auto          last = ledger_.lastTouchOf(uri, txn.parent);
    if (last != 0) {
        const auto* touch = ledger_.transaction(last);
        for (const auto& prior : touch->entries) {
            if (!prior.uri.ref().equals(entry.uri.ref())) continue;
            auto payload = ledger_.payloadBytes(prior.payloadRef);
            if (payload.is_err())
                return ca::core::Err(ledgerFailure("resolve before bytes",
                                                   std::move(payload).unwrap_err()));
            bytes = std::move(payload).unwrap();
            break;
        }
    }
    else {
        // 基线侧：压缩基线 section 或原始快照。
        if (ledger_.hasBaselineSection()) {
            auto baseline = ledger_.baselineBytes();
            if (baseline.is_err())
                return ca::core::Err(
                    ledgerFailure("resolve before bytes", std::move(baseline).unwrap_err()));
            auto baselineBytes = std::move(baseline).unwrap();
            std::vector<ca::u8> found;
            auto hit = project::findContainerEntry(
                reinterpret_cast<const ca::u8*>(baselineBytes.data()), baselineBytes.size(), uri,
                found);
            if (hit.is_err())
                return ca::core::Err(ledgerFailure("resolve before bytes",
                                                   std::move(hit).unwrap_err()));
            if (hit.unwrap()) bytes = std::move(found);
        }
        else {
            const auto* resource = original_ != nullptr ? original_->getResource(uri) : nullptr;
            if (resource != nullptr) {
                auto content = bytesOf(*resource);
                if (content.is_err()) return ca::core::Err(std::move(content).unwrap_err());
                bytes = std::move(content).unwrap();
            }
        }
    }
    // digest 自检：before 侧字节必须与台账记录一致（载荷/基线损毁当场暴露）。
    const auto digest =
        project::ledgerDigestOfBytes(bytes.data(), bytes.size());
    if (!digest.ref().equals(entry.before.ref())) {
        return ca::core::Err(error(WorkspaceError::DataLoss, "resolve before bytes",
                                   "digest mismatch for '" + uri + "': ledger says " +
                                       entry.before.to_std_string() + ", resolved " +
                                       digest.to_std_string()));
    }
    return ca::core::Ok(std::move(bytes));
}

WorkspaceErrorInfo WorkspaceBase::applyLedgerTransaction(
    model::ResourceContainer& container, const project::LedgerTransaction& txn,
    const std::string& operation) const
{
    for (const auto& entry : txn.entries) {
        const std::string uri = entry.uri.to_std_string();
        const bool        exists = container.hasResource(uri);
        if (entry.op == project::LedgerEntryOp::Add && exists) {
            return error(WorkspaceError::DataLoss, operation,
                         "add entry but uri already exists: " + uri);
        }
        if (entry.op == project::LedgerEntryOp::Replace && !exists) {
            return error(WorkspaceError::DataLoss, operation,
                         "replace entry but uri is missing: " + uri);
        }
        if (entry.op == project::LedgerEntryOp::Remove) {
            if (!exists)
                return error(WorkspaceError::DataLoss, operation,
                             "remove entry but uri is missing: " + uri);
            auto& resources = container.getResources();
            resources.erase(std::string_view(uri));
            continue;
        }
        auto payload = ledger_.payloadBytes(entry.payloadRef);
        if (payload.is_err()) return ledgerFailure(operation, std::move(payload).unwrap_err());
        auto bytes = std::move(payload).unwrap();
        const auto digest = project::ledgerDigestOfBytes(bytes.data(), bytes.size());
        if (!digest.ref().equals(entry.after.ref())) {
            return error(WorkspaceError::DataLoss, operation,
                         "overlay digest mismatch for '" + uri + "': ledger says " +
                             entry.after.to_std_string() + ", payload is " +
                             digest.to_std_string());
        }
        container.replaceResource(uri, resourceFromBytes(uri, std::move(bytes)));
    }
    return WorkspaceErrorInfo{};
}

WorkspaceErrorInfo WorkspaceBase::inverseApplyLedgerTransaction(
    model::ResourceContainer& container, const project::LedgerTransaction& txn,
    const std::string& operation) const
{
    for (const auto& entry : txn.entries) {
        const std::string uri    = entry.uri.to_std_string();
        const bool        exists = container.hasResource(uri);
        if (entry.op == project::LedgerEntryOp::Add) {
            if (!exists)
                return error(WorkspaceError::DataLoss, operation,
                             "undo add but uri is missing: " + uri);
            auto& resources = container.getResources();
            resources.erase(std::string_view(uri));
            continue;
        }
        if (entry.op == project::LedgerEntryOp::Remove && exists) {
            return error(WorkspaceError::DataLoss, operation,
                         "undo remove but uri already exists: " + uri);
        }
        if (entry.op == project::LedgerEntryOp::Replace && !exists) {
            return error(WorkspaceError::DataLoss, operation,
                         "undo replace but uri is missing: " + uri);
        }
        auto before = resolveBeforeBytes(txn, entry);
        if (before.is_err()) return std::move(before).unwrap_err();
        container.replaceResource(uri,
                                  resourceFromBytes(uri, std::move(before).unwrap()));
    }
    return WorkspaceErrorInfo{};
}

WorkspaceErrorInfo WorkspaceBase::materializeState(
    std::unique_ptr<model::ResourceContainer>& candidate, ca::u64 txn,
    const std::string& operation) const
{
    const ca::u64 head = ledger_.headTxn();
    if (txn == head) return WorkspaceErrorInfo{};
    if (txn > head) {
        for (const ca::u64 step : ledger_.chainTo(txn)) {
            if (step <= head) continue;
            const auto* row = ledger_.transaction(step);
            const auto  applied = applyLedgerTransaction(*candidate, *row, operation);
            if (applied.is_err()) return applied;
        }
        return WorkspaceErrorInfo{};
    }
    auto chain = ledger_.chainTo(head);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (*it <= txn) continue;
        const auto* row     = ledger_.transaction(*it);
        const auto  applied = inverseApplyLedgerTransaction(*candidate, *row, operation);
        if (applied.is_err()) return applied;
    }
    return WorkspaceErrorInfo{};
}

void WorkspaceBase::autoCompactLedger()
{
    if (!ledger_.bound() || !ledger_.shouldAutoCompact()) return;
    // 自动压缩（尽力而为）：先落 pending（压缩前置），再按保留参数阈值推进；
    // 任何失败留给下一次触发，不打断调用方主路径。
    if (ledger_.hasPending()) {
        auto flushed = ledger_.flush(project::rfc3339UtcNow());
        if (flushed.is_err()) return;
    }
    const auto point = ledger_.retentionPoint(project::LedgerCompactRequest{}, true, nullptr);
    if (point <= ledger_.baseTxn()) return;
    try {
        auto candidate = cloneWorking();
        const auto materialized = materializeState(candidate, point, "ledger compaction");
        if (materialized.is_err()) return;
        std::vector<std::pair<ca::str::Utf8String, std::vector<ca::u8>>> entries;
        std::vector<std::string>                                         uris;
        candidate->visitResources(
            [&](std::string_view uri, const model::IResource&) { uris.push_back(std::string(uri)); });
        std::sort(uris.begin(), uris.end());
        for (const auto& uri : uris) {
            const auto* resource = candidate->getResource(uri);
            if (resource == nullptr) continue;
            auto bytes = bytesOf(*resource);
            if (bytes.is_err()) return;
            entries.emplace_back(project::copyLedgerText(uri), std::move(bytes).unwrap());
        }
        (void)ledger_.compact(project::LedgerCompactRequest{},
                              project::serializeContainerEntries(entries),
                              project::rfc3339UtcNow(), true);
    }
    catch (const std::exception&) {
        return;
    }
}

WorkspaceResult<void> WorkspaceBase::bindLedgerStore(ca::str::Utf8StringRef bundleDirRef)
{
    const std::string bundleDir = bundleDirRef.to_std_string();
    const WorkspaceErrorInfo loaded = ensureLoaded("bind ledger store");
    if (loaded.is_err()) return ca::core::Err(std::move(loaded));
    if (bundleDir.empty())
        return ca::core::Err(
            error(WorkspaceError::InvalidArgument, "bind ledger store", "path is empty"));
    auto digest = ledgerBaseDigest();
    if (digest.is_err()) return ca::core::Err(std::move(digest).unwrap_err());

    // 采纳失败不破坏当前会话：先在旁路物化，成功才换底。
    const std::string previousDigest = ledger_.baseContainerDigest().to_std_string();
    auto opened = project::EditLedgerStore::openBundle(
        project::copyLedgerText(bundleDir), project::copyLedgerText(digest.unwrap()));
    if (opened.is_err())
        return ca::core::Err(ledgerFailure("bind ledger store", std::move(opened).unwrap_err()));
    ledger_ = std::move(opened).unwrap();
    if (ledger_.headTxn() != ledger_.baseTxn()) {
        // 候选处于基线态（刚打开的主资源）：从基点向头做前向重放——与
        // materializeState（候选在头位，向书签位正/反向走）语义不同。
        auto candidate = cloneWorking();
        WorkspaceErrorInfo replayFailure;
        for (const ca::u64 step : ledger_.chainTo(ledger_.headTxn())) {
            const auto* row = ledger_.transaction(step);
            if (row == nullptr) {
                replayFailure = error(WorkspaceError::DataLoss, "bind ledger store",
                                      "chain step " + std::to_string(step) +
                                          " is not on the active chain");
                break;
            }
            replayFailure = applyLedgerTransaction(*candidate, *row, "bind ledger store");
            if (replayFailure.is_err()) break;
        }
        if (replayFailure.is_err()) {
            // 回落纯内存引擎（持有锁已随 move 释放），保留旧基线 digest。
            auto fallback = project::EditLedgerStore::createInMemory(
                project::copyLedgerText(previousDigest));
            if (fallback.is_err()) {
                return ca::core::Err(ledgerFailure("bind ledger store",
                                                   std::move(fallback).unwrap_err()));
            }
            ledger_ = std::move(fallback).unwrap();
            return ca::core::Err(std::move(replayFailure));
        }
        working_ = std::move(candidate);
    }
    revision_ = 0;
    redoHint_.clear();
    return ca::core::Ok();
}

std::vector<std::vector<project::LedgerIdentityRename>>
WorkspaceBase::headIdentitySpecBatches() const
{
    std::vector<std::vector<project::LedgerIdentityRename>> batches;
    //纯内存/基线态台账（head == base）无现役事务，天然空集；不抛错——
    //自愈路径把"无事可补"当常态。
    if (ledger_.headTxn() == ledger_.baseTxn()) return batches;
    for (const ca::u64 step : ledger_.chainTo(ledger_.headTxn())) {
        const auto* row = ledger_.transaction(step);
        if (row == nullptr || row->identity.empty()) continue;
        //LedgerIdentityRename move-only：逐条 clone 深拷贝出库。
        std::vector<project::LedgerIdentityRename> batch;
        batch.reserve(row->identity.size());
        for (const auto& spec : row->identity) batch.push_back(spec.clone());
        batches.push_back(std::move(batch));
    }
    return batches;
}

WorkspaceResult<void> WorkspaceBase::flushLedger()
{
    auto flushed = ledger_.flush(project::rfc3339UtcNow());
    if (flushed.is_err())
        return ca::core::Err(ledgerFailure("flush ledger", std::move(flushed).unwrap_err()));
    autoCompactLedger();
    return ca::core::Ok();
}

WorkspaceResult<project::LedgerCompactReport> WorkspaceBase::compactLedger(
    const project::LedgerCompactRequest& request)
{
    const WorkspaceErrorInfo loaded = ensureLoaded("compact ledger");
    if (loaded.is_err()) return ca::core::Err(std::move(loaded));
    // 手动压缩前置无 pending（有则先 flush——数据安全序，见 §5.4）。
    if (ledger_.hasPending()) {
        auto flushed = ledger_.flush(project::rfc3339UtcNow());
        if (flushed.is_err())
            return ca::core::Err(ledgerFailure("compact ledger", std::move(flushed).unwrap_err()));
    }
    const auto point = ledger_.retentionPoint(request, false, nullptr);
    if (point <= ledger_.baseTxn()) {
        return ca::core::Err(error(WorkspaceError::FailedPrecondition, "compact ledger",
                                   "retention point does not advance past anchor"));
    }
    try {
        auto candidate = cloneWorking();
        const auto materialized = materializeState(candidate, point, "compact ledger");
        if (materialized.is_err()) return ca::core::Err(materialized);
        std::vector<std::pair<ca::str::Utf8String, std::vector<ca::u8>>> entries;
        std::vector<std::string>                                         uris;
        candidate->visitResources(
            [&](std::string_view uri, const model::IResource&) { uris.push_back(std::string(uri)); });
        std::sort(uris.begin(), uris.end());
        for (const auto& uri : uris) {
            const auto* resource = candidate->getResource(uri);
            if (resource == nullptr) continue;
            auto bytes = bytesOf(*resource);
            if (bytes.is_err()) return ca::core::Err(std::move(bytes).unwrap_err());
            entries.emplace_back(project::copyLedgerText(uri), std::move(bytes).unwrap());
        }
        auto report = ledger_.compact(request, project::serializeContainerEntries(entries),
                                      project::rfc3339UtcNow(), false);
        if (report.is_err())
            return ca::core::Err(ledgerFailure("compact ledger", std::move(report).unwrap_err()));
        return ca::core::Ok(std::move(report).unwrap());
    }
    catch (const std::exception& exception) {
        return ca::core::Err(error(WorkspaceError::Internal, "compact ledger", exception.what()));
    }
}

}   // namespace mjt::workspace
