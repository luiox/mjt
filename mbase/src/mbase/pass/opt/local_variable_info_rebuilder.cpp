#include "mbase/pass/opt/local_variable_info_rebuilder.hpp"
#include "mbase/pass/opt/recover_detail.hpp"
#include "mbase/asm/insn/insn_util.hpp"

namespace mjt::base {

ca::str::Utf8String LocalVariableInfoRebuilder::analyzeType(int opcode)
{
    switch (opcode) {
    case Opcodes::ISTORE:
    case Opcodes::ILOAD: return ca::str::Utf8String::from_cstr("I");
    case Opcodes::LSTORE:
    case Opcodes::LLOAD: return ca::str::Utf8String::from_cstr("J");
    case Opcodes::FSTORE:
    case Opcodes::FLOAD: return ca::str::Utf8String::from_cstr("F");
    case Opcodes::DSTORE:
    case Opcodes::DLOAD: return ca::str::Utf8String::from_cstr("D");
    case Opcodes::ASTORE:
    case Opcodes::ALOAD: return ca::str::Utf8String::from_cstr("Ljava/lang/Object;");
    default: return ca::str::Utf8String::from_cstr("Ljava/lang/Object;");
    }
}

ca::str::Utf8String LocalVariableInfoRebuilder::getPrettyName(int index)
{
    return nameWithIndex("var", index);
}

PreservedAnalyses LocalVariableInfoRebuilder::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();

    //扫描指令构造 slot -> 使用该 slot 的 VarInsnNode 表
    std::unordered_map<int, std::vector<VarInsnNode*>> usagesTable;
    for (auto* insn : methodNode.instructions) {
        if (insn->getType() == AbstractInsnNode::VAR_INSN) {
            auto* varInsn = ca::core::tag_cast::cast<VarInsnNode*>(insn);
            usagesTable[varInsn->var].push_back(varInsn);
        }
    }
    //一个也没有则退出（对齐 Java usagesTable.isEmpty）
    if (usagesTable.empty()) return PreservedAnalyses::all();

    auto& arena = am.arena();

    //已存在则重命名（对齐 Java getPrettyName(i)）
    if (!methodNode.localVariables.empty()) {
        for (int i = 0; i < static_cast<int>(methodNode.localVariables.size()); i++) {
            auto* lv = methodNode.localVariables[static_cast<std::size_t>(i)];
            if (lv->name.equals(ca::str::Utf8StringRef::from_cstr("this"))) continue;
            lv->name = arena.intern(getPrettyName(i));
        }
        return PreservedAnalyses::none();
    }

    //没有则重建：先保证首尾是 LabelNode（dev_plan E pilot：合成边界样板收敛到
    //InsnUtil::ensureBoundaryLabels；本 pass 的合成 label 参与 LVT 范围，保留不撤销；
    //B8 起合成边界落方法内容 arena）。
    InsnUtil::ensureBoundaryLabels(methodNode.instructions, methodNode.arena());

    //首尾 label：first 从前往后找，last 从后往前找（对齐 Java findLabelNode/reverseFindLabelNode）
    LabelNode* firstLabel = nullptr;
    for (auto* insn : methodNode.instructions) {
        if (insn->getType() == AbstractInsnNode::LABEL) {
            firstLabel = ca::core::tag_cast::cast<LabelNode*>(insn);
            break;
        }
    }
    LabelNode* lastLabel = nullptr;
    for (auto* node = methodNode.instructions.getLast(); node != nullptr;
         node       = node->getPrevious()) {
        if (node->getType() == AbstractInsnNode::LABEL) {
            lastLabel = ca::core::tag_cast::cast<LabelNode*>(node);
            break;
        }
    }

    int currentSlotIndex = 0;

    //"this" 的作用域是整个方法（非 static）
    if (!MethodUtil::isStatic(methodNode)) {
        //Java: "L" + context.currentClass.name + ";" —— 经 CurrentClassContext 取类名
        ca::str::Utf8String thisDescStr;
        auto*               ctx = am.getPinned<CurrentClassContext>();
        if (ctx && ctx->classNode) {
            ca::str::Utf8StringBuilder b;
            b.append("L");
            b.append(ctx->classNode->name);
            b.append(";");
            thisDescStr = b.build();
        }
        //TODO: 若 ctx 为空（直接跑方法 pass 无 class 上下文），thisDescStr 为空，
        //重建结果不完整。对齐 Java 需类名；经 ClassToMethodPassAdaptor 时已注入。
        methodNode.localVariables.push_back(
            methodNode.create<LocalVariableNode>(arena.intern(ca::str::Utf8StringRef::from_cstr("this")),
                                  arena.intern(thisDescStr),
                                  ca::str::Utf8StringRef{},
                                  firstLabel ? firstLabel->label : nullptr,
                                  lastLabel ? lastLabel->label : nullptr,
                                  currentSlotIndex));
        currentSlotIndex++;
    }

    //参数（从 desc 取，对齐 Java Type.getMethodType(desc).getArgumentTypes）
    auto paramTypes = Type::getArgumentTypes(methodNode.desc);
    for (std::size_t i = 0; i < paramTypes.size(); i++) {
        //staticArena 退役：描述符 ref 存入 LocalVariableNode（随方法树存活），
        //intern 落点必选注入方法树作业级 arena（与上方 name/desc 同池）。
        methodNode.localVariables.push_back(
            methodNode.create<LocalVariableNode>(arena.intern(nameWithIndex("arg", static_cast<int>(i))),
                                  paramTypes[i].getDescriptor(arena),
                                  ca::str::Utf8StringRef{},
                                  firstLabel ? firstLabel->label : nullptr,
                                  lastLabel ? lastLabel->label : nullptr,
                                  currentSlotIndex));
        currentSlotIndex++;
    }

    //局部变量（对齐 Java usagesTable.entry，跳过已处理的 slot）
    for (auto& [slot, usages] : usagesTable) {
        if (slot <= currentSlotIndex) continue;
        //desc 从首个使用的 opcode 推断（对齐 Java analyzeType(insns.getFirst)）
        auto desc = analyzeType(usages.front()->getOpcode());
        //start/end label：start 从首个使用点往前找第一个 label（对齐 reverseFindLabelNode）
        //end 往后找（对齐 findLabelNode）
        LabelNode* start = nullptr;
        for (auto* node = usages.front()->getPrevious(); node != nullptr;
             node       = node->getPrevious()) {
            if (node->getType() == AbstractInsnNode::LABEL) {
                start = ca::core::tag_cast::cast<LabelNode*>(node);
                break;
            }
        }
        LabelNode* end = nullptr;
        for (auto* node = usages.front()->getNext(); node != nullptr; node = node->getNext()) {
            if (node->getType() == AbstractInsnNode::LABEL) {
                end = ca::core::tag_cast::cast<LabelNode*>(node);
                break;
            }
        }
        methodNode.localVariables.push_back(
            methodNode.create<LocalVariableNode>(arena.intern(nameWithIndex("var", slot)),
                                  arena.intern(desc),
                                  ca::str::Utf8StringRef{},
                                  start ? start->label : nullptr,
                                  end ? end->label : nullptr,
                                  slot));
    }

    return PreservedAnalyses::none();
}

}   //namespace mjt::base
