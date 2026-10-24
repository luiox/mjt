#include "pysdk_facade.hpp"

#include "pysdk_limits.hpp"

#include "mjbase/asm/remapper/resource_remapper.hpp"
#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/mapping/mapping_format_registry.hpp"
#include "mjbase/mapping/shadow_alias_format.hpp"
#include "mjbase/model/resource_helper.hpp"
#include "mjbase/model/resource_model.hpp"
#include "mjbase/renamer/name_suggestion_file.hpp"
#include "mjbase/renamer/rename_engine.hpp"
#include "mjbase/renamer/shadow_alias.hpp"

#include "shared/decompiler.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>

namespace mjt::pysdk {

using jbytecode::tree::ClassNode;
using mjbase::model::ClassResource;
using mjbase::model::IResource;
using mjbase::model::ManifestResource;
using mjbase::model::ResourceContainer;
using mjbase::model::ResourceHelper;
using mjbase::model::UnknownResource;

// ── 会话 ─────────────────────────────────────────────────────────────────

void FacadeSession::rebuildIndex()
{
    nameToUri.clear();
    duplicateUris.clear();
    // mjbase visitResources 回调收 string_view（mbase 是 const string&）。
    container->visitResources([&](std::string_view uriView, const IResource& resource) {
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
    ca::str::Utf8StringArena probe;
    try {
        jbytecode::core::ClassReader reader(probe, bytes.data(),
                                            static_cast<ca::i32>(bytes.size()));
        auto name = reader.getClassName();
        if (name.is_empty()) return std::nullopt;
        return name.to_std_string();
    }
    catch (...) {
        return std::nullopt;
    }
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
    const auto* r = s.container->getResource(uri);
    if (!r) return nullptr;
    return ca::core::tag_cast::dyn_cast<const ClassResource>(r);
}

}   // namespace

// ── 容器/条目面 ──────────────────────────────────────────────────────────

void facadeImportJar(FacadeSession& session, const std::string& path)
{
    if (path.empty()) throw std::invalid_argument("import: path is empty");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        throw std::runtime_error("import: not a file: " + path);
    }
    // 协作层取消检查点：importFromJar 内部循环引擎私有，检查点只能落在入口
    // （逐条目粒度进不去，这是接受的成本，见 doc_ai/pysdk-script-limits.md）。
    deadlineCheck();
    // mjbase importFromJar 返回 ModelResult<void>（不再抛异常）。
    const auto imported = ResourceHelper::importFromJar(*session.container, path);
    if (imported.is_err()) {
        throw std::runtime_error("import: failed: " + path);
    }
    session.sourcePath = path;
    session.rebuildIndex();
}

std::vector<std::pair<std::string, std::string>> facadeListEntries(const FacadeSession& s)
{
    std::vector<std::pair<std::string, std::string>> out;
    s.container->visitResources([&](std::string_view uriView, const IResource& resource) {
        out.emplace_back(std::string(uriView),
                         std::string(mjbase::model::to_string(resource.getType())));
    });
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<ca::u8> facadeGetEntryBytes(const FacadeSession& s, const std::string& uri)
{
    const auto* r = s.container->getResource(uri);
    if (!r) throw StaleRefError("entry not found: " + uri);
    if (const auto* cr = ca::core::tag_cast::dyn_cast<const ClassResource>(r)) return cr->get();
    if (const auto* mr = ca::core::tag_cast::dyn_cast<const ManifestResource>(r))
        return mr->get().toBytes();
    if (const auto* ur = ca::core::tag_cast::dyn_cast<const UnknownResource>(r)) return ur->get();
    throw std::runtime_error("entry bytes: unsupported resource type: " + uri);
}

void facadeSetEntryBytes(FacadeSession& s, const std::string& uri, std::vector<ca::u8> bytes)
{
    auto* r = s.container->getResource(uri);
    if (!r) throw StaleRefError("entry not found: " + uri);
    // 分支判定必须 isa（tag 受检）：cast 是不受检 static_cast，对任何
    // Resource* 都非空——旧代码直接 cast 做判定，Manifest/Unknown 条目误入
    // ClassResource 分支类型混淆写内存（实测 MSVC 下 SEH，pybind 兜底成
    // 无信息 RuntimeError）。可变指针按 libca 约定 isa 守护后走 cast。
    if (ca::core::tag_cast::isa<ClassResource>(r)) {
        auto* cr = ca::core::tag_cast::cast<ClassResource>(r);
        cr->set(std::move(bytes));
        // 外部改字节：作废该类缓存树（下次树操作按新字节重建）。
        if (auto name = winnerNameFor(s, uri)) s.trees.erase(*name);
        return;
    }
    if (ca::core::tag_cast::isa<ManifestResource>(r)) {
        ca::core::tag_cast::cast<ManifestResource>(r)->set(
            mjbase::jar::Manifest::fromBytes(bytes));
        return;
    }
    if (ca::core::tag_cast::isa<UnknownResource>(r)) {
        ca::core::tag_cast::cast<UnknownResource>(r)->set(std::move(bytes));
        return;
    }
    throw std::runtime_error("set entry bytes: unsupported resource type: " + uri);
}

void facadeAddEntry(FacadeSession& s, const std::string& uri, std::vector<ca::u8> bytes)
{
    if (uri.empty()) throw std::invalid_argument("add entry: uri is empty");
    if (s.container->hasResource(uri)) {
        throw StaleRefError("entry already exists: " + uri);
    }
    auto name = facadeProbeClassName(bytes);
    if (name && !name->empty()) {
        s.container->addResource(uri, std::make_unique<ClassResource>(uri, std::move(bytes)));
        if (s.nameToUri.count(*name)) {
            s.duplicateUris.emplace(*name, uri);   // first-wins：后者落选
        }
        else {
            s.nameToUri[*name] = uri;
        }
        return;
    }
    s.container->addResource(uri, std::make_unique<UnknownResource>(uri, std::move(bytes)));
}

void facadeRemoveEntry(FacadeSession& s, const std::string& uri)
{
    if (!s.container->hasResource(uri)) throw StaleRefError("entry not found: " + uri);

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
        return s.container->getResources().erase(uri), void();
    }

    // 落选者移除：仅清落选登记。
    for (auto it = s.duplicateUris.begin(); it != s.duplicateUris.end(); ++it) {
        if (it->second == uri) {
            s.duplicateUris.erase(it);
            break;
        }
    }
    s.container->getResources().erase(uri);
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
    if (s.container->hasResource(*uri)) {
        if (!replace) throw StaleRefError("entry already exists: " + *uri);
        facadeRemoveEntry(s, *uri);
    }
    facadeAddEntry(s, *uri, std::move(bytes));
}

std::vector<std::string> facadeClassNames(const FacadeSession& s)
{
    std::vector<std::string> out;
    out.reserve(s.nameToUri.size());
    for (const auto& [name, uri] : s.nameToUri) out.push_back(name);
    return out;
}

std::string facadeClassUri(const FacadeSession& s, const std::string& name)
{
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

std::vector<ca::u8> facadeClassBytes(const FacadeSession& s, const std::string& name)
{
    if (s.duplicateUris.count(name)) {
        throw StaleRefError("ambiguous duplicate class name: " + name);
    }
    const auto* cr = classResourceOf(s, facadeClassUri(s, name));
    if (!cr) throw std::runtime_error("not a class entry: " + name);
    return cr->get();
}

void facadeSetClassBytes(FacadeSession& s, const std::string& name, std::vector<ca::u8> bytes)
{
    if (s.duplicateUris.count(name)) {
        throw StaleRefError("ambiguous duplicate class name: " + name);
    }
    auto probed = facadeProbeClassName(bytes);
    if (!probed || *probed != name) {
        throw std::runtime_error("set class bytes: bytes are not a class file for " + name);
    }
    auto it = s.nameToUri.find(name);
    if (it != s.nameToUri.end()) {
        auto* r = s.container->getResource(it->second);
        if (!ca::core::tag_cast::isa<ClassResource>(r)) {
            throw std::runtime_error("not a class entry: " + name);
        }
        auto* cr = ca::core::tag_cast::cast<ClassResource>(r);
        cr->set(std::move(bytes));
        s.trees.erase(name);   // 外部改字节：作废缓存树
        return;
    }
    // 新增类（v2 缺口修复）：URI = name + ".class"。
    const std::string uri = name + ".class";
    if (s.container->hasResource(uri)) {
        throw StaleRefError("entry already exists: " + uri);
    }
    s.container->addResource(uri, std::make_unique<ClassResource>(uri, std::move(bytes)));
    s.nameToUri[name] = uri;
}

void facadeExportJar(FacadeSession& s, const std::string& path,
                     const std::vector<std::string>& uris)
{
    // 导出即全量语义（实测补刀盘：不 flush 的 export 静默丢掉脏树/挂账改名）。
    // 帧重算失败即抛（fail-loud 优于静默出坏罐）；要跳过帧重算可先
    // flush(compute_frames=False) 再 export。
    facadeFlush(s, true);
    facadeSyncEntries(s);
    if (uris.empty()) {
        stagedExportJar(*s.container, path);
        return;
    }
    // 子集导出：Class/Manifest/Unknown 深拷贝入临时容器。
    ResourceContainer subset;
    for (const auto& uri : uris) {
        const auto* r = s.container->getResource(uri);
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
    ResourceHelper::exportToClassFile(*s.container, path, facadeClassUri(s, name));
}

// ═══════════════════════════════════════════════════════════════════════
// 树面（B2，契约 §2.2-2.6）：lazy 解析 + 键重解析 fail-closed + 脏纪律。
// ═══════════════════════════════════════════════════════════════════════

namespace {

using jbytecode::tree::AbstractInsnNode;
using jbytecode::tree::ClassNode;
using jbytecode::tree::FieldInsnNode;
using jbytecode::tree::FieldNode;
using jbytecode::tree::InsnNode;
using jbytecode::tree::IntInsnNode;
using jbytecode::tree::InvokeDynamicInsnNode;
using jbytecode::tree::IincInsnNode;
using jbytecode::tree::JumpInsnNode;
using jbytecode::core::Label;
using jbytecode::tree::LabelNode;
using jbytecode::tree::LineNumberNode;
using jbytecode::tree::LdcInsnNode;
using jbytecode::tree::LookupSwitchInsnNode;
using jbytecode::tree::MethodInsnNode;
using jbytecode::tree::MethodNode;
using jbytecode::tree::MultiANewArrayInsnNode;
using jbytecode::tree::TableSwitchInsnNode;
using jbytecode::tree::TypeInsnNode;
using jbytecode::tree::VarInsnNode;
using jbytecode::tree::TryCatchBlockNode;

ca::str::Utf8StringRef internIn(FacadeSession& s, const std::string& v)
{
    // 按 byte_length 入池：ldc 字符串载荷是 MUTF-8 转换产物，可含内嵌 NUL，
    // from_cstr 的 strlen 舞步会在首个 NUL 处截断。
    return s.arena->intern(std::string_view(v));
}

/// lazy 解析：首触建缓存（契约 §3.1 单一真相：树只在缓存里一份）。
TreeEntry& resolveTree(FacadeSession& s, const std::string& name)
{
    if (name.rfind("\x00ephem:", 0) == 0) {
        throw StaleRefError("identity ops not supported on ephemeral method views");
    }
    if (s.duplicateUris.count(name)) {
        throw StaleRefError("ambiguous duplicate class name: " + name);
    }
    auto it = s.trees.find(name);
    if (it != s.trees.end()) return it->second;
    auto uriIt = s.nameToUri.find(name);
    if (uriIt == s.nameToUri.end()) throw StaleRefError("class not found: " + name);
    const auto* r = s.container->getResource(uriIt->second);
    const auto* cr = ca::core::tag_cast::dyn_cast<const ClassResource>(r);
    if (!cr) throw std::runtime_error("not a class entry: " + name);
    const auto& bytes = cr->get();
    auto        node  = std::make_shared<ClassNode>();
    try {
        jbytecode::core::ClassReader reader(*s.arena, bytes.data(),
                                            static_cast<ca::i32>(bytes.size()));
        reader.accept(node.get(), 0);
    }
    catch (const std::exception& e) {
        throw std::runtime_error("parse failed for " + name + ": " + e.what());
    }
    // 名符校验零分配（Utf8StringRef 逐字节等值；to_std_string 每次解析都堆分配）。
    if (node->name.is_empty() || !(node->name == name.c_str())) {
        throw std::runtime_error("parsed class name mismatch for " + name);
    }
    TreeEntry entry;
    entry.node  = std::move(node);
    entry.dirty = false;
    return s.trees.emplace(name, std::move(entry)).first->second;
}

MethodNode* resolveMethod(TreeEntry& te, const std::string& name, const std::string& desc)
{
    // 热路径零分配（每次 facade 调用都走这里；逐字节等值语义与旧
    // to_std_string 比较一致）。
    for (auto* m : te.node->methods) {
        if (m->name == name.c_str() && m->desc == desc.c_str()) return m;
    }
    throw StaleRefError("method not found: " + te.node->name.to_std_string() + "." + name + desc);
}

FieldNode* resolveField(TreeEntry& te, const std::string& name, const std::string& desc)
{
    for (auto* f : te.node->fields) {
        if (f->name == name.c_str() && f->desc == desc.c_str()) return f;
    }
    throw StaleRefError("field not found: " + te.node->name.to_std_string() + "." + name);
}

bool methodExists(TreeEntry& te, const std::string& name, const std::string& desc)
{
    for (auto* m : te.node->methods) {
        if (m->name == name.c_str() && m->desc == desc.c_str()) return true;
    }
    return false;
}

bool fieldExists(TreeEntry& te, const std::string& name, const std::string& desc)
{
    for (auto* f : te.node->fields) {
        if (f->name == name.c_str() && f->desc == desc.c_str()) return true;
    }
    return false;
}

/// uid 状态按需物化：首次触达按链表序发号（契约 §2.4 每方法单调 uid）。
void materialize(InsnUidState& st, MethodNode& m)
{
    if (st.materialized) return;
    ca::u32 uid = st.nextUid;
    for (auto* p = m.instructions.getFirst(); p; p = p->getNext()) {
        st.byUid.emplace(uid, p);
        st.uidOf.emplace(p, uid);
        ++uid;
    }
    st.nextUid      = uid;
    st.materialized = true;
}

InsnUidState& uidState(TreeEntry& te, MethodNode& m)
{
    auto& st = te.uidStates[&m];
    materialize(st, m);
    return st;
}

/// 统一方法解析（契约 §3.6）："\x00ephem:" 前缀 → 管线 ephemeral 表
/// （te=null，uid 态挂 session.ephemeralUids）；否则树缓存路径。
struct MethodRef2
{
    MethodNode* m  = nullptr;
    TreeEntry*  te = nullptr;   ///< null = ephemeral
};

MethodRef2 resolveMethodFlex(FacadeSession& s, const std::string& cls, const std::string& name,
                             const std::string& desc)
{
    if (cls.rfind("\x00ephem:", 0) == 0) {
        auto it = s.ephemeralMethods.find(cls);
        if (it == s.ephemeralMethods.end() || !(it->second->name == name.c_str()) ||
            !(it->second->desc == desc.c_str())) {
            throw StaleRefError("ephemeral method not alive: " + name + desc);
        }
        return MethodRef2{it->second, nullptr};
    }
    TreeEntry& te = resolveTree(s, cls);
    return MethodRef2{resolveMethod(te, name, desc), &te};
}

InsnUidState& stateFor(FacadeSession& s, MethodRef2 r)
{
    auto& st = r.te ? r.te->uidStates[r.m] : s.ephemeralUids[r.m];
    materialize(st, *r.m);
    return st;
}

void markDirty(MethodRef2 r)
{
    if (r.te) r.te->dirty = true;
}

AbstractInsnNode* resolveInsn(InsnUidState& st, ca::u32 uid)
{
    if (st.dead.count(uid)) throw StaleRefError("insn removed: uid " + std::to_string(uid));
    auto it = st.byUid.find(uid);
    if (it == st.byUid.end()) throw StaleRefError("no such insn uid: " + std::to_string(uid));
    return it->second;
}

LabelNode* resolveLabelNode(InsnUidState& st, ca::u32 uid)
{
    auto* node = resolveInsn(st, uid);
    if (node->getType() != AbstractInsnNode::LABEL) {
        throw std::runtime_error("uid " + std::to_string(uid) + " is not a label node");
    }
    return static_cast<LabelNode*>(node);
}

/// 发号 + 挂链 + 标脏的公共尾（B9：节点一律经 m->create 归属方法 arena）。
template <class T, class... Args>
ca::u32 emitNode(FacadeSession& s, MethodRef2 r, std::optional<ca::u32> pos, Args&&... args)
{
    auto& st   = stateFor(s, r);
    auto* node = r.m->create<T>(std::forward<Args>(args)...);
    if (pos) {
        r.m->instructions.insert(resolveInsn(st, *pos), node);
    }
    else {
        r.m->instructions.add(node);
    }
    const ca::u32 uid = st.nextUid++;
    st.byUid.emplace(uid, node);
    st.uidOf.emplace(node, uid);
    st.labelIndexDirty = true;   // 新节点可能携带新 Label
    markDirty(r);
    return uid;
}

/// Label* → uid 反查（缓存读侧；内容 = byUid ∖ dead ∩ LABEL，与逐次重建恒等）。
const std::unordered_map<Label*, ca::u32>& labelIndex(InsnUidState& st)
{
    if (st.labelIndexDirty) {
        st.labelIndex.clear();
        for (const auto& [uid, node] : st.byUid) {
            if (st.dead.count(uid)) continue;
            if (node->getType() == AbstractInsnNode::LABEL) {
                st.labelIndex[static_cast<LabelNode*>(node)->label] = uid;
            }
        }
        st.labelIndexDirty = false;
    }
    return st.labelIndex;
}

InsnDetail detailOf(InsnUidState& st, MethodNode* m, ca::u32 uid)
{
    auto*        node = resolveInsn(st, uid);
    InsnDetail   d;
    d.uid    = uid;
    d.opcode = node->opcode;
    d.kind   = node->getType();
    switch (d.kind) {
    case AbstractInsnNode::INT_INSN:
        d.operand = static_cast<IntInsnNode*>(node)->operand;
        break;
    case AbstractInsnNode::VAR_INSN:
        d.var = static_cast<VarInsnNode*>(node)->var;
        break;
    case AbstractInsnNode::TYPE_INSN:
        d.desc = static_cast<TypeInsnNode*>(node)->desc.to_std_string();
        break;
    case AbstractInsnNode::FIELD_INSN: {
        const auto& f = *static_cast<FieldInsnNode*>(node);
        d.owner = f.owner.to_std_string();
        d.name  = f.name.to_std_string();
        d.desc  = f.desc.to_std_string();
        break;
    }
    case AbstractInsnNode::METHOD_INSN: {
        const auto& mi = *static_cast<MethodInsnNode*>(node);
        d.owner = mi.owner.to_std_string();
        d.name  = mi.name.to_std_string();
        d.desc  = mi.desc.to_std_string();
        d.itf   = mi.isInterface;
        break;
    }
    case AbstractInsnNode::INVOKE_DYNAMIC_INSN: {
        const auto& id = *static_cast<InvokeDynamicInsnNode*>(node);
        d.name = id.name.to_std_string();
        d.desc = id.desc.to_std_string();
        break;
    }
    case AbstractInsnNode::JUMP_INSN: {
        const auto& labels = labelIndex(st);
        auto it     = labels.find(static_cast<JumpInsnNode*>(node)->label);
        if (it != labels.end()) d.targetUid = it->second;
        break;
    }
    case AbstractInsnNode::LDC_INSN: {
        const auto& cst = static_cast<LdcInsnNode*>(node)->cst;
        if (auto* v = cst.as<ca::str::Utf8StringRef>()) {
            d.cstTag = 1;
            d.cstStr = v->to_std_string();
        }
        else if (auto* v = cst.as<int>()) {
            d.cstTag = 2;
            d.cstInt = *v;
        }
        else if (auto* v = cst.as<ca::i64>()) {
            d.cstTag = 3;
            d.cstInt = *v;
        }
        else if (auto* v = cst.as<float>()) {
            d.cstTag = 4;
            d.cstFloat = *v;
        }
        else if (auto* v = cst.as<double>()) {
            d.cstTag = 5;
            d.cstFloat = *v;
        }
        break;
    }
    case AbstractInsnNode::IINC_INSN: {
        const auto* ii = static_cast<IincInsnNode*>(node);
        d.var  = ii->var;
        d.incr = ii->incr;
        break;
    }
    case AbstractInsnNode::TABLESWITCH_INSN: {
        const auto* ts  = static_cast<TableSwitchInsnNode*>(node);
        const auto& labels = labelIndex(st);
        d.switchMin = ts->min;
        d.switchMax = ts->max;
        auto dIt    = labels.find(ts->dflt);
        if (dIt != labels.end()) d.defaultUid = dIt->second;
        for (auto* l : ts->labels) {
            auto it = labels.find(l);
            d.targetUids.push_back(it != labels.end() ? it->second : 0);
        }
        break;
    }
    case AbstractInsnNode::LOOKUPSWITCH_INSN: {
        const auto* ls  = static_cast<LookupSwitchInsnNode*>(node);
        const auto& labels = labelIndex(st);
        d.switchKeys = ls->keys;
        auto dIt     = labels.find(ls->dflt);
        if (dIt != labels.end()) d.defaultUid = dIt->second;
        for (auto* l : ls->labels) {
            auto it = labels.find(l);
            d.targetUids.push_back(it != labels.end() ? it->second : 0);
        }
        break;
    }
    case AbstractInsnNode::MULTIANEWARRAY_INSN: {
        const auto* ma = static_cast<MultiANewArrayInsnNode*>(node);
        d.desc = ma->desc.to_std_string();
        d.dims = ma->dims;
        break;
    }
    case AbstractInsnNode::LINE:
        d.line = static_cast<LineNumberNode*>(node)->line;
        break;
    default:
        break;
    }
    return d;
}

}   // namespace

const char* facadeInsnKindName(int kind)
{
    switch (kind) {
    case AbstractInsnNode::INSN: return "insn";
    case AbstractInsnNode::INT_INSN: return "int";
    case AbstractInsnNode::VAR_INSN: return "var";
    case AbstractInsnNode::TYPE_INSN: return "type";
    case AbstractInsnNode::FIELD_INSN: return "field";
    case AbstractInsnNode::METHOD_INSN: return "method";
    case AbstractInsnNode::INVOKE_DYNAMIC_INSN: return "indy";
    case AbstractInsnNode::JUMP_INSN: return "jump";
    case AbstractInsnNode::LABEL: return "label";
    case AbstractInsnNode::LDC_INSN: return "ldc";
    case AbstractInsnNode::IINC_INSN: return "iinc";
    case AbstractInsnNode::TABLESWITCH_INSN: return "tableswitch";
    case AbstractInsnNode::LOOKUPSWITCH_INSN: return "lookupswitch";
    case AbstractInsnNode::MULTIANEWARRAY_INSN: return "multianewarray";
    case AbstractInsnNode::FRAME: return "frame";
    case AbstractInsnNode::LINE: return "line";
    default: return "unknown";
    }
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

// ── 读 ──

ca::core::Any facadeInternedStr(FacadeSession& s, const std::string& v)
{
    return ca::core::Any(internIn(s, v));
}

void facadeParseClass(FacadeSession& s, const std::string& name) { resolveTree(s, name); }

ClassInfo facadeClassInfo(FacadeSession& s, const std::string& name)
{
    auto& te = resolveTree(s, name);
    ClassInfo out;
    out.name       = te.node->name.to_std_string();
    out.superName  = te.node->superName.to_std_string();
    out.access     = te.node->access;
    out.version    = te.node->version & 0xFFFF;
    for (const auto& i : te.node->interfaces) out.interfaces.push_back(i.to_std_string());
    return out;
}

std::vector<std::tuple<std::string, std::string, int>> facadeMembers(FacadeSession& s,
                                                                     const std::string& name,
                                                                     bool methods)
{
    auto& te = resolveTree(s, name);
    std::vector<std::tuple<std::string, std::string, int>> out;
    if (methods) {
        for (auto* m : te.node->methods)
            out.emplace_back(m->name.to_std_string(), m->desc.to_std_string(), m->access);
    }
    else {
        for (auto* f : te.node->fields)
            out.emplace_back(f->name.to_std_string(), f->desc.to_std_string(), f->access);
    }
    return out;
}

MemberInfo facadeMethodInfo(FacadeSession& s, const std::string& cls, const std::string& name,
                            const std::string& desc)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    MemberInfo out;
    out.name      = m->name.to_std_string();
    out.desc      = m->desc.to_std_string();
    out.access    = m->access;
    out.maxStack  = m->maxStack;
    out.maxLocals = m->maxLocals;
    for (const auto& e : m->exceptions) out.exceptions.push_back(e.to_std_string());
    return out;
}

MemberInfo facadeFieldInfo(FacadeSession& s, const std::string& cls, const std::string& name,
                           const std::string& desc)
{
    auto&      te = resolveTree(s, cls);
    auto*      f  = resolveField(te, name, desc);
    MemberInfo out;
    out.name   = f->name.to_std_string();
    out.desc   = f->desc.to_std_string();
    out.access = f->access;
    return out;
}

ClassHierarchyReport facadeClassHierarchy(FacadeSession& s, const std::string& name)
{
    // 继承闭包查询（super 链 + 全部接口传递闭包 + 沿途声明成员集）：源码树建
    // 继承成员集不必再外跑 javap/反射。容器外父类/接口（java/lang/Object 及
    // JDK 类型常态）不可解析即跳过——闭包只覆盖容器内可达类；起点类不存在
    // 照旧 StaleRefError（fail-closed）。BFS + visited 集防环。
    resolveTree(s, name);   // 起点存在性校验（缺失抛 StaleRefError）

    ClassHierarchyReport    out;
    std::vector<std::string> pending{name};
    std::set<std::string>    visited;
    while (!pending.empty()) {
        const std::string current = pending.front();
        pending.erase(pending.begin());
        if (!visited.insert(current).second) continue;

        if (s.duplicateUris.count(current) != 0 || s.nameToUri.count(current) == 0) {
            continue;   // 容器外引用（JDK 类等）：闭包不含，不报错
        }
        TreeEntry& te = resolveTree(s, current);

        HierarchyClass info;
        info.name       = te.node->name.to_std_string();
        info.superName  = te.node->superName.to_std_string();
        for (const auto& i : te.node->interfaces) info.interfaces.push_back(i.to_std_string());
        for (auto* m : te.node->methods) {
            out.methods.emplace_back(info.name, m->name.to_std_string(),
                                     m->desc.to_std_string(), m->access);
        }
        for (auto* f : te.node->fields) {
            out.fields.emplace_back(info.name, f->name.to_std_string(),
                                    f->desc.to_std_string(), f->access);
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
    auto& te             = resolveTree(s, name);
    te.node->superName   = internIn(s, super);
    te.dirty             = true;
}

void facadeClassSetAccess(FacadeSession& s, const std::string& name, int access)
{
    auto& te         = resolveTree(s, name);
    te.node->access  = access;
    te.dirty         = true;
}

void facadeClassSetVersion(FacadeSession& s, const std::string& name, int version)
{
    auto& te           = resolveTree(s, name);
    te.node->version   = version;
    te.dirty           = true;
}

void facadeClassSetInterfaces(FacadeSession& s, const std::string& name,
                              const std::vector<std::string>& interfaces)
{
    auto& te = resolveTree(s, name);
    te.node->interfaces.clear();
    for (const auto& i : interfaces) te.node->interfaces.push_back(internIn(s, i));
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

    auto& te           = s.trees.at(oldName);
    te.node->name      = internIn(s, newName);
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
    return newName;
}

// ── 写（方法/字段） ──

std::string facadeRenameMethod(FacadeSession& s, const std::string& cls, const std::string& name,
                               const std::string& desc, const std::string& newName)
{
    auto& te = resolveTree(s, cls);
    auto* m  = resolveMethod(te, name, desc);
    if (newName != name && methodExists(te, newName, desc)) {
        throw StaleRefError("method already exists: " + newName + desc);
    }
    m->name  = internIn(s, newName);
    te.dirty = true;
    return newName;
}

std::string facadeRenameField(FacadeSession& s, const std::string& cls, const std::string& name,
                              const std::string& desc, const std::string& newName)
{
    auto& te = resolveTree(s, cls);
    auto* f  = resolveField(te, name, desc);
    if (newName != name) {
        for (auto* other : te.node->fields) {
            if (other != f && other->name.to_std_string() == newName) {
                throw StaleRefError("field already exists: " + newName);
            }
        }
    }
    f->name  = internIn(s, newName);
    te.dirty = true;
    return newName;
}

void facadeMethodSetDesc(FacadeSession& s, const std::string& cls, const std::string& name,
                         const std::string& desc, const std::string& newDesc)
{
    auto& te = resolveTree(s, cls);
    auto* m  = resolveMethod(te, name, desc);
    if (newDesc != desc && methodExists(te, name, newDesc)) {
        throw StaleRefError("method already exists: " + name + newDesc);
    }
    m->desc  = internIn(s, newDesc);
    te.dirty = true;
}

void facadeMethodSetAccess(FacadeSession& s, const std::string& cls, const std::string& name,
                           const std::string& desc, int access)
{
    auto& te = resolveTree(s, cls);
    resolveMethod(te, name, desc)->access = access;
    te.dirty                              = true;
}

void facadeMethodSetMaxStack(FacadeSession& s, const std::string& cls, const std::string& name,
                             const std::string& desc, int v)
{
    auto& te = resolveTree(s, cls);
    resolveMethod(te, name, desc)->maxStack = v;
    te.dirty                                = true;
}

void facadeMethodSetMaxLocals(FacadeSession& s, const std::string& cls, const std::string& name,
                              const std::string& desc, int v)
{
    auto& te = resolveTree(s, cls);
    resolveMethod(te, name, desc)->maxLocals = v;
    te.dirty                                 = true;
}

void facadeMethodSetExceptions(FacadeSession& s, const std::string& cls, const std::string& name,
                               const std::string& desc,
                               const std::vector<std::string>& exceptions)
{
    auto& te = resolveTree(s, cls);
    auto* m  = resolveMethod(te, name, desc);
    m->exceptions.clear();
    for (const auto& e : exceptions) m->exceptions.push_back(internIn(s, e));
    te.dirty = true;
}

void facadeFieldSetAccess(FacadeSession& s, const std::string& cls, const std::string& name,
                          const std::string& desc, int access)
{
    auto& te = resolveTree(s, cls);
    resolveField(te, name, desc)->access = access;
    te.dirty                             = true;
}

std::string facadeAddMethod(FacadeSession& s, const std::string& cls, const std::string& name,
                            const std::string& desc, int access, int maxStack, int maxLocals)
{
    auto& te = resolveTree(s, cls);
    if (methodExists(te, name, desc)) {
        throw StaleRefError("method already exists: " + name + desc);
    }
    // MethodNode 本体堆对象（ClassNode 析构统一 delete，nodes.hpp:637-638 契约）。
    static const std::vector<ca::str::Utf8StringRef> kNoExceptions;
    auto* m = new MethodNode(access, internIn(s, name), internIn(s, desc),
                             ca::str::Utf8StringRef{}, kNoExceptions);
    m->maxStack  = maxStack;
    m->maxLocals = maxLocals;
    te.node->methods.push_back(m);
    te.dirty = true;
    return name;
}

std::string facadeAddField(FacadeSession& s, const std::string& cls, const std::string& name,
                           const std::string& desc, int access)
{
    auto& te = resolveTree(s, cls);
    if (fieldExists(te, name, desc)) throw StaleRefError("field already exists: " + name);
    auto* f = new FieldNode(access, internIn(s, name), internIn(s, desc),
                            ca::str::Utf8StringRef{}, ca::core::Any());
    te.node->fields.push_back(f);
    te.dirty = true;
    return name;
}

void facadeRemoveMethod(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc)
{
    auto& te = resolveTree(s, cls);
    auto* m  = resolveMethod(te, name, desc);
    te.node->methods.erase(std::remove(te.node->methods.begin(), te.node->methods.end(), m),
                           te.node->methods.end());
    te.uidStates.erase(m);   // 墓地随方法销毁，uid 不再可解析
    delete m;                // MethodNode 堆对象（ClassNode dtor 只删余下成员）
    te.dirty = true;
}

void facadeRemoveField(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc)
{
    auto& te = resolveTree(s, cls);
    auto* f  = resolveField(te, name, desc);
    te.node->fields.erase(std::remove(te.node->fields.begin(), te.node->fields.end(), f),
                          te.node->fields.end());
    delete f;
    te.dirty = true;
}

// ── 指令面 ──

ca::u32 facadeInsnCount(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    return static_cast<ca::u32>(m->instructions.size());
}

std::vector<InsnBrief> facadeInsns(FacadeSession& s, const std::string& cls,
                                   const std::string& name, const std::string& desc)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    std::vector<InsnBrief> out;
    for (auto* p = m->instructions.getFirst(); p; p = p->getNext()) {
        auto it = st.uidOf.find(p);
        if (it == st.uidOf.end()) continue;
        out.push_back(InsnBrief{it->second, p->opcode, p->getType()});
    }
    return out;
}

InsnDetail facadeInsnDetail(FacadeSession& s, const std::string& cls, const std::string& name,
                            const std::string& desc, ca::u32 uid)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    return detailOf(stateFor(s, r), r.m, uid);
}

ca::u32 facadeInsnIndex(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc, ca::u32 uid)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    return static_cast<ca::u32>(m->instructions.indexOf(resolveInsn(st, uid)));
}

namespace {

std::optional<ca::u32> uidOfNode(const InsnUidState& st, AbstractInsnNode* node)
{
    auto it = st.uidOf.find(node);
    if (it == st.uidOf.end()) return std::nullopt;
    return it->second;
}

}   // namespace

std::optional<ca::u32> facadeFirstInsn(FacadeSession& s, const std::string& cls,
                                       const std::string& name, const std::string& desc)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    return uidOfNode(st, m->instructions.getFirst());
}

std::optional<ca::u32> facadeLastInsn(FacadeSession& s, const std::string& cls,
                                      const std::string& name, const std::string& desc)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    return uidOfNode(st, m->instructions.getLast());
}

