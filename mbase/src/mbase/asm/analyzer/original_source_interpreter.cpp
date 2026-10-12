#include "original_source_interpreter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>

#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/core/type.hpp"
#include "jbytecode/tree/insn_type_traits.hpp"
#include "garena/arena.hpp"
#include "mbase/asm/matcher/match_step.hpp"
#include "mbase/asm/matcher/pattern_matcher.hpp"
#include "simple_interpreter.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

namespace {

using jbytecode::core::Opcodes;
using jbytecode::core::Type;
using jbytecode::tree::AbstractInsnNode;

template<typename T>
const T* constantAs(const OriginalSourceValue& value)
{
    const auto* constant = value.getConstantValue();
    return constant ? constant->getValue().as<T>() : nullptr;
}

std::int32_t addI(std::int32_t a, std::int32_t b)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

std::int32_t subI(std::int32_t a, std::int32_t b)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) - static_cast<std::uint32_t>(b));
}

std::int32_t mulI(std::int32_t a, std::int32_t b)
{
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}

std::int64_t addL(std::int64_t a, std::int64_t b)
{
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
}

std::int64_t subL(std::int64_t a, std::int64_t b)
{
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
}

std::int64_t mulL(std::int64_t a, std::int64_t b)
{
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(a) * static_cast<std::uint64_t>(b));
}

std::int32_t divI(std::int32_t a, std::int32_t b)
{
    if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
        return a;
    }
    return a / b;
}

std::int64_t divL(std::int64_t a, std::int64_t b)
{
    if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
        return a;
    }
    return a / b;
}

std::int32_t remI(std::int32_t a, std::int32_t b)
{
    if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
        return 0;
    }
    return a % b;
}

std::int64_t remL(std::int64_t a, std::int64_t b)
{
    if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
        return 0;
    }
    return a % b;
}

std::int32_t floatToInt(float value)
{
    if (std::isnan(value)) {
        return 0;
    }
    if (value >= static_cast<float>(std::numeric_limits<std::int32_t>::max())) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (value <= static_cast<float>(std::numeric_limits<std::int32_t>::min())) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(value);
}

