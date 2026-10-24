#pragma once

// pysdk_facade.hpp — mjt-py SDK v3 facade（契约：doc_ai/mjt-py-sdk-spec.md v3）。
//
// Ref=键：Python 侧只持 (handle, name/desc/uid) 纯值；每次调用本层按键重解析，
// 失效一律 StaleRefError（fail-closed，不静默、不复活）。树缓存 / name↔URI 索引 /
// 指令 uid 映射 / 脏纪律全部本层自管——Python 零所有权、零对象图、零引擎指针。
//
// 首语义（#962）：容器内同名类按 JVM first-wins 保留首条目，落选者记入
// duplicateUris_；唯一 winner（或被晋升者）可经类名寻址。

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jbytecode/jbytecode.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mjbase/asm/remapper/member_remap_plan.hpp"
#include "mjbase/mapping/intermediate_mapping.hpp"
#include "mjbase/model/resource_container.hpp"
#include "mjbase/renamer/shadow_alias.hpp"

#include "pysdk_jar.hpp"

namespace jbytecode::tree {
class ClassNode;
class MethodNode;
class FieldNode;
class AbstractInsnNode;
}   // namespace jbytecode::tree

namespace mjt::pysdk {

/// v3 失效语义异常：键解析失败 / 结构歧义。pybind 注册为 KeyError 子类。
class StaleRefError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// 指令 uid 状态（按需物化；随树生灭——uid 映射绝不越过树寿命）。
struct InsnUidState
{
    bool    materialized = false;
    ca::u32 nextUid      = 1;
    std::unordered_map<ca::u32, jbytecode::tree::AbstractInsnNode*>    byUid;
    std::unordered_map<jbytecode::tree::AbstractInsnNode*, ca::u32>    uidOf;
    std::unordered_set<ca::u32> dead;   ///< 墓地：remove 后保留映射，解析即抛
    /// Label* → uid 反查缓存（lazy；emit/remove 置脏后按 byUid∖dead 重建）。
    /// 与逐次重建 labelUids 结果恒等——只是免掉跳转/switch 读侧每次 O(n)
    /// 重建（定点优化，语义不变）。
    std::unordered_map<jbytecode::core::Label*, ca::u32> labelIndex;
    bool labelIndexDirty = true;
};

/// 类树缓存条目。脏纪律：树改动置位；write_back 清位；外部改字节 → 整条目作废。
/// MVP 不驱逐（会话内常驻，lazy 解析已收敛 open 成本）；脏树永不参与驱逐。
struct TreeEntry
{
    std::shared_ptr<jbytecode::tree::ClassNode> node;
    bool dirty = false;
    std::unordered_map<jbytecode::tree::MethodNode*, InsnUidState> uidStates;
};

/// 容器会话：v3 SDK 的一切 C++ 侧状态归此，Python 只见句柄与键。
class FacadeSession
{
public:
    ca::u64     handle = 0;
    std::string sourcePath;
    std::shared_ptr<ca::str::Utf8StringArena> arena =
        std::make_shared<ca::str::Utf8StringArena>();
    std::unique_ptr<mjbase::model::ResourceContainer> container =
        std::make_unique<mjbase::model::ResourceContainer>();

    /// name↔URI 索引（first-wins，#962；导入/add/load/remove 时维护）。
    std::map<std::string, std::string> nameToUri;
    /// first-wins 落选者（internal name → 落选 URI，保序；winner 移除时按序晋升）。
    std::multimap<std::string, std::string> duplicateUris;

    /// 树缓存（lazy：首次树操作时解析；uid 映射随条目生灭）。
    std::map<std::string, TreeEntry> trees;

    /// 管线 tail ephemeral 方法（"\x00ephem:<n>" → 管线内部 MethodNode）；
    /// 仅 pass run() 调用期内登记有效（契约 §3.6 唯一对象形例外）。
    std::unordered_map<std::string, jbytecode::tree::MethodNode*> ephemeralMethods;
    std::unordered_map<jbytecode::tree::MethodNode*, InsnUidState> ephemeralUids;
    ca::u64 ephemeralNext = 1;

    /// 最近一次 run_pass 产物（L5 报告等位物；值拷贝视图）。
    std::shared_ptr<PyRunStats> lastStats;
    std::string                 lastPipeline;