std::optional<ca::u32> facadeNextInsn(FacadeSession& s, const std::string& cls,
                                      const std::string& name, const std::string& desc,
                                      ca::u32 uid)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    return uidOfNode(st, resolveInsn(st, uid)->getNext());
}

std::optional<ca::u32> facadePrevInsn(FacadeSession& s, const std::string& cls,
                                      const std::string& name, const std::string& desc,
                                      ca::u32 uid)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    return uidOfNode(st, resolveInsn(st, uid)->getPrevious());
}

void facadeInsnSetOpcode(FacadeSession& s, const std::string& cls, const std::string& name,
                         const std::string& desc, ca::u32 uid, int op)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    resolveInsn(st, uid)->opcode = op;
    markDirty(r);
}

void facadeInsnSetVar(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::u32 uid, int var)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    auto* node = resolveInsn(st, uid);
    if (node->getType() == AbstractInsnNode::VAR_INSN) {
        static_cast<VarInsnNode*>(node)->var = var;
    }
    else if (node->getType() == AbstractInsnNode::IINC_INSN) {
        static_cast<IincInsnNode*>(node)->var = var;
    }
    else {
        throw std::runtime_error("insn_set_var: kind mismatch (" +
                                 std::string(facadeInsnKindName(node->getType())) + ")");
    }
    markDirty(r);
}

