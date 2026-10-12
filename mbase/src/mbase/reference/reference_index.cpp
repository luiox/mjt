#include "mbase/reference/reference_index.hpp"

#include "mbase/asm/analyzer/dataflow_analyzer.hpp"
#include "jbytecode/jbytecode.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/core/type.hpp"
#include "mbase/model/resource_model.hpp"
#include "mbase/reference/reflection_interpreter.hpp"
#include "mbase/analysis/alias_analysis.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

namespace {

using namespace jbytecode::tree;

std::string str(ca::str::Utf8StringRef value)
{
    return value.to_std_string();
}

std::string methodKey(const std::string& owner, const std::string& name, const std::string& desc)
{
    return owner + "\n" + name + "\n" + desc;
}

std::string fieldKey(const std::string& owner, const std::string& name, const std::string& desc)
{
    return owner + "\n" + name + "\n" + desc;
}

std::string siteKey(const ReferenceSite& site)
{
    return methodKey(site.sourceClass, site.sourceMethod, site.sourceDesc);
}

ReferenceSite makeSite(const ClassNode& clazz, const MethodNode& method, AbstractInsnNode* insn,
                       ca::i32 insnIndex)
{
    ReferenceSite site;
    site.sourceClass  = str(clazz.name);
    site.sourceMethod = str(method.name);
    site.sourceDesc   = str(method.desc);
    site.opcode       = insn ? insn->getOpcode() : -1;
    site.insnIndex    = insnIndex;
    return site;
}

ReferenceSite makeClassSite(const ClassNode& clazz)
{
    ReferenceSite site;
    site.sourceClass = str(clazz.name);
    site.opcode      = -1;
    site.insnIndex   = -1;
    return site;
}

std::string normalizeClassName(std::string value)
{
    if (value.size() >= 2 && value.front() == 'L' && value.back() == ';') {
        value = value.substr(1, value.size() - 2);
    }
    for (auto& c : value) {
        if (c == '.') c = '/';
    }
    return value;
}

std::string annotationDescriptorType(ca::str::Utf8StringRef desc)
{
    return normalizeClassName(str(desc));
}

bool looksLikeClassName(const std::string& value)
{
    if (value.empty()) return false;
    if (value.find('/') == std::string::npos && value.find('.') == std::string::npos) {
        return false;
    }
    if (value.find(' ') != std::string::npos || value.find('(') != std::string::npos ||
        value.find(')') != std::string::npos) {
        return false;
    }
    return true;
}

void appendDescriptorTypes(ca::str::Utf8StringRef desc, std::vector<std::string>& out)
{
    for (ca::usize i = 0; i < desc.byte_length(); ++i) {
        ca::u8 c = desc.byte_at(i);
        if (c != 'L') continue;

        ca::usize start = i + 1;
        ca::usize end   = start;
        while (end < desc.byte_length() && desc.byte_at(end) != ';') {
            end++;
        }
        if (end >= desc.byte_length()) break;
        out.push_back(desc.substr(start, end - start).to_std_string());
        i = end;
    }
}

void appendSignatureTypes(ca::str::Utf8StringRef signature, std::vector<std::string>& out)
{
    for (ca::usize i = 0; i < signature.byte_length(); ++i) {
        ca::u8 c = signature.byte_at(i);
        if (c != 'L') continue;

        ca::usize start = i + 1;
        ca::usize end   = start;
        while (end < signature.byte_length()) {
            ca::u8 current = signature.byte_at(end);
            if (current == ';' || current == '<') break;
            end++;
        }
        if (end > start) out.push_back(signature.substr(start, end - start).to_std_string());
    }
}

std::string normalizeClassConstantType(const jbytecode::core::Type& type)
{
    //staticArena 退役：内部名仅瞬时转 std::string，函数级局部 arena 零驻留。
    ca::str::Utf8StringArena scratch;
    std::string              internalName = str(type.getInternalName(scratch));
    if (internalName.size() >= 2 && internalName.front() == 'L' && internalName.back() == ';') {
        return internalName.substr(1, internalName.size() - 2);
    }
    return internalName;
}

bool isMethodHandleTag(int tag)
{
    return tag == jbytecode::core::Opcodes::H_INVOKEVIRTUAL ||
           tag == jbytecode::core::Opcodes::H_INVOKESTATIC ||
           tag == jbytecode::core::Opcodes::H_INVOKESPECIAL ||
           tag == jbytecode::core::Opcodes::H_NEWINVOKESPECIAL ||
           tag == jbytecode::core::Opcodes::H_INVOKEINTERFACE;
}

bool isFieldHandleTag(int tag)
{
    return tag == jbytecode::core::Opcodes::H_GETFIELD ||
           tag == jbytecode::core::Opcodes::H_GETSTATIC ||
           tag == jbytecode::core::Opcodes::H_PUTFIELD ||
           tag == jbytecode::core::Opcodes::H_PUTSTATIC;
}

bool isLambdaMetafactoryHandle(const jbytecode::core::Handle& handle)
{
    return str(handle.getOwner()) == "java/lang/invoke/LambdaMetafactory" &&
           (str(handle.getName()) == "metafactory" || str(handle.getName()) == "altMetafactory");
}

bool isReflectionMethodLookup(const MethodInsnNode& insn)
{
    std::string owner = str(insn.owner);
    std::string name  = str(insn.name);
    std::string desc  = str(insn.desc);
    return owner == "java/lang/Class" && (name == "getDeclaredMethod" || name == "getMethod") &&
           desc == "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;";
}

bool isReflectionConstructorLookup(const MethodInsnNode& insn)
{
    std::string owner = str(insn.owner);
    std::string name  = str(insn.name);
    std::string desc  = str(insn.desc);
    return owner == "java/lang/Class" &&
           (name == "getDeclaredConstructor" || name == "getConstructor") &&
           desc == "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;";
}

bool isReflectionFieldLookup(const MethodInsnNode& insn)
{
    std::string owner = str(insn.owner);
    std::string name  = str(insn.name);
    std::string desc  = str(insn.desc);
    return owner == "java/lang/Class" && (name == "getDeclaredField" || name == "getField") &&
           desc == "(Ljava/lang/String;)Ljava/lang/reflect/Field;";
}

AbstractInsnNode* nextReferenceInsn(AbstractInsnNode* insn)
{
    while (insn && (insn->getType() == AbstractInsnNode::LABEL ||
                    insn->getType() == AbstractInsnNode::LINE ||
                    insn->getType() == AbstractInsnNode::FRAME)) {
        insn = insn->getNext();
    }
    return insn;
}

bool isClassArrayStoreValue(AbstractInsnNode* insn)
{
    AbstractInsnNode* next = nextReferenceInsn(insn ? insn->getNext() : nullptr);
    return next && next->getOpcode() == jbytecode::core::Opcodes::AASTORE;
}

void appendAnnotationTypes(const AnnotationNode* annotation, std::vector<std::string>& out)
{
    if (!annotation) return;

    std::string type = annotationDescriptorType(annotation->desc);
    if (!type.empty()) out.push_back(type);

    for (const auto& element : annotation->elements) {
        if (element.tag == 'e' || element.tag == 'c') {
            std::string value = normalizeClassName(element.sval.to_std_string());
            if (!value.empty()) out.push_back(value);
        }
        if (element.nested) {
            appendAnnotationTypes(element.nested, out);
        }
    }
}

void appendAnnotationsTypes(const std::vector<AnnotationNode*>& annotations,
                            std::vector<std::string>&           out)
{
    for (const auto* annotation : annotations) {
        appendAnnotationTypes(annotation, out);
    }
}

const std::vector<MethodReference>   kEmptyMethodRefs;
const std::vector<FieldReference>    kEmptyFieldRefs;
const std::vector<TypeReference>     kEmptyTypeRefs;
const std::vector<MethodDeclaration> kEmptyMethodDecls;
const std::vector<FieldDeclaration>  kEmptyFieldDecls;

}   //namespace

