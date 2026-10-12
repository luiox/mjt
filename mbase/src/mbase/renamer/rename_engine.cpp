#include "mbase/renamer/rename_engine.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "mbase/asm/remapper/resource_remapper.hpp"
#include "mbase/mapping/identifier_util.hpp"
#include "mbase/model/resource_model.hpp"

namespace mjt::base {
namespace {

//── 管线内部字符串工作表示：std::string 键（对齐 workspace.cpp 内部管线惯例），
//模块边界（plan/record/proposal）一律 Utf8String/Utf8StringRef。 ──

std::string toStd(const ca::str::Utf8StringRef value)
{
    return value.to_std_string();
}

ca::str::Utf8String toUtf8(const std::string& value)
{
    return ca::str::Utf8String::from_cstr(value.c_str());
}

///plan/record mutator 入参视图：mutator 同步 intern，全表达式内有效即可
///（workspace.cpp buildRenamePlan 的 ref lambda 同款纪律）。
ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

bool isJavaKeyword(ca::str::Utf8StringRef name)
{
    for (const auto& keyword : IdentifierUtil::javaKeyWords()) {
        if (keyword.equals(name)) return true;
    }
    return false;
}

bool isReservedMethodName(ca::str::Utf8StringRef name)
{
    return name.equals("<init>") || name.equals("<clinit>");
}

///内部名路径合法性（类名/包路径）：按 '/' 分段，每段须为合法标识符且非关键字。
///段合法性复用 IdentifierUtil::isValidJavaIdentifier（口径与 mbase 既有映射线一致）。
enum class NamePathVerdict
{
    Ok,
    Illegal,
    Keyword,
};

NamePathVerdict checkNamePath(ca::str::Utf8StringRef path)
{
    if (path.is_empty()) return NamePathVerdict::Illegal;
    for (const auto& segment : path.split(ca::str::Utf8StringRef::from_cstr("/"))) {
        if (segment.is_empty()) return NamePathVerdict::Illegal;
        if (!IdentifierUtil::isValidJavaIdentifier(segment)) return NamePathVerdict::Illegal;
        if (isJavaKeyword(segment)) return NamePathVerdict::Keyword;
    }
    return NamePathVerdict::Ok;
}

std::string trimTrailingSlash(std::string path)
{
    while (!path.empty() && path.back() == '/') path.pop_back();
    return path;
}

///owner 内的既有成员占用表（撞名检测用，值 = (name, desc)；字段槽忽略 desc）。
///方法 desc 以 plan 类映射预映像（mapped-desc 空间，luiox/）：类改名
///会同步改写方法描述符中的类类型，最终类文件状态在 mapped-desc 空间，方法槽查撞
///必须与之同空间。调用时机：类映射已全部入 plan 之后。
struct OwnerSlots
{
    std::set<std::string>                         fieldNames;
    std::set<std::pair<std::string, std::string>> methodSlots;
};

std::unordered_map<std::string, OwnerSlots> collectOwnerSlots(const ClassHierarchy&   hierarchy,
                                                              const MemberRemapPlan& plan)
{
    std::unordered_map<std::string, OwnerSlots> slots;
    for (ca::usize i = 0; i < hierarchy.classCount(); ++i) {
        const auto& entry = hierarchy.entry(static_cast<ClassIndex>(i));
        if (entry.classNode == nullptr) continue;   //phantom 不占名
        auto& owner = slots[toStd(entry.name)];
        for (const auto* field : entry.classNode->fields)
            owner.fieldNames.insert(toStd(field->name));
        for (const auto* method : entry.classNode->methods) {
            owner.methodSlots.emplace(
                toStd(method->name), toStd(plan.mapMethodDescriptor(method->desc)));
        }
    }
    return slots;
}

///proposal 管线条目：指向 run 内存活的建议向量。
struct PipelineClassChange
{
    ca::usize   proposalIdx = 0;
    std::string oldName;
    std::string newName;
    bool        fromPackage = false;
};

struct PipelineFieldChange
{
    ca::usize   proposalIdx = 0;
    std::string owner;
    std::string oldName;
    std::string desc;
    std::string newName;
};

struct PipelineMethodChange
{
    ca::usize   proposalIdx = 0;
    std::string owner;
    std::string oldName;
    std::string desc;
    std::string newName;
    bool        fromClosure = false;
    bool        dropped     = false;   ///< 所属闭包组整组被拒时置位
};

struct MethodGroup
{
    std::vector<ca::usize> memberIndices;   //→ methodChanges 下标
};

///同 target 裁决键：class/package = 新名全局槽；field = owner+新名（名字槽）；
///method = owner+新名+desc（重载是合法的不同槽）。
struct TargetKey
{
    ProposalKind kind = ProposalKind::Class;
    std::string  owner;
    std::string  name;
    std::string  desc;