void facadeInsnSetIncr(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, ca::u32 uid, int incr)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    auto* node = resolveInsn(st, uid);
    if (node->getType() != AbstractInsnNode::IINC_INSN) {
        throw std::runtime_error("insn_set_incr: kind mismatch (" +
                                 std::string(facadeInsnKindName(node->getType())) + ")");
    }
    static_cast<IincInsnNode*>(node)->incr = incr;
    markDirty(r);
}

void facadeInsnSetOperand(FacadeSession& s, const std::string& cls, const std::string& name,
                          const std::string& desc, ca::u32 uid, int operand)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    auto* node = resolveInsn(st, uid);
    if (node->getType() != AbstractInsnNode::INT_INSN) {
        throw std::runtime_error("insn_set_operand: kind mismatch (" +
                                 std::string(facadeInsnKindName(node->getType())) + ")");
    }
    static_cast<IntInsnNode*>(node)->operand = operand;
    markDirty(r);
}

void facadeInsnSetDesc(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, ca::u32 uid, const std::string& typeDesc)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    auto* node = resolveInsn(st, uid);
    if (node->getType() == AbstractInsnNode::TYPE_INSN) {
        static_cast<TypeInsnNode*>(node)->desc = internIn(s, typeDesc);
    }
    else if (node->getType() == AbstractInsnNode::MULTIANEWARRAY_INSN) {
        static_cast<MultiANewArrayInsnNode*>(node)->desc = internIn(s, typeDesc);
    }
    else {
        throw std::runtime_error("insn_set_desc: kind mismatch (" +
                                 std::string(facadeInsnKindName(node->getType())) + ")");
    }
    markDirty(r);
}

