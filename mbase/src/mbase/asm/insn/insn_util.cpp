#include <cassert>
#include "mbase/asm/insn/insn_util.hpp"
#include <cmath>
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/core/constants.hpp"
#include "jbytecode/tree/insn_type_traits.hpp"
#include "jbytecode/jbytecode.hpp"

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

AbstractInsnNode* InsnUtil::getIntInsn(garena::Arena& arena, int value)
{
    if (value >= -1 && value <= 5) {
        static constexpr int ICONST_MAP[] = {Opcodes::ICONST_M1,
                                             Opcodes::ICONST_0,
                                             Opcodes::ICONST_1,
                                             Opcodes::ICONST_2,
                                             Opcodes::ICONST_3,
                                             Opcodes::ICONST_4,
                                             Opcodes::ICONST_5};
        return arena.create<InsnNode>(ICONST_MAP[value + 1]);
    }
    if (value >= -128 && value <= 127) return arena.create<IntInsnNode>(Opcodes::BIPUSH, value);
    if (value >= -32768 && value <= 32767) return arena.create<IntInsnNode>(Opcodes::SIPUSH, value);
    return arena.create<LdcInsnNode>(value);
}

AbstractInsnNode* InsnUtil::getLongInsn(garena::Arena& arena, ca::i64 value)
{
    if (value == 0) return arena.create<InsnNode>(Opcodes::LCONST_0);
    if (value == 1) return arena.create<InsnNode>(Opcodes::LCONST_1);
    return arena.create<LdcInsnNode>(value);
}

AbstractInsnNode* InsnUtil::getFloatInsn(garena::Arena& arena, float value)
{
    if (value == 0.0f && !std::signbit(value)) return arena.create<InsnNode>(Opcodes::FCONST_0);
    if (value == 1.0f) return arena.create<InsnNode>(Opcodes::FCONST_1);
    if (value == 2.0f) return arena.create<InsnNode>(Opcodes::FCONST_2);
    return arena.create<LdcInsnNode>(value);
}

AbstractInsnNode* InsnUtil::getDoubleInsn(garena::Arena& arena, double value)
{
    if (value == 0.0 && !std::signbit(value)) return arena.create<InsnNode>(Opcodes::DCONST_0);
    if (value == 1.0) return arena.create<InsnNode>(Opcodes::DCONST_1);
    return arena.create<LdcInsnNode>(value);
}

//对应 Java InsnUtil.getTypeInsn(Type) → arena.create<LdcInsnNode>(t)
AbstractInsnNode* InsnUtil::getTypeInsn(garena::Arena& arena, const Type& t)
{
    return arena.create<LdcInsnNode>(t);
}

//DEV-11 修订：堆版不再是待删除双轨，而是 InsnBuilder/MatchContext 暂存管线的
//专用建点入口（暂存节点必须堆分配：builder 析构盲 delete，arena 版会双重释放，
//见 insn_builder.hpp setNodeArena 冻结注释）。直插绑定列表的调用点已全部迁往
//下方 (Arena&, v) / (InsnList&, v) 重载；本组退役前提 = 暂存器语义重设计。
//混合清除契约下堆节点入绑定列表由容器 delete，行为安全（deviations DEV-11）。
AbstractInsnNode* InsnUtil::getIntInsn(int value)
{
    if (value >= -1 && value <= 5) {
        static constexpr int ICONST_MAP[] = {Opcodes::ICONST_M1,
                                             Opcodes::ICONST_0,
                                             Opcodes::ICONST_1,
                                             Opcodes::ICONST_2,
                                             Opcodes::ICONST_3,
                                             Opcodes::ICONST_4,
                                             Opcodes::ICONST_5};
        return new InsnNode(ICONST_MAP[value + 1]);
    }
    if (value >= -128 && value <= 127) return new IntInsnNode(Opcodes::BIPUSH, value);
    if (value >= -32768 && value <= 32767) return new IntInsnNode(Opcodes::SIPUSH, value);
    return new LdcInsnNode(value);
}

