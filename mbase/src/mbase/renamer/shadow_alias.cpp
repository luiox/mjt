#include "mbase/renamer/shadow_alias.hpp"

#include <algorithm>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "jbytecode/core/readers.hpp"
#include "jbytecode/tree/nodes.hpp"
#include "jhasm/util/escape_util.hpp"
#include "libca/core/tag_cast.hpp"
#include "libca/crypto/random.hpp"
#include "mbase/mapping/identifier_util.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/model/resource_model.hpp"

#include <cstdio>
#include <stdexcept>

namespace mjt::base {
namespace {

using ca::str::Utf8String;
using ca::str::Utf8StringArena;
using ca::str::Utf8StringRef;

constexpr char kKeySeparator = '\x1f';

std::string toStd(const Utf8StringRef& value)
{
    return value.to_std_string();
}

Utf8String toUtf8(const std::string& value)
{
    return Utf8String::from_data(reinterpret_cast<const ca::u8*>(value.data()), value.size());
}

///混淆名可能是非法 UTF-8 的字节载体（MUTF-8 1:1 往返字节），登记一律走
///intern_raw 跳过校验，避免合法混淆名被静默置空。
Utf8StringRef internName(Utf8StringArena& arena, Utf8StringRef value)
{
    return value.is_empty() ? Utf8StringRef()
                            : arena.intern_raw(value.data(), value.byte_length());
}

///成员复合 key：owner\x1fname\x1fdesc（空 owner 同款拼装，前导分隔符防前缀串线）。
std::string composeMemberKey(const std::string& owner, const std::string& name,
                             const std::string& desc)
{
    std::string key;
    key.reserve(owner.size() + name.size() + desc.size() + 2);
    key += owner;
    key += kKeySeparator;
    key += name;
    key += kKeySeparator;
    key += desc;
    return key;
}

///(name, desc) 字典序比较器（钉死：类内稳定序）。
bool memberPairLess(const std::pair<std::string, std::string>& a,
                    const std::pair<std::string, std::string>& b)
{
    if (a.first != b.first) return a.first < b.first;
    return a.second < b.second;
}

///jasm 显示 token 的转义形式（与 libjhasm printer 的 literal 同一函数，
///保证改写映射的 key 与反汇编文本里的 token 逐字节一致）。
std::string jasmEscaped(const Utf8StringRef& value)
{
    const auto utf16   = ca::str::Utf16String::from_utf8_string(value);
    const auto escaped = jhasm::util::EscapeUtil::escape_literal(utf16.ref());
    return escaped.to_utf8_string().to_std_string();
}

///引用 token：4 字节 CSPRNG → 8 位小写 hex（uint32 %08x 语义）；
///与 taken 撞号则重生成（表内唯一）。随机源 = libca secure_random_bytes
///（系统 CSPRNG）；系统 RNG 失败属环境灾难，抛出而非自造随机源降级。
std::string freshUniqueToken8(std::unordered_set<std::string>& taken)
{
    for (;;) {
        auto bytes = ca::crypto::secure_random_bytes(4);
        if (bytes.is_err()) {
            throw std::runtime_error("secure_random_bytes failed: system RNG unavailable");
        }
        auto        data    = std::move(bytes).unwrap();
        const auto* raw     = data.as_ptr();
        char        buf[9]  = {};
        std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x", raw[0], raw[1], raw[2], raw[3]);
        if (taken.insert(std::string(buf, 8)).second) return std::string(buf, 8);
    }
}

std::string formatIndexName(const char* prefix, ca::usize index)
{
    return std::string(prefix) + std::to_string(index);
}

///jasm 文本扫描的分隔符：其余字符（含 / . $ < > \ 与转义序列）均为 token 字符。
bool isJasmDelimiter(char c)
{
    switch (c) {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
    case '{':
    case '}':
    case '(':
    case ')':
    case '[':
    case ']':
    case '"':
    case '\'':
    case ',':
    case ';': return true;
    default: return false;
    }
}

}   //namespace

//─────────────────────────── ShadowAliasTable ───────────────────────────

ShadowAliasTable ShadowAliasTable::fromMapping(const IntermediateMapping& mappings)
{
    ShadowAliasTable table;
    for (const auto& [key, value] : mappings.getClasses()) {
        table.addClass(value.oldName, value.newName, value.token);
    }
    for (const auto& [owner, fields] : mappings.getFields()) {
        for (const auto& fm : fields) {
            table.addField(fm.ownerName, fm.desc, fm.oldName, fm.newName, fm.token);
        }
    }
    for (const auto& [owner, methods] : mappings.getMethods()) {
        for (const auto& mm : methods) {
            table.addMethod(mm.ownerName, mm.desc, mm.oldName, mm.newName, mm.token);
        }
    }
    return table;
}

IntermediateMapping ShadowAliasTable::toMapping() const
{
    IntermediateMapping mappings;
    for (const auto& entry : entries_) {
        switch (entry.kind) {
        case ShadowKind::Class:
            mappings.addClass(entry.rawName, entry.shadowName, entry.token8);
            break;
        case ShadowKind::Field:
            mappings.addField(entry.owner, entry.desc, entry.rawName, entry.shadowName,
                              entry.token8);
            break;
        case ShadowKind::Method:
            mappings.addMethod(entry.owner, entry.desc, entry.rawName, entry.shadowName,
                               entry.token8);
            break;
        }
    }
    return mappings;
}

bool ShadowAliasTable::addClass(Utf8StringRef raw, Utf8StringRef shadow, Utf8StringRef token8)
{
    if (raw.is_empty() || shadow.is_empty()) return false;
    if (classByRaw_.find(raw) != classByRaw_.end()) return false;

    const auto rawView    = internName(arena_, raw);
    const auto shadowView = internName(arena_, shadow);
    const auto tokenView  = internName(arena_, token8);
    const auto index      = entries_.size();
    entries_.push_back(
        {ShadowKind::Class, Utf8StringRef(), Utf8StringRef(), rawView, shadowView, tokenView});
    classByRaw_.emplace(rawView, index);
    byShadowName_[shadowView].push_back(index);
    byRawName_[rawView].push_back(index);
    return true;
}

bool ShadowAliasTable::addField(Utf8StringRef owner, Utf8StringRef desc, Utf8StringRef raw,
                                Utf8StringRef shadow, Utf8StringRef token8)
{
    if (raw.is_empty() || shadow.is_empty()) return false;
    const auto key  = composeMemberKey(toStd(owner), toStd(raw), toStd(desc));
    const auto view = Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(key.data()),
                                              key.size());
    if (memberByOwnerKey_.find(view) != memberByOwnerKey_.end()) return false;

