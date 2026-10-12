#pragma once

#include "mbase/asm/insn/class_util.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "jclassfile/io/class_file_reader.hpp"
#include "jclassfile/io/class_file_writer.hpp"
#include "jclassfile/transform/illegal_stripping_transformer.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/jbytecode.hpp"
#include "libca/core/tag_cast.hpp"
#include <exception>
#include <cstring>
#include <unordered_set>
#include <vector>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;
using namespace jbytecode::commons;
using namespace jbytecode::util;

namespace {

///混合清除(B7)：owner 非 null 且 owns 的节点交给 arena 回收，其余维持立即 delete。
template<class T>
void deleteAndClear(garena::Arena* owner, std::vector<T*>& values)
{
    for (auto* value : values) {
        if (!owner || !owner->owns(value)) delete value;
    }
    values.clear();
}

static void clearInsnAnnotations(garena::Arena* owner, AbstractInsnNode* insn)
{
    deleteAndClear(owner, insn->visibleTypeAnnotations);
    deleteAndClear(owner, insn->invisibleTypeAnnotations);
}

static void clearLineNumbers(garena::Arena* owner, InsnList& insns)
{
    auto* node = insns.getFirst();
    while (node) {
        auto* next = node->getNext();
        clearInsnAnnotations(owner, node);
        if (node->getType() == AbstractInsnNode::LINE) {
            insns.removeAndDelete(node);
        }
        node = next;
    }
}

static void clearFieldMeta(FieldNode& field)
{
    auto* owner = &field.arena();
    deleteAndClear(owner, field.visibleAnnotations);
    deleteAndClear(owner, field.invisibleAnnotations);
    deleteAndClear(owner, field.visibleTypeAnnotations);
    deleteAndClear(owner, field.invisibleTypeAnnotations);
    deleteAndClear(owner, field.attrs);
}

static void clearMethodMeta(MethodNode& method)
{
    auto* owner = &method.arena();
    deleteAndClear(owner, method.parameters);
    deleteAndClear(owner, method.localVariables);
    deleteAndClear(owner, method.visibleAnnotations);
    deleteAndClear(owner, method.invisibleAnnotations);
    deleteAndClear(owner, method.visibleTypeAnnotations);
    deleteAndClear(owner, method.invisibleTypeAnnotations);
    deleteAndClear(owner, method.visibleParameterAnnotations);
    deleteAndClear(owner, method.invisibleParameterAnnotations);
    method.visibleParameterIndices.clear();
    method.invisibleParameterIndices.clear();
    deleteAndClear(owner, method.visibleLocalVariableAnnotations);
    deleteAndClear(owner, method.invisibleLocalVariableAnnotations);
    deleteAndClear(owner, method.attrs);
    if (method.annotationDefault && !method.arena().owns(method.annotationDefault)) {
        delete method.annotationDefault;   //混合清除(B7)
        method.annotationDefault = nullptr;
    }
    clearLineNumbers(&method.arena(), method.instructions);
}

static void markLabelUsed(std::unordered_set<Label*>& usedLabels, Label* label)
{
    if (label) usedLabels.insert(label);
}

static void collectLocalVariableAnnotationLabels(
    std::unordered_set<Label*>&                      usedLabels,
    const std::vector<LocalVariableAnnotationNode*>& annotations)
{
    for (const auto* annotation : annotations) {
        if (!annotation) continue;
        for (auto* label : annotation->start) markLabelUsed(usedLabels, label);
        for (auto* label : annotation->end) markLabelUsed(usedLabels, label);
    }
}

static void collectReferencedLabels(const MethodNode&           method,
                                    std::unordered_set<Label*>& usedLabels)
{
    for (auto* node = method.instructions.getFirst(); node; node = node->getNext()) {
        switch (node->getType()) {
        case AbstractInsnNode::JUMP_INSN:
            markLabelUsed(usedLabels, ca::core::tag_cast::cast<JumpInsnNode*>(node)->label);
            break;
        case AbstractInsnNode::TABLESWITCH_INSN:
        {
            auto* switchInsn = ca::core::tag_cast::cast<TableSwitchInsnNode*>(node);
            markLabelUsed(usedLabels, switchInsn->dflt);
            for (auto* label : switchInsn->labels) markLabelUsed(usedLabels, label);
            break;
        }
        case AbstractInsnNode::LOOKUPSWITCH_INSN:
        {
            auto* switchInsn = ca::core::tag_cast::cast<LookupSwitchInsnNode*>(node);
            markLabelUsed(usedLabels, switchInsn->dflt);
            for (auto* label : switchInsn->labels) markLabelUsed(usedLabels, label);
            break;
        }
        case AbstractInsnNode::LINE:
            markLabelUsed(usedLabels, ca::core::tag_cast::cast<LineNumberNode*>(node)->start);
            break;
        case AbstractInsnNode::FRAME:
        {
            auto* frame = ca::core::tag_cast::cast<FrameNode*>(node);
            InsnUtil::collectFrameLabels(frame->local, usedLabels);
            InsnUtil::collectFrameLabels(frame->stack, usedLabels);
            break;
        }
        default: break;
        }
    }

    for (const auto* tryCatchBlock : method.tryCatchBlocks) {
        if (!tryCatchBlock) continue;
        markLabelUsed(usedLabels, tryCatchBlock->start);
        markLabelUsed(usedLabels, tryCatchBlock->end);
        markLabelUsed(usedLabels, tryCatchBlock->handler);
    }
    for (const auto* localVariable : method.localVariables) {
        if (!localVariable) continue;
        markLabelUsed(usedLabels, localVariable->start);
        markLabelUsed(usedLabels, localVariable->end);
    }
    collectLocalVariableAnnotationLabels(usedLabels, method.visibleLocalVariableAnnotations);
    collectLocalVariableAnnotationLabels(usedLabels, method.invisibleLocalVariableAnnotations);

    auto* first = method.instructions.getFirst();
    if (first && first->getType() == AbstractInsnNode::LABEL) {
        markLabelUsed(usedLabels, ca::core::tag_cast::cast<LabelNode*>(first)->label);
    }
    auto* last = method.instructions.getLast();
    if (last && last->getType() == AbstractInsnNode::LABEL) {
        markLabelUsed(usedLabels, ca::core::tag_cast::cast<LabelNode*>(last)->label);
    }
}

}   //namespace

