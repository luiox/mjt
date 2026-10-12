#include "constant_interpreter.hpp"

#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include <limits>

#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/tree/insn_type_traits.hpp"
#include "jbytecode/jbytecode.hpp"
#include "mbase/asm/insn/math_helper.hpp"

namespace mjt::base {

ConstantInterpreter::ConstantInterpreter(IConstantReferenceHandler* referenceHandler)
    : referenceHandler_(referenceHandler)
{}

ConstantInterpreter::ConstantInterpreter(IConstantReferenceHandler* referenceHandler, bool isStatic,
                                         int localVariables, ca::str::Utf8StringRef descr,
                                         const std::vector<ca::core::Any>& args)
    : referenceHandler_(referenceHandler)
{
    desc_ = jbytecode::core::Type::getArgumentTypes(descr);
    std::vector<ca::core::Any> reformatted;
    if (!isStatic) {
        reformatted.emplace_back();
    }
    for (size_t i = 0; i < desc_.size(); ++i) {
        reformatted.push_back(i < args.size() ? args[i] : ca::core::Any{});
        if (desc_[i].getSize() == 2) {
            reformatted.emplace_back();
        }
    }
    if (static_cast<int>(reformatted.size()) > localVariables) {
        throw std::invalid_argument(
            "Your method descriptor has more arguments than the local variable count. "
            "Descriptor: " +
            std::string(reinterpret_cast<const char*>(descr.data()), descr.byte_length()) +
            ", Local Variables: " + std::to_string(localVariables) +
            ", Arguments: " + std::to_string(reformatted.size()));
    }
    while (static_cast<int>(reformatted.size()) < localVariables) {
        reformatted.emplace_back();
    }
    args_ = std::move(reformatted);
}

ConstantValue ConstantInterpreter::newOperation(const jbytecode::tree::AbstractInsnNode* insn)
{
    jbytecode::analysis::BasicValue v = basic_.newOperation(insn);
    switch (insn->getOpcode()) {
    case jbytecode::core::Opcodes::ACONST_NULL: return ConstantValue::Null;
    case jbytecode::core::Opcodes::ICONST_M1:
    case jbytecode::core::Opcodes::ICONST_0:
    case jbytecode::core::Opcodes::ICONST_1:
    case jbytecode::core::Opcodes::ICONST_2:
    case jbytecode::core::Opcodes::ICONST_3:
    case jbytecode::core::Opcodes::ICONST_4:
    case jbytecode::core::Opcodes::ICONST_5:
        return ConstantValue(jbytecode::analysis::BasicValue::INT_VALUE,
                             insn->getOpcode() - jbytecode::core::Opcodes::ICONST_0);
    case jbytecode::core::Opcodes::LCONST_0:
    case jbytecode::core::Opcodes::LCONST_1:
        return ConstantValue(
            jbytecode::analysis::BasicValue::LONG_VALUE,
            static_cast<int64_t>(insn->getOpcode() - jbytecode::core::Opcodes::LCONST_0));
    case jbytecode::core::Opcodes::FCONST_0:
    case jbytecode::core::Opcodes::FCONST_1:
    case jbytecode::core::Opcodes::FCONST_2:
        return ConstantValue(
            jbytecode::analysis::BasicValue::FLOAT_VALUE,
            static_cast<float>(insn->getOpcode() - jbytecode::core::Opcodes::FCONST_0));
    case jbytecode::core::Opcodes::DCONST_0:
    case jbytecode::core::Opcodes::DCONST_1:
        return ConstantValue(
            jbytecode::analysis::BasicValue::DOUBLE_VALUE,
            static_cast<double>(insn->getOpcode() - jbytecode::core::Opcodes::DCONST_0));
    case jbytecode::core::Opcodes::BIPUSH:
    case jbytecode::core::Opcodes::SIPUSH:
    {
        auto* intInsn = ca::core::tag_cast::cast<const jbytecode::tree::IntInsnNode*>(insn);
        return ConstantValue(jbytecode::analysis::BasicValue::INT_VALUE, intInsn->operand);
    }
    case jbytecode::core::Opcodes::LDC:
    {
        auto* ldc = ca::core::tag_cast::cast<const jbytecode::tree::LdcInsnNode*>(insn);
        return ConstantValue(v, ldc->cst);
    }
    case jbytecode::core::Opcodes::GETSTATIC:
    {
        auto*         fin = ca::core::tag_cast::cast<const jbytecode::tree::FieldInsnNode*>(insn);
        ca::core::Any o =
            referenceHandler_->getFieldValueOrNull(v, fin->owner, fin->name, fin->desc);
        return ConstantValue(v, o);
    }
    default: return v.getType() == nullptr ? ConstantValue{} : ConstantValue(v, ca::core::Any{});
    }
}

ConstantValue ConstantInterpreter::copyOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                                 const ConstantValue&                     value)
{
    if (!args_.empty() && !value.isKnown()) {
        switch (insn->getOpcode()) {
        case jbytecode::core::Opcodes::ALOAD:
        case jbytecode::core::Opcodes::ILOAD:
        case jbytecode::core::Opcodes::LLOAD:
        case jbytecode::core::Opcodes::DLOAD:
        case jbytecode::core::Opcodes::FLOAD:
        {
            auto* varInsn = ca::core::tag_cast::cast<const jbytecode::tree::VarInsnNode*>(insn);
            if (varInsn->var < static_cast<int>(args_.size())) {
                ConstantValue result = value;
                result.setValue(args_[varInsn->var]);
                return result;
            }
            break;
        }
        default: break;
        }
    }
    return value;
}