    bool operator<(const TargetKey& other) const
    {
        if (kind != other.kind) return kind < other.kind;
        if (owner != other.owner) return owner < other.owner;
        if (name != other.name) return name < other.name;
        return desc < other.desc;
    }
};

TargetKey targetKeyOf(const NameProposal& proposal)
{
    switch (proposal.kind) {
    case ProposalKind::Class:
    case ProposalKind::Package: return {proposal.kind, {}, toStd(proposal.newName), {}};
    case ProposalKind::Field:
        return {proposal.kind, toStd(proposal.owner), toStd(proposal.newName), {}};
    case ProposalKind::Method:
        return {
            proposal.kind, toStd(proposal.owner), toStd(proposal.newName), toStd(proposal.desc)};
    }
    return {proposal.kind, {}, toStd(proposal.newName), {}};
}

///同 old slot 键：同一成员/类只允许一个终名——两条幸存建议改同一位 → 裁决。
TargetKey oldSlotKeyOf(const NameProposal& proposal)
{
    switch (proposal.kind) {
    case ProposalKind::Class:
    case ProposalKind::Package: return {proposal.kind, {}, toStd(proposal.oldName), {}};
    case ProposalKind::Field:
    case ProposalKind::Method:
        return {
            proposal.kind, toStd(proposal.owner), toStd(proposal.oldName), toStd(proposal.desc)};
    }
    return {proposal.kind, {}, toStd(proposal.oldName), {}};
}

///owner 类内是否存在精确 (name, desc) 方法声明。
bool classDeclaresMethod(const jbytecode::tree::ClassNode& classNode, const std::string& name,
                         const std::string& desc)
{
    const auto nameRef = refOf(name);
    const auto descRef = refOf(desc);
    for (const auto* method : classNode.methods) {
        if (method != nullptr && method->name.equals(nameRef) && method->desc.equals(descRef)) {
            return true;
        }
    }
    return false;
}

///owner 类内是否存在精确 (name, desc) 字段声明。
bool classDeclaresField(const jbytecode::tree::ClassNode& classNode, const std::string& name,
                        const std::string& desc)
{
    const auto nameRef = refOf(name);
    const auto descRef = refOf(desc);
    for (const auto* field : classNode.fields) {
        if (field != nullptr && field->name.equals(nameRef) && field->desc.equals(descRef)) {
            return true;
        }
    }
    return false;
}

///沿 super 链上溯，返回声明 (name, desc) 字段的最近类；无处声明返回 INVALID_CLASS。
ClassIndex resolveInstanceFieldOwnerIndex(const ClassHierarchy& hierarchy, ClassIndex start,
                                          const std::string& name, const std::string& desc)
{
    ClassIndex current = start;
    while (current != INVALID_CLASS) {
        const auto& entry = hierarchy.entry(current);
        if (entry.classNode != nullptr && classDeclaresField(*entry.classNode, name, desc)) {
            return current;
        }
        current = hierarchy.superClass(current);
    }
    return INVALID_CLASS;
}

///闭世界判定（obf rename_core hierarchyIsClosedForVirtualRename 同口径）：
///自 start 沿 super + interfaces 闭包可达的全部类名都在容器中有真实类节点
///（java/lang/Object 豁免）。false = 虚分派闭包延伸到容器外，继承引用不可
///安全补别名。
bool hierarchyClosedForVirtualRename(const ClassHierarchy& hierarchy, ClassIndex start)
{
    std::vector<ClassIndex>      pending{start};
    std::unordered_set<ClassIndex> visited;

    while (!pending.empty()) {
        const ClassIndex index = pending.back();
        pending.pop_back();
        if (!visited.insert(index).second) continue;

        const auto& entry = hierarchy.entry(index);
        if (entry.classNode == nullptr) {
            if (entry.name.equals(refOf("java/lang/Object"))) continue;
            return false;
        }

        const auto enqueue = [&](ca::str::Utf8StringRef name) {
            if (name.is_empty()) return true;
            const ClassIndex dependency = hierarchy.indexOf(name);
            if (dependency == INVALID_CLASS) return name.equals(refOf("java/lang/Object"));
            pending.push_back(dependency);
            return true;
        };

        if (!enqueue(entry.classNode->superName)) return false;
        for (const auto& interfaceName : entry.classNode->interfaces) {
            if (!enqueue(interfaceName)) return false;
        }
    }
    return true;
}

///proposal 的槽位定位串（撞名报错面）：kind + owner + 原位 + 终名，让拒绝流水
///的 detail 自含「谁撞了谁」两侧坐标——只报 source/confidence 时无法定位撞在哪。
std::string proposalSlotRepr(const NameProposal& proposal)
{
    const char* kind = proposal.kind == ProposalKind::Class    ? "class"
                       : proposal.kind == ProposalKind::Package ? "package"
                       : proposal.kind == ProposalKind::Field   ? "field"
                                                                 : "method";
    if (proposal.kind == ProposalKind::Class || proposal.kind == ProposalKind::Package) {
        return std::string(kind) + " " + toStd(proposal.oldName) + " -> " +
               toStd(proposal.newName);
    }
    return std::string(kind) + " " + toStd(proposal.owner) + "." + toStd(proposal.oldName) +
           toStd(proposal.desc) + " -> " + toStd(proposal.newName);
}

///proposal 的来源定位串：source + confidence + 槽位坐标。
std::string proposalOriginRepr(const NameProposal& proposal)
{
    return "source '" + toStd(proposal.source) + "' confidence " +
           std::to_string(proposal.confidence) + ": " + proposalSlotRepr(proposal);
}

///裁决结果：key 首次认领 / 在位者保位（挑战者出局）/ 挑战者夺位（在位者出局）。
enum class Adjudication
{
    Claimed,
    IncumbentKept,
    ChallengerWon,
};

///同 target 裁决：高位 confidence 者胜；同 confidence 先到者胜（后到拒）。
///出局者下标经 `loserIdx` 返回，由调用方（RenamerEngine 友元域内）写流水。
///`slotLabel` 区分新名槽/旧位槽（detail 自含两侧坐标：撞在哪个槽、双方各是谁）。
Adjudication adjudicate(std::map<TargetKey, ca::usize>& winnerOfKey,
                        std::vector<ca::usize>& winners, const std::vector<NameProposal>& proposals,
                        const TargetKey& key, ca::usize index, ca::usize& loserIdx)
{
    auto it = winnerOfKey.find(key);
    if (it == winnerOfKey.end()) {
        winnerOfKey.emplace(key, index);
        winners.push_back(index);
        return Adjudication::Claimed;
    }
    const double incumbentConfidence  = proposals[it->second].confidence;
    const double challengerConfidence = proposals[index].confidence;
    if (challengerConfidence > incumbentConfidence) {
        loserIdx   = it->second;
        it->second = index;
        std::replace(winners.begin(), winners.end(), loserIdx, index);
        return Adjudication::ChallengerWon;
    }
    loserIdx = index;
    return Adjudication::IncumbentKept;
}

}   //namespace

//─────────────────────────── RenameContext ───────────────────────────

ca::StatusOr<std::unique_ptr<RenameContext>> RenameContext::build(ResourceContainer& container)
{
    //堆上构造：ClassHierarchy 借用本对象 arena_ 的地址（indexOf 查询按需
    //intern），构造后对象地址不得再变——unique_ptr 转移不移动对象本身。
    std::unique_ptr<RenameContext> context(new RenameContext());
    try {
        context->hierarchy_.build(container, context->arena_);
        context->methods_.build(context->hierarchy_);
    }
    catch (const std::exception& exception) {
        return ca::Err(ca::Status::error(ca::StatusCode::INTERNAL,
                                         std::string("build rename context: ") + exception.what()));
    }
    return ca::Ok(std::move(context));
}

bool RenameContext::classExists(ca::str::Utf8StringRef internalName) const
{
    const auto index = hierarchy_.indexOf(internalName);
    return index != INVALID_CLASS && hierarchy_.entry(index).classNode != nullptr;
}

bool RenameContext::isSubType(ca::str::Utf8StringRef sub, ca::str::Utf8StringRef super) const
{
    const auto subIndex   = hierarchy_.indexOf(sub);
    const auto superIndex = hierarchy_.indexOf(super);
    if (subIndex == INVALID_CLASS || superIndex == INVALID_CLASS) return false;
    return hierarchy_.isSubType(subIndex, superIndex);
}

ca::str::Utf8StringRef RenameContext::superClassOf(ca::str::Utf8StringRef internalName) const
{
    const auto index = hierarchy_.indexOf(internalName);
    if (index == INVALID_CLASS) return ca::str::Utf8StringRef();
    const auto superIndex = hierarchy_.superClass(index);
    if (superIndex == INVALID_CLASS) return ca::str::Utf8StringRef();
    return hierarchy_.nameOf(superIndex);
}

bool RenameContext::methodExists(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                 ca::str::Utf8StringRef desc) const
{
    const auto ownerIndex = hierarchy_.indexOf(owner);
    if (ownerIndex == INVALID_CLASS || hierarchy_.entry(ownerIndex).classNode == nullptr) {
        return false;
    }
    for (const auto* method : hierarchy_.entry(ownerIndex).classNode->methods) {
        if (method->name.equals(name) && method->desc.equals(desc)) return true;
    }
    return false;
}

ca::StatusOr<ca::i32> RenameContext::overrideComponentRootOf(ca::str::Utf8StringRef owner,
                                                             ca::str::Utf8StringRef name,
                                                             ca::str::Utf8StringRef desc) const
{
    const auto ownerIndex = hierarchy_.indexOf(owner);
    if (ownerIndex == INVALID_CLASS || hierarchy_.entry(ownerIndex).classNode == nullptr) {
        return ca::Err(
            ca::Status::error(ca::StatusCode::NOT_FOUND,
                              "override component root: owner not in container: " + toStd(owner)));
    }
    for (ca::i32 index = 0; index < methods_.methodCount(); ++index) {
        const auto& entry = methods_.entry(index);
        if (entry.owner != ownerIndex) continue;
        const auto* method = hierarchy_.entry(ownerIndex).classNode->methods[entry.methodIndex];
        if (method->name.equals(name) && method->desc.equals(desc)) {
            return ca::Ok(methods_.findComponentRoot(index));
        }
    }
    return ca::Err(ca::Status::error(ca::StatusCode::NOT_FOUND,
                                     "override component root: method not declared: " +
                                         toStd(owner) + "." + toStd(name) + toStd(desc)));
}

ca::StatusOr<std::vector<OverrideComponentMember>> RenameContext::overrideComponentOf(
    ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name, ca::str::Utf8StringRef desc) const
{
    const auto ownerIndex = hierarchy_.indexOf(owner);
    if (ownerIndex == INVALID_CLASS || hierarchy_.entry(ownerIndex).classNode == nullptr) {
        return ca::Err(
            ca::Status::error(ca::StatusCode::NOT_FOUND,
                              "override component: owner not in container: " + toStd(owner)));
    }

    ca::i32 selected = -1;
    for (ca::i32 index = 0; index < methods_.methodCount(); ++index) {
        const auto& entry = methods_.entry(index);
        if (entry.owner != ownerIndex) continue;
        const auto* method = hierarchy_.entry(ownerIndex).classNode->methods[entry.methodIndex];
        if (method->name.equals(name) && method->desc.equals(desc)) {
            selected = index;
            break;
        }
    }
    if (selected < 0) {
        return ca::Err(ca::Status::error(ca::StatusCode::NOT_FOUND,
                                         "override component: method not declared: " +
                                             toStd(owner) + "." + toStd(name) + toStd(desc)));
    }

    const auto                           root = methods_.findComponentRoot(selected);
    std::vector<OverrideComponentMember> members;
    for (const auto index : methods_.componentMethods(root)) {
        const auto& entry  = methods_.entry(index);
        const auto& clazz  = hierarchy_.entry(entry.owner);
        const auto* method = clazz.classNode->methods[entry.methodIndex];
        members.push_back(
            {toUtf8(toStd(clazz.name)), toUtf8(toStd(method->name)), toUtf8(toStd(method->desc))});
    }
    return ca::Ok(std::move(members));
}

//─────────────────────────── NameChangeRecord ───────────────────────────

ca::str::Utf8StringRef rejectionReasonName(RejectionReason reason)
{
    switch (reason) {
    case RejectionReason::None: return ca::str::Utf8StringRef::from_cstr("none");
    case RejectionReason::Schema: return ca::str::Utf8StringRef::from_cstr("schema");
    case RejectionReason::IllegalIdentifier:
        return ca::str::Utf8StringRef::from_cstr("illegal_identifier");
    case RejectionReason::Keyword: return ca::str::Utf8StringRef::from_cstr("keyword");
    case RejectionReason::ReservedName: return ca::str::Utf8StringRef::from_cstr("reserved_name");
    case RejectionReason::NotFound: return ca::str::Utf8StringRef::from_cstr("not_found");
    case RejectionReason::Conflict: return ca::str::Utf8StringRef::from_cstr("conflict");
    case RejectionReason::ProviderError: return ca::str::Utf8StringRef::from_cstr("provider_error");
    case RejectionReason::ApplyFailed: return ca::str::Utf8StringRef::from_cstr("apply_failed");
    }
    return ca::str::Utf8StringRef::from_cstr("unknown");
}

void NameChangeRecord::addAccepted(AcceptedChange change)
{
    accepted_.push_back(std::move(change));
}

void NameChangeRecord::addRejected(NameProposal proposal, RejectionReason reason,
                                   ca::str::Utf8String detail)
{
    rejected_.push_back({std::move(proposal), reason, std::move(detail)});
}

ca::StatusOr<ca::str::Utf8String> NameChangeRecord::exportMapping(
    const IntermediateMapping& mapping, ca::str::Utf8StringRef formatName,
    const MappingFormatRegistry& registry)
{
    if (!registry.hasFormat(formatName)) {
        return ca::Err(ca::Status::error(ca::StatusCode::INVALID_ARGUMENT,
                                         "unknown mapping format: " + toStd(formatName)));
    }
    return ca::Ok(registry.format(formatName, mapping));
}

ca::StatusOr<ca::str::Utf8String> NameChangeRecord::exportMapping(
    ca::str::Utf8StringRef formatName, const MappingFormatRegistry& registry) const
{
    return exportMapping(mapping_, formatName, registry);
}

//─────────────────────────── RenamerEngine ───────────────────────────

RenamerRunResult RenamerEngine::run(ResourceContainer&                container,
                                    const std::vector<NameProvider*>& providers,
                                    const RenamerOptions&             options)
{
    RenamerRunResult result;

    //── 0. 只读视图：类容器 + 类/方法层级（一次构建，全程只读）──
    auto contextResult = RenameContext::build(container);
    if (contextResult.is_err()) {
        result.status = std::move(contextResult).unwrap_err();
        return result;
    }
    std::unique_ptr<RenameContext> context = std::move(contextResult).unwrap();

    //── 1. 收集各 provider 建议流 ──
    std::vector<NameProposal> proposals;
    for (auto* provider : providers) {
        if (provider == nullptr) continue;
        auto stream = provider->propose(*context);
        if (stream.is_err()) {
            const auto failure = stream.unwrap_err();
            ++result.stats.rejected;
            result.record.addRejected(NameProposal{},
                                      RejectionReason::ProviderError,
                                      toUtf8("provider '" + provider->name().to_std_string() +
                                             "' propose failed: " + failure.to_string()));
            continue;
        }
        auto batch = std::move(stream).unwrap();
        for (auto& proposal : batch) {
            if (proposal.source.is_empty()) proposal.source = provider->name().clone();
            proposals.push_back(std::move(proposal));
        }
    }
    result.stats.proposed = proposals.size();

    //── 2. 逐条 schema + 合法性 + 存在性过滤（fail-closed：逐条拒，不中断）──
    std::vector<ca::usize> validated;
    for (ca::usize i = 0; i < proposals.size(); ++i) {
        const auto&                                            proposal = proposals[i];
        std::optional<std::pair<RejectionReason, std::string>> verdict;

        const auto reject = [&verdict](RejectionReason reason, std::string detail) {
            verdict = {reason, std::move(detail)};
        };
        const auto requireOwnerShape = [&]() {
            if (!proposal.owner.is_empty())
                reject(RejectionReason::Schema, "class/package proposal must not set owner");
            else if (!proposal.desc.is_empty())
                reject(RejectionReason::Schema, "class/package proposal must not set desc");
            else if (proposal.oldName.is_empty())
                reject(RejectionReason::Schema, "empty old name");
            else if (proposal.oldName.equals(proposal.newName))
                reject(RejectionReason::Schema, "no-op rename");
        };

        if (!(proposal.confidence >= 0.0 && proposal.confidence <= 1.0)) {
            reject(RejectionReason::Schema,
                   "confidence out of [0,1]: " + std::to_string(proposal.confidence));
        }
        else if (proposal.newName.is_empty()) {
            reject(RejectionReason::Schema, "empty new name");
        }
        else if (proposal.kind == ProposalKind::Class) {
            requireOwnerShape();
            if (!verdict && !context->classExists(proposal.oldName)) {
                reject(RejectionReason::NotFound,
                       "class not in container: " + toStd(proposal.oldName));
            }
        }
        else if (proposal.kind == ProposalKind::Package) {
            requireOwnerShape();
            if (!verdict) {
                const auto oldPrefix = trimTrailingSlash(toStd(proposal.oldName));
                const auto newPrefix = trimTrailingSlash(toStd(proposal.newName));
                if (oldPrefix.empty() || newPrefix.empty()) {
                    reject(RejectionReason::Schema, "empty package path");
                }
                else if (oldPrefix == newPrefix) {
                    reject(RejectionReason::Schema, "no-op rename");
                }
                else {
                    //包前缀下至少要有一个容器内真实类
                    bool        hasClass  = false;
                    const auto& hierarchy = context->hierarchy_;
                    for (ca::usize c = 0; c < hierarchy.classCount() && !hasClass; ++c) {
                        const auto& entry = hierarchy.entry(static_cast<ClassIndex>(c));
                        if (entry.classNode == nullptr) continue;
                        const auto name = toStd(entry.name);
                        hasClass        = name == oldPrefix || name.rfind(oldPrefix + "/", 0) == 0;
                    }
                    if (!hasClass) {
                        reject(RejectionReason::NotFound,
                               "no container class under package prefix: " + oldPrefix);
                    }
                }
            }
        }
        else if (proposal.kind == ProposalKind::Field) {
            if (proposal.owner.is_empty() || proposal.oldName.is_empty() ||
                proposal.desc.is_empty()) {
                reject(RejectionReason::Schema, "field proposal requires owner, old name and desc");
            }
            else if (proposal.oldName.equals(proposal.newName)) {
                reject(RejectionReason::Schema, "no-op rename");
            }
            else if (!context->classExists(proposal.owner)) {
                reject(RejectionReason::NotFound,
                       "owner class not in container: " + toStd(proposal.owner));
            }
        }
        else if (proposal.kind == ProposalKind::Method) {
            if (proposal.owner.is_empty() || proposal.oldName.is_empty() ||
                proposal.desc.is_empty()) {
                reject(RejectionReason::Schema,
                       "method proposal requires owner, old name and desc");
            }
            else if (isReservedMethodName(proposal.oldName)) {
                reject(RejectionReason::ReservedName,
                       toStd(proposal.oldName) + " is exempt from renaming");
            }
            else if (isReservedMethodName(proposal.newName)) {
                reject(RejectionReason::ReservedName,
                       toStd(proposal.newName) + " cannot be a new name");
            }
            else if (proposal.oldName.equals(proposal.newName)) {
                reject(RejectionReason::Schema, "no-op rename");
            }
            else if (!context->classExists(proposal.owner)) {
                reject(RejectionReason::NotFound,
                       "owner class not in container: " + toStd(proposal.owner));
            }
            else if (!context->methodExists(proposal.owner, proposal.oldName, proposal.desc)) {
                reject(RejectionReason::NotFound,
                       "method not declared in owner: " + toStd(proposal.owner) + "." +
                           toStd(proposal.oldName) + toStd(proposal.desc));
            }
        }

        //新名合法性（与 kind 相关的口径）
        if (!verdict) {
            if (proposal.kind == ProposalKind::Class || proposal.kind == ProposalKind::Package) {
                switch (checkNamePath(proposal.newName)) {
                case NamePathVerdict::Illegal:
                    reject(
                        RejectionReason::IllegalIdentifier,
                        "new name is not a legal internal name path: " + toStd(proposal.newName));
                    break;
                case NamePathVerdict::Keyword:
                    reject(RejectionReason::Keyword,
                           "new name segment is a java keyword: " + toStd(proposal.newName));
                    break;
                case NamePathVerdict::Ok: break;
                }
            }
            else {
                if (!IdentifierUtil::isValidJavaIdentifier(proposal.newName)) {
                    reject(RejectionReason::IllegalIdentifier,
                           "new name is not a legal java identifier: " + toStd(proposal.newName));
                }
                else if (isJavaKeyword(proposal.newName)) {
                    reject(RejectionReason::Keyword,
                           "new name is a java keyword: " + toStd(proposal.newName));
                }
            }
        }

        if (verdict) {
            ++result.stats.rejected;
            result.record.addRejected(
                std::move(proposals[i]), verdict->first, toUtf8(verdict->second));
            continue;
        }
        validated.push_back(i);
    }

    //── 3. 同 target confidence 裁决（高者胜；同 confidence 先到先得，后到拒）──
    //detail 自含两侧坐标（撞在哪个槽、双方 owner/old/new）——只报计数/来源时
    //排查者无法定位撞名对象。
    const auto recordAdjudication =
        [&](Adjudication verdict, ca::usize loserIdx, ca::usize winnerIdx, const char* slotLabel) {
            if (verdict == Adjudication::Claimed) return;
            ++result.stats.rejected;
            const auto& winner     = proposals[winnerIdx];
            const auto& loser      = proposals[loserIdx];
            const auto  winnerRepr = proposalOriginRepr(winner);
            result.record.addRejected(
                std::move(proposals[loserIdx]),
                RejectionReason::Conflict,
                toUtf8(verdict == Adjudication::ChallengerWon
                           ? std::string(slotLabel) + " superseded by higher confidence proposal (" +
                                 winnerRepr + "); rejected: " + proposalSlotRepr(loser)
                           : std::string(slotLabel) + " already claimed by arrival-earlier proposal (" +
                                 winnerRepr + "); rejected: " + proposalSlotRepr(loser)));
        };

    //3a. 新名槽：同终名同槽的候选二选一。
    std::map<TargetKey, ca::usize> winnerOfKey;
    std::vector<ca::usize>         winners;
    for (const auto index : validated) {
        const auto key     = targetKeyOf(proposals[index]);
        ca::usize  loser   = index;
        const auto verdict = adjudicate(winnerOfKey, winners, proposals, key, index, loser);
        recordAdjudication(verdict, loser, winnerOfKey.at(key), "target");
    }
    //3b. 旧位槽：同一成员/类被两条幸存建议改到不同新名 → 再裁决一次。
    {
        std::map<TargetKey, ca::usize> winnerOfOldSlot;
        std::vector<ca::usize>         survivors;
        for (const auto index : winners) {
            const auto key   = oldSlotKeyOf(proposals[index]);
            ca::usize  loser = index;
            const auto verdict =
                adjudicate(winnerOfOldSlot, survivors, proposals, key, index, loser);
            recordAdjudication(verdict, loser, winnerOfOldSlot.at(key), "old slot");
        }
        winners = std::move(survivors);   //到达序保持
    }
    std::sort(winners.begin(), winners.end());

    //4-pre. 同一 override 组件被多条建议分别点名（如 interface.m→foo 与 impl.m→bar）
    //时 3a/3b 均挡不住（旧位槽与新名槽各不相同）→ 展开前按组件根去重：
    //同组件多建议走 confidence 裁决（高者胜；同分先到），败者记 Conflict 留 ledger。
    //去重后各组件至多一条建议 → 6c 的旧槽占用恰好各让位/回插一次。
    {
        std::map<ca::i32, ca::usize> componentWinnerOf;
        std::set<ca::usize>          componentLosers;
        for (const auto index : winners) {
            const auto& proposal = proposals[index];
            if (proposal.kind != ProposalKind::Method) continue;

            auto root = context->overrideComponentRootOf(
                proposal.owner.ref(), proposal.oldName.ref(), proposal.desc.ref());
            if (root.is_err()) continue;   //防御：step 2 已验存在性，不可达则交单例降级
            const auto key       = root.unwrap();
            const auto incumbent = componentWinnerOf.find(key);
            if (incumbent == componentWinnerOf.end()) {
                componentWinnerOf.emplace(key, index);
                continue;
            }
            const bool challengerWins =
                proposal.confidence > proposals[incumbent->second].confidence;
            const auto loserIdx  = challengerWins ? incumbent->second : index;
            const auto winnerIdx = challengerWins ? index : incumbent->second;
            ++result.stats.rejected;
            const auto& winner = proposals[winnerIdx];
            //两侧建议指向同一 override 组件，组件定位任取其一（胜者坐标）。
            const auto componentRepr = "override component of " + proposalSlotRepr(winner);
            result.record.addRejected(
                std::move(proposals[loserIdx]),
                RejectionReason::Conflict,
                toUtf8(challengerWins
                           ? componentRepr + " superseded by higher confidence proposal (" +
                                 proposalOriginRepr(winner) + "); rejected: " +
                                 proposalSlotRepr(proposals[loserIdx])
                           : componentRepr + " already claimed by arrival-earlier proposal (" +
                                 proposalOriginRepr(winner) + "); rejected: " +
                                 proposalSlotRepr(proposals[loserIdx])));
            componentLosers.insert(loserIdx);
            if (challengerWins) incumbent->second = index;
        }
        if (!componentLosers.empty()) {
            std::vector<ca::usize> kept;
            kept.reserve(winners.size());
            for (const auto index : winners) {
                if (componentLosers.count(index) == 0) kept.push_back(index);
            }
            winners = std::move(kept);
        }
    }

    //── 4. method override 闭包整组同改（workspace.cpp buildRenamePlan 同款现法）──
    std::vector<PipelineMethodChange> methodChanges;
    std::vector<MethodGroup>          methodGroups;
    for (const auto index : winners) {
        const auto& proposal = proposals[index];
        if (proposal.kind != ProposalKind::Method) continue;

        std::vector<OverrideComponentMember> members;
        auto                                 component = context->overrideComponentOf(
            proposal.owner.ref(), proposal.oldName.ref(), proposal.desc.ref());
        if (component.is_err()) {
            //不在容器/不在方法层级（罕见：owner 校验已过，图构建缺类）→ 单例组降级
            members.push_back(
                {proposal.owner.clone(), proposal.oldName.clone(), proposal.desc.clone()});
        }
        else {
            members = std::move(component).unwrap();
        }

        MethodGroup group;
        bool        primary = true;
        for (const auto& member : members) {
            if (member.name.equals(proposal.newName)) continue;   //已是目标名：跳过该成员
            PipelineMethodChange change{index,
                                        toStd(member.owner),
                                        toStd(member.name),
                                        toStd(member.desc),
                                        toStd(proposal.newName),
                                        !primary,
                                        false};
            group.memberIndices.push_back(methodChanges.size());
            methodChanges.push_back(std::move(change));
            primary = false;
        }
        methodGroups.push_back(std::move(group));
    }

    //── 5. class/field 赢家落管线；package 前缀展开（到达序，all-or-reject）──
    std::vector<PipelineClassChange>      classChanges;
    std::vector<PipelineFieldChange>      fieldChanges;
    std::unordered_map<std::string, bool> claimedOldClass;
    std::unordered_map<std::string, bool> claimedTargetClass;
    for (const auto index : winners) {
        const auto& proposal = proposals[index];
        if (proposal.kind == ProposalKind::Class) {
            classChanges.push_back(
                {index, toStd(proposal.oldName), toStd(proposal.newName), false});
            claimedOldClass[toStd(proposal.oldName)]    = true;
            claimedTargetClass[toStd(proposal.newName)] = true;
        }
        else if (proposal.kind == ProposalKind::Field) {
            fieldChanges.push_back({index,
                                    toStd(proposal.owner),
                                    toStd(proposal.oldName),
                                    toStd(proposal.desc),
                                    toStd(proposal.newName)});
        }
    }
    for (const auto index : winners) {
        const auto& proposal = proposals[index];
        if (proposal.kind != ProposalKind::Package) continue;

        const auto oldPrefix = trimTrailingSlash(toStd(proposal.oldName));
        const auto newPrefix = trimTrailingSlash(toStd(proposal.newName));
        std::vector<std::pair<std::string, std::string>> expansion;
        std::string                                      clashedClass;
        bool                                             clash     = false;
        const auto&                                      hierarchy = context->hierarchy_;
        for (ca::usize c = 0; c < hierarchy.classCount(); ++c) {
            const auto& entry = hierarchy.entry(static_cast<ClassIndex>(c));
            if (entry.classNode == nullptr) continue;
            const auto name = toStd(entry.name);
            if (name != oldPrefix && name.rfind(oldPrefix + "/", 0) != 0) continue;
            const auto mapped = newPrefix + name.substr(oldPrefix.size());
            if (claimedOldClass.count(name) != 0 || claimedTargetClass.count(mapped) != 0) {
                clashedClass = name + " -> " + mapped;
                clash        = true;
                break;
            }
            expansion.emplace_back(name, mapped);
        }
        if (expansion.empty() || clash) {
            ++result.stats.rejected;
            result.record.addRejected(
                std::move(proposals[index]),
                clash ? RejectionReason::Conflict : RejectionReason::NotFound,
                toUtf8(clash ? "package expansion collides with claimed class rename: " +
                                   clashedClass
                             : "no container class under package prefix: " + oldPrefix));
            continue;
        }
        for (const auto& [oldName, mapped] : expansion) {
            classChanges.push_back({index, oldName, mapped, true});
            claimedOldClass[oldName]   = true;
            claimedTargetClass[mapped] = true;
        }
    }

    //── 6. 残余撞名 `_N` 后缀消解（共享一张既有成员占用表；被改名旧位让位）──

    //6a. 类名空间：被改名旧名让位，接受侧逐一占用；撞名 `_1`、`_2`… 递增。
    {
        std::unordered_set<std::string> renamedOlds;
        for (const auto& change : classChanges) renamedOlds.insert(change.oldName);
        std::unordered_set<std::string> occupied;
        const auto&                     hierarchy = context->hierarchy_;
        for (ca::usize c = 0; c < hierarchy.classCount(); ++c) {
            const auto& entry = hierarchy.entry(static_cast<ClassIndex>(c));
            if (entry.classNode == nullptr) continue;
            const auto name = toStd(entry.name);
            if (renamedOlds.count(name) == 0) occupied.insert(name);
        }
        for (auto& change : classChanges) {
            auto      candidate = change.newName;
            ca::usize suffix    = 1;
            while (occupied.count(candidate) != 0) {
                candidate = change.newName + "_" + std::to_string(suffix++);
            }
            if (candidate != change.newName) ++result.stats.suffixResolved;
            change.newName = std::move(candidate);
            occupied.insert(change.newName);
        }
    }

    //6-pre. 类映射先行入 plan（luiox/数据流调整）：6c 的方法槽查撞
    //在 mapped-desc 空间进行（类改名同步改写方法描述符中的类类型，最终类
    //文件状态在 mapped-desc 空间），desc 预映像依赖 plan 中的类映射——与
    //step 7 成员规则 newOwner/newDesc 预映像同机制。时序安全：类终名在
    //6a 后已定（后缀消解完成），classChanges 自 step 5 起不再变化，6b/6c
    //不触类名空间；step 7 的 record 落账与导出口径不受影响。
    for (const auto& change : classChanges) {
        result.plan.mapClass(refOf(change.oldName), refOf(change.newName));
    }
    auto occupancy = collectOwnerSlots(context->hierarchy_, result.plan);

    //6b. 字段名空间（名字槽，同 owner 内唯一；纯名无 desc，不涉查撞空间切换）。
    {
        for (auto& change : fieldChanges) occupancy[change.owner].fieldNames.erase(change.oldName);
        for (auto& change : fieldChanges) {
            auto&     occupied  = occupancy[change.owner].fieldNames;
            auto      candidate = change.newName;
            ca::usize suffix    = 1;
            while (occupied.count(candidate) != 0) {
                candidate = change.newName + "_" + std::to_string(suffix++);
            }
            if (candidate != change.newName) ++result.stats.suffixResolved;
            change.newName = std::move(candidate);
            occupied.insert(change.newName);
        }
    }

    //6c. 方法闭包组：(owner, name, desc) 槽位；组内统一后缀保闭包一致性，
    //找不到一致后缀 → 整组拒（fail-closed，不破坏虚分派）。
    //查撞空间 = mapped-desc（luiox/）：占用表收集时已按 plan 类
    //映射预映像，此处 erase/查撞/回插/落位四路 desc 同口径经 mapMethodDescriptor
    //预映像（成员规则 newDesc 预映像同机制）。
    {
        const auto mappedMethodDesc = [&result](const std::string& desc) {
            return toStd(result.plan.mapMethodDescriptor(refOf(desc)));
        };
        for (auto& change : methodChanges) {
            occupancy[change.owner].methodSlots.erase(
                {change.oldName, mappedMethodDesc(change.desc)});
        }
        for (auto& group : methodGroups) {
            if (group.memberIndices.empty()) continue;
            const auto& base     = methodChanges[group.memberIndices.front()].newName;
            bool        resolved = false;
            std::string chosen   = base;
            for (ca::usize suffix = 0; suffix <= 64 && !resolved; ++suffix) {
                const auto candidate = suffix == 0 ? base : base + "_" + std::to_string(suffix);
                bool       freeEverywhere = true;
                for (const auto memberIndex : group.memberIndices) {
                    const auto& member = methodChanges[memberIndex];
                    if (occupancy[member.owner].methodSlots.count(
                            {candidate, mappedMethodDesc(member.desc)}) != 0) {
                        freeEverywhere = false;
                        break;
                    }
                }
                if (freeEverywhere) {
                    chosen   = candidate;
                    resolved = true;
                }
            }
            if (!resolved) {
                ++result.stats.rejected;
                const auto proposalIdx = methodChanges[group.memberIndices.front()].proposalIdx;
                std::string owners;
                for (const auto memberIndex : group.memberIndices) {
                    if (!owners.empty()) owners += ", ";
                    owners += methodChanges[memberIndex].owner;
                }
                result.record.addRejected(
                    std::move(proposals[proposalIdx]),
                    RejectionReason::Conflict,
                    toUtf8("no consistent _N suffix fits override component of '" +
                           methodChanges[group.memberIndices.front()].oldName +
                           "' (tried base and _1.._64; members: " + owners + ")"));
                for (const auto memberIndex : group.memberIndices) {
                    auto& member   = methodChanges[memberIndex];
                    member.dropped = true;
                    //整组拒 = 该方法实际没改走：必须回插旧槽占用，否则后续组
                    //可把新名撞上这个仍在的 (owner, name, desc) 槽 → 同 owner
                    //重复成员名。组件根去重后各组件不相交，旧槽恰好回插一次。
                    occupancy[member.owner].methodSlots.insert(
                        {member.oldName, mappedMethodDesc(member.desc)});
                }
                continue;
            }
            if (chosen != base) ++result.stats.suffixResolved;
            for (const auto memberIndex : group.memberIndices) {
                auto& member   = methodChanges[memberIndex];
                member.newName = chosen;
                occupancy[member.owner].methodSlots.insert({chosen, mappedMethodDesc(member.desc)});
            }
        }
    }

    //── 7. 构造 MemberRemapPlan + NameChangeRecord ──
    std::set<ca::usize> acceptedProposals;
    auto                acceptEntry = [&acceptedProposals](ca::usize proposalIdx) {
        acceptedProposals.insert(proposalIdx);
    };
    //成员改名的 owner 若无类名映射，补恒等类行（c A A）：Tiny-V2 等
    //类嵌套格式的导出以类行为挂载点，缺行会静默丢成员映射（往返恒等验收基准）。
    const auto ensureOwnerRow = [&result](const std::string& owner) {
        if (result.record.mapping().getClassMapping(refOf(owner)) == nullptr) {
            result.record.mappingMut().addClass(refOf(owner), refOf(owner));
        }
    };

    for (const auto& change : classChanges) {
        //类映射已在 6-pre 提前入 plan（：6c mapped-desc 查撞依赖），
        //此处只落 record 与审计流水。
        result.record.mappingMut().addClass(refOf(change.oldName), refOf(change.newName));
        const auto& proposal = proposals[change.proposalIdx];
        result.record.addAccepted({ProposalKind::Class,
                                   ca::str::Utf8String(),
                                   toUtf8(change.oldName),
                                   ca::str::Utf8String(),
                                   toUtf8(change.newName),
                                   proposal.source.clone(),
                                   proposal.confidence,
                                   change.fromPackage,
                                   proposal.rationale.clone(),
                                   proposal.evidence.clone()});
        acceptEntry(change.proposalIdx);
        ++result.stats.renamedMembers;
    }
    //package 建议：包级映射本身入 record（类级展开已逐条入上）。
    {
        std::set<ca::usize> packageDone;
        for (const auto& change : classChanges) {
            if (!change.fromPackage) continue;
            const auto proposalIdx = change.proposalIdx;
            if (!packageDone.insert(proposalIdx).second) continue;
            const auto& proposal = proposals[proposalIdx];
            result.record.mappingMut().addPackage(
                refOf(trimTrailingSlash(toStd(proposal.oldName))),
                refOf(trimTrailingSlash(toStd(proposal.newName))));
            result.record.addAccepted({ProposalKind::Package,
                                       ca::str::Utf8String(),
                                       toUtf8(trimTrailingSlash(toStd(proposal.oldName))),
                                       ca::str::Utf8String(),
                                       toUtf8(trimTrailingSlash(toStd(proposal.newName))),
                                       proposal.source.clone(),
                                       proposal.confidence,
                                       false,
                                       proposal.rationale.clone(),
                                       proposal.evidence.clone()});
            acceptEntry(proposalIdx);
        }
    }
    for (const auto& change : fieldChanges) {
        ensureOwnerRow(change.owner);
        //规则的 newOwner/newDesc 必须预映像（类映射已全部入 plan）：留原值会让
        //remapper 的指令同步路径把调用点 owner/描述符写回旧名（P1 实证：成员
        //改名与 owner 类改名同 plan 时，remapFieldInsn 优先按规则整槽赋值）。
        const auto newOwner = result.plan.mapInternalName(refOf(change.owner));
        const auto newDesc  = result.plan.mapFieldDescriptor(refOf(change.desc));
        result.plan.mapField({refOf(change.owner),
                              refOf(change.oldName),
                              refOf(change.desc),
                              newOwner,
                              refOf(change.newName),
                              newDesc});
        result.record.mappingMut().addField(
            refOf(change.owner), refOf(change.desc), refOf(change.oldName), refOf(change.newName));
        const auto& proposal = proposals[change.proposalIdx];
        result.record.addAccepted({ProposalKind::Field,
                                   toUtf8(change.owner),
                                   toUtf8(change.oldName),
                                   toUtf8(change.desc),
                                   toUtf8(change.newName),
                                   proposal.source.clone(),
                                   proposal.confidence,
                                   false,
                                   proposal.rationale.clone(),
                                   proposal.evidence.clone()});
        acceptEntry(change.proposalIdx);
        ++result.stats.renamedMembers;

        //字段继承别名（plan-only，不入 record/导出）：javac 可能以子类为 owner
        //发出继承字段的 field 指令（getfield B.f，f 实际声明于超类 A），而
        //remapper 只按精确 (owner,name,desc) 查规则。沿 obf rename_core 同款
        //口径补齐：子类 B 沿 super 链解析出的声明 owner 恰为本字段 owner 时，
        //逐子类补 (B, oldName, desc) → 新名别名规则。SRG/mapping 记录仍只含
        //声明处一条（与旧核心一致，别名是应用面同步细节）。
        const auto ownerIndex = context->hierarchy_.indexOf(refOf(change.owner));
        if (ownerIndex == INVALID_CLASS) continue;
        for (ca::usize c = 0; c < context->hierarchy_.classCount(); ++c) {
            const auto classIndex = static_cast<ClassIndex>(c);
            if (classIndex == ownerIndex ||
                !context->hierarchy_.isSubType(classIndex, ownerIndex)) {
                continue;
            }
            if (resolveInstanceFieldOwnerIndex(context->hierarchy_,
                                               classIndex,
                                               change.oldName,
                                               change.desc) != ownerIndex) {
                continue;
            }
            const auto& aliasEntry = context->hierarchy_.entry(classIndex);
            if (aliasEntry.classNode == nullptr) continue;
            result.plan.mapField({refOf(toStd(aliasEntry.name)),
                                  refOf(change.oldName),
                                  refOf(change.desc),
                                  result.plan.mapInternalName(refOf(toStd(aliasEntry.name))),
                                  refOf(change.newName),
                                  result.plan.mapFieldDescriptor(refOf(change.desc))});
        }
    }
    for (const auto& change : methodChanges) {
        if (change.dropped) continue;
        ensureOwnerRow(change.owner);
        //同 field：newOwner/newDesc 预映像，避免同 plan 类改名时调用点写回旧值。
        const auto newOwner = result.plan.mapInternalName(refOf(change.owner));
        const auto newDesc  = result.plan.mapMethodDescriptor(refOf(change.desc));
        result.plan.mapMethod({refOf(change.owner),
                               refOf(change.oldName),
                               refOf(change.desc),
                               newOwner,
                               refOf(change.newName),
                               newDesc});
        result.record.mappingMut().addMethod(
            refOf(change.owner), refOf(change.desc), refOf(change.oldName), refOf(change.newName));
        const auto& proposal = proposals[change.proposalIdx];
        result.record.addAccepted({ProposalKind::Method,
                                   toUtf8(change.owner),
                                   toUtf8(change.oldName),
                                   toUtf8(change.desc),
                                   toUtf8(change.newName),
                                   proposal.source.clone(),
                                   proposal.confidence,
                                   change.fromClosure,
                                   proposal.rationale.clone(),
                                   proposal.evidence.clone()});
        acceptEntry(change.proposalIdx);
        ++result.stats.renamedMembers;
    }

    //方法继承别名（plan-only，不入 record/导出）：闭世界子类继承了改名方法但
    //未自行声明时，其继承引用（invokevirtual B.m，m 实际声明于超类 A）在
    //remapper 的精确规则查找下会漏改。沿 obf rename_core 同款口径补齐：
    //1) C 未声明 (oldName, desc)（声明者由闭包规则覆盖）；
    //2) C 的继承闭包封闭（不延伸到容器外类）；
    //3) C 是组内任一成员 owner 的子类型；
    //满足则补 (C, oldName, desc) → 组新名别名规则。同一 (C, oldName, desc)
    //被两组以不同新名称命时歧义——整条放弃（旧核心同口径）。
    {
        struct MethodAliasCandidate
        {
            std::string owner;
            std::string oldName;
            std::string desc;
            std::string newName;
            bool        ambiguous = false;
        };
        std::map<std::tuple<std::string, std::string, std::string>, MethodAliasCandidate>
            methodAliases;

        for (const auto& group : methodGroups) {
            if (group.memberIndices.empty()) continue;
            const auto& first = methodChanges[group.memberIndices.front()];
            if (first.dropped) continue;   //整组被拒：方法未改走，无别名可补

            for (ca::usize c = 0; c < context->hierarchy_.classCount(); ++c) {
                const auto  classIndex = static_cast<ClassIndex>(c);
                const auto& entry      = context->hierarchy_.entry(classIndex);
                if (entry.classNode == nullptr ||
                    classDeclaresMethod(*entry.classNode, first.oldName, first.desc) ||
                    !hierarchyClosedForVirtualRename(context->hierarchy_, classIndex)) {
                    continue;
                }

                bool inheritsGroup = false;
                for (const auto memberIndex : group.memberIndices) {
                    const auto& member = methodChanges[memberIndex];
                    const auto  memberOwnerIndex =
                        context->hierarchy_.indexOf(refOf(member.owner));
                    if (memberOwnerIndex != INVALID_CLASS &&
                        context->hierarchy_.isSubType(classIndex, memberOwnerIndex)) {
                        inheritsGroup = true;
                        break;
                    }
                }
                if (!inheritsGroup) continue;

                const auto key =
                    std::make_tuple(toStd(entry.name), first.oldName, first.desc);
                auto [it, inserted] = methodAliases.try_emplace(
                    key, MethodAliasCandidate{toStd(entry.name), first.oldName, first.desc, first.newName});
                if (!inserted && it->second.newName != first.newName) it->second.ambiguous = true;
            }
        }
        for (const auto& [key, alias] : methodAliases) {
            (void)key;
            if (alias.ambiguous) continue;
            result.plan.mapMethod({refOf(alias.owner),
                                   refOf(alias.oldName),
                                   refOf(alias.desc),
                                   result.plan.mapInternalName(refOf(alias.owner)),
                                   refOf(alias.newName),
                                   result.plan.mapMethodDescriptor(refOf(alias.desc))});
        }
    }
    result.stats.accepted = acceptedProposals.size();

    //── 8. plan-only 或应用（应用失败全量不落盘：既有 remapper 原子性口径）──
    if (!options.planOnly) {
        if (result.plan.empty()) {
            //空 plan + 非 plan-only：无可应用内容即成功（zero-applied）。
            //applyResourceRemapPlan 对空 plan 返回 false，直接调用会被误判 ApplyFailed。
            result.applied = true;
        }
        else {
            ca::str::Utf8StringArena applyArena;
            if (!applyResourceRemapPlan(container, applyArena, result.plan, options.policy)) {
                ++result.stats.rejected;
                result.record.addRejected(
                    NameProposal{},
                    RejectionReason::ApplyFailed,
                    toUtf8("applyResourceRemapPlan failed; nothing committed"));
                result.status = ca::Status::error(
                    ca::StatusCode::FAILED_PRECONDITION,
                    "renamer apply failed; container untouched per remapper atomicity");
                return result;
            }
            result.applied = true;
        }
    }

    result.status = ca::OkStatus();
    return result;
}

}   //namespace mjt::base