void ReferenceIndex::clear()
{
    classDecls_.clear();
    methodDecls_.clear();
    fieldDecls_.clear();
    methodRefs_.clear();
    fieldRefs_.clear();
    typeRefs_.clear();
    classDeclsByKey_.clear();
    methodDeclsByKey_.clear();
    fieldDeclsByKey_.clear();
    methodDeclsByOwner_.clear();
    fieldDeclsByOwner_.clear();
    methodRefsByTarget_.clear();
    fieldRefsByTarget_.clear();
    typeRefsByTarget_.clear();
    methodRefsByTargetOwner_.clear();
    fieldRefsByTargetOwner_.clear();
    methodRefsBySource_.clear();
    fieldRefsBySource_.clear();
    typeRefsBySource_.clear();
    methodRefsBySourceClass_.clear();
    fieldRefsBySourceClass_.clear();
    typeRefsBySourceClass_.clear();
}

void ReferenceIndex::build(ResourceContainer& container, ca::str::Utf8StringArena& arena)
{
    clear();
    AliasAnalysis aliases;
    aliases.build(container, arena);
    for (auto& [uri, resource] : container.getResources()) {
        if (resource->getType() != ResourceType::Class) continue;
        auto* cls = ca::core::tag_cast::cast<ClassResource>(resource.get());
        if (!cls || cls->get().empty()) continue;
        try {
            indexClassBytes(cls->get(), arena, &aliases);
        }
        catch (...) {
            //ReferenceIndex is a foundation analysis. Keep it available even
            //when one damaged class cannot be parsed by the strict tree reader.
        }
    }
    resolveReflectionMemberDescriptors();
}

void ReferenceIndex::appendMethodReference(const MethodReference& ref)
{
    methodRefs_.push_back(ref);
    methodRefsByTarget_[methodKey(ref.target.owner, ref.target.name, ref.target.desc)].push_back(
        ref);
    methodRefsByTargetOwner_[ref.target.owner].push_back(ref);
    methodRefsBySource_[siteKey(ref.site)].push_back(ref);
    methodRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
}

void ReferenceIndex::appendFieldReference(const FieldReference& ref)
{
    fieldRefs_.push_back(ref);
    fieldRefsByTarget_[fieldKey(ref.target.owner, ref.target.name, ref.target.desc)].push_back(ref);
    fieldRefsByTargetOwner_[ref.target.owner].push_back(ref);
    fieldRefsBySource_[siteKey(ref.site)].push_back(ref);
    fieldRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
}

void ReferenceIndex::appendTypeReference(const TypeReference& ref, bool methodScoped)
{
    typeRefs_.push_back(ref);
    typeRefsByTarget_[ref.targetType].push_back(ref);
    if (methodScoped) {
        typeRefsBySource_[siteKey(ref.site)].push_back(ref);
    }
    typeRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
}

void ReferenceIndex::rebuildReferenceLookups()
{
    methodRefsByTarget_.clear();
    methodRefsByTargetOwner_.clear();
    methodRefsBySource_.clear();
    methodRefsBySourceClass_.clear();
    for (const auto& ref : methodRefs_) {
        methodRefsByTarget_[methodKey(ref.target.owner, ref.target.name, ref.target.desc)]
            .push_back(ref);
        methodRefsByTargetOwner_[ref.target.owner].push_back(ref);
        methodRefsBySource_[siteKey(ref.site)].push_back(ref);
        methodRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
    }

    fieldRefsByTarget_.clear();
    fieldRefsByTargetOwner_.clear();
    fieldRefsBySource_.clear();
    fieldRefsBySourceClass_.clear();
    for (const auto& ref : fieldRefs_) {
        fieldRefsByTarget_[fieldKey(ref.target.owner, ref.target.name, ref.target.desc)].push_back(
            ref);
        fieldRefsByTargetOwner_[ref.target.owner].push_back(ref);
        fieldRefsBySource_[siteKey(ref.site)].push_back(ref);
        fieldRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
    }
}