ConstantValue ConstantInterpreter::newValue(const jbytecode::core::Type* type)
{
    jbytecode::analysis::BasicValue v = basic_.newValue(type);
    return v.getType() == nullptr ? ConstantValue{} : ConstantValue(v, ca::core::Any{});
}

ConstantValue ConstantInterpreter::unaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                                  const ConstantValue&                     value)
{
    jbytecode::analysis::BasicValue v = basic_.unaryOperation(insn, value.getType());
    switch (insn->getOpcode()) {
    case jbytecode::core::Opcodes::GETFIELD:
    {
        auto* fin = ca::core::tag_cast::cast<const jbytecode::tree::FieldInsnNode*>(insn);
        return ConstantValue(
            v, referenceHandler_->getFieldValueOrNull(v, fin->owner, fin->name, fin->desc));
    }
    case jbytecode::core::Opcodes::NEWARRAY:
    {
        int size = value.getAsInteger();
        if (value.isKnown() && size >= 0) {
            auto* intInsn = ca::core::tag_cast::cast<const jbytecode::tree::IntInsnNode*>(insn);
            return ConstantValue(v, newArray(intInsn->operand, size));
        }
        return ConstantValue(v, ca::core::Any{});
    }
    case jbytecode::core::Opcodes::ARRAYLENGTH:
    {
        if (value.getValue().has_value()) {
            int len = getArrayLength(value.getValue());
            if (len >= 0) return ConstantValue(v, len);
        }
        return ConstantValue(v, ca::core::Any{});
    }
    default:
        return v.getType() == nullptr
                   ? ConstantValue{}
                   : ConstantValue(v, getUnaryValue(insn->getOpcode(), value.getValue()));
    }
}

ConstantValue ConstantInterpreter::binaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                                   const ConstantValue& a, const ConstantValue& b)
{
    jbytecode::analysis::BasicValue v = basic_.binaryOperation(insn, a.getType(), b.getType());
    if (insn->getOpcode() == jbytecode::core::Opcodes::LSHL ||
        insn->getOpcode() == jbytecode::core::Opcodes::LSHR ||
        insn->getOpcode() == jbytecode::core::Opcodes::LUSHR) {
        //libjbytecode BasicInterpreter 当前未声明 long shift 的结果类型，mbase 在此补全。
        v = jbytecode::analysis::BasicValue::LONG_VALUE;
    }
    switch (insn->getOpcode()) {
    case jbytecode::core::Opcodes::BALOAD:
    case jbytecode::core::Opcodes::CALOAD:
    case jbytecode::core::Opcodes::SALOAD:
    case jbytecode::core::Opcodes::IALOAD:
    case jbytecode::core::Opcodes::FALOAD:
    case jbytecode::core::Opcodes::DALOAD:
    case jbytecode::core::Opcodes::LALOAD:
    case jbytecode::core::Opcodes::AALOAD:
    {
        if (a.isKnown() && b.isKnown() && b.isInteger()) {
            int index = b.getAsInteger();
            int len   = getArrayLength(a.getValue());
            if (index >= 0 && index < len) {
                if (insn->getOpcode() == jbytecode::core::Opcodes::AALOAD)
                    return ConstantValue(v, getArrayElement(a.getValue(), index));
                else
                    return ConstantValue(v, getArrayElementAsNumber(a.getValue(), index));
            }
        }
        return ConstantValue(v, ca::core::Any{});
    }
    default:
        return v.getType() == nullptr
                   ? ConstantValue{}
                   : ConstantValue(v,
                                   getBinaryValue(insn->getOpcode(), a.getValue(), b.getValue()));
    }
}

ConstantValue ConstantInterpreter::ternaryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                                    const ConstantValue& a, const ConstantValue& b,
                                                    const ConstantValue& c)
{
    if (a.isKnown() && b.isKnown() && c.isKnown()) {
        int index = b.getAsInteger();
        int len   = getArrayLength(a.getValue());
        if (index >= 0 && index < len) {
            setArrayElement(a.getValue(), insn->getOpcode(), index, c.getValue());
        }
    }
    return ConstantValue{};
}

