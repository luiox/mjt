#include "pysdk_facade.hpp"

#include "pysdk_limits.hpp"

#include "mjbase/asm/remapper/resource_remapper.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/mapping/mapping_format_registry.hpp"
#include "mjbase/mapping/shadow_alias_format.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/project/analysis_db.hpp"
#include "mjbase/renamer/name_suggestion_file.hpp"
#include "mjbase/renamer/rename_engine.hpp"
#include "mjbase/renamer/shadow_alias.hpp"

#include "shared/decompiler.hpp"
#include "workspace/analysis_census.hpp"
#include "workspace/analysis_ingest.hpp"
#include "workspace/classfile_support.hpp"
#include "workspace/constant_scan.hpp"
#include "workspace/dex_domain.hpp"
#include "workspace/reference_scan.hpp"
#include "workspace/search_match.hpp"
#include "workspace/workspace.hpp"

#include "mjbase/jasm/classfile_utf8.hpp"

#include <jbc/analysis/domain.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <regex>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>

namespace mjt::pysdk {

using mjbase::model::ClassResource;
using mjbase::model::IResource;
using mjbase::model::ManifestResource;
using mjbase::model::ResourceContainer;
using mjbase::model::ResourceHelper;
using mjbase::model::UnknownResource;

namespace ir  = jbc::ir;
namespace jb  = jbc::core;
namespace jan = jbc::analysis;

// ── 会话 ─────────────────────────────────────────────────────────────────

FacadeSession::FacadeSession()
{
    // 空容器出生（new_container 契约）：条目族写面走 applyEdit 管道，其
    // ensureLoaded 对纯 new_container 会话（从不开源文件）也须直通——出生
    // 即建立 loaded 空态。openEmpty 只含空态，失败即环境坏，抛构造期异常。
    const auto born = ws.openEmpty();
    if (born.is_err())
        throw std::runtime_error("session: open empty: " + born.unwrap_err().message);
}

void FacadeSession::rebuildIndex()
{
    nameToUri.clear();
    duplicateUris.clear();
    // mjbase visitResources 回调收 string_view（mbase 是 const string&）。
    facadeContainer(*this).visitResources([&](std::string_view uriView, const IResource& resource) {
        const std::string uri(uriView);
        const auto* cr = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!cr) return;
        const auto& bytes = cr->get();
        if (bytes.empty()) return;
        auto name = facadeProbeClassName(bytes);
        if (!name || name->empty()) return;   // 头解析失败：不入索引（broken 条目）
        if (nameToUri.count(*name)) {
            duplicateUris.emplace(*name, uri);   // first-wins（#962）：保留首条目
            return;
        }
        nameToUri[*name] = uri;
    });
}

FacadeRegistry& FacadeRegistry::instance()
{
    static FacadeRegistry registry;
    return registry;
}

FacadeSession& FacadeRegistry::create()
{
    auto  session = std::make_unique<FacadeSession>();
    auto* raw     = session.get();
    raw->handle   = next_++;
    sessions_.emplace(raw->handle, std::move(session));
    return *raw;
}

FacadeSession& FacadeRegistry::get(ca::u64 handle)
{
    auto it = sessions_.find(handle);
    if (it == sessions_.end()) {
        throw StaleRefError("container handle not alive: " + std::to_string(handle));
    }
    return *it->second;
}

const FacadeSession& FacadeRegistry::get(ca::u64 handle) const
{
    auto it = sessions_.find(handle);
    if (it == sessions_.end()) {
        throw StaleRefError("container handle not alive: " + std::to_string(handle));
    }
    return *it->second;
}

void FacadeRegistry::close(ca::u64 handle) { sessions_.erase(handle); }

// ── 工具 ─────────────────────────────────────────────────────────────────

std::optional<std::string> facadeProbeClassName(const std::vector<ca::u8>& bytes)
{
    if (bytes.size() < 4) return std::nullopt;
    // 骨架解析对名（L0/L1 eager，属性全跳过——probe 不付方法体成本）。
    // 非 const：ReadResult 持 unique_ptr（move-only），unwrap 必须 move 消费
    // （const 对象 std::move 出的是 const&&，只会落到 const& 拷贝重载上）。
    auto read = jb::readClass(jb::ByteView(bytes));
    if (read.is_err()) return std::nullopt;
    // ReadResult 持 unique_ptr（move-only），且 unwrap()&& 按值返回：链式
    // `.file->thisName()` 的模型随行尾临时量析构，返回视图立即悬垂（实测
    // 前缀 3 字节落进复用堆块变成指针字节）。必须先落命名局部再取视图，
    // 字符串拷出须在模型存活期内完成。
    auto opened = std::move(read).unwrap();
    const auto name = opened.file->thisName();
    if (name.empty()) return std::nullopt;
    return std::string(name);
}

namespace {

/// 两阶段落盘（暂存 + 尺寸校验 + rename；PyJar::save 同款纪律）。
void stagedExportJar(ResourceContainer& container, const std::string& path)
{
    if (path.empty()) throw std::invalid_argument("export: path is empty");
    const auto outputPath = std::filesystem::u8path(path);
    const auto parent     = outputPath.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent)) {
        throw std::runtime_error("export: parent directory does not exist: " + path);
    }
    auto stagedPath = outputPath;
    stagedPath += ".tmp";
    try {
        const auto exported = ResourceHelper::exportToJar(container, stagedPath.generic_u8string());
        if (exported.is_err()) {
            throw std::runtime_error("export: jar write failed: "
                                     + stagedPath.generic_u8string());
        }
        std::error_code ec;
        const auto size = std::filesystem::file_size(stagedPath, ec);
        if (ec || size == 0) {
            std::filesystem::remove(stagedPath, ec);
            throw std::runtime_error("export: output was not written");
        }
        std::filesystem::rename(stagedPath, outputPath, ec);
        if (ec) {
            std::filesystem::remove(stagedPath, ec);
            throw std::runtime_error("export: staged output was not moved into place");
        }
    }
    catch (...) {
        std::error_code ec;
        std::filesystem::remove(stagedPath, ec);
        throw;
    }
}

/// 索引中 winner 名 → URI 的反向定位（无则 nullopt）。
std::optional<std::string> winnerNameFor(const FacadeSession& s, const std::string& uri)
{
    for (const auto& [name, mapped] : s.nameToUri) {
        if (mapped == uri) return name;
    }
    return std::nullopt;
}

const ClassResource* classResourceOf(const FacadeSession& s, const std::string& uri)
{
    const auto* r = facadeContainer(s).getResource(uri);
    if (!r) return nullptr;
    return ca::core::tag_cast::dyn_cast<const ClassResource>(r);
}

}   // namespace

// ── 容器/条目面 ──────────────────────────────────────────────────────────

mjbase::model::ResourceContainer& facadeContainer(FacadeSession& s)
{
    auto* container = s.ws.workingContainer();
    if (container == nullptr) throw StaleRefError("session is not open");
    return *container;
}

const mjbase::model::ResourceContainer& facadeContainer(const FacadeSession& s)
{
    const auto* container = s.ws.workingContainer();
    if (container == nullptr) throw StaleRefError("session is not open");
    return *container;
}

/// @brief 条目族写边界（W4b）：applyEdit 单入口——ResourceBytes/
///        RemoveResource 变体（字节面，无门禁，SDK 原语义原样）；失败抛
///        runtime_error（SDK 错误面）。
void applyEditEntry(FacadeSession& s, mjt::workspace::EditCommand::Op op)
{
    if (s.ws.isDexBacked()) throw std::runtime_error("APK/DEX entries are read-only");
    mjt::workspace::MutationRequest request;
    const auto applied =
        mjt::workspace::applyEdit(s.ws, mjt::workspace::EditCommand{std::move(op)}, request);
    if (applied.status.is_err())
        throw std::runtime_error("apply edit: " + applied.status.message);
}

void facadeImportJar(FacadeSession& session, const std::string& path)
{
    if (path.empty()) throw std::invalid_argument("import: path is empty");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::u8path(path), ec)) {
        throw std::runtime_error("import: not a file: " + path);
    }
    // 协作层取消检查点：importFromJar 内部循环引擎私有，检查点只能落在入口
    // （逐条目粒度进不去，这是接受的成本，见 doc_ai/pysdk-script-limits.md）。
    deadlineCheck();
    // W4 统一：open 走 container_io 注册表（jar 行 = importFromJar +
    // 原始/工作双快照）；错误口径保持迁移前文案。
    const auto imported = session.ws.open(mjt::workspace::utf8Ref(path));
    if (imported.is_err()) {
        throw std::runtime_error("import: failed: " + path + ": " +
                                 imported.unwrap_err().message);
    }
    session.sourcePath = path;
    session.rebuildIndex();
    facadeInvalidateAnalysisDb(session);   // 容器内容整体换血：库走下次查询重建
}

