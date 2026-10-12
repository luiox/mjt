#include "mbase/pass/opt/char_arr_to_str_simplifier.hpp"

#include "libca/str/utf16_string.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using ca::str::ZUtf8StringRef;

PreservedAnalyses CharArrToStrSimplifier::run(MethodNode&            methodNode,
                                              MethodAnalysisManager& context)
{
    (void)context;
    auto& insns = methodNode.instructions;
    if (insns.isEmpty()) return PreservedAnalyses::all();

    //重建表绑定方法 arena：!changed 早退时析构会掠过 arena 克隆产物（unbound 表
    //会误 delete arena 节点），堆上合成的 Ldc/MethodInsn 节点仍由表负责 delete。
    InsnList                           result(&methodNode.arena());
    std::unordered_map<Label*, Label*> clonedLabels;
    bool                               changed = false;
    for (int i = 0; i < insns.size();) {
        auto* insn = insns.get(i);

        int   len;
        auto* next = (i + 1 < insns.size()) ? insns.get(i + 1) : nullptr;
        if (InsnUtil::getIntValue(insn, len) && len >= 0 && next &&
            next->getOpcode() == Opcodes::NEWARRAY && ca::core::tag_cast::cast<IntInsnNode*>(next) &&
            static_cast<IntInsnNode*>(next)->operand == Opcodes::T_CHAR) {

            int                  p     = i + 2;
            bool                 valid = true;
            std::vector<ca::u16> codeUnits;
            for (int j = 0; j < len; j++) {
                if (p + 3 >= insns.size()) {
                    valid = false;
                    break;
                }
                auto* dupInsn     = insns.get(p + j * 4);
                auto* idxInsn     = insns.get(p + j * 4 + 1);
                auto* charInsn    = insns.get(p + j * 4 + 2);
                auto* caStoreInsn = insns.get(p + j * 4 + 3);

                int idx, ch;
                if (dupInsn->getOpcode() != Opcodes::DUP || !InsnUtil::getIntValue(idxInsn, idx) ||
                    idx != j || !InsnUtil::getIntValue(charInsn, ch) ||
                    caStoreInsn->getOpcode() != Opcodes::CASTORE) {
                    valid = false;
                    break;
                }
                //CASTORE follows JVM int-to-char truncation semantics.
                codeUnits.push_back(static_cast<ca::u16>(ch));
            }

            if (valid) {
                std::string text;
                try {
                    ca::str::Utf16String utf16(codeUnits.data(), codeUnits.size());
                    text = utf16.to_utf8_string().ref().to_std_string();
                }
                catch (...) {
                    valid = false;
                }
                if (!valid) {
                    //B8 新 clone 协议：result 最终并回 methodNode.instructions，产物归其 arena。
                    result.add(insn->clone(methodNode.arena(), clonedLabels));
                    i++;
                    continue;
                }
                result.add(methodNode.create<LdcInsnNode>(text));
                result.add(methodNode.create<MethodInsnNode>(Opcodes::INVOKEVIRTUAL,
                                              ZUtf8StringRef::from_static("java/lang/String"),
                                              ZUtf8StringRef::from_static("toCharArray"),
                                              ZUtf8StringRef::from_static("()[C"),
                                              false));
                i       = p + len * 4;
                changed = true;
                continue;
            }
        }

        //B8 新 clone 协议：产物归 methodNode 的 arena（result 结束时并回 instructions）。
        result.add(insn->clone(methodNode.arena(), clonedLabels));
        i++;
    }

    if (!changed) return PreservedAnalyses::all();
    insns.clear();
    insns.add(result);
    return PreservedAnalyses::none();
}

}   //namespace mjt::base