//========== NopRemover ==========

//========== IllegalAccessRemover ==========

//各类型不允许出现的 access flags（对齐 Java clean/IllegalAccessRemover.java 的意图）。
//JVM/ASM 的 access flags 存在上下文别名：例如 0x0020 在 class 上是 ACC_SUPER，
//在 method 上是 ACC_SYNCHRONIZED，在 module requires 上是 ACC_TRANSITIVE。
//这里按当前成员上下文过滤掉会误伤合法语义的别名位。
static constexpr int ILLEGAL_CLASS_ACCESS[] = {Opcodes::ACC_STATIC,
                                               Opcodes::ACC_VOLATILE,
                                               Opcodes::ACC_VARARGS,
                                               Opcodes::ACC_NATIVE,
                                               Opcodes::ACC_STRICT};

static constexpr int ILLEGAL_METHOD_ACCESS[] = {Opcodes::ACC_INTERFACE,
                                                Opcodes::ACC_ANNOTATION,
                                                Opcodes::ACC_ENUM,
                                                Opcodes::ACC_MODULE,
                                                Opcodes::ACC_RECORD};

static constexpr int ILLEGAL_FIELD_ACCESS[] = {Opcodes::ACC_SUPER,
                                               Opcodes::ACC_NATIVE,
                                               Opcodes::ACC_INTERFACE,
                                               Opcodes::ACC_ABSTRACT,
                                               Opcodes::ACC_STRICT,
                                               Opcodes::ACC_ANNOTATION,
                                               Opcodes::ACC_MODULE,
                                               Opcodes::ACC_RECORD};