std::int64_t floatToLong(float value)
{
    if (std::isnan(value)) {
        return 0;
    }
    if (value >= static_cast<float>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value <= static_cast<float>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

std::int32_t doubleToInt(double value)
{
    if (std::isnan(value)) {
        return 0;
    }
    if (value >= static_cast<double>(std::numeric_limits<std::int32_t>::max())) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (value <= static_cast<double>(std::numeric_limits<std::int32_t>::min())) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(value);
}

std::int64_t doubleToLong(double value)
{
    if (std::isnan(value)) {
        return 0;
    }
    if (value >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return std::numeric_limits<std::int64_t>::max();
    }
    if (value <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
        return std::numeric_limits<std::int64_t>::min();
    }
    return static_cast<std::int64_t>(value);
}

int valueSizeForNewOperation(const AbstractInsnNode* insn)
{
    switch (insn->getOpcode()) {
    case Opcodes::LCONST_0:
    case Opcodes::LCONST_1:
    case Opcodes::DCONST_0:
    case Opcodes::DCONST_1: return 2;
    case Opcodes::LDC:
    {
        auto*                ldc = ca::core::tag_cast::cast<const jbytecode::tree::LdcInsnNode*>(insn);
        const ca::core::Any& cst = ldc->cst;
        if (cst.as<std::int64_t>() || cst.as<double>()) {
            return 2;
        }
        if (cst.as<jbytecode::core::ConstantDynamic>()) {
            const auto& cd = *cst.as<const jbytecode::core::ConstantDynamic>();
            return Type::getType(cd.getDescriptor()).getSize();
        }
        return 1;
    }
    case Opcodes::GETSTATIC:
    {
        auto* field = ca::core::tag_cast::cast<const jbytecode::tree::FieldInsnNode*>(insn);
        return Type::getType(field->desc).getSize();
    }
    default: return 1;
    }
}

int valueSizeForUnaryOperation(const AbstractInsnNode* insn)
{
    switch (insn->getOpcode()) {
    case Opcodes::LNEG:
    case Opcodes::DNEG:
    case Opcodes::I2L:
    case Opcodes::I2D:
    case Opcodes::L2D:
    case Opcodes::F2L:
    case Opcodes::F2D:
    case Opcodes::D2L: return 2;
    case Opcodes::GETFIELD:
    {
        auto* field = ca::core::tag_cast::cast<const jbytecode::tree::FieldInsnNode*>(insn);
        return Type::getType(field->desc).getSize();
    }
    default: return 1;
    }
}

int valueSizeForBinaryOperation(const AbstractInsnNode* insn)
{
    switch (insn->getOpcode()) {
    case Opcodes::LALOAD:
    case Opcodes::DALOAD:
    case Opcodes::LADD:
    case Opcodes::DADD:
    case Opcodes::LSUB:
    case Opcodes::DSUB:
    case Opcodes::LMUL:
    case Opcodes::DMUL:
    case Opcodes::LDIV:
    case Opcodes::DDIV:
    case Opcodes::LREM:
    case Opcodes::DREM:
    case Opcodes::LSHL:
    case Opcodes::LSHR:
    case Opcodes::LUSHR:
    case Opcodes::LAND:
    case Opcodes::LOR:
    case Opcodes::LXOR: return 2;
    default: return 1;
    }
}

int valueSizeForNaryOperation(const AbstractInsnNode* insn)
{
    int opcode = insn->getOpcode();
    if (opcode == Opcodes::MULTIANEWARRAY) {
        return 1;
    }
    if (opcode == Opcodes::INVOKEDYNAMIC) {
        auto* id = ca::core::tag_cast::cast<const jbytecode::tree::InvokeDynamicInsnNode*>(insn);
        return Type::getReturnType(id->desc).getSize();
    }
    auto* method = ca::core::tag_cast::cast<const jbytecode::tree::MethodInsnNode*>(insn);
    return Type::getReturnType(method->desc).getSize();
}

std::optional<OriginalSourceConstantValue> foldUnaryConstant(const AbstractInsnNode*    insn,
                                                             const OriginalSourceValue& value)
{
    switch (insn->getOpcode()) {
    case Opcodes::IINC:
    {
        auto* iinc = ca::core::tag_cast::cast<const jbytecode::tree::IincInsnNode*>(insn);
        if (auto* p = constantAs<int>(value)) {
            return OriginalSourceConstantValue::of(static_cast<int>(addI(*p, iinc->incr)));
        }
        break;
    }
    case Opcodes::INEG:
        if (auto* p = constantAs<int>(value)) {
            return OriginalSourceConstantValue::of(static_cast<int>(subI(0, *p)));
        }
        break;
    case Opcodes::LNEG:
        if (auto* p = constantAs<std::int64_t>(value)) {
            return OriginalSourceConstantValue::of(subL(0, *p));
        }
        break;
    case Opcodes::FNEG:
        if (auto* p = constantAs<float>(value)) {
            return OriginalSourceConstantValue::of(-*p);
        }
        break;
    case Opcodes::DNEG:
        if (auto* p = constantAs<double>(value)) {
            return OriginalSourceConstantValue::of(-*p);
        }
        break;
    case Opcodes::I2L:
        if (auto* p = constantAs<int>(value)) {
            return OriginalSourceConstantValue::of(static_cast<std::int64_t>(*p));
        }
        break;
    case Opcodes::I2F:
        if (auto* p = constantAs<int>(value)) {
            return OriginalSourceConstantValue::of(static_cast<float>(*p));
        }
        break;
    case Opcodes::I2D:
        if (auto* p = constantAs<int>(value)) {
            return OriginalSourceConstantValue::of(static_cast<double>(*p));
        }
        break;
    case Opcodes::I2B:
        if (auto* p = constantAs<int>(value)) {
            return OriginalSourceConstantValue::of(static_cast<int>(static_cast<std::int8_t>(*p)));
        }
        break;
    case Opcodes::I2C:
        if (auto* p = constantAs<int>(value)) {
            return OriginalSourceConstantValue::of(
                static_cast<int>(static_cast<std::uint16_t>(*p)));
        }
        break;
    case Opcodes::I2S:
        if (auto* p = constantAs<int>(value)) {
            return OriginalSourceConstantValue::of(static_cast<int>(static_cast<std::int16_t>(*p)));
        }
        break;
    case Opcodes::L2I:
        if (auto* p = constantAs<std::int64_t>(value)) {
            return OriginalSourceConstantValue::of(static_cast<int>(static_cast<std::int32_t>(*p)));
        }
        break;
    case Opcodes::L2F:
        if (auto* p = constantAs<std::int64_t>(value)) {
            return OriginalSourceConstantValue::of(static_cast<float>(*p));
        }
        break;
    case Opcodes::L2D:
        if (auto* p = constantAs<std::int64_t>(value)) {
            return OriginalSourceConstantValue::of(static_cast<double>(*p));
        }
        break;
    case Opcodes::F2I:
        if (auto* p = constantAs<float>(value)) {
            return OriginalSourceConstantValue::of(static_cast<int>(floatToInt(*p)));
        }
        break;
    case Opcodes::F2L:
        if (auto* p = constantAs<float>(value)) {
            return OriginalSourceConstantValue::of(floatToLong(*p));
        }
        break;
    case Opcodes::F2D:
        if (auto* p = constantAs<float>(value)) {
            return OriginalSourceConstantValue::of(static_cast<double>(*p));
        }
        break;
    case Opcodes::D2I:
        if (auto* p = constantAs<double>(value)) {
            return OriginalSourceConstantValue::of(static_cast<int>(doubleToInt(*p)));
        }
        break;
    case Opcodes::D2L:
        if (auto* p = constantAs<double>(value)) {
            return OriginalSourceConstantValue::of(doubleToLong(*p));
        }
        break;
    case Opcodes::D2F:
        if (auto* p = constantAs<double>(value)) {
            return OriginalSourceConstantValue::of(static_cast<float>(*p));
        }
        break;
    default: break;
    }
    return std::nullopt;
}

int compareFloat(float a, float b, int opcode)
{
    if (std::isnan(a) || std::isnan(b)) {
        return opcode == Opcodes::FCMPL ? -1 : 1;
    }
    return (a > b) - (a < b);
}

int compareDouble(double a, double b, int opcode)
{
    if (std::isnan(a) || std::isnan(b)) {
        return opcode == Opcodes::DCMPL ? -1 : 1;
    }
    return (a > b) - (a < b);
}

std::optional<OriginalSourceConstantValue> foldBinaryConstant(const AbstractInsnNode*    insn,
                                                              const OriginalSourceValue& value1,
                                                              const OriginalSourceValue& value2)
{
    switch (insn->getOpcode()) {
    case Opcodes::IADD:
        if (auto* a = constantAs<int>(value1))
            if (auto* b = constantAs<int>(value2))
                return OriginalSourceConstantValue::of(static_cast<int>(addI(*a, *b)));
        break;
    case Opcodes::ISUB:
        if (auto* a = constantAs<int>(value1))
            if (auto* b = constantAs<int>(value2))
                return OriginalSourceConstantValue::of(static_cast<int>(subI(*a, *b)));
        break;
    case Opcodes::IMUL:
        if (auto* a = constantAs<int>(value1))
            if (auto* b = constantAs<int>(value2))
                return OriginalSourceConstantValue::of(static_cast<int>(mulI(*a, *b)));
        break;
    case Opcodes::IDIV:
        if (auto* a = constantAs<int>(value1))
            if (auto* b = constantAs<int>(value2)) {
                if (*b != 0) return OriginalSourceConstantValue::of(static_cast<int>(divI(*a, *b)));
            }
        break;
    case Opcodes::IREM:
        if (auto* a = constantAs<int>(value1))
            if (auto* b = constantAs<int>(value2)) {
                if (*b != 0) return OriginalSourceConstantValue::of(static_cast<int>(remI(*a, *b)));
            }
        break;
    case Opcodes::IAND:
        if (auto* a = constantAs<int>(value1))
            if (auto* b = constantAs<int>(value2)) return OriginalSourceConstantValue::of(*a & *b);
        break;
    case Opcodes::IOR:
        if (auto* a = constantAs<int>(value1))
            if (auto* b = constantAs<int>(value2)) return OriginalSourceConstantValue::of(*a | *b);
        break;
    case Opcodes::IXOR:
        if (auto* a = constantAs<int>(value1))
            if (auto* b = constantAs<int>(value2)) return OriginalSourceConstantValue::of(*a ^ *b);
        break;
    case Opcodes::ISHL:
        if (auto* a = constantAs<int>(value1))
            if (auto* b = constantAs<int>(value2))
                return OriginalSourceConstantValue::of(
                    static_cast<int>(static_cast<std::uint32_t>(*a) << (*b & 0x1f)));
        break;
    case Opcodes::ISHR:
        if (auto* a = constantAs<int>(value1))
            if (auto* b = constantAs<int>(value2))
                return OriginalSourceConstantValue::of(
                    static_cast<int>(static_cast<std::int32_t>(*a) >> (*b & 0x1f)));
        break;
    case Opcodes::IUSHR:
        if (auto* a = constantAs<int>(value1))
            if (auto* b = constantAs<int>(value2))
                return OriginalSourceConstantValue::of(
                    static_cast<int>(static_cast<std::uint32_t>(*a) >> (*b & 0x1f)));
        break;
    case Opcodes::LADD:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<std::int64_t>(value2))
                return OriginalSourceConstantValue::of(addL(*a, *b));
        break;
    case Opcodes::LSUB:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<std::int64_t>(value2))
                return OriginalSourceConstantValue::of(subL(*a, *b));
        break;
    case Opcodes::LMUL:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<std::int64_t>(value2))
                return OriginalSourceConstantValue::of(mulL(*a, *b));
        break;
    case Opcodes::LDIV:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<std::int64_t>(value2)) {
                if (*b != 0) return OriginalSourceConstantValue::of(divL(*a, *b));
            }
        break;
    case Opcodes::LREM:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<std::int64_t>(value2)) {
                if (*b != 0) return OriginalSourceConstantValue::of(remL(*a, *b));
            }
        break;
    case Opcodes::LAND:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<std::int64_t>(value2))
                return OriginalSourceConstantValue::of(*a & *b);
        break;
    case Opcodes::LOR:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<std::int64_t>(value2))
                return OriginalSourceConstantValue::of(*a | *b);
        break;
    case Opcodes::LXOR:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<std::int64_t>(value2))
                return OriginalSourceConstantValue::of(*a ^ *b);
        break;
    case Opcodes::LSHL:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<int>(value2))
                return OriginalSourceConstantValue::of(
                    static_cast<std::int64_t>(static_cast<std::uint64_t>(*a) << (*b & 0x3f)));
        break;
    case Opcodes::LSHR:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<int>(value2))
                return OriginalSourceConstantValue::of(
                    static_cast<std::int64_t>(*a >> (*b & 0x3f)));
        break;
    case Opcodes::LUSHR:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<int>(value2))
                return OriginalSourceConstantValue::of(
                    static_cast<std::int64_t>(static_cast<std::uint64_t>(*a) >> (*b & 0x3f)));
        break;
    case Opcodes::FADD:
        if (auto* a = constantAs<float>(value1))
            if (auto* b = constantAs<float>(value2))
                return OriginalSourceConstantValue::of(*a + *b);
        break;
    case Opcodes::FSUB:
        if (auto* a = constantAs<float>(value1))
            if (auto* b = constantAs<float>(value2))
                return OriginalSourceConstantValue::of(*a - *b);
        break;
    case Opcodes::FMUL:
        if (auto* a = constantAs<float>(value1))
            if (auto* b = constantAs<float>(value2))
                return OriginalSourceConstantValue::of(*a * *b);
        break;
    case Opcodes::FDIV:
        if (auto* a = constantAs<float>(value1))
            if (auto* b = constantAs<float>(value2))
                return OriginalSourceConstantValue::of(*a / *b);
        break;
    case Opcodes::FREM:
        if (auto* a = constantAs<float>(value1))
            if (auto* b = constantAs<float>(value2))
                return OriginalSourceConstantValue::of(std::fmod(*a, *b));
        break;
    case Opcodes::DADD:
        if (auto* a = constantAs<double>(value1))
            if (auto* b = constantAs<double>(value2))
                return OriginalSourceConstantValue::of(*a + *b);
        break;
    case Opcodes::DSUB:
        if (auto* a = constantAs<double>(value1))
            if (auto* b = constantAs<double>(value2))
                return OriginalSourceConstantValue::of(*a - *b);
        break;
    case Opcodes::DMUL:
        if (auto* a = constantAs<double>(value1))
            if (auto* b = constantAs<double>(value2))
                return OriginalSourceConstantValue::of(*a * *b);
        break;
    case Opcodes::DDIV:
        if (auto* a = constantAs<double>(value1))
            if (auto* b = constantAs<double>(value2))
                return OriginalSourceConstantValue::of(*a / *b);
        break;
    case Opcodes::DREM:
        if (auto* a = constantAs<double>(value1))
            if (auto* b = constantAs<double>(value2))
                return OriginalSourceConstantValue::of(std::fmod(*a, *b));
        break;
    case Opcodes::LCMP:
        if (auto* a = constantAs<std::int64_t>(value1))
            if (auto* b = constantAs<std::int64_t>(value2))
                return OriginalSourceConstantValue::of((*a > *b) - (*a < *b));
        break;
    case Opcodes::FCMPL:
    case Opcodes::FCMPG:
        if (auto* a = constantAs<float>(value1))
            if (auto* b = constantAs<float>(value2))
                return OriginalSourceConstantValue::of(compareFloat(*a, *b, insn->getOpcode()));
        break;
    case Opcodes::DCMPL:
    case Opcodes::DCMPG:
        if (auto* a = constantAs<double>(value1))
            if (auto* b = constantAs<double>(value2))
                return OriginalSourceConstantValue::of(compareDouble(*a, *b, insn->getOpcode()));
        break;
    default: break;
    }
    return std::nullopt;
}