void facadeInsnSetFieldRef(FacadeSession& s, const std::string& cls, const std::string& name,
                           const std::string& desc, ca::u32 uid, const std::string& owner,
                           const std::string& refName, const std::string& refDesc)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    auto* node = resolveInsn(st, uid);
    if (node->getType() != AbstractInsnNode::FIELD_INSN) {
        throw std::runtime_error("insn_set_field_ref: kind mismatch (" +
                                 std::string(facadeInsnKindName(node->getType())) + ")");
    }
    auto& f     = *static_cast<FieldInsnNode*>(node);
    f.owner     = internIn(s, owner);
    f.name      = internIn(s, refName);
    f.desc      = internIn(s, refDesc);
    markDirty(r);
}

void facadeInsnSetMethodRef(FacadeSession& s, const std::string& cls, const std::string& name,
                            const std::string& desc, ca::u32 uid, const std::string& owner,
                            const std::string& refName, const std::string& refDesc, bool itf)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    auto* node = resolveInsn(st, uid);
    if (node->getType() != AbstractInsnNode::METHOD_INSN) {
        throw std::runtime_error("insn_set_method_ref: kind mismatch (" +
                                 std::string(facadeInsnKindName(node->getType())) + ")");
    }
    auto& mi  = *static_cast<MethodInsnNode*>(node);
    mi.owner  = internIn(s, owner);
    mi.name   = internIn(s, refName);
    mi.desc   = internIn(s, refDesc);
    mi.isInterface = itf;
    markDirty(r);
}