void ReferenceIndex::restore(std::vector<ClassDeclaration>  classDecls,
                             std::vector<MethodDeclaration> methodDecls,
                             std::vector<FieldDeclaration>  fieldDecls,
                             std::vector<MethodReference>   methodRefs,
                             std::vector<FieldReference>    fieldRefs,
                             std::vector<TypeReference>     typeRefs)
{
    clear();
    classDecls_ = std::move(classDecls);
    methodDecls_ = std::move(methodDecls);
    fieldDecls_ = std::move(fieldDecls);
    methodRefs_ = std::move(methodRefs);
    fieldRefs_ = std::move(fieldRefs);
    typeRefs_ = std::move(typeRefs);

    //Declaration-derived lookups mirror indexClassBytes: first-seen wins, and
    //ByOwner keeps declaration order so resolveReflectionMemberDescriptors
    //would still see the same sequence if it were re-run (it is not —
    //serialized refs already carry resolved reflection descriptors).
    for (std::size_t i = 0; i < classDecls_.size(); ++i) {
        classDeclsByKey_.emplace(classDecls_[i].name, i);
    }
    for (std::size_t i = 0; i < methodDecls_.size(); ++i) {
        const auto& d = methodDecls_[i];
        methodDeclsByKey_.emplace(methodKey(d.method.owner, d.method.name, d.method.desc), i);
        methodDeclsByOwner_[d.method.owner].push_back(d);
    }
    for (std::size_t i = 0; i < fieldDecls_.size(); ++i) {
        const auto& d = fieldDecls_[i];
        fieldDeclsByKey_.emplace(fieldKey(d.field.owner, d.field.name, d.field.desc), i);
        fieldDeclsByOwner_[d.field.owner].push_back(d);
    }

    //Method/field ref maps share logic with rebuildReferenceLookups, but
    //type-ref maps need the class-scoped vs method-scoped split that
    //appendTypeReference applies at build time.
    for (const auto& ref : methodRefs_) {
        methodRefsByTarget_[methodKey(ref.target.owner, ref.target.name, ref.target.desc)]
            .push_back(ref);
        methodRefsByTargetOwner_[ref.target.owner].push_back(ref);
        methodRefsBySource_[siteKey(ref.site)].push_back(ref);
        methodRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
    }
    for (const auto& ref : fieldRefs_) {
        fieldRefsByTarget_[fieldKey(ref.target.owner, ref.target.name, ref.target.desc)]
            .push_back(ref);
        fieldRefsByTargetOwner_[ref.target.owner].push_back(ref);
        fieldRefsBySource_[siteKey(ref.site)].push_back(ref);
        fieldRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
    }
    for (const auto& ref : typeRefs_) {
        typeRefsByTarget_[ref.targetType].push_back(ref);
        //makeClassSite leaves sourceMethod empty; method-scoped refs always
        //carry it. Restore treats this as the methodScoped discriminator,
        //exactly mirroring appendTypeReference's branch.
        const bool methodScoped = !ref.site.sourceMethod.empty();
        if (methodScoped) {
            typeRefsBySource_[siteKey(ref.site)].push_back(ref);
        }
        typeRefsBySourceClass_[ref.site.sourceClass].push_back(ref);
    }
}

void ReferenceIndex::resolveReflectionMemberDescriptors()
{
    bool changed = false;

    auto methodDescForArgs = [&](const std::string& owner,
                                 const std::string& name,
                                 const std::string& argDesc) {
        auto        it = methodDeclsByOwner_.find(owner);
        std::string desc;
        bool        found = false;
        if (it == methodDeclsByOwner_.end() || argDesc.empty()) return desc;
        for (const auto& decl : it->second) {
            if (decl.method.name != name) continue;
            const std::string& candidate = decl.method.desc;
            const auto         close     = candidate.find(')');
            if (close == std::string::npos) continue;
            if (argDesc.size() != close + 1 || candidate.compare(0, argDesc.size(), argDesc) != 0) {
                continue;
            }
            if (found) return std::string{};
            desc  = candidate;
            found = true;
        }
        return desc;
    };

    auto uniqueMethodDesc = [&](const std::string& owner, const std::string& name) {
        auto        it = methodDeclsByOwner_.find(owner);
        std::string desc;
        bool        found = false;
        if (it == methodDeclsByOwner_.end()) return desc;
        for (const auto& decl : it->second) {
            if (decl.method.name != name) continue;
            if (found) return std::string{};
            desc  = decl.method.desc;
            found = true;
        }
        return desc;
    };

    auto uniqueFieldDesc = [&](const std::string& owner, const std::string& name) {
        auto        it = fieldDeclsByOwner_.find(owner);
        std::string desc;
        bool        found = false;
        if (it == fieldDeclsByOwner_.end()) return desc;
        for (const auto& decl : it->second) {
            if (decl.field.name != name) continue;
            if (found) return std::string{};
            desc  = decl.field.desc;
            found = true;
        }
        return desc;
    };

    for (auto& ref : methodRefs_) {
        if ((ref.kind != MethodReferenceKind::Reflection &&
             ref.kind != MethodReferenceKind::ReflectionExecution) ||
            !ref.target.desc.empty()) {
            continue;
        }
        std::string desc =
            methodDescForArgs(ref.target.owner, ref.target.name, ref.reflectionArgDesc);
        if (desc.empty()) desc = uniqueMethodDesc(ref.target.owner, ref.target.name);
        if (desc.empty()) continue;
        ref.target.desc = desc;
        changed         = true;
    }

    for (auto& ref : fieldRefs_) {
        if ((ref.kind != FieldReferenceKind::Reflection &&
             ref.kind != FieldReferenceKind::ReflectionAccess) ||
            !ref.target.desc.empty()) {
            continue;
        }
        const std::string desc = uniqueFieldDesc(ref.target.owner, ref.target.name);
        if (desc.empty()) continue;
        ref.target.desc = desc;
        changed         = true;
    }

    if (changed) rebuildReferenceLookups();
}