bool containsAll(const jbytecode::analysis::SmallSet<const AbstractInsnNode*>& self,
                 const jbytecode::analysis::SmallSet<const AbstractInsnNode*>& other)
{
    if (self.size() < other.size()) {
        return false;
    }
    for (size_t i = 0; i < other.size(); ++i) {
        if (!self.contains(other[i])) {
            return false;
        }
    }
    return true;
}

bool sameConstant(const OriginalSourceValue& value1, const OriginalSourceValue& value2)
{
    if (value1.constantValue_.has_value() != value2.constantValue_.has_value()) {
        return false;
    }
    if (!value1.constantValue_) {
        return true;
    }
    return *value1.constantValue_ == *value2.constantValue_;
}

bool sameCopiedFrom(const OriginalSourceValue& value1, const OriginalSourceValue& value2)
{
    if (value1.copiedFrom_ == value2.copiedFrom_) {
        return true;
    }
    if (!value1.copiedFrom_ || !value2.copiedFrom_) {
        return false;
    }
    return *value1.copiedFrom_ == *value2.copiedFrom_;
}

}   //namespace

OriginalSourceValue OriginalSourceInterpreter::newValue(const Type* type)
{
    if (type == nullptr) {
        return OriginalSourceValue(1, false);
    }
    if (type->getSort() == Type::VOID) {
        return {};
    }
    return OriginalSourceValue(type->getSize(), false);
}