std::vector<std::pair<std::string, std::string>> facadeListEntries(const FacadeSession& s)
{
    std::vector<std::pair<std::string, std::string>> out;
    facadeContainer(s).visitResources([&](std::string_view uriView, const IResource& resource) {
        out.emplace_back(std::string(uriView),
                         std::string(mjbase::model::to_string(resource.getType())));
    });
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<ca::u8> facadeGetEntryBytes(const FacadeSession& s, const std::string& uri)
{
    const auto* r = facadeContainer(s).getResource(uri);
    if (!r) throw StaleRefError("entry not found: " + uri);
    if (const auto* cr = ca::core::tag_cast::dyn_cast<const ClassResource>(r)) return cr->get();
    if (const auto* mr = ca::core::tag_cast::dyn_cast<const ManifestResource>(r))
        return mr->get().toBytes();
    if (const auto* ur = ca::core::tag_cast::dyn_cast<const UnknownResource>(r)) return ur->get();
    throw std::runtime_error("entry bytes: unsupported resource type: " + uri);
}

void facadeSetEntryBytes(FacadeSession& s, const std::string& uri, std::vector<ca::u8> bytes)
{
    const auto* r = facadeContainer(s).getResource(uri);
    if (!r) throw StaleRefError("entry not found: " + uri);
    // W4b：字节置入走 applyEdit 单入口（ResourceBytes 保持原资源形态置
    // 字节——Manifest 形按文本重析，畸形文本由 handler 抛 DataLoss；类形
    // 原样置入，不设门禁）。
    // 原字节逃生门：字节形态不可预判（可能改名/畸形/换资源形态）→ 不走
    // 增量同步，库标 stale 下次查询重建（fail-open 到重建的粗粒度面）。
    const bool wasClass = ca::core::tag_cast::isa<ClassResource>(r);
    applyEditEntry(s, mjt::workspace::EditCommand::ResourceBytes{
                          mjt::workspace::utf8(uri), std::move(bytes)});
    if (wasClass) {
        // 外部改字节：作废该类缓存树（下次树操作按新字节重建）。
        if (auto name = winnerNameFor(s, uri)) s.trees.erase(*name);
    }
    facadeInvalidateAnalysisDb(s);   // 非类资源字节进资源串并集面 → 重建换快照
}

void facadeAddEntry(FacadeSession& s, const std::string& uri, std::vector<ca::u8> bytes)
{
    if (uri.empty()) throw std::invalid_argument("add entry: uri is empty");
    if (facadeContainer(s).hasResource(uri)) {
        throw StaleRefError("entry already exists: " + uri);
    }
    auto name = facadeProbeClassName(bytes);
    // W4b：条目落容器走 applyEdit（ResourceBytes 新增路径按骨架解析判形，
    // 与本处 probe 同口径）；索引/树缓存照旧由 facade 层维护。
    applyEditEntry(s, mjt::workspace::EditCommand::ResourceBytes{
                          mjt::workspace::utf8(uri), std::move(bytes)});
    if (name && !name->empty()) {
        if (s.nameToUri.count(*name)) {
            s.duplicateUris.emplace(*name, uri);   // first-wins：后者落选
        }
        else {
            s.nameToUri[*name] = uri;
        }
        facadeInvalidateAnalysisDb(s);   // 条目集合变更（结构变更面）→ 重建
        return;
    }
    facadeInvalidateAnalysisDb(s);
}

void facadeRemoveEntry(FacadeSession& s, const std::string& uri)
{
    if (!facadeContainer(s).hasResource(uri)) throw StaleRefError("entry not found: " + uri);

    // winner 移除：落选者按序晋升（JVM 语义下唯一条目即生效）。
    if (auto name = winnerNameFor(s, uri)) {
        auto next = s.duplicateUris.find(*name);
        if (next != s.duplicateUris.end()) {
            s.nameToUri[*name] = next->second;
            s.duplicateUris.erase(next);
        }
        else {
            s.nameToUri.erase(*name);
        }
        s.trees.erase(*name);   // winner/loser 字节可能不同，缓存一律作废
        facadeInvalidateAnalysisDb(s);   // 条目集合变更（结构变更面）→ 重建
        applyEditEntry(s, mjt::workspace::EditCommand::RemoveResource{
                              mjt::workspace::utf8(uri)});
        return;
    }

    // 落选者移除：仅清落选登记。
    for (auto it = s.duplicateUris.begin(); it != s.duplicateUris.end(); ++it) {
        if (it->second == uri) {
            s.duplicateUris.erase(it);
            break;
        }
    }
    applyEditEntry(s, mjt::workspace::EditCommand::RemoveResource{
                          mjt::workspace::utf8(uri)});
    facadeInvalidateAnalysisDb(s);
}

void facadeLoadClassFile(FacadeSession& s, const std::string& path,
                         std::optional<std::string> uri, bool replace)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        throw std::runtime_error("load class file: not a file: " + path);
    }
    std::ifstream in(path, std::ios::binary);
    std::vector<ca::u8> bytes((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
    if (bytes.empty()) throw std::runtime_error("load class file: empty file: " + path);

    auto name = facadeProbeClassName(bytes);
    if (!name || name->empty()) {
        throw std::runtime_error("load class file: not a class file: " + path);
    }
    if (!uri) uri = std::filesystem::u8path(path).filename().u8string();
    if (facadeContainer(s).hasResource(*uri)) {
        if (!replace) throw StaleRefError("entry already exists: " + *uri);
        facadeRemoveEntry(s, *uri);
    }
    facadeAddEntry(s, *uri, std::move(bytes));
}

std::vector<std::string> facadeClassNames(const FacadeSession& s)
{
    std::vector<std::string> out;
    if (const auto* dex = s.ws.dexDomain()) {
        out = dex->classNames();
        std::sort(out.begin(), out.end());
        return out;
    }
    out.reserve(s.nameToUri.size());
    for (const auto& [name, uri] : s.nameToUri) out.push_back(name);
    return out;
}

std::string facadeClassUri(const FacadeSession& s, const std::string& name)
{
    if (const auto* dex = s.ws.dexDomain()) {
        auto cls = dex->classInfo(name);
        if (cls.is_err()) throw StaleRefError(cls.unwrap_err().message);
        return std::move(cls).unwrap().resourceUri.to_std_string();
    }
    auto it = s.nameToUri.find(name);
    if (it == s.nameToUri.end()) throw StaleRefError("class not found: " + name);
    return it->second;
}

std::vector<std::string> facadeDuplicateNames(const FacadeSession& s)
{
    std::vector<std::string> out;
    for (const auto& [name, uri] : s.duplicateUris) out.push_back(name);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::string facadeAmbiguousDuplicateMessage(const FacadeSession& s, const std::string& name)
{
    // 候选全集 = winner（nameToUri）+ 落选者（duplicateUris 同名段，登记序）。
    // first-wins 纪律下 name 在 duplicateUris 里 ⇒ nameToUri 必有 winner。
    std::vector<std::string> candidates;
    if (const auto winner = s.nameToUri.find(name); winner != s.nameToUri.end())
        candidates.push_back(winner->second);
    const auto range = s.duplicateUris.equal_range(name);
    for (auto it = range.first; it != range.second; ++it) candidates.push_back(it->second);
    std::string out = "ambiguous duplicate class name: " + name + " (candidates:";
    for (const auto& uri : candidates) {
        out += ' ';
        out += uri;
    }
    out += " ); exclude one of the duplicate entries (remove_entry) or address them "
           "by explicit entry uri";
    return out;
}

std::vector<ca::u8> facadeClassBytes(const FacadeSession& s, const std::string& name)
{
    if (s.duplicateUris.count(name)) {
        throw StaleRefError(facadeAmbiguousDuplicateMessage(s, name));
    }
    const auto* cr = classResourceOf(s, facadeClassUri(s, name));
    if (!cr) throw std::runtime_error("not a class entry: " + name);
    return cr->get();
}

void facadeSetClassBytes(FacadeSession& s, const std::string& name, std::vector<ca::u8> bytes)
{
    if (s.duplicateUris.count(name)) {
        throw StaleRefError(facadeAmbiguousDuplicateMessage(s, name));
    }
    auto probed = facadeProbeClassName(bytes);
    if (!probed || *probed != name) {
        throw std::runtime_error("set class bytes: bytes are not a class file for " + name);
    }
    auto it = s.nameToUri.find(name);
    if (it != s.nameToUri.end()) {
        const auto* r = facadeContainer(s).getResource(it->second);
        if (!ca::core::tag_cast::isa<ClassResource>(r)) {
            throw std::runtime_error("not a class entry: " + name);
        }
        // W4b：替换走 applyEdit（原条目为类形 → ResourceBytes 保持类形置入）。
        applyEditEntry(s, mjt::workspace::EditCommand::ResourceBytes{
                              mjt::workspace::utf8(it->second), std::move(bytes)});
        s.trees.erase(name);   // 外部改字节：作废缓存树
        // 内容编辑边界（既有类字节整体替换）：增量同步追平该类事实。
        facadeSyncAnalysisDbAfterEdit(s, name);
        return;
    }
    // 新增类（v2 缺口修复）：URI = name + ".class"。
    const std::string uri = name + ".class";
    if (facadeContainer(s).hasResource(uri)) {
        throw StaleRefError("entry already exists: " + uri);
    }
    applyEditEntry(s, mjt::workspace::EditCommand::ResourceBytes{
                          mjt::workspace::utf8(uri), std::move(bytes)});
    s.nameToUri[name] = uri;
    facadeInvalidateAnalysisDb(s);   // 条目集合变更（结构变更面）→ 重建
}

void facadeExportJar(FacadeSession& s, const std::string& path,
                     const std::vector<std::string>& uris)
{
    if (s.ws.isDexBacked()) throw std::runtime_error("JAR export requires JVM input");
    // 导出即全量语义（实测补刀盘：不 flush 的 export 静默丢掉脏树/挂账改名）。
    // 帧重算失败即抛（fail-loud 优于静默出坏罐）；要跳过帧重算可先
    // flush(compute_frames=False) 再 export。
    facadeFlush(s, true);
    facadeSyncEntries(s);
    if (uris.empty()) {
        stagedExportJar(facadeContainer(s), path);
        return;
    }
    // 子集导出：Class/Manifest/Unknown 深拷贝入临时容器。
    ResourceContainer subset;
    for (const auto& uri : uris) {
        const auto* r = facadeContainer(s).getResource(uri);
        if (!r) throw StaleRefError("entry not found: " + uri);
        if (const auto* cr = ca::core::tag_cast::dyn_cast<const ClassResource>(r)) {
            subset.addResource(uri, std::make_unique<ClassResource>(uri, cr->get()));
        }
        else if (const auto* mr = ca::core::tag_cast::dyn_cast<const ManifestResource>(r)) {
            subset.addResource(uri, std::make_unique<ManifestResource>(mr->get()));
        }
        else if (const auto* ur = ca::core::tag_cast::dyn_cast<const UnknownResource>(r)) {
            subset.addResource(uri, std::make_unique<UnknownResource>(uri, ur->get()));
        }
        else {
            throw std::runtime_error("subset export: unsupported resource type: " + uri);
        }
    }
    stagedExportJar(subset, path);
}

void facadeExportClass(FacadeSession& s, const std::string& name, const std::string& path)
{
    // 脏树先写回再导出（save/flush 同一纪律）；挂账改名一并落账。
    auto it = s.trees.find(name);
    if (it != s.trees.end() && it->second.dirty) facadeWriteBack(s, name, true);
    facadeSyncEntries(s);
    ResourceHelper::exportToClassFile(facadeContainer(s), path, facadeClassUri(s, name));
}

// ═══════════════════════════════════════════════════════════════════════
// 树面（B2，契约 §2.2-2.6）：lazy 解析 + 键重解析 fail-closed + 脏纪律。
//
// RFC 0002 B6：底座 = libjbc ClassSession（backing ClassFile + ClassUnit
// 骨架 + 方法级 promote/lower）。指令键 = InsnId/LabelId 句柄；uid 映射在
// MethodUi 物化期建立、编辑期增量维护（墓地在 addrOf 留底，解析即抛）。
// 行序视图（label/line 与指令同列）沿用旧 ASM 树列表语义：pos=uid 插其前；
// 标签锚点动态跟随「行位在插入点之前的末锚标签改锚新指令」。
// ═══════════════════════════════════════════════════════════════════════

namespace {

constexpr int kAccStatic = 0x0008;

/// 专用局部变量码（iload_0 族）：0x1A-0x2D 载入 / 0x3B-0x4E 存储，每族 4 连号。
bool isDedicatedVarOp(jb::Op op)
{
    const int v = static_cast<int>(op);
    return (v >= 0x1A && v <= 0x2D) || (v >= 0x3B && v <= 0x4E);
}

/// 专用 var 码的槽位（编码在 opcode 字节里；libjbc 导入不落 Insn.a）。
int dedicatedVarSlot(jb::Op op)
{
    const int v = static_cast<int>(op);
    return v < 0x2E ? (v - 0x1A) % 4 : (v - 0x3B) % 4;
}

/// 指令 kind 外显值（ASM 系编码，Python 契约按值消费）。
int kindOf(jb::Op op)
{
    switch (op) {
    case jb::Op::LDC:
    case jb::Op::LDC_W:
    case jb::Op::LDC2_W:
        return insn_kind::kLdc;
    case jb::Op::NEW:
    case jb::Op::ANEWARRAY:
    case jb::Op::CHECKCAST:
    case jb::Op::INSTANCEOF:
        return insn_kind::kType;
    case jb::Op::GETSTATIC:
    case jb::Op::PUTSTATIC:
    case jb::Op::GETFIELD:
    case jb::Op::PUTFIELD:
        return insn_kind::kField;
    case jb::Op::INVOKEVIRTUAL:
    case jb::Op::INVOKESPECIAL:
    case jb::Op::INVOKESTATIC:
    case jb::Op::INVOKEINTERFACE:
        return insn_kind::kMethod;
    case jb::Op::INVOKEDYNAMIC:
        return insn_kind::kIndy;
    case jb::Op::IINC:
        return insn_kind::kIinc;
    case jb::Op::TABLESWITCH:
        return insn_kind::kTableSwitch;
    case jb::Op::LOOKUPSWITCH:
        return insn_kind::kLookupSwitch;
    case jb::Op::MULTIANEWARRAY:
        return insn_kind::kMultiANewArray;
    default:
        break;
    }
    // NEWARRAY 走 ASM IntInsn 形态（atype 载荷在 operand 位）。
    if (op == jb::Op::NEWARRAY) return insn_kind::kInt;
    const auto sh = jb::shapeOf(op);
    if (sh == jb::Shape::Branch2 || sh == jb::Shape::Branch4) return insn_kind::kJump;
    // 专用局部变量码（iload_0 族，shape None）：按 var 族呈现——合成语料
    // （javac 口径折叠）与通用码两态并存，呈现层归一保判据门单口径。
    if (isDedicatedVarOp(op)) return insn_kind::kVar;
    if (sh == jb::Shape::Imm1 || sh == jb::Shape::Imm2) return insn_kind::kInt;
    return insn_kind::kInsn;
}

bool isControlOp(jb::Op op)
{
    const auto sh = jb::shapeOf(op);
    return sh == jb::Shape::Branch2 || sh == jb::Shape::Branch4 ||
           sh == jb::Shape::TableSwitch || sh == jb::Shape::LookupSwitch;
}

/// Insn 直排便利件（kNoPc = 编辑新建，无源布局；与 pysdk_jar 合成面同款）。
ir::Insn mkInsn(jb::Op op)
{
    ir::Insn in;
    in.op    = op;
    in.srcPc = ir::kNoPc;
    return in;
}

/// lazy 解析：首触建缓存（契约 §3.1 单一真相：树只在缓存里一份）。
TreeEntry& resolveTree(FacadeSession& s, const std::string& name)
{
    if (s.duplicateUris.count(name)) {
        throw StaleRefError(facadeAmbiguousDuplicateMessage(s, name));
    }
    auto it = s.trees.find(name);
    if (it != s.trees.end()) return it->second;
    auto uriIt = s.nameToUri.find(name);
    if (uriIt == s.nameToUri.end()) throw StaleRefError("class not found: " + name);
    const auto* cr = classResourceOf(s, uriIt->second);
    if (!cr) throw std::runtime_error("not a class entry: " + name);
    const auto& bytes   = cr->get();
    auto        opened  = ir::makeSessionFromBytes(jb::ByteView(bytes));
    if (opened.is_err()) {
        throw std::runtime_error("parse failed for " + name + ": session build failed");
    }
    auto session = std::move(opened).unwrap();
    // 名符校验（零分配：符号池字节视图逐字节等值）。
    auto& unit = session->unit();
    if (unit.symbols().str(unit.thisClass) != name) {
        throw std::runtime_error("parsed class name mismatch for " + name);
    }
    TreeEntry entry;
    entry.session = std::move(session);
    entry.dirty   = false;
    return s.trees.emplace(name, std::move(entry)).first->second;
}

struct MethodRef
{
    TreeEntry*   te = nullptr;
    ir::MethodId id;
};

ir::MethodUnit& methodUnitOf(TreeEntry& te, ir::MethodId id)
{
    auto* mu = te.session->unit().method(id);
    if (!mu) throw StaleRefError("method handle not alive");
    return *mu;
}

MethodRef resolveMethod(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc)
{
    TreeEntry& te = resolveTree(s, cls);
    const auto id = te.session->unit().findMethodId(name, desc);
    if (id == ir::kNoMethodId) {
        throw StaleRefError("method not found: " + cls + "." + name + desc);
    }
    return MethodRef{&te, id};
}

ir::FieldId resolveField(FacadeSession& s, const std::string& cls, const std::string& name,
                         const std::string& desc)
{
    TreeEntry& te = resolveTree(s, cls);
    const auto id = te.session->unit().findFieldId(name, desc);
    if (id == ir::kNoFieldId) {
        throw StaleRefError("field not found: " + cls + "." + name);
    }
    return id;
}

bool methodExists(TreeEntry& te, const std::string& name, const std::string& desc)
{
    return te.session->unit().findMethodId(name, desc) != ir::kNoMethodId;
}

bool fieldExists(TreeEntry& te, const std::string& name, const std::string& desc)
{
    return te.session->unit().findFieldId(name, desc) != ir::kNoFieldId;
}

/// LNT 属性读取（物化期的 line 投影源）：Code 嵌套属性里找 LineNumberTable。
const jb::LineNumberTableAttr* lntOf(TreeEntry& te, const ir::MethodUnit& mu)
{
    auto& mi = te.session->backing().methods[mu.backingIndex];
    auto  ca = te.session->backing().decodeCode(mi);
    if (ca.is_err()) return nullptr;
    for (auto& slot : ca.unwrap()->attrs) {
        if (te.session->backing().utf8(slot.nameCp) != "LineNumberTable") continue;
        const auto decoded = te.session->backing().decodeAttr(slot);
        if (decoded.is_err()) return nullptr;
        return ca::core::tag_cast::dyn_cast<const jb::LineNumberTableAttr>(decoded.unwrap());
    }
    return nullptr;
}

/// uid 状态按需物化（契约 §2.4 每方法单调 uid；行序 = 旧 ASM 树列表语义：
/// 逐指令「前锚标签（槽序）+ startPc 相合的 LNT 行 + 指令」，尾部 = end 锚
/// 标签 + 余量 LNT 行）。标签并集 = 分支/switch 目标 + 异常表三界。
MethodUi& materialize(TreeEntry& te, ir::MethodUnit& mu)
{
    auto& st = te.uidStates[mu.id.slot];
    if (st.materialized) return st;
    st.materialized = true;
    if (!mu.code) {
        // 无 Code 属性（abstract/native）：空视图（计数 0，编辑面在
        // requireCode 挡掉）。promote 前置闸：先 hasCode 分流，promote 的
        // Err 只剩真失败。已 promote 的不重入（promote 幂等 = 重建，会作废
        // 全部句柄）。
        if (ir::hasCode(*te.session, mu.backingIndex)) {
            const auto promoted = ir::promoteMethod(*te.session, mu.id);
            if (promoted.is_err()) {
                throw std::runtime_error(std::string("promote failed: ") +
                                         ir::to_string(promoted.unwrap_err()));
            }
        }
        else {
            return st;
        }
    }

    ir::CodeUnit& cu  = *mu.code;
    ir::CodeDom&  dom = cu.dom;

    // 标签并集去重（slot 为键；map 迭代即槽序）。
    std::map<ca::u32, ir::LabelId> labels;
    const auto take = [&](ir::LabelId l) {
        if (l == ir::kNoLabel) return;
        if (!dom.validLabel(l)) return;
        labels.emplace(l.slot, l);
    };
    for (const auto id : dom.order()) {
        const ir::Insn* in = dom.peek(id);
        if (!in) continue;
        take(in->target);
        for (const auto t : in->targets) take(t);
    }
    for (const auto& e : cu.exc) {
        take(e.start);
        take(e.end);
        take(e.handler);
    }
    std::map<ca::u32, std::vector<ir::LabelId>> atInsn;   // 锚指令槽 → 标签（槽序）
    std::vector<ir::LabelId>                    atEnd;
    for (const auto& [slot, l] : labels) {
        const auto target = dom.resolveLabel(l);
        if (target.is_err()) continue;
        if (target.unwrap().isEnd()) {
            atEnd.push_back(l);
        }
        else {
            atInsn[target.unwrap().insn.slot].push_back(l);
        }
    }

    // LNT 行按 pc 入账（multimap 保属性行序；行号投影期间消费）。
    const auto* lnt = lntOf(te, mu);
    if (lnt) {
        for (ca::usize i = 0; i < lnt->rows.size(); ++i) {
            st.pendingLines.emplace(lnt->rows[i].startPc, i);
        }
    }

    ca::u32 uid = st.nextUid;
    const auto allocLabel = [&](ir::LabelId l) {
        InsnAddr addr;
        addr.kind  = InsnAddr::Slot::Label;
        addr.label = l;
        st.addrOf.emplace(uid, addr);
        st.uidOfLabelSlot.emplace(l.slot, uid);
        st.order.push_back(uid);
        ++uid;
    };
    const auto allocLine = [&](ca::u32 pc, int line) {
        InsnAddr addr;
        addr.kind   = InsnAddr::Slot::Line;
        addr.linePc = pc;
        addr.lineNo = line;
        st.addrOf.emplace(uid, addr);
        st.order.push_back(uid);
        ++uid;
    };
    const auto allocInsn = [&](ir::InsnId id) {
        InsnAddr addr;
        addr.kind = InsnAddr::Slot::Insn;
        addr.insn = id;
        st.addrOf.emplace(uid, addr);
        st.uidOfInsnSlot.emplace(id.slot, uid);
        st.order.push_back(uid);
        ++uid;
    };

    for (const auto id : dom.order()) {
        const ir::Insn* in = dom.peek(id);
        if (!in) continue;
        const auto anchors = atInsn.find(id.slot);
        if (anchors != atInsn.end()) {
            for (const auto l : anchors->second) allocLabel(l);
        }
        if (lnt && in->srcPc != ir::kNoPc) {
            for (auto it = st.pendingLines.lower_bound(in->srcPc);
                 it != st.pendingLines.end() && it->first == in->srcPc;) {
                allocLine(it->first, static_cast<int>(lnt->rows[it->second].line));
                it = st.pendingLines.erase(it);
            }
        }
        allocInsn(id);
    }
    for (const auto l : atEnd) allocLabel(l);
    if (lnt) {
        // 锚不进存活布局的余量行（死 pc / 末尾 pc）：行序尾部兜底投影。
        for (const auto& [pc, row] : st.pendingLines) {
            allocLine(pc, static_cast<int>(lnt->rows[row].line));
        }
        st.pendingLines.clear();
    }
    st.nextUid = uid;
    return st;
}

MethodUi& stateFor(MethodRef r) { return materialize(*r.te, methodUnitOf(*r.te, r.id)); }

void markDirty(MethodRef r) { r.te->dirty = true; }

/// uid → 身份解析（墓地在 addrOf 留底：解析即抛，不复活）。
const InsnAddr& resolveAddr(MethodRef r, ca::u32 uid)
{
    auto& st = stateFor(r);
    auto  it = st.addrOf.find(uid);
    if (it == st.addrOf.end()) {
        throw StaleRefError("no such insn uid: " + std::to_string(uid));
    }
    const InsnAddr& a = it->second;
    if (a.kind == InsnAddr::Slot::Insn) {
        auto& dom = methodUnitOf(*r.te, r.id).code->dom;
        if (dom.peek(a.insn) == nullptr) {
            throw StaleRefError("insn removed: uid " + std::to_string(uid));
        }
    }
    else if (a.kind == InsnAddr::Slot::Label) {
        auto& dom = methodUnitOf(*r.te, r.id).code->dom;
        if (!dom.validLabel(a.label)) {
            throw StaleRefError("label removed: uid " + std::to_string(uid));
        }
    }
    return a;
}

/// 带 Code 体的方法视图（abstract/native 的编辑面在此挡掉）。
ir::CodeUnit& requireCode(MethodRef r)
{
    auto& mu = methodUnitOf(*r.te, r.id);
    materialize(*r.te, mu);
    if (!mu.code) {
        throw StaleRefError("method has no code body: " +
                            std::string(r.te->session->unit().symbols().str(mu.name)));
    }
    return *mu.code;
}

/// uid → 存活指令句柄（写面用）。
ir::InsnId liveInsn(MethodRef r, ca::u32 uid)
{
    const auto& a = resolveAddr(r, uid);
    if (a.kind != InsnAddr::Slot::Insn) {
        throw std::runtime_error("uid " + std::to_string(uid) + " is not an insn row");
    }
    return a.insn;
}

ir::LabelId liveLabel(MethodRef r, ca::u32 uid)
{
    const auto& a = resolveAddr(r, uid);
    if (a.kind != InsnAddr::Slot::Label) {
        throw std::runtime_error("uid " + std::to_string(uid) + " is not a label row");
    }
    return a.label;
}

/// 行序重建（rowOf 与 order 同步；O(n)，方法体规模下足够）。
void rebuildRowOf(MethodUi& st)
{
    st.rowOf.clear();
    for (ca::usize i = 0; i < st.order.size(); ++i) {
        st.rowOf[st.order[i]] = i;
    }
}

/// srcPc → 现存指令句柄（Line 行插入锚的回定位）；miss 显式抛。
ir::InsnId findInsnBySrcPc(ir::CodeDom& dom, ca::u32 pc)
{
    for (const auto id : dom.order()) {
        const ir::Insn* in = dom.peek(id);
        if (in && in->srcPc == pc) return id;
    }
    throw StaleRefError("line anchor pc is no longer live: " + std::to_string(pc));
}

/// 发射公共尾：行插位 → DOM 落位 → 行/addr 簿记 → 末锚标签回锚。
///
/// 前向控制目标处理：libjbc checkHandles 拒绝 end 锚分支目标（要求锚在存活
/// 指令），而旧 ASM 树的标签锚是「行位之后第一条指令」的动态事实。桥接：
/// 发射前把 end 锚目标临时绑到尾指令放行校验，插入后按行序回锚——行位在
/// 插入点之前的 end 锚标签改锚新指令（标签领地 = 其行位起的第一条指令），
/// 未回锚的 pinned 目标恢复 end 锚。
ca::u32 emitAt(MethodRef r, ir::Insn in, std::optional<ca::u32> pos)
{
    auto& te  = *r.te;
    auto& st  = stateFor(r);
    auto& cu  = requireCode(r);
    auto& dom = cu.dom;

    ca::usize  rowIndex = st.order.size();
    ir::InsnId before   = ir::kNoInsn;
    if (pos) {
        const auto& a = resolveAddr(r, *pos);
        rowIndex = st.rowOf[*pos];
        if (a.kind == InsnAddr::Slot::Insn) {
            before = a.insn;
        }
        else if (a.kind == InsnAddr::Slot::Label) {
            // 行插在其前；end 锚标签没有实体位置 → 尾插（与旧列表语义一致：
            // 后续发射按行序把标签改锚到新指令）。
            const auto t = dom.resolveLabel(a.label);
            if (t.is_err()) throw StaleRefError("label removed: uid " + std::to_string(*pos));
            if (!t.unwrap().isEnd()) before = t.unwrap().insn;
        }
        else {
            before = findInsnBySrcPc(dom, a.linePc);
        }
    }

    std::vector<ir::LabelId> pinned;
    if (isControlOp(in.op)) {
        const auto pinTargets = [&](ir::LabelId l) {
            if (l == ir::kNoLabel || !dom.validLabel(l)) return;
            const auto t = dom.resolveLabel(l);
            if (t.is_err() || !t.unwrap().isEnd()) return;
            const auto tail = dom.last();
            if (tail == ir::kNoInsn) {
                throw StaleRefError("forward branch target has no anchor insn yet");
            }
            if (dom.bindLabel(l, tail).is_err()) {
                throw std::runtime_error("emit: failed to stage forward target");
            }
            pinned.push_back(l);
        };
        pinTargets(in.target);
        for (const auto t : in.targets) pinTargets(t);
    }

    const auto placed = before != ir::kNoInsn ? dom.insertBefore(before, std::move(in))
                                              : dom.append(std::move(in));
    if (placed.is_err()) {
        throw std::runtime_error(std::string("emit failed: ") +
                                 ir::to_string(placed.unwrap_err()));
    }
    const ir::InsnId nid = placed.unwrap();

    const ca::u32 uid = st.nextUid++;
    InsnAddr      addr;
    addr.kind = InsnAddr::Slot::Insn;
    addr.insn = nid;
    st.addrOf.emplace(uid, addr);
    st.uidOfInsnSlot.emplace(nid.slot, uid);
    st.order.insert(st.order.begin() + static_cast<std::ptrdiff_t>(rowIndex), uid);
    rebuildRowOf(st);

    // 回锚一：既有 end 锚标签（行位在插入点之前）→ 改锚新指令。
    for (auto& [luid, la] : st.addrOf) {
        if (la.kind != InsnAddr::Slot::Label) continue;
        if (std::find(pinned.begin(), pinned.end(), la.label) != pinned.end()) continue;
        const auto rowIt = st.rowOf.find(luid);
        if (rowIt == st.rowOf.end() || rowIt->second >= rowIndex) continue;
        const auto t = dom.resolveLabel(la.label);
        if (t.is_err() || !t.unwrap().isEnd()) continue;
        if (dom.bindLabel(la.label, nid).is_err()) {
            throw std::runtime_error("emit: failed to re-anchor boundary label");
        }
    }
    // 回锚二：pinned 前向目标——行位在插入点之前 = 新指令即其领地首指令；
    // 否则恢复 end 锚（发射目标仍在方法末尾边界）。
    for (const auto l : pinned) {
        const auto luidIt = st.uidOfLabelSlot.find(l.slot);
        if (luidIt == st.uidOfLabelSlot.end()) continue;
        const auto rowIt = st.rowOf.find(luidIt->second);
        const auto bound = (rowIt != st.rowOf.end() && rowIt->second < rowIndex)
                               ? dom.bindLabel(l, nid)
                               : dom.bindLabel(l, ir::kNoInsn);
        if (bound.is_err()) {
            throw std::runtime_error("emit: failed to settle forward target");
        }
    }

    markDirty(r);
    return uid;
}

/// 分支目标改指（emit_jump 后的 set_target 面）：end 锚目标的 pin/restore。
void retarget(MethodRef r, ca::u32 uid, ca::u32 targetUid)
{
    auto& cu  = requireCode(r);
    auto& dom = cu.dom;
    const auto& a  = resolveAddr(r, uid);
    const auto& ta = resolveAddr(r, targetUid);
    if (a.kind != InsnAddr::Slot::Insn || ta.kind != InsnAddr::Slot::Label) {
        throw std::runtime_error("insn_set_target: kind mismatch");
    }
    auto*     in = dom.get(a.insn).unwrap();
    const auto sh = jb::shapeOf(in->op);
    if (sh != jb::Shape::Branch2 && sh != jb::Shape::Branch4) {
        throw std::runtime_error("insn_set_target: kind mismatch (" +
                                 std::string(facadeInsnKindName(kindOf(in->op))) + ")");
    }
    const bool endAnchored = [&] {
        const auto t = dom.resolveLabel(ta.label);
        return t.is_ok() && t.unwrap().isEnd();
    }();
    if (endAnchored) {
        const auto tail = dom.last();
        if (tail == ir::kNoInsn) {
            throw StaleRefError("forward branch target has no anchor insn yet");
        }
        if (dom.bindLabel(ta.label, tail).is_err()) {
            throw std::runtime_error("set_target: failed to stage forward target");
        }
    }
    in->target = ta.label;
    if (endAnchored) {
        // set_target 不插入指令：目标行位不动 → 恒恢复 end 锚。
        if (dom.bindLabel(ta.label, ir::kNoInsn).is_err()) {
            throw std::runtime_error("set_target: failed to restore target anchor");
        }
    }
    markDirty(r);
}

// ── 读 ──

ClassInfo classInfoOf(TreeEntry& te)
{
    auto&      unit = te.session->unit();
    auto&      sp   = unit.symbols();
    ClassInfo  out;
    out.name      = std::string(sp.str(unit.thisClass));
    out.superName = std::string(sp.str(unit.superClass));
    // 旧树版本口径：低 16 位 = major，高 16 位 = minor（libjbc 打包相反）。
    out.version = static_cast<int>(((unit.version & 0xFFFFu) << 16) | (unit.version >> 16));
    out.access  = static_cast<int>(unit.access);
    for (const auto itf : unit.interfaces) out.interfaces.push_back(std::string(sp.str(itf)));
    return out;
}

MemberInfo methodInfoOf(TreeEntry& te, ir::MethodId id)
{
    auto&      mu = methodUnitOf(te, id);
    auto&      cf = te.session->backing();
    auto&      sp = te.session->unit().symbols();
    MemberInfo out;
    out.name   = std::string(sp.str(mu.name));
    out.desc   = std::string(sp.str(mu.desc));
    out.access = static_cast<int>(mu.access);
    if (mu.code) {
        out.maxStack  = mu.code->maxStack;
        out.maxLocals = mu.code->maxLocals;
    }
    else {
        // 无 Code 属性（abstract/native）时 decodeCode 返回 Ok(nullptr)：
        // Result ok ≠ 指针非空，解引用前必须判空（实测 SEH → pybind
        // "Caught an unknown exception!"）。
        auto ca = cf.decodeCode(cf.methods[mu.backingIndex]);
        if (ca.is_ok() && ca.unwrap() != nullptr) {
            out.maxStack  = ca.unwrap()->maxStack;
            out.maxLocals = ca.unwrap()->maxLocals;
        }
    }
    // Exceptions 属性 → 类名列表（CP Class 逐条解析）。
    auto& mi = cf.methods[mu.backingIndex];
    for (auto& slot : mi.attrs) {
        if (cf.utf8(slot.nameCp) != "Exceptions") continue;
        const auto decoded = cf.decodeAttr(slot);
        if (decoded.is_err()) break;
        const auto* exc =
            ca::core::tag_cast::dyn_cast<const jb::ExceptionsAttr>(decoded.unwrap());
        if (!exc) break;
        for (const auto cpIndex : exc->exceptionCps) {
            out.exceptions.emplace_back(cf.className(cpIndex));
        }
        break;
    }
    return out;
}

InsnDetail detailOf(MethodRef r, ca::u32 uid)
{
    auto&        te  = *r.te;
    auto&        st  = stateFor(r);
    auto&        cu  = requireCode(r);
    ir::CodeDom& dom = cu.dom;
    const auto&  a   = resolveAddr(r, uid);
    InsnDetail   d;
    d.uid = uid;
    if (a.kind == InsnAddr::Slot::Label) {
        d.kind   = insn_kind::kLabel;
        d.opcode = -1;
        return d;
    }
    if (a.kind == InsnAddr::Slot::Line) {
        d.kind   = insn_kind::kLine;
        d.opcode = -1;
        d.line   = a.lineNo;
        return d;
    }
    ir::Insn* in = dom.get(a.insn).unwrap();
    d.opcode     = static_cast<int>(in->op);
    d.kind       = kindOf(in->op);
    const auto uidOfLabel = [&](ir::LabelId l) {
        if (l == ir::kNoLabel) return ca::u32{0};
        auto it = st.uidOfLabelSlot.find(l.slot);
        return it != st.uidOfLabelSlot.end() ? it->second : ca::u32{0};
    };
    switch (d.kind) {
    case insn_kind::kInt:
        d.operand = in->a;
        break;
    case insn_kind::kVar:
        // 专用 var 码槽位从 opcode 推导（导入不落 a）；通用码读 a。
        d.var = isDedicatedVarOp(in->op) ? dedicatedVarSlot(in->op) : in->a;
        break;
    case insn_kind::kType:
    case insn_kind::kMultiANewArray:
        d.desc = std::string(te.session->classOf(*in));
        if (d.kind == insn_kind::kMultiANewArray) d.dims = in->b;
        break;
    case insn_kind::kField:
    case insn_kind::kMethod:
    case insn_kind::kIndy: {
        ir::ClassSession::MemberSem sem;
        if (te.session->memberOf(*in, sem)) {
            d.owner = std::string(sem.owner);
            d.name  = std::string(sem.name);
            d.desc  = std::string(sem.desc);
            d.itf   = sem.kind == ir::ClassSession::MemberSem::Kind::InterfaceMethod;
        }
        break;
    }
    case insn_kind::kJump:
        d.targetUid = uidOfLabel(in->target);
        break;
    case insn_kind::kLdc: {
        ir::ClassSession::ConstSem c;
        if (te.session->constOf(*in, c)) {
            switch (c.kind) {
            case ir::ClassSession::ConstSem::Kind::String:
                d.cstTag = 1;
                d.cstStr = std::string(c.str);
                break;
            case ir::ClassSession::ConstSem::Kind::Int:
                d.cstTag = 2;
                d.cstInt = c.num;
                break;
            case ir::ClassSession::ConstSem::Kind::Long:
                d.cstTag = 3;
                d.cstInt = c.num;
                break;
            case ir::ClassSession::ConstSem::Kind::Float:
                d.cstTag = 4;
                if (const auto bits = static_cast<ca::u32>(c.bits); true) {
                    std::memcpy(&d.cstFloat, &bits, sizeof(float));
                }
                break;
            case ir::ClassSession::ConstSem::Kind::Double:
                d.cstTag = 5;
                std::memcpy(&d.cstFloat, &c.bits, sizeof(double));
                break;
            default:
                break;
            }
        }
        break;
    }
    case insn_kind::kIinc:
        d.var  = in->a;
        d.incr = in->b;
        break;
    case insn_kind::kTableSwitch:
        d.switchMin  = in->a;
        d.switchMax  = in->a + static_cast<int>(in->targets.size()) - 1;
        d.defaultUid = uidOfLabel(in->target);
        for (const auto t : in->targets) d.targetUids.push_back(uidOfLabel(t));
        break;
    case insn_kind::kLookupSwitch:
        d.switchKeys = in->keys;
        d.defaultUid = uidOfLabel(in->target);
        for (const auto t : in->targets) d.targetUids.push_back(uidOfLabel(t));
        break;
    default:
        break;
    }
    return d;
}

/// ldc 载荷 Any → CP 槽（str/int/i64/float/double 五族；其余显式拒收）。
ca::u32 cpOfAny(TreeEntry& te, const ca::core::Any& value)
{
    auto& session = *te.session;
    if (const auto* v = value.as<ca::str::Utf8StringRef>()) {
        return session.internString(
            std::string_view(reinterpret_cast<const char*>(v->data()), v->byte_length()));
    }
    if (value.is<int>()) return session.internInteger(*value.as<int>());
    if (value.is<ca::i64>()) return session.internLong(*value.as<ca::i64>());
    if (value.is<float>()) {
        ca::u32 bits = 0;
        std::memcpy(&bits, value.as<float>(), sizeof(float));
        return session.internFloatBits(bits);
    }
    if (value.is<double>()) {
        ca::u64 bits = 0;
        std::memcpy(&bits, value.as<double>(), sizeof(double));
        return session.internDoubleBits(bits);
    }
    throw std::runtime_error("ldc payload must be str/int/float");
}

}   // namespace

const char* facadeInsnKindName(int kind)
{
    switch (kind) {
    case insn_kind::kInsn: return "insn";
    case insn_kind::kInt: return "int";
    case insn_kind::kVar: return "var";
    case insn_kind::kType: return "type";
    case insn_kind::kField: return "field";
    case insn_kind::kMethod: return "method";
    case insn_kind::kIndy: return "indy";
    case insn_kind::kJump: return "jump";
    case insn_kind::kLabel: return "label";
    case insn_kind::kLdc: return "ldc";
    case insn_kind::kIinc: return "iinc";
    case insn_kind::kTableSwitch: return "tableswitch";
    case insn_kind::kLookupSwitch: return "lookupswitch";
    case insn_kind::kMultiANewArray: return "multianewarray";
    case insn_kind::kFrame: return "frame";
    case insn_kind::kLine: return "line";
    default: return "unknown";
    }
}

/// 专用 var 码的呈现助记名 = 基名（iload_0 → iload）；其余原样。
const char* facadeInsnMnemonic(int op)
{
    static const char* kDedicatedBases[] = {
        "iload", "lload", "fload", "dload", "aload",
        "istore", "lstore", "fstore", "dstore", "astore",
    };
    if (op >= 0x1A && op <= 0x2D) return kDedicatedBases[(op - 0x1A) / 4];
    if (op >= 0x3B && op <= 0x4E) return kDedicatedBases[5 + (op - 0x3B) / 4];
    return facadeOpcodeName(op);
}

const char* facadeOpcodeName(int op)
{
    // 标准 JVM opcode 助记名表（索引=opcode 值，JVM 规范序 0..201）。
    static const char* kNames[] = {
        "nop", "aconst_null", "iconst_m1", "iconst_0", "iconst_1", "iconst_2", "iconst_3",
        "iconst_4", "iconst_5", "lconst_0", "lconst_1", "fconst_0", "fconst_1", "fconst_2",
        "dconst_0", "dconst_1", "bipush", "sipush", "ldc", "ldc_w", "ldc2_w", "iload", "lload",
        "fload", "dload", "aload", "iload_0", "iload_1", "iload_2", "iload_3", "lload_0",
        "lload_1", "lload_2", "lload_3", "fload_0", "fload_1", "fload_2", "fload_3", "dload_0",
        "dload_1", "dload_2", "dload_3", "aload_0", "aload_1", "aload_2", "aload_3", "iaload",
        "laload", "faload", "daload", "aaload", "baload", "caload", "saload", "istore", "lstore",
        "fstore", "dstore", "astore", "istore_0", "istore_1", "istore_2", "istore_3", "lstore_0",
        "lstore_1", "lstore_2", "lstore_3", "fstore_0", "fstore_1", "fstore_2", "fstore_3",
        "dstore_0", "dstore_1", "dstore_2", "dstore_3", "astore_0", "astore_1", "astore_2",
        "astore_3", "iastore", "lastore", "fastore", "dastore", "aastore", "bastore", "castore",
        "sastore", "pop", "pop2", "dup", "dup_x1", "dup_x2", "dup2", "dup2_x1", "dup2_x2",
        "swap", "iadd", "ladd", "fadd", "dadd", "isub", "lsub", "fsub", "dsub", "imul", "lmul",
        "fmul", "dmul", "idiv", "ldiv", "fdiv", "ddiv", "irem", "lrem", "frem", "drem", "ineg",
        "lneg", "fneg", "dneg", "ishl", "lshl", "ishr", "lshr", "iushr", "lushr", "iand", "land",
        "ior", "lor", "ixor", "lxor", "iinc", "i2l", "i2f", "i2d", "l2i", "l2f", "l2d", "f2i",
        "f2l", "f2d", "d2i", "d2l", "d2f", "i2b", "i2c", "i2s", "lcmp", "fcmpl", "fcmpg",
        "dcmpl", "dcmpg", "ifeq", "ifne", "iflt", "ifge", "ifgt", "ifle", "if_icmpeq",
        "if_icmpne", "if_icmplt", "if_icmpge", "if_icmpgt", "if_icmple", "if_acmpeq",
        "if_acmpne", "goto", "jsr", "ret", "tableswitch", "lookupswitch", "ireturn", "lreturn",
        "freturn", "dreturn", "areturn", "return", "getstatic", "putstatic", "getfield",
        "putfield", "invokevirtual", "invokespecial", "invokestatic", "invokeinterface",
        "invokedynamic", "new", "newarray", "anewarray", "arraylength", "athrow", "checkcast",
        "instanceof", "monitorenter", "monitorexit", "wide", "multianewarray", "ifnull",
        "ifnonnull", "goto_w", "jsr_w",
    };
    if (op < 0 || op >= static_cast<int>(sizeof(kNames) / sizeof(kNames[0]))) return "?";
    return kNames[op];
}

ca::core::Any facadeInternedStr(FacadeSession& s, const std::string& v)
{
    // 按 byte_length 入池：ldc 字符串载荷是 MUTF-8 转换产物，可含内嵌 NUL，
    // from_cstr 的 strlen 舞步会在首个 NUL 处截断。
    return ca::core::Any(s.arena->intern(std::string_view(v)));
}

// ── 读 ──

void facadeParseClass(FacadeSession& s, const std::string& name)
{
    TreeEntry& te   = resolveTree(s, name);
    auto&      unit = te.session->unit();
    // 附着即全量解码（fail-closed）：全部带体方法升格，坏方法体在 parse 面
    // 显式失败（与旧 ClassReader.accept 全量解析等位）。
    for (auto& mu : unit.methods) {
        if (mu.code != nullptr) continue;
        if (!ir::hasCode(*te.session, mu.backingIndex)) continue;
        const auto promoted = ir::promoteMethod(*te.session, mu.id);
        if (promoted.is_err()) {
            throw std::runtime_error(std::string("parse failed for ") + name + ": " +
                                     ir::to_string(promoted.unwrap_err()));
        }
    }
}

ClassInfo facadeClassInfo(FacadeSession& s, const std::string& name)
{
    if (const auto* dex = s.ws.dexDomain()) {
        auto result = dex->classInfo(name);
        if (result.is_err()) throw StaleRefError(result.unwrap_err().message);
        const auto cls = std::move(result).unwrap();
        ClassInfo info;
        info.name = cls.internalName.to_std_string();
        info.superName = cls.superName.to_std_string();
        info.access = static_cast<int>(cls.access);
        info.version = static_cast<int>(cls.version);
        for (const auto& iface : cls.interfaces)
            info.interfaces.push_back(iface.to_std_string());
        return info;
    }
    return classInfoOf(resolveTree(s, name));
}

std::vector<std::tuple<std::string, std::string, int>> facadeMembers(FacadeSession& s,
                                                                     const std::string& name,
                                                                     bool methods)
{
    std::vector<std::tuple<std::string, std::string, int>> out;
    if (const auto* dex = s.ws.dexDomain()) {
        auto result = dex->classInfo(name);
        if (result.is_err()) throw StaleRefError(result.unwrap_err().message);
        const auto cls = std::move(result).unwrap();
        const auto append = [&out](const auto& members) {
            for (const auto& member : members)
                out.emplace_back(member.name.to_std_string(), member.descriptor.to_std_string(),
                                 static_cast<int>(member.access));
        };
        if (methods) append(cls.methods);
        else append(cls.fields);
        return out;
    }
    auto& te   = resolveTree(s, name);
    auto& unit = te.session->unit();
    auto& sp   = unit.symbols();
    if (methods) {
        for (const auto& mu : unit.methods) {
            out.emplace_back(std::string(sp.str(mu.name)), std::string(sp.str(mu.desc)),
                             static_cast<int>(mu.access));
        }
    }
    else {
        for (const auto& fu : unit.fields) {
            out.emplace_back(std::string(sp.str(fu.name)), std::string(sp.str(fu.desc)),
                             static_cast<int>(fu.access));
        }
    }
    return out;
}

MemberInfo facadeMethodInfo(FacadeSession& s, const std::string& cls, const std::string& name,
                            const std::string& desc)
{
    return methodInfoOf(resolveTree(s, cls), resolveMethod(s, cls, name, desc).id);
}

MemberInfo facadeFieldInfo(FacadeSession& s, const std::string& cls, const std::string& name,
                           const std::string& desc)
{
    auto& te   = resolveTree(s, cls);
    const auto id = resolveField(s, cls, name, desc);
    auto* fu   = te.session->unit().field(id);
    auto& sp   = te.session->unit().symbols();
    MemberInfo out;
    out.name   = std::string(sp.str(fu->name));
    out.desc   = std::string(sp.str(fu->desc));
    out.access = static_cast<int>(fu->access);
    return out;
}

ClassHierarchyReport facadeClassHierarchy(FacadeSession& s, const std::string& name)
{
    // 继承闭包查询（super 链 + 全部接口传递闭包 + 沿途声明成员集）：源码树建
    // 继承成员集不必再外跑 javap/反射。容器外父类/接口（java/lang/Object 及
    // JDK 类型常态）不可解析即跳过——闭包只覆盖容器内可达类；起点类不存在
    // 照旧 StaleRefError（fail-closed）。BFS + visited 集防环。
    resolveTree(s, name);   // 起点存在性校验（缺失抛 StaleRefError）

    ClassHierarchyReport     out;
    std::vector<std::string> pending{name};
    std::set<std::string>    visited;
    while (!pending.empty()) {
        const std::string current = pending.front();
        pending.erase(pending.begin());
        if (!visited.insert(current).second) continue;

        if (s.duplicateUris.count(current) != 0 || s.nameToUri.count(current) == 0) {
            continue;   // 容器外引用（JDK 类等）：闭包不含，不报错
        }
        TreeEntry& te   = resolveTree(s, current);
        auto&      unit = te.session->unit();
        auto&      sp   = unit.symbols();

        HierarchyClass info;
        info.name      = std::string(sp.str(unit.thisClass));
        info.superName = std::string(sp.str(unit.superClass));
        for (const auto itf : unit.interfaces)
            info.interfaces.push_back(std::string(sp.str(itf)));
        for (const auto& mu : unit.methods) {
            out.methods.emplace_back(info.name, std::string(sp.str(mu.name)),
                                     std::string(sp.str(mu.desc)),
                                     static_cast<int>(mu.access));
        }
        for (const auto& fu : unit.fields) {
            out.fields.emplace_back(info.name, std::string(sp.str(fu.name)),
                                    std::string(sp.str(fu.desc)),
                                    static_cast<int>(fu.access));
        }

        // 继续上溯：super + 接口入队（必须在 info 被 move 进 results 之前取值）。
        if (!info.superName.empty()) pending.push_back(info.superName);
        for (const auto& i : info.interfaces) pending.push_back(i);
        out.classes.push_back(std::move(info));
    }
    return out;
}

// ── 写（类） ──

void facadeClassSetSuper(FacadeSession& s, const std::string& name, const std::string& super)
{
    TreeEntry& te   = resolveTree(s, name);
    auto&      unit = te.session->unit();
    unit.superClass = unit.symbols().intern(super);
    if (te.session->commitClassMetadata().is_err()) {
        throw std::runtime_error("class_set_super: commit failed for " + name);
    }
    te.dirty = true;
}

void facadeClassSetAccess(FacadeSession& s, const std::string& name, int access)
{
    TreeEntry& te   = resolveTree(s, name);
    auto&      unit = te.session->unit();
    unit.access     = static_cast<ca::u32>(access);
    if (te.session->commitClassMetadata().is_err()) {
        throw std::runtime_error("class_set_access: commit failed for " + name);
    }
    te.dirty = true;
}

void facadeClassSetVersion(FacadeSession& s, const std::string& name, int version)
{
    TreeEntry& te   = resolveTree(s, name);
    auto&      unit = te.session->unit();
    // 旧树口径：低 16 位 = major，高 16 位 = minor（libjbc 打包相反，读面同）。
    const auto major = static_cast<ca::u32>(version) & 0xFFFFu;
    const auto minor = (static_cast<ca::u32>(version) >> 16) & 0xFFFFu;
    unit.version     = (major << 16) | minor;
    if (te.session->commitClassMetadata().is_err()) {
        throw std::runtime_error("class_set_version: commit failed for " + name);
    }
    te.dirty = true;
}

void facadeClassSetInterfaces(FacadeSession& s, const std::string& name,
                              const std::vector<std::string>& interfaces)
{
    TreeEntry& te   = resolveTree(s, name);
    auto&      unit = te.session->unit();
    unit.interfaces.clear();
    for (const auto& itf : interfaces) {
        unit.interfaces.push_back(unit.symbols().intern(itf));
    }
    if (te.session->commitClassMetadata().is_err()) {
        throw std::runtime_error("class_set_interfaces: commit failed for " + name);
    }
    te.dirty = true;
}

std::string facadeRenameClass(FacadeSession& s, const std::string& oldName,
                              const std::string& newName)
{
    resolveTree(s, oldName);   // 存在性校验
    if (oldName == newName) return newName;
    if (s.nameToUri.count(newName) || s.duplicateUris.count(newName)) {
        throw StaleRefError("class already exists: " + newName);
    }
    auto uriIt = s.nameToUri.find(oldName);
    if (uriIt == s.nameToUri.end()) throw StaleRefError("class not found: " + oldName);
    const std::string uri = uriIt->second;
    s.nameToUri.erase(uriIt);
    s.nameToUri[newName] = uri;

    auto& te = s.trees.at(oldName);
    te.session->unit().thisClass = te.session->unit().symbols().intern(newName);
    if (te.session->commitClassMetadata().is_err()) {
        throw std::runtime_error("rename_class: commit failed for " + newName);
    }
    te.dirty           = true;
    auto tel           = s.trees.extract(oldName);   // uid 状态随条目整体迁移
    tel.key()          = newName;
    s.trees.insert(std::move(tel));

    // G2 复现盘：类改名必须连带条目 URI 迁移，否则 export 生成 entry↔this_class
    // 失配的坏罐、不 flush 则静默丢写。这里只挂账；write_back/flush/export/
    // sync_entries 边界统一落账（树字节在写回时才序列化，避免 rename 即写）。
    {
        // 新条目路径完全由新内部名推导（包目录跟随），保证 entry↔this_class
        // 恒一致（verify_container 的 match 口径）。
        const auto slash = newName.find_last_of('/');
        const std::string newUri =
            (slash == std::string::npos ? "" : newName.substr(0, slash + 1)) +
            newName.substr(slash + 1) + ".class";
        if (newUri != uri) s.pendingEntryRenames[uri] = newUri;
    }
    // 结构变更面：mjbase 类身份按 internal_name 只增不删——旧名类行在库内
    // 是幽灵，条目 URI 迁移还要换条目锚。全量重建是唯一不引入遮蔽复杂度
    // 的出口（低频操作，重建成本可接受）。
    facadeInvalidateAnalysisDb(s);
    return newName;
}

// ── 写（方法/字段） ──

std::string facadeRenameMethod(FacadeSession& s, const std::string& cls, const std::string& name,
                               const std::string& desc, const std::string& newName)
{
    TreeEntry& te = resolveTree(s, cls);
    MethodRef  r  = resolveMethod(s, cls, name, desc);
    if (newName != name && methodExists(te, newName, desc)) {
        throw StaleRefError("method already exists: " + newName + desc);
    }
    auto& mu     = methodUnitOf(te, r.id);
    mu.name      = te.session->unit().symbols().intern(newName);
    if (te.session->commitMethodMetadata(r.id).is_err()) {
        throw std::runtime_error("rename_method: commit failed for " + cls + "." + newName);
    }
    te.dirty = true;
    return newName;
}

std::string facadeRenameField(FacadeSession& s, const std::string& cls, const std::string& name,
                              const std::string& desc, const std::string& newName)
{
    TreeEntry& te = resolveTree(s, cls);
    const auto id = resolveField(s, cls, name, desc);
    if (newName != name && fieldExists(te, newName, desc)) {
        throw StaleRefError("field already exists: " + newName);
    }
    auto* fu     = te.session->unit().field(id);
    fu->name     = te.session->unit().symbols().intern(newName);
    if (te.session->commitFieldMetadata(id).is_err()) {
        throw std::runtime_error("rename_field: commit failed for " + cls + "." + newName);
    }
    te.dirty = true;
    return newName;
}

void facadeMethodSetDesc(FacadeSession& s, const std::string& cls, const std::string& name,
                         const std::string& desc, const std::string& newDesc)
{
    TreeEntry& te = resolveTree(s, cls);
    MethodRef  r  = resolveMethod(s, cls, name, desc);
    auto&      mu = methodUnitOf(te, r.id);
    mu.desc      = te.session->unit().symbols().intern(newDesc);
    if (te.session->commitMethodMetadata(r.id).is_err()) {
        throw std::runtime_error("method_set_desc: commit failed for " + cls + "." + name);
    }
    te.dirty = true;
}

void facadeMethodSetAccess(FacadeSession& s, const std::string& cls, const std::string& name,
                           const std::string& desc, int access)
{
    TreeEntry& te = resolveTree(s, cls);
    MethodRef  r  = resolveMethod(s, cls, name, desc);
    methodUnitOf(te, r.id).access = static_cast<ca::u32>(access);
    if (te.session->commitMethodMetadata(r.id).is_err()) {
        throw std::runtime_error("method_set_access: commit failed for " + cls + "." + name);
    }
    te.dirty = true;
}

void facadeMethodSetMaxStack(FacadeSession& s, const std::string& cls, const std::string& name,
                             const std::string& desc, int v)
{
    MethodRef r = resolveMethod(s, cls, name, desc);
    requireCode(r).maxStack = static_cast<ca::u16>(v);
    markDirty(r);
}

void facadeMethodSetMaxLocals(FacadeSession& s, const std::string& cls, const std::string& name,
                              const std::string& desc, int v)
{
    MethodRef r = resolveMethod(s, cls, name, desc);
    requireCode(r).maxLocals = static_cast<ca::u16>(v);
    markDirty(r);
}

void facadeMethodSetExceptions(FacadeSession& s, const std::string& cls, const std::string& name,
                               const std::string& desc,
                               const std::vector<std::string>& exceptions)
{
    TreeEntry& te = resolveTree(s, cls);
    MethodRef  r  = resolveMethod(s, cls, name, desc);
    auto&      mi = te.session->backing().methods[methodUnitOf(te, r.id).backingIndex];
    // 挂载语义「只挂不改」：替换 = 先摘旧槽再挂新体。
    for (auto it = mi.attrs.begin(); it != mi.attrs.end(); ++it) {
        if (te.session->backing().utf8(it->nameCp) == "Exceptions") {
            mi.attrs.erase(it);
            break;
        }
    }
    if (!exceptions.empty()) {
        auto body = std::make_unique<jb::ExceptionsAttr>();
        for (const auto& e : exceptions) {
            body->exceptionCps.push_back(te.session->internClass(e));
        }
        const auto mounted = te.session->attachAttr(r.id, "Exceptions", std::move(body));
        if (mounted.is_err()) {
            throw std::runtime_error("method_set_exceptions: mount failed for " + cls +
                                     "." + name);
        }
    }
    te.dirty = true;
}

void facadeFieldSetAccess(FacadeSession& s, const std::string& cls, const std::string& name,
                          const std::string& desc, int access)
{
    TreeEntry& te = resolveTree(s, cls);
    const auto id = resolveField(s, cls, name, desc);
    te.session->unit().field(id)->access = static_cast<ca::u32>(access);
    if (te.session->commitFieldMetadata(id).is_err()) {
        throw std::runtime_error("field_set_access: commit failed for " + cls + "." + name);
    }
    te.dirty = true;
}

std::string facadeAddMethod(FacadeSession& s, const std::string& cls, const std::string& name,
                            const std::string& desc, int access, int maxStack, int maxLocals)
{
    TreeEntry& te = resolveTree(s, cls);
    if (methodExists(te, name, desc)) {
        throw StaleRefError("method already exists: " + name + desc);
    }
    ir::MethodSeed seed;
    seed.name      = name;
    seed.desc      = desc;
    seed.access    = static_cast<ca::u32>(access);
    // native/abstract 方法禁止 Code 属性（JVMS 4.6）；libjbc writeClass
    // 校验拒载，旧树管线静默容忍掩盖了这一契约。
    seed.withCode  = (access & (0x0100 /*NATIVE*/ | 0x0400 /*ABSTRACT*/)) == 0;
    seed.maxStack  = static_cast<ca::u16>(maxStack);
    seed.maxLocals = static_cast<ca::u16>(maxLocals);
    const auto added = te.session->addMethod(seed);
    if (added.is_err()) {
        throw std::runtime_error(std::string("add_method failed: ") +
                                 ir::to_string(added.unwrap_err()));
    }
    te.dirty = true;
    return name;
}

std::string facadeAddField(FacadeSession& s, const std::string& cls, const std::string& name,
                           const std::string& desc, int access)
{
    TreeEntry& te = resolveTree(s, cls);
    if (fieldExists(te, name, desc)) {
        throw StaleRefError("field already exists: " + name);
    }
    ir::FieldSeed seed;
    seed.name   = name;
    seed.desc   = desc;
    seed.access = static_cast<ca::u32>(access);
    if (te.session->addField(seed).is_err()) {
        throw std::runtime_error("add_field failed");
    }
    te.dirty = true;
    return name;
}

void facadeRemoveMethod(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc)
{
    TreeEntry& te = resolveTree(s, cls);
    MethodRef  r  = resolveMethod(s, cls, name, desc);
    // 身份簿记与 uid 态同步退场（slot 复用由 libjbc 发号器管，uid 态按 slot
    // 键清除，新成员从 uid 1 重发号）。
    const auto removed = te.session->removeMethod(r.id);
    if (removed.is_err()) {
        throw std::runtime_error(std::string("remove_method failed: ") +
                                 ir::to_string(removed.unwrap_err()));
    }
    te.uidStates.erase(r.id.slot);
    te.dirty = true;
}

void facadeRemoveField(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc)
{
    TreeEntry& te = resolveTree(s, cls);
    const auto id = resolveField(s, cls, name, desc);
    const auto removed = te.session->removeField(id);
    if (removed.is_err()) {
        throw std::runtime_error("remove_field failed");
    }
    te.dirty = true;
}

// ── 指令面 ──

ca::u32 facadeInsnCount(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc)
{
    return static_cast<ca::u32>(stateFor(resolveMethod(s, cls, name, desc)).order.size());
}

std::vector<InsnBrief> facadeInsns(FacadeSession& s, const std::string& cls,
                                   const std::string& name, const std::string& desc)
{
    MethodRef  r  = resolveMethod(s, cls, name, desc);
    auto&      st = stateFor(r);
    ir::CodeDom* dom = r.te->session->unit().method(r.id)->code
                           ? &r.te->session->unit().method(r.id)->code->dom
                           : nullptr;
    std::vector<InsnBrief> out;
    out.reserve(st.order.size());
    for (const auto uid : st.order) {
        const auto& a = st.addrOf.at(uid);
        if (a.kind == InsnAddr::Slot::Label) {
            out.push_back({uid, -1, insn_kind::kLabel});
        }
        else if (a.kind == InsnAddr::Slot::Line) {
            out.push_back({uid, -1, insn_kind::kLine});
        }
        else {
            const ir::Insn* in = dom ? dom->peek(a.insn) : nullptr;
            out.push_back({uid, in ? static_cast<int>(in->op) : -1,
                           in ? kindOf(in->op) : insn_kind::kInsn});
        }
    }
    return out;
}

InsnDetail facadeInsnDetail(FacadeSession& s, const std::string& cls, const std::string& name,
                            const std::string& desc, ca::u32 uid)
{
    return detailOf(resolveMethod(s, cls, name, desc), uid);
}

ca::u32 facadeInsnIndex(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc, ca::u32 uid)
{
    MethodRef r = resolveMethod(s, cls, name, desc);
    resolveAddr(r, uid);   // 墓地解析即抛（fail-closed 不复活）
    auto& st = stateFor(r);
    auto  it = st.rowOf.find(uid);
    if (it == st.rowOf.end()) {
        throw StaleRefError("no such insn uid: " + std::to_string(uid));
    }
    return static_cast<ca::u32>(it->second);
}

std::optional<ca::u32> facadeFirstInsn(FacadeSession& s, const std::string& cls,
                                       const std::string& name, const std::string& desc)
{
    auto& st = stateFor(resolveMethod(s, cls, name, desc));
    if (st.order.empty()) return std::nullopt;
    return st.order.front();
}

std::optional<ca::u32> facadeLastInsn(FacadeSession& s, const std::string& cls,
                                      const std::string& name, const std::string& desc)
{
    auto& st = stateFor(resolveMethod(s, cls, name, desc));
    if (st.order.empty()) return std::nullopt;
    return st.order.back();
}

std::optional<ca::u32> facadeNextInsn(FacadeSession& s, const std::string& cls,
                                      const std::string& name, const std::string& desc,
                                      ca::u32 uid)
{
    MethodRef r = resolveMethod(s, cls, name, desc);
    resolveAddr(r, uid);
    auto& st = stateFor(r);
    auto  it = st.rowOf.find(uid);
    if (it == st.rowOf.end() || it->second + 1 >= st.order.size()) return std::nullopt;
    return st.order[it->second + 1];
}

std::optional<ca::u32> facadePrevInsn(FacadeSession& s, const std::string& cls,
                                      const std::string& name, const std::string& desc,
                                      ca::u32 uid)
{
    MethodRef r = resolveMethod(s, cls, name, desc);
    resolveAddr(r, uid);
    auto& st = stateFor(r);
    auto  it = st.rowOf.find(uid);
    if (it == st.rowOf.end() || it->second == 0) return std::nullopt;
    return st.order[it->second - 1];
}

// ── 指令写面 ──

void facadeInsnSetOpcode(FacadeSession& s, const std::string& cls, const std::string& name,
                         const std::string& desc, ca::u32 uid, int op)
{
    MethodRef  r   = resolveMethod(s, cls, name, desc);
    auto&      dom = requireCode(r).dom;
    ir::InsnId id  = liveInsn(r, uid);
    // 形状换挡由调用方自证（libjbc get 契约：改 op 前调用方保证操作数匹配）；
    // 控制流 op 换入时目标句柄若悬空会在 lower 期 fail-closed。
    dom.get(id).unwrap()->op = static_cast<jb::Op>(op);
    markDirty(r);
}

void facadeInsnSetVar(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::u32 uid, int var)
{
    MethodRef  r   = resolveMethod(s, cls, name, desc);
    ir::InsnId id  = liveInsn(r, uid);
    requireCode(r).dom.get(id).unwrap()->a = var;
    markDirty(r);
}

void facadeInsnSetIncr(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, ca::u32 uid, int incr)
{
    MethodRef  r   = resolveMethod(s, cls, name, desc);
    ir::InsnId id  = liveInsn(r, uid);
    requireCode(r).dom.get(id).unwrap()->b = incr;
    markDirty(r);
}

void facadeInsnSetOperand(FacadeSession& s, const std::string& cls, const std::string& name,
                          const std::string& desc, ca::u32 uid, int operand)
{
    MethodRef  r   = resolveMethod(s, cls, name, desc);
    ir::InsnId id  = liveInsn(r, uid);
    requireCode(r).dom.get(id).unwrap()->a = operand;
    markDirty(r);
}

void facadeInsnSetDesc(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, ca::u32 uid, const std::string& typeDesc)
{
    TreeEntry& te  = resolveTree(s, cls);
    MethodRef  r   = resolveMethod(s, cls, name, desc);
    ir::InsnId id  = liveInsn(r, uid);
    auto*      in  = requireCode(r).dom.get(id).unwrap();
    const auto k   = kindOf(in->op);
    if (k != insn_kind::kType && k != insn_kind::kMultiANewArray) {
        throw std::runtime_error("insn_set_desc: kind mismatch (" +
                                 std::string(facadeInsnKindName(k)) + ")");
    }
    in->cp = te.session->internClass(typeDesc);
    markDirty(r);
}

void facadeInsnSetFieldRef(FacadeSession& s, const std::string& cls, const std::string& name,
                           const std::string& desc, ca::u32 uid, const std::string& owner,
                           const std::string& refName, const std::string& refDesc)
{
    TreeEntry& te  = resolveTree(s, cls);
    MethodRef  r   = resolveMethod(s, cls, name, desc);
    ir::InsnId id  = liveInsn(r, uid);
    auto*      in  = requireCode(r).dom.get(id).unwrap();
    if (!te.session->setMember(*in, ir::ClassSession::MemberSem::Kind::Field, owner, refName,
                               refDesc)) {
        throw std::runtime_error("insn_set_field_ref: kind mismatch");
    }
    markDirty(r);
}

void facadeInsnSetMethodRef(FacadeSession& s, const std::string& cls, const std::string& name,
                            const std::string& desc, ca::u32 uid, const std::string& owner,
                            const std::string& refName, const std::string& refDesc, bool itf)
{
    TreeEntry& te  = resolveTree(s, cls);
    MethodRef  r   = resolveMethod(s, cls, name, desc);
    ir::InsnId id  = liveInsn(r, uid);
    auto*      in  = requireCode(r).dom.get(id).unwrap();
    const auto kind = itf ? ir::ClassSession::MemberSem::Kind::InterfaceMethod
                          : ir::ClassSession::MemberSem::Kind::Method;
    if (!te.session->setMember(*in, kind, owner, refName, refDesc)) {
        throw std::runtime_error("insn_set_method_ref: kind mismatch");
    }
    markDirty(r);
}

void facadeInsnSetCst(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::u32 uid, ca::core::Any value)
{
    TreeEntry& te  = resolveTree(s, cls);
    MethodRef  r   = resolveMethod(s, cls, name, desc);
    ir::InsnId id  = liveInsn(r, uid);
    auto*      in  = requireCode(r).dom.get(id).unwrap();
    if (kindOf(in->op) != insn_kind::kLdc) {
        throw std::runtime_error("insn_set_cst: kind mismatch (" +
                                 std::string(facadeInsnKindName(kindOf(in->op))) + ")");
    }
    in->cp = cpOfAny(te, value);
    markDirty(r);
}

void facadeInsnSetTarget(FacadeSession& s, const std::string& cls, const std::string& name,
                         const std::string& desc, ca::u32 uid, ca::u32 targetUid)
{
    retarget(resolveMethod(s, cls, name, desc), uid, targetUid);
}

// ── 发射族 ──

ca::u32 facadeEmitInsn(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int op, std::optional<ca::u32> pos)
{
    return emitAt(resolveMethod(s, cls, name, desc), mkInsn(static_cast<jb::Op>(op)), pos);
}

ca::u32 facadeEmitVar(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, int op, int var, std::optional<ca::u32> pos)
{
    auto in = mkInsn(static_cast<jb::Op>(op));
    in.a    = var;
    return emitAt(resolveMethod(s, cls, name, desc), std::move(in), pos);
}

ca::u32 facadeEmitIinc(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int var, int incr, std::optional<ca::u32> pos)
{
    auto in = mkInsn(jb::Op::IINC);
    in.a    = var;
    in.b    = incr;
    return emitAt(resolveMethod(s, cls, name, desc), std::move(in), pos);
}

ca::u32 facadeEmitType(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int op, const std::string& typeDesc,
                       std::optional<ca::u32> pos)
{
    TreeEntry& te = resolveTree(s, cls);
    auto in      = mkInsn(static_cast<jb::Op>(op));
    in.cp        = te.session->internClass(typeDesc);
    return emitAt(resolveMethod(s, cls, name, desc), std::move(in), pos);
}

ca::u32 facadeEmitField(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc, int op, const std::string& owner,
                        const std::string& refName, const std::string& refDesc,
                        std::optional<ca::u32> pos)
{
    TreeEntry& te = resolveTree(s, cls);
    auto in      = mkInsn(static_cast<jb::Op>(op));
    in.cp        = te.session->internFieldRef(owner, refName, refDesc);
    return emitAt(resolveMethod(s, cls, name, desc), std::move(in), pos);
}

ca::u32 facadeEmitMethodCall(FacadeSession& s, const std::string& cls, const std::string& name,
                             const std::string& desc, int op, const std::string& owner,
                             const std::string& refName, const std::string& refDesc, bool itf,
                             std::optional<ca::u32> pos)
{
    TreeEntry& te = resolveTree(s, cls);
    auto in      = mkInsn(static_cast<jb::Op>(op));
    in.cp        = itf ? te.session->internInterfaceMethodRef(owner, refName, refDesc)
                       : te.session->internMethodRef(owner, refName, refDesc);
    return emitAt(resolveMethod(s, cls, name, desc), std::move(in), pos);
}

ca::u32 facadeEmitLdc(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::core::Any value, std::optional<ca::u32> pos)
{
    TreeEntry& te = resolveTree(s, cls);
    auto in      = mkInsn(jb::Op::LDC);
    in.cp        = cpOfAny(te, value);
    return emitAt(resolveMethod(s, cls, name, desc), std::move(in), pos);
}

ca::u32 facadeEmitJump(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int op, ca::u32 targetUid,
                       std::optional<ca::u32> pos)
{
    MethodRef r = resolveMethod(s, cls, name, desc);
    auto in      = mkInsn(static_cast<jb::Op>(op));
    in.target    = liveLabel(r, targetUid);
    return emitAt(r, std::move(in), pos);
}

ca::u32 facadeEmitTableSwitch(FacadeSession& s, const std::string& cls, const std::string& name,
                              const std::string& desc, int min, int max, ca::u32 defaultUid,
                              const std::vector<ca::u32>& targetUids, std::optional<ca::u32> pos)
{
    // 结构性校验 fail-closed（JVMS tableswitch：low..high 逐个对应 targets）。
    if (min > max) {
        throw std::runtime_error("emit_tableswitch: min > max");
    }
    if (targetUids.size() != static_cast<ca::usize>(max - min + 1)) {
        throw std::runtime_error("emit_tableswitch: targets size must equal max-min+1");
    }
    MethodRef r = resolveMethod(s, cls, name, desc);
    auto in      = mkInsn(jb::Op::TABLESWITCH);
    in.a         = min;
    in.b         = max;
    in.target    = liveLabel(r, defaultUid);
    for (const auto t : targetUids) in.targets.push_back(liveLabel(r, t));
    return emitAt(r, std::move(in), pos);
}

ca::u32 facadeEmitLookupSwitch(FacadeSession& s, const std::string& cls, const std::string& name,
                               const std::string& desc, const std::vector<int>& keys,
                               ca::u32 defaultUid, const std::vector<ca::u32>& targetUids,
                               std::optional<ca::u32> pos)
{
    // JVM 查找表序：keys 严格升序（乱序写出的类真机验证必炸——fail-closed）。
    if (keys.size() != targetUids.size()) {
        throw std::runtime_error("emit_lookupswitch: keys and targets size mismatch");
    }
    for (ca::usize i = 1; i < keys.size(); ++i) {
        if (keys[i - 1] >= keys[i]) {
            throw std::runtime_error("emit_lookupswitch: keys must be strictly ascending");
        }
    }
    MethodRef r = resolveMethod(s, cls, name, desc);
    auto in      = mkInsn(jb::Op::LOOKUPSWITCH);
    in.target    = liveLabel(r, defaultUid);
    in.keys      = keys;
    for (const auto t : targetUids) in.targets.push_back(liveLabel(r, t));
    return emitAt(r, std::move(in), pos);
}

ca::u32 facadeEmitLabel(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc, std::optional<ca::u32> pos)
{
    MethodRef  r   = resolveMethod(s, cls, name, desc);
    auto&      st  = stateFor(r);
    auto&      dom = requireCode(r).dom;
    ca::usize rowIndex = st.order.size();
    if (pos) {
        resolveAddr(r, *pos);   // 墓地解析即抛（fail-closed 不复活）
        rowIndex = st.rowOf[*pos];
    }
    // Boundary 角色，锚在前置边界/方法末尾；锚定随后续发射按行序规则动态跟随。
    const auto made = dom.createLabel(ir::kNoInsn);
    if (made.is_err()) {
        throw std::runtime_error("emit_label: createLabel failed");
    }
    const ir::LabelId lid = made.unwrap();
    const ca::u32 uid = st.nextUid++;
    InsnAddr addr;
    addr.kind  = InsnAddr::Slot::Label;
    addr.label = lid;
    st.addrOf.emplace(uid, addr);
    st.uidOfLabelSlot.emplace(lid.slot, uid);
    st.order.insert(st.order.begin() + static_cast<std::ptrdiff_t>(rowIndex), uid);
    rebuildRowOf(st);
    markDirty(r);
    return uid;
}

void facadeRemoveInsn(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::u32 uid)
{
    TreeEntry& te = resolveTree(s, cls);
    MethodRef  r  = resolveMethod(s, cls, name, desc);
    auto&      st = stateFor(r);
    // 先解析（墓地二次删除在此抛），再摘链。
    const InsnAddr addr = resolveAddr(r, uid);
    if (addr.kind == InsnAddr::Slot::Insn) {
        auto& dom = requireCode(r).dom;
        // 仍被 branch/switch 字段或存活 Boundary 标签锚定 → StillUsed，
        // fail-closed 抛（比旧树的悬垂链表更严，坏罐在编辑面挡掉）。
        const auto erased = dom.erase(addr.insn);
        if (erased.is_err()) {
            throw StaleRefError("insn remove refused: uid " + std::to_string(uid) + " (" +
                                ir::to_string(erased.unwrap_err()) + ")");
        }
        st.uidOfInsnSlot.erase(addr.insn.slot);
    }
    else if (addr.kind == InsnAddr::Slot::Label) {
        auto& dom = requireCode(r).dom;
        const auto destroyed = dom.destroyLabel(addr.label);
        if (destroyed.is_err()) {
            throw StaleRefError("label remove refused: uid " + std::to_string(uid) + " (" +
                                ir::to_string(destroyed.unwrap_err()) + ")");
        }
        st.uidOfLabelSlot.erase(addr.label.slot);
    }
    else {
        // Line 行是 LNT 投影：删行 = 直改 LNT 属性（物化投影随属性走）。
        auto& mu = methodUnitOf(te, r.id);
        auto& cf = te.session->backing();
        auto  ca = cf.decodeCode(cf.methods[mu.backingIndex]);
        if (ca.is_err()) throw std::runtime_error("line remove: code attr unavailable");
        jb::AttrSlot* slotPtr = nullptr;
        for (auto& slot : ca.unwrap()->attrs) {
            if (cf.utf8(slot.nameCp) == "LineNumberTable") {
                slotPtr = &slot;
                break;
            }
        }
        if (!slotPtr) throw std::runtime_error("line remove: no LineNumberTable");
        const auto decoded = cf.decodeAttr(*slotPtr);
        if (decoded.is_err()) throw std::runtime_error("line remove: decode failed");
        auto* lnt = ca::core::tag_cast::dyn_cast<jb::LineNumberTableAttr>(decoded.unwrap());
        if (!lnt) throw std::runtime_error("line remove: attr kind mismatch");
        // tag_cast::dyn_cast 只出 const 指针；可变写入按库规约 isa 守护 + cast。
        auto* rawBody    = slotPtr->mutate();
        auto* mutableLnt = ca::core::tag_cast::isa<jb::LineNumberTableAttr>(rawBody)
                               ? ca::core::tag_cast::cast<jb::LineNumberTableAttr>(rawBody)
                               : nullptr;
        if (!mutableLnt) throw std::runtime_error("line remove: mutate failed");
        bool removed = false;
        for (auto it = mutableLnt->rows.begin(); it != mutableLnt->rows.end(); ++it) {
            if (it->startPc == addr.linePc && static_cast<int>(it->line) == addr.lineNo) {
                mutableLnt->rows.erase(it);
                removed = true;
                break;
            }
        }
        if (!removed) throw StaleRefError("line row not found: uid " + std::to_string(uid));
    }
    auto rowIt = st.rowOf.find(uid);
    if (rowIt != st.rowOf.end()) {
        st.order.erase(st.order.begin() + static_cast<std::ptrdiff_t>(rowIt->second));
        rebuildRowOf(st);
    }
    markDirty(r);
}

// ── 异常表 ──

std::vector<std::tuple<ca::u32, ca::u32, ca::u32, std::string>> facadeTryCatches(
    FacadeSession& s, const std::string& cls, const std::string& name, const std::string& desc)
{
    MethodRef r = resolveMethod(s, cls, name, desc);
    auto&     cu = requireCode(r);
    auto&     st = stateFor(r);
    auto&     cf = r.te->session->backing();
    std::vector<std::tuple<ca::u32, ca::u32, ca::u32, std::string>> out;
    const auto uidOfLabel = [&](ir::LabelId l) {
        if (l == ir::kNoLabel) return ca::u32{0};
        auto it = st.uidOfLabelSlot.find(l.slot);
        return it != st.uidOfLabelSlot.end() ? it->second : ca::u32{0};
    };
    for (const auto& e : cu.exc) {
        out.emplace_back(uidOfLabel(e.start), uidOfLabel(e.end), uidOfLabel(e.handler),
                         e.catchCp == 0 ? std::string() : std::string(cf.className(e.catchCp)));
    }
    return out;
}

void facadeAddTryCatch(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, ca::u32 startUid, ca::u32 endUid,
                       ca::u32 handlerUid, const std::string& catchType)
{
    TreeEntry& te = resolveTree(s, cls);
    MethodRef  r  = resolveMethod(s, cls, name, desc);
    auto&      cu = requireCode(r);
    // 三 uid 均须为 label 节点（旧契约同款校验）。
    const auto startId   = liveLabel(r, startUid);
    const auto endId     = liveLabel(r, endUid);
    const auto handlerId = liveLabel(r, handlerUid);
    cu.exc.push_back({startId, endId, handlerId,
                      catchType.empty() ? ca::u32{0} : te.session->internClass(catchType)});
    markDirty(r);
}

void facadeRemoveTryCatch(FacadeSession& s, const std::string& cls, const std::string& name,
                          const std::string& desc, ca::usize rowIndex)
{
    MethodRef r = resolveMethod(s, cls, name, desc);
    auto&     cu = requireCode(r);
    if (rowIndex >= cu.exc.size()) {
        throw StaleRefError("try/catch row out of range: " + std::to_string(rowIndex));
    }
    cu.exc.erase(cu.exc.begin() + static_cast<std::ptrdiff_t>(rowIndex));
    markDirty(r);
}

// ── 写回/落盘 ──

std::vector<std::string> facadeSyncEntries(FacadeSession& s)
{
    std::vector<std::string> moved;
    for (const auto& [oldUri, newUri] : s.pendingEntryRenames) {
        const auto* r  = facadeContainer(s).getResource(oldUri);
        const auto* cr = r ? ca::core::tag_cast::dyn_cast<const ClassResource>(r) : nullptr;
        if (!cr) continue;   // 条目已不在：挂账作废
        // W4c：条目搬移走 applyEdit 单管道（RenameResource 变体：单事务原子，
        // 资源对象原样搬移不碰字节；沿用挂账只对类条目生效的旧纪律）。
        applyEditEntry(s, mjt::workspace::EditCommand::RenameResource{
                              mjt::workspace::utf8(oldUri), mjt::workspace::utf8(newUri)});
        for (auto& [name, uri] : s.nameToUri) {
            if (uri == oldUri) uri = newUri;
        }
        moved.push_back(oldUri + " -> " + newUri);
    }
    s.pendingEntryRenames.clear();
    return moved;
}

namespace {

/// 帧重算 + SMT 再生（compute_frames=true 档）：
///   lower（布局重定位）→ Strict 帧计算 → maxes 采用 → SMT 构建 → 嵌套槽
///   换体（Modified 强制重编码；缺槽 makeNew、不需要则摘槽）→ 再 lower
///   （maxes/SMT 落盘；布局未变，重定位通路不再触发，换上的 SMT 原样编码）。
void recomputeFrames(TreeEntry& te, const std::string& name, ir::MethodUnit& mu)
{
    auto& session = *te.session;
    auto& cf      = session.backing();
    auto& cu      = *mu.code;

    jan::MethodSig sig;
    sig.isStatic  = (mu.access & kAccStatic) != 0;
    sig.desc      = session.unit().symbols().str(mu.desc);
    sig.name      = session.unit().symbols().str(mu.name);
    sig.thisClass = session.unit().symbols().str(session.unit().thisClass);

    jan::ShapeResolver   resolver;
    jan::FrameEngine     engine(cf.cp, resolver, jan::FramePolicy::Strict);
    std::vector<jan::FrameAt> frames;
    const auto           summary = engine.compute(cu, sig, frames);
    if (!summary.ok) {
        throw std::runtime_error("write_back: frame computation failed for " + name + "." +
                                 std::string(sig.name) + " (" +
                                 jan::to_string(summary.fail) + ")");
    }
    if (!jan::applyComputedMaxes(cu, summary, sig)) {
        throw std::runtime_error("write_back: computed maxes out of u16 range for " + name +
                                 "." + std::string(sig.name));
    }
    auto built = jan::buildStackMapTable(session, cu, frames);
    if (built.is_err()) {
        // v<50 无 SMT 定义：跳过（调用方契约：老版本类不产帧）。
        if (built.unwrap_err() == jan::SmtBuildError::ClassTooOld) return;
        throw std::runtime_error("write_back: stack map table build failed for " + name +
                                 "." + std::string(sig.name) + " (" +
                                 jan::to_string(built.unwrap_err()) + ")");
    }
    auto res = std::move(built).unwrap();

    auto  ca        = cf.decodeCode(cf.methods[mu.backingIndex]);
    if (ca.is_err()) {
        throw std::runtime_error("write_back: code attr decode failed for " + name);
    }
    auto* codeAttr = ca.unwrap();
    auto  slotIt   = codeAttr->attrs.end();
    for (auto it = codeAttr->attrs.begin(); it != codeAttr->attrs.end(); ++it) {
        if (cf.utf8(it->nameCp) == "StackMapTable") {
            slotIt = it;
            break;
        }
    }
    if (res.needed) {
        if (slotIt != codeAttr->attrs.end()) {
            // 换体 + 置脏：既有重编码通路拾取新帧（body swap 是槽级手术的
            // 认可形态；mutate 通路只服务解码后的就地小改）。
            slotIt->body  = std::move(res.attr);
            slotIt->state = jb::AttrSlot::State::Modified;
        }
        else {
            codeAttr->attrs.push_back(
                jb::AttrSlot::makeNew(session.internUtf8("StackMapTable"), std::move(res.attr)));
        }
    }
    else if (slotIt != codeAttr->attrs.end()) {
        // 无控制流目标：空表也不写（SmtBuildResult.needed 契约）。
        codeAttr->attrs.erase(slotIt);
    }
}

}   // namespace

void facadeWriteBack(FacadeSession& s, const std::string& name, bool computeFrames)
{
    TreeEntry& te   = resolveTree(s, name);
    auto&      unit = te.session->unit();
    // 第一遍 lower：编辑布局落盘（嵌套 LVT/LNT/SMT 按 pcmap fail-closed 重定位；
    // 未编辑方法 copy-through 零成本）。
    for (auto& mu : unit.methods) {
        if (!mu.code) continue;
        const auto lowered = ir::lowerMethod(*te.session, mu.id);
        if (lowered.is_err()) {
            throw std::runtime_error("write_back: lower failed for " + name + "." +
                                     std::string(unit.symbols().str(mu.name)) + " (" +
                                     ir::to_string(lowered.unwrap_err()) + ")");
        }
    }
    if (computeFrames) {
        for (auto& mu : unit.methods) {
            if (!mu.code) continue;
            recomputeFrames(te, name, mu);
            const auto lowered = ir::lowerMethod(*te.session, mu.id);
            if (lowered.is_err()) {
                throw std::runtime_error("write_back: final lower failed for " + name + "." +
                                         std::string(unit.symbols().str(mu.name)) + " (" +
                                         ir::to_string(lowered.unwrap_err()) + ")");
            }
        }
    }
    const auto written = jb::writeClass(te.session->backing());
    if (written.is_err()) {
        throw std::runtime_error("write_back: serialization failed for " + name);
    }
    auto bytes = std::move(written).unwrap();
    if (bytes.empty()) throw std::runtime_error("write_back: empty serialization for " + name);
    auto uriIt = s.nameToUri.find(name);
    if (uriIt == s.nameToUri.end()) throw StaleRefError("class not found: " + name);
    const auto* r = facadeContainer(s).getResource(uriIt->second);
    if (!ca::core::tag_cast::isa<ClassResource>(r)) {
        throw std::runtime_error("not a class entry: " + name);
    }
    // W4b：写回字节走 applyEdit（原条目类形保持，无门禁——帧计算已在上
    // 方按 compute_frames 完成，fail-loud 语义不变）。
    applyEditEntry(s, mjt::workspace::EditCommand::ResourceBytes{
                          mjt::workspace::utf8(uriIt->second), std::move(bytes)});
    te.dirty = false;
    facadeSyncEntries(s);   // 挂账的类改名条目搬移随写回落账（G2）
    // 内容编辑边界：落账字节增量同步进会话库（库未建 / 已 stale 直通；
    // rename 类 stale 已置，同步被跳过，重建时按新名重新入库）。
    facadeSyncAnalysisDbAfterEdit(s, name);
}

std::vector<std::string> facadeFlush(FacadeSession& s, bool computeFrames)
{
    std::vector<std::string> out;
    for (const auto& [name, te] : s.trees) {
        if (!te.dirty) continue;
        deadlineCheck();   // 协作层取消检查点（每类粒度）
        facadeWriteBack(s, name, computeFrames);
        out.push_back(name);
    }
    return out;
}

void facadeSave(FacadeSession& s, const std::string& path, bool computeFrames)
{
    if (s.ws.isDexBacked()) throw std::runtime_error("JAR save requires JVM input");
    facadeFlush(s, computeFrames);
    facadeSyncEntries(s);
    stagedExportJar(facadeContainer(s), path);
}

// ── rename token 面（契约 §2.7） ─────────────────────────────────────────

namespace {

FacadeSession::RenameToken& renameTokenOf(FacadeSession& s)
{
    if (!s.renameToken) {
        FacadeSession::RenameToken t;
        t.mapping = std::make_shared<mjbase::mapping::IntermediateMapping>();
        s.renameToken = std::move(t);
    }
    return *s.renameToken;
}

ca::str::Utf8StringRef internPlan(FacadeSession& s, const std::string& v)
{
    // 同 internIn：按 byte_length 入池，不吃 c_str 截断。
    return s.arena->intern(std::string_view(v));
}

/// 脚本面便捷：点分名 → internal name（token 面收点分/internal 均可，契约 §2.7）。
std::string toInternalName(std::string name)
{
    for (auto& c : name) {
        if (c == '.') c = '/';
    }
    return name;
}

}   // namespace

void facadeRenameBegin(FacadeSession& s) { s.renameToken.reset(); }

void facadeTokenRenameClass(FacadeSession& s, const std::string& oldName,
                            const std::string& newName)
{
    auto& t  = renameTokenOf(s);
    const auto ov = internPlan(s, toInternalName(oldName));
    const auto nv = internPlan(s, toInternalName(newName));
    t.plan.mapClass(ov, nv);
    t.mapping->addClass(ov, nv);
}

void facadeTokenRenameMethod(FacadeSession& s, const std::string& owner, const std::string& name,
                             const std::string& desc, const std::string& newName)
{
    auto& t  = renameTokenOf(s);
    const auto ov = internPlan(s, toInternalName(owner));
    const auto on = internPlan(s, name);
    const auto od = internPlan(s, desc);
    const auto nn = internPlan(s, newName);
    // mjbase 文本重载：六段经计划自有 KeyPool 驻留（驻留 ref 隐式转 string_view）。
    t.plan.mapMethod(ov, on, od, ov, nn, od);
    t.mapping->addMethod(ov, od, on, nn);
}

void facadeTokenRenameField(FacadeSession& s, const std::string& owner, const std::string& name,
                            const std::string& desc, const std::string& newName)
{
    auto& t  = renameTokenOf(s);
    const auto ov = internPlan(s, toInternalName(owner));
    const auto on = internPlan(s, name);
    const auto od = internPlan(s, desc);
    const auto nn = internPlan(s, newName);
    t.plan.mapField(ov, on, od, ov, nn, od);
    t.mapping->addField(ov, od, on, nn);
}

void facadeTokenCommit(FacadeSession& s, bool reflectionStrings, bool annotations,
                       bool signatures, bool resources)
{
    if (!s.renameToken) throw std::runtime_error("rename commit: empty plan (rename_begin first)");
    auto& t = *s.renameToken;
    // 先 flush 全部脏树：remapper 从容器字节重写并随后 clearTrees，未落盘的
    // 树编辑会被静默丢弃（实测：41 个折叠调用点整批蒸发）。取 computeFrames=
    // false：remap 本身保帧；需要重算帧的脚本应显式 flush(h, true)。
    facadeFlush(s, /*computeFrames=*/false);
    mjbase::asm_remapper::RemapPolicy policy;
    policy.bytecode          = true;
    policy.reflectionStrings = reflectionStrings;
    policy.annotations       = annotations;
    policy.signatures        = signatures;
    policy.resources         = resources;
    // W4c：remap 走 applyEdit 单管道（RemapPlan 变体：候选容器重写 + 改动类
    // 门禁 + commit 落账；plan 借用须活过调用期——token 持有；载荷 Policy 禁
    // 拷贝，移交 move）。
    applyEditEntry(s, mjt::workspace::EditCommand::RemapPlan{&t.plan, std::move(policy)});
    s.clearTrees();     // 容器字节整体被改写 → 树缓存作废（lazy 重解析）
    s.rebuildIndex();   // remapper 连资源 URI 一起改名 → nameToUri 索引必须重建
    facadeInvalidateAnalysisDb(s);   // 容器内容整体换血：库走下次查询重建
}

void facadeMappingDump(FacadeSession& s, const std::string& path, const std::string& fmt)
{
    if (!s.renameToken) throw std::runtime_error("mapping dump: empty plan (rename_begin first)");
    auto  registry = mjbase::mapping::MappingFormatRegistry::withStandardFormats();
    auto  norm     = fmt;
    for (auto& c : norm) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string name = norm;
    if (norm == "enigma") name = "Enigma";
    else if (norm == "tiny" || norm == "tiny-v2") name = "Tiny-V2";
    else if (norm == "tiny-v1") name = "Tiny-V1";
    else if (norm == "proguard") name = "ProGuard";
    else if (norm == "srg") name = "SRG";
    else if (norm == "simple") name = "Simple";
    else if (norm == "r8") name = "R8";
    auto text = mjbase::renamer::NameChangeRecord::exportMapping(
        *s.renameToken->mapping, name, registry);
    if (!text.is_ok()) throw std::runtime_error("unknown mapping format: " + fmt);
    std::ofstream ofs(std::filesystem::u8path(path), std::ios::binary);
    if (!ofs) throw std::runtime_error("cannot write mapping file: " + path);
    ofs << std::move(text).unwrap().ref().to_std_string();
}

std::tuple<ca::usize, ca::usize, ca::usize> facadeMappingCounts(FacadeSession& s)
{
    if (!s.renameToken) return {0, 0, 0};
    const auto& m = *s.renameToken->mapping;
    return {m.getClasses().size(), m.getMethods().size(), m.getFields().size()};
}

// ═══════════════════════════════════════════════════════════════════════
// 容器完整性 / 批量改名 / shadow 显示层（实测补刀批）
// ═══════════════════════════════════════════════════════════════════════

namespace {

/// 描述符里的类型引用抽取（数组剥壳、基本类型跳过；字段/方法描述符通用）。
/// MVP 口径：不追 signature/annotation 内类型（verify 的 dep_missing 是启发式
/// 计量，不是完备闭包）。
void collectTypesFromDesc(const std::string& desc, std::unordered_set<std::string>& out)
{
    size_t i = 0;
    while (i < desc.size()) {
        while (i < desc.size() && desc[i] == '[') ++i;
        if (i < desc.size() && desc[i] == 'L') {
            const auto end = desc.find(';', i);
            if (end == std::string::npos) break;
            out.insert(desc.substr(i + 1, end - i - 1));
            i = end + 1;
        }
        else {
            ++i;
        }
    }
}

bool isJdkType(const std::string& name)
{
    return name.rfind("java/", 0) == 0 || name.rfind("javax/", 0) == 0 ||
           name.rfind("jdk/", 0) == 0 || name.rfind("sun/", 0) == 0 ||
           name.rfind("com/sun/", 0) == 0 || name.rfind("org/w3c/", 0) == 0 ||
           name.rfind("org/xml/", 0) == 0 || name.rfind("org/ietf/", 0) == 0 ||
           name.rfind("org/omg/", 0) == 0;
}

}   // namespace

ContainerVerifyReport facadeVerifyContainer(FacadeSession& s)
{
    ContainerVerifyReport rep;
    std::unordered_set<std::string> declared;                 // 容器内实际声明的类名
    std::unordered_map<std::string, std::unordered_set<std::string>> refsByUri;

    facadeContainer(s).visitResources([&](std::string_view uriView, const IResource& resource) {
        const std::string uri(uriView);
        deadlineCheck();   // 协作层取消检查点（每条目粒度；只读操作可安全中断）
        const auto* cr = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!cr) {
            ++rep.nonClass;
            return;
        }
        // 解析闸：malformed 单独计（entry 路径进 detail）。
        std::shared_ptr<ir::ClassSession> session;
        try {
            auto opened = ir::makeSessionFromBytes(jb::ByteView(cr->get()));
            if (opened.is_err()) {
                ++rep.malformed;
                rep.malformedDetails.push_back({uri, "session build failed"});
                return;
            }
            session = std::move(opened).unwrap();
            auto&   unit = session->unit();
            // 指令级引用抽取需要方法体：逐个升格（坏方法体计入 malformed）。
            for (auto& mu : unit.methods) {
                if (mu.code != nullptr) continue;
                if (!ir::hasCode(*session, mu.backingIndex)) continue;
                const auto promoted = ir::promoteMethod(*session, mu.id);
                if (promoted.is_err()) {
                    ++rep.malformed;
                    rep.malformedDetails.push_back(
                        {uri, "promote failed: " + std::string(ir::to_string(promoted.unwrap_err()))});
                    return;
                }
            }
        }
        catch (const std::exception& e) {
            ++rep.malformed;
            rep.malformedDetails.push_back({uri, e.what()});
            return;
        }

        // entry 路径 ↔ this_class 比对（oracle entry-probe 同款口径）。
        auto&  unit         = session->unit();
        auto&  sp           = unit.symbols();
        const std::string declaredName(sp.str(unit.thisClass));
        declared.insert(declaredName);
        std::string expected = uri;
        if (expected.size() > 6 && expected.compare(expected.size() - 6, 6, ".class") == 0)
            expected = expected.substr(0, expected.size() - 6);
        if (declaredName == expected) {
            ++rep.match;
        }
        else {
            ++rep.mismatch;
            rep.mismatchDetails.push_back({uri, "declared=" + declaredName});
        }

        // 引用抽取：super/interfaces/字段与方法描述符/指令 owner 与 type desc。
        auto& refs = refsByUri[uri];
        if (!sp.str(unit.superClass).empty()) refs.insert(std::string(sp.str(unit.superClass)));
        for (const auto itf : unit.interfaces) refs.insert(std::string(sp.str(itf)));
        for (const auto& fu : unit.fields) {
            collectTypesFromDesc(std::string(sp.str(fu.desc)), refs);
        }
        for (auto& mu : unit.methods) {
            collectTypesFromDesc(std::string(sp.str(mu.desc)), refs);
            if (!mu.code) continue;
            auto& dom = mu.code->dom;
            for (const auto id : dom.order()) {
                const ir::Insn* in = dom.peek(id);
                if (!in) continue;
                ir::ClassSession::MemberSem sem;
                if (session->memberOf(*in, sem) &&
                    sem.kind != ir::ClassSession::MemberSem::Kind::InvokeDynamic) {
                    refs.insert(std::string(sem.owner));
                }
                const auto cls = session->classOf(*in);
                if (!cls.empty()) refs.insert(std::string(cls));
            }
        }
    });

    for (const auto& [uri, refs] : refsByUri) {
        for (const auto& ref : refs) {
            if (isJdkType(ref) || declared.count(ref)) continue;
            ++rep.depMissing;
            rep.depMissingDetails.push_back({uri, ref});
        }
    }
    return rep;
}

SanitizeLvtReport facadeSanitizeLvt(FacadeSession& s)
{
    SanitizeLvtReport rep;
    for (const auto& name : facadeClassNames(s)) {
        deadlineCheck();   // 协作层取消检查点（每类粒度）
        TreeEntry* tePtr = nullptr;
        try {
            tePtr = &resolveTree(s, name);
        }
        catch (const std::exception&) {
            continue;   // malformed 条目跳过（G1 场景：坏类不阻塞其余类清洗）
        }
        TreeEntry& te   = *tePtr;
        auto&      cf   = te.session->backing();
        auto&      unit = te.session->unit();
        ++rep.classes;
        for (auto& mu : unit.methods) {
            // 物化（按需升格）先于存活 pc 采集：未触达方法也要清其 LVT。
            stateFor(MethodRef{&te, mu.id});
            if (!mu.code) continue;
            auto& dom = mu.code->dom;
            // 存活旧布局 pc 集（合成指令 kNoPc 不入集：LVT 行锚不了它们）。
            std::set<ca::u32> livePcs;
            for (const auto id : dom.order()) {
                const ir::Insn* in = dom.peek(id);
                if (in && in->srcPc != ir::kNoPc) livePcs.insert(in->srcPc);
            }
            auto  ca = cf.decodeCode(cf.methods[mu.backingIndex]);
            if (ca.is_err()) continue;
            jb::CodeAttr* codeAttr = ca.unwrap();
            for (auto& slot : codeAttr->attrs) {
                if (cf.utf8(slot.nameCp) != "LocalVariableTable") continue;
                const auto decoded = cf.decodeAttr(slot);
                if (decoded.is_err()) continue;
                auto* lvt =
                    ca::core::tag_cast::dyn_cast<jb::LocalVariableTableAttr>(decoded.unwrap());
                if (!lvt) continue;
                // G1 清洗：start 锚脱链（pc 不在存活集）或 end 锚既不存活也
                // 非方法末尾的行剔除；(slot,start,end,name,desc) 去重。
                const auto  codeLen    = static_cast<ca::u32>(codeAttr->code.size());
                std::map<std::tuple<ca::u32, ca::u32, ca::u32, std::string, std::string>, bool>
                    seen;
                std::vector<jb::LocalVariableTableAttr::Row> keep;
                for (const auto& row : lvt->rows) {
                    const ca::u32 endPc = row.startPc + row.length;
                    const bool linked = livePcs.count(row.startPc) != 0 &&
                                        (livePcs.count(endPc) != 0 || endPc == codeLen);
                    if (!linked) {
                        ++rep.dropped;
                        continue;
                    }
                    const auto key = std::make_tuple(
                        row.slot, row.startPc, endPc, std::string(cf.utf8(row.nameCp)),
                        std::string(cf.utf8(row.descCp)));
                    if (!seen.emplace(key, true).second) {
                        ++rep.deduped;
                        continue;
                    }
                    keep.push_back(row);
                }
                if (keep.size() != lvt->rows.size()) {
                    auto* rawLvtBody = slot.mutate();
                    auto* mutableLvt =
                        ca::core::tag_cast::isa<jb::LocalVariableTableAttr>(rawLvtBody)
                            ? ca::core::tag_cast::cast<jb::LocalVariableTableAttr>(rawLvtBody)
                            : nullptr;
                    if (!mutableLvt) continue;
                    mutableLvt->rows = std::move(keep);
                    te.dirty = true;
                }
            }
        }
    }
    return rep;
}

namespace {

/// 被拒流水 → 脚本可读条目（owner/old/new/撞名对象定位，fail-closed 报错面）。
/// mjbase 0.0.28：proposal 字段为 string_view 驻留视图，detail 换型 Utf8String。
std::vector<RejectedSuggestion> rejectedEntriesOf(
    const std::vector<mjbase::renamer::RejectedProposal>& rejected)
{
    std::vector<RejectedSuggestion> out;
    out.reserve(rejected.size());
    for (const auto& entry : rejected) {
        RejectedSuggestion item;
        item.kind = std::string(
            mjbase::renamer::proposalKindLabel(entry.proposal.kind));
        item.owner = std::string(entry.proposal.owner);
        item.name = std::string(entry.proposal.oldName);
        item.desc = std::string(entry.proposal.desc);
        item.newName = std::string(entry.proposal.newName);
        item.source = std::string(entry.proposal.source);
        item.reason = std::string(
            mjbase::renamer::rejectionReasonName(entry.reason));
        item.detail = entry.detail.to_std_string();
        out.push_back(std::move(item));
    }
    return out;
}

}   // namespace

SuggestionRunReport facadeRenameApplySuggestions(FacadeSession& s, const std::string& path,
                                                 const std::string& policyName)
{
    auto file = mjbase::renamer::readNameSuggestionFile(path);
    if (file.is_err()) {
        // SuggestionFileError.message 0.0.28 起为 Utf8String。
        throw std::runtime_error("suggestions: " +
                                 std::move(file).unwrap_err().message.to_std_string());
    }
    mjbase::asm_remapper::RemapPolicy policy;
    if (policyName == "bytecode-only") {
        policy = mjbase::asm_remapper::RemapPolicy::bytecodeOnly();
    }
    else if (policyName == "bytecode-reflect") {
        policy = mjbase::asm_remapper::RemapPolicy::bytecodeAndReflectionStrings();
    }
    else if (policyName == "bytecode-metadata") {
        policy = mjbase::asm_remapper::RemapPolicy::bytecodeAndMetadata();
    }
    else if (policyName == "all") {
        policy = mjbase::asm_remapper::RemapPolicy::all();
    }
    else {
        throw std::runtime_error("unknown policy: " + policyName +
                                 " (bytecode-only/bytecode-reflect/bytecode-metadata/all)");
    }

    mjbase::renamer::RenamerOptions options;
    options.planOnly = false;
    // RemapPolicy 0.0.28 起 move-only：policy 此后不再读，移交给引擎选项。
    options.policy = std::move(policy);
    mjbase::renamer::FileSuggestionProvider provider(std::move(file).unwrap());
    mjbase::renamer::RenamerEngine          engine;
    // 写边界例外（W4c 裁决）：建议引擎是整容器批量重写（自带事务/回滚语义，
    // 产物含资源 URI 搬移），不走 EditCommand 单编辑形态——直驱容器后
    // clearTrees/rebuildIndex 全量重建，与 import_jar 同类的整体换血面。
    auto result = engine.run(facadeContainer(s), {&provider}, options);
    // mjbase：ca::Status 通道换 ok+error 值对；error 0.0.28 起为 Utf8String。
    if (!result.ok) {
        throw std::runtime_error("rename apply failed: " + result.error.to_std_string());
    }

    s.clearTrees();     // 容器字节整体被改写（管线直驱同款语义）
    s.rebuildIndex();
    facadeSyncEntries(s);   // 引擎走 remapper 资源搬移，无挂账；幂等清账
    facadeInvalidateAnalysisDb(s);   // 容器内容整体换血：库走下次查询重建

    // plan/映射收进 rename token：mapping_dump / mapping_counts 直接可用。
    // mjbase 引擎产物是 renamer::RenamePlan（跨域类型，不搬入 token 的
    // asm_remapper::MemberRemapPlan）；apply 已就地落容器，token 只承载
    // mapping 消费面——plan 留空，后续 commit 走 rename_token 面自攒。
    FacadeSession::RenameToken t;
    t.mapping = std::make_shared<mjbase::mapping::IntermediateMapping>();
    t.mapping->putAll(result.record.mapping());
    s.renameToken = std::move(t);

    return {result.stats.proposed, result.stats.accepted, result.stats.rejected,
            result.stats.renamedMembers, rejectedEntriesOf(result.record.rejected())};
}

ShadowGenReport facadeShadowGenerate(FacadeSession& s)
{
    s.shadowTable = std::make_shared<mjbase::renamer::ShadowAliasTable>();
    // mjbase：随机源失败走 Result 值语义（不抛异常）；错误文本 0.0.28 起
    // 为 Utf8String（move-only）——result 不能 const，unwrap_err 走移动重载。
    auto generated = mjbase::renamer::generateMechanicalShadowNames(facadeContainer(s), *s.shadowTable);
    if (generated.is_err()) {
        throw std::runtime_error("shadow generate: " +
                                 std::move(generated).unwrap_err().to_std_string());
    }
    const auto& stats = generated.unwrap();
    return {stats.assignedClasses, stats.assignedMethods, stats.assignedFields,
            stats.keptEntries};
}

ca::usize facadeShadowLoadFile(FacadeSession& s, const std::string& path)
{
    std::ifstream ifs(std::filesystem::u8path(path), std::ios::binary);
    if (!ifs) throw std::runtime_error("cannot read shadow sidecar: " + path);
    const std::string raw((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    auto parsed = mjbase::mapping::parseShadowAliasText(
        ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(raw.data()),
                                          raw.size()));
    if (parsed.is_err()) {
        throw std::runtime_error("shadow sidecar: " +
                                 std::move(parsed).unwrap_err().message.to_std_string());
    }
    s.shadowTable = std::make_shared<mjbase::renamer::ShadowAliasTable>(
        mjbase::renamer::ShadowAliasTable::fromMapping(std::move(parsed).unwrap()));
    return s.shadowTable->size();
}

void facadeShadowDumpFile(FacadeSession& s, const std::string& path)
{
    if (!s.shadowTable || s.shadowTable->empty()) {
        throw std::runtime_error("shadow table empty (shadow_generate/shadow_load first)");
    }
    auto text = mjbase::mapping::writeShadowAliasText(s.shadowTable->toMapping());
    if (text.is_err()) {
        throw std::runtime_error("shadow dump: " +
                                 std::move(text).unwrap_err().message.to_std_string());
    }
    std::ofstream ofs(std::filesystem::u8path(path), std::ios::binary);
    if (!ofs) throw std::runtime_error("cannot write shadow sidecar: " + path);
    const auto& payload = std::move(text).unwrap();
    ofs.write(reinterpret_cast<const char*>(payload.data()),
              static_cast<std::streamsize>(payload.byte_length()));
}

std::vector<std::tuple<std::string, std::string, std::string, std::string, std::string,
                       std::string>>
facadeShadowEntries(FacadeSession& s)
{
    std::vector<std::tuple<std::string, std::string, std::string, std::string, std::string,
                           std::string>>
        out;
    if (!s.shadowTable) return out;
    for (const auto& e : s.shadowTable->entries()) {
        const char* kind = e.kind == mjbase::renamer::ShadowKind::Class    ? "class"
                           : e.kind == mjbase::renamer::ShadowKind::Field  ? "field"
                                                                         : "method";
        out.emplace_back(kind, std::string(e.owner), std::string(e.desc),
                         std::string(e.rawName), std::string(e.shadowName),
                         std::string(e.token8));
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════════
// 工具等位物批（退役 MCP 工具语义迁位；workspace.cpp 同口径的本地复刻——
// diffContainers/scanJarRepairIssues 是基座 workspace 编译单元的文件内私有，
// SDK 面按同一算法在 ResourceContainer 上重实现）。
// ═══════════════════════════════════════════════════════════════════════

namespace {

/// 资源字节读出（Class 原字节 / Manifest 序列化现值 / Unknown 原样）。
std::vector<ca::u8> resourceBytes(const IResource& resource)
{
    if (const auto* cls = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource)) {
        return cls->get();
    }
    if (const auto* mf = ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource)) {
        return mf->get().toBytes();
    }
    if (const auto* unk = ca::core::tag_cast::dyn_cast<const UnknownResource>(&resource)) {
        return unk->get();
    }
    return {};
}

/// 容器深拷贝（workspace cloneContainer 同款三分支；URI 视图指向资源自持
/// 文本，先物化再写入新容器）。
std::unique_ptr<ResourceContainer> cloneContainer(const ResourceContainer& container)
{
    auto out = std::make_unique<ResourceContainer>();
    container.visitResources([&](std::string_view uri, const IResource& resource) {
        const std::string key(uri);
        if (const auto* cls = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource)) {
            out->addResource(key, std::make_unique<ClassResource>(key, cls->get()));
        }
        else if (const auto* mf =
                     ca::core::tag_cast::dyn_cast<const ManifestResource>(&resource)) {
            out->addResource(key, std::make_unique<ManifestResource>(mf->get()));
        }
        else if (const auto* unk =
                     ca::core::tag_cast::dyn_cast<const UnknownResource>(&resource)) {
            out->addResource(key, std::make_unique<UnknownResource>(key, unk->get()));
        }
    });
    return out;
}

/// diffContainers 同口径：全 uri 并集升序，逐条目单次公共前后缀收缩。
std::vector<JarByteDiff> diffContainers(const ResourceContainer& before,
                                        const ResourceContainer& after)
{
    std::set<std::string> uris;
    before.visitResources([&](std::string_view uri, const IResource&) { uris.insert(std::string(uri)); });
    after.visitResources([&](std::string_view uri, const IResource&) { uris.insert(std::string(uri)); });

    std::vector<JarByteDiff> diffs;
    for (const auto& uri : uris) {
        const IResource* beforeResource = before.getResource(uri);
        const IResource* afterResource  = after.getResource(uri);
        std::vector<ca::u8> beforeBytes, afterBytes;
        if (beforeResource != nullptr) beforeBytes = resourceBytes(*beforeResource);
        if (afterResource != nullptr) afterBytes = resourceBytes(*afterResource);
        if (beforeBytes == afterBytes) continue;

        ca::usize start = 0;
        while (start < beforeBytes.size() && start < afterBytes.size() &&
               beforeBytes[start] == afterBytes[start]) {
            ++start;
        }
        ca::usize beforeEnd = beforeBytes.size();
        ca::usize afterEnd  = afterBytes.size();
        while (beforeEnd > start && afterEnd > start &&
               beforeBytes[beforeEnd - 1] == afterBytes[afterEnd - 1]) {
            --beforeEnd;
            --afterEnd;
        }
        JarByteDiff diff;
        diff.uri        = uri;
        diff.beforeSize = beforeBytes.size();
        diff.afterSize  = afterBytes.size();
        diff.ranges.push_back({start, beforeEnd - start, afterEnd - start});
        diffs.push_back(std::move(diff));
    }
    return diffs;
}

}   // namespace

std::unique_ptr<ResourceContainer> facadeCloneContainer(const FacadeSession& s)
{
    return cloneContainer(facadeContainer(s));
}

std::vector<JarByteDiff> facadeDiffContainers(const ResourceContainer& before,
                                              const ResourceContainer& after)
{
    return diffContainers(before, after);
}

JarDiffReport facadeJarDiff(FacadeSession& s, const std::string& referencePath)
{
    if (referencePath.empty()) throw std::runtime_error("reference path is empty");
    deadlineCheck();
    ResourceContainer reference;
    auto imported = ResourceHelper::importFromJar(reference, referencePath);
    if (imported.is_err()) {
        throw std::runtime_error(std::string("import reference jar failed: ") +
                                 mjbase::model::to_string(std::move(imported).unwrap_err()));
    }
    JarDiffReport report;
    report.referencePath = referencePath;
    report.diffs         = diffContainers(facadeContainer(s), reference);
    for (const auto& diff : report.diffs) {
        if (facadeContainer(s).getResource(diff.uri) == nullptr) {
            ++report.referenceOnly;
        }
        else if (reference.getResource(diff.uri) == nullptr) {
            ++report.workspaceOnly;
        }
        else {
            ++report.modified;
        }
    }
    return report;
}

namespace {

/// jar 修复扫描（workspace scanJarRepairIssues 同口径：ClassResource 过
/// JvmJasmService 校验、.class 扩展名非类条目、corrupt_output_stream 标记）。
std::vector<JarRepairIssueView> scanJarRepairIssuesLocal(const ResourceContainer& container,
                                                         bool removeInvalidClassEntries,
                                                         bool removeCorruptOutputMarkers,
                                                         ca::usize& classCount)
{
    mjbase::jasm::JvmJasmService validator;
    std::vector<JarRepairIssueView> issues;
    container.visitResources([&](std::string_view uriView, const IResource& resource) {
        const std::string uri(uriView);
        const auto* classResource =
            ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (classResource != nullptr) {
            ++classCount;
            if (!validator.validate(classResource->get()).isOk()) {
                issues.push_back({uri,
                                  "invalid_class",
                                  "class bytes did not pass Jasm validation",
                                  classResource->get().size(),
                                  removeInvalidClassEntries});
            }
        }
        else {
            constexpr char kExtension[] = ".class";
            const bool classExtended =
                uri.size() >= sizeof(kExtension) - 1 &&
                uri.compare(uri.size() - (sizeof(kExtension) - 1), sizeof(kExtension) - 1,
                            kExtension) == 0;
            if (classExtended) {
                const auto bytes = resourceBytes(resource);
                issues.push_back({uri,
                                  "invalid_class_entry",
                                  "a .class entry was not recognized as valid class bytes",
                                  bytes.size(),
                                  removeInvalidClassEntries});
            }
        }
        if (uri.find("corrupt_output_stream") != std::string::npos) {
            const auto bytes = resourceBytes(resource);
            issues.push_back({uri,
                              "corrupt_output_marker",
                              "resource name matches a known corrupt output marker",
                              bytes.size(),
                              removeCorruptOutputMarkers});
        }
    });
    std::sort(issues.begin(), issues.end(),
              [](const JarRepairIssueView& left, const JarRepairIssueView& right) {
                  if (left.uri != right.uri) return left.uri < right.uri;
                  return left.kind < right.kind;
              });
    return issues;
}

JarRepairReport jarRepairCommon(FacadeSession& s, bool removeInvalidClassEntries,
                                bool removeCorruptOutputMarkers, bool apply)
{
    deadlineCheck();
    JarRepairReport report;
    report.resourceCount = facadeContainer(s).size();
    report.issues        = scanJarRepairIssuesLocal(facadeContainer(s),
                                                    removeInvalidClassEntries,
                                                    removeCorruptOutputMarkers,
                                                    report.classCount);
    if (!apply) return report;
    for (const auto& issue : report.issues) {
        if (issue.selected) report.removed.push_back(issue.uri);
    }
    for (const auto& uri : report.removed) {
        facadeRemoveEntry(s, uri);
    }
    return report;
}

}   // namespace

JarRepairReport facadeJarRepairAnalyze(FacadeSession& s, bool removeInvalidClassEntries,
                                       bool removeCorruptOutputMarkers)
{
    return jarRepairCommon(s, removeInvalidClassEntries, removeCorruptOutputMarkers, false);
}

JarRepairReport facadeJarRepairApply(FacadeSession& s, bool removeInvalidClassEntries,
                                     bool removeCorruptOutputMarkers)
{
    return jarRepairCommon(s, removeInvalidClassEntries, removeCorruptOutputMarkers, true);
}

DecompileCompareReport facadeDecompilerCompare(FacadeSession& s, const std::string& name,
                                               const std::string& cfrJar,
                                               const std::string& vineflowerJar,
                                               const std::string& javaProgram,
                                               std::int64_t       timeoutMs)
{
    deadlineCheck();
    if (!s.nameToUri.count(name)) {
        throw StaleRefError("class not indexed: " + name);
    }
    namespace fs = std::filesystem;
    const auto root = fs::path(fs::temp_directory_path()) /
                      ("mjt_pysdk_compare_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    fs::create_directories(root);
    struct RootGuard
    {
        fs::path root;
        ~RootGuard()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    } guard{root};

    const auto snapshot = root / "workspace.jar";
    facadeExportJar(s, snapshot.u8string(), {});

    DecompileCompareReport report;
    report.internalName = name;
    const auto backends = {
        std::pair{mjt::DecompilerKind::Cfr, cfrJar},
        std::pair{mjt::DecompilerKind::Vineflower, vineflowerJar},
    };
    for (const auto& [kind, jarOverride] : backends) {
        auto available = mjt::inspectDecompiler(kind, jarOverride, javaProgram);
        if (!available.artifactAvailable || !available.javaAvailable) {
            throw std::runtime_error("decompiler backend '" + available.id +
                                     "' is not available: " + available.unavailableReason);
        }
        mjt::DecompileRequest request;
        request.inputPath     = snapshot.u8string();
        request.outputPath    = (root / "sources").u8string();
        request.kind          = kind;
        request.decompilerJar = available.decompilerJar;
        request.javaProgram   = available.javaProgram;
        request.timeout       = std::chrono::milliseconds(timeoutMs > 0
                                                             ? timeoutMs
                                                             : 120000);
        request.tempRoot      = root.u8string();
        auto decompiled = mjt::decompile(request);
        if (decompiled.is_err()) {
            throw std::runtime_error("decompile with " + available.id + " failed: " +
                                     decompiled.unwrap_err().to_string());
        }
        const auto sourcePath = fs::path(request.outputPath) / (name + ".java");
        std::error_code ec;
        if (!fs::is_regular_file(sourcePath, ec) || ec) {
            throw std::runtime_error("decompiler did not produce source for " + name);
        }
        std::ifstream source(sourcePath, std::ios::binary);
        const std::string text((std::istreambuf_iterator<char>(source)),
                               std::istreambuf_iterator<char>());
        report.sources.push_back({name,
                                  available.id,
                                  std::move(decompiled).unwrap().decompilerJar,
                                  "class",
                                  text});
    }
    return report;
}

namespace {

ca::str::Utf8StringRef pathRefOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// sidecar 文件面共用前段：路径规约（缺省 `<jar>.mjt-shadow`；file 显式覆盖
/// 但不得指向 jar 本身）+ jar 存在检查 + 导入临时容器。
ResourceContainer importForSidecar(const std::string& jarPath, const std::string& file,
                                   std::string& sidecarPath)
{
    sidecarPath = file.empty() ? jarPath + ".mjt-shadow" : file;
    if (!file.empty() && sidecarPath == jarPath) {
        throw std::runtime_error("shadow sidecar path must not be the jar itself: " +
                                 sidecarPath);
    }
    std::error_code fsError;
    if (!std::filesystem::exists(jarPath, fsError) || fsError) {
        throw std::runtime_error("jar not found: " + jarPath);
    }
    ResourceContainer container;
    auto imported = ResourceHelper::importFromJar(container, jarPath);
    if (imported.is_err()) {
        throw std::runtime_error(std::string("import jar failed: ") +
                                 mjbase::model::to_string(std::move(imported).unwrap_err()));
    }
    return container;
}

/// 读既有 sidecar 进表（缺失 = 空表；解析失败 fail-closed）。
mjbase::renamer::ShadowAliasTable loadSidecarTable(const std::string& sidecarPath)
{
    mjbase::renamer::ShadowAliasTable table;
    std::error_code                   fsError;
    if (!std::filesystem::exists(sidecarPath, fsError)) return table;
    auto existing = mjbase::mapping::readShadowAliasFile(pathRefOf(sidecarPath));
    if (existing.is_err()) {
        throw std::runtime_error(
            "read shadow sidecar failed: " +
            std::move(existing).unwrap_err().message.to_std_string());
    }
    return mjbase::renamer::ShadowAliasTable::fromMapping(std::move(existing).unwrap());
}

void writeSidecarTable(const std::string& sidecarPath,
                       const mjbase::renamer::ShadowAliasTable& table)
{
    auto written =
        mjbase::mapping::writeShadowAliasFile(pathRefOf(sidecarPath), table.toMapping());
    if (written.is_err()) {
        throw std::runtime_error(
            "write shadow sidecar failed: " +
            std::move(written).unwrap_err().message.to_std_string());
    }
}

}   // namespace

// ── 会话级分析库（查询面底座 + 编辑边界增量同步） ──────────────────────────
//
// 建立成本 = 一次整库 ingest（容器深拷贝导出临时 jar → workspace 打开 →
// analysis_ingest 同一事实源），此后 DB-first 查询直连会话库；编辑边界
//（write_back / jasm apply / set_class_bytes）按脏类增量同步（类事实
// upsert + refs 切片 + 常量面批量切片），结构变更（import/add/remove/
// set_entry_bytes/rename/token 提交/建议应用）标 stale 下次查询全量重建。
// schema v13 资源串事实面落地后查询面无引擎回落态：regex 档按 mjbase 契约
// 全量拉取客户端过滤（search_match 共享层单源），query 边界态由共享层校验
// 器报错——会话库 workspace 只留给 ingest，不再喂任何引擎活扫描。

/// 会话级分析库状态（pysdk_facade.hpp 前置声明的实现）。
class SessionAnalysisDb
{
public:
    bool ready = false;   ///< 已建立（workspace + db 可用）。
    bool stale = false;   ///< 结构变更 / 同步失败 → 下次查询全量重建。

    SessionAnalysisDb()                                   = default;
    SessionAnalysisDb(const SessionAnalysisDb&)           = delete;
    SessionAnalysisDb& operator=(const SessionAnalysisDb&) = delete;

    ~SessionAnalysisDb() { close(); }

    /// 解除连接 + 删临时目录。析构序：先关库再删目录——Windows 下开着
    /// sqlite 句柄删不掉 .mjdb 文件。
    void close()
    {
        db.reset();
        workspace.reset();
        if (!root.empty()) {
            std::error_code ec;
            std::filesystem::remove_all(root, ec);
            root.clear();
        }
        ready = false;
    }

    /// 全量重建：整目录拆除重来。库文件是全新文件——旧库的全部输入行随之
    /// 消失，可见域（searchClasses 族跨输入并集）零残留；rename / 条目变更
    /// 留下的幽灵类行与墓碑边界一并清零。
    void rebuild(FacadeSession& session)
    {
        close();
        build(session);
    }

    /// 建立：任一步失败抛（查询面显式报错，与临时库建立失败同语义）。
    /// ready 只在全部步骤成功后置位，失败状态由 ensure 的下次 rebuild 清扫。
    void build(FacadeSession& session)
    {
        std::error_code fsError;
        // 临时目录按句柄命名：句柄单调只增不复用 → 单进程内唯一；测试按名
        // 断言会话关闭后目录即清。先清残留（上会话异常退出可能遗留）。
        root = std::filesystem::path(std::filesystem::temp_directory_path()) /
               ("mjt_pysdk_session_" + std::to_string(session.handle));
        std::filesystem::remove_all(root, fsError);
        std::filesystem::create_directories(root, fsError);
        if (fsError) {
            throw std::runtime_error("create session db temp dir failed: " +
                                     fsError.message());
        }

        if (!session.ws.isDexBacked()) {
            auto cloned = facadeCloneContainer(session);
            const auto jarPath = (root / "container.jar").u8string();
            auto exported = ResourceHelper::exportToJar(*cloned, jarPath);
            if (exported.is_err()) {
                throw std::runtime_error(
                    std::string("export container for session db failed: ") +
                    mjbase::model::to_string(std::move(exported).unwrap_err()));
            }
            workspace = std::make_unique<mjt::workspace::WorkspaceBase>();
            if (auto opened = workspace->open(pathRefOf(jarPath)); opened.is_err()) {
                throw std::runtime_error("open session db workspace failed: " +
                                         std::move(opened).unwrap_err().message);
            }
        }

        const auto dbPath = (root / "analysis.mjdb").string();
        auto opened = mjbase::project::AnalysisDb::open(
            pathRefOf(dbPath), mjbase::project::AnalysisDbOpenMode::ReadWrite);
        if (opened.is_err()) {
            throw std::runtime_error("open session analysis db failed: " +
                                     std::move(opened).unwrap_err().message.to_std_string());
        }
        db = std::make_unique<mjbase::project::AnalysisDb>(std::move(opened).unwrap());
        if (auto ingested = mjt::workspace::ingestIntoAnalysisDb(engine(session), *db);
            ingested.is_err()) {
            throw std::runtime_error("session db ingest failed: " +
                                     std::move(ingested).unwrap_err().message);
        }
        deadlineCheck();
        ready = true;
        stale = false;
    }

    mjbase::project::AnalysisDb&   analysis() { return *db; }
    mjt::workspace::WorkspaceBase& engine(FacadeSession& session)
    {
        return workspace ? *workspace : session.ws;
    }

private:
    std::filesystem::path                          root;
    std::unique_ptr<mjt::workspace::WorkspaceBase> workspace;
    std::unique_ptr<mjbase::project::AnalysisDb>   db;
};

FacadeSession::~FacadeSession() = default;

void facadeInvalidateAnalysisDb(FacadeSession& s)
{
    if (s.analysisDb) s.analysisDb->stale = true;
}

namespace {

using MethodKey = std::pair<std::string, std::string>;

/// std::string → 拥有态 Utf8String（pybind 层来的 str 已是合法 UTF-8，字节
/// 收编；与 MCP query.cpp 的 needle 构造同口径）。
ca::str::Utf8String utf8Owned(std::string_view value)
{
    return ca::str::Utf8String::from_data_unchecked(
        reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

/// std::string → 借用视图（调用期内存活前提下的 API 形参适配；MCP utf8Ref
/// 同款，空串显式空视图防 data() 悬垂）。
ca::str::Utf8StringRef utf8RefOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(
        value.empty() ? nullptr : reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

/// 会话库解析（search/xref/hierarchy 共用）：未建 → 建立；stale → 全量重建；
/// 建立失败原样抛（查询面显式报错，与临时库建立失败同语义）。
SessionAnalysisDb& ensureSessionAnalysisDb(FacadeSession& s)
{
    if (!s.analysisDb) s.analysisDb = std::make_unique<SessionAnalysisDb>();
    if (!s.analysisDb->ready || s.analysisDb->stale) s.analysisDb->rebuild(s);
    return *s.analysisDb;
}

/// 同步算法的 Result 错误收口：标记 stale + 静默返回（fail-open 纪律，与
/// facadeSyncAnalysisDbAfterEdit 的 catch 分支同一出口——库标 stale，下次
/// 查询全量重建）。
void syncFailed(SessionAnalysisDb& held) { held.stale = true; }

/// 编辑后类的类事实（DbClassInput 组装 + 成员键集）。
struct SyncClassFacts
{
    mjbase::project::DbClassInput input;
    /// (isField, (name, descriptor)) 序对——成员幽灵检查的方法/字段全集。
    std::vector<std::pair<bool, MethodKey>> memberKeys;
};

/// jbc 会话 backing → 类事实（super/access/version/interfaces/成员含
/// signature；workspace listClasses 的投影口径——version = major |
/// minor<<16）。类身份键（internalName）由调用方比对。
SyncClassFacts classFactsOf(const jbc::core::ClassFile& cf)
{
    SyncClassFacts facts;
    facts.input.internalName = utf8Owned(mjbase::jasm::classFileUtf8ToString(cf.thisName()));
    facts.input.superName    = utf8Owned(mjbase::jasm::classFileUtf8ToString(cf.superName()));
    facts.input.access       = cf.access;
    facts.input.version =
        static_cast<ca::i32>(cf.major) | (static_cast<ca::i32>(cf.minor) << 16);
    for (const ca::u16 interfaceCp : cf.interfaces) {
        facts.input.interfaces.push_back(utf8Owned(cf.className(interfaceCp)));
    }
    // Signature 属性解码要非 const 成员（lazy 槽解码通路）——jbc 会话
    // backing 本就归本函数调用方持有，去 const 是解码副作用，非改写。
    auto& mutableCf = const_cast<jbc::core::ClassFile&>(cf);
    const auto addMember = [&](bool isField, jbc::core::MemberInfo& member) {
        mjbase::project::DbMemberInput row;
        row.kind       = isField ? mjbase::project::DbMemberKind::Field
                                 : mjbase::project::DbMemberKind::Method;
        row.name       = utf8Owned(cf.nameOf(member));
        row.descriptor = utf8Owned(cf.descOf(member));
        row.signature  =
            utf8Owned(mjt::workspace::support::signatureText(mutableCf, member.attrs));
        row.access     = member.access;
        facts.input.members.push_back(std::move(row));
        // 嵌套 braced-init 不进 push_back（MSVC 模板实参推导不吃两层花括号，
        // CI 实证 C2665）——显式构造逐层落型。
        facts.memberKeys.emplace_back(
            isField,
            MethodKey(std::string(cf.nameOf(member)), std::string(cf.descOf(member))));
    };
    for (auto& field : mutableCf.fields) addMember(true, field);
    for (auto& method : mutableCf.methods) addMember(false, method);
    return facts;
}

/// 编辑后类的引用切片重扫结果分桶（analysis_edit_sync::collectMethodSlices
/// 的 pysdk 等位物——该方法.workspace.classInfo 依赖面在 pysdk 无对应物，
/// 数据源换成 facade 容器 + ReferenceScan::buildFor 受影响类切片）：方法体
/// 切片逐方法替换，source method 为空的行是类头切片。site 过滤只收 @p owner
/// 名下的事实。
std::map<MethodKey, std::vector<mjbase::project::DbReferenceRecord>> collectReferenceSlices(
    const mjt::workspace::ReferenceScan& scan, const std::string& owner,
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
    for (const mjt::workspace::MethodReference& reference : scan.methodReferences()) {
        push(reference.site.sourceClass, reference.site.sourceMethod,
             reference.site.sourceDescriptor, reference.site.insnIndex,
             mjbase::project::DbReferenceKind::Method, reference.target.owner,
             reference.target.name, reference.target.descriptor);
    }
    for (const mjt::workspace::FieldReference& reference : scan.fieldReferences()) {
        push(reference.site.sourceClass, reference.site.sourceMethod,
             reference.site.sourceDescriptor, reference.site.insnIndex,
             mjbase::project::DbReferenceKind::Field, reference.target.owner,
             reference.target.name, reference.target.descriptor);
    }
    for (const mjt::workspace::TypeReference& reference : scan.typeReferences()) {
        push(reference.site.sourceClass, reference.site.sourceMethod,
             reference.site.sourceDescriptor, reference.site.insnIndex,
             mjbase::project::DbReferenceKind::Type, reference.targetType,
             ca::str::Utf8String{}, ca::str::Utf8String{});
    }
    return slices;
}

/// 内容编辑落账后的脏类同步（facadeSyncAnalysisDbAfterEdit 的实质；stale /
/// 未建库在入口已直通）。失败一律 syncFailed（fail-open 到重建），编辑命令
/// 本身不受影响。流程 = 类事实 upsert（登记新成员行）→ 成员幽灵检查（库有
/// 而类没有的键：remove/rename/set_desc 族旧键——mjbase 成员身份只增不删，
/// 行级清理的唯一出口是全量重建）→ 类头 + 逐方法引用切片换新 → 常量面批量
/// 切片（mjbase 0.0.30 replaceConstantFaceSlices 单事务合批）。
void syncSessionAnalysisDbForClass(SessionAnalysisDb& held, FacadeSession& s,
                                   const std::string& name)
{
    mjbase::project::AnalysisDb& db = held.analysis();

    // 类字节：容器当前落账状态（write_back / jasm apply / set_class_bytes
    // 落账后调用，字节恒可读；未索引名 / 非类条目属调用契约破坏 → 标
    // stale 收口）。
    auto uriIt = s.nameToUri.find(name);
    if (uriIt == s.nameToUri.end()) return syncFailed(held);
    const auto* resource = facadeContainer(s).getResource(uriIt->second);
    const auto* cr =
        resource ? ca::core::tag_cast::dyn_cast<const ClassResource>(resource) : nullptr;
    if (!cr) return syncFailed(held);
    const std::vector<ca::u8>& bytes = cr->get();

    auto opened = jbc::ir::makeSessionFromBytes(jbc::core::ByteView(bytes));
    if (!opened.is_ok()) return syncFailed(held);   // 畸形字节不追平旧切片：交重建
    auto heldSession = std::move(opened).unwrap();
    const jbc::core::ClassFile& cf = heldSession->backing();
    SyncClassFacts facts = classFactsOf(cf);
    if (facts.input.internalName.to_std_string() != name) {
        return syncFailed(held);   // 条目字节与索引名漂移（结构态异常）：交重建
    }

    // 类事实 upsert（新成员行随批登记；entryId=0 = 保持既有条目锚——内容
    // 编辑不改条目集合，锚面与墓碑纪律交给结构变更的全量重建）。
    std::vector<mjbase::project::DbClassInput> classInputs;
    classInputs.push_back(std::move(facts.input));
    if (auto ingested = db.ingestClasses(0, std::move(classInputs)); ingested.is_err())
        return syncFailed(held);

    // 成员幽灵检查（方法键去重：畸形重复方法键只入一次——sliceOf 首调即
    // 移走切片，重复键会以空切片覆写首次写入）。
    std::vector<MethodKey> methodKeys;
    {
        std::set<std::pair<bool, MethodKey>> present;
        for (const auto& [isField, key] : facts.memberKeys) {
            present.emplace(isField, key);
            if (!isField &&
                std::find(methodKeys.begin(), methodKeys.end(), key) == methodKeys.end()) {
                methodKeys.push_back(key);
            }
        }
        auto members = db.listMembers(utf8RefOf(name));
        if (members.is_err()) return syncFailed(held);
        for (const auto& row : std::move(members).unwrap()) {
            // 嵌套 braced-init 直初始化同样规避（MSVC 对 pair 的模板构造不吃
            // 两层花括号，CI 实证同族 C2665）——显式构造逐层落型。
            const std::pair<bool, MethodKey> key(
                row.kind == mjbase::project::DbMemberKind::Field,
                MethodKey(row.name.to_std_string(), row.descriptor.to_std_string()));
            if (!present.count(key)) return syncFailed(held);
        }
    }

    auto found = db.findClass(utf8RefOf(name));
    if (found.is_err()) return syncFailed(held);
    auto classRow = std::move(found).unwrap();
    if (!classRow.has_value()) return syncFailed(held);   // upsert 刚写即 miss：防御
    const ca::i64 classId = classRow->classId;

    // 引用切片：类头 + 逐方法（现存方法即使零引用也写空切片，清掉编辑前
    // 的旧行——与 analysis_edit_sync 同纪律）。
    mjt::workspace::ReferenceScan scan;
    // 花括号初始化 vector<Utf8String> 会落 initializer_list 构造（元素要拷
    // 贝，Utf8String move-only → MSVC C2280，CI 实证）——push_back 走移动。
    std::vector<ca::str::Utf8String> changedUris;
    changedUris.push_back(utf8Owned(uriIt->second));
    scan.buildFor(facadeContainer(s), changedUris);

    std::vector<mjbase::project::DbReferenceRecord> header;
    auto methodSlices = collectReferenceSlices(scan, name, header);
    if (auto replaced = db.replaceReferencesFromClass(0, classId, std::move(header));
        replaced.is_err())
        return syncFailed(held);

    std::map<MethodKey, ca::i64> methodIds;
    {
        auto members = db.listMembers(utf8RefOf(name));
        if (members.is_err()) return syncFailed(held);
        for (const auto& row : std::move(members).unwrap()) {
            if (row.kind != mjbase::project::DbMemberKind::Method) continue;
            methodIds[{row.name.to_std_string(), row.descriptor.to_std_string()}] =
                row.memberId;
        }
    }
    for (const auto& key : methodKeys) {
        const auto id = methodIds.find(key);
        if (id == methodIds.end()) continue;   // upsert 刚写即 miss：防御
        std::vector<mjbase::project::DbReferenceRecord> records;
        const auto slice = methodSlices.find(key);
        if (slice != methodSlices.end()) records = std::move(slice->second);
        if (auto replaced = db.replaceReferencesFromMember(0, id->second, std::move(records));
            replaced.is_err())
            return syncFailed(held);
    }

    // 常量面批量切片（mjbase 0.0.30 单事务合批）：类级 cp_strings/cp_numbers
    // 整类换新 + 逐方法数值切片（有 Code 方法恒有切片——零数值也写空切片
    // 清旧行；无 Code 方法写空切片同效）。成员幽灵已在上面标 stale 返回，
    // 消失方法无需单独清切片。
    mjbase::project::DbClassConstantFaces faces;
    faces.classId   = classId;
    faces.cpStrings = mjt::workspace::cpStringRecords(cf);
    faces.cpNumbers = mjt::workspace::cpNumberRecords(cf);
    auto numberSlices = mjt::workspace::methodNumberSlices(*heldSession);
    const auto sliceOf = [&numberSlices](const MethodKey& key)
        -> std::optional<std::vector<mjbase::project::DbNumberRecord>> {
        for (auto& slice : numberSlices) {
            if (slice.name == key.first && slice.descriptor == key.second)
                return std::move(slice.records);
        }
        return std::nullopt;
    };
    for (const auto& key : methodKeys) {
        const auto id = methodIds.find(key);
        if (id == methodIds.end()) continue;   // 成员行缺席：防御（引用切片段同款）
        mjbase::project::DbMemberNumberSlices memberSlice;
        memberSlice.memberId = id->second;
        // slice 必须非 const：DbNumberRecord 仅可移动，const optional 解引用
        // 退化为拷贝赋值。
        auto slice = sliceOf(key);
        if (slice.has_value()) memberSlice.records = std::move(*slice);
        faces.memberNumbers.push_back(std::move(memberSlice));
    }
    std::vector<mjbase::project::DbClassConstantFaces> faceBatches;
    faceBatches.push_back(std::move(faces));
    if (auto replaced = db.replaceConstantFaceSlices(0, faceBatches); replaced.is_err())
        return syncFailed(held);
}

}   // namespace

void facadeSyncAnalysisDbAfterEdit(FacadeSession& s, const std::string& name)
{
    if (!s.analysisDb || !s.analysisDb->ready || s.analysisDb->stale) return;
    try {
        deadlineCheck();
        syncSessionAnalysisDbForClass(*s.analysisDb, s, name);
    }
    catch (const DeadlineExceeded&) {
        throw;   // 协作层取消：不可吞（吞了 = 脚本超限静默续跑）。
    }
    catch (...) {
        // fail-open 到重建：库标 stale 下次查询整库重来；编辑命令本身照常
        // 成功（增量同步是可丢弃的缓存追平，不是编辑事务的一部分）。
        s.analysisDb->stale = true;
    }
}

ShadowSidecarReport facadeShadowGenerateFile(const std::string& jarPath,
                                             const std::string& file)
{
    deadlineCheck();
    std::string sidecarPath;
    auto container = importForSidecar(jarPath, file, sidecarPath);
    auto table     = loadSidecarTable(sidecarPath);
    // mjbase 补号幂等：已分配条目保留（kept），随机源失败走 Err 值语义
    //（错误文本 0.0.28 起为 Utf8String）。
    auto generated = mjbase::renamer::generateMechanicalShadowNames(container, table);
    if (generated.is_err()) {
        throw std::runtime_error("generate shadow names failed: " +
                                 std::move(generated).unwrap_err().to_std_string());
    }
    const auto& stats = generated.unwrap();
    writeSidecarTable(sidecarPath, table);
    return {sidecarPath,
            stats.assignedClasses,
            stats.assignedMethods,
            stats.assignedFields,
            stats.keptEntries,
            0,
            table.size()};
}

ShadowSidecarReport facadeShadowPruneFile(const std::string& jarPath,
                                          const std::string& file)
{
    deadlineCheck();
    std::string sidecarPath;
    auto container = importForSidecar(jarPath, file, sidecarPath);
    std::error_code fsError;
    if (!std::filesystem::exists(sidecarPath, fsError)) {
        throw std::runtime_error("shadow sidecar not found: " + sidecarPath +
                                 " (generate it with shadow_generate_file first)");
    }
    auto table   = loadSidecarTable(sidecarPath);
    auto removed = mjbase::renamer::pruneShadowAliases(container, table);
    if (removed > 0) writeSidecarTable(sidecarPath, table);
    return {sidecarPath, 0, 0, 0, 0, removed, table.size()};
}

ClassCensusFileReport facadeClassCensusFile(const std::string& jarPath,
                                            const std::string& outDir)
{
    deadlineCheck();
    std::error_code fsError;
    if (!std::filesystem::exists(jarPath, fsError) || fsError) {
        throw std::runtime_error("jar not found: " + jarPath);
    }
    const std::string directory = outDir.empty() ? jarPath + ".census" : outDir;
    if (directory == jarPath) {
        throw std::runtime_error(
            "census output directory must not be the jar itself: " + directory);
    }

    // 投影数据源是一次性临时分析库：整库导入 → 普查投影导出 → 即弃（#39：
    // 库是事实源、导出是投影；临时库不落工程面，随 root guard 清扫）。
    namespace fs = std::filesystem;
    const auto root = fs::path(fs::temp_directory_path()) /
                      ("mjt_pysdk_census_" +
                       std::to_string(std::chrono::steady_clock::now()
                                          .time_since_epoch()
                                          .count()));
    fs::create_directories(root, fsError);
    struct RootGuard
    {
        fs::path root;
        ~RootGuard()
        {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    } guard{root};

    mjt::workspace::WorkspaceBase workspace;
    if (auto opened = workspace.open(pathRefOf(jarPath)); opened.is_err()) {
        const auto error = std::move(opened).unwrap_err();
        throw std::runtime_error("open jar failed: " + error.message);
    }

    const auto dbPath = root / "analysis.mjdb";
    auto opened = mjbase::project::AnalysisDb::open(pathRefOf(dbPath.string()),
                                                    mjbase::project::AnalysisDbOpenMode::ReadWrite);
    if (opened.is_err()) {
        throw std::runtime_error(
            "open census analysis db failed: " +
            std::move(opened).unwrap_err().message.to_std_string());
    }
    auto db = std::move(opened).unwrap();

    if (auto ingested = mjt::workspace::ingestIntoAnalysisDb(workspace, db);
        ingested.is_err()) {
        throw std::runtime_error("census ingest failed: " +
                                 std::move(ingested).unwrap_err().message);
    }

    auto inputId = mjt::workspace::resolveCensusInput(db, workspace.sourcePath());
    if (inputId.is_err()) {
        throw std::runtime_error("census input resolve failed: " +
                                 std::move(inputId).unwrap_err().message);
    }

    auto exported = mjt::workspace::exportCensus(db, inputId.unwrap(), directory);
    if (exported.is_err()) {
        throw std::runtime_error("census export failed: " +
                                 std::move(exported).unwrap_err().message);
    }
    const auto paths = std::move(exported).unwrap();

    // 报告数字与导出同库序同口径：同一输入的 container 汇总投影。
    auto summary = db.censusContainer(inputId.unwrap());
    if (summary.is_err()) {
        throw std::runtime_error("census summary failed: " +
                                 std::move(summary).unwrap_err().message.to_std_string());
    }
    const auto census = std::move(summary).unwrap();
    if (auto saved = db.save(); saved.is_err()) {
        throw std::runtime_error("census db checkpoint failed: " +
                                 std::move(saved).unwrap_err().message.to_std_string());
    }

    ClassCensusFileReport report;
    report.classCensusFile     = paths.classCensus;
    report.containerCensusFile = paths.containerCensus;
    report.memberCensusFile    = paths.memberCensus;
    report.classes       = static_cast<ca::usize>(census.classes);
    report.members       = paths.members;
    report.methods       = static_cast<ca::usize>(census.methods);
    report.fields        = static_cast<ca::usize>(census.fields);
    report.nativeMethods = static_cast<ca::usize>(census.nativeMethods);
    report.references    = static_cast<ca::usize>(census.references);
    report.entriesActive    = static_cast<ca::usize>(census.entriesActive);
    report.entriesIrregular = static_cast<ca::usize>(census.entriesIrregular);
    return report;
}

// ── 查询面（analysis.db DB-first，语义随 mjt #94） ────────────────────────
// utf8Owned/utf8RefOf 定义在会话库段（编辑同步共用）；本段只留查询面私有件。

namespace {

namespace fs = std::filesystem;

/// ASCII 折叠（引擎 matchesText 的 lowercase 同款）。
std::string lowercasedText(const std::string& value)
{
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return lowered;
}

/// match_mode 文本 → mjbase 文本匹配档（contains/prefix/exact；regex 由调用
/// 方守卫挡在 DB 面之外）。
mjbase::project::DbTextMatchKind matchKindOf(const std::string& mode)
{
    if (mode == "exact") return mjbase::project::DbTextMatchKind::Exact;
    if (mode == "prefix") return mjbase::project::DbTextMatchKind::Prefix;
    return mjbase::project::DbTextMatchKind::Contains;
}

/// 分析库查询错误 → runtime_error：Schema 类给 mjbase 版本失配的明确指引
///（MCP 面 Schema → FAILED_PRECONDITION 的 pysdk 等位文本），其余走常规通道。
std::runtime_error dbQueryError(const char* operation, mjbase::project::AnalysisDbError error)
{
    const auto message = error.message.to_std_string();
    if (error.kind == mjbase::project::AnalysisDbErrorKind::Schema) {
        return std::runtime_error(std::string(operation) +
                                  " failed: analysis db schema mismatch"
                                  " (mjbase schema upgrade; rebuild the analysis db): " +
                                  message);
    }
    return std::runtime_error(std::string(operation) + " failed: analysis db error: " + message);
}


/// 分面参数守卫（错位 fail-closed → invalid_argument = Python ValueError）。
/// kind 决定合法过滤位（MCP 分支键白名单的 pysdk 等位）：classes 不吃 owner/
/// member_kind/string_kind/opcode；members 不吃 string_kind/opcode；strings
/// 不吃 member_kind/opcode；numbers 无 match_mode（值等值语义）。
void validateSearchRequest(const SearchRequest& request)
{
    const bool isClasses = request.kind == "classes";
    const bool isMembers = request.kind == "members";
    const bool isStrings = request.kind == "strings";
    const bool isNumbers = request.kind == "numbers";
    if (!(isClasses || isMembers || isStrings || isNumbers))
        throw std::invalid_argument("search kind must be classes, members, strings or numbers"
                                    "; got " + request.kind);
    if (request.matchMode != "contains" && request.matchMode != "prefix" &&
        request.matchMode != "exact" && request.matchMode != "regex")
        throw std::invalid_argument("match_mode must be contains, exact, prefix or regex; got " +
                                    request.matchMode);
    if (isNumbers && request.matchMode != "contains")
        throw std::invalid_argument("numbers search matches by numeric value equality;"
                                    " match_mode is not supported");
    if (!isMembers && !request.memberKind.empty())
        throw std::invalid_argument(request.kind + " search does not take member_kind");
    if (isMembers && !request.memberKind.empty() && request.memberKind != "field" &&
        request.memberKind != "method")
        throw std::invalid_argument("member kind must be field or method");
    if (!isStrings && !request.stringKind.empty())
        throw std::invalid_argument(request.kind + " search does not take string_kind");
    if (!isNumbers && request.opcode != -1)
        throw std::invalid_argument(request.kind + " search does not take opcode");
    if (isNumbers && (request.opcode < -1 || request.opcode >= 200))
        throw std::invalid_argument("opcode must be between 0 and 199");
    if (isClasses && !request.owner.empty())
        throw std::invalid_argument("classes search does not take owner; use package_prefix");
    if (isStrings && !request.stringKind.empty() && request.stringKind != "string_literal" &&
        request.stringKind != "utf8_name" && request.stringKind != "utf8_descriptor" &&
        request.stringKind != "utf8_classref")
        throw std::invalid_argument("string_kind must be string_literal, utf8_name,"
                                    " utf8_descriptor or utf8_classref; got " + request.stringKind);
}

/// 裸 UTF8 行的分面 → string_kind 文本（constant_scan 分类器单源，MCP
/// utf8StringKind 的 pysdk 同款投影）。
std::string utf8KindText(const std::string& value)
{
    switch (mjt::workspace::constantUtf8Facet(value)) {
    case mjt::workspace::ConstantUtf8Facet::Descriptor: return "utf8_descriptor";
    case mjt::workspace::ConstantUtf8Facet::ClassRef: return "utf8_classref";
    case mjt::workspace::ConstantUtf8Facet::Name: break;
    }
    return "utf8_name";
}

/// 前缀档收紧（engine prefix = contains 超集，DB Prefix 档真前缀幂等）：
/// 判别文本面按行种类——类行 = 内部名/URI，成员行 = owner/name/descriptor/
/// value，其余 = value（MCP filterPrefixHits 同口径）。
void filterPrefixHits(std::vector<mjt::workspace::SearchHit>& hits, const std::string& query,
                      bool caseSensitive)
{
    const auto hasPrefix = [&](const std::string& text) {
        return (caseSensitive ? text : lowercasedText(text)).rfind(
                   caseSensitive ? query : lowercasedText(query), 0) == 0;
    };
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                              [&](const mjt::workspace::SearchHit& hit) {
                                  if (hit.kind == "class")
                                      return !hasPrefix(hit.owner.to_std_string()) &&
                                             !hasPrefix(hit.resourceUri.to_std_string());
                                  if (hit.kind == "field" || hit.kind == "method")
                                      return !hasPrefix(hit.owner.to_std_string()) &&
                                             !hasPrefix(hit.name.to_std_string()) &&
                                             !hasPrefix(hit.descriptor.to_std_string()) &&
                                             !hasPrefix(hit.value.to_std_string());
                                  return !hasPrefix(hit.value.to_std_string());
                              }),
              hits.end());
}

/// strings 分面过滤（引擎出口收口后）：string_literal 放行串常量（常量池
/// String 行 + 非 class 资源可打印串）；裸 UTF8 行按内容形态细分
///（utf8KindText，与响应 string_kind 同一判定）。
void filterStringKind(std::vector<mjt::workspace::SearchHit>& hits, const std::string& wanted)
{
    const bool wantLiteral = wanted == "string_literal";
    hits.erase(std::remove_if(hits.begin(), hits.end(),
                              [&](const mjt::workspace::SearchHit& hit) {
                                  if (wantLiteral)
                                      return hit.kind != "constant_pool_string" &&
                                             hit.kind != "resource_string";
                                  if (hit.kind != "constant_pool_utf8") return true;
                                  return utf8KindText(hit.value.to_std_string()) != wanted;
                              }),
              hits.end());
}

/// 引擎 SearchHit → 输出行投影（string_kind 随行种类落位；MCP searchHitJson
/// 的 pysdk 行形）。
SearchRow searchRowOf(mjt::workspace::SearchHit&& hit)
{
    SearchRow row;
    row.resourceUri      = hit.resourceUri.to_std_string();
    row.kind             = hit.kind.to_std_string();
    row.owner            = hit.owner.to_std_string();
    row.name             = hit.name.to_std_string();
    row.descriptor       = hit.descriptor.to_std_string();
    row.instructionIndex = hit.instructionIndex;
    row.value            = hit.value.to_std_string();
    if (row.kind == "constant_pool_string" || row.kind == "resource_string")
        row.stringKind = "string_literal";
    else if (row.kind == "constant_pool_utf8") row.stringKind = utf8KindText(row.value);
    return row;
}

}   // namespace

std::vector<SearchRow> facadeSearch(FacadeSession& s, const SearchRequest& request)
{
    deadlineCheck();
    validateSearchRequest(request);

    // 引擎回落与 strings 资源串并集共用的引擎过滤选项；prefix 档引擎无原生
    // 面——按 contains 取超集，出口 filterPrefixHits 收紧（MCP 同口径）。
    mjt::workspace::SearchOptions options;
    if (request.matchMode == "exact")
        options.matchMode = mjt::workspace::SearchMatchMode::Exact;
    else if (request.matchMode == "regex")
        options.matchMode = mjt::workspace::SearchMatchMode::Regex;
    options.caseSensitive = request.caseSensitive;
    options.owner         = utf8Owned(request.owner);
    options.packagePrefix = utf8Owned(request.packagePrefix);
    options.memberKind    = utf8Owned(request.memberKind);
    options.opcode        = request.opcode;

    // 查询面全量 DB-first（schema v13 资源串事实面落地后的终态，与 MCP 同
    // 形）：contains/prefix/exact 三档 SQL 可表达直查会话库（编辑边界增量
    // 同步保新鲜，见会话库段）；regex 无 SQLite 原生等价——按 mjbase 契约
    // 全量拉取（needle 空 = 不做文本过滤，LIMIT i64 极值惯例）+ 客户端过滤
    //（search_match 共享层 matchesText regex 档单源，与引擎逐字节同形）；
    // query 边界态（空/超长/member_kind/opcode/数值字面量/非法 pattern）由
    // 共享层校验器给出与引擎面逐字节同形的报错（会话库建立失败显式抛，无
    // 引擎回落态——workspace 快照不再喂任何引擎活扫描）。numbers 无
    // match_mode（值等值语义）。
    SessionAnalysisDb&            held = ensureSessionAnalysisDb(s);
    mjbase::project::AnalysisDb&  db   = held.analysis();
    std::vector<mjt::workspace::SearchHit> hitList;

    {

        if (request.kind == "classes") {
            auto validation =
                mjt::workspace::validateSearchOptions(request.query, options, "search classes");
            if (validation.is_err())
                throw std::runtime_error("search classes failed: " + validation.message);
            // regex 档：全量拉取（needle 空）+ 客户端过滤（引擎类行文本面 =
            // 内部名 / 条目 path 双面任一命中）。
            std::optional<std::regex> compiled;
            if (request.matchMode == "regex") {
                auto compiledResult =
                    mjt::workspace::compileSearchRegex(request.query, options, "search classes");
                if (compiledResult.is_err())
                    throw std::runtime_error("search classes failed: " +
                                             compiledResult.unwrap_err().message);
                compiled = std::move(compiledResult).unwrap();
            }
            const std::regex* compiledRegex = compiled ? &*compiled : nullptr;
            mjbase::project::DbClassSearchQuery dbQuery;
            dbQuery.needle        = compiledRegex != nullptr ? ca::str::Utf8String{}
                                                             : utf8Owned(request.query);
            dbQuery.match         = matchKindOf(request.matchMode);
            dbQuery.caseSensitive = request.caseSensitive;
            dbQuery.packagePrefix = utf8Owned(request.packagePrefix);
            // 全量行（pysdk 面不分页，LIMIT 上限取 i64 极值即「取尽」）；库行序
            // = 引擎命中行全序，不经客户端重排。
            auto searched = db.searchClassesPaged(dbQuery,
                                                  std::numeric_limits<ca::i64>::max(), 0);
            if (searched.is_err())
                throw dbQueryError("search classes", std::move(searched).unwrap_err());
            // unwrap()&& 按值返回：先落命名局部再取行引用（悬垂防线，census 同款）。
            auto  page = std::move(searched).unwrap();
            auto& rows = page.rows;
            hitList.reserve(rows.size());
            for (auto& row : rows) {
                if (compiledRegex != nullptr &&
                    !mjt::workspace::matchesText(row.internalName.to_std_string(),
                                                 request.query, options, compiledRegex) &&
                    !mjt::workspace::matchesText(row.entryPath.to_std_string(),
                                                 request.query, options, compiledRegex)) {
                    continue;
                }
                hitList.push_back({std::move(row.entryPath), ca::str::Utf8String::from_cstr("class"),
                                   row.internalName.clone(), {}, {}, -1,
                                   std::move(row.internalName)});
            }
        }
        else if (request.kind == "members") {
            auto validation =
                mjt::workspace::validateSearchOptions(request.query, options, "search members");
            if (validation.is_err())
                throw std::runtime_error("search members failed: " + validation.message);
            // regex 档：全量拉取 + 客户端过滤（引擎成员行文本面 = owner/name/
            // descriptor/signature 四面 + 单空格拼接复合串，任一命中即命中）。
            std::optional<std::regex> compiled;
            if (request.matchMode == "regex") {
                auto compiledResult =
                    mjt::workspace::compileSearchRegex(request.query, options, "search members");
                if (compiledResult.is_err())
                    throw std::runtime_error("search members failed: " +
                                             compiledResult.unwrap_err().message);
                compiled = std::move(compiledResult).unwrap();
            }
            const std::regex* compiledRegex = compiled ? &*compiled : nullptr;
            mjbase::project::DbMemberSearchQuery dbQuery;
            dbQuery.needle        = compiledRegex != nullptr ? ca::str::Utf8String{}
                                                             : utf8Owned(request.query);
            dbQuery.match         = matchKindOf(request.matchMode);
            dbQuery.caseSensitive = request.caseSensitive;
            dbQuery.owner         = utf8Owned(request.owner);
            dbQuery.packagePrefix = utf8Owned(request.packagePrefix);
            if (request.memberKind == "field")
                dbQuery.memberKind = mjbase::project::DbMemberKind::Field;
            else if (request.memberKind == "method")
                dbQuery.memberKind = mjbase::project::DbMemberKind::Method;
            auto searched = db.searchMembersPaged(dbQuery,
                                                  std::numeric_limits<ca::i64>::max(), 0);
            if (searched.is_err())
                throw dbQueryError("search members", std::move(searched).unwrap_err());
            auto  page = std::move(searched).unwrap();
            auto& rows = page.rows;
            hitList.reserve(rows.size());
            for (auto& row : rows) {
                // value = name + descriptor 无空格拼接（引擎命中行同款）。
                const std::string value = row.name.to_std_string() + row.descriptor.to_std_string();
                if (compiledRegex != nullptr) {
                    // 引擎成员行文本面（search_match::matchesText 同源）。
                    const std::string ownerText = row.internalName.to_std_string();
                    const std::string compound = ownerText + " " +
                                                 row.name.to_std_string() + " " +
                                                 row.descriptor.to_std_string() + " " +
                                                 row.signature.to_std_string();
                    const bool hit =
                        mjt::workspace::matchesText(ownerText, request.query, options,
                                                    compiledRegex) ||
                        mjt::workspace::matchesText(row.name.to_std_string(), request.query,
                                                    options, compiledRegex) ||
                        mjt::workspace::matchesText(row.descriptor.to_std_string(),
                                                    request.query, options, compiledRegex) ||
                        mjt::workspace::matchesText(row.signature.to_std_string(),
                                                    request.query, options, compiledRegex) ||
                        mjt::workspace::matchesText(compound, request.query, options,
                                                    compiledRegex);
                    if (!hit) continue;
                }
                hitList.push_back({std::move(row.entryPath),
                                   row.kind == mjbase::project::DbMemberKind::Field
                                       ? ca::str::Utf8String::from_cstr("field")
                                       : ca::str::Utf8String::from_cstr("method"),
                                   std::move(row.internalName), std::move(row.name),
                                   std::move(row.descriptor), -1, utf8Owned(value)});
            }
        }
        else if (request.kind == "strings") {
            auto validation =
                mjt::workspace::validateSearchOptions(request.query, options, "search strings");
            if (validation.is_err())
                throw std::runtime_error("search strings failed: " + validation.message);
            // regex 档：全量拉取 + 客户端过滤（引擎 strings 行文本面 = value
            // 单面，常量池行与资源串行同面）。
            std::optional<std::regex> compiled;
            if (request.matchMode == "regex") {
                auto compiledResult =
                    mjt::workspace::compileSearchRegex(request.query, options, "search strings");
                if (compiledResult.is_err())
                    throw std::runtime_error("search strings failed: " +
                                             compiledResult.unwrap_err().message);
                compiled = std::move(compiledResult).unwrap();
            }
            const std::regex* compiledRegex = compiled ? &*compiled : nullptr;
            mjbase::project::DbStringSearchQuery dbQuery;
            dbQuery.needle        = compiledRegex != nullptr ? ca::str::Utf8String{}
                                                             : utf8Owned(request.query);
            dbQuery.match         = matchKindOf(request.matchMode);
            dbQuery.caseSensitive = request.caseSensitive;
            dbQuery.owner         = utf8Owned(request.owner);
            dbQuery.packagePrefix = utf8Owned(request.packagePrefix);
            // string_kind 不下推 SQL：与 MCP 同款 post-filter（filterStringKind），
            // 少一个枚举映射面。全量行；行序 = 消费方命中行全序。
            auto searched = db.searchStringsPaged(dbQuery,
                                                  std::numeric_limits<ca::i64>::max(), 0);
            if (searched.is_err())
                throw dbQueryError("search strings", std::move(searched).unwrap_err());
            auto  page = std::move(searched).unwrap();
            auto& rows = page.rows;
            hitList.reserve(rows.size());
            for (auto& row : rows) {
                if (compiledRegex != nullptr &&
                    !mjt::workspace::matchesText(row.value.to_std_string(), request.query,
                                                 options, compiledRegex)) {
                    continue;
                }
                hitList.push_back({std::move(row.entryPath),
                                   row.kind == mjbase::project::DbStringKind::String
                                       ? ca::str::Utf8String::from_cstr("constant_pool_string")
                                       : ca::str::Utf8String::from_cstr("constant_pool_utf8"),
                                   std::move(row.internalName), {}, {}, -1, std::move(row.value)});
            }
            // 资源串并集（owner 与 packagePrefix 均空时）：schema v13 起资源串
            // 与常量池同库同源（resources 表，ingest 整库替换——非 class 资源
            // 字节只随结构变更（必标 stale → 重建）变化，内容编辑不触碰，会话
            // 库恒新鲜）；作用域非空时 DB-only（引擎该场景本就不出资源串，域
            // 零漂移）。regex 档全量拉取后同面过滤。
            if (options.owner.is_empty() && options.packagePrefix.is_empty()) {
                mjbase::project::DbResourceSearchQuery resourceQuery;
                resourceQuery.needle        = dbQuery.needle.clone();
                resourceQuery.match         = dbQuery.match;
                resourceQuery.caseSensitive = dbQuery.caseSensitive;
                auto resources = db.searchResourceStringsPaged(
                    resourceQuery, std::numeric_limits<ca::i64>::max(), 0);
                if (resources.is_err())
                    throw dbQueryError("search strings", std::move(resources).unwrap_err());
                auto resourcePage = std::move(resources).unwrap();
                hitList.reserve(hitList.size() + resourcePage.rows.size());
                for (auto& row : resourcePage.rows) {
                    if (compiledRegex != nullptr &&
                        !mjt::workspace::matchesText(row.value.to_std_string(), request.query,
                                                     options, compiledRegex)) {
                        continue;
                    }
                    hitList.push_back({std::move(row.entryPath),
                                       ca::str::Utf8String::from_cstr("resource_string"),
                                       {}, {}, {}, -1, std::move(row.value)});
                }
            }
            mjt::workspace::sortAndDeduplicateSearchHits(hitList);
        }
        else if (request.kind == "numbers") {
            // 引擎 searchNumbers 的边界态校验同形直通（validateSearchOptions
            // 先行——空/超长/opcode 词表，validateNumberQuery 随后——数值字面
            // 量完整解析），报错逐字节同形。
            auto validation =
                mjt::workspace::validateSearchOptions(request.query, options, "search numbers");
            if (validation.is_err())
                throw std::runtime_error("search numbers failed: " + validation.message);
            validation = mjt::workspace::validateNumberQuery(request.query);
            if (validation.is_err())
                throw std::runtime_error("search numbers failed: " + validation.message);
            mjbase::project::DbNumberQuery dbQuery;
            dbQuery.needle        = utf8Owned(request.query);
            // 收窄键（超集安全）：stold 完整解析成功时整值且 |v|<2^63 → valueInt，
            // static_cast<double> 的 IEEE 位型 → valueBits（与抽取层入库键同键
            // 合流）；解析失败不收窄，只剩 needle 字节等值守卫。
            const auto narrow = mjt::workspace::numberNarrowKeys(request.query);
            dbQuery.valueInt      = narrow.valueInt;
            dbQuery.valueBits     = narrow.valueBits;
            dbQuery.owner         = utf8Owned(request.owner);
            dbQuery.packagePrefix = utf8Owned(request.packagePrefix);
            // opcode 门透传 SQL：>= 0 时常量池行（opcode 列 NULL）自动出局，与
            // 引擎「opcode 门排除常量池来源」同口径。
            dbQuery.opcode        = request.opcode;
            auto searched = db.searchNumbers(dbQuery);
            if (searched.is_err())
                throw dbQueryError("search numbers", std::move(searched).unwrap_err());
            // 客户端终审（共享层数值等值函数，引擎 matchesNumber 同语义）：候选
            // 超集 → 精确命中，再按引擎七元组全序收口（DB 行序是库序，含常量池/
            // 方法体两源并集）。
            auto rows = std::move(searched).unwrap();
            hitList.reserve(rows.size());
            for (auto& row : rows) {
                if (!mjt::workspace::numbersEqual(row.valueText.to_std_string(), request.query))
                    continue;
                hitList.push_back({std::move(row.entryPath),
                                   row.memberName.is_empty()
                                       ? ca::str::Utf8String::from_cstr("constant_pool_number")
                                       : ca::str::Utf8String::from_cstr("instruction_number"),
                                   std::move(row.internalName), std::move(row.memberName),
                                   std::move(row.memberDescriptor), static_cast<ca::i32>(row.insnIndex),
                                   std::move(row.valueText)});
            }
            mjt::workspace::sortAndDeduplicateSearchHits(hitList);
        }
    }

    if (request.matchMode == "prefix") filterPrefixHits(hitList, request.query, request.caseSensitive);
    if (request.kind == "strings" && !request.stringKind.empty())
        filterStringKind(hitList, request.stringKind);

    std::vector<SearchRow> rows;
    rows.reserve(hitList.size());
    for (auto& hit : hitList) rows.push_back(searchRowOf(std::move(hit)));
    return rows;
}

namespace {

/// 成员描述符回填（bind 层 resolveMemberDesc 同款语义搬进 facade：宿主类内
/// 名称唯一命中即用；名字不存在报缺失、真重载报歧义并列候选——MCP
/// backfillMemberDescriptor 同形）。宿主类不在索引 → StaleRefError（键失效
/// 语义先行）；表外目标（如 JDK 成员）须显式给 descriptor。
/// facadeMembers 第三参是 methods 旗标（true=方法/false=字段），本函数形参
/// 是 fields——回填目标面取反传入（CI 实证：直传会把方法回填查成字段表，
/// 全部 backfill "member not found"）。
std::string resolveMemberDescriptor(FacadeSession& s, const std::string& cls,
                                    const std::string& name, bool fields)
{
    std::vector<std::string> hits;
    for (const auto& [memberName, memberDesc, memberAccess] :
         facadeMembers(s, cls, !fields)) {
        (void)memberAccess;
        if (memberName != name) continue;
        if (std::find(hits.begin(), hits.end(), memberDesc) == hits.end()) hits.push_back(memberDesc);
    }
    if (hits.size() == 1) return hits.front();
    facadeClassUri(s, cls);   // 未索引类 → StaleRefError（fail-closed 键失效）
    if (hits.empty())
        throw std::invalid_argument("member not found: " + cls + "." + name);
    std::string message = "ambiguous member " + cls + "." + name + "; candidates:";
    for (const auto& candidate : hits) message += " " + candidate;
    throw std::invalid_argument(message);
}

}   // namespace

std::vector<XrefRow> facadeXref(FacadeSession& s, const XrefRequest& request)
{
    deadlineCheck();
    const bool isClass  = request.target == "class";
    const bool isMethod = request.target == "method";
    const bool isField  = request.target == "field";
    if (!(isClass || isMethod || isField))
        throw std::invalid_argument("xref target must be class, method or field; got " +
                                    request.target);
    if (request.direction != "to" && request.direction != "from")
        throw std::invalid_argument("xref direction must be to or from; got " + request.direction);
    if (request.direction == "from" && !isMethod)
        throw std::invalid_argument("xref direction=from requires target=method; got " +
                                    request.target);

    std::string owner      = request.owner;
    std::string name       = request.name;
    std::string descriptor = request.descriptor;
    if (isClass) {
        if (owner.empty())
            throw std::invalid_argument("xref target=class requires owner (internal name)");
        if (!name.empty() || !descriptor.empty())
            throw std::invalid_argument("xref target=class does not take name/descriptor");
    }
    else {
        if (owner.empty() || name.empty())
            throw std::invalid_argument("xref target=" + request.target +
                                        " requires owner and name");
        // descriptor 省略时按宿主类名称唯一解析回填；显式 descriptor 原样
        // 直通（表外合法目标如 JDK 成员的引用查询零漂移，MCP raw 直通同口径）。
        if (descriptor.empty())
            descriptor = resolveMemberDescriptor(s, owner, name, isField);
    }

    const bool toFace = request.direction == "to";
    const auto projectRows = [](const std::vector<mjt::workspace::ReferenceInfo>& items) {
        std::vector<XrefRow> out;
        out.reserve(items.size());
        for (const auto& item : items) {
            XrefRow row;
            row.sourceClass      = item.sourceClass.to_std_string();
            row.sourceMethod     = item.sourceMethod.to_std_string();
            row.sourceDescriptor = item.sourceDescriptor.to_std_string();
            row.instructionIndex = item.instructionIndex;
            row.supporting       = item.supporting;
            row.supportId        = item.supportId.to_std_string();
            row.target           = item.target.to_std_string();
            row.kind             = item.kind.to_std_string();
            out.push_back(std::move(row));
        }
        return out;
    };
    const auto* dex = s.ws.dexDomain();
    if (dex && dex->hasDynamicReferenceTables()) {
        auto rows = [&]() -> mjt::workspace::WorkspaceResult<
                                 std::vector<mjt::workspace::ReferenceInfo>> {
            if (!toFace) return dex->calleesOfMethod(owner, name, descriptor, false);
            if (isMethod) return dex->xrefsToMethod(owner, name, descriptor);
            if (isField) return dex->xrefsToField(owner, name, descriptor);
            return dex->xrefsToClass(owner);
        }();
        if (rows.is_err())
            throw std::runtime_error("xref reference query: " + std::move(rows).unwrap_err().message);
        return projectRows(std::move(rows).unwrap());
    }

    // 会话库在编辑边界增量同步。
    SessionAnalysisDb&           held = ensureSessionAnalysisDb(s);
    mjbase::project::AnalysisDb& db   = held.analysis();
    auto rows = [&]() -> ca::Result<std::vector<mjbase::project::DbReferenceRecord>,
                                    mjbase::project::AnalysisDbError> {
        if (!toFace)
            return db.referencesFromMember(utf8RefOf(owner), utf8RefOf(name), utf8RefOf(descriptor));
        if (isMethod)
            return db.referencesToMethod(utf8RefOf(owner), utf8RefOf(name), utf8RefOf(descriptor));
        if (isField)
            return db.referencesToField(utf8RefOf(owner), utf8RefOf(name), utf8RefOf(descriptor));
        return db.referencesToClass(utf8RefOf(owner));
    }();
    if (rows.is_err())
        throw dbQueryError("xref reference query", std::move(rows).unwrap_err());

    // 行映射（MCP xref 分支逐字对齐）：
    // - insnIndex 静态收窄 i64→i32（扫描面本就是 i32 序号）；
    // - kind 文本按分支恒定：to-method/to-field/from-method → "direct"，
    //   to-class → "type"（引擎 referenceInfo 的字面量）；
    // - target 文本：成员分支 = owner+"."+name+descriptor 拼接、类分支 =
    //   内部名本身；
    // - to-class 只取 Type 行、from-method 只取 Method 行（callees 切片面），
    //   to-method/to-field 全行——与引擎 typeReferencesTo/methodReferencesFrom
    //   的切片面一致；
    // - supporting 恒 false：pysdk 会话无支撑容器挂载，标注本就是无操作。
    std::vector<mjt::workspace::ReferenceInfo> items;
    auto records = std::move(rows).unwrap();
    items.reserve(records.size());
    for (auto& row : records) {
        const bool kindRowWanted = isClass ? row.kind == mjbase::project::DbReferenceKind::Type
                                 : toFace  ? true
                                           : row.kind == mjbase::project::DbReferenceKind::Method;
        if (!kindRowWanted) continue;
        mjt::workspace::ReferenceInfo info;
        info.sourceClass      = std::move(row.sourceClass);
        info.sourceMethod     = std::move(row.sourceMethod);
        info.sourceDescriptor = std::move(row.sourceDescriptor);
        info.instructionIndex = static_cast<ca::i32>(row.insnIndex);
        if (isClass) {
            info.target = std::move(row.targetClass);
            info.kind   = ca::str::Utf8String::from_cstr("type");
        }
        else {
            const std::string target = row.targetClass.to_std_string() + "." +
                                       row.targetName.to_std_string() +
                                       row.targetDescriptor.to_std_string();
            info.target = utf8Owned(target);
            info.kind   = ca::str::Utf8String::from_cstr("direct");
        }
        items.push_back(std::move(info));
    }
    // 引用行收口：sortReferences 全序（无支撑容器挂载，标注无操作）。
    held.engine(s).finalizeReferenceRows(items);

    return projectRows(items);
}

namespace {

/// 层次分面参数守卫（错位 fail-closed → invalid_argument = Python ValueError）。
/// kind 词表外 / 空起点 / 分页位错位三道：limit 与 offset 必须成对给出；仅
/// 闭包两方向有 *Paged 变体（all_subclasses/implementors），super_chain 与
/// direct_subclasses 不吃分页。负值窗口不在此重复口径——留给库面
/// validatePageWindow 的 Invalid（dbQueryError 通道，与前作 query 边界态同形）。
void validateHierarchyRequest(const HierarchyRequest& request)
{
    const bool isSuperChain = request.kind == "super_chain";
    const bool isDirect     = request.kind == "direct_subclasses";
    const bool isAll        = request.kind == "all_subclasses";
    const bool isImpl       = request.kind == "implementors";
    if (!(isSuperChain || isDirect || isAll || isImpl))
        throw std::invalid_argument("hierarchy kind must be super_chain, direct_subclasses,"
                                    " all_subclasses or implementors; got " + request.kind);
    if (request.query.empty())
        throw std::invalid_argument("hierarchy query (start class internal name) is required");
    if (request.limit.has_value() != request.offset.has_value())
        throw std::invalid_argument("hierarchy limit and offset must be given together");
    if (request.limit.has_value() && !(isAll || isImpl))
        throw std::invalid_argument(request.kind + " hierarchy does not take limit/offset"
                                    " (no paged variant in analysis db)");
}

}   // namespace

std::vector<HierarchyRow> facadeHierarchy(FacadeSession& s, const HierarchyRequest& request)
{
    deadlineCheck();
    validateHierarchyRequest(request);

    // 会话库（编辑边界增量同步保新鲜；DB-only 面，无引擎回落）。
    SessionAnalysisDb&           held = ensureSessionAnalysisDb(s);
    mjbase::project::AnalysisDb& db   = held.analysis();

    // 起点登记核（fail-closed 寻址）：mjbase 四面未登记起点均返回空表，pysdk
    // 面升格为明确寻址错误——文本对齐库面 Invalid 口径（"... is not registered: 名"）。
    auto registered = db.findClass(utf8RefOf(request.query));
    if (registered.is_err())
        throw dbQueryError("hierarchy start class lookup", std::move(registered).unwrap_err());
    if (!std::move(registered).unwrap().has_value())
        throw std::invalid_argument("hierarchy " + request.kind +
                                    " start class is not registered: " + request.query);

    // 行投影底座：internal_name → 关联活跃条目 path，全类一扫（needle 空 =
    // 无文本过滤全量行，searchClassesPaged 同一可见域）——闭包行只携内部名，
    // uri 由本表回填；未登记的 super 链尾名不在表内 → uri 留空。
    mjbase::project::DbClassSearchQuery all;
    auto scanned = db.searchClassesPaged(all, std::numeric_limits<ca::i64>::max(), 0);
    if (scanned.is_err())
        throw dbQueryError("hierarchy class uri projection", std::move(scanned).unwrap_err());
    std::unordered_map<std::string, std::string> uriByName;
    {
        // unwrap()&& 按值返回：先落命名局部再取行引用（悬垂防线，前作同款）。
        auto page = std::move(scanned).unwrap();
        uriByName.reserve(page.rows.size());
        for (auto& row : page.rows)
            uriByName.emplace(row.internalName.to_std_string(), row.entryPath.to_std_string());
    }

    // 查询分发：全量走非 Paged 直查；分页（闭包两方向）透传 *Paged 变体——
    // 窗口语义全在库面（升序拼接契约），门面零重排。起点视图借用 request.query
    //（调用期存活，utf8RefOf 同前作借用前提）。
    const bool paged = request.limit.has_value();
    auto names = [&]() -> ca::Result<std::vector<ca::str::Utf8String>,
                                     mjbase::project::AnalysisDbError> {
        const ca::str::Utf8StringRef start = utf8RefOf(request.query);
        if (request.kind == "super_chain") return db.superChain(start);
        if (request.kind == "direct_subclasses") return db.directSubclasses(start);
        if (request.kind == "all_subclasses")
            return paged ? db.allSubclassesPaged(start, *request.limit, *request.offset)
                         : db.allSubclasses(start);
        return paged ? db.implementorsPaged(start, *request.limit, *request.offset)
                     : db.implementors(start);
    }();
    const std::string operation = "hierarchy " + request.kind;
    if (names.is_err()) throw dbQueryError(operation.c_str(), std::move(names).unwrap_err());

    // unwrap()&& 按值返回：先落命名局部再取行引用（悬垂防线，前作同款）。
    auto chain = std::move(names).unwrap();
    std::vector<HierarchyRow> rows;
    rows.reserve(chain.size());
    for (auto& name : chain) {
        HierarchyRow row;
        row.className = name.to_std_string();
        const auto found = uriByName.find(row.className);
        if (found != uriByName.end()) row.resourceUri = found->second;
        rows.push_back(std::move(row));
    }
    return rows;
}

}   // namespace mjt::pysdk