void ReferenceIndex::indexClassBytes(const std::vector<ca::u8>& bytes,
                                     ca::str::Utf8StringArena& arena, const AliasAnalysis* aliases)
{
    ClassNode                    clazz;
    jbytecode::core::ClassReader reader(arena, bytes);
    reader.accept(
        &clazz,
        jbytecode::core::ClassReader::SKIP_DEBUG | jbytecode::core::ClassReader::SKIP_FRAMES);

    std::string className                = str(clazz.name);
    auto        appendClassTypeReference = [&](const std::string& target, TypeReferenceKind kind) {
        if (target.empty()) return;
        TypeReference ref;
        ref.site       = makeClassSite(clazz);
        ref.targetType = target;
        ref.kind       = kind;
        appendTypeReference(ref, false);
    };
    if (classDeclsByKey_.find(className) == classDeclsByKey_.end()) {
        ClassDeclaration decl;
        decl.name      = className;
        decl.superName = str(clazz.superName);
        for (auto iface : clazz.interfaces) {
            decl.interfaces.push_back(str(iface));
        }
        decl.access                 = clazz.access;
        classDeclsByKey_[decl.name] = classDecls_.size();
        classDecls_.push_back(decl);
    }

    if (clazz.superName.byte_length() > 0) {
        appendClassTypeReference(str(clazz.superName), TypeReferenceKind::ClassSuper);
    }
    for (auto iface : clazz.interfaces) {
        appendClassTypeReference(str(iface), TypeReferenceKind::ClassInterface);
    }
    {
        std::vector<std::string> classSignatureTypes;
        appendSignatureTypes(clazz.signature, classSignatureTypes);
        for (const auto& type : classSignatureTypes) {
            appendClassTypeReference(type, TypeReferenceKind::Signature);
        }

        std::vector<std::string> classAnnotationTypes;
        appendAnnotationsTypes(clazz.visibleAnnotations, classAnnotationTypes);
        appendAnnotationsTypes(clazz.invisibleAnnotations, classAnnotationTypes);
        appendAnnotationsTypes(clazz.visibleTypeAnnotations, classAnnotationTypes);
        appendAnnotationsTypes(clazz.invisibleTypeAnnotations, classAnnotationTypes);
        for (const auto& type : classAnnotationTypes) {
            appendClassTypeReference(type, TypeReferenceKind::Annotation);
        }

        appendClassTypeReference(str(clazz.outerClass), TypeReferenceKind::StructuralMetadata);
        appendClassTypeReference(str(clazz.nestHost), TypeReferenceKind::StructuralMetadata);
        for (const auto& member : clazz.nestMembers) {
            appendClassTypeReference(str(member), TypeReferenceKind::StructuralMetadata);
        }
        for (const auto& subclass : clazz.permittedSubclasses) {
            appendClassTypeReference(str(subclass), TypeReferenceKind::StructuralMetadata);
        }
        for (const auto* innerClass : clazz.innerClasses) {
            if (!innerClass) continue;
            appendClassTypeReference(str(innerClass->name), TypeReferenceKind::StructuralMetadata);
            appendClassTypeReference(str(innerClass->outerName),
                                     TypeReferenceKind::StructuralMetadata);
        }
        for (const auto* component : clazz.recordComponents) {
            if (!component) continue;

            std::vector<std::string> componentDescTypes;
            appendDescriptorTypes(component->descriptor, componentDescTypes);
            for (const auto& type : componentDescTypes) {
                appendClassTypeReference(type, TypeReferenceKind::StructuralMetadata);
            }

            std::vector<std::string> componentSignatureTypes;
            appendSignatureTypes(component->signature, componentSignatureTypes);
            for (const auto& type : componentSignatureTypes) {
                appendClassTypeReference(type, TypeReferenceKind::Signature);
            }

            std::vector<std::string> componentAnnotationTypes;
            appendAnnotationsTypes(component->visibleAnnotations, componentAnnotationTypes);
            appendAnnotationsTypes(component->invisibleAnnotations, componentAnnotationTypes);
            appendAnnotationsTypes(component->visibleTypeAnnotations, componentAnnotationTypes);
            appendAnnotationsTypes(component->invisibleTypeAnnotations, componentAnnotationTypes);
            for (const auto& type : componentAnnotationTypes) {
                appendClassTypeReference(type, TypeReferenceKind::Annotation);
            }
        }
        if (clazz.moduleNode) {
            appendClassTypeReference(str(clazz.moduleNode->mainClass),
                                     TypeReferenceKind::StructuralMetadata);
            for (const auto& service : clazz.moduleNode->usesList) {
                appendClassTypeReference(str(service), TypeReferenceKind::StructuralMetadata);
            }
            for (const auto& provide : clazz.moduleNode->providesList) {
                appendClassTypeReference(str(provide.service),
                                         TypeReferenceKind::StructuralMetadata);
                for (const auto& provider : provide.providers) {
                    appendClassTypeReference(str(provider),
                                             TypeReferenceKind::StructuralMetadata);
                }
            }
        }
    }

    for (auto* field : clazz.fields) {
        if (!field) continue;
        FieldDeclaration decl;
        decl.field      = {className, str(field->name), str(field->desc)};
        decl.access     = field->access;
        std::string key = fieldKey(decl.field.owner, decl.field.name, decl.field.desc);
        if (fieldDeclsByKey_.find(key) == fieldDeclsByKey_.end()) {
            fieldDeclsByKey_[key] = fieldDecls_.size();
            fieldDecls_.push_back(decl);
            fieldDeclsByOwner_[decl.field.owner].push_back(decl);
        }

        std::vector<std::string> descTypes;
        appendDescriptorTypes(field->desc, descTypes);
        for (const auto& type : descTypes) {
            TypeReference ref;
            ref.site       = makeClassSite(clazz);
            ref.targetType = type;
            ref.kind       = TypeReferenceKind::FieldDeclarationDescriptor;
            appendTypeReference(ref, false);
        }

        std::vector<std::string> signatureTypes;
        appendSignatureTypes(field->signature, signatureTypes);
        for (const auto& type : signatureTypes) {
            TypeReference ref;
            ref.site       = makeClassSite(clazz);
            ref.targetType = type;
            ref.kind       = TypeReferenceKind::Signature;
            appendTypeReference(ref, false);
        }

        std::vector<std::string> annotationTypes;
        appendAnnotationsTypes(field->visibleAnnotations, annotationTypes);
        appendAnnotationsTypes(field->invisibleAnnotations, annotationTypes);
        for (const auto& type : annotationTypes) {
            TypeReference ref;
            ref.site       = makeClassSite(clazz);
            ref.targetType = type;
            ref.kind       = TypeReferenceKind::Annotation;
            appendTypeReference(ref, false);
        }
    }

    for (auto* method : clazz.methods) {
        if (!method) continue;
        MethodDeclaration decl;
        decl.method     = {className, str(method->name), str(method->desc)};
        decl.access     = method->access;
        std::string key = methodKey(decl.method.owner, decl.method.name, decl.method.desc);
        if (methodDeclsByKey_.find(key) == methodDeclsByKey_.end()) {
            methodDeclsByKey_[key] = methodDecls_.size();
            methodDecls_.push_back(decl);
            methodDeclsByOwner_[decl.method.owner].push_back(decl);
        }
    }

    for (auto* method : clazz.methods) {
        if (!method) continue;
        ca::i32                  insnIndex       = 0;
        ReferenceSite            declarationSite = makeSite(clazz, *method, nullptr, -1);
        std::vector<std::string> methodDescTypes;
        appendDescriptorTypes(method->desc, methodDescTypes);
        for (const auto& type : methodDescTypes) {
            TypeReference ref;
            ref.site       = declarationSite;
            ref.targetType = type;
            ref.kind       = TypeReferenceKind::MethodDescriptor;
            appendTypeReference(ref, true);
        }

        std::vector<std::string> methodSignatureTypes;
        appendSignatureTypes(method->signature, methodSignatureTypes);
        for (const auto& type : methodSignatureTypes) {
            TypeReference ref;
            ref.site       = declarationSite;
            ref.targetType = type;
            ref.kind       = TypeReferenceKind::Signature;
            appendTypeReference(ref, true);
        }

        std::vector<std::string> methodAnnotationTypes;
        appendAnnotationsTypes(method->visibleAnnotations, methodAnnotationTypes);
        appendAnnotationsTypes(method->invisibleAnnotations, methodAnnotationTypes);
        appendAnnotationsTypes(method->visibleTypeAnnotations, methodAnnotationTypes);
        appendAnnotationsTypes(method->invisibleTypeAnnotations, methodAnnotationTypes);
        appendAnnotationsTypes(method->visibleParameterAnnotations, methodAnnotationTypes);
        appendAnnotationsTypes(method->invisibleParameterAnnotations, methodAnnotationTypes);
        for (const auto* annotation : method->visibleLocalVariableAnnotations) {
            appendAnnotationTypes(annotation, methodAnnotationTypes);
        }
        for (const auto* annotation : method->invisibleLocalVariableAnnotations) {
            appendAnnotationTypes(annotation, methodAnnotationTypes);
        }
        if (method->annotationDefault) {
            appendAnnotationTypes(method->annotationDefault, methodAnnotationTypes);
        }
        for (const auto& type : methodAnnotationTypes) {
            TypeReference ref;
            ref.site       = declarationSite;
            ref.targetType = type;
            ref.kind       = TypeReferenceKind::Annotation;
            appendTypeReference(ref, true);
        }

        bool        hasLastStringConstant    = false;
        bool        hasReflectionClassTarget = false;
        bool        hasReflectionMemberName  = false;
        std::string lastStringConstant;
        std::string reflectionClassTarget;
        std::string reflectionMemberName;

        auto appendHandleReference = [&](const jbytecode::core::Handle& handle,
                                         const ReferenceSite&           site,
                                         MethodReferenceKind            methodKind) {
            TypeReference ownerRef;
            ownerRef.site       = site;
            ownerRef.targetType = str(handle.getOwner());
            ownerRef.kind       = TypeReferenceKind::MethodHandleOwner;
            appendTypeReference(ownerRef, true);

            if (isMethodHandleTag(handle.getTag())) {
                MethodReference ref;
                ref.site   = site;
                ref.target = {str(handle.getOwner()), str(handle.getName()), str(handle.getDesc())};
                ref.isInterface = handle.isInterface();
                ref.kind        = methodKind;
                appendMethodReference(ref);

                std::vector<std::string> descTypes;
                appendDescriptorTypes(handle.getDesc(), descTypes);
                for (const auto& type : descTypes) {
                    TypeReference descRef;
                    descRef.site       = site;
                    descRef.targetType = type;
                    descRef.kind       = TypeReferenceKind::MethodDescriptor;
                    appendTypeReference(descRef, true);
                }
            }
            else if (isFieldHandleTag(handle.getTag())) {
                FieldReference ref;
                ref.site   = site;
                ref.target = {str(handle.getOwner()), str(handle.getName()), str(handle.getDesc())};
                ref.kind   = FieldReferenceKind::MethodHandle;
                appendFieldReference(ref);

                std::vector<std::string> descTypes;
                appendDescriptorTypes(handle.getDesc(), descTypes);
                for (const auto& type : descTypes) {
                    TypeReference descRef;
                    descRef.site       = site;
                    descRef.targetType = type;
                    descRef.kind       = TypeReferenceKind::FieldDescriptor;
                    appendTypeReference(descRef, true);
                }
            }
        };

        for (auto* insn = method->instructions.getFirst(); insn;
             insn       = insn->getNext(), ++insnIndex) {
            if (insn->getType() == AbstractInsnNode::METHOD_INSN) {
                auto*           mi   = ca::core::tag_cast::cast<MethodInsnNode>(insn);
                ReferenceSite   site = makeSite(clazz, *method, insn, insnIndex);
                MethodReference ref;
                ref.site        = site;
                ref.target      = {str(mi->owner), str(mi->name), str(mi->desc)};
                ref.isInterface = mi->isInterface;
                appendMethodReference(ref);

                if (hasLastStringConstant && ref.target.owner == "java/lang/Class" &&
                    ref.target.name == "forName" &&
                    ref.target.desc == "(Ljava/lang/String;)Ljava/lang/Class;" &&
                    looksLikeClassName(lastStringConstant)) {
                    reflectionClassTarget    = normalizeClassName(lastStringConstant);
                    hasReflectionClassTarget = true;
                    hasReflectionMemberName  = false;
                    TypeReference reflectionRef;
                    reflectionRef.site       = site;
                    reflectionRef.targetType = reflectionClassTarget;
                    reflectionRef.kind       = TypeReferenceKind::ReflectionString;
                    appendTypeReference(reflectionRef, true);
                }
                else if (hasReflectionClassTarget && hasReflectionMemberName &&
                         isReflectionMethodLookup(*mi)) {
                    MethodReference reflectionRef;
                    reflectionRef.site   = site;
                    reflectionRef.target = {reflectionClassTarget, reflectionMemberName, ""};
                    reflectionRef.reflectionArgDesc = reflectionArgumentDescriptor(*mi);
                    reflectionRef.isInterface       = false;
                    reflectionRef.kind              = MethodReferenceKind::Reflection;
                    appendMethodReference(reflectionRef);
                    hasReflectionClassTarget = false;
                    hasReflectionMemberName  = false;
                }
                else if (hasReflectionClassTarget && isReflectionConstructorLookup(*mi)) {
                    MethodReference reflectionRef;
                    reflectionRef.site              = site;
                    reflectionRef.target            = {reflectionClassTarget, "<init>", ""};
                    reflectionRef.reflectionArgDesc = reflectionArgumentDescriptor(*mi);
                    reflectionRef.isInterface       = false;
                    reflectionRef.kind              = MethodReferenceKind::Reflection;
                    appendMethodReference(reflectionRef);
                    hasReflectionClassTarget = false;
                    hasReflectionMemberName  = false;
                }
                else if (hasReflectionClassTarget && hasReflectionMemberName &&
                         isReflectionFieldLookup(*mi)) {
                    FieldReference reflectionRef;
                    reflectionRef.site   = site;
                    reflectionRef.target = {reflectionClassTarget, reflectionMemberName, ""};
                    reflectionRef.kind   = FieldReferenceKind::Reflection;
                    appendFieldReference(reflectionRef);
                    hasReflectionClassTarget = false;
                    hasReflectionMemberName  = false;
                }
                hasLastStringConstant = false;

                //A member owner is also a class/type use. This keeps the
                //direct index useful for class reachability before call graph
                //and hierarchy resolution are layered on top.
                TypeReference typeRef;
                typeRef.site       = ref.site;
                typeRef.targetType = ref.target.owner;
                typeRef.kind       = TypeReferenceKind::MethodOwner;
                appendTypeReference(typeRef, true);

                std::vector<std::string> descTypes;
                appendDescriptorTypes(mi->desc, descTypes);
                for (const auto& type : descTypes) {
                    TypeReference descRef;
                    descRef.site       = ref.site;
                    descRef.targetType = type;
                    descRef.kind       = TypeReferenceKind::MethodDescriptor;
                    appendTypeReference(descRef, true);
                }
            }
            else if (insn->getType() == AbstractInsnNode::FIELD_INSN) {
                auto*          fi = ca::core::tag_cast::cast<FieldInsnNode>(insn);
                FieldReference ref;
                ref.site   = makeSite(clazz, *method, insn, insnIndex);
                ref.target = {str(fi->owner), str(fi->name), str(fi->desc)};
                appendFieldReference(ref);
                hasLastStringConstant = false;

                TypeReference typeRef;
                typeRef.site       = ref.site;
                typeRef.targetType = ref.target.owner;
                typeRef.kind       = TypeReferenceKind::FieldOwner;
                appendTypeReference(typeRef, true);

                std::vector<std::string> descTypes;
                appendDescriptorTypes(fi->desc, descTypes);
                for (const auto& type : descTypes) {
                    TypeReference descRef;
                    descRef.site       = ref.site;
                    descRef.targetType = type;
                    descRef.kind       = TypeReferenceKind::FieldDescriptor;
                    appendTypeReference(descRef, true);
                }
            }
            else if (insn->getType() == AbstractInsnNode::TYPE_INSN) {
                auto*         ti = ca::core::tag_cast::cast<TypeInsnNode>(insn);
                TypeReference ref;
                ref.site       = makeSite(clazz, *method, insn, insnIndex);
                ref.targetType = str(ti->desc);
                ref.kind       = TypeReferenceKind::TypeInstruction;
                appendTypeReference(ref, true);
                hasLastStringConstant = false;
            }
            else if (insn->getType() == AbstractInsnNode::MULTIANEWARRAY_INSN) {
                auto*         ma = ca::core::tag_cast::cast<MultiANewArrayInsnNode>(insn);
                TypeReference ref;
                ref.site       = makeSite(clazz, *method, insn, insnIndex);
                ref.targetType = str(ma->desc);
                ref.kind       = TypeReferenceKind::MultiANewArray;
                appendTypeReference(ref, true);

                std::vector<std::string> descTypes;
                appendDescriptorTypes(ma->desc, descTypes);
                for (const auto& type : descTypes) {
                    TypeReference descRef;
                    descRef.site       = ref.site;
                    descRef.targetType = type;
                    descRef.kind       = TypeReferenceKind::MultiANewArray;
                    appendTypeReference(descRef, true);
                }
                hasLastStringConstant = false;
            }
            else if (insn->getType() == AbstractInsnNode::LDC_INSN) {
                auto* ldc             = ca::core::tag_cast::cast<LdcInsnNode>(insn);
                hasLastStringConstant = false;
                if (auto* value = ldc->cst.as<std::string>()) {
                    lastStringConstant      = *value;
                    reflectionMemberName    = *value;
                    hasLastStringConstant   = true;
                    hasReflectionMemberName = true;
                    continue;
                }
                if (auto* value = ldc->cst.as<ca::str::Utf8StringRef>()) {
                    lastStringConstant      = value->to_std_string();
                    reflectionMemberName    = lastStringConstant;
                    hasLastStringConstant   = true;
                    hasReflectionMemberName = true;
                    continue;
                }
                if (auto* handle = ldc->cst.as<jbytecode::core::Handle>()) {
                    appendHandleReference(*handle,
                                          makeSite(clazz, *method, insn, insnIndex),
                                          MethodReferenceKind::MethodHandle);
                    continue;
                }
                if (auto* type = ldc->cst.as<jbytecode::core::Type>()) {
                    int sort = type->getSort();
                    if (sort != jbytecode::core::Type::OBJECT &&
                        sort != jbytecode::core::Type::ARRAY)
                        continue;
                    TypeReference ref;
                    ref.site       = makeSite(clazz, *method, insn, insnIndex);
                    ref.targetType = normalizeClassConstantType(*type);
                    ref.kind       = TypeReferenceKind::ClassConstant;
                    appendTypeReference(ref, true);
                    if (!hasReflectionMemberName && !isClassArrayStoreValue(insn)) {
                        reflectionClassTarget    = ref.targetType;
                        hasReflectionClassTarget = true;
                    }
                }
            }
            else if (insn->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN) {
                auto*         indy = ca::core::tag_cast::cast<InvokeDynamicInsnNode>(insn);
                ReferenceSite site = makeSite(clazz, *method, insn, insnIndex);
                appendHandleReference(indy->bsm, site, MethodReferenceKind::MethodHandle);

                MethodReferenceKind methodKind = isLambdaMetafactoryHandle(indy->bsm)
                                                     ? MethodReferenceKind::Lambda
                                                     : MethodReferenceKind::MethodHandle;
                for (const auto& arg : indy->bsmArgs) {
                    if (auto* handle = arg.as<jbytecode::core::Handle>()) {
                        appendHandleReference(*handle, site, methodKind);
                    }
                    else if (auto* type = arg.as<jbytecode::core::Type>()) {
                        int sort = type->getSort();
                        if (sort == jbytecode::core::Type::OBJECT ||
                            sort == jbytecode::core::Type::ARRAY) {
                            TypeReference ref;
                            ref.site       = site;
                            ref.targetType = normalizeClassConstantType(*type);
                            ref.kind       = TypeReferenceKind::ClassConstant;
                            appendTypeReference(ref, true);
                        }
                    }
                }
                hasLastStringConstant = false;
            }
            else {
                hasLastStringConstant = false;
            }
        }

        ReflectionInterpreter interpreter(aliases);
        auto                  snapshot =
            analyzeFrames<ReflectionValue>(&interpreter, clazz.name, *method);
        //fail-open（JSR/RET、畸形控制流等）：与迁移前 catch AnalyzerException
        //语义一致——保留 direct 与 lookup 事实，不追加 reflection 数据流事实。
        if (!snapshot.empty()) {
            const auto& frames = snapshot.frames();

            for (ca::i32 executionIndex = 0;
                 executionIndex < static_cast<ca::i32>(frames.size());
                 ++executionIndex) {
                auto* frame = frames[static_cast<ca::usize>(executionIndex)];
                auto* insn  = method->instructions.get(executionIndex);
                if (!frame || !insn || insn->getType() != AbstractInsnNode::METHOD_INSN) continue;

                auto*         methodInsn = ca::core::tag_cast::cast<MethodInsnNode>(insn);
                const ca::i32 argumentCount =
                    jbytecode::core::Type::getArgumentCount(methodInsn->desc);
                const ca::i32 receiverIndex = frame->getStackSize() - argumentCount - 1;
                if (receiverIndex < 0) continue;

                ReflectionValue receiver = frame->getStack(receiverIndex);
                if (aliases) {
                    const auto* alias = aliases->reflectionReceiverOf(
                        str(clazz.name), str(method->name), str(method->desc), executionIndex);
                    if (alias && alias->isKnown() && alias->identity->rfind("class:", 0) == 0) {
                        receiver.kind  = ReflectionValue::Kind::Class;
                        receiver.owner = alias->identity->substr(6);
                    }
                }
                ReflectionExecutionTarget target;
                if (!resolveReflectionExecution(*methodInsn, receiver, target)) {
                    continue;
                }

                ReferenceSite site = makeSite(clazz, *method, insn, executionIndex);
                if (target.kind == ReflectionExecutionTarget::Kind::Field) {
                    FieldReference ref;
                    ref.site   = std::move(site);
                    ref.target = {target.owner, target.name, target.desc};
                    ref.kind   = FieldReferenceKind::ReflectionAccess;
                    appendFieldReference(ref);
                }
                else {
                    MethodReference ref;
                    ref.site              = std::move(site);
                    ref.target            = {target.owner, target.name, target.desc};
                    ref.reflectionArgDesc = target.reflectionArgDesc;
                    ref.kind              = MethodReferenceKind::ReflectionExecution;
                    appendMethodReference(ref);
                }
            }
        }
    }
}

