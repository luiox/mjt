#pragma once

#include "libca/core/any.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "jbytecode/core/type.hpp"
#include "jbytecode/jbytecode.hpp"
#include "jbytecode/tree/insn_list.hpp"
#include "mbase/asm/analyzer/constant_payload.hpp"
#include "mbase/asm/insn/insn_util.hpp"

namespace mjt::base {

class OriginalSourceValue;

///@brief Constant payload carried by OriginalSourceValue.
///
///This wrapper intentionally lives separately from the generic
///mbase::ConstantValue lattice used by ConstantInterpreter. Original-source
///analysis needs a small immutable payload that can survive copy-source chain
///snapshots without pulling in type-lattice state. Supported payloads mirror
///bytecode constants used by jump prediction and deobfuscation heuristics:
///int, int64_t, float, double and std::string.
struct OriginalSourceConstantValue
{
    ca::core::Any value_;

    static OriginalSourceConstantValue of(ca::core::Any value)
    {
        return OriginalSourceConstantValue{std::move(value)};
    }

    const ca::core::Any& get() const { return value_; }

    const ca::core::Any& getValue() const { return value_; }

    bool operator==(const OriginalSourceConstantValue& other) const
    {
        return constantAnyEqual(value_, other.value_);
    }

    bool operator!=(const OriginalSourceConstantValue& other) const { return !(*this == other); }
};

///@brief SourceInterpreter value that remembers the original producer chain.
///
///SourceValue only records the current producer instruction set. Many
///deobfuscation optimizers also need to know whether a value was loaded from a
///method parameter, copied through locals/stack shuffles, or derived from a
///known constant. OriginalSourceValue adds:
///
///- a stable copied-from snapshot chain for loads, stores and dup-like copies;
///- a canonical originalSource_ pointer inside that owned chain;
///- an optional constant payload propagated through recognized operations;
///- an isMethodParameter_ marker and parameterLocal_ index for entry-frame locals.
///
///Values are copied by value by Frame and Analyzer. copyOperation owns a
///snapshot of the copied value, while subsequent OriginalSourceValue copies
///share that already-owned parent chain and normalize originalSource_. This
///keeps copiedFrom_ away from temporary frame slots without making every frame
///copy duplicate the full provenance history.
class OriginalSourceValue : public jbytecode::analysis::SourceValue
{
public:
    std::shared_ptr<OriginalSourceValue>       copiedFrom_;
    OriginalSourceValue*                       originalSource_ = this;
    std::optional<OriginalSourceConstantValue> constantValue_;
    bool                                       isMethodParameter_ = false;
    int                                        parameterLocal_    = -1;

    OriginalSourceValue() = default;

    OriginalSourceValue(const OriginalSourceValue& other)
        : jbytecode::analysis::SourceValue(other.size_, other.insns_)
        , copiedFrom_(other.copiedFrom_)
        , constantValue_(other.constantValue_)
        , isMethodParameter_(other.isMethodParameter_)
        , parameterLocal_(other.parameterLocal_)
    {
        normalizeOriginalSource();
    }

    OriginalSourceValue(OriginalSourceValue&& other) noexcept
        : jbytecode::analysis::SourceValue(other.size_, other.insns_)
        , copiedFrom_(std::move(other.copiedFrom_))
        , constantValue_(std::move(other.constantValue_))
        , isMethodParameter_(other.isMethodParameter_)
        , parameterLocal_(other.parameterLocal_)
    {
        normalizeOriginalSource();
    }

    OriginalSourceValue& operator=(const OriginalSourceValue& other)
    {
        if (this == &other) {
            return *this;
        }
        size_              = other.size_;
        insns_             = other.insns_;
        copiedFrom_        = other.copiedFrom_;
        constantValue_     = other.constantValue_;
        isMethodParameter_ = other.isMethodParameter_;
        parameterLocal_    = other.parameterLocal_;
        normalizeOriginalSource();
        return *this;
    }

    OriginalSourceValue& operator=(OriginalSourceValue&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }
        size_              = other.size_;
        insns_             = other.insns_;
        copiedFrom_        = std::move(other.copiedFrom_);
        constantValue_     = std::move(other.constantValue_);
        isMethodParameter_ = other.isMethodParameter_;
        parameterLocal_    = other.parameterLocal_;
        normalizeOriginalSource();
        return *this;
    }

    OriginalSourceValue(int size, bool isMethodParameter, int parameterLocal = -1)
        : jbytecode::analysis::SourceValue(
              size, jbytecode::analysis::SmallSet<const jbytecode::tree::AbstractInsnNode*>())
        , copiedFrom_(nullptr)
        , isMethodParameter_(isMethodParameter)
        , parameterLocal_(isMethodParameter ? parameterLocal : -1)
    {
        normalizeOriginalSource();
    }

    OriginalSourceValue(int size, const jbytecode::tree::AbstractInsnNode* insnNode)
        : OriginalSourceValue(size, insnNode, nullptr, std::nullopt)
    {}

    OriginalSourceValue(
        int                                                                            size,
        const jbytecode::analysis::SmallSet<const jbytecode::tree::AbstractInsnNode*>& insnSet)
        : jbytecode::analysis::SourceValue(size, insnSet)
        , copiedFrom_(nullptr)
        , isMethodParameter_(false)
    {
        normalizeOriginalSource();
    }