    const auto ownerView  = internName(arena_, owner);
    const auto descView   = internName(arena_, desc);
    const auto rawView    = internName(arena_, raw);
    const auto shadowView = internName(arena_, shadow);
    const auto tokenView  = internName(arena_, token8);
    const auto keyView    = arena_.intern(std::string_view(key));
    const auto index      = entries_.size();
    entries_.push_back({ShadowKind::Field, ownerView, descView, rawView, shadowView, tokenView});
    memberByOwnerKey_.emplace(keyView, index);
    byShadowName_[shadowView].push_back(index);
    byRawName_[rawView].push_back(index);
    return true;
}

bool ShadowAliasTable::addMethod(Utf8StringRef owner, Utf8StringRef desc, Utf8StringRef raw,
                                 Utf8StringRef shadow, Utf8StringRef token8)
{
    if (raw.is_empty() || shadow.is_empty()) return false;
    const auto key  = composeMemberKey(toStd(owner), toStd(raw), toStd(desc));
    const auto view = Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(key.data()),
                                              key.size());
    if (memberByOwnerKey_.find(view) != memberByOwnerKey_.end()) return false;

    const auto ownerView  = internName(arena_, owner);
    const auto descView   = internName(arena_, desc);
    const auto rawView    = internName(arena_, raw);
    const auto shadowView = internName(arena_, shadow);
    const auto tokenView  = internName(arena_, token8);
    const auto keyView    = arena_.intern(std::string_view(key));
    const auto index      = entries_.size();
    entries_.push_back({ShadowKind::Method, ownerView, descView, rawView, shadowView, tokenView});
    memberByOwnerKey_.emplace(keyView, index);
    byShadowName_[shadowView].push_back(index);
    byRawName_[rawView].push_back(index);
    return true;
}