AbstractInsnNode* InsnUtil::getLongInsn(ca::i64 value)
{
    if (value == 0) return new InsnNode(Opcodes::LCONST_0);
    if (value == 1) return new InsnNode(Opcodes::LCONST_1);
    return new LdcInsnNode(value);
}

AbstractInsnNode* InsnUtil::getFloatInsn(float value)
{
    if (value == 0.0f && !std::signbit(value)) return new InsnNode(Opcodes::FCONST_0);
    if (value == 1.0f) return new InsnNode(Opcodes::FCONST_1);
    if (value == 2.0f) return new InsnNode(Opcodes::FCONST_2);
    return new LdcInsnNode(value);
}

AbstractInsnNode* InsnUtil::getDoubleInsn(double value)
{
    if (value == 0.0 && !std::signbit(value)) return new InsnNode(Opcodes::DCONST_0);
    if (value == 1.0) return new InsnNode(Opcodes::DCONST_1);
    return new LdcInsnNode(value);
}

//对应 Java InsnUtil.getTypeInsn(Type) → new LdcInsnNode(t)
AbstractInsnNode* InsnUtil::getTypeInsn(const Type& t)
{
    return new LdcInsnNode(t);
}

//-- InsnList 便利重载：经列表绑定的 owner arena 委托（未绑定列表 debug 断言）--
AbstractInsnNode* InsnUtil::getIntInsn(jbytecode::tree::InsnList& list, int value)
{
    assert(list.ownerArena() != nullptr && "InsnUtil::getIntInsn: unbound list");
    return getIntInsn(*list.ownerArena(), value);
}
AbstractInsnNode* InsnUtil::getLongInsn(jbytecode::tree::InsnList& list, ca::i64 value)
{
    assert(list.ownerArena() != nullptr && "InsnUtil::getLongInsn: unbound list");
    return getLongInsn(*list.ownerArena(), value);
}
AbstractInsnNode* InsnUtil::getFloatInsn(jbytecode::tree::InsnList& list, float value)
{
    assert(list.ownerArena() != nullptr && "InsnUtil::getFloatInsn: unbound list");
    return getFloatInsn(*list.ownerArena(), value);
}
AbstractInsnNode* InsnUtil::getDoubleInsn(jbytecode::tree::InsnList& list, double value)
{
    assert(list.ownerArena() != nullptr && "InsnUtil::getDoubleInsn: unbound list");
    return getDoubleInsn(*list.ownerArena(), value);
}
AbstractInsnNode* InsnUtil::getTypeInsn(jbytecode::tree::InsnList& list, const Type& t)
{
    assert(list.ownerArena() != nullptr && "InsnUtil::getTypeInsn: unbound list");
    return getTypeInsn(*list.ownerArena(), t);
}

bool InsnUtil::isReturn(int opcode)
{
    return opcode >= Opcodes::IRETURN && opcode <= Opcodes::RETURN;
}

bool InsnUtil::isIf(int opcode)
{
    return (opcode >= Opcodes::IFEQ && opcode <= Opcodes::IF_ACMPNE) || opcode == Opcodes::IFNULL ||
           opcode == Opcodes::IFNONNULL;
}

int InsnUtil::invertIf(int opcode)
{
    switch (opcode) {
        case Opcodes::IFEQ: return Opcodes::IFNE;
        case Opcodes::IFNE: return Opcodes::IFEQ;
        case Opcodes::IFLT: return Opcodes::IFGE;
        case Opcodes::IFGE: return Opcodes::IFLT;
        case Opcodes::IFGT: return Opcodes::IFLE;
        case Opcodes::IFLE: return Opcodes::IFGT;
        case Opcodes::IF_ICMPEQ: return Opcodes::IF_ICMPNE;
        case Opcodes::IF_ICMPNE: return Opcodes::IF_ICMPEQ;
        case Opcodes::IF_ICMPLT: return Opcodes::IF_ICMPGE;
        case Opcodes::IF_ICMPGE: return Opcodes::IF_ICMPLT;
        case Opcodes::IF_ICMPGT: return Opcodes::IF_ICMPLE;
        case Opcodes::IF_ICMPLE: return Opcodes::IF_ICMPGT;
        case Opcodes::IF_ACMPEQ: return Opcodes::IF_ACMPNE;
        case Opcodes::IF_ACMPNE: return Opcodes::IF_ACMPEQ;
        case Opcodes::IFNULL: return Opcodes::IFNONNULL;
        case Opcodes::IFNONNULL: return Opcodes::IFNULL;
        default: return opcode;
    }
}

