#include "mbase/pass/opt/meta_info_remover.hpp"
#include "mbase/pass/opt/clean_pass_detail.hpp"

namespace mjt::base {

PreservedAnalyses MetaInfoRemover::run(jbytecode::tree::ClassNode& classNode,
                                       ClassAnalysisManager&       context)
{
    (void)context;

    classNode.sourceFile  = ca::str::Utf8StringRef{};
    classNode.sourceDebug = ca::str::Utf8StringRef{};
    deleteAndClear(&classNode.arena(), classNode.visibleAnnotations);
    deleteAndClear(&classNode.arena(), classNode.invisibleAnnotations);
    deleteAndClear(&classNode.arena(), classNode.visibleTypeAnnotations);
    deleteAndClear(&classNode.arena(), classNode.invisibleTypeAnnotations);
    deleteAndClear(&classNode.arena(), classNode.attrs);

    for (auto* field : classNode.fields) {
        if (field) clearFieldMeta(*field);
    }
    for (auto* method : classNode.methods) {
        if (method) clearMethodMeta(*method);
    }
    for (auto* recordComponent : classNode.recordComponents) {
        if (!recordComponent) continue;
        deleteAndClear(&classNode.arena(), recordComponent->visibleAnnotations);
        deleteAndClear(&classNode.arena(), recordComponent->invisibleAnnotations);
        deleteAndClear(&classNode.arena(), recordComponent->visibleTypeAnnotations);
        deleteAndClear(&classNode.arena(), recordComponent->invisibleTypeAnnotations);
        deleteAndClear(&classNode.arena(), recordComponent->attrs);
    }

    return PreservedAnalyses::none();
}

PreservedAnalyses FinalRemover::run(jbytecode::tree::ClassNode& classNode,
                                    ClassAnalysisManager&       am)
{
    (void)am;
    int count = 0;
    for (auto* field : classNode.fields) {
        if (field && FieldUtil::isFinal(*field)) {
            FieldUtil::removeFinal(*field);
            count++;
        }
    }
    return count > 0 ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses SyntheticBridgeRemover::run(jbytecode::tree::ClassNode& classNode,
                                              ClassAnalysisManager&       am)
{
    (void)am;
    int removeCount = 0;
    if (ClassUtil::isSynthetic(classNode)) {
        ClassUtil::removeSynthetic(classNode);
        removeCount++;
    }
    for (auto* method : classNode.methods) {
        if (!method) continue;
        if (MethodUtil::isSynthetic(*method)) {
            MethodUtil::removeSynthetic(*method);
            removeCount++;
        }
        if (MethodUtil::isBridge(*method)) {
            MethodUtil::removeBridge(*method);
            removeCount++;
        }
    }
    for (auto* field : classNode.fields) {
        if (!field) continue;
        if (FieldUtil::isSynthetic(*field)) {
            FieldUtil::removeSynthetic(*field);
            removeCount++;
        }
    }
    return removeCount > 0 ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses KotlinMetadataRemover::run(jbytecode::tree::ClassNode& classNode,
                                             ClassAnalysisManager&       am)
{
    (void)am;
    bool modified = false;
    modified |= removeAnnotationsInList(classNode.arena(), classNode.visibleAnnotations, isKotlinMetadata);
    for (auto* method : classNode.methods) {
        if (!method) continue;
        modified |= removeAnnotationsInList(method->arena(), method->visibleAnnotations, isJetbrainsAnnotation);
        modified |= removeAnnotationsInList(method->arena(), method->invisibleAnnotations, isJetbrainsAnnotation);
    }
    for (auto* field : classNode.fields) {
        if (!field) continue;
        modified |= removeAnnotationsInList(field->arena(), field->visibleAnnotations, isJetbrainsAnnotation);
        modified |= removeAnnotationsInList(field->arena(), field->invisibleAnnotations, isJetbrainsAnnotation);
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses SourceInfoRemover::run(jbytecode::tree::ClassNode& classNode,
                                         ClassAnalysisManager&       am)
{
    (void)am;
    bool modified = false;
    if (!classNode.sourceDebug.is_empty()) modified = true;
    classNode.sourceDebug = ca::str::Utf8StringRef{};
    if (!classNode.sourceFile.is_empty()) modified = true;
    classNode.sourceFile = ca::str::Utf8StringRef{};
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses SignatureRemover::run(jbytecode::tree::ClassNode& classNode,
                                        ClassAnalysisManager&       am)
{
    (void)am;
    bool modified = false;
    if (!classNode.signature.is_empty()) modified = true;
    classNode.signature = ca::str::Utf8StringRef{};
    for (auto* method : classNode.methods) {
        if (!method) continue;
        if (!method->signature.is_empty()) {
            method->signature = ca::str::Utf8StringRef{};
            modified          = true;
        }
    }
    for (auto* field : classNode.fields) {
        if (!field) continue;
        if (!field->signature.is_empty()) {
            field->signature = ca::str::Utf8StringRef{};
            modified         = true;
        }
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses IllegalAnnotationRemover::run(jbytecode::tree::ClassNode& classNode,
                                                ClassAnalysisManager&       am)
{
    (void)am;
    bool modified = false;
    modified |= removeMalformedAnnotations(classNode.arena(), classNode.visibleAnnotations);
    modified |= removeMalformedAnnotations(classNode.arena(), classNode.invisibleAnnotations);
    for (auto* method : classNode.methods) {
        if (!method) continue;
        modified |= removeMalformedAnnotations(method->arena(), method->visibleAnnotations);
        modified |= removeMalformedAnnotations(method->arena(), method->invisibleAnnotations);
    }
    for (auto* field : classNode.fields) {
        if (!field) continue;
        modified |= removeMalformedAnnotations(field->arena(), field->visibleAnnotations);
        modified |= removeMalformedAnnotations(field->arena(), field->invisibleAnnotations);
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses IllegalSignatureRemover::run(jbytecode::tree::ClassNode& classNode,
                                               ClassAnalysisManager&       am)
{
    (void)am;
    bool modified = false;
    modified |= clearInvalidSignature(classNode.signature, SignatureKind::Class);
    for (auto* method : classNode.methods) {
        if (!method) continue;
        modified |= clearInvalidSignature(method->signature, SignatureKind::Method);
    }
    for (auto* field : classNode.fields) {
        if (!field) continue;
        modified |= clearInvalidSignature(field->signature, SignatureKind::Field);
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses IllegalTypeAnnotationRemover::run(jbytecode::tree::ClassNode& classNode,
                                                    ClassAnalysisManager&       am)
{
    (void)am;
    bool modified = false;
    modified |= removeMalformedAnnotations(classNode.arena(), classNode.visibleTypeAnnotations);
    modified |= removeMalformedAnnotations(classNode.arena(), classNode.invisibleTypeAnnotations);
    for (auto* method : classNode.methods) {
        if (!method) continue;
        modified |= removeMalformedAnnotations(method->arena(), method->visibleTypeAnnotations);
        modified |= removeMalformedAnnotations(method->arena(), method->invisibleTypeAnnotations);
    }
    for (auto* field : classNode.fields) {
        if (!field) continue;
        modified |= removeMalformedAnnotations(field->arena(), field->visibleTypeAnnotations);
        modified |= removeMalformedAnnotations(field->arena(), field->invisibleTypeAnnotations);
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses IllegalSourceInfoRemover::run(jbytecode::tree::ClassNode& classNode,
                                                ClassAnalysisManager&       am)
{
    (void)am;
    bool modified = false;
    if (isIllegalDebugInfo(classNode.sourceDebug)) {
        classNode.sourceDebug = ca::str::Utf8StringRef{};
        modified              = true;
    }
    if (isIllegalDebugInfo(classNode.sourceFile)) {
        classNode.sourceFile = ca::str::Utf8StringRef{};
        modified             = true;
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses UnknownAttributeCleaner::run(jbytecode::tree::ClassNode& classNode,
                                               ClassAnalysisManager&       am)
{
    (void)am;
    bool modified = false;
    modified |= removeUnknownAttributes(classNode.attrs);
    for (auto* method : classNode.methods) {
        if (!method) continue;
        modified |= removeUnknownAttributes(method->attrs);
    }
    for (auto* field : classNode.fields) {
        if (!field) continue;
        modified |= removeUnknownAttributes(field->attrs);
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses IllegalAccessRemover::run(jbytecode::tree::ClassNode& classNode,
                                            ClassAnalysisManager&       context)
{
    (void)context;
    bool modified = false;

    for (int flag : ILLEGAL_CLASS_ACCESS) {
        if ((classNode.access & flag) != 0) {
            classNode.access &= ~flag;
            modified = true;
        }
    }

    for (auto* method : classNode.methods) {
        for (int flag : ILLEGAL_METHOD_ACCESS) {
            if ((method->access & flag) != 0) {
                method->access &= ~flag;
                modified = true;
            }
        }
        for (auto* param : method->parameters) {
            for (int flag : ILLEGAL_PARAM_ACCESS) {
                if ((param->access & flag) != 0) {
                    param->access &= ~flag;
                    modified = true;
                }
            }
        }
    }

    for (auto* field : classNode.fields) {
        if (!(classNode.access & Opcodes::ACC_ENUM) && (field->access & Opcodes::ACC_ENUM) != 0) {
            field->access &= ~Opcodes::ACC_ENUM;
            modified = true;
        }
        for (int flag : ILLEGAL_FIELD_ACCESS) {
            if ((field->access & flag) != 0) {
                field->access &= ~flag;
                modified = true;
            }
        }
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses SpecificAnnotationRemover::run(jbytecode::tree::ClassNode& classNode,
                                                 ClassAnalysisManager&       context)
{
    (void)context;
    bool modified = false;

    if (!classNode.visibleAnnotations.empty()) {
        if (removeAnnotationsIf(classNode.arena(), classNode.visibleAnnotations)) modified = true;
    }
    if (!classNode.invisibleAnnotations.empty()) {
        if (removeAnnotationsIf(classNode.arena(), classNode.invisibleAnnotations)) modified = true;
    }

    for (auto* field : classNode.fields) {
        if (!field) continue;
        if (!field->visibleAnnotations.empty()) {
            if (removeAnnotationsIf(field->arena(), field->visibleAnnotations)) modified = true;
        }
        if (!field->invisibleAnnotations.empty()) {
            if (removeAnnotationsIf(field->arena(), field->invisibleAnnotations)) modified = true;
        }
    }

    for (auto* method : classNode.methods) {
        if (!method) continue;
        if (!method->visibleAnnotations.empty()) {
            if (removeAnnotationsIf(method->arena(), method->visibleAnnotations)) modified = true;
        }
        if (!method->invisibleAnnotations.empty()) {
            if (removeAnnotationsIf(method->arena(), method->invisibleAnnotations)) modified = true;
        }

        if (!method->visibleParameterAnnotations.empty()) {
            if (removeAnnotationsIf(method->arena(), method->visibleParameterAnnotations)) modified = true;
        }
        if (!method->invisibleParameterAnnotations.empty()) {
            if (removeAnnotationsIf(method->arena(), method->invisibleParameterAnnotations)) modified = true;
        }
    }

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses LineNumberRemover::run(jbytecode::tree::MethodNode& methodNode,
                                         MethodAnalysisManager&       am)
{
    (void)am;
    bool  modified = false;
    auto* node     = methodNode.instructions.getFirst();
    while (node) {
        auto* next = node->getNext();
        if (node->getType() == AbstractInsnNode::LINE) {
            methodNode.instructions.removeAndDelete(node);
            modified = true;
        }
        node = next;
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses LocalVariableInfoRemover::run(jbytecode::tree::MethodNode& methodNode,
                                                MethodAnalysisManager&       am)
{
    (void)am;
    bool modified = false;
    if (!methodNode.localVariables.empty()) {
        deleteAndClear(&methodNode.arena(), methodNode.localVariables);
        modified = true;
    }
    if (!methodNode.visibleLocalVariableAnnotations.empty()) {
        deleteAndClear(&methodNode.arena(), methodNode.visibleLocalVariableAnnotations);
        modified = true;
    }
    if (!methodNode.invisibleLocalVariableAnnotations.empty()) {
        deleteAndClear(&methodNode.arena(), methodNode.invisibleLocalVariableAnnotations);
        modified = true;
    }
    if (!methodNode.attrs.empty()) {
        deleteAndClear(&methodNode.arena(), methodNode.attrs);
        modified = true;
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses ParametersInfoRemover::run(jbytecode::tree::MethodNode& methodNode,
                                             MethodAnalysisManager&       am)
{
    (void)am;
    if (!methodNode.parameters.empty()) {
        for (auto* p : methodNode.parameters)
            if (!methodNode.arena().owns(p)) delete p;   //混合清除(B7)
        methodNode.parameters.clear();
        return PreservedAnalyses::none();
    }
    return PreservedAnalyses::all();
}

PreservedAnalyses ExceptionInfoRemover::run(jbytecode::tree::MethodNode& methodNode,
                                            MethodAnalysisManager&       am)
{
    (void)am;
    if (!methodNode.exceptions.empty()) {
        methodNode.exceptions.clear();
        return PreservedAnalyses::none();
    }
    return PreservedAnalyses::all();
}

PreservedAnalyses LongIndyNameOptimizer::run(jbytecode::tree::MethodNode& methodNode,
                                             MethodAnalysisManager&       am)
{
    (void)am;
    auto& arena    = am.arena();
    bool  modified = false;
    auto* node     = methodNode.instructions.getFirst();
    while (node) {
        if (node->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN) {
            auto* indy = ca::core::tag_cast::cast<InvokeDynamicInsnNode*>(node);
            if (indy->name.byte_length() > 64) {
                int                        hash = javaStringHashCode(indy->name);
                ca::str::Utf8StringBuilder b;
                b.append("indyMethod");
                b.append(std::to_string(hash).c_str());
                indy->name = arena.intern(b.build());
                modified   = true;
            }
        }
        node = node->getNext();
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses FrameInfoRemover::run(jbytecode::tree::MethodNode& methodNode,
                                        MethodAnalysisManager&       am)
{
    (void)am;
    bool  modified = false;
    auto* node     = methodNode.instructions.getFirst();
    while (node) {
        auto* next = node->getNext();
        if (node->getType() == AbstractInsnNode::FRAME) {
            methodNode.instructions.removeAndDelete(node);
            modified = true;
        }
        node = next;
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses IllegalVarargsRemover::run(jbytecode::tree::MethodNode& methodNode,
                                             MethodAnalysisManager&       am)
{
    (void)am;
    if ((methodNode.access & Opcodes::ACC_VARARGS) == 0) return PreservedAnalyses::all();
    if (hasArrayLastArgument(methodNode.desc)) return PreservedAnalyses::all();
    methodNode.access &= ~Opcodes::ACC_VARARGS;
    return PreservedAnalyses::none();
}

PreservedAnalyses IllegalLocalVariableInfoRemover::run(jbytecode::tree::MethodNode& methodNode,
                                                       MethodAnalysisManager&       am)
{
    (void)am;
    bool modified = false;
    auto it       = methodNode.localVariables.begin();
    while (it != methodNode.localVariables.end()) {
        auto* local = *it;
        if (!local || local->name.is_empty() || local->name.byte_at(0) > 128) {
            if (!methodNode.arena().owns(local)) delete local;   //混合清除(B7)
            it       = methodNode.localVariables.erase(it);
            modified = true;
        }
        else {
            ++it;
        }
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