Utf8StringRef ShadowAliasTable::classShadow(Utf8StringRef raw) const
{
    auto it = classByRaw_.find(raw);
    return it == classByRaw_.end() ? Utf8StringRef() : entries_[it->second].shadowName;
}

Utf8StringRef ShadowAliasTable::fieldShadow(Utf8StringRef owner, Utf8StringRef raw,
                                            Utf8StringRef desc) const
{
    const auto key  = composeMemberKey(toStd(owner), toStd(raw), toStd(desc));
    const auto view = Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(key.data()),
                                              key.size());
    auto it = memberByOwnerKey_.find(view);
    if (it == memberByOwnerKey_.end()) return Utf8StringRef();
    const auto& entry = entries_[it->second];
    return entry.kind == ShadowKind::Field ? entry.shadowName : Utf8StringRef();
}

Utf8StringRef ShadowAliasTable::methodShadow(Utf8StringRef owner, Utf8StringRef raw,
                                             Utf8StringRef desc) const
{
    const auto key  = composeMemberKey(toStd(owner), toStd(raw), toStd(desc));
    const auto view = Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(key.data()),
                                              key.size());
    auto it = memberByOwnerKey_.find(view);
    if (it == memberByOwnerKey_.end()) return Utf8StringRef();
    const auto& entry = entries_[it->second];
    return entry.kind == ShadowKind::Method ? entry.shadowName : Utf8StringRef();
}

ca::str::Utf8String ShadowAliasTable::renderClass(Utf8StringRef raw) const
{
    const auto shadow = classShadow(raw);
    const auto rawEsc = IdentifierUtil::printReadable(toStd(raw));
    if (shadow.is_empty()) return toUtf8(rawEsc);
    return toUtf8(IdentifierUtil::printReadable(toStd(shadow)) + " (" + rawEsc + ")");
}

ca::str::Utf8String ShadowAliasTable::renderMemberName(Utf8StringRef owner, Utf8StringRef raw,
                                                       Utf8StringRef desc) const
{
    auto shadow = methodShadow(owner, raw, desc);
    if (shadow.is_empty()) shadow = fieldShadow(owner, raw, desc);
    const auto rawEsc = IdentifierUtil::printReadable(toStd(raw));
    if (shadow.is_empty()) return toUtf8(rawEsc);
    return toUtf8(IdentifierUtil::printReadable(toStd(shadow)) + " (" + rawEsc + ")");
}

ca::str::Utf8String ShadowAliasTable::renderMember(Utf8StringRef owner, Utf8StringRef name,
                                                   Utf8StringRef desc) const
{
    const auto ownerEsc = IdentifierUtil::printReadable(toStd(owner));
    const auto nameEsc  = IdentifierUtil::printReadable(toStd(name));
    const auto descEsc  = IdentifierUtil::printReadable(toStd(desc));
    const auto rawForm  = ownerEsc + "." + nameEsc + ":" + descEsc;

    const auto ownerShadow = classShadow(owner);
    auto       nameShadow  = methodShadow(owner, name, desc);
    if (nameShadow.is_empty()) nameShadow = fieldShadow(owner, name, desc);
    if (ownerShadow.is_empty() && nameShadow.is_empty()) return toUtf8(rawForm);

    auto ownerPart = ownerEsc;
    if (!ownerShadow.is_empty()) {
        ownerPart = IdentifierUtil::printReadable(toStd(ownerShadow)) + " (" + ownerEsc + ")";
    }
    auto namePart = nameEsc;
    if (!nameShadow.is_empty()) {
        namePart = IdentifierUtil::printReadable(toStd(nameShadow)) + " (" + nameEsc + ")";
    }

    //P0-2（luiox/）：尾随 ` (完整原始形式)` 属信息删除，必须逐字段
    //核对尾段与前段完全重合才可去。当前格式下 owner/name 在前段必然出现
    //（直接或 `shadow (raw)` 括号内；结构性后缀比较，不用子串搜索，防名字
    //本身含 `(`/`)` 时误判），desc 恒以 `:` + descEsc 收尾 → 三字段全部重合，
    //尾段为纯重复，恒去；若未来格式变动致任一字段仅尾段可见，则保留尾段。
    const auto covered = [](const std::string& part, const std::string& rawEsc) {
        if (part == rawEsc) return true;
        const std::string paren = " (" + rawEsc + ")";
        return part.size() >= paren.size() &&
               part.compare(part.size() - paren.size(), paren.size(), paren) == 0;
    };
    std::string text = ownerPart + "." + namePart + ":" + descEsc;
    if (!(covered(ownerPart, ownerEsc) && covered(namePart, nameEsc))) {
        text += " (" + rawForm + ")";
    }
    return toUtf8(text);
}

