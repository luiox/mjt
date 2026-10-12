#include "mbase/asm/insn/class_util.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

//========== ClassUtil ==========

ca::str::Utf8StringRef ClassUtil::getName(ClassNode& cn)
{
    return cn.name;
}

bool ClassUtil::isPublic(ClassNode& cn)
{
    return (cn.access & Opcodes::ACC_PUBLIC) != 0;
}

bool ClassUtil::isInterface(ClassNode& cn)
{
    return (cn.access & Opcodes::ACC_INTERFACE) != 0;
}

bool ClassUtil::isEnum(ClassNode& cn)
{
    return (cn.access & Opcodes::ACC_ENUM) != 0;
}

bool ClassUtil::isSynthetic(ClassNode& cn)
{
    return (cn.access & Opcodes::ACC_SYNTHETIC) != 0;
}

void ClassUtil::removeSynthetic(ClassNode& cn)
{
    cn.access &= ~Opcodes::ACC_SYNTHETIC;
}

MethodNode* ClassUtil::findMethod(ClassNode& cn, const ca::str::Utf8StringRef& name,
                                  const ca::str::Utf8StringRef& desc)
{
    for (auto* m : cn.methods) {
        if (m->name == name && m->desc == desc) return m;
    }
    return nullptr;
}

FieldNode* ClassUtil::findField(ClassNode& cn, const ca::str::Utf8StringRef& name,
                                const ca::str::Utf8StringRef& desc)
{
    for (auto* f : cn.fields) {
        if (f->name == name && f->desc == desc) return f;
    }
    return nullptr;
}

//========== FieldUtil ==========

bool FieldUtil::isPublic(FieldNode& fn)
{
    return (fn.access & Opcodes::ACC_PUBLIC) != 0;
}
bool FieldUtil::isPrivate(FieldNode& fn)
{
    return (fn.access & Opcodes::ACC_PRIVATE) != 0;
}
bool FieldUtil::isStatic(FieldNode& fn)
{
    return (fn.access & Opcodes::ACC_STATIC) != 0;
}
bool FieldUtil::isFinal(FieldNode& fn)
{
    return (fn.access & Opcodes::ACC_FINAL) != 0;
}
bool FieldUtil::isSynthetic(FieldNode& fn)
{
    return (fn.access & Opcodes::ACC_SYNTHETIC) != 0;
}
void FieldUtil::removeFinal(FieldNode& fn)
{
    fn.access &= ~Opcodes::ACC_FINAL;
}
void FieldUtil::removeSynthetic(FieldNode& fn)
{
    fn.access &= ~Opcodes::ACC_SYNTHETIC;
}

//========== MethodUtil ==========

bool MethodUtil::isPublic(MethodNode& mn)
{
    return (mn.access & Opcodes::ACC_PUBLIC) != 0;
}
bool MethodUtil::isPrivate(MethodNode& mn)
{
    return (mn.access & Opcodes::ACC_PRIVATE) != 0;
}
bool MethodUtil::isStatic(MethodNode& mn)
{
    return (mn.access & Opcodes::ACC_STATIC) != 0;
}
bool MethodUtil::isAbstract(MethodNode& mn)
{
    return (mn.access & Opcodes::ACC_ABSTRACT) != 0;
}
bool MethodUtil::isNative(MethodNode& mn)
{
    return (mn.access & Opcodes::ACC_NATIVE) != 0;
}
bool MethodUtil::isSynthetic(MethodNode& mn)
{
    return (mn.access & Opcodes::ACC_SYNTHETIC) != 0;
}
bool MethodUtil::isBridge(MethodNode& mn)
{
    return (mn.access & Opcodes::ACC_BRIDGE) != 0;
}
void MethodUtil::removeSynthetic(MethodNode& mn)
{
    mn.access &= ~Opcodes::ACC_SYNTHETIC;
}
void MethodUtil::removeBridge(MethodNode& mn)
{
    mn.access &= ~Opcodes::ACC_BRIDGE;
}

bool MethodUtil::isConstructor(MethodNode& mn)
{
    return mn.name.equals("<init>");
}

bool MethodUtil::isClinit(MethodNode& mn)
{
    return mn.name.equals("<clinit>");
}

int MethodUtil::getArgCount(ca::str::Utf8StringRef desc)
{
    //单一真相源：Type::getArgumentsAndReturnSizes 的 argumentsSize 已含隐式 this 槽
    //（起始 1），本函数只计描述符参数槽数（不含 this），故减 1。
    return (jbytecode::core::Type::getArgumentsAndReturnSizes(desc) >> 2) - 1;
}

ca::str::Utf8StringRef MethodUtil::getReturnType(ca::str::Utf8StringRef desc)
{
    auto paren = desc.index_of(')');
    if (paren == ca::str::Utf8StringRef::npos) return ca::str::Utf8StringRef();
    return desc.slice(paren + 1, desc.byte_length());
}

int MethodUtil::evalCodeSize(MethodNode& mn)
{
    //Rough estimate: each instruction is ~1 byte on average
    //For a more accurate count, would need to sum instruction sizes
    return (int)mn.instructions.size();
}

//单 slot 的 var store（仅长形式 ISTORE..ASTORE，对齐 Java InsnUtil.isSingleVarStore）。
static bool isSingleVarStore(int opcode)
{
    return opcode >= Opcodes::ISTORE && opcode <= Opcodes::ASTORE;
}
//单 slot 的 var load（仅长形式 ILOAD..ALOAD，对齐 Java InsnUtil.isSingleVarLoad）。
static bool isSingleVarLoad(int opcode)
{
    return opcode >= Opcodes::ILOAD && opcode <= Opcodes::ALOAD;
}

void MethodUtil::getLocalVarUsages(MethodNode& methodNode, LocalVarUsages& localVarUsages)
{
    if (methodNode.instructions.size() == 0) {
        return;
    }
    getAreaLocalVarUsages(methodNode, 0, methodNode.instructions.size(), localVarUsages);
}

void MethodUtil::getAreaLocalVarUsages(MethodNode& methodNode, int start, int end,
                                       LocalVarUsages& localVarUsages)
{
    auto& insns = methodNode.instructions;
    //区间 [start, end) 左闭右开，遍历记录每个 slot 的 store/load 下标。
    for (int i = start; i < end; i++) {
        auto* insn = insns.get(i);
        if (insn == nullptr) continue;
        if (insn->getType() == AbstractInsnNode::VAR_INSN) {
            auto* varInsn = ca::core::tag_cast::cast<VarInsnNode*>(insn);
            if (isSingleVarStore(varInsn->getOpcode())) {
                localVarUsages[varInsn->var].first.push_back(i);
            }
            else if (isSingleVarLoad(varInsn->getOpcode())) {
                localVarUsages[varInsn->var].second.push_back(i);
            }
        }
        else if (insn->getType() == AbstractInsnNode::IINC_INSN) {
            //IINC 修改 slot（读改写）：按 store 语义计入，防止常量传播把被 iinc
            //更新过的变量错替换为常量（否则循环终止条件会被破坏）。
            auto* iincInsn = ca::core::tag_cast::cast<IincInsnNode*>(insn);
            localVarUsages[iincInsn->var].first.push_back(i);
        }
    }
}

}   //namespace mjt::base