static constexpr int ILLEGAL_PARAM_ACCESS[] = {
    Opcodes::ACC_PUBLIC,    Opcodes::ACC_PRIVATE,      Opcodes::ACC_PROTECTED,
    Opcodes::ACC_STATIC,    Opcodes::ACC_SUPER,        Opcodes::ACC_SYNCHRONIZED,
    Opcodes::ACC_OPEN,      Opcodes::ACC_TRANSITIVE,   Opcodes::ACC_VOLATILE,
    Opcodes::ACC_BRIDGE,    Opcodes::ACC_STATIC_PHASE, Opcodes::ACC_VARARGS,
    Opcodes::ACC_TRANSIENT, Opcodes::ACC_NATIVE,       Opcodes::ACC_INTERFACE,
    Opcodes::ACC_ABSTRACT,  Opcodes::ACC_STRICT,       Opcodes::ACC_ANNOTATION,
    Opcodes::ACC_ENUM,      Opcodes::ACC_RECORD,       Opcodes::ACC_DEPRECATED};

//========== MetaInfoRemover ==========

//========== InvalidTryCatchRemover ==========

namespace {

//从指令列表中查找指定 Label 对应的 LabelNode
static LabelNode* findLabelNode(InsnList& insns, jbytecode::core::Label* label)
{
    for (auto* node = insns.getFirst(); node; node = node->getNext()) {
        if (node->getType() == AbstractInsnNode::LABEL) {
            auto* ln = ca::core::tag_cast::cast<LabelNode*>(node);
            if (ln->label == label) return ln;
        }
    }
    return nullptr;
}

//判断 start 和 end 标签之间是否有实际指令（无实际指令则 try-catch 范围为空）
static bool isEmptyTryRange(InsnList& insns, jbytecode::core::Label* startLabel,
                            jbytecode::core::Label* endLabel)
{
    bool foundStart = false;
    for (auto* node = insns.getFirst(); node; node = node->getNext()) {
        if (node->getType() == AbstractInsnNode::LABEL) {
            auto* ln = ca::core::tag_cast::cast<LabelNode*>(node);
            if (!foundStart && ln->label == startLabel) {
                foundStart = true;
                continue;
            }
            if (foundStart && ln->label == endLabel) {
                return true;   //到达 end 时未发现任何实际指令
            }
        }
        if (foundStart) {
            auto t = node->getType();
            if (t != AbstractInsnNode::LABEL && t != AbstractInsnNode::LINE &&
                t != AbstractInsnNode::FRAME) {
                return false;   //找到了实际指令
            }
        }
    }
    return false;
}

}   //namespace

//========== SpecificAnnotationRemover ==========

namespace {

//硬编码的黑名单注解描述符列表（与 Java 活代码完全一致）
static constexpr const char* ANNOTATION_BLACKLIST[] = {
    "Ltech/origincloud/OriginShieldMetadata;",
    "Ljvavx/annotation/Nullable;",
    "Ljvavx/annotation/NotNull;",
    "Ltech/origincloud/obf/ObfuscationSourceDebug;",
    "Ltech/origincloud/Mapping;",
    "Ltech/origincloud/obf/OriginShield;",
    "Ltech/skidonion/obfuscator/annotations/NativeObfuscation$Inline;",
    "Ltech/origincloud/obf/ObfuscationMetadata;",
    "Ltech/origincloud/obf/OriginShield;",
    "Ljvavx/annotation/NotNull;",
    "Ltech/skidonion/obfuscator/annotations/NativeObfuscation;",
    "Ljvavx/annotation/WhoCaresNull;",
    "Lorg/jetbrains/annotations/Nullable;",
    "Lorg/jetbrains/annotations/NotNull;",
    "Ltech/origincloud/obf/Mapping;",
    "Lorg/jetbrains/annotations/NullableOrNotNull;",
    "Ljvavx/annotation/NullableOrNotNull;",
    "Lorg/jetbrains/annotations/WhoCaresNull;"};

static bool isInBlacklist(const ca::str::Utf8StringRef& desc)
{
    for (const auto* blackDesc : ANNOTATION_BLACKLIST) {
        if (desc == ca::str::Utf8StringRef::from_cstr(blackDesc)) {
            return true;
        }
    }
    return false;
}

static bool shouldRemoveAnnotation(AnnotationNode* annotation)
{
    if (!annotation) return true;
    auto& desc = annotation->desc;
    //Java 谓词：desc == null
    if (desc.is_empty()) return true;
    //desc.length <= 3
    if (desc.length() <= 3) return true;
    //不是以 L 开头
    if (desc.byte_length() < 1 || desc.byte_at(0) != 'L') return true;
    //不是以 ; 结尾
    if (desc.byte_length() < 1 || desc.byte_at(desc.byte_length() - 1) != ';') return true;
    //包含换行符、空字符或空格（用 index_of 搜索码点）
    if (desc.index_of(static_cast<ca::u32>('\n')) != ca::str::Utf8StringRef::npos) return true;
    if (desc.index_of(static_cast<ca::u32>('\0')) != ca::str::Utf8StringRef::npos) return true;
    if (desc.index_of(static_cast<ca::u32>(' ')) != ca::str::Utf8StringRef::npos) return true;
    //在黑名单中
    if (isInBlacklist(desc)) return true;
    return false;
}

//从 vector 中移除符合谓词的 AnnotationNode，返回是否移除了任何节点
static bool removeAnnotationsIf(garena::Arena& owner, std::vector<AnnotationNode*>& annotations)
{
    bool removed = false;
    auto it      = annotations.begin();
    while (it != annotations.end()) {
        if (shouldRemoveAnnotation(*it)) {
            if (!owner.owns(*it)) delete *it;   //混合清除(B7)
            it      = annotations.erase(it);
            removed = true;
        }
        else {
            ++it;
        }
    }
    return removed;
}

}   //namespace