OriginalSourceValue OriginalSourceInterpreter::newParameterValue(bool isInstanceMethod, int local,
                                                                 const Type& type)
{
    (void)isInstanceMethod;
    return OriginalSourceValue(type.getSize(), true, local);
}

OriginalSourceValue OriginalSourceInterpreter::newOperation(const AbstractInsnNode* insn)
{
    return OriginalSourceValue(valueSizeForNewOperation(insn), insn);
}

OriginalSourceValue OriginalSourceInterpreter::copyOperation(const AbstractInsnNode*    insn,
                                                             const OriginalSourceValue& value)
{
    //Frame values are copied and moved by the worklist; own a snapshot instead
    //of aliasing the caller's stack/local slot.
    return OriginalSourceValue(insn, std::make_shared<OriginalSourceValue>(value));
}

OriginalSourceValue OriginalSourceInterpreter::unaryOperation(const AbstractInsnNode*    insn,
                                                              const OriginalSourceValue& value)
{
    int  size     = valueSizeForUnaryOperation(insn);
    auto constant = foldUnaryConstant(insn, value);
    if (constant) {
        return OriginalSourceValue(size, insn, nullptr, std::move(*constant));
    }
    return OriginalSourceValue(size, insn);
}

OriginalSourceValue OriginalSourceInterpreter::binaryOperation(const AbstractInsnNode*    insn,
                                                               const OriginalSourceValue& value1,
                                                               const OriginalSourceValue& value2)
{
    int  size     = valueSizeForBinaryOperation(insn);
    auto constant = foldBinaryConstant(insn, value1, value2);
    if (constant) {
        return OriginalSourceValue(size, insn, nullptr, std::move(*constant));
    }
    return OriginalSourceValue(size, insn);
}