bool InsnUtil::isLabelNode(const jbytecode::tree::AbstractInsnNode* insn)
{
    return insn != nullptr && insn->getType() == AbstractInsnNode::LABEL;
}

bool InsnUtil::isIfNull(int opcode)
{
    return opcode == Opcodes::IFNULL || opcode == Opcodes::IFNONNULL;
}

bool InsnUtil::isGoto(int opcode)
{
    return opcode == Opcodes::GOTO;
}

bool InsnUtil::isSwitch(int opcode)
{
    return opcode == Opcodes::TABLESWITCH || opcode == Opcodes::LOOKUPSWITCH;
}

bool InsnUtil::isLoad(int opcode)
{
    return (opcode >= Opcodes::ILOAD && opcode <= Opcodes::ALOAD) ||
           (opcode >= Constants::ILOAD_0 && opcode <= Constants::ALOAD_3);
}

bool InsnUtil::isStore(int opcode)
{
    return (opcode >= Opcodes::ISTORE && opcode <= Opcodes::ASTORE) ||
           (opcode >= Constants::ISTORE_0 && opcode <= Constants::ASTORE_3);
}

bool InsnUtil::isArrayLoad(int opcode)
{
    return opcode >= Opcodes::IALOAD && opcode <= Opcodes::SALOAD;
}

bool InsnUtil::isArrayStore(int opcode)
{
    return opcode >= Opcodes::IASTORE && opcode <= Opcodes::SASTORE;
}

bool InsnUtil::isConstant(int opcode)
{
    return (opcode >= Opcodes::ACONST_NULL && opcode <= Opcodes::DCONST_1) ||
           opcode == Opcodes::LDC || opcode == Opcodes::BIPUSH || opcode == Opcodes::SIPUSH;
}

bool InsnUtil::isInvoke(int opcode)
{
    return opcode >= Opcodes::INVOKEVIRTUAL && opcode <= Opcodes::INVOKEDYNAMIC;
}

bool InsnUtil::isFieldAccess(int opcode)
{
    return opcode >= Opcodes::GETSTATIC && opcode <= Opcodes::PUTFIELD;
}

bool InsnUtil::isThrow(int opcode)
{
    return opcode == Opcodes::ATHROW;
}

bool InsnUtil::isMonitor(int opcode)
{
    return opcode == Opcodes::MONITORENTER || opcode == Opcodes::MONITOREXIT;
}

bool InsnUtil::isStackOp(int opcode)
{
    return (opcode >= Opcodes::POP && opcode <= Opcodes::SWAP) || opcode == Opcodes::DUP2 ||
           opcode == Opcodes::DUP2_X1 || opcode == Opcodes::DUP2_X2;
}

int InsnUtil::getStackEffect(int opcode)
{
    return getStackPushCount(opcode) - getStackPopCount(opcode);
}