std::vector<ShadowAliasEntry> ShadowAliasTable::findByShadowName(Utf8StringRef shadow) const
{
    std::vector<ShadowAliasEntry> hits;
    auto                          it = byShadowName_.find(shadow);
    if (it == byShadowName_.end()) return hits;
    for (auto index : it->second) hits.push_back(entries_[index]);
    return hits;
}

std::vector<ShadowAliasEntry> ShadowAliasTable::findByRawName(Utf8StringRef raw) const
{
    std::vector<ShadowAliasEntry> hits;
    auto                          it = byRawName_.find(raw);
    if (it == byRawName_.end()) return hits;
    for (auto index : it->second) hits.push_back(entries_[index]);
    return hits;
}

std::vector<ShadowAliasEntry> ShadowAliasTable::entriesOfClass(Utf8StringRef owner) const
{
    std::vector<ShadowAliasEntry> hits;
    if (auto* entry = classEntry(owner)) hits.push_back(*entry);
    for (const auto& entry : entries_) {
        if (entry.kind != ShadowKind::Class && entry.owner.equals(owner)) hits.push_back(entry);
    }
    return hits;
}

const ShadowAliasEntry* ShadowAliasTable::classEntry(Utf8StringRef raw) const
{
    auto it = classByRaw_.find(raw);
    return it == classByRaw_.end() ? nullptr : &entries_[it->second];
}

ca::usize ShadowAliasTable::backfillTokens()
{
    //已占用 token 集合（含历史条目）；新号在此域内唯一（碰撞重生成）。
    std::unordered_set<std::string> taken;
    taken.reserve(entries_.size() * 2);
    for (const auto& entry : entries_) {
        if (!entry.token8.is_empty()) taken.insert(toStd(entry.token8));
    }

    ca::usize assigned = 0;
    for (auto& entry : entries_) {
        if (!entry.token8.is_empty()) continue;
        entry.token8 = internName(
            arena_, Utf8StringRef::from_string_view(freshUniqueToken8(taken)));
        ++assigned;
    }
    return assigned;
}

bool ShadowAliasTable::classShadowTaken(Utf8StringRef shadow, Utf8StringRef exceptRaw) const
{
    auto it = byShadowName_.find(shadow);
    if (it == byShadowName_.end()) return false;
    for (auto index : it->second) {
        const auto& entry = entries_[index];
        if (entry.kind == ShadowKind::Class && !entry.rawName.equals(exceptRaw)) return true;
    }
    return false;
}

bool ShadowAliasTable::memberShadowTaken(Utf8StringRef owner, Utf8StringRef shadow) const
{
    auto it = byShadowName_.find(shadow);
    if (it == byShadowName_.end()) return false;
    for (auto index : it->second) {
        const auto& entry = entries_[index];
        if (entry.kind != ShadowKind::Class && entry.owner.equals(owner)) return true;
    }
    return false;
}

void ShadowAliasTable::clear()
{
    arena_.clear();
    entries_.clear();
    classByRaw_.clear();
    memberByOwnerKey_.clear();
    byShadowName_.clear();
    byRawName_.clear();
}

//─────────────────────────── 批量机械占位名生成 ───────────────────────────

