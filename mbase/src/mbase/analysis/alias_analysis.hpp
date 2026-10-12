#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "libca/core/datatype.hpp"
#include "mbase/analysis/andersen_points_to.hpp"
#include "mbase/reference/reference_index.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace jbytecode {
namespace tree {
class ClassNode;
}
}   //namespace jbytecode::tree

namespace mjt::base {

///@brief Conservative points-to value kind shared by data-flow consumers.
///
///Unknown is deliberately a first-class value. Consumers may only turn an
///alias into a concrete target when the value is known and stable across every
///predecessor, including exceptional predecessors.
enum class AliasValueKind
{
    Unknown,
    Null,
    Object,
    Array,
    Container,
    Primitive
};

///@brief 进程级别名身份驻留表（P3 字符串内码）。
///
///alias 引擎内 identity/pointsTo 一律存驻留串指针：内容相同 ⇔ 指针相同，
///拷贝 = 8B 指针、等值比较 = 指针相等。mjt CLI 的 alias 路径单线程，
///表无锁；指针地址稳定（unordered_map 节点键）。
inline const std::string* aliasInterned(const std::string& text)
{
    if (text.empty()) {
        static const std::string empty;
        return &empty;
    }
    static std::unordered_map<std::string, char> table;
    const auto [it, inserted] = table.try_emplace(text, 0);
    (void)inserted;
    return &it->first;
}

struct AliasValue
{
    AliasValueKind                  kind = AliasValueKind::Unknown;
    //默认即驻留空串（P2 语义 identity=""），永不为 null——消费方解引用安全。
    const std::string*              identity = aliasInterned({});
    std::vector<const std::string*> pointsTo;             //驻留指针，按被指串字典序去重

    static AliasValue unknown()
    {
        AliasValue value;
        value.identity = aliasInterned({});
        return value;
    }
    static AliasValue nullValue()
    {
        AliasValue value;
        value.kind     = AliasValueKind::Null;
        value.identity = aliasInterned({});
        return value;
    }
    static AliasValue object(const std::string& id)
    {
        AliasValue value;
        value.kind     = AliasValueKind::Object;
        value.identity = aliasInterned(id);
        value.pointsTo.push_back(value.identity);
        return value;
    }
    static AliasValue array(const std::string& id)
    {
        AliasValue value;
        value.kind     = AliasValueKind::Array;
        value.identity = aliasInterned(id);
        value.pointsTo.push_back(value.identity);
        return value;
    }
    static AliasValue container(const std::string& id)
    {
        AliasValue value;
        value.kind     = AliasValueKind::Container;
        value.identity = aliasInterned(id);
        value.pointsTo.push_back(value.identity);
        return value;
    }
    static AliasValue primitive(const std::string& id)
    {
        AliasValue value;
        value.kind     = AliasValueKind::Primitive;
        value.identity = aliasInterned(id);
        return value;
    }

    bool isKnown() const { return kind != AliasValueKind::Unknown; }
    bool operator==(const AliasValue& other) const
    {
        //驻留保证：指针相等 ⇔ 内容相等。
        return kind == other.kind && identity == other.identity;
    }
    bool operator!=(const AliasValue& other) const { return !(*this == other); }
};

///@brief Merge two predecessor values.
///
///Equal values survive. Any disagreement, including Unknown, becomes
///Unknown; this prevents reflection/remap consumers from guessing across
///branches. The operation is commutative and idempotent.
inline AliasValue mergeAliasValues(const AliasValue& lhs, const AliasValue& rhs)
{
    if (lhs.isKnown() && rhs.isKnown() && lhs == rhs) return lhs;
    AliasValue result = AliasValue::unknown();
    result.pointsTo   = lhs.pointsTo;
    result.pointsTo.insert(result.pointsTo.end(), rhs.pointsTo.begin(), rhs.pointsTo.end());
    std::sort(result.pointsTo.begin(), result.pointsTo.end(),
              [](const std::string* a, const std::string* b) { return *a < *b; });
    result.pointsTo.erase(std::unique(result.pointsTo.begin(), result.pointsTo.end()),
                          result.pointsTo.end());
    return result;
}

///@brief Method-local alias frame keyed by JVM local slot.
struct AliasFrame
{
    std::unordered_map<ca::i32, AliasValue> locals;
    bool                                    hasExceptionalPredecessor = false;