int InsnUtil::getStackPushCount(int opcode)
{
    if (opcode >= Opcodes::IRETURN && opcode <= Opcodes::RETURN) return 0;
    if (opcode == Opcodes::ATHROW) return 0;
    if (opcode >= Opcodes::ACONST_NULL && opcode <= Opcodes::DCONST_1)
        return (opcode == Opcodes::LCONST_0 || opcode == Opcodes::LCONST_1 ||
                opcode == Opcodes::DCONST_0 || opcode == Opcodes::DCONST_1)
                   ? 2
                   : 1;
    if (opcode == Opcodes::BIPUSH || opcode == Opcodes::SIPUSH || opcode == Opcodes::LDC) return 1;
    //if (opcode == Opcodes::LDC_W) return 1;
    //if (opcode == Opcodes::LDC2_W) return 2;
    if (opcode >= Opcodes::ILOAD && opcode <= Opcodes::ALOAD)
        return (opcode == Opcodes::LLOAD || opcode == Opcodes::DLOAD) ? 2 : 1;
    if (opcode >= Constants::ILOAD_0 && opcode <= Constants::ALOAD_3) {
        if (opcode >= Constants::LLOAD_0 && opcode <= Constants::LLOAD_3) return 2;
        if (opcode >= Constants::DLOAD_0 && opcode <= Constants::DLOAD_3) return 2;
        return 1;
    }
    if (opcode >= Opcodes::IALOAD && opcode <= Opcodes::SALOAD) return 1;
    if (opcode == Opcodes::ARRAYLENGTH) return 1;
    if (opcode == Opcodes::GETSTATIC || opcode == Opcodes::GETFIELD) return 1;
    return 0;
}

int InsnUtil::getStackPopCount(int opcode)
{
    if (opcode >= Opcodes::ISTORE && opcode <= Opcodes::ASTORE)
        return (opcode == Opcodes::LSTORE || opcode == Opcodes::DSTORE) ? 2 : 1;
    if (opcode >= Constants::ISTORE_0 && opcode <= Constants::ASTORE_3) {
        if (opcode >= Constants::LSTORE_0 && opcode <= Constants::LSTORE_3) return 2;
        if (opcode >= Constants::DSTORE_0 && opcode <= Constants::DSTORE_3) return 2;
        return 1;
    }
    if (opcode >= Opcodes::IASTORE && opcode <= Opcodes::SASTORE) return 3;
    if (opcode >= Opcodes::POP && opcode <= Opcodes::SWAP) {
        if (opcode == Opcodes::POP2 || opcode == Opcodes::DUP2_X2 || opcode == Opcodes::DUP2_X1)
            return 2;
        return 1;
    }
    if (opcode >= Opcodes::IRETURN && opcode <= Opcodes::RETURN) {
        if (opcode == Opcodes::RETURN) return 0;
        return (opcode == Opcodes::LRETURN || opcode == Opcodes::DRETURN) ? 2 : 1;
    }
    if (opcode >= Opcodes::IFEQ && opcode <= Opcodes::IF_ACMPNE) return 2;
    if (opcode == Opcodes::IFNULL || opcode == Opcodes::IFNONNULL) return 1;
    if (opcode == Opcodes::ATHROW) return 1;
    return 0;
}

//========== 扩展分类 ==========

bool InsnUtil::isArithmeticInsn(int opcode)
{
    return opcode >= Opcodes::IADD && opcode <= Opcodes::LXOR;
}

bool InsnUtil::isTypeCastInsn(int opcode)
{
    return opcode >= Opcodes::I2L && opcode <= Opcodes::D2F;
}

bool InsnUtil::isTerminator(int opcode)
{
    return isReturn(opcode) || isGoto(opcode) || isSwitch(opcode) || isThrow(opcode) ||
           isIf(opcode);
}

bool InsnUtil::isPureComputation(jbytecode::tree::AbstractInsnNode* insn)
{
    int op = insn->getOpcode();
    return isConstant(op) || isArithmeticInsn(op) || isTypeCastInsn(op) || isStackOp(op);
}

bool InsnUtil::isBoundaryNode(jbytecode::tree::AbstractInsnNode* insn)
{
    int type = insn->getType();
    return type == AbstractInsnNode::LABEL || type == AbstractInsnNode::FRAME ||
           type == AbstractInsnNode::LINE;
}

void InsnUtil::collectFrameLabels(const std::vector<ca::core::Any>& values,
                                  std::unordered_set<Label*>&        labels)
{
    for (const auto& value : values) {
        if (const auto* label = value.as<Label*>(); label && *label) {
            labels.insert(*label);
        }
        else if (const auto* label = value.as<const Label*>(); label && *label) {
            labels.insert(const_cast<Label*>(*label));
        }
    }
}