const std::vector<MethodReference>& ReferenceIndex::methodReferencesTo(
    const std::string& owner, const std::string& name, const std::string& desc) const
{
    auto it = methodRefsByTarget_.find(methodKey(owner, name, desc));
    return it == methodRefsByTarget_.end() ? kEmptyMethodRefs : it->second;
}

const std::vector<FieldReference>& ReferenceIndex::fieldReferencesTo(const std::string& owner,
                                                                     const std::string& name,
                                                                     const std::string& desc) const
{
    auto it = fieldRefsByTarget_.find(fieldKey(owner, name, desc));
    return it == fieldRefsByTarget_.end() ? kEmptyFieldRefs : it->second;
}

const std::vector<TypeReference>& ReferenceIndex::typeReferencesTo(
    const std::string& targetType) const
{
    auto it = typeRefsByTarget_.find(targetType);
    return it == typeRefsByTarget_.end() ? kEmptyTypeRefs : it->second;
}

const std::vector<MethodReference>& ReferenceIndex::methodReferencesFrom(
    const std::string& owner, const std::string& name, const std::string& desc) const
{
    auto it = methodRefsBySource_.find(methodKey(owner, name, desc));
    return it == methodRefsBySource_.end() ? kEmptyMethodRefs : it->second;
}

const std::vector<FieldReference>& ReferenceIndex::fieldReferencesFrom(
    const std::string& owner, const std::string& name, const std::string& desc) const
{
    auto it = fieldRefsBySource_.find(methodKey(owner, name, desc));
    return it == fieldRefsBySource_.end() ? kEmptyFieldRefs : it->second;
}