    /// rename 会话 token（单活动；rename_begin 重置；commit/dump 后保持至下次 begin）。
    struct RenameToken
    {
        mjbase::asm_remapper::MemberRemapPlan plan;
        std::shared_ptr<mjbase::mapping::IntermediateMapping> mapping;
    };
    std::optional<RenameToken> renameToken;

    /// 类改名 → 条目 URI 迁移挂账（oldUri → newUri）。rename_class 只改树与索引；
    /// 条目搬移在 write_back/flush/save/export/sync_entries 边界统一落账
    /// （G2 复现盘：不落账 = export 静默丢改名，flush 后 = entry↔this_class 失配）。
    std::map<std::string, std::string> pendingEntryRenames;

    /// shadow 显示层表（#1072/#1156：真名→仅显示层名字；永不写回字节码）。
    /// shadow_generate/shadow_load 填充，shadow_dump/shadow_entries 消费。
    std::shared_ptr<mjbase::renamer::ShadowAliasTable> shadowTable;

    /// 全树缓存作废（管线直驱容器字节 / rename 提交后调用；lazy 下零成本）。
    void clearTrees() { trees.clear(); }

    /// 重建 name↔URI 索引（导入后调用；头解析对名，first-wins 记落选者）。
    void rebuildIndex();
};

/// 会话注册表：句柄 → 会话；close 后句柄解析即 StaleRefError。
class FacadeRegistry
{
public:
    static FacadeRegistry& instance();