//========== 扩展 Stack 分析 ==========

int InsnUtil::getConsumeStackCount(int opcode)
{
    if (opcode >= Opcodes::ISTORE && opcode <= Opcodes::ASTORE)
        return (opcode == Opcodes::LSTORE || opcode == Opcodes::DSTORE) ? 2 : 1;
    if (opcode >= Opcodes::IRETURN && opcode <= Opcodes::RETURN) {
        if (opcode == Opcodes::RETURN) return 0;
        return (opcode == Opcodes::LRETURN || opcode == Opcodes::DRETURN) ? 2 : 1;
    }
    if (opcode >= Opcodes::IALOAD && opcode <= Opcodes::SALOAD) return 2;
    if (opcode >= Opcodes::IASTORE && opcode <= Opcodes::SASTORE) return 3;
    if (opcode >= Opcodes::POP && opcode <= Opcodes::SWAP) {
        if (opcode == Opcodes::DUP_X2) return 3;
        if (opcode == Opcodes::DUP2_X2) return 4;
        if (opcode == Opcodes::DUP2_X1) return 3;
        if (opcode == Opcodes::DUP_X1 || opcode == Opcodes::DUP2 || opcode == Opcodes::SWAP ||
            opcode == Opcodes::POP2)
            return 2;
        return 1;
    }
    if ((opcode >= Opcodes::IFEQ && opcode <= Opcodes::IF_ACMPNE)) return 2;
    if (opcode == Opcodes::IFNULL || opcode == Opcodes::IFNONNULL) return 1;
    if (opcode == Opcodes::ATHROW || opcode == Opcodes::TABLESWITCH ||
        opcode == Opcodes::LOOKUPSWITCH)
        return 1;
    if (opcode == Opcodes::GETFIELD) return 1;
    if (opcode == Opcodes::PUTSTATIC) return 1;
    if (opcode == Opcodes::PUTFIELD) return 2;
    if (opcode == Opcodes::NEWARRAY || opcode == Opcodes::ANEWARRAY) return 1;
    if (opcode == Opcodes::ARRAYLENGTH) return 1;
    if (opcode == Opcodes::CHECKCAST || opcode == Opcodes::INSTANCEOF) return 1;
    if (opcode == Opcodes::MONITORENTER || opcode == Opcodes::MONITOREXIT) return 1;
    if (opcode >= Opcodes::INEG && opcode <= Opcodes::DNEG) return 1;
    if (opcode >= Opcodes::I2L && opcode <= Opcodes::D2F) return 1;
    return 0;
}

//========== 常量提取 ==========

bool InsnUtil::getIntValue(jbytecode::tree::AbstractInsnNode* insn, int& out)
{
    int op = insn->getOpcode();
    if (op >= Opcodes::ICONST_M1 && op <= Opcodes::ICONST_5) {
        static constexpr int ICONST_VALS[] = {-1, 0, 1, 2, 3, 4, 5};
        out                                = ICONST_VALS[op - Opcodes::ICONST_M1];
        return true;
    }
    if (op == Opcodes::BIPUSH || op == Opcodes::SIPUSH) {
        auto* intInsn = ca::core::tag_cast::cast<jbytecode::tree::IntInsnNode>(insn);
        out           = intInsn->operand;
        return true;
    }
    if (op == Opcodes::LDC) {
        auto* ldc = ca::core::tag_cast::cast<jbytecode::tree::LdcInsnNode>(insn);
        if (ldc->cst.is<int>()) {
            out = ldc->cst.cast<int>();
            return true;
        }
    }
    return false;
}

bool InsnUtil::getLongValue(jbytecode::tree::AbstractInsnNode* insn, ca::i64& out)
{
    int op = insn->getOpcode();
    if (op == Opcodes::LCONST_0) {
        out = 0;
        return true;
    }
    if (op == Opcodes::LCONST_1) {
        out = 1;
        return true;
    }
    if (op == Opcodes::LDC /* || op == Opcodes::LDC2_W */) {
        auto* ldc = ca::core::tag_cast::cast<jbytecode::tree::LdcInsnNode>(insn);
        if (ldc->cst.is<ca::i64>()) {
            out = ldc->cst.cast<ca::i64>();
            return true;
        }
    }
    return false;
}

