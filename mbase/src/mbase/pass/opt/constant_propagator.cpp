#include "mbase/pass/opt/constant_propagator.hpp"
#include "mbase/spi/class_resource_pending_commit.hpp"

#include <algorithm>
#include <unordered_map>
#include <vector>

#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/core/any.hpp"
#include "mbase/analysis/method_effect_analysis.hpp"
#include "mbase/asm/insn/class_util.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/model/resource_model.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

struct CsfrFieldInfo
{
    ca::str::Utf8StringRef owner;
    ca::str::Utf8StringRef name;
    ca::str::Utf8StringRef desc;
    int                    putCnt = 0;
    int                    getCnt = 0;
    ca::core::Any          value;
};

bool isNumericConstant(AbstractInsnNode* insn)
{
    const int opcode = insn->getOpcode();
    if ((opcode >= Opcodes::ICONST_M1 && opcode <= Opcodes::ICONST_5) ||
        opcode == Opcodes::SIPUSH || opcode == Opcodes::BIPUSH ||
        opcode == Opcodes::LCONST_0 || opcode == Opcodes::LCONST_1 ||
        (opcode >= Opcodes::FCONST_0 && opcode <= Opcodes::FCONST_2) ||
        (opcode >= Opcodes::DCONST_0 && opcode <= Opcodes::DCONST_1)) {
        return true;
    }
    if (insn->getType() != AbstractInsnNode::LDC_INSN) return false;
    auto* ldc = ca::core::tag_cast::cast<LdcInsnNode*>(insn);
    return ldc->cst.as<int>() != nullptr || ldc->cst.as<ca::i64>() != nullptr ||
           ldc->cst.as<long long>() != nullptr || ldc->cst.as<float>() != nullptr ||
           ldc->cst.as<double>() != nullptr;
}

}   //namespace

PreservedAnalyses ConsLocalVarOptimizer::run(jbytecode::tree::MethodNode& methodNode,
                                             MethodAnalysisManager&       am)
{
    (void)am;
    auto& insns = methodNode.instructions;
    if (insns.isEmpty()) return PreservedAnalyses::all();

    //指令数组（对齐 Java Arrays.asList(toArray)）。
    auto instructions = insns.toArray();
    int  arrayLen     = static_cast<int>(instructions.size());

    //局部变量使用表（对齐 Java MethodUtil.getLocalVarUsages）。
    LocalVarUsages usages;
    MethodUtil::getLocalVarUsages(methodNode, usages);
    if (usages.empty()) return PreservedAnalyses::all();

    //load 指令 → 替换指令（克隆的常量生产者）。
    std::unordered_map<AbstractInsnNode*, AbstractInsnNode*> replaceMap;
    std::unordered_map<Label*, Label*>                       cloneMap;
    bool                                                     modified = false;

    for (auto& [slot, pair] : usages) {
        auto& stores = pair.first;
        auto& loads  = pair.second;
        //store 次数 != 1 跳过（对齐 Java stores.size != 1）。
        if (stores.size() != 1) continue;
        //load 次数为 0 跳过。
        if (loads.empty()) continue;
        //store 前一条指令必须是数值常量。
        int storeIdx = stores.front();
        if (storeIdx - 1 < 0 ||
            !isNumericConstant(instructions[static_cast<std::size_t>(storeIdx - 1)]))
            continue;

        //把常量生产者克隆到所有 load 处（对齐 Java methodNode.instructions.set(loadInsn,
        //copyInsnNode(insn))）。
        AbstractInsnNode* producer = instructions[static_cast<std::size_t>(storeIdx - 1)];
        for (int loadIdx : loads) {
            AbstractInsnNode* loadInsn = instructions[static_cast<std::size_t>(loadIdx)];
            //B8 新 clone 协议：替换节点经 insns.set 挂回 methodNode.instructions，
            //产物归该方法 arena（生产者同 arena，Label 保身份直返）。
            replaceMap[loadInsn]       = producer->clone(methodNode.arena(), cloneMap);
            modified                   = true;
        }
    }

    if (!modified) return PreservedAnalyses::all();

    //应用替换（对齐 Java builder 风格：遍历原指令，命中则换）。
    //注：InsnList::set 经 removeAndDelete 丢弃旧节点——堆节点当场 delete、arena 节点
    //只摘链随方法域回收；无论哪种，调用方都不可再 delete old（double-free/坏 free）。
    auto* node = insns.getFirst();
    while (node) {
        auto it = replaceMap.find(node);
        if (it != replaceMap.end()) {
            AbstractInsnNode* next = node->getNext();
            insns.set(node, it->second);   //set 删旧挂新，node 此后悬垂
            node = next;
        }
        else {
            node = node->getNext();
        }
    }
    return PreservedAnalyses::none();
}