    AliasValue local(ca::i32 index) const
    {
        auto it = locals.find(index);
        return it == locals.end() ? AliasValue::unknown() : it->second;
    }
};

///@brief Merge normal and exceptional predecessor frames conservatively.
inline AliasFrame mergeAliasFrames(const AliasFrame& lhs, const AliasFrame& rhs)
{
    AliasFrame result;
    result.hasExceptionalPredecessor =
        lhs.hasExceptionalPredecessor || rhs.hasExceptionalPredecessor;
    result.locals = lhs.locals;
    for (const auto& [slot, value] : rhs.locals) {
        auto it = result.locals.find(slot);
        if (it == result.locals.end()) {
            result.locals.emplace(slot, AliasValue::unknown());
        }
        else {
            it->second = mergeAliasValues(it->second, value);
        }
    }
    //A slot present only on the left is also an unknown incoming value from
    //the right predecessor. Keep the merge commutative and fail closed.
    for (const auto& [slot, value] : lhs.locals) {
        if (rhs.locals.find(slot) == rhs.locals.end()) {
            result.locals[slot] = AliasValue::unknown();
        }
    }
    return result;
}

///@brief Stable interprocedural summary consumed by reflection/remap users.
///
///P2（perf/alias-closure）：摘要只持有**自身指令事实**（own facts）；跨方法
///事实经 CallBinding（callee 引用 + 调用点实参绑定）惰性展开。此前 callee
///事实逐轮内联复制进调用方摘要，环路上每轮翻倍（minzi 实测 2^k 发散）且
///摘要体积与调用链深度耦合；引用化后摘要体积只随自身指令数增长，展开流
///与内联物化版在收敛点逐字段等值（AliasAnalysisWorklistTest/
///lazyExpansionReproducesMaterializedFacts 钉死）。
struct AliasMethodSummary
{
    struct FieldWrite
    {
        FieldRefKey field;
        AliasValue  receiver;
        AliasValue  value;
        bool        isStatic = false;
    };

    struct ContainerWrite
    {
        AliasValue container;
        AliasValue key;
        AliasValue value;
    };

    struct ArrayWrite
    {
        AliasValue array;
        AliasValue index;
        AliasValue value;
    };

    struct ReflectionReceiver
    {
        ca::i32    instructionIndex = -1;
        AliasValue receiver;
    };

    ///@brief 调用点绑定：callee 摘要引用（按方法键）+ 调用点实参。
    ///
    ///展开语义与 P1 内联逐组对应：
    ///- fieldWrites 组用 fwReceiver/fwArguments 实例化（控制流方法由
    ///analyzeControlFlowFacts 以 CFG 帧值重写，与 P1 流路径同源）；
    ///- arrayWrites/containerWrites 组恒用 receiver/arguments（线性栈值，
    ///与 P1 主走查同源）；
    ///- 各组活性开关钉死 P1 的逐组替换/失效行为（flow 路径只替换 FW 组；
    ///snapshot 失败只失效 FW 组）。
    struct CallBinding
    {
        std::string             calleeKey;
        ca::i32                 instructionIndex = -1;
        AliasValue              receiver;
        std::vector<AliasValue> arguments;
        AliasValue              fwReceiver;
        std::vector<AliasValue> fwArguments;
        bool                    fieldWritesActive     = true;
        bool                    arrayWritesActive     = true;
        bool                    containerWritesActive = true;
    };

    MethodRefKey                    method;
    std::vector<AliasValue>         parameters;
    AliasValue                      returnValue;
    std::vector<FieldWrite>         fieldWrites;
    std::vector<ArrayWrite>         arrayWrites;
    std::vector<ContainerWrite>     containerWrites;
    std::vector<ReflectionReceiver> reflectionReceivers;
    std::vector<CallBinding>        callBindings;
    bool                            hasExceptionalExit = false;
};

///@brief 展开 callee 绑定后的等价事实流（P1 物化语义的等价视图）。
///
///诊断/测试专用：expandFieldWrites 等三组与 P1 内联物化摘要的对应组在
///收敛点逐字段等值。别名事实的运行时消费面（fieldValueOf / summaryOf /
///reflectionReceiverOf 等索引查询）不经此路径。
struct AliasExpandedFacts
{
    std::vector<AliasMethodSummary::FieldWrite>     fieldWrites;
    std::vector<AliasMethodSummary::ArrayWrite>     arrayWrites;
    std::vector<AliasMethodSummary::ContainerWrite> containerWrites;
};

struct AliasFieldSummary
{
    FieldRefKey field;
    AliasValue  receiver;
    AliasValue  value;
};

///@brief Shared alias-analysis contract.
///
///The analysis owns the value/merge contract and computes stable summaries to
///a whole-program fixed point. Branching methods are evaluated with the shared
///Analyzer so local and operand-stack values merge at real CFG joins;
///disagreement remains Unknown while retaining diagnostic points-to targets.
///Higher-level analyses must use these values instead of defining a private
///reflection-specific alias lattice.
class AliasAnalysis
{
public:
    void build(ResourceContainer& container, ca::str::Utf8StringArena& arena);