bool InsnUtil::getFloatValue(jbytecode::tree::AbstractInsnNode* insn, float& out)
{
    int op = insn->getOpcode();
    if (op == Opcodes::FCONST_0) {
        out = 0.0f;
        return true;
    }
    if (op == Opcodes::FCONST_1) {
        out = 1.0f;
        return true;
    }
    if (op == Opcodes::FCONST_2) {
        out = 2.0f;
        return true;
    }
    if (op == Opcodes::LDC) {
        auto* ldc = ca::core::tag_cast::cast<jbytecode::tree::LdcInsnNode>(insn);
        if (ldc->cst.is<float>()) {
            out = ldc->cst.cast<float>();
            return true;
        }
    }
    return false;
}

bool InsnUtil::getDoubleValue(jbytecode::tree::AbstractInsnNode* insn, double& out)
{
    int op = insn->getOpcode();
    if (op == Opcodes::DCONST_0) {
        out = 0.0;
        return true;
    }
    if (op == Opcodes::DCONST_1) {
        out = 1.0;
        return true;
    }
    if (op == Opcodes::LDC) {
        auto* ldc = ca::core::tag_cast::cast<jbytecode::tree::LdcInsnNode>(insn);
        if (ldc->cst.is<double>()) {
            out = ldc->cst.cast<double>();
            return true;
        }
    }
    return false;
}

bool InsnUtil::getStringValue(jbytecode::tree::AbstractInsnNode* insn, std::string& out)
{
    if (insn->getOpcode() == Opcodes::LDC) {
        auto* ldc = ca::core::tag_cast::cast<jbytecode::tree::LdcInsnNode>(insn);
        if (auto* value = ldc->cst.as<std::string>()) {
            out = *value;
            return true;
        }
        if (auto* value = ldc->cst.as<ca::str::Utf8StringRef>()) {
            out = value->to_std_string();
            return true;
        }
        if (auto* value = ldc->cst.as<const char*>()) {
            out = *value ? *value : "";
            return true;
        }
    }
    return false;
}

//========== 合成边界 label（dev_plan E pilot）==========

InsnUtil::BoundaryLabels InsnUtil::ensureBoundaryLabels(jbytecode::tree::InsnList& insns,
                                                        garena::Arena&             arena)
{
    BoundaryLabels boundary;
    //前置条件：列表非空（调用方先判空）。
    //B8：合成边界 Label/LabelNode 一律落目标 arena（归属盖章同 MethodNode::createLabel），
    //弃用堆分配的 LabelNode 默认构造。
    if (insns.getFirst()->getType() != jbytecode::tree::AbstractInsnNode::LABEL) {
        auto* label    = arena.create<jbytecode::core::Label>();
        label->setOwner(&arena);
        boundary.start = arena.create<jbytecode::tree::LabelNode>(label);
        insns.insertBefore(insns.getFirst(), boundary.start);
    }
    if (insns.getLast()->getType() != jbytecode::tree::AbstractInsnNode::LABEL) {
        auto* label  = arena.create<jbytecode::core::Label>();
        label->setOwner(&arena);
        boundary.end = arena.create<jbytecode::tree::LabelNode>(label);
        insns.add(boundary.end);
    }
    return boundary;
}

void InsnUtil::removeBoundaryLabels(jbytecode::tree::InsnList&            insns,
                                    const BoundaryLabels& boundary)
{
    //removeAndDelete 对 arena 产物只摘链（随 arena 统一析构），堆产物维持立即析构。
    if (boundary.start) {
        insns.removeAndDelete(boundary.start);
    }
    if (boundary.end) {
        insns.removeAndDelete(boundary.end);
    }
}

}   //namespace mjt::base