OriginalSourceValue OriginalSourceInterpreter::ternaryOperation(const AbstractInsnNode*    insn,
                                                                const OriginalSourceValue& value1,
                                                                const OriginalSourceValue& value2,
                                                                const OriginalSourceValue& value3)
{
    (void)value1;
    (void)value2;
    (void)value3;
    return OriginalSourceValue(1, insn);
}

OriginalSourceValue OriginalSourceInterpreter::naryOperation(
    const AbstractInsnNode* insn, const std::vector<OriginalSourceValue>& values)
{
    int size = valueSizeForNaryOperation(insn);

    //Matchers need a temporary list; clone the invoke node so analysis never
    //transfers ownership of the live IR.
    //B8 新 clone 协议：克隆产物进函数局部 scratch arena（纯分析暂存、永不挂树，
    //亦不被 matcher 回调带出）。临时列表必须绑定 scratch：unbound 表析构会
    //delete 一切节点（含 arena 节点）→ 堆损坏；绑定后 arena 节点被跳过、
    //由 scratch 析构统一回收。scratch 声明先于列表 → 析构居后。
    garena::Arena scratch;
    std::unordered_map<jbytecode::core::Label*, jbytecode::core::Label*> clonedLabels;
    auto clonedInsn = const_cast<AbstractInsnNode*>(insn)->clone(scratch, clonedLabels);
    jbytecode::tree::InsnList insnList(&scratch);
    insnList.add(clonedInsn);

    MatchContext context;
    context.original   = &insnList;
    context.startIndex = 0;
    context.endIndex   = 0;

    //Method interpreters may inspect argument values; give them a local copy so
    //matcher callbacks cannot mutate analyzer frame state.
    std::vector<OriginalSourceValue> stackVals = values;
    for (const auto& mi : SimpleInterpreter::METHOD_INTERPRETERS) {
        if (mi.step_->tryMatch(const_cast<AbstractInsnNode*>(insn), context)) {
            auto cv = mi.methodComputation_(insn, stackVals);
            if (cv) {
                return OriginalSourceValue(size, insn, nullptr, std::move(*cv));
            }
        }
    }

    return OriginalSourceValue(size, insn);
}