const std::vector<TypeReference>& ReferenceIndex::typeReferencesFrom(const std::string& owner,
                                                                     const std::string& name,
                                                                     const std::string& desc) const
{
    auto it = typeRefsBySource_.find(methodKey(owner, name, desc));
    return it == typeRefsBySource_.end() ? kEmptyTypeRefs : it->second;
}

const std::vector<MethodReference>& ReferenceIndex::methodReferencesFromClass(
    const std::string& owner) const
{
    auto it = methodRefsBySourceClass_.find(owner);
    return it == methodRefsBySourceClass_.end() ? kEmptyMethodRefs : it->second;
}

const std::vector<FieldReference>& ReferenceIndex::fieldReferencesFromClass(
    const std::string& owner) const
{
    auto it = fieldRefsBySourceClass_.find(owner);
    return it == fieldRefsBySourceClass_.end() ? kEmptyFieldRefs : it->second;
}

const std::vector<TypeReference>& ReferenceIndex::typeReferencesFromClass(
    const std::string& owner) const
{
    auto it = typeRefsBySourceClass_.find(owner);
    return it == typeRefsBySourceClass_.end() ? kEmptyTypeRefs : it->second;
}

const std::vector<MethodReference>& ReferenceIndex::methodReferencesToOwner(
    const std::string& owner) const
{
    auto it = methodRefsByTargetOwner_.find(owner);
    return it == methodRefsByTargetOwner_.end() ? kEmptyMethodRefs : it->second;
}