    ///@brief Reflection-closure scoped build（rename --apply 的 alias 段专用）。
    ///
    ///apply 路径的 alias 消费面只有反射重映射（applyMemberRemapPlan 的
    ///reflectionReceiverOf 富集 + ReflectionInterpreter 的 summaryOf /
    ///fieldValueOf 查询）。据此把摘要集合收缩到反射闭包，不做全罐固定点：
    ///R1 含 java/lang/Class 反射查找调用点（getMethod/getDeclaredMethod/
    ///getConstructor/getDeclaredConstructor/getField/getDeclaredField）的类；
    ///R2 R1∪R3∪R4 的容器内传递 callee 闭包（保 in-scope 摘要与全罐
    ///build 逐字段一致——callee 事实内联/返回值实例化的依赖方向）；
    ///R3 含 Class 形（Ljava/lang/Class;）静态字段读写（PUTSTATIC/GETSTATIC）
    ///指令的类——静态槽 fieldKey(unknown, field) 按 field 全局聚合；
    ///R4 声明 Class 形静态字段的类（静态类常量缓存的典型声明点）。
    ///作用域外的类不产出摘要（summaryOf 返回 nullptr，消费端等价于"无
    ///callee 事实"的既有 fail-open 语义）。convergenceRounds 只反映
    ///本闭包固定点的轮数。
    void buildReflectionClosure(ResourceContainer& container, ca::str::Utf8StringArena& arena);

    void clear();

    const AliasMethodSummary* summaryOf(const std::string& owner, const std::string& name,
                                        const std::string& desc) const;

    ///@brief Return the shared alias fact for a reflection lookup receiver.
    const AliasValue* reflectionReceiverOf(const std::string& owner, const std::string& name,
                                           const std::string& desc, ca::i32 instructionIndex) const;

    const std::vector<AliasMethodSummary>& summaries() const { return summaries_; }

    const AliasValue* fieldValueOf(const std::string& owner, const std::string& name,
                                   const std::string& desc) const;

    const AliasValue* fieldValueOf(const AliasValue& receiver, const std::string& owner,
                                   const std::string& name, const std::string& desc) const;

    ///@brief Resolve a field read across every receiver allocation target.
    ///
    ///A receiver that is Unknown may still carry diagnostic points-to targets
    ///from a CFG merge. Each target is queried independently. Conflicting
    ///field values remain Unknown while their points-to sets are retained;
    ///callers must not treat that result as a unique concrete alias.
    std::optional<AliasValue> resolvedFieldValueOf(const AliasValue&  receiver,
                                                   const std::string& owner,
                                                   const std::string& name,
                                                   const std::string& desc) const;

    const AliasValue* containerValueOf(const std::string& identity) const;
    const AliasValue* containerValueOf(const AliasValue& container, const AliasValue& key) const;

    ///@brief Resolve an array element by concrete or wildcard index.
    const AliasValue* arrayValueOf(const AliasValue& array, const AliasValue& index) const;

    ///@brief Resolve an array element across diagnostic points-to targets.
    std::optional<AliasValue> resolvedArrayValueOf(const AliasValue& array,
                                                   const AliasValue& index) const;

    ///@brief Return the concrete allocation type represented by a known value.
    std::vector<std::string> possibleTypesOf(const AliasValue& value) const;

    std::vector<std::string> possibleReturnTypesOf(const std::string& owner,
                                                   const std::string& name,
                                                   const std::string& desc) const;

    ///@brief Return Andersen allocation sites reaching a method's return slot.
    std::vector<std::string> andersenReturnPointsToOf(const std::string& owner,
                                                      const std::string& name,
                                                      const std::string& desc) const;

    ///@brief Return Andersen allocation sites reaching one method parameter.
    std::vector<std::string> andersenParameterPointsToOf(const std::string& owner,
                                                         const std::string& name,
                                                         const std::string& desc,
                                                         ca::usize          parameter) const;

    ///@brief 展开调用绑定后的等价事实流（诊断/测试专用，语义见 AliasExpandedFacts）。
    AliasExpandedFacts expandedFactsOf(const std::string& owner, const std::string& name,
                                       const std::string& desc) const;

