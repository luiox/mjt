#include "mbase/pass/opt/static_field_default_inliner.hpp"
#include "mbase/pass/opt/structure_detail.hpp"

namespace mjt::base {

PreservedAnalyses StaticFieldDefaultInliner::run(ClassNode& classNode, ClassAnalysisManager& cam)
{
    (void)cam;
    //找 value 为空且 static 的字段（对齐 Java fieldNode.value == null && isStatic）
    //key = name+desc（Java 用 className+name+desc，但 putstatic 时校验 owner==本类，故此处同效）
    std::unordered_map<std::string, FieldNode*> fieldNodes;
    auto keyOf = [](const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc) {
        return name.to_std_string() + desc.to_std_string();
    };
    for (auto* field : classNode.fields) {
        if (!field) continue;
        if (!field->value.has_value() && FieldUtil::isStatic(*field)) {
            fieldNodes[keyOf(field->name, field->desc)] = field;
        }
    }

    //找 <clinit>（对齐 Java MethodUtil.findClinitMethod）
    MethodNode* clinit = ClassUtil::findMethod(classNode,
                                               ca::str::Utf8StringRef::from_cstr("<clinit>"),
                                               ca::str::Utf8StringRef::from_cstr("()V"));
    if (!clinit) return PreservedAnalyses::all();

    bool                           modified = false;
    std::vector<AbstractInsnNode*> toRemove;
    auto                           instructions = clinit->instructions.toArray();

    //预扫描 <clinit> 里每个字段的 PUTSTATIC 次数：同字段多次写入时只保留最后一次
    //值才是最终值，内联任一次都会产生错误 ConstantValue（dev_plan B 节：固定
    //<clinit> 删除顺序、保守跳过重复写）。
    std::unordered_map<std::string, int> putCount;
    for (auto* insn : instructions) {
        if (insn->getType() == AbstractInsnNode::FIELD_INSN &&
            insn->getOpcode() == Opcodes::PUTSTATIC) {
            auto* fieldInsn = ca::core::tag_cast::cast<FieldInsnNode*>(insn);
            ++putCount[keyOf(fieldInsn->name, fieldInsn->desc)];
        }
    }

    for (size_t i = 0; i < instructions.size(); i++) {
        auto* insn = instructions[i];
        if (insn->getType() == AbstractInsnNode::FIELD_INSN &&
            insn->getOpcode() == Opcodes::PUTSTATIC) {
            auto* fieldInsn = ca::core::tag_cast::cast<FieldInsnNode*>(insn);
            auto  it        = fieldNodes.find(keyOf(fieldInsn->name, fieldInsn->desc));
            //对齐 Java：还需 owner==classNode.name（fieldNodes 的 key 含 owner 在 Java 里；
            //C++ 此处只在本类 clinit 里找，owner 必为本类，但为忠实仍校验）
            if (it == fieldNodes.end() || !fieldInsn->owner.equals(classNode.name)) continue;
            if (putCount[keyOf(fieldInsn->name, fieldInsn->desc)] != 1) continue;
            FieldNode* fieldNode = it->second;
            if (i == 0) continue;
            AbstractInsnNode* prev = instructions[i - 1];
            //long/double
            if (fieldNode->desc.equals(ca::str::Utf8StringRef::from_cstr("J")) ||
                fieldNode->desc.equals(ca::str::Utf8StringRef::from_cstr("D"))) {
                ca::i64 lv;
                double  dv;
                if (InsnUtil::getLongValue(prev, lv)) {
                    fieldNode->value = lv;
                    toRemove.push_back(prev);
                    modified = true;
                    continue;
                }
                else if (InsnUtil::getDoubleValue(prev, dv)) {
                    fieldNode->value = dv;
                    toRemove.push_back(prev);
                    modified = true;
                    continue;
                }
            }
            else if (fieldNode->desc.equals(ca::str::Utf8StringRef::from_cstr("I")) ||
                     fieldNode->desc.equals(ca::str::Utf8StringRef::from_cstr("F"))) {
                int   iv;
                float fv;
                if (InsnUtil::getIntValue(prev, iv)) {
                    fieldNode->value = iv;
                    toRemove.push_back(prev);
                    modified = true;
                    continue;
                }
                else if (InsnUtil::getFloatValue(prev, fv)) {
                    fieldNode->value = fv;
                    toRemove.push_back(prev);
                    modified = true;
                    continue;
                }
            }
        }
    }

    if (!modified) return PreservedAnalyses::all();

    //删除前导常量 + putstatic（putstatic 也删：内联后不再需要赋值）
    //对齐 Java builder.deleteLastInsnNode + 跳过 putstatic 的保存
    for (auto* insn : toRemove) {
        clinit->instructions.removeAndDelete(insn);
    }
    //还需删除被内联的 putstatic 本身（Java continue 跳过了 addInsnNode）
    //重新扫描删 putstatic（仅删前导已被删、且 value 已被设的）
    std::vector<AbstractInsnNode*> putstaticToRemove;
    for (auto* insn : clinit->instructions.toArray()) {
        if (insn->getType() == AbstractInsnNode::FIELD_INSN &&
            insn->getOpcode() == Opcodes::PUTSTATIC) {
            auto* fieldInsn = ca::core::tag_cast::cast<FieldInsnNode*>(insn);
            auto  it        = fieldNodes.find(keyOf(fieldInsn->name, fieldInsn->desc));
            if (it != fieldNodes.end() && it->second->value.has_value()) {
                putstaticToRemove.push_back(insn);
            }
        }
    }
    for (auto* insn : putstaticToRemove) {
        clinit->instructions.removeAndDelete(insn);
    }
    return PreservedAnalyses::none();
}

}   //namespace mjt::base