void OriginalSourceInterpreter::returnOperation(const AbstractInsnNode*    insn,
                                                const OriginalSourceValue& value,
                                                const OriginalSourceValue& expected)
{
    (void)insn;
    (void)value;
    (void)expected;
}

OriginalSourceValue OriginalSourceInterpreter::merge(const OriginalSourceValue& value1,
                                                     const OriginalSourceValue& value2)
{
    if (value1 == value2) {
        return value1;
    }

    bool cvEqual         = sameConstant(value1, value2);
    bool copiedFromEqual = sameCopiedFrom(value1, value2);
    bool parameterEqual  = value1.isMethodParameter() == value2.isMethodParameter() &&
                          value1.getParameterLocal() == value2.getParameterLocal();

    auto setUnion = value1.insns_.union_(value2.insns_);
    if (value1.size_ == value2.size_ && cvEqual && parameterEqual && copiedFromEqual &&
        containsAll(value1.insns_, value2.insns_)) {
        return value1;
    }

    std::optional<OriginalSourceConstantValue> constant;
    if (cvEqual) {
        constant = value1.constantValue_;
    }

    std::shared_ptr<OriginalSourceValue> mergedCopiedFrom;
    if (copiedFromEqual && value1.copiedFrom_) {
        mergedCopiedFrom = value1.copiedFrom_;
    }
    else if (value1.copiedFrom_ && value2.copiedFrom_) {
        auto merged      = merge(*value1.copiedFrom_, *value2.copiedFrom_);
        mergedCopiedFrom = std::make_shared<OriginalSourceValue>(std::move(merged));
    }

    int mergedSize = (std::min)(value1.size_, value2.size_);
    if (setUnion.size() == 1) {
        return OriginalSourceValue(
            mergedSize, setUnion[0], std::move(mergedCopiedFrom), std::move(constant));
    }

    OriginalSourceValue merged(mergedSize,
                               setUnion,
                               std::move(mergedCopiedFrom),
                               cvEqual ? std::move(constant) : std::nullopt);
    if (parameterEqual && value1.isMethodParameter()) {
        merged.isMethodParameter_ = true;
        merged.parameterLocal_    = value1.getParameterLocal();
    }
    return merged;
}

}   //namespace mjt::base