ConstantValue ConstantInterpreter::naryOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                                 const std::vector<ConstantValue>&        values)
{
    jbytecode::analysis::BasicValue v =
        basic_.naryOperation(insn, std::vector<jbytecode::analysis::BasicValue>{});
    switch (insn->getOpcode()) {
    case jbytecode::core::Opcodes::INVOKEVIRTUAL:
    case jbytecode::core::Opcodes::INVOKESTATIC:
    case jbytecode::core::Opcodes::INVOKESPECIAL:
    case jbytecode::core::Opcodes::INVOKEINTERFACE:
    {
        auto* min = ca::core::tag_cast::cast<const jbytecode::tree::MethodInsnNode*>(insn);
        if (v.getType() == nullptr) return ConstantValue{};
        return ConstantValue(
            v,
            referenceHandler_->getMethodReturnOrNull(v, min->owner, min->name, min->desc, values));
    }
    default: return v.getType() == nullptr ? ConstantValue{} : ConstantValue(v, ca::core::Any{});
    }
}

void ConstantInterpreter::returnOperation(const jbytecode::tree::AbstractInsnNode* insn,
                                          const ConstantValue& value, const ConstantValue& expected)
{
    (void)insn;
    (void)value;
    (void)expected;
}

ConstantValue ConstantInterpreter::merge(const ConstantValue& a, const ConstantValue& b)
{
    if (a.equals(b)) return a;
    jbytecode::analysis::BasicValue t = basic_.merge(a.getType(), b.getType());
    return ConstantValue(t, ca::core::Any{});
}

ca::core::Any ConstantInterpreter::newArray(int arrayType, int size)
{
    switch (arrayType) {
    case jbytecode::core::Opcodes::T_BOOLEAN: return std::vector<char>(size, 0);
    case jbytecode::core::Opcodes::T_CHAR: return std::vector<char16_t>(size, 0);
    case jbytecode::core::Opcodes::T_BYTE: return std::vector<int8_t>(size, 0);
    case jbytecode::core::Opcodes::T_SHORT: return std::vector<int16_t>(size, 0);
    case jbytecode::core::Opcodes::T_INT: return std::vector<int32_t>(size, 0);
    case jbytecode::core::Opcodes::T_FLOAT: return std::vector<float>(size, 0.0f);
    case jbytecode::core::Opcodes::T_DOUBLE: return std::vector<double>(size, 0.0);
    case jbytecode::core::Opcodes::T_LONG: return std::vector<int64_t>(size, 0);
    default: return ca::core::Any{};
    }
}

int ConstantInterpreter::getArrayLength(const ca::core::Any& arr)
{
    if (!arr.has_value()) return -1;
    if (auto* p = arr.as<const std::vector<char>>()) return static_cast<int>(p->size());
    if (auto* p = arr.as<const std::vector<char16_t>>()) return static_cast<int>(p->size());
    if (auto* p = arr.as<const std::vector<int8_t>>()) return static_cast<int>(p->size());
    if (auto* p = arr.as<const std::vector<int16_t>>()) return static_cast<int>(p->size());
    if (auto* p = arr.as<const std::vector<int32_t>>()) return static_cast<int>(p->size());
    if (auto* p = arr.as<const std::vector<int64_t>>()) return static_cast<int>(p->size());
    if (auto* p = arr.as<const std::vector<float>>()) return static_cast<int>(p->size());
    if (auto* p = arr.as<const std::vector<double>>()) return static_cast<int>(p->size());
    return -1;
}

ca::core::Any ConstantInterpreter::getArrayElement(const ca::core::Any& arr, int index)
{
    if (auto* p = arr.as<const std::vector<char>>()) return static_cast<int>((*p)[index]);
    if (auto* p = arr.as<const std::vector<char16_t>>()) return static_cast<int>((*p)[index]);
    if (auto* p = arr.as<const std::vector<int8_t>>()) return static_cast<int>((*p)[index]);
    if (auto* p = arr.as<const std::vector<int16_t>>()) return static_cast<int>((*p)[index]);
    if (auto* p = arr.as<const std::vector<int32_t>>()) return (*p)[index];
    if (auto* p = arr.as<const std::vector<int64_t>>()) return (*p)[index];
    if (auto* p = arr.as<const std::vector<float>>()) return (*p)[index];
    if (auto* p = arr.as<const std::vector<double>>()) return (*p)[index];
    return ca::core::Any{};
}