//========== IllegalClassFileCleaner ==========

//========== IllegalMetaInfoRemover container ==========

namespace {

static bool isMalformedAnnotationDesc(const ca::str::Utf8StringRef& desc)
{
    if (desc.is_empty()) return true;
    if (desc.length() <= 3) return true;
    if (desc.byte_at(0) != 'L') return true;
    if (desc.byte_at(desc.byte_length() - 1) != ';') return true;
    if (desc.index_of(static_cast<ca::u32>('\n')) != ca::str::Utf8StringRef::npos) return true;
    if (desc.index_of(static_cast<ca::u32>('\0')) != ca::str::Utf8StringRef::npos) return true;
    if (desc.index_of(static_cast<ca::u32>(' ')) != ca::str::Utf8StringRef::npos) return true;
    return false;
}

static bool removeMalformedAnnotations(garena::Arena&             owner,
                                       std::vector<AnnotationNode*>& annotations)
{
    bool modified = false;
    auto it       = annotations.begin();
    while (it != annotations.end()) {
        if (!*it || isMalformedAnnotationDesc((*it)->desc)) {
            if (!owner.owns(*it)) delete *it;   //混合清除(B7)
            it       = annotations.erase(it);
            modified = true;
        }
        else {
            ++it;
        }
    }
    return modified;
}

enum class SignatureKind
{
    Class,
    Method,
    Field
};

static bool isValidSignature(const ca::str::Utf8StringRef& signature, SignatureKind kind)
{
    if (signature.is_empty()) return true;
    try {
        SignatureVisitor visitor(Opcodes::ASM10_EXPERIMENTAL);
        SignatureReader  reader(signature);
        if (kind == SignatureKind::Field) {
            reader.acceptType(&visitor);
        }
        else {
            reader.accept(&visitor);
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

static bool clearInvalidSignature(ca::str::Utf8StringRef& signature, SignatureKind kind)
{
    if (signature.is_empty() || isValidSignature(signature, kind)) return false;
    signature = ca::str::Utf8StringRef{};
    return true;
}

static bool removeUnknownAttributes(std::vector<Attribute*>& attrs)
{
    bool modified = false;
    auto it       = attrs.begin();
    while (it != attrs.end()) {
        if (!*it || (*it)->isUnknown()) {
            delete *it;
            it       = attrs.erase(it);
            modified = true;
        }
        else {
            ++it;
        }
    }
    return modified;
}

static bool endsWithJava(const ca::str::Utf8StringRef& value)
{
    static constexpr const char* suffix    = ".java";
    static constexpr ca::usize   suffixLen = 5;
    if (value.byte_length() < suffixLen) return false;
    ca::usize offset = value.byte_length() - suffixLen;
    for (ca::usize i = 0; i < suffixLen; i++) {
        if (value.byte_at(offset + i) != static_cast<unsigned char>(suffix[i])) return false;
    }
    return true;
}

static bool isAsciiAlnum(ca::u32 c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static bool isIllegalDebugInfo(const ca::str::Utf8StringRef& info)
{
    if (info.is_empty()) return false;
    if (info.length() > 100) return true;
    if (!endsWithJava(info)) return true;
    ca::usize baseLen = info.byte_length() - 5;
    if (baseLen == 0) return true;
    ca::u32 first = info.byte_at(0);
    if (first >= '0' && first <= '9') return true;
    for (ca::usize i = 0; i < baseLen; i++) {
        if (!isAsciiAlnum(info.byte_at(i))) return true;
    }
    return false;
}

static bool hasArrayLastArgument(const ca::str::Utf8StringRef& desc)
{
    try {
        auto args = Type::getArgumentTypes(desc);
        return !args.empty() && args.back().getSort() == Type::ARRAY;
    }
    catch (...) {
        return false;
    }
}

}   //namespace

//========== UnusedLabelRemover ==========

//============================================================================
//MetaInfoRemover 容器拆解：10 个子 pass，对齐 Java clean/MetaInfoRemover.java。
//============================================================================

//Java MetaInfoRemover.FinalRemover：遍历所有字段，移除 ACC_FINAL。
//从注解 vector 中删 desc 在白名单内的注解，返回是否删过（对齐 Java stream filter）。
static bool removeAnnotationsInList(garena::Arena&                owner,
                                    std::vector<AnnotationNode*>& annotations,
                                    bool (*match)(const ca::str::Utf8StringRef&))
{
    bool removed = false;
    auto it      = annotations.begin();
    while (it != annotations.end()) {
        if (!*it || match((*it)->desc)) {
            if (!owner.owns(*it)) delete *it;   //混合清除(B7)
            it      = annotations.erase(it);
            removed = true;
        }
        else {
            ++it;
        }
    }
    return removed;
}

//判定是否为 @kotlin/Metadata（对齐 Java !desc.equals("Lkotlin/Metadata;") 的反向）。
static bool isKotlinMetadata(const ca::str::Utf8StringRef& desc)
{
    return desc.equals(ca::str::Utf8StringRef::from_cstr("Lkotlin/Metadata;"));
}

//判定是否为 jetbrains 三注解之一（对齐 Java annotations.contains(desc)）。
static bool isJetbrainsAnnotation(const ca::str::Utf8StringRef& desc)
{
    return desc.equals(ca::str::Utf8StringRef::from_cstr("Lorg/jetbrains/annotations/NotNull;")) ||
           desc.equals(ca::str::Utf8StringRef::from_cstr("Lorg/jetbrains/annotations/JvmName;")) ||
           desc.equals(ca::str::Utf8StringRef::from_cstr("Lorg/jetbrains/annotations/Nullable;"));
}

//Java MetaInfoRemover.KotlinMetadataRemover：类级删 @kotlin/Metadata；方法/字段删 jetbrains
//三注解。
//Java MetaInfoRemover.SourceInfoRemover：sourceFile/sourceDebug 置空。
//Java MetaInfoRemover.LineNumberRemover：遍历指令删 LineNumberNode。
//Java MetaInfoRemover.SyntheticBridgeRemover：类/方法/字段去 synthetic，方法再去 bridge。
//Java MetaInfoRemover.LocalVariableInfoRemover：localVariables 置 null（C++：清空并释放）。
//Java MetaInfoRemover.ParametersInfoRemover：parameters 置 null（C++：清空并释放）。
//Java MetaInfoRemover.ExceptionInfoRemover：exceptions 置 null（C++：清空 vector）。
//Java String.hashCode：s[0]*31^(n-1) + ... + s[n-1]，按 char（UTF-16 码元）。
//注意 Java 用 char（16 位）；此处按 ASCII 字节路径计算（indy name 通常是 ASCII），对齐语义。
static int javaStringHashCode(const ca::str::Utf8StringRef& s)
{
    int  h   = 0;
    auto len = s.byte_length();
    for (ca::usize i = 0; i < len; i++) {
        h = 31 * h + static_cast<unsigned char>(s.byte_at(i));
    }
    return h;
}

//Java MetaInfoRemover.LongIndyNameOptimizer：name 长度 > 64 则改为 "indyMethod"+hashCode。
//Java MetaInfoRemover.SignatureRemover：类/方法/字段 signature 置空。

}   //namespace mjt::base