namespace {

///容器内一个 class 的成员采样（生成器排序输入；std::string 便于稳定字典序）。
struct ClassSample
{
    std::string                                      name;
    std::vector<std::pair<std::string, std::string>> methods;  //(name, desc)
    std::vector<std::pair<std::string, std::string>> fields;   //(name, desc)
};

std::vector<ClassSample> sampleContainerClasses(ResourceContainer& container)
{
    std::vector<ClassSample> samples;
    for (const auto& [uri, resource] : container.getResources()) {
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto*       cls   = ca::core::tag_cast::cast<ClassResource>(resource.get());
        const auto& bytes = cls->get();
        if (bytes.empty()) continue;

        //逐类独立 arena：解析产物只活到采样完成，字符串统一 intern 进目标表。
        Utf8StringArena localArena;
        jbytecode::tree::ClassNode node;
        jbytecode::core::ClassReader reader(localArena, bytes.data(), static_cast<ca::i32>(bytes.size()));
        reader.accept(&node, jbytecode::core::ClassReader::SKIP_DEBUG | jbytecode::core::ClassReader::SKIP_FRAMES);

        ClassSample sample;
        sample.name = toStd(node.name);
        for (const auto* method : node.methods) {
            sample.methods.emplace_back(toStd(method->name), toStd(method->desc));
        }
        for (const auto* field : node.fields) {
            sample.fields.emplace_back(toStd(field->name), toStd(field->desc));
        }
        samples.push_back(std::move(sample));
    }
    return samples;
}

}   //namespace

ShadowGenStats generateMechanicalShadowNames(ResourceContainer& container,
                                             ShadowAliasTable&  table)
{
    ShadowGenStats stats;
    stats.keptEntries = table.size();

    auto samples = sampleContainerClasses(container);
    std::sort(samples.begin(), samples.end(),
              [](const ClassSample& a, const ClassSample& b) { return a.name < b.name; });

    //类内成员按 (name, desc) 字典序；N 为"分配计数器"（从 1 递增，只在实际
    //分配时前进）——已有 shadow（手工名/上一轮批号）的符号跳过不占号，撞名
    //递增跳过；jar 不变重跑零改写（幂等），容器遍历序不影响输出（先排序）。
    ca::usize classCounter = 1;
    for (const auto& sample : samples) {
        const auto owner = Utf8StringRef::from_string_view(sample.name);

        if (table.classEntry(owner) == nullptr) {
            ca::usize n = classCounter;
            while (table.classShadowTaken(
                Utf8StringRef::from_string_view(formatIndexName("Class_", n)), owner)) {
                ++n;
            }
            table.addClass(owner,
                           Utf8StringRef::from_string_view(formatIndexName("Class_", n)));
            classCounter = n + 1;
            ++stats.assignedClasses;
        }

        auto sortedMethods = sample.methods;
        std::sort(sortedMethods.begin(), sortedMethods.end(), memberPairLess);
        ca::usize methodCounter = 1;
        for (const auto& [methodName, methodDesc] : sortedMethods) {
            const auto raw  = Utf8StringRef::from_string_view(methodName);
            const auto desc = Utf8StringRef::from_string_view(methodDesc);
            if (!table.methodShadow(owner, raw, desc).is_empty()) continue;
            ca::usize n = methodCounter;
            while (table.memberShadowTaken(
                owner, Utf8StringRef::from_string_view(formatIndexName("Method_", n)))) {
                ++n;
            }
            table.addMethod(owner, desc, raw,
                            Utf8StringRef::from_string_view(formatIndexName("Method_", n)));
            methodCounter = n + 1;
            ++stats.assignedMethods;
        }

        auto sortedFields = sample.fields;
        std::sort(sortedFields.begin(), sortedFields.end(), memberPairLess);
        ca::usize fieldCounter = 1;
        for (const auto& [fieldName, fieldDesc] : sortedFields) {
            const auto raw  = Utf8StringRef::from_string_view(fieldName);
            const auto desc = Utf8StringRef::from_string_view(fieldDesc);
            if (!table.fieldShadow(owner, raw, desc).is_empty()) continue;
            ca::usize n = fieldCounter;
            while (table.memberShadowTaken(
                owner, Utf8StringRef::from_string_view(formatIndexName("Field_", n)))) {
                ++n;
            }
            table.addField(owner, desc, raw,
                           Utf8StringRef::from_string_view(formatIndexName("Field_", n)));
            fieldCounter = n + 1;
            ++stats.assignedFields;
        }
    }

    //：token 列收尾——新条目与历史缺号条目（v1 sidecar 遗留）一律补
    //CSPRNG token8（表内唯一；已有 token 原样保留，幂等）。随机列不参与
    //上面的编号与排序，编号确定性不受影响。
    table.backfillTokens();
    return stats;
}

