#include "mbase/asm/matcher/step_util.hpp"
#include "jbytecode/tree/insn_type_traits.hpp"

namespace mjt::base {

namespace {

template<typename T>
bool isLdcValue(jbytecode::tree::AbstractInsnNode* node)
{
    if (!node || node->getType() != jbytecode::tree::AbstractInsnNode::LDC_INSN) return false;
    return ca::core::tag_cast::cast<jbytecode::tree::LdcInsnNode>(node)->cst.as<T>() != nullptr;
}

bool isLdcLong(jbytecode::tree::AbstractInsnNode* node)
{
    return isLdcValue<ca::i64>(node) || isLdcValue<long long>(node);
}

bool isLdcReference(jbytecode::tree::AbstractInsnNode* node)
{
    return isLdcValue<std::string>(node) || isLdcValue<ca::str::Utf8StringRef>(node) ||
           isLdcValue<jbytecode::core::Type>(node);
}

}   //namespace

IMatchStep* StepUtil::loadInt()
{
    static auto* step = new AnyStep([](jbytecode::tree::AbstractInsnNode* n) {
        int op = n->getOpcode();
        return op == jbytecode::core::Opcodes::ILOAD || op == jbytecode::core::Opcodes::BIPUSH ||
               op == jbytecode::core::Opcodes::SIPUSH || isLdcValue<int>(n) ||
               (op >= jbytecode::core::Constants::ILOAD_0 &&
                op <= jbytecode::core::Constants::ILOAD_3) ||
               (op >= jbytecode::core::Opcodes::ICONST_M1 &&
                op <= jbytecode::core::Opcodes::ICONST_5);
    });
    return step;
}

IMatchStep* StepUtil::loadFloat()
{
    static auto* step = new AnyStep([](jbytecode::tree::AbstractInsnNode* n) {
        int op = n->getOpcode();
        return op == jbytecode::core::Opcodes::FLOAD || isLdcValue<float>(n) ||
               (op >= jbytecode::core::Constants::FLOAD_0 &&
                op <= jbytecode::core::Constants::FLOAD_3) ||
               (op >= jbytecode::core::Opcodes::FCONST_0 &&
                op <= jbytecode::core::Opcodes::FCONST_2);
    });
    return step;
}

IMatchStep* StepUtil::loadDouble()
{
    static auto* step = new AnyStep([](jbytecode::tree::AbstractInsnNode* n) {
        int op = n->getOpcode();
        return op == jbytecode::core::Opcodes::DLOAD || isLdcValue<double>(n) ||
               (op >= jbytecode::core::Constants::DLOAD_0 &&
                op <= jbytecode::core::Constants::DLOAD_3) ||
               (op >= jbytecode::core::Opcodes::DCONST_0 &&
                op <= jbytecode::core::Opcodes::DCONST_1);
    });
    return step;
}

IMatchStep* StepUtil::loadLong()
{
    static auto* step = new AnyStep([](jbytecode::tree::AbstractInsnNode* n) {
        int op = n->getOpcode();
        return op == jbytecode::core::Opcodes::LLOAD || isLdcLong(n) ||
               (op >= jbytecode::core::Constants::LLOAD_0 &&
                op <= jbytecode::core::Constants::LLOAD_3) ||
               (op >= jbytecode::core::Opcodes::LCONST_0 &&
                op <= jbytecode::core::Opcodes::LCONST_1);
    });
    return step;
}

IMatchStep* StepUtil::loadStr()
{
    static auto* step =
        new AnyStep([](jbytecode::tree::AbstractInsnNode* n) { return isLdcReference(n); });
    return step;
}

IMatchStep* StepUtil::invokestatic(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                   ca::str::Utf8StringRef desc)
{
    return new SimpleStep(jbytecode::core::Opcodes::INVOKESTATIC,
                          [owner, name, desc](jbytecode::tree::AbstractInsnNode* n) {
                              auto* m = ca::core::tag_cast::cast<jbytecode::tree::MethodInsnNode>(n);
                              return m && m->owner == owner && m->name == name && m->desc == desc;
                          });
}

IMatchStep* StepUtil::invokevirtual(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                    ca::str::Utf8StringRef desc)
{
    return new SimpleStep(jbytecode::core::Opcodes::INVOKEVIRTUAL,
                          [owner, name, desc](jbytecode::tree::AbstractInsnNode* n) {
                              auto* m = ca::core::tag_cast::cast<jbytecode::tree::MethodInsnNode>(n);
                              return m && m->owner == owner && m->name == name && m->desc == desc;
                          });
}

IMatchStep* StepUtil::invokespecial(ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                    ca::str::Utf8StringRef desc)
{
    return new SimpleStep(jbytecode::core::Opcodes::INVOKESPECIAL,
                          [owner, name, desc](jbytecode::tree::AbstractInsnNode* n) {
                              auto* m = ca::core::tag_cast::cast<jbytecode::tree::MethodInsnNode>(n);
                              return m && m->owner == owner && m->name == name && m->desc == desc;
                          });
}

}   //namespace mjt::base