void facadeInsnSetCst(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::u32 uid, ca::core::Any value)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    auto* node = resolveInsn(st, uid);
    if (node->getType() != AbstractInsnNode::LDC_INSN) {
        throw std::runtime_error("insn_set_cst: kind mismatch (" +
                                 std::string(facadeInsnKindName(node->getType())) + ")");
    }
    static_cast<LdcInsnNode*>(node)->cst = std::move(value);
    markDirty(r);
}

void facadeInsnSetTarget(FacadeSession& s, const std::string& cls, const std::string& name,
                         const std::string& desc, ca::u32 uid, ca::u32 targetUid)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    auto* node = resolveInsn(st, uid);
    if (node->getType() != AbstractInsnNode::JUMP_INSN) {
        throw std::runtime_error("insn_set_target: kind mismatch (" +
                                 std::string(facadeInsnKindName(node->getType())) + ")");
    }
    auto* target = resolveLabelNode(st, targetUid);
    static_cast<JumpInsnNode*>(node)->label = target->label;
    markDirty(r);
}

ca::u32 facadeEmitInsn(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int op, std::optional<ca::u32> pos)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    return emitNode<InsnNode>(s, r, pos, op);
}

ca::u32 facadeEmitVar(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, int op, int var, std::optional<ca::u32> pos)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    return emitNode<VarInsnNode>(s, r, pos, op, var);
}