PreservedAnalyses LabelConsLocalVarPropagator::run(jbytecode::tree::MethodNode& methodNode,
                                                   MethodAnalysisManager&       am)
{
    (void)am;
    auto& insns = methodNode.instructions;
    if (insns.isEmpty()) return PreservedAnalyses::all();

    auto instructions = insns.toArray();
    int  arrayLen     = static_cast<int>(instructions.size());

    //找下一个 label 的下标（对齐 Java findNextLabel）。
    auto findNextLabel = [&instructions](int from) {
        int i = from;
        while (i < static_cast<int>(instructions.size())) {
            if (instructions[static_cast<std::size_t>(i)]->getType() == AbstractInsnNode::LABEL)
                return i;
            i++;
        }
        return static_cast<int>(instructions.size());
    };

    int curLabel = findNextLabel(0);
    if (curLabel == arrayLen) {
        curLabel = 0;
    }

    //load 指令 → 替换指令（对齐 Java localVarToValMap）。
    std::unordered_map<AbstractInsnNode*, AbstractInsnNode*> replaceMap;
    std::unordered_map<Label*, Label*>                       cloneMap;
    bool                                                     modified = false;

    while (curLabel < arrayLen) {
        int nextLabel = findNextLabel(curLabel + 1);
        //区间 [curLabel, nextLabel) 内统计 slot 使用（对齐 Java getAreaLocalVarUsages）。
        LocalVarUsages usages;
        MethodUtil::getAreaLocalVarUsages(methodNode, curLabel, nextLabel, usages);

        for (auto& [slot, pair] : usages) {
            auto& stores = pair.first;
            auto& loads  = pair.second;
            //从每个 store 开始，找第一个 >= store 的 load（对齐 Java 内层循环）。
            for (int store : stores) {
                for (int load : loads) {
                    if (load < store) continue;   //小于跳过
                    //load 和 store 之间不能有其他 label 或 IINC（IINC 修改 slot，
                    //对齐 ConsLocalVarOptimizer 的 usages 语义）。
                    bool canReplace = true;
                    for (int i = store + 1; i < load; i++) {
                        const int type = instructions[static_cast<std::size_t>(i)]->getType();
                        if (type == AbstractInsnNode::LABEL || type == AbstractInsnNode::IINC_INSN) {
                            canReplace = false;
                            break;
                        }
                    }
                    if (!canReplace) break;

                    //store 前一条必须是数值常量。
                    if (store - 1 < 0 ||
                        !isNumericConstant(instructions[static_cast<std::size_t>(store - 1)]))
                        continue;

                    AbstractInsnNode* prevInsn = instructions[static_cast<std::size_t>(store - 1)];
                    //按类型取值并构造对应常量加载（对齐 Java
                    //getIntInsn/getLongInsn/getDoubleInsn/getFloatInsn）。
                    int     iv;
                    ca::i64 lv;
                    double  dv;
                    float   fv;
                    if (InsnUtil::getIntValue(prevInsn, iv)) {
                        replaceMap[instructions[static_cast<std::size_t>(load)]] =
                            InsnUtil::getIntInsn(insns, iv);
                        modified = true;
                    }
                    else if (InsnUtil::getLongValue(prevInsn, lv)) {
                        replaceMap[instructions[static_cast<std::size_t>(load)]] =
                            InsnUtil::getLongInsn(insns, lv);
                        modified = true;
                    }
                    else if (InsnUtil::getDoubleValue(prevInsn, dv)) {
                        replaceMap[instructions[static_cast<std::size_t>(load)]] =
                            InsnUtil::getDoubleInsn(insns, dv);
                        modified = true;
                    }
                    else if (InsnUtil::getFloatValue(prevInsn, fv)) {
                        replaceMap[instructions[static_cast<std::size_t>(load)]] =
                            InsnUtil::getFloatInsn(insns, fv);
                        modified = true;
                    }
                    break;   //对齐 Java「再往后没必要尝试」
                }
            }
        }
        curLabel = nextLabel;
    }

    if (!modified) return PreservedAnalyses::all();

    //应用替换。
    //注：InsnList::set 经 removeAndDelete 丢弃旧节点——堆节点当场 delete、arena 节点
    //只摘链随方法域回收；无论哪种，调用方都不可再 delete old（double-free/坏 free）。
    auto* node = insns.getFirst();
    while (node) {
        auto it = replaceMap.find(node);
        if (it != replaceMap.end()) {
            AbstractInsnNode* next = node->getNext();
            insns.set(node, it->second);   //set 删旧挂新，node 此后悬垂
            node = next;
        }
        else {
            node = node->getNext();
        }
    }
    return PreservedAnalyses::none();
}