ca::usize pruneShadowAliases(ResourceContainer& container, ShadowAliasTable& table)
{
    //存在性索引：class 集合 + 每 owner 的成员 (kind, desc, name) 集合。
    std::unordered_set<std::string> classes;
    std::unordered_map<std::string, std::unordered_set<std::string>> membersByOwner;
    for (const auto& sample : sampleContainerClasses(container)) {
        classes.insert(sample.name);
        auto& members = membersByOwner[sample.name];
        for (const auto& [methodName, methodDesc] : sample.methods) {
            members.insert(std::string("m") + kKeySeparator + methodName + kKeySeparator +
                           methodDesc);
        }
        for (const auto& [fieldName, fieldDesc] : sample.fields) {
            members.insert(std::string("f") + kKeySeparator + fieldName + kKeySeparator +
                           fieldDesc);
        }
    }

    //先收集存活条目（owned 拷贝），再原地重建——arena_.clear 使旧视图失效，
    //重建期不得再引用旧条目。token8 随行（：prune 不得丢 token）。
    struct KeptEntry
    {
        ShadowKind  kind;
        std::string owner;
        std::string desc;
        std::string raw;
        std::string shadow;
        std::string token8;
    };
    std::vector<KeptEntry> kept;
    for (const auto& entry : table.entries()) {
        const auto owner = toStd(entry.owner);
        const auto desc  = toStd(entry.desc);
        const auto raw   = toStd(entry.rawName);
        bool       alive = false;
        if (entry.kind == ShadowKind::Class) {
            alive = classes.count(raw) > 0;
        }
        else if (classes.count(owner) > 0) {
            const auto&       members = membersByOwner[owner];
            const std::string key     = std::string(entry.kind == ShadowKind::Field ? "f" : "m") +
                                    kKeySeparator + raw + kKeySeparator + desc;
            alive = members.count(key) > 0;
        }
        if (alive)
            kept.push_back(
                {entry.kind, owner, desc, raw, toStd(entry.shadowName), toStd(entry.token8)});
    }
    const ca::usize removed = table.size() - kept.size();
    if (removed == 0) return 0;

    table.clear();
    for (const auto& entry : kept) {
        const auto owner  = Utf8StringRef::from_string_view(entry.owner);
        const auto desc   = Utf8StringRef::from_string_view(entry.desc);
        const auto raw    = Utf8StringRef::from_string_view(entry.raw);
        const auto shadow = Utf8StringRef::from_string_view(entry.shadow);
        const auto token  = Utf8StringRef::from_string_view(entry.token8);
        switch (entry.kind) {
        case ShadowKind::Class: table.addClass(raw, shadow, token); break;
        case ShadowKind::Field: table.addField(owner, desc, raw, shadow, token); break;
        case ShadowKind::Method: table.addMethod(owner, desc, raw, shadow, token); break;
        }
    }
    return removed;
}

//─────────────────────────── jasm 显示层改写 ───────────────────────────