ca::u32 facadeEmitIinc(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int var, int incr, std::optional<ca::u32> pos)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    return emitNode<IincInsnNode>(s, r, pos, var, incr);
}

ca::u32 facadeEmitType(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int op, const std::string& typeDesc,
                       std::optional<ca::u32> pos)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    return emitNode<TypeInsnNode>(s, r, pos, op, internIn(s, typeDesc));
}

ca::u32 facadeEmitField(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc, int op, const std::string& owner,
                        const std::string& refName, const std::string& refDesc,
                        std::optional<ca::u32> pos)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    return emitNode<FieldInsnNode>(s, r, pos, op, internIn(s, owner), internIn(s, refName),
                                   internIn(s, refDesc));
}

ca::u32 facadeEmitMethodCall(FacadeSession& s, const std::string& cls, const std::string& name,
                             const std::string& desc, int op, const std::string& owner,
                             const std::string& refName, const std::string& refDesc, bool itf,
                             std::optional<ca::u32> pos)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    return emitNode<MethodInsnNode>(s, r, pos, op, internIn(s, owner), internIn(s, refName),
                                    internIn(s, refDesc), itf);
}

ca::u32 facadeEmitLdc(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::core::Any value, std::optional<ca::u32> pos)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    return emitNode<LdcInsnNode>(s, r, pos, std::move(value));
}

ca::u32 facadeEmitJump(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int op, ca::u32 targetUid,
                       std::optional<ca::u32> pos)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    auto* target = resolveLabelNode(st, targetUid);
    return emitNode<JumpInsnNode>(s, r, pos, op, target->label);
}

namespace {

/// switch 目标解析：default + targets 全数校验为既有 label 节点
/// （返回序：[default, target0, target1, ...]）。
std::vector<Label*> resolveSwitchTargets(InsnUidState& st, ca::u32 defaultUid,
                                         const std::vector<ca::u32>& targetUids)
{
    std::vector<Label*> out;
    out.reserve(targetUids.size() + 1);
    out.push_back(resolveLabelNode(st, defaultUid)->label);
    for (const auto uid : targetUids) out.push_back(resolveLabelNode(st, uid)->label);
    return out;
}

}   // namespace

ca::u32 facadeEmitTableSwitch(FacadeSession& s, const std::string& cls, const std::string& name,
                              const std::string& desc, int min, int max, ca::u32 defaultUid,
                              const std::vector<ca::u32>& targetUids, std::optional<ca::u32> pos)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    // 结构性校验（fail-closed）：表项数与值域宽度必须一致，否则写出的类
    // 跳转表错位（真机验证必炸）。
    if (static_cast<ca::i64>(max) - static_cast<ca::i64>(min) + 1 !=
        static_cast<ca::i64>(targetUids.size())) {
        throw std::runtime_error(
            "emit_switch table: targets count (" + std::to_string(targetUids.size()) +
            ") != max-min+1 (" + std::to_string(static_cast<ca::i64>(max) - min + 1) + ")");
    }
    auto labels = resolveSwitchTargets(st, defaultUid, targetUids);
    auto* dflt  = labels.front();
    labels.erase(labels.begin());
    return emitNode<TableSwitchInsnNode>(s, r, pos, min, max, dflt, labels);
}