ca::core::Any ConstantInterpreter::getArrayElementAsNumber(const ca::core::Any& arr, int index)
{
    if (auto* p = arr.as<const std::vector<char>>()) return static_cast<int>((*p)[index]);
    if (auto* p = arr.as<const std::vector<char16_t>>()) return static_cast<int>((*p)[index]);
    if (auto* p = arr.as<const std::vector<int8_t>>()) return static_cast<int>((*p)[index]);
    if (auto* p = arr.as<const std::vector<int16_t>>()) return static_cast<int>((*p)[index]);
    if (auto* p = arr.as<const std::vector<int32_t>>()) return (*p)[index];
    if (auto* p = arr.as<const std::vector<int64_t>>()) return (*p)[index];
    if (auto* p = arr.as<const std::vector<float>>()) return (*p)[index];
    if (auto* p = arr.as<const std::vector<double>>()) return (*p)[index];
    return ca::core::Any{};
}

void ConstantInterpreter::setArrayElement(const ca::core::Any& arr, int opcode, int index,
                                          const ca::core::Any& value)
{
    auto getInt = [&]() -> int {
        if (!value.has_value()) return 0;
        if (auto* p = value.as<int>()) return *p;
        if (auto* p = value.as<short>()) return *p;
        if (auto* p = value.as<char>()) return *p;
        if (auto* p = value.as<int8_t>()) return *p;
        return 0;
    };
    switch (opcode) {
    case jbytecode::core::Opcodes::BASTORE:
        if (auto* p = const_cast<ca::core::Any&>(arr).as<std::vector<int8_t>>()) {
            (*p)[index] = static_cast<int8_t>(getInt());
        }
        else if (auto* p = const_cast<ca::core::Any&>(arr).as<std::vector<char>>()) {
            (*p)[index] = static_cast<char>(getInt() != 0);
        }
        break;
    case jbytecode::core::Opcodes::CASTORE:
        if (auto* p = const_cast<ca::core::Any&>(arr).as<std::vector<char16_t>>()) {
            (*p)[index] = static_cast<char16_t>(getInt());
        }
        break;
    case jbytecode::core::Opcodes::SASTORE:
        if (auto* p = const_cast<ca::core::Any&>(arr).as<std::vector<int16_t>>()) {
            (*p)[index] = static_cast<int16_t>(getInt());
        }
        break;
    case jbytecode::core::Opcodes::IASTORE:
        if (auto* p = const_cast<ca::core::Any&>(arr).as<std::vector<int32_t>>()) {
            (*p)[index] = getInt();
        }
        break;
    case jbytecode::core::Opcodes::FASTORE:
        if (auto* p = const_cast<ca::core::Any&>(arr).as<std::vector<float>>()) {
            auto* vp    = value.as<float>();
            (*p)[index] = vp ? *vp : static_cast<float>(getInt());
        }
        break;
    case jbytecode::core::Opcodes::DASTORE:
        if (auto* p = const_cast<ca::core::Any&>(arr).as<std::vector<double>>()) {
            auto* vp    = value.as<double>();
            (*p)[index] = vp ? *vp : static_cast<double>(getInt());
        }
        break;
    case jbytecode::core::Opcodes::LASTORE:
        if (auto* p = const_cast<ca::core::Any&>(arr).as<std::vector<int64_t>>()) {
            auto* vp    = value.as<int64_t>();
            (*p)[index] = vp ? *vp : static_cast<int64_t>(getInt());
        }
        break;
    default: break;
    }
}

ca::core::Any ConstantInterpreter::getUnaryValue(int opcode, const ca::core::Any& value)
{
    return MathHelper::unaryAny(opcode, value);
}

int ConstantInterpreter::toInt(const ca::core::Any& v)
{
    if (!v.has_value()) return 0;
    if (auto* p = v.as<int>()) return *p;
    if (auto* p = v.as<bool>()) return static_cast<int>(*p);
    if (auto* p = v.as<short>()) return *p;
    if (auto* p = v.as<char>()) return *p;
    if (auto* p = v.as<unsigned short>()) return *p;
    if (auto* p = v.as<int8_t>()) return *p;
    if (auto* p = v.as<int16_t>()) return *p;
    return 0;
}

double ConstantInterpreter::toDouble(const ca::core::Any& v)
{
    if (!v.has_value()) return 0.0;
    if (auto* p = v.as<int>()) return static_cast<double>(*p);
    if (auto* p = v.as<int64_t>()) return static_cast<double>(*p);
    if (auto* p = v.as<float>()) return *p;
    if (auto* p = v.as<double>()) return *p;
    if (auto* p = v.as<bool>()) return static_cast<double>(*p);
    if (auto* p = v.as<short>()) return static_cast<double>(*p);
    if (auto* p = v.as<char>()) return static_cast<double>(*p);
    if (auto* p = v.as<unsigned short>()) return static_cast<double>(*p);
    return 0.0;
}

ca::core::Any ConstantInterpreter::getBinaryValue(int opcode, const ca::core::Any& a,
                                                  const ca::core::Any& b)
{
    return MathHelper::binaryAny(opcode, a, b);
}

}   //namespace mjt::base