    ///@brief 展开 method 摘要的指定事实组（P2 惰性实例化核心）。
    ///
    ///递归拼接「自身事实 + 逐绑定实例化的 callee 展开流」；环路上每个方法
    ///只展开一次（重入时只贡献 callee 自身事实并按当前绑定实例化）——收敛
    ///点与 P1 内联物化流等值（环上回流的重复事实已在既有位置）。运行时
    ///固定点与 summarize 的局部 map 馈给亦经此路径。
    void expandFieldWrites(const AliasMethodSummary&                          summary,
                           std::vector<ca::usize>&                            visiting,
                           std::vector<AliasMethodSummary::FieldWrite>&       out) const;
    void expandArrayWrites(const AliasMethodSummary&                          summary,
                           std::vector<ca::usize>&                            visiting,
                           std::vector<AliasMethodSummary::ArrayWrite>&       out) const;
    void expandContainerWrites(const AliasMethodSummary&                          summary,
                               std::vector<ca::usize>&                            visiting,
                               std::vector<AliasMethodSummary::ContainerWrite>&   out) const;

    ///@brief Number of whole-program rounds used to reach the last fixed point.
    ca::usize convergenceRounds() const { return convergenceRounds_; }

    void addSummary(AliasMethodSummary summary);

    ///@brief 绑定键 → 摘要槽位（解析域 = expansionSource_ 优先，缺省自身）。
    const AliasMethodSummary* resolveBinding(const std::string& calleeKey,
                                             ca::usize*         slotIndex) const;

private:
    static std::string key(const MethodRefKey& method);
    void               indexAndersenSummary(const AliasMethodSummary& summary);

    ///@brief 共享固定点核心（build 与 buildReflectionClosure 的公共实现）。
    ///
    ///P1（perf/alias-closure）：memoized Jacobi——每轮仅重算「记录输入」发生
    ///变化的方法。对每个方法在 summarize 时记录其经 previous 查询的全部
    ///摘要键与槽键；某键自上次重算后发生过内容变更（deferred 记账，保
    ///Jacobi 语义——轮内只看前一轮快照）才重算，否则逐字节复用上一轮
    ///摘要。轮数与全量重算版恒等（S_k 序列逐方法等值的归纳性质），
    ///O(方法数²) 全量重算消失；sameSummaries 全量深比较由逐方法比较 +
    ///槽键记账（增量化）替代。
    void buildFixedPoint(const std::vector<const jbytecode::tree::ClassNode*>& scope,
                         ca::usize                                             roundBound);

    ///@brief 按记账键取槽值（buildFixedPoint 的轮末变更过滤专用）。
    ///
    ///nsKey 形如 "F|A|C|<exact slot key>"（与查询方法记录的键完全同构）；
    ///摘要键（"M\n…" 前缀）不经过本函数（重算时直接 sameSummary 比较）。
    const AliasValue* findRecordedSlot(const std::string& nsKey) const;

    std::vector<AliasMethodSummary>             summaries_;
    std::unordered_map<std::string, ca::usize>  byKey_;
    std::vector<AliasFieldSummary>              fields_;
    std::unordered_map<std::string, ca::usize>  fieldsByKey_;
    std::unordered_map<std::string, AliasValue> arrayValues_;
    std::unordered_map<std::string, AliasValue> containerValues_;
    AndersenPointsToSolver                      andersen_;
    ca::usize                                   convergenceRounds_ = 0;

    //── P1 memoized Jacobi 的构建期状态（仅 buildFixedPoint 期间非默认）──
    //查询记录：summarize 经 previous 的 const 查询方法收集输入键；
    //槽变更候选上报：addSummary 把本轮触碰的精确槽键交给驱动过滤。
    mutable std::vector<std::string>                queryLog_;
    mutable bool                                    recordingQueries_ = false;
    std::unordered_set<std::string>*                slotChangeSink_   = nullptr;
    bool                                            suspendAndersenIndexing_ = false;

    //── P2 引用化摘要的构建期状态 ──
    //绑定键解析域：固定点期间 addSummary/summarize 的展开一律读前一轮快照
    //（Jacobi 轮内只读上一轮），由驱动把它指到 current；终态=自身。
    const AliasAnalysis* expansionSource_ = nullptr;

};

struct AliasAnalysisResult
{
    using Result = AliasAnalysis;

    Result run(ResourceContainer& container, GlobalAnalysisManager& gam);
};

}   //namespace mjt::base