ca::u32 facadeEmitLookupSwitch(FacadeSession& s, const std::string& cls, const std::string& name,
                               const std::string& desc, const std::vector<int>& keys,
                               ca::u32 defaultUid, const std::vector<ca::u32>& targetUids,
                               std::optional<ca::u32> pos)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    // 结构性校验（fail-closed）：key/target 成对 + JVM 要求 key 严格升序
    // （乱序写出的类真机 lookupswitch 二分查找结果未定义）。
    if (keys.size() != targetUids.size()) {
        throw std::runtime_error("emit_switch lookup: keys count (" + std::to_string(keys.size()) +
                                 ") != targets count (" + std::to_string(targetUids.size()) + ")");
    }
    for (std::size_t i = 1; i < keys.size(); ++i) {
        if (keys[i - 1] >= keys[i]) {
            throw std::runtime_error("emit_switch lookup: keys not strictly ascending at " +
                                     std::to_string(i));
        }
    }
    auto labels = resolveSwitchTargets(st, defaultUid, targetUids);
    auto* dflt  = labels.front();
    labels.erase(labels.begin());
    return emitNode<LookupSwitchInsnNode>(s, r, pos, dflt, keys, labels);
}

ca::u32 facadeEmitLabel(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc, std::optional<ca::u32> pos)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    // B8 归属制：Label 归方法 arena（v2 堆 new Label 由 LabelNode 空析构泄漏，此处置正）。
    auto* label = m->arena().create<Label>();
    label->setOwner(&m->arena());
    return emitNode<LabelNode>(s, r, pos, label);
}

void facadeRemoveInsn(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::u32 uid)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    auto* node = resolveInsn(st, uid);
    // 摘链不销毁（insn_list.hpp:419 arena 节点语义）：节点内存随树回收，
    // uid 保留映射进墓地——后续解析 fail-closed（契约 §3.3）。
    m->instructions.remove(node);
    st.dead.insert(uid);
    st.labelIndexDirty = true;   // 墓地扩员：label 反查集随之收缩
    markDirty(r);
}

// ── 异常表 ──

std::vector<std::tuple<ca::u32, ca::u32, ca::u32, std::string>> facadeTryCatches(
    FacadeSession& s, const std::string& cls, const std::string& name, const std::string& desc)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    const auto& labels = labelIndex(stateFor(s, r));
    std::vector<std::tuple<ca::u32, ca::u32, ca::u32, std::string>> out;
    for (const auto* tc : m->tryCatchBlocks) {
        auto su = labels.find(tc->start);
        auto eu = labels.find(tc->end);
        auto hu = labels.find(tc->handler);
        out.emplace_back(su != labels.end() ? su->second : 0, eu != labels.end() ? eu->second : 0,
                         hu != labels.end() ? hu->second : 0, tc->type.to_std_string());
    }
    return out;
}

void facadeAddTryCatch(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, ca::u32 startUid, ca::u32 endUid,
                       ca::u32 handlerUid, const std::string& catchType)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    auto& st = stateFor(s, r);
    auto* ls = resolveLabelNode(st, startUid);
    auto* le = resolveLabelNode(st, endUid);
    auto* lh = resolveLabelNode(st, handlerUid);
    // TryCatchBlockNode 经方法 nodeArena_ 分配（pysdk 双重释放定案）。
    m->tryCatchBlocks.push_back(
        m->create<TryCatchBlockNode>(ls->label, le->label, lh->label, internIn(s, catchType)));
    markDirty(r);
}

void facadeRemoveTryCatch(FacadeSession& s, const std::string& cls, const std::string& name,
                          const std::string& desc, ca::usize rowIndex)
{
    const auto r = resolveMethodFlex(s, cls, name, desc);
    auto* m = r.m;
    if (rowIndex >= m->tryCatchBlocks.size()) {
        throw StaleRefError("try-catch row out of range: " + std::to_string(rowIndex));
    }
    m->tryCatchBlocks.erase(m->tryCatchBlocks.begin() +
                            static_cast<std::ptrdiff_t>(rowIndex));
    markDirty(r);
}

// ── 写回/落盘 ──

void facadeWriteBack(FacadeSession& s, const std::string& name, bool computeFrames)
{
    TreeEntry& te = resolveTree(s, name);
    jbytecode::core::ClassWriter writer(
        *s.arena, computeFrames ? jbytecode::core::ClassWriter::COMPUTE_FRAMES : 0);
    te.node->accept(&writer);
    auto bytes = writer.toByteArray();
    if (bytes.empty()) throw std::runtime_error("write_back: empty serialization for " + name);
    auto uriIt = s.nameToUri.find(name);
    if (uriIt == s.nameToUri.end()) throw StaleRefError("class not found: " + name);
    auto* r = s.container->getResource(uriIt->second);
    if (!ca::core::tag_cast::isa<ClassResource>(r)) {
        throw std::runtime_error("not a class entry: " + name);
    }
    auto* cr = ca::core::tag_cast::cast<ClassResource>(r);
    cr->set(std::move(bytes));
    te.dirty = false;
    facadeSyncEntries(s);   // 挂账的类改名条目搬移随写回落账（G2）
}

std::vector<std::string> facadeSyncEntries(FacadeSession& s)
{
    std::vector<std::string> moved;
    for (const auto& [oldUri, newUri] : s.pendingEntryRenames) {
        auto* r  = s.container->getResource(oldUri);
        auto* cr = r ? ca::core::tag_cast::dyn_cast<ClassResource>(r) : nullptr;
        if (!cr) continue;   // 条目已不在：挂账作废
        std::vector<ca::u8> bytes = cr->get();   // 拷贝后再搬（同容器内 move 键）
        s.container->getResources().erase(oldUri);
        s.container->addResource(newUri,
                                 std::make_unique<ClassResource>(newUri, std::move(bytes)));
        for (auto& [name, uri] : s.nameToUri) {
            if (uri == oldUri) uri = newUri;
        }
        moved.push_back(oldUri + " -> " + newUri);
    }
    s.pendingEntryRenames.clear();
    return moved;
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
    facadeFlush(s, computeFrames);
    facadeSyncEntries(s);
    stagedExportJar(*s.container, path);
}

// ── ephemeral 方法（契约 §3.6） ──────────────────────────────────────────

std::string facadeAdoptEphemeralMethod(FacadeSession& s, jbytecode::tree::MethodNode& m,
                                       const std::string& name, const std::string& desc)
{
    const std::string key = "\x00ephem:" + std::to_string(s.ephemeralNext++);
    s.ephemeralMethods[key] = &m;
    return key;
}

