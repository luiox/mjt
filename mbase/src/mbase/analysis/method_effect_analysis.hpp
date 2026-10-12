#pragma once

///@defgroup method_effect_analysis 方法副作用分析模块
///
///## 目的
///
///`MethodEffectAnalysis` 是 P3.3 共享查询层，用于纯函数和未使用结果优化。
///它为全局通行证提供一个保守的答案，判断方法是 `Pure`、`ReadOnly`、`Impure` 还是 `Unknown`，
///而不是每个通行证携带私有字节码白名单。
///
///## 模型
///
///分析扫描项目类资源，为每个声明方法记录一个摘要。局部字节码扫描首先对直接效应进行分类：
///
///- 常量、加载、存储到局部变量、算术操作、分支、开关、类型转换和返回为 `Pure`；
///- 字段读取和 `arraylength` 将方法提升为 `ReadOnly`；
///- 堆写入、数组存储、分配、显式抛出、监视器和 invokedynamic 为 `Impure`；
///- native、抽象、构造器、类初始化器、解析缺失和未解析方法为 `Unknown`。
///
///局部扫描后，通过对直接方法调用的不动点传播效果。对声明方法的调用使用其当前摘要。
///对资源集之外的调用回退到小型 JDK 白名单（当前为 `java/lang/Math`、`java/lang/StrictMath`
///和少量只读 `java/lang/String` 查询）。其他所有外部调用保持为 `Unknown`。
///
///## 消费者规则
///
///此分析仅回答堆副作用问题。它不证明可以在不改变异常、空检查或类初始化时机的情况下
///删除调用。破坏性通行证如 `UnusedResultCallEliminater`、`RedundantInvokeRemover`
///和未来的内联器必须将摘要与指令形状检查以及它们自己的异常/类初始化策略结合使用。
///
///初始消费者是 `RedundantInvokeRemover`：它仅对其 `invoke; pop/pop2` 形状扫描已选择
///的项目局部方法使用共享模型，并且仅接受 `Pure` 方法。`ReadOnly` 方法对后续通行证
///仍然可见，但不会在此首次发布中被移除。
///
///`UnusedResultCallEliminater` 有意更窄。它是方法通行证，因此不消费全局摘要。
///它仅移除硬编码无异常 JDK 调用的相邻常量生产者，将项目局部纯调用留给
///`RedundantInvokeRemover`。

#include <string>
#include <unordered_map>
#include <vector>
#include <cmath>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/reference/reference_index.hpp"
#include "mbase/spi/pass_pipeline.hpp"

namespace mjt::base {

///@brief Conservative side-effect class for a JVM method body.
///
///The lattice is intentionally small because P3 consumers need a safe shared
///predicate before doing destructive optimizations:
///
///- Pure: only local stack/local arithmetic, constants, branches, and calls to
///methods also proven Pure.
///- ReadOnly: may read fields or call ReadOnly methods, but does not write
///heap state.
///- Impure: definitely writes heap state, allocates, synchronizes, throws, or
///performs another effectful bytecode operation.
///- Unknown: body or callee is unavailable, unresolved, native, abstract, or
///outside the current conservative model.
///
///Exception behavior and class-initialization effects are not erased by this
///classification. Passes that delete calls must still apply their own
///instruction-shape and null/class-init safety policy.
enum class MethodEffectKind
{
    Pure,
    ReadOnly,
    Impure,
    Unknown
};

enum class ConstantReturnKind
{
    Unknown,
    Int,
    Long,
    Float,
    Double,
    String,
    Null
};

struct ConstantReturnValue
{
    ConstantReturnKind kind          = ConstantReturnKind::Unknown;
    ca::i64            intValue      = 0;
    double             floatingValue = 0.0;
    std::string        stringValue;

    bool isKnown() const { return kind != ConstantReturnKind::Unknown; }
    bool operator==(const ConstantReturnValue& other) const
    {
        if (kind != other.kind) return false;
        switch (kind) {
        case ConstantReturnKind::Int:
        case ConstantReturnKind::Long: return intValue == other.intValue;
        case ConstantReturnKind::Float:
        case ConstantReturnKind::Double:
            return floatingValue == other.floatingValue ||
                   (std::isnan(floatingValue) && std::isnan(other.floatingValue));
        case ConstantReturnKind::String: return stringValue == other.stringValue;
        case ConstantReturnKind::Unknown:
        case ConstantReturnKind::Null: return true;
        }
        return false;
    }
};

///@brief Stable effect summary for one declared project method.
struct MethodEffectSummary
{
    MethodRefKey        method;
    MethodEffectKind    kind     = MethodEffectKind::Unknown;
    bool                declared = false;
    bool                hasBody  = false;
    ConstantReturnValue constantReturn;
};

///@brief Return a stable display name for diagnostics and tests.
const char* methodEffectKindName(MethodEffectKind kind);

///@brief Whole-program method side-effect model.
///
///MethodEffectModel parses class resources, assigns a local bytecode effect to
///every declared method, and then runs a fixed-point propagation over direct
///method calls. The build is conservative: parse failures are skipped,
///unresolved calls become Unknown unless covered by the tiny JDK whitelist, and
///recursive components only become Pure/ReadOnly when every member body and
///outgoing call supports that result.
class MethodEffectModel
{
public:
    ///@brief Rebuild summaries from all class resources in a container.
    ///
    ///Existing results are discarded. The arena is used by ClassReader while
    ///decoding class trees; public query results own std::string copies and do
    ///not depend on temporary ClassNode lifetime.
    void build(ResourceContainer& container, ca::str::Utf8StringArena& arena);

    ///@brief Remove all summaries and lookup tables.
    void clear();

    ///@brief Return a method summary by exact owner/name/descriptor.
    const MethodEffectSummary* effectOf(const std::string& owner, const std::string& name,
                                        const std::string& desc) const;

    ///@brief Return the effect kind for a method, or Unknown when absent.
    MethodEffectKind effectKindOf(const std::string& owner, const std::string& name,
                                  const std::string& desc) const;

    ///@brief Return true when the method is known Pure.
    bool isPure(const std::string& owner, const std::string& name, const std::string& desc) const;

    ///@brief Return true when the method is known Pure or ReadOnly.
    bool isReadOnlyOrPure(const std::string& owner, const std::string& name,
                          const std::string& desc) const;

    ///@brief Return a proven integer constant from a pure direct/wrapper method.
    ConstantReturnValue constantReturnOf(const std::string& owner, const std::string& name,
                                         const std::string& desc) const;

    ///@brief Return every summary in deterministic scan order.
    const std::vector<MethodEffectSummary>& summaries() const { return summaries_; }

    ///@brief Number of method summaries currently held.
    ca::usize size() const { return summaries_.size(); }

private:
    void appendSummary(const MethodEffectSummary& summary);

    std::vector<MethodEffectSummary>           summaries_;
    std::unordered_map<std::string, ca::usize> summariesByKey_;
};

///@brief Global analysis wrapper for MethodEffectModel.
struct MethodEffectAnalysis
{
    using Result = MethodEffectModel;

    Result run(ResourceContainer& container, GlobalAnalysisManager& gam);
};

}   //namespace mjt::base