    /// 新建会话（openJar 异常时调用方须 close 回滚）。
    FacadeSession& create();
    /// 解析句柄；未知/已 close → StaleRefError。
    FacadeSession&       get(ca::u64 handle);
    const FacadeSession& get(ca::u64 handle) const;
    void                 close(ca::u64 handle);
    ca::usize            size() const { return sessions_.size(); }

private:
    std::map<ca::u64, std::unique_ptr<FacadeSession>> sessions_;
    ca::u64 next_ = 1;
};

// ── 会话级操作（B1：容器/条目面） ─────────────────────────────────────────

/// 从 jar 导入全部资源 + 重建索引（open_jar 的实质）。
void facadeImportJar(FacadeSession& session, const std::string& path);

/// 条目清单（全部资源，含非类）：(uri, 类型名) 序对。
std::vector<std::pair<std::string, std::string>> facadeListEntries(const FacadeSession& s);

/// 条目原始字节（Class/Unknown/Manifest 均可读；Manifest 序列化现值）。
std::vector<ca::u8> facadeGetEntryBytes(const FacadeSession& s, const std::string& uri);

/// 条目字节替换（Class→作废缓存树；Manifest→fromBytes 重析；Unknown→直换）。
void facadeSetEntryBytes(FacadeSession& s, const std::string& uri, std::vector<ca::u8> bytes);

/// 新增条目：URI 已存在 → StaleRefError；类字节自动进索引（first-wins 记落选）。
void facadeAddEntry(FacadeSession& s, const std::string& uri, std::vector<ca::u8> bytes);

/// 删除条目 + 索引/树缓存清理（winner 移除时晋升落选者）。
void facadeRemoveEntry(FacadeSession& s, const std::string& uri);

/// 磁盘 loose .class 入容器；URI 默认 = 文件名；冲突默认抛错（replace=true 覆盖）。
void facadeLoadClassFile(FacadeSession& s, const std::string& path,
                         std::optional<std::string> uri, bool replace);

/// 已解析类名（升序，= 索引键序）。
std::vector<std::string> facadeClassNames(const FacadeSession& s);

/// 类名 → URI（索引反查；未索引 → StaleRefError）。
std::string facadeClassUri(const FacadeSession& s, const std::string& name);

/// first-wins 落选类名清单（升序）。
std::vector<std::string> facadeDuplicateNames(const FacadeSession& s);

/// 类字节（索引 O(1) 直取，不建树）。
std::vector<ca::u8> facadeClassBytes(const FacadeSession& s, const std::string& name);

/// 类字节写回/新增（不存在则新增条目 URI=name+".class"）；覆盖时作废缓存树。
void facadeSetClassBytes(FacadeSession& s, const std::string& name, std::vector<ca::u8> bytes);

/// 导出 jar（uris 空=全量；非空=子集，Class/Manifest/Unknown 深拷贝入临时容器）。
/// 两阶段落盘（暂存 + rename；PyJar::save 同款纪律）。
void facadeExportJar(FacadeSession& s, const std::string& path,
                     const std::vector<std::string>& uris);

/// 单类导出：脏树先 write_back 再落盘为 .class 文件。
void facadeExportClass(FacadeSession& s, const std::string& name, const std::string& path);

// ── 工具（pysdk_jar 共用；B1 起由 facade 侧持有） ─────────────────────────

/// class 字节头解析对名（不建树；失败 → nullopt）。
std::optional<std::string> facadeProbeClassName(const std::vector<ca::u8>& bytes);

/// 指令 kind 名（AbstractInsnNode::TYPE_* → 助记串）。
const char* facadeInsnKindName(int kind);

/// 字符串 ldc 载荷收进会话 arena → Any（绑定层临时 std::string 的悬垂视图
/// 防线：Any 存 Utf8StringRef 视图，必须指向会话寿命的存储）。
ca::core::Any facadeInternedStr(FacadeSession& s, const std::string& v);

/// 标准 JVM opcode 助记名（索引=opcode 值，规范序 0..201；越界返回 "?"）。
const char* facadeOpcodeName(int op);

// ── 树面（B2，契约 §2.2-2.6）─────────────────────────────────────────────
// 键口径：cref=(句柄, internal name)、mref/fref=(句柄, cls, name, desc)——
// 元组由绑定层拼装，facade 直言普通参数。所有树操作 lazy 解析（首触即建缓存），
// 改动一律标脏；冲突（键已占用）与失效（键解析不出）同走 StaleRefError。

struct ClassInfo
{
    std::string              name, superName;
    int                      access = 0, version = 0;
    std::vector<std::string> interfaces;
};

/// 方法/字段信息（字段用 name/desc/access，其余空 0）。
struct MemberInfo
{
    std::string              name, desc;
    int                      access = 0;
    int                      maxStack = 0, maxLocals = 0;
    std::vector<std::string> exceptions;
};

// ── 读 ──

/// 附着解析（幂等；不存在/解析失败/索引名不符 fail-closed）。
void facadeParseClass(FacadeSession& s, const std::string& name);

ClassInfo facadeClassInfo(FacadeSession& s, const std::string& name);

/// (name, desc, access) 列表；methods=true 方法 / false 字段。
std::vector<std::tuple<std::string, std::string, int>> facadeMembers(FacadeSession& s,
                                                                     const std::string& name,
                                                                     bool methods);

MemberInfo facadeMethodInfo(FacadeSession& s, const std::string& cls, const std::string& name,
                            const std::string& desc);
MemberInfo facadeFieldInfo(FacadeSession& s, const std::string& cls, const std::string& name,
                           const std::string& desc);

// ── 写（类） ──

void facadeClassSetSuper(FacadeSession& s, const std::string& name, const std::string& super);
void facadeClassSetAccess(FacadeSession& s, const std::string& name, int access);
void facadeClassSetVersion(FacadeSession& s, const std::string& name, int version);
void facadeClassSetInterfaces(FacadeSession& s, const std::string& name,
                              const std::vector<std::string>& interfaces);
/// 类改名 + 索引迁移；返回新名（=新键）。只改类自身身份，引用重写走 rename token 面。
std::string facadeRenameClass(FacadeSession& s, const std::string& oldName,
                              const std::string& newName);

// ── 写（方法/字段） ──

/// 改名族返回新键的键段（绑定层拼装新 mref/fref；旧键后续解析 fail-closed）。
std::string facadeRenameMethod(FacadeSession& s, const std::string& cls, const std::string& name,
                               const std::string& desc, const std::string& newName);
std::string facadeRenameField(FacadeSession& s, const std::string& cls, const std::string& name,
                              const std::string& desc, const std::string& newName);
void        facadeMethodSetDesc(FacadeSession& s, const std::string& cls, const std::string& name,
                                const std::string& desc, const std::string& newDesc);
void        facadeMethodSetAccess(FacadeSession& s, const std::string& cls,
                                  const std::string& name, const std::string& desc, int access);
void        facadeMethodSetMaxStack(FacadeSession& s, const std::string& cls,
                                    const std::string& name, const std::string& desc, int v);
void        facadeMethodSetMaxLocals(FacadeSession& s, const std::string& cls,
                                     const std::string& name, const std::string& desc, int v);
void        facadeMethodSetExceptions(FacadeSession& s, const std::string& cls,
                                      const std::string& name, const std::string& desc,
                                      const std::vector<std::string>& exceptions);
void        facadeFieldSetAccess(FacadeSession& s, const std::string& cls, const std::string& name,
                                 const std::string& desc, int access);

/// 新增成员（MethodNode 堆 new，nodes.hpp:637-638 契约）；键冲突 → StaleRefError。
std::string facadeAddMethod(FacadeSession& s, const std::string& cls, const std::string& name,
                            const std::string& desc, int access, int maxStack, int maxLocals);
std::string facadeAddField(FacadeSession& s, const std::string& cls, const std::string& name,
                           const std::string& desc, int access);
void        facadeRemoveMethod(FacadeSession& s, const std::string& cls, const std::string& name,
                               const std::string& desc);
void        facadeRemoveField(FacadeSession& s, const std::string& cls, const std::string& name,
                              const std::string& desc);

// ── 指令面（uid 跨结构编辑稳定；remove 后进墓地解析即抛） ──

struct InsnBrief
{
    ca::u32 uid = 0;
    int     opcode = 0, kind = 0;
};

struct InsnDetail
{
    ca::u32 uid = 0;
    int     opcode = 0, kind = 0;
    int     var = 0, incr = 0, operand = 0, dims = 0, line = 0;
    std::string desc, owner, name;
    bool        itf = false;
    /// ldc 载荷判别：0=无 1=str 2=int 3=i64 4/5=float/double。
    int         cstTag = 0;
    std::string cstStr;
    ca::i64     cstInt = 0;
    double      cstFloat = 0;
    /// 跳转/switch 目标（label → 其 LabelNode 的 uid）。
    ca::u32              targetUid = 0, defaultUid = 0;
    int                  switchMin = 0, switchMax = 0;
    std::vector<ca::u32> targetUids;
    std::vector<int>     switchKeys;
};

ca::u32 facadeInsnCount(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc);
std::vector<InsnBrief> facadeInsns(FacadeSession& s, const std::string& cls,
                                   const std::string& name, const std::string& desc);
InsnDetail             facadeInsnDetail(FacadeSession& s, const std::string& cls,
                                        const std::string& name, const std::string& desc,
                                        ca::u32 uid);
/// 诊断用 O(n) 位次（非键）。
ca::u32 facadeInsnIndex(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc, ca::u32 uid);
std::optional<ca::u32> facadeFirstInsn(FacadeSession& s, const std::string& cls,
                                       const std::string& name, const std::string& desc);
std::optional<ca::u32> facadeLastInsn(FacadeSession& s, const std::string& cls,
                                      const std::string& name, const std::string& desc);
std::optional<ca::u32> facadeNextInsn(FacadeSession& s, const std::string& cls,
                                      const std::string& name, const std::string& desc,
                                      ca::u32 uid);
std::optional<ca::u32> facadePrevInsn(FacadeSession& s, const std::string& cls,
                                      const std::string& name, const std::string& desc,
                                      ca::u32 uid);

void facadeInsnSetOpcode(FacadeSession& s, const std::string& cls, const std::string& name,
                         const std::string& desc, ca::u32 uid, int op);
void facadeInsnSetVar(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::u32 uid, int var);
void facadeInsnSetIncr(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, ca::u32 uid, int incr);
void facadeInsnSetOperand(FacadeSession& s, const std::string& cls, const std::string& name,
                          const std::string& desc, ca::u32 uid, int operand);
void facadeInsnSetDesc(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, ca::u32 uid, const std::string& typeDesc);
void facadeInsnSetFieldRef(FacadeSession& s, const std::string& cls, const std::string& name,
                           const std::string& desc, ca::u32 uid, const std::string& owner,
                           const std::string& refName, const std::string& refDesc);
void facadeInsnSetMethodRef(FacadeSession& s, const std::string& cls, const std::string& name,
                            const std::string& desc, ca::u32 uid, const std::string& owner,
                            const std::string& refName, const std::string& refDesc, bool itf);
void facadeInsnSetCst(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::u32 uid, ca::core::Any value);
void facadeInsnSetTarget(FacadeSession& s, const std::string& cls, const std::string& name,
                         const std::string& desc, ca::u32 uid, ca::u32 targetUid);

/// 发指令：pos=nullopt 尾插，pos=uid 插其前；返回新 uid。
ca::u32 facadeEmitInsn(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int op, std::optional<ca::u32> pos);
ca::u32 facadeEmitVar(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, int op, int var, std::optional<ca::u32> pos);
ca::u32 facadeEmitIinc(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int var, int incr, std::optional<ca::u32> pos);
ca::u32 facadeEmitType(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int op, const std::string& typeDesc,
                       std::optional<ca::u32> pos);
ca::u32 facadeEmitField(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc, int op, const std::string& owner,
                        const std::string& refName, const std::string& refDesc,
                        std::optional<ca::u32> pos);
ca::u32 facadeEmitMethodCall(FacadeSession& s, const std::string& cls, const std::string& name,
                             const std::string& desc, int op, const std::string& owner,
                             const std::string& refName, const std::string& refDesc, bool itf,
                             std::optional<ca::u32> pos);
ca::u32 facadeEmitLdc(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::core::Any value, std::optional<ca::u32> pos);
ca::u32 facadeEmitJump(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, int op, ca::u32 targetUid,
                       std::optional<ca::u32> pos);
/// 发 TABLESWITCH：min..max 逐个对应 targets（targets.size() == max-min+1，结构性
/// 校验 fail-closed）；default_uid 与 targets 均为既有 label uid（emit_jump 同款
/// 语义）。pos=None 尾插 / pos=uid 插其前；返回 switch 自身的新 uid。
ca::u32 facadeEmitTableSwitch(FacadeSession& s, const std::string& cls, const std::string& name,
                              const std::string& desc, int min, int max, ca::u32 defaultUid,
                              const std::vector<ca::u32>& targetUids, std::optional<ca::u32> pos);
/// 发 LOOKUPSWITCH：keys 与 targets 成对（size 相等）且 keys 须严格升序
/// （JVM 查找表序，乱序写出的类真机验证必炸——fail-closed）；default_uid 与
/// targets 均为既有 label uid。pos 同 emit 族；返回 switch 自身的新 uid。
ca::u32 facadeEmitLookupSwitch(FacadeSession& s, const std::string& cls, const std::string& name,
                               const std::string& desc, const std::vector<int>& keys,
                               ca::u32 defaultUid, const std::vector<ca::u32>& targetUids,
                               std::optional<ca::u32> pos);
/// 建标签（Label 归属方法 arena，B8 归属制）；返回其 uid。
ca::u32 facadeEmitLabel(FacadeSession& s, const std::string& cls, const std::string& name,
                        const std::string& desc, std::optional<ca::u32> pos);
/// 摘链进墓地（uid 保留映射，后续解析抛 "insn removed"；节点内存随树回收）。
void facadeRemoveInsn(FacadeSession& s, const std::string& cls, const std::string& name,
                      const std::string& desc, ca::u32 uid);

// ── 异常表 ──

std::vector<std::tuple<ca::u32, ca::u32, ca::u32, std::string>> facadeTryCatches(
    FacadeSession& s, const std::string& cls, const std::string& name, const std::string& desc);
/// 三 uid 均须为 label 节点。
void facadeAddTryCatch(FacadeSession& s, const std::string& cls, const std::string& name,
                       const std::string& desc, ca::u32 startUid, ca::u32 endUid,
                       ca::u32 handlerUid, const std::string& catchType);
void facadeRemoveTryCatch(FacadeSession& s, const std::string& cls, const std::string& name,
                          const std::string& desc, ca::usize rowIndex);

// ── 写回/落盘（脏纪律） ──

/// 单类 树→字节 + 清脏；COMPUTE_FRAMES 写不出即抛（结构/栈校验语义）。
void facadeWriteBack(FacadeSession& s, const std::string& name, bool computeFrames);

/// 全脏树写回；返回写出的类名（升序）。
std::vector<std::string> facadeFlush(FacadeSession& s, bool computeFrames);

/// flush 全脏 + 两阶段落盘；未解析/未脏类字节原样导出（零成本）。
void facadeSave(FacadeSession& s, const std::string& path, bool computeFrames);

// ── ephemeral 方法（契约 §3.6；B3 管线 tail 用） ──────────────────────────

/// 登记 pipeline 内部方法 → 返回 ephemeral cls 键段（"\x00ephem:<n>"）。
/// uid 态挂 session.ephemeralUids；读/指令结构面全量可用，身份写面（rename/
/// add/remove 成员）不支持。release 后该键全部解析 fail-closed。
std::string facadeAdoptEphemeralMethod(FacadeSession& s, jbytecode::tree::MethodNode& m,
                                       const std::string& name, const std::string& desc);
void facadeReleaseEphemeralMethod(FacadeSession& s, const std::string& ephemKey);

// ── rename token 面（契约 §2.7；单活动 token，rename_begin 重置） ─────────

/// 重置 rename 会话（收计划前调用）。
void facadeRenameBegin(FacadeSession& s);
/// 类改名入计划（收 internal name；v2 PyRenameEngine 同款语义——引用完整性
/// 由 remapper 在 commit 时保证）。
void facadeTokenRenameClass(FacadeSession& s, const std::string& oldName,
                            const std::string& newName);
void facadeTokenRenameMethod(FacadeSession& s, const std::string& owner, const std::string& name,
                             const std::string& desc, const std::string& newName);
void facadeTokenRenameField(FacadeSession& s, const std::string& owner, const std::string& name,
                            const std::string& desc, const std::string& newName);
/// 应用计划到容器（字节码面恒开；reflection/annotations/signatures/resources
/// 可选，resources 默认开——条目 URI 迁移与 MANIFEST/services 重写同属一次
/// 语义改名，默认关闭会让导出 jar 新类挂旧 URI）。提交前自动 flush 全部脏树
/// （computeFrames=false：remap 保帧；COMPUTE_FRAMES 写回对尾插死代码形状有
/// 损坏案例，需要重算的脚本显式 flush(h, true)）。
/// 成功后全部缓存树作废（管线直驱容器字节同款语义）。
void facadeTokenCommit(FacadeSession& s, bool reflectionStrings, bool annotations,
                       bool signatures, bool resources);
/// 导出 sidecar（fmt：Enigma/Tiny-V2/Tiny-V1/ProGuard/SRG/Simple/R8）。
void facadeMappingDump(FacadeSession& s, const std::string& path, const std::string& fmt);
/// (class, method, field) 计数。
std::tuple<ca::usize, ca::usize, ca::usize> facadeMappingCounts(FacadeSession& s);

/// 记录一次管线/pass 运行报告（run_pass 出口调用；私有侧 run_pipeline 复用同槽）。
void facadeRecordRun(FacadeSession& s, const std::string& name,
                     const std::shared_ptr<PyRunStats>& stats);

// ── 容器完整性 / 批量改名 / shadow 显示层（实测补刀批） ──────────────────

/// 落账全部挂账的类改名条目搬移（老 URI 字节 → 新 URI + 索引迁移）；
/// 返回 "old -> new" 迁移清单。write_back/flush/save/export 出口自动调用，
/// 亦可脚本显式调用。
std::vector<std::string> facadeSyncEntries(FacadeSession& s);

struct VerifyIssue
{
    std::string entry, detail;
};

/// 容器完整性报告（=外部 entry-probe oracle 的 SDK 内建等位物）。
struct ContainerVerifyReport
{
    ca::usize match = 0, mismatch = 0, malformed = 0, depMissing = 0, nonClass = 0;
    std::vector<VerifyIssue> mismatchDetails;   ///< entry 路径 ↔ this_class 不符
    std::vector<VerifyIssue> malformedDetails;  ///< 解析失败
    std::vector<VerifyIssue> depMissingDetails; ///< 引用的非 JDK 类不在容器内
};

/// 全量体检：逐 *.class 条目解析（malformed）、entry 名 ↔ 声明名比对（mismatch）、
/// 常量池引用的容器外非 JDK 类清点（dep_missing）。与数据无关，只读。
ContainerVerifyReport facadeVerifyContainer(FacadeSession& s);

struct SanitizeLvtReport
{
    ca::usize dropped = 0, deduped = 0, classes = 0;
};

/// LVT 清洗（G1 复现盘：Invalid start_pc / Duplicated LVT entry）：
/// 全类逐方法剔除 start/end 标签已脱链（墓地/删节点）的 LocalVariableNode、
/// 按 (index,start,end,name,desc) 去重；改动标脏。局部变量节点为方法 arena
/// 持有（nodes.hpp visitLocalVariable create<>），摘除只出 vector、不 delete。
SanitizeLvtReport facadeSanitizeLvt(FacadeSession& s);

struct RejectedSuggestion
{
    std::string kind;      ///< class/package/field/method
    std::string owner;     ///< class/package 建议为空
    std::string name;      ///< 原名（oldName）
    std::string desc;      ///< field/method 为空
    std::string newName;
    std::string source;
    std::string reason;    ///< conflict/not_found/schema/...（rejectionReasonName 口径）
    std::string detail;    ///< 自含两侧坐标的撞名定位（撞在哪个槽、双方 owner/old/new）
};

struct SuggestionRunReport
{
    ca::usize proposed = 0, accepted = 0, rejected = 0, renamedMembers = 0;
    /// 被拒流水（fail-closed 报错面）：脚本侧可直接读 owner/old/new/撞名对象定位，
    /// 不再只拿计数。
    std::vector<RejectedSuggestion> rejectedEntries;
};

/// 继承闭包单类信息（name + 直接 super + 直接接口）。
struct HierarchyClass
{
    std::string              name, superName;
    std::vector<std::string> interfaces;
};

/// 继承闭包查询结果：闭包内类（self 先序 BFS）+ 沿途声明成员集
/// （methods/fields 元组 = (owner, name, desc, access)；只含容器内类）。容器外
/// 父类/接口（java/lang/Object 常态）不入闭包。
struct ClassHierarchyReport
{
    std::vector<HierarchyClass> classes;
    std::vector<std::tuple<std::string, std::string, std::string, int>> methods;
    std::vector<std::tuple<std::string, std::string, std::string, int>> fields;
};

/// 继承闭包查询（super 链 + 全部接口传递闭包 + 沿途成员集）；起点类不存在抛
/// StaleRefError。
ClassHierarchyReport facadeClassHierarchy(FacadeSession& s, const std::string& name);

/// 批量改名（mjt rename --suggestions --apply 的 SDK 等位物）：读建议文件
/// （v1 schema，mjbase/renamer/name_suggestion_file）→ RenamerEngine 全量裁决
/// + 应用（planOnly=false）。policy：bytecode-only / bytecode-reflect /
/// bytecode-metadata / all。成功后 clearTrees+rebuildIndex；plan/映射收进
/// rename token（mapping_dump/mapping_counts 直接可用）。
SuggestionRunReport facadeRenameApplySuggestions(FacadeSession& s, const std::string& path,
                                                 const std::string& policy);

struct ShadowGenReport
{
    ca::usize classes = 0, methods = 0, fields = 0, kept = 0;
};

/// 机械占位名批量生成（mjt shadow 的 SDK 等位物，#1072 钉死规则 + token 补号）。
/// 幂等：已登记条目保留。覆盖会话 shadow 表。
ShadowGenReport facadeShadowGenerate(FacadeSession& s);

/// 载入 shadow sidecar（严格 schema 入口；session.shadowTable 整表替换）；
/// 返回条目数。
ca::usize facadeShadowLoadFile(FacadeSession& s, const std::string& path);

/// 会话 shadow 表写出 sidecar（v2 JSON，token 列随行）。
void facadeShadowDumpFile(FacadeSession& s, const std::string& path);

/// 会话 shadow 表全条目：(kind, owner, desc, raw, shadow, token8)；
/// kind ∈ class/field/method。空表（未生成/未载入）→ 空列表。
std::vector<std::tuple<std::string, std::string, std::string, std::string, std::string,
                       std::string>>
facadeShadowEntries(FacadeSession& s);

// ── 工具等位物批（退役 MCP 工具的 SDK 语义迁位；jar_diff/jar_fix/对比/侧车） ──

/// 单条目字节级 diff（mjbase diffContainers 同口径：单一公共前后缀收缩，
/// 一个 range 覆盖第一差异字节到最后差异字节）。
struct JarByteRange
{
    ca::usize offset        = 0;
    ca::usize beforeLength = 0;
    ca::usize afterLength  = 0;
};

struct JarByteDiff
{
    std::string             uri;
    ca::usize               beforeSize = 0;
    ca::usize               afterSize  = 0;
    std::vector<JarByteRange> ranges;
};

/// 会话容器 vs 外部参考 JAR 的全资源字节比较（retired jar_diff 等位物）。
/// 计数口径：workspaceOnly = 仅会话容器有；referenceOnly = 仅参考有；
/// modified = 两侧都有但字节不同（清单不含三者，只含差异条目）。
struct JarDiffReport
{
    std::string             referencePath;
    ca::usize               workspaceOnly = 0, referenceOnly = 0, modified = 0;
    std::vector<JarByteDiff> diffs;
};

/// jar_diff：参考 JAR 导入临时容器后与会话容器全量比对；uri 升序。
JarDiffReport facadeJarDiff(FacadeSession& s, const std::string& referencePath);

/// 容器深拷贝（opt_preview 试算隔离面；workspace cloneContainer 同口径）。
std::unique_ptr<mjbase::model::ResourceContainer> facadeCloneContainer(const FacadeSession& s);

/// diffContainers 同口径字节 diff（jar_diff / opt_preview 共用算法面）；
/// uri 升序，逐条目单次公共前后缀收缩。
std::vector<JarByteDiff> facadeDiffContainers(const mjbase::model::ResourceContainer& before,
                                              const mjbase::model::ResourceContainer& after);

/// 单个修复候选问题（retired jar_fix 扫描口径）。
struct JarRepairIssueView
{
    std::string uri, kind, message;
    ca::usize   size = 0;
    bool        selected = false;
};

struct JarRepairReport
{
    ca::usize                        resourceCount = 0, classCount = 0;
    std::vector<JarRepairIssueView>  issues;
    /// apply 时实际移除的条目（analyze 恒空）。
    std::vector<std::string>         removed;
};

/// jar_fix_analyze：只读扫描（invalid_class = ClassResource 过不了 Jasm 校验；
/// invalid_class_entry = .class 扩展名但不是 ClassResource；corrupt_output_marker
/// = uri 含 corrupt_output_stream 标记）。selected 按 request 开关回填。
JarRepairReport facadeJarRepairAnalyze(FacadeSession& s, bool removeInvalidClassEntries,
                                       bool removeCorruptOutputMarkers);

/// jar_fix_apply：同扫描后移除全部 selected 条目（索引/树缓存同步清理）。
JarRepairReport facadeJarRepairApply(FacadeSession& s, bool removeInvalidClassEntries,
                                     bool removeCorruptOutputMarkers);

/// 单后端反编译产物（retired decompiler_compare 的 source 形状）。
struct DecompileCompareSource
{
    std::string internalName, backend, decompilerJar, sourceScope, source;
};

struct DecompileCompareReport
{
    std::string                        internalName;
    std::vector<DecompileCompareSource> sources;
};

/// decompiler_compare：会话容器导出快照后用 CFR 与 Vineflower 各跑一次同名类，
/// 双源并列返回。后端不可用（jar/java 发现失败）fail-closed 抛错。
DecompileCompareReport facadeDecompilerCompare(FacadeSession& s, const std::string& name,
                                               const std::string& cfrJar,
                                               const std::string& vineflowerJar,
                                               const std::string& javaProgram,
                                               std::int64_t       timeoutMs);

/// shadow sidecar 文件面报告（generate 用 assigned* 字段，prune 用 removed）。
struct ShadowSidecarReport
{
    std::string file;
    ca::usize   assignedClasses = 0, assignedMethods = 0, assignedFields = 0;
    ca::usize   keptEntries = 0, removed = 0, totalEntries = 0;
};

/// shadow_generate_file：jar 导入临时容器 + 既有 sidecar 幂等回填 +
/// 机械占位名补号后写 sidecar（缺省 `<jar>.mjt-shadow`，file 显式覆盖但
/// 不得指向 jar 本身）。只写 sidecar，永不改 jar。
ShadowSidecarReport facadeShadowGenerateFile(const std::string& jarPath,
                                             const std::string& file);

/// shadow_prune_file：清 sidecar 里 jar 中已不存在的条目；sidecar 缺失报错；
/// removed == 0 不回写文件。
ShadowSidecarReport facadeShadowPruneFile(const std::string& jarPath,
                                          const std::string& file);

}   // namespace mjt::pysdk