void facadeReleaseEphemeralMethod(FacadeSession& s, const std::string& ephemKey)
{
    auto it = s.ephemeralMethods.find(ephemKey);
    if (it == s.ephemeralMethods.end()) return;
    s.ephemeralUids.erase(it->second);
    s.ephemeralMethods.erase(it);
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
    // false：remap 本身保帧，且 facade 写回的 COMPUTE_FRAMES 对「尾插死代码」
    // 形状会产出损坏方法体（iconst_1 蒸发、长出 frame/label/athrow，实测）；
    // 需要重算帧的脚本应显式 flush(h, true)/save(compute_frames=true)。
    facadeFlush(s, /*computeFrames=*/false);
    mjbase::asm_remapper::RemapPolicy policy;
    policy.bytecode          = true;
    policy.reflectionStrings = reflectionStrings;
    policy.annotations       = annotations;
    policy.signatures        = signatures;
    policy.resources         = resources;
    // mjbase 不再收 arena（计划自驻留），返回 Result<bool, RemapError>。
    const auto applied =
        mjbase::asm_remapper::applyResourceRemapPlan(*s.container, t.plan, policy);
    if (applied.is_err()) {
        throw std::runtime_error(std::string("rename commit failed: ")
                                 + mjbase::asm_remapper::to_string(applied.unwrap_err()));
    }
    if (!applied.unwrap()) {
        throw std::runtime_error("rename commit failed (remapper rejected plan)");
    }
    s.clearTrees();     // 容器字节整体被改写 → 树缓存作废（lazy 重解析）
    s.rebuildIndex();   // remapper 连资源 URI 一起改名 → nameToUri 索引必须重建
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

void facadeRecordRun(FacadeSession& s, const std::string& name,
                     const std::shared_ptr<PyRunStats>& stats)
{
    s.lastStats    = stats;
    s.lastPipeline = name;
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

    s.container->visitResources([&](std::string_view uriView, const IResource& resource) {
        const std::string uri(uriView);
        deadlineCheck();   // 协作层取消检查点（每条目粒度；只读操作可安全中断）
        const auto* cr = ca::core::tag_cast::dyn_cast<const ClassResource>(&resource);
        if (!cr) {
            ++rep.nonClass;
            return;
        }
        // 解析闸：malformed 单独计（entry 路径进 detail）。arena 存活须覆盖
        // 节点字符串的全部读取（name 在 try 外读）——不得缩进 try 块内。
        auto node = std::make_unique<ClassNode>();
        ca::str::Utf8StringArena localArena;
        try {
            jbytecode::core::ClassReader reader(localArena, cr->get().data(),
                                                static_cast<ca::i32>(cr->get().size()));
            reader.accept(node.get(), 0);
        }
        catch (const std::exception& e) {
            ++rep.malformed;
            rep.malformedDetails.push_back({uri, e.what()});
            return;
        }

        // entry 路径 ↔ this_class 比对（oracle entry-probe 同款口径）。
        const std::string declaredName = node->name.to_std_string();
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
        if (!node->superName.is_empty()) refs.insert(node->superName.to_std_string());
        for (const auto& it : node->interfaces) refs.insert(it.to_std_string());
        for (const auto* f : node->fields) collectTypesFromDesc(f->desc.to_std_string(), refs);
        for (const auto* m : node->methods) {
            collectTypesFromDesc(m->desc.to_std_string(), refs);
            for (auto* n = m->instructions.getFirst(); n; n = n->getNext()) {
                switch (n->getType()) {
                case jbytecode::tree::AbstractInsnNode::TYPE_INSN:
                    refs.insert(static_cast<jbytecode::tree::TypeInsnNode*>(n)
                                    ->desc.to_std_string());
                    break;
                case jbytecode::tree::AbstractInsnNode::FIELD_INSN:
                    refs.insert(static_cast<jbytecode::tree::FieldInsnNode*>(n)
                                    ->owner.to_std_string());
                    break;
                case jbytecode::tree::AbstractInsnNode::METHOD_INSN:
                    refs.insert(static_cast<jbytecode::tree::MethodInsnNode*>(n)
                                    ->owner.to_std_string());
                    break;
                default:
                    break;
                }
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
        TreeEntry& te = *tePtr;
        ++rep.classes;
        for (auto* m : te.node->methods) {
            auto& st     = uidState(te, *m);           // 物化（墓地感知的 label 集）
            const auto& labels = labelIndex(st);
            std::map<const jbytecode::core::Label*, ca::usize> pos;   // label → 链表位次
            ca::usize p = 0;
            for (auto* n = m->instructions.getFirst(); n; n = n->getNext()) {
                if (n->getType() == jbytecode::tree::AbstractInsnNode::LABEL)
                    pos[static_cast<jbytecode::tree::LabelNode*>(n)->label] = p++;
            }
            std::set<std::tuple<int, const jbytecode::core::Label*, const jbytecode::core::Label*,
                                std::string, std::string>>
                seen;
            std::vector<jbytecode::tree::LocalVariableNode*> keep;
            for (auto* lv : m->localVariables) {
                const bool linked =
                    lv->start != nullptr && lv->end != nullptr &&
                    labels.count(lv->start) != 0 && labels.count(lv->end) != 0;
                const bool ordered = linked && pos.at(lv->start) < pos.at(lv->end);
                const auto key     = std::make_tuple(lv->index, lv->start, lv->end,
                                                     lv->name.to_std_string(),
                                                     lv->descriptor.to_std_string());
                if (!linked || !ordered) {
                    ++rep.dropped;
                    continue;
                }
                if (!seen.insert(key).second) {
                    ++rep.deduped;
                    continue;
                }
                keep.push_back(lv);
            }
            if (keep.size() != m->localVariables.size()) {
                m->localVariables = std::move(keep);
                markDirty(resolveMethodFlex(s, name, m->name.to_std_string(),
                                            m->desc.to_std_string()));
            }
        }
    }
    return rep;
}

namespace {

/// 被拒流水 → 脚本可读条目（owner/old/new/撞名对象定位，fail-closed 报错面）。
/// mjbase：proposal 字段为 string_view 驻留视图，detail 为 std::string。
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
        item.detail = entry.detail;
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
        throw std::runtime_error("suggestions: " + std::move(file).unwrap_err().message);
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
    options.policy   = policy;
    mjbase::renamer::FileSuggestionProvider provider(std::move(file).unwrap());
    mjbase::renamer::RenamerEngine          engine;
    auto result = engine.run(*s.container, {&provider}, options);
    // mjbase：ca::Status 通道换 ok+error 值对。
    if (!result.ok) {
        throw std::runtime_error("rename apply failed: " + result.error);
    }

    s.clearTrees();     // 容器字节整体被改写（管线直驱同款语义）
    s.rebuildIndex();
    facadeSyncEntries(s);   // 引擎走 remapper 资源搬移，无挂账；幂等清账

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
    // mjbase：随机源失败走 Result 值语义（不抛异常）。
    const auto generated =
        mjbase::renamer::generateMechanicalShadowNames(*s.container, *s.shadowTable);
    if (generated.is_err()) {
        throw std::runtime_error("shadow generate: " + generated.unwrap_err());
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

}   // namespace

/// 容器深拷贝（workspace cloneContainer 同款三分支；URI 视图指向资源自持
/// 文本，先物化再写入新容器）。
std::unique_ptr<ResourceContainer> facadeCloneContainer(const FacadeSession& s)
{
    auto out = std::make_unique<ResourceContainer>();
    s.container->visitResources([&](std::string_view uri, const IResource& resource) {
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
std::vector<JarByteDiff> facadeDiffContainers(const ResourceContainer& before,
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
    report.diffs         = facadeDiffContainers(*s.container, reference);
    for (const auto& diff : report.diffs) {
        if (s.container->getResource(diff.uri) == nullptr) {
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
    report.resourceCount = s.container->size();
    report.issues        = scanJarRepairIssuesLocal(*s.container,
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

ShadowSidecarReport facadeShadowGenerateFile(const std::string& jarPath,
                                             const std::string& file)
{
    deadlineCheck();
    std::string sidecarPath;
    auto container = importForSidecar(jarPath, file, sidecarPath);
    auto table     = loadSidecarTable(sidecarPath);
    // mjbase 补号幂等：已分配条目保留（kept），随机源失败走 Err 值语义。
    auto generated = mjbase::renamer::generateMechanicalShadowNames(container, table);
    if (generated.is_err()) {
        throw std::runtime_error("generate shadow names failed: " +
                                 std::move(generated).unwrap_err());
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

}   // namespace mjt::pysdk