PreservedAnalyses ConsReturnPropagator::run(ResourceContainer& container, GlobalAnalysisManager& am)
{
    if (!am.isRegistered<MethodEffectAnalysis>()) {
        am.registerAnalysis<MethodEffectAnalysis>();
    }
    const auto& effects     = am.getResult<MethodEffectAnalysis>(container);
    auto&       arena       = am.arena();
    bool        modifiedAny = false;
    ClassResourcePendingCommit pending;

    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!resource || !ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto* cls = ca::core::tag_cast::cast<ClassResource>(resource.get());
        if (cls->get().empty()) continue;

        try {
            ClassNode   classNode;
            ClassReader reader(arena, cls->get().data(), static_cast<ca::i32>(cls->get().size()));
            reader.accept(&classNode, ClassReader::SKIP_DEBUG | ClassReader::SKIP_FRAMES);
            bool modifiedClass = false;

            for (auto* method : classNode.methods) {
                if (!method) continue;
                for (auto* insn = method->instructions.getFirst(); insn;) {
                    auto* next = insn->getNext();
                    if (insn->getType() != AbstractInsnNode::METHOD_INSN ||
                        insn->getOpcode() != Opcodes::INVOKESTATIC) {
                        insn = next;
                        continue;
                    }

                    auto* call = ca::core::tag_cast::cast<MethodInsnNode>(insn);
                    if (!call->owner.equals(classNode.name)) {
                        insn = next;
                        continue;
                    }
                    const auto constant = effects.constantReturnOf(call->owner.to_std_string(),
                                                                   call->name.to_std_string(),
                                                                   call->desc.to_std_string());
                    AbstractInsnNode* replacement = nullptr;
                    switch (constant.kind) {
                    case ConstantReturnKind::Int:
                        replacement = InsnUtil::getIntInsn(
                            method->instructions, static_cast<ca::i32>(constant.intValue));
                        break;
                    case ConstantReturnKind::Long:
                        replacement =
                            InsnUtil::getLongInsn(method->instructions, constant.intValue);
                        break;
                    case ConstantReturnKind::Float:
                        replacement = InsnUtil::getFloatInsn(
                            method->instructions, static_cast<float>(constant.floatingValue));
                        break;
                    case ConstantReturnKind::Double:
                        replacement =
                            InsnUtil::getDoubleInsn(method->instructions, constant.floatingValue);
                        break;
                    case ConstantReturnKind::String:
                        replacement =
                            method->instructions.create<LdcInsnNode>(constant.stringValue);
                        break;
                    case ConstantReturnKind::Null:
                        replacement = method->instructions.create<InsnNode>(Opcodes::ACONST_NULL);
                        break;
                    case ConstantReturnKind::Unknown: break;
                    }
                    if (!replacement) {
                        insn = next;
                        continue;
                    }

                    const auto arguments = Type::getArgumentTypes(call->desc);
                    for (auto it = arguments.rbegin(); it != arguments.rend(); ++it) {
                        method->instructions.insertBefore(
                            insn,
                            method->instructions.create<InsnNode>(
                                it->getSize() == 2 ? Opcodes::POP2 : Opcodes::POP));
                    }
                    method->instructions.set(insn, replacement);
                    modifiedClass = true;
                    insn          = next;
                }
            }

            if (!modifiedClass) continue;
            ClassWriter writer(arena, 0);
            classNode.accept(&writer);
            //两阶段提交：写回推迟到循环结束（作业级原子性）。
            pending.stage(cls, writer.toByteArray());
            modifiedAny = true;
        }
        catch (...) {
            //Malformed resources remain untouched; missing facts are conservative.
        }
    }
    pending.commit();
    return modifiedAny ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses ConsStaticFieldReplacer::run(ResourceContainer&     container,
                                               GlobalAnalysisManager& gam)
{
    auto& arena = gam.arena();
    //两阶段提交：写回推迟到循环结束（作业级原子性）。
    ClassResourcePendingCommit pending;

    //一次性把所有 ClassResource 载入 ClassNode（key = uri）。改动后写回对应 ClassResource。
    std::vector<std::pair<ClassResource*, ClassNode*>> loaded;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (!resource || !ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto*       cls   = ca::core::tag_cast::cast<ClassResource>(resource.get());
        const auto& bytes = cls->get();
        if (bytes.empty()) continue;
        auto*       classNode = new ClassNode();
        ClassReader reader(arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
        reader.accept(classNode, ClassReader::SKIP_DEBUG | ClassReader::SKIP_FRAMES);
        loaded.emplace_back(cls, classNode);
    }

    //① 建字段表：owner -> FieldInfo 列表（对齐 Java 第一遍 iterateFieldNode）。
    std::unordered_map<ca::str::Utf8StringRef, std::vector<CsfrFieldInfo>> fieldInfoMap;
    for (auto& [cls, classNode] : loaded) {
        (void)cls;
        for (auto* field : classNode->fields) {
            if (!field || !FieldUtil::isStatic(*field)) continue;
            if (field->value.has_value()) {
                CsfrFieldInfo info{classNode->name, field->name, field->desc, 0, 0, field->value};
                fieldInfoMap[classNode->name].push_back(std::move(info));
            }
            else {
                //value 为空：boolean 字段填默认 false（对齐 Java fieldNode.desc.equals("Z")）。
                if (field->desc.equals(ca::str::Utf8StringRef::from_cstr("Z"))) {
                    CsfrFieldInfo info{classNode->name, field->name, field->desc, 0, 0, false};
                    fieldInfoMap[classNode->name].push_back(std::move(info));
                }
            }
        }
    }

    //② 统计 putstatic/getstatic 次数（对齐 Java 第二遍 iterateMethodNode）。
    for (auto& [cls, classNode] : loaded) {
        (void)cls;
        for (auto* method : classNode->methods) {
            if (!method) continue;
            for (auto* insn : method->instructions) {
                if (insn->getType() != AbstractInsnNode::FIELD_INSN) continue;
                auto* fieldInsn = ca::core::tag_cast::cast<FieldInsnNode*>(insn);
                auto  it        = fieldInfoMap.find(fieldInsn->owner);
                if (it == fieldInfoMap.end()) continue;
                for (auto& info : it->second) {
                    if (info.name.equals(fieldInsn->name) && info.desc.equals(fieldInsn->desc)) {
                        if (fieldInsn->getOpcode() == Opcodes::PUTSTATIC) {
                            info.putCnt++;
                        }
                        else if (fieldInsn->getOpcode() == Opcodes::GETSTATIC) {
                            info.getCnt++;
                        }
                    }
                }
            }
        }
    }

    //③ 剔除：putstatic≠0 的（对齐 Java removeIf putCnt != 0）。
    for (auto& [owner, list] : fieldInfoMap) {
        (void)owner;
        list.erase(
            std::remove_if(
                list.begin(), list.end(), [](const CsfrFieldInfo& f) { return f.putCnt != 0; }),
            list.end());
    }
    //剔除：putstatic 和 getstatic 都为 0 的（对齐 Java removeIf getCnt==0 && putCnt==0）。
    for (auto& [owner, list] : fieldInfoMap) {
        (void)owner;
        list.erase(
            std::remove_if(list.begin(),
                           list.end(),
                           [](const CsfrFieldInfo& f) { return f.getCnt == 0 && f.putCnt == 0; }),
            list.end());
    }
    //剔除空列表（对齐 Java entrySet.removeIf isEmpty）。
    for (auto it = fieldInfoMap.begin(); it != fieldInfoMap.end();) {
        if (it->second.empty())
            it = fieldInfoMap.erase(it);
        else
            ++it;
    }

    if (fieldInfoMap.empty()) {
        for (auto& [cls, classNode] : loaded) delete classNode;
        return PreservedAnalyses::all();
    }

    //④ 把 getstatic 替换为常量加载（对齐 Java 第三遍），并写回改动的类。
    //用就地 set（模式 B）而非 InsnBuilder 整体重建：避免「builder.add(原裸指针) + clear」
    //导致的双重持有/use-after-free（见 ir-ownership-design.md §4 禁用模式 E）。
    int count = 0;
    for (auto& [cls, classNode] : loaded) {
        bool classModified = false;
        for (auto* method : classNode->methods) {
            if (!method) continue;
            bool methodModified = false;
            //迭代用「先存 next 再 set」：set 会 delete 当前节点。
            auto* insn = method->instructions.getFirst();
            while (insn) {
                auto* next = insn->getNext();
                if (insn->getType() == AbstractInsnNode::FIELD_INSN &&
                    insn->getOpcode() == Opcodes::GETSTATIC) {
                    auto* fieldInsn = ca::core::tag_cast::cast<FieldInsnNode*>(insn);
                    auto  it        = fieldInfoMap.find(fieldInsn->owner);
                    if (it != fieldInfoMap.end()) {
                        CsfrFieldInfo* found = nullptr;
                        for (auto& f : it->second) {
                            if (f.owner.equals(fieldInsn->owner) &&
                                f.name.equals(fieldInsn->name) && f.desc.equals(fieldInsn->desc)) {
                                found = &f;
                                break;
                            }
                        }
                        if (found != nullptr) {
                            //构造常量加载指令，就地 set 替换 getstatic（set 删旧挂新）。
                            AbstractInsnNode*    newInsn = nullptr;
                            const ca::core::Any& val     = found->value;
                            if (auto* p = val.as<int>()) {
                                newInsn = method->instructions.create<LdcInsnNode>(*p);
                            }
                            else if (auto* p = val.as<ca::i64>()) {
                                newInsn = method->instructions.create<LdcInsnNode>(*p);
                            }
                            else if (auto* p = val.as<long long>()) {
                                newInsn = method->instructions.create<LdcInsnNode>(
                                    static_cast<ca::i64>(*p));
                            }
                            else if (auto* p = val.as<float>()) {
                                newInsn = method->instructions.create<LdcInsnNode>(*p);
                            }
                            else if (auto* p = val.as<double>()) {
                                newInsn = method->instructions.create<LdcInsnNode>(*p);
                            }
                            else if (auto* p = val.as<bool>()) {
                                newInsn = method->instructions.create<InsnNode>(
                                    Opcodes::ICONST_0 + (*p ? 1 : 0));
                            }
                            if (newInsn != nullptr) {
                                method->instructions.set(insn,
                                                         newInsn);   //set delete insn，挂 newInsn
                                count++;
                                methodModified = true;
                            }
                        }
                    }
                }
                insn = next;
            }
            if (methodModified) {
                classModified = true;
            }
        }
        if (classModified) {
            //写回字节（对齐 GlobalToClassPassAdaptor 的序列化）。
            ClassWriter writer(arena, 0);
            classNode->accept(&writer);
            //两阶段提交：写回推迟到循环结束（作业级原子性）。
            pending.stage(cls, writer.toByteArray());
        }
    }
    pending.commit();

    for (auto& [cls, classNode] : loaded) delete classNode;
    (void)count;
    return count > 0 ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
