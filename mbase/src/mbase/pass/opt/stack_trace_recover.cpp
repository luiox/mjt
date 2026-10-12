#include "mbase/pass/opt/stack_trace_recover.hpp"
#include "mbase/pass/opt/recover_detail.hpp"

namespace mjt::base {

PreservedAnalyses StackTraceRecover::run(ClassNode& classNode, ClassAnalysisManager& cam)
{
    auto& arena = cam.arena();

    //1. 恢复源文件名（对齐 Java name.replace("/", ".") + ".java"）
    ca::str::Utf8String dotName = classNode.name.replace_all(
        ca::str::Utf8StringRef::from_cstr("/"), ca::str::Utf8StringRef::from_cstr("."));
    ca::str::Utf8StringBuilder fullName;
    fullName.append(dotName);
    fullName.append(".java");
    auto fullNameRef      = arena.intern(fullName.build());
    classNode.sourceDebug = fullNameRef;
    classNode.sourceFile  = fullNameRef;

    //2. 在每个 try-catch handler label 后插入 dup;invokevirtual printStackTraceV
    //注：Java 的 tryNode.handler 是 LabelNode；C++ 的 TryCatchBlockNode.handler 是
    //jbytecode::core::Label*（非 LabelNode）。需先建 Label* -> LabelNode* 映射定位。
    for (auto* methodNode : classNode.methods) {
        if (!methodNode) continue;
        //建 Label* -> LabelNode* 映射
        std::unordered_map<Label*, LabelNode*> labelToNode;
        for (auto* insn : methodNode->instructions) {
            if (insn->getType() == AbstractInsnNode::LABEL) {
                labelToNode[ca::core::tag_cast::cast<LabelNode*>(insn)->label] = ca::core::tag_cast::cast<LabelNode*>(insn);
            }
        }
        //按 handler label 聚合 catch 类型（同一 handler 可能被多个 try 块共享）：
        //- 单一类型 → 以该类型为 invokevirtual owner；
        //- catch-all（type 为空）或多类型共享 → 以 java/lang/Throwable 为 owner。
        //printStackTraceV 声明在 Throwable 上，合法 catch 类型必为 Throwable
        //子类，以自身为 owner 总可验证；而硬编码 java/lang/Exception 会让
        //catch-all / catch(Throwable/Error) 的 handler 产生 VerifyError
        //（入栈类型不可赋给 Exception）。
        struct HandlerOwner
        {
            LabelNode*             label;
            ca::str::Utf8StringRef owner;
            bool                   conflicted;
        };
        std::vector<HandlerOwner> handlers;
        for (auto* tryNode : methodNode->tryCatchBlocks) {
            auto it = labelToNode.find(tryNode->handler);
            if (it == labelToNode.end()) continue;
            auto hit = std::find_if(handlers.begin(), handlers.end(), [&](const HandlerOwner& h) {
                return h.label == it->second;
            });
            if (hit == handlers.end()) {
                handlers.push_back({it->second, tryNode->type, false});
            }
            else if (!hit->owner.equals(tryNode->type)) {
                hit->conflicted = true;
            }
        }
        for (const auto& handler : handlers) {
            auto printOwner = (handler.conflicted || handler.owner.is_empty())
                                  ? ca::str::Utf8StringRef::from_cstr("java/lang/Throwable")
                                  : handler.owner;
            //在异常处理器开头插入 e.printStackTrace：dup + invokevirtual。
            //Java 原版语义是 handler label 之后（处理器入口）；本移植的
            //InsnList::insert 一律前插，故取 label 的下一节点作锚点
            //（label 是末节点时退化为头部插入）。
            auto snippet = InsnBuilder::build([&](InsnBuilder& b) {
                b.dup();
                b.invokevirtual(printOwner,
                                ca::str::Utf8StringRef::from_cstr("printStackTrace"),
                                ca::str::Utf8StringRef::from_cstr("()V"));
            });
            auto* anchor = handler.label->getNext();
            if (anchor != nullptr) {
                methodNode->instructions.insert(anchor, snippet);
            }
            else {
                methodNode->instructions.insert(snippet);
            }
        }
    }

    //3. 每个 label 后插入 LineNumberNode，行号递增（对齐 Java line 从 1 递增，溢出回绕）
    int line = 1;
    for (auto* methodNode : classNode.methods) {
        if (!methodNode) continue;
        std::vector<LabelNode*> insertPos;
        for (auto* insn : methodNode->instructions) {
            if (insn->getType() == AbstractInsnNode::LABEL) {
                insertPos.push_back(ca::core::tag_cast::cast<LabelNode*>(insn));
            }
        }
        for (auto* labelNode : insertPos) {
            //LineNumberNode 必须排在 label 之后：visitLineNumber 写出时读
            //label->bytecodeOffset，排在 label 之前会在偏移未解析时写出
            //0xFFFF（ClassFormatError: Invalid pc in LineNumberTable）。
            auto* ln = methodNode->instructions.create<LineNumberNode>(line, labelNode->label);
            if (auto* next = labelNode->getNext()) {
                methodNode->instructions.insert(next, ln);
            }
            else {
                methodNode->instructions.add(ln);
            }
            line++;
            //对齐 Java：line > Short.MAX_VALUE - Short.MIN_VALUE 则回绕
            if (line > (static_cast<int>(32767) - static_cast<int>(-32768))) {
                line = 1;
            }
        }
    }

    return PreservedAnalyses::none();
}

}   //namespace mjt::base