ca::str::Utf8String rewriteJasmForDisplay(const ca::str::Utf8String& jasmText,
                                          const ShadowAliasTable&    table,
                                          Utf8StringRef              ownerClass)
{
    //替换映射（key = jasm 转义后的 token 形式，与 printer literal 同源）：
    //- class 条目全 jar 生效（.class/.super/描述符内的引用）；
    //- 成员条目仅 ownerClass 生效：裸名 token（.field/.method 声明行）与
    //`owner.name` 复合 token（invoke 操作数 / handle 字面量）。
    //field 与 method 在 JVM 里允许同名（混淆器会造）：raw 名相同时按上下文
    //区分——名字 token 后随 `(` 开头的描述符即方法位，否则字段位；仅登记了
    //一种 kind 时不区分上下文（声明行/操作数/同名局部注解一律同显）。
    std::unordered_map<std::string, std::string> classMap;
    std::unordered_map<std::string, std::string> methodMap;
    std::unordered_map<std::string, std::string> fieldMap;
    std::unordered_map<std::string, std::string> compositeMethodMap;
    std::unordered_map<std::string, std::string> compositeFieldMap;
    for (const auto& entry : table.entries()) {
        if (entry.kind == ShadowKind::Class) {
            classMap[jasmEscaped(entry.rawName)] = jasmEscaped(entry.shadowName);
            continue;
        }
        if (!entry.owner.equals(ownerClass)) continue;
        const auto rawKey     = jasmEscaped(entry.rawName);
        const auto shadowText = jasmEscaped(entry.shadowName);
        if (entry.kind == ShadowKind::Method) {
            methodMap[rawKey] = shadowText;
            compositeMethodMap[jasmEscaped(entry.owner) + "." + rawKey] = shadowText;
        }
        else {
            fieldMap[rawKey] = shadowText;
            compositeFieldMap[jasmEscaped(entry.owner) + "." + rawKey] = shadowText;
        }
    }
    if (classMap.empty() && methodMap.empty() && fieldMap.empty()) {
        return Utf8String::from_data(jasmText.data(), jasmText.byte_length());
    }

    const char*  text = reinterpret_cast<const char*>(jasmText.data());
    const auto   length = jasmText.byte_length();
    std::string  out;
    out.reserve(length + length / 8);

    bool        inQuote   = false;  //"..."
    bool        inLiteral = false;  //'...'
    bool        escaped   = false;  //引号内反斜杠转义
    std::string token;

    //名字 token 后（同行、跳过空格）首个字符为 `(` ⇒ 方法上下文。
    auto nextTokenIsDescriptor = [&](ca::usize delimiterIndex) {
        auto j = delimiterIndex;
        while (j < length && (text[j] == ' ' || text[j] == '\t')) ++j;
        return j < length && text[j] == '(';
    };
    //双 kind 同名时按上下文取映射；仅一种 kind 时不区分上下文。
    auto memberShadowFor = [&](const std::string& key, bool methodContext)
        -> const std::string* {
        const bool hasMethod = methodMap.count(key) > 0;
        const bool hasField  = fieldMap.count(key) > 0;
        if (hasMethod && hasField) {
            return methodContext ? &methodMap[key] : &fieldMap[key];
        }
        if (hasMethod) return &methodMap[key];
        if (hasField) return &fieldMap[key];
        return nullptr;
    };
    auto compositeShadowFor = [&](const std::string& key, bool methodContext)
        -> const std::string* {
        const bool hasMethod = compositeMethodMap.count(key) > 0;
        const bool hasField  = compositeFieldMap.count(key) > 0;
        if (hasMethod && hasField) {
            return methodContext ? &compositeMethodMap[key] : &compositeFieldMap[key];
        }
        if (hasMethod) return &compositeMethodMap[key];
        if (hasField) return &compositeFieldMap[key];
        return nullptr;
    };

    auto flushToken = [&](ca::usize delimiterIndex) {
        if (token.empty()) return;
        auto replace = [&](const std::string& with) {
            out += with;
            token.clear();
        };
        const bool methodContext = nextTokenIsDescriptor(delimiterIndex);
        if (auto* shadow = compositeShadowFor(token, methodContext)) {
            replace(*shadow);
            return;
        }
        if (auto* shadow = memberShadowFor(token, methodContext)) {
            replace(*shadow);
            return;
        }
        if (auto it = classMap.find(token); it != classMap.end()) {
            replace(it->second);
            return;
        }
        //描述符对象类型段 `L<name>;`（[ 与 ; 是分隔符，token 即 L<name>）：
        //剥 L 前缀连带替换，未命中原样。
        if (token.size() > 1 && token[0] == 'L') {
            if (auto it = classMap.find(token.substr(1)); it != classMap.end()) {
                replace("L" + it->second);
                return;
            }
        }
        replace(token);
    };

    for (ca::usize i = 0; i < length; ++i) {
        const char c = text[i];
        if (inQuote || inLiteral) {
            out += c;
            if (escaped) {
                escaped = false;
            }
            else if (c == '\\') {
                escaped = true;
            }
            else if ((inQuote && c == '"') || (inLiteral && c == '\'')) {
                inQuote = inLiteral = false;
            }
            continue;
        }
        if (isJasmDelimiter(c)) {
            flushToken(i);
            out += c;
            if (c == '"') inQuote = true;
            else if (c == '\'') inLiteral = true;
            continue;
        }
        token += c;
    }
    flushToken(length);
    return toUtf8(out);
}

}   //namespace mjt::base