const std::vector<FieldReference>& ReferenceIndex::fieldReferencesToOwner(
    const std::string& owner) const
{
    auto it = fieldRefsByTargetOwner_.find(owner);
    return it == fieldRefsByTargetOwner_.end() ? kEmptyFieldRefs : it->second;
}

const MethodDeclaration* ReferenceIndex::declaredMethod(const std::string& owner,
                                                        const std::string& name,
                                                        const std::string& desc) const
{
    auto it = methodDeclsByKey_.find(methodKey(owner, name, desc));
    return it == methodDeclsByKey_.end() ? nullptr : &methodDecls_[it->second];
}

const FieldDeclaration* ReferenceIndex::declaredField(const std::string& owner,
                                                      const std::string& name,
                                                      const std::string& desc) const
{
    auto it = fieldDeclsByKey_.find(fieldKey(owner, name, desc));
    return it == fieldDeclsByKey_.end() ? nullptr : &fieldDecls_[it->second];
}

const std::vector<MethodDeclaration>& ReferenceIndex::declaredMethodsIn(
    const std::string& owner) const
{
    auto it = methodDeclsByOwner_.find(owner);
    return it == methodDeclsByOwner_.end() ? kEmptyMethodDecls : it->second;
}

const std::vector<FieldDeclaration>& ReferenceIndex::declaredFieldsIn(
    const std::string& owner) const
{
    auto it = fieldDeclsByOwner_.find(owner);
    return it == fieldDeclsByOwner_.end() ? kEmptyFieldDecls : it->second;
}

const ClassDeclaration* ReferenceIndex::declaredClass(const std::string& owner) const
{
    auto it = classDeclsByKey_.find(owner);
    return it == classDeclsByKey_.end() ? nullptr : &classDecls_[it->second];
}

}   //namespace mjt::base