    OriginalSourceValue(
        int                                                                            size,
        const jbytecode::analysis::SmallSet<const jbytecode::tree::AbstractInsnNode*>& insnSet,
        std::shared_ptr<OriginalSourceValue>                                           copiedFrom,
        std::optional<OriginalSourceConstantValue> constantValue)
        : jbytecode::analysis::SourceValue(size, insnSet)
        , copiedFrom_(std::move(copiedFrom))
        , constantValue_(std::move(constantValue))
        , isMethodParameter_(false)
    {
        normalizeOriginalSource();
    }

    OriginalSourceValue(const jbytecode::tree::AbstractInsnNode* insnNode,
                        std::shared_ptr<OriginalSourceValue>     copiedFrom)
        : OriginalSourceValue(copiedFrom ? copiedFrom->size_ : 1, insnNode, std::move(copiedFrom),
                              std::nullopt)
    {}

    OriginalSourceValue(int size, const jbytecode::tree::AbstractInsnNode* insn,
                        std::shared_ptr<OriginalSourceValue>       copiedFrom,
                        std::optional<OriginalSourceConstantValue> constantValue)
        : jbytecode::analysis::SourceValue(size, insn)
        , copiedFrom_(std::move(copiedFrom))
        , isMethodParameter_(false)
    {
        if (constantValue) {
            constantValue_ = std::move(constantValue);
        }
        else if (copiedFrom_) {
            constantValue_ = copiedFrom_->constantValue_;
        }
        else {
            constantValue_ = readInsnConstant(insn);
        }
        normalizeOriginalSource();
    }

    bool isOneWayProduced() const { return insns_.size() == 1; }

    const jbytecode::tree::AbstractInsnNode* getProducer() const
    {
        if (insns_.size() != 1) {
            throw std::logic_error("Expected only one instruction, but got " +
                                   std::to_string(insns_.size()));
        }
        return insns_[0];
    }

    OriginalSourceConstantValue* getConstantValue()
    {
        return constantValue_ ? &(*constantValue_) : nullptr;
    }

    const OriginalSourceConstantValue* getConstantValue() const
    {
        return constantValue_ ? &(*constantValue_) : nullptr;
    }

    bool isMethodParameter() const { return isMethodParameter_; }

    int getParameterLocal() const { return parameterLocal_; }

    OriginalSourceValue* walkToLastParentValue(std::function<bool(OriginalSourceValue*)> until)
    {
        OriginalSourceValue* value = this;
        while (value->copiedFrom_) {
            if (until(value)) {
                break;
            }
            value = value->copiedFrom_.get();
        }
        return value;
    }

    bool equals(const OriginalSourceValue& other) const
    {
        if (this == &other) {
            return true;
        }
        if (size_ != other.size_) {
            return false;
        }
        if (isMethodParameter_ != other.isMethodParameter_) {
            return false;
        }
        if (parameterLocal_ != other.parameterLocal_) {
            return false;
        }
        if (insns_ != other.insns_) {
            return false;
        }
        if (!copiedFromEquals(copiedFrom_, other.copiedFrom_)) {
            return false;
        }
        if (constantValue_.has_value() != other.constantValue_.has_value()) {
            return false;
        }
        if (constantValue_ && *constantValue_ != *other.constantValue_) {
            return false;
        }
        return true;
    }

    bool operator==(const OriginalSourceValue& other) const { return equals(other); }

    bool operator!=(const OriginalSourceValue& other) const { return !equals(other); }

private:
    static bool copiedFromEquals(const std::shared_ptr<OriginalSourceValue>& lhs,
                                 const std::shared_ptr<OriginalSourceValue>& rhs)
    {
        if (lhs == rhs) {
            return true;
        }
        if (!lhs || !rhs) {
            return false;
        }
        return *lhs == *rhs;
    }

    static std::optional<OriginalSourceConstantValue> readInsnConstant(
        const jbytecode::tree::AbstractInsnNode* insn)
    {
        if (!insn) {
            return std::nullopt;
        }

        auto* mutableInsn = const_cast<jbytecode::tree::AbstractInsnNode*>(insn);
        int   ival;
        if (InsnUtil::getIntValue(mutableInsn, ival)) {
            return OriginalSourceConstantValue::of(ival);
        }
        std::int64_t lval;
        if (InsnUtil::getLongValue(mutableInsn, lval)) {
            return OriginalSourceConstantValue::of(lval);
        }
        float fval;
        if (InsnUtil::getFloatValue(mutableInsn, fval)) {
            return OriginalSourceConstantValue::of(fval);
        }
        double dval;
        if (InsnUtil::getDoubleValue(mutableInsn, dval)) {
            return OriginalSourceConstantValue::of(dval);
        }
        std::string sval;
        if (InsnUtil::getStringValue(mutableInsn, sval)) {
            return OriginalSourceConstantValue::of(sval);
        }
        return std::nullopt;
    }

    void normalizeOriginalSource()
    {
        originalSource_ =
            (!copiedFrom_ || copiedFrom_->isMethodParameter_) ? this : copiedFrom_->originalSource_;
    }
};

}   //namespace mjt::base
