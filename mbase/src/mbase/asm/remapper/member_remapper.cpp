#include "mbase/asm/remapper/member_remapper.hpp"

#include "jbytecode/core/type.hpp"
#include "mbase/analysis/alias_analysis.hpp"
#include "mbase/asm/analyzer/dataflow_analyzer.hpp"
#include "mbase/reference/reflection_interpreter.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {
namespace {

using namespace jbytecode::core;
using namespace jbytecode::tree;

bool assignIfChanged(ca::str::Utf8StringRef& target, ca::str::Utf8StringArena& arena,
                     ca::str::Utf8StringRef value)
{
    if (target.equals(value)) return false;
    target = arena.intern(value);
    return true;
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

///owner 槽统一映射入口：直接类名走 mapInternalName；`[L...;`/`[[L...;` 数组型
///owner（enum values 的 clone 引用等）按字段描述符整体重映射，元素类名随类
///映射同步改写。只认直接类名的旧路径在类改名后会把数组型 owner 原样留下——
///receiver（getstatic 拿到的 `[L新名;`）与 owner（`[L旧名;`）类型失配，真机
///一调即炸，且 entry 探针与 -Xverify:all 都抓不到（一个只看入口，一个不执行
///方法体）。
ca::str::Utf8StringRef mapOwnerRef(const MemberRemapPlan& plan, ca::str::Utf8StringRef owner)
{
    if (!owner.is_empty() && owner.byte_at(0) == '[') return plan.mapFieldDescriptor(owner);
    return plan.mapInternalName(owner);
}

std::string toInternalReflectionName(const std::string& value)
{
    std::string internal = value;
    if (internal.size() >= 2 && internal.front() == 'L' && internal.back() == ';') {
        internal = internal.substr(1, internal.size() - 2);
    }
    for (auto& c : internal) {
        if (c == '.') c = '/';
    }
    return internal;
}

std::string restoreReflectionNameShape(const std::string& original, const std::string& mapped)
{
    std::string value = mapped;
    if (original.find('.') != std::string::npos && original.find('/') == std::string::npos) {
        for (auto& c : value) {
            if (c == '/') c = '.';
        }
    }
    if (original.size() >= 2 && original.front() == 'L' && original.back() == ';') {
        return "L" + value + ";";
    }
    return value;
}

std::string remapReflectionClassName(const MemberRemapPlan& plan, const std::string& value)
{
    if (!looksLikeClassName(value)) return value;
    const std::string internal = toInternalReflectionName(value);
    auto              mapped   = plan.mapInternalName(ca::str::Utf8StringRef::from_cstr(internal.c_str()));
    if (mapped.equals(ca::str::Utf8StringRef::from_cstr(internal.c_str()))) return value;
    return restoreReflectionNameShape(value, mapped.to_std_string());
}

std::string remapSignatureWith(const MemberRemapPlan& plan, const std::string& signature)
{
    std::string out;
    out.reserve(signature.size());
    for (ca::usize i = 0; i < signature.size(); ++i) {
        char c = signature[i];
        out.push_back(c);
        if (c != 'L') continue;

        ca::usize start = i + 1;
        ca::usize end   = start;
        while (end < signature.size() && signature[end] != ';' && signature[end] != '<') {
            end++;
        }
        if (end <= start) continue;

        const std::string oldName = signature.substr(start, end - start);
        auto              newName = plan.mapInternalName(ca::str::Utf8StringRef::from_cstr(oldName.c_str()));
        out.append(newName.data(), newName.data() + newName.byte_length());
        i = end - 1;
    }
    return out;
}

bool assignSignatureIfChanged(ca::str::Utf8StringRef& target, ca::str::Utf8StringArena& arena,
                              const MemberRemapPlan& plan)
{
    if (target.byte_length() == 0) return false;
    return assignIfChanged(target, arena,
                           ca::str::Utf8StringRef::from_cstr(
                               remapSignatureWith(plan, target.to_std_string()).c_str()));
}

bool remapAnnotation(AnnotationNode& annotation, ca::str::Utf8StringArena& arena,
                     const MemberRemapPlan& plan)
{
    bool changed = false;
    if (annotation.desc.byte_length() > 0) {
        changed |= assignIfChanged(annotation.desc, arena, plan.mapFieldDescriptor(annotation.desc));
    }
    for (auto& element : annotation.elements) {
        if ((element.tag == '@' || element.tag == '[') && element.nested) {
            changed |= remapAnnotation(*element.nested, arena, plan);
        }
        else if (element.tag == 'e') {
            auto      value = element.sval.ref();
            ca::usize space = value.byte_length();
            for (ca::usize i = 0; i < value.byte_length(); ++i) {
                if (value.byte_at(i) == ' ') {
                    space = i;
                    break;
                }
            }
            if (space < value.byte_length()) {
                auto descRef = value.slice(0, space);
                auto nameRef =
                    value.slice(space + 1, value.byte_length() - space - 1);
                auto mapped = plan.mapFieldDescriptor(descRef);
                if (!mapped.equals(descRef)) {
                    std::string text = mapped.to_std_string() + " " + nameRef.to_std_string();
                    element.sval     = ca::str::Utf8String(
                        reinterpret_cast<const unsigned char*>(text.data()), text.size());
                    changed = true;
                }
            }
        }
        else if (element.tag == 'c') {
            auto descRef = element.sval.ref();
            auto mapped  = plan.mapFieldDescriptor(descRef);
            if (!mapped.equals(descRef)) {
                element.sval = ca::str::Utf8String(
                    reinterpret_cast<const unsigned char*>(mapped.data()),
                    mapped.byte_length());
                changed = true;
            }
        }
    }
    return changed;
}

bool remapAnnotations(const std::vector<AnnotationNode*>& annotations,
                      ca::str::Utf8StringArena& arena, const MemberRemapPlan& plan)
{
    bool changed = false;
    for (auto* annotation : annotations) {
        if (!annotation) continue;
        changed |= remapAnnotation(*annotation, arena, plan);
    }
    return changed;
}

bool readLdcString(LdcInsnNode& ldcInsn, std::string& value)
{
    if (auto* text = ldcInsn.cst.as<std::string>()) {
        value = *text;
        return true;
    }
    if (auto* text = ldcInsn.cst.as<ca::str::Utf8StringRef>()) {
        value = text->to_std_string();
        return true;
    }
    return false;
}

void assignLdcString(LdcInsnNode& ldcInsn, ca::str::Utf8StringArena& arena,
                     ca::str::Utf8StringRef value)
{
    ///原 LDC cst 是 Utf8StringRef 还是 std::string 已无区别：映射后的新串一律 intern 进
    ///调用方注入的作业级 arena（applyMemberRemapPlan ← gam.arena），避免 writer 边界
    ///fallback re-intern 丢失去重收益。
    ldcInsn.cst = ca::core::Any(arena.intern(value));
}

std::string readReflectionClassLiteralOwner(LdcInsnNode& ldcInsn)
{
    auto* type = ldcInsn.cst.as<Type>();
    if (!type) return {};

    const int sort = type->getSort();
    if (sort != Type::OBJECT && sort != Type::ARRAY) return {};
    //staticArena 退役：内部名仅瞬时转 std::string（ARRAY 排序会 intern），
    //函数级局部 arena 零驻留。
    ca::str::Utf8StringArena scratch;
    std::string              owner = type->getInternalName(scratch).to_std_string();
    if (owner.size() >= 2 && owner.front() == 'L' && owner.back() == ';') {
        owner = owner.substr(1, owner.size() - 2);
    }
    return owner;
}

bool isReflectionClassForName(const MethodInsnNode& insn)
{
    return insn.owner.equals(ca::str::Utf8StringRef::from_cstr("java/lang/Class")) &&
           insn.name.equals(ca::str::Utf8StringRef::from_cstr("forName")) &&
           insn.desc.equals(
               ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;)Ljava/lang/Class;"));
}

bool isReflectionMethodLookup(const MethodInsnNode& insn)
{
    return insn.owner.equals(ca::str::Utf8StringRef::from_cstr("java/lang/Class")) &&
           (insn.name.equals(ca::str::Utf8StringRef::from_cstr("getDeclaredMethod")) ||
            insn.name.equals(ca::str::Utf8StringRef::from_cstr("getMethod"))) &&
           insn.desc.equals(ca::str::Utf8StringRef::from_cstr(
               "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;"));
}

bool isReflectionFieldLookup(const MethodInsnNode& insn)
{
    return insn.owner.equals(ca::str::Utf8StringRef::from_cstr("java/lang/Class")) &&
           (insn.name.equals(ca::str::Utf8StringRef::from_cstr("getDeclaredField")) ||
            insn.name.equals(ca::str::Utf8StringRef::from_cstr("getField"))) &&
           insn.desc.equals(
               ca::str::Utf8StringRef::from_cstr("(Ljava/lang/String;)Ljava/lang/reflect/Field;"));
}

bool remapReflectionMemberNames(MethodNode& method, ca::str::Utf8StringArena& arena,
                                const MemberRemapPlan& plan, const std::string& owner,
                                const AliasAnalysis* aliases)
{
    bool         changed = false;
    std::string  reflectionOwner;
    LdcInsnNode* lastStringLdc = nullptr;
    std::string  lastString;

    for (auto* insn : method.instructions.toArray()) {
        if (!insn) continue;

        if (insn->getType() == AbstractInsnNode::LDC_INSN) {
            auto*       ldc = ca::core::tag_cast::cast<LdcInsnNode*>(insn);
            std::string text;
            if (readLdcString(*ldc, text)) {
                lastStringLdc = ldc;
                lastString    = text;
                continue;
            }

            const std::string literalOwner = readReflectionClassLiteralOwner(*ldc);
            if (!literalOwner.empty()) {
                if (reflectionOwner.empty() || !lastStringLdc) {
                    reflectionOwner = literalOwner;
                    lastStringLdc   = nullptr;
                    lastString.clear();
                }
            }
            continue;
        }

        if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
        auto* call = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
        if (isReflectionClassForName(*call)) {
            if (lastStringLdc && looksLikeClassName(lastString)) {
                reflectionOwner = toInternalReflectionName(lastString);
                const std::string mapped = remapReflectionClassName(plan, lastString);
                if (mapped != lastString) {
                    assignLdcString(*lastStringLdc,
                                    arena,
                                    ca::str::Utf8StringRef::from_cstr(mapped.c_str()));
                    changed = true;
                }
            }
            lastStringLdc = nullptr;
            lastString.clear();
            continue;
        }

        if (!reflectionOwner.empty() && lastStringLdc && isReflectionMethodLookup(*call)) {
            auto ownerRef = ca::str::Utf8StringRef::from_cstr(reflectionOwner.c_str());
            auto nameRef  = ca::str::Utf8StringRef::from_cstr(lastString.c_str());
            if (const auto* rule = plan.findMethodByName(ownerRef, nameRef)) {
                if (!rule->newName.equals(nameRef)) {
                    assignLdcString(*lastStringLdc, arena, rule->newName);
                    changed = true;
                }
            }
            reflectionOwner.clear();
            lastStringLdc = nullptr;
            lastString.clear();
            continue;
        }

        if (!reflectionOwner.empty() && lastStringLdc && isReflectionFieldLookup(*call)) {
            auto ownerRef = ca::str::Utf8StringRef::from_cstr(reflectionOwner.c_str());
            auto nameRef  = ca::str::Utf8StringRef::from_cstr(lastString.c_str());
            if (const auto* rule = plan.findFieldByName(ownerRef, nameRef)) {
                if (!rule->newName.equals(nameRef)) {
                    assignLdcString(*lastStringLdc, arena, rule->newName);
                    changed = true;
                }
            }
            reflectionOwner.clear();
            lastStringLdc = nullptr;
            lastString.clear();
        }

        //A linear literal pair is only valid until the next unrelated call.
        //Cross-call propagation belongs to AliasAnalysis; retaining this
        //state here would guess across container mutations and wrappers.
        reflectionOwner.clear();
        lastStringLdc = nullptr;
        lastString.clear();
    }
    if (!aliases) return changed;

    ReflectionInterpreter                   interpreter(aliases);
    auto                                    snapshot = analyzeFrames<ReflectionValue>(
        &interpreter, ca::str::Utf8StringRef::from_cstr(owner.c_str()), method);
    //fail-open：JSR/RET、畸形控制流等不可分析时直接保留已改写结果，
    //与迁移前 catch (...) 返回 changed 语义一致。
    if (snapshot.empty()) return changed;
    const auto& frames = snapshot.frames();

    const auto instructions = method.instructions.toArray();
    for (ca::i32 index = 0; index < static_cast<ca::i32>(instructions.size()); ++index) {
        auto* insn = instructions[static_cast<ca::usize>(index)];
        if (!insn || insn->getType() != AbstractInsnNode::METHOD_INSN ||
            index >= static_cast<ca::i32>(frames.size()))
            continue;
        auto* frame = frames[static_cast<ca::usize>(index)];
        if (!frame) continue;
        auto*      call         = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
        const bool methodLookup = isReflectionMethodLookup(*call);
        const bool fieldLookup  = isReflectionFieldLookup(*call);
        if (!methodLookup && !fieldLookup) continue;
        const ca::i32 receiverIndex =
            frame->getStackSize() - Type::getArgumentCount(call->desc) - 1;
        if (receiverIndex < 0 || receiverIndex + 1 >= frame->getStackSize()) continue;
        auto receiver = frame->getStack(receiverIndex);
        if (const auto* alias = aliases->reflectionReceiverOf(
                owner, method.name.to_std_string(), method.desc.to_std_string(), index)) {
            if (alias->isKnown() && alias->identity->rfind("class:", 0) == 0) {
                receiver.kind  = ReflectionValue::Kind::Class;
                receiver.owner = alias->identity->substr(6);
            }
        }
        const auto member = frame->getStack(receiverIndex + 1);
        if (receiver.kind != ReflectionValue::Kind::Class ||
            member.kind != ReflectionValue::Kind::String)
            continue;

        auto ownerRef = ca::str::Utf8StringRef::from_cstr(receiver.owner.c_str());
        auto nameRef  = ca::str::Utf8StringRef::from_cstr(member.name.c_str());
        ca::str::Utf8StringRef mappedName;
        if (methodLookup) {
            const auto* rule = plan.findMethodByName(ownerRef, nameRef);
            if (rule) mappedName = rule->newName;
        }
        else {
            const auto* rule = plan.findFieldByName(ownerRef, nameRef);
            if (rule) mappedName = rule->newName;
        }
        if (mappedName.is_empty() || mappedName.equals(nameRef)) continue;

        for (ca::i32 cursor = index - 1; cursor >= 0; --cursor) {
            auto* candidate = instructions[static_cast<ca::usize>(cursor)];
            if (candidate->getType() == AbstractInsnNode::LABEL) break;
            if (candidate->getType() != AbstractInsnNode::LDC_INSN) continue;
            auto*       ldc = ca::core::tag_cast::cast<LdcInsnNode*>(candidate);
            std::string text;
            if (readLdcString(*ldc, text) && text == member.name) {
                assignLdcString(*ldc, arena, mappedName);
                changed = true;
                break;
            }
        }
    }
    return changed;
}

std::string innerSimpleName(const std::string& internalName)
{
    const auto dollar = internalName.rfind('$');
    if (dollar == std::string::npos || dollar + 1 >= internalName.size()) return {};
    return internalName.substr(dollar + 1);
}

bool remapInnerClassMetadata(InnerClassNode& innerClass, ca::str::Utf8StringArena& arena,
                             const MemberRemapPlan& plan)
{
    bool changed = false;
    auto oldName = innerClass.name;
    auto newName = plan.mapInternalName(oldName);
    changed |= assignIfChanged(innerClass.name, arena, newName);

    if (!innerClass.outerName.is_empty()) {
        changed |= assignIfChanged(innerClass.outerName, arena,
                                   plan.mapInternalName(innerClass.outerName));
    }

    if (!innerClass.innerName.is_empty() && !newName.equals(oldName)) {
        const std::string oldSimpleStr = innerSimpleName(oldName.to_std_string());
        const std::string newSimpleStr = innerSimpleName(newName.to_std_string());
        auto oldSimple = ca::str::Utf8StringRef::from_cstr(oldSimpleStr.c_str());
        auto newSimple = ca::str::Utf8StringRef::from_cstr(newSimpleStr.c_str());
        if (!oldSimple.is_empty() && !newSimple.is_empty() &&
            innerClass.innerName.equals(oldSimple)) {
            changed |= assignIfChanged(innerClass.innerName, arena, newSimple);
        }
    }
    return changed;
}

std::string recordComponentMappedName(const MemberRemapPlan& plan, ca::str::Utf8StringRef owner,
                                      ca::str::Utf8StringRef name, ca::str::Utf8StringRef desc)
{
    const FieldRemapRule* fieldRule = plan.findField(owner, name, desc);
    std::string           accessorDescBuf = "()";
    accessorDescBuf.append(reinterpret_cast<const char*>(desc.data()), desc.byte_length());
    auto                  accessorDesc    = ca::str::Utf8StringRef::from_cstr(accessorDescBuf.c_str());
    const MethodRemapRule* methodRule     = plan.findMethod(owner, name, accessorDesc);
    if (fieldRule && methodRule && !fieldRule->newName.equals(methodRule->newName)) return name.to_std_string();
    if (fieldRule) return fieldRule->newName.to_std_string();
    if (methodRule) return methodRule->newName.to_std_string();
    return name.to_std_string();
}

bool remapRecordComponent(RecordComponentNode& component, ca::str::Utf8StringArena& arena,
                          const MemberRemapPlan& plan, ca::str::Utf8StringRef owner,
                          const RemapPolicy& policy)
{
    bool changed = false;
    auto oldName = component.name;
    auto oldDesc = component.descriptor;
    if (policy.bytecode) {
        changed |= assignIfChanged(
            component.name, arena, ca::str::Utf8StringRef::from_cstr(
                recordComponentMappedName(plan, owner, oldName, oldDesc).c_str()));
        changed |= assignIfChanged(component.descriptor, arena, plan.mapFieldDescriptor(oldDesc));
    }
    if (policy.signatures) {
        changed |= assignSignatureIfChanged(component.signature, arena, plan);
    }
    if (policy.annotations) {
        changed |= remapAnnotations(component.visibleAnnotations, arena, plan);
        changed |= remapAnnotations(component.invisibleAnnotations, arena, plan);
    }
    return changed;
}

bool remapClassStructuralMetadata(ClassNode& classNode, ca::str::Utf8StringArena& arena,
                                  const MemberRemapPlan& plan)
{
    bool changed = false;
    if (!classNode.outerClass.is_empty()) {
        auto oldOuterClass = classNode.outerClass;
        auto oldOuterName  = classNode.outerMethod;
        auto oldOuterDesc  = classNode.outerMethodDesc;
        const auto* rule   = oldOuterName.is_empty()
                                 ? nullptr
                                 : plan.findMethod(oldOuterClass, oldOuterName, oldOuterDesc);

        changed |= assignIfChanged(classNode.outerClass, arena, plan.mapInternalName(oldOuterClass));
        if (rule) {
            changed |= assignIfChanged(classNode.outerMethod, arena, rule->newName);
            changed |= assignIfChanged(
                classNode.outerMethodDesc,
                arena,
                rule->newDesc.is_empty() ? plan.mapMethodDescriptor(oldOuterDesc) : rule->newDesc);
        }
        else if (!oldOuterDesc.is_empty()) {
            changed |= assignIfChanged(classNode.outerMethodDesc, arena,
                                       plan.mapMethodDescriptor(oldOuterDesc));
        }
    }

    if (!classNode.nestHost.is_empty()) {
        changed |= assignIfChanged(classNode.nestHost, arena,
                                   plan.mapInternalName(classNode.nestHost));
    }
    for (auto& member : classNode.nestMembers) {
        changed |= assignIfChanged(member, arena, plan.mapInternalName(member));
    }
    for (auto& subclass : classNode.permittedSubclasses) {
        changed |= assignIfChanged(subclass, arena, plan.mapInternalName(subclass));
    }
    for (auto* innerClass : classNode.innerClasses) {
        if (innerClass) changed |= remapInnerClassMetadata(*innerClass, arena, plan);
    }
    return changed;
}

bool remapFieldInsn(FieldInsnNode& fieldInsn, ca::str::Utf8StringArena& arena,
                    const MemberRemapPlan& plan)
{
    auto oldOwner = fieldInsn.owner;
    auto oldName  = fieldInsn.name;
    auto oldDesc  = fieldInsn.desc;

    bool changed = false;
    if (auto* rule = plan.findField(oldOwner, oldName, oldDesc)) {
        changed |= assignIfChanged(fieldInsn.owner, arena, rule->newOwner);
        changed |= assignIfChanged(fieldInsn.name, arena, rule->newName);
        auto newDesc = rule->newDesc.is_empty() ? plan.mapFieldDescriptor(oldDesc) : rule->newDesc;
        changed |= assignIfChanged(fieldInsn.desc, arena, newDesc);
        return changed;
    }

    changed |= assignIfChanged(fieldInsn.owner, arena, mapOwnerRef(plan, oldOwner));
    changed |= assignIfChanged(fieldInsn.desc, arena, plan.mapFieldDescriptor(oldDesc));
    return changed;
}

bool remapMethodInsn(MethodInsnNode& methodInsn, ca::str::Utf8StringArena& arena,
                     const MemberRemapPlan& plan)
{
    auto oldOwner = methodInsn.owner;
    auto oldName  = methodInsn.name;
    auto oldDesc  = methodInsn.desc;

    bool changed = false;
    if (auto* rule = plan.findMethod(oldOwner, oldName, oldDesc)) {
        changed |= assignIfChanged(methodInsn.owner, arena, rule->newOwner);
        changed |= assignIfChanged(methodInsn.name, arena, rule->newName);
        changed |= assignIfChanged(methodInsn.desc, arena, rule->newDesc);
        return changed;
    }

    changed |= assignIfChanged(methodInsn.owner, arena, mapOwnerRef(plan, oldOwner));
    changed |= assignIfChanged(methodInsn.desc, arena, plan.mapMethodDescriptor(oldDesc));
    return changed;
}

bool remapTypeConstant(Type& type, ca::str::Utf8StringArena& arena,
                       const MemberRemapPlan& plan)
{
    ca::str::Utf8StringRef mapped;
    switch (type.getSort()) {
    case Type::INTERNAL:
        //staticArena 退役：OBJECT/INTERNAL 排序 getInternalName 返回 value_ 不
        //intern，落点参数按必选口径走作业级 arena（下方重映射结果也归同一池）。
        mapped = plan.mapInternalName(type.getInternalName(arena));
        if (mapped.equals(type.getInternalName(arena))) return false;
        type = Type::getObjectType(arena.intern(mapped));
        return true;
    case Type::OBJECT:
    case Type::ARRAY:
        mapped = plan.mapFieldDescriptor(type.getDescriptor(arena));
        if (mapped.equals(type.getDescriptor(arena))) return false;
        type = Type::fromArena(arena, mapped);
        return true;
    case Type::METHOD:
        mapped = plan.mapMethodDescriptor(type.getDescriptor(arena));
        if (mapped.equals(type.getDescriptor(arena))) return false;
        type = Type::fromArena(arena, mapped);
        return true;
    default: return false;
    }
}

bool remapHandle(Handle& handle, ca::str::Utf8StringArena& arena,
                 const MemberRemapPlan& plan)
{
    const auto oldOwner = handle.getOwner();
    const auto oldName  = handle.getName();
    const auto oldDesc  = handle.getDesc();
    //数组型 owner（数组 clone 等方法句柄可指向 `[LX;`）与直接类名同口径映射。
    auto       newOwner = mapOwnerRef(plan, oldOwner);
    auto       newName  = oldName;
    auto       newDesc  = oldDesc;

    switch (handle.getTag()) {
    case Opcodes::H_GETFIELD:
    case Opcodes::H_GETSTATIC:
    case Opcodes::H_PUTFIELD:
    case Opcodes::H_PUTSTATIC:
        if (const auto* rule = plan.findField(oldOwner, oldName, oldDesc)) {
            newOwner = rule->newOwner;
            newName  = rule->newName;
            newDesc  = rule->newDesc.is_empty() ? plan.mapFieldDescriptor(oldDesc) : rule->newDesc;
        }
        else {
            newDesc = plan.mapFieldDescriptor(oldDesc);
        }
        break;
    default:
        if (const auto* rule = plan.findMethod(oldOwner, oldName, oldDesc)) {
            newOwner = rule->newOwner;
            newName  = rule->newName;
            newDesc  = rule->newDesc;
        }
        else {
            newDesc = plan.mapMethodDescriptor(oldDesc);
        }
        break;
    }

    const Handle mapped(handle.getTag(),
                        arena.intern(newOwner),
                        arena.intern(newName),
                        arena.intern(newDesc),
                        handle.isInterface());
    if (mapped == handle) return false;
    handle = mapped;
    return true;
}

bool remapConstantValue(ca::core::Any& value, ca::str::Utf8StringArena& arena,
                        const MemberRemapPlan& plan)
{
    if (auto* type = value.as<Type>()) {
        Type mapped = *type;
        if (!remapTypeConstant(mapped, arena, plan)) return false;
        value = ca::core::Any(mapped);
        return true;
    }
    if (auto* handle = value.as<Handle>()) {
        Handle mapped = *handle;
        if (!remapHandle(mapped, arena, plan)) return false;
        value = ca::core::Any(mapped);
        return true;
    }
    if (auto* dynamic = value.as<ConstantDynamic>()) {
        auto descriptor = plan.mapFieldDescriptor(dynamic->getDescriptor());
        auto bootstrap  = dynamic->getBootstrapMethod();
        bool changed    = !descriptor.equals(dynamic->getDescriptor());
        changed |= remapHandle(bootstrap, arena, plan);

        auto arguments = dynamic->getBootstrapMethodArguments();
        for (auto& argument : arguments) {
            changed |= remapConstantValue(argument, arena, plan);
        }
        if (!changed) return false;

        value = ca::core::Any(ConstantDynamic(arena.intern(dynamic->getName()),
                                              arena.intern(descriptor),
                                              std::move(bootstrap),
                                              std::move(arguments)));
        return true;
    }
    return false;
}

bool remapFrameValue(ca::core::Any& value, ca::str::Utf8StringArena& arena,
                     const MemberRemapPlan& plan)
{
    const auto mapName = [&](ca::str::Utf8StringRef oldName) {
        return !oldName.is_empty() && oldName.byte_at(0) == '['
                   ? plan.mapFieldDescriptor(oldName)
                   : plan.mapInternalName(oldName);
    };
    if (auto* name = value.as<ca::str::Utf8StringRef>()) {
        const auto mapped = mapName(*name);
        if (mapped.equals(*name)) return false;
        value = ca::core::Any(arena.intern(mapped));
        return true;
    }
    if (auto* name = value.as<std::string>()) {
        const auto oldRef = ca::str::Utf8StringRef::from_cstr(name->c_str());
        const auto mapped = mapName(oldRef);
        if (mapped.equals(oldRef)) return false;
        ///与 Utf8StringRef 分支对齐：映射结果 intern 进作业级 arena，而非回落 std::string。
        value = ca::core::Any(arena.intern(mapped));
        return true;
    }
    return false;
}

bool remapInvokeDynamic(InvokeDynamicInsnNode& invokeDynamic,
                        ca::str::Utf8StringArena& arena,
                        const MemberRemapPlan& plan)
{
    const auto oldDesc = invokeDynamic.desc;
    const bool isLambdaMetafactory =
        invokeDynamic.bsm.getOwner().equals("java/lang/invoke/LambdaMetafactory") &&
        (invokeDynamic.bsm.getName().equals("metafactory") ||
         invokeDynamic.bsm.getName().equals("altMetafactory"));
    bool changed = assignIfChanged(invokeDynamic.desc, arena, plan.mapMethodDescriptor(oldDesc));
    changed |= remapHandle(invokeDynamic.bsm, arena, plan);

    if (isLambdaMetafactory && !invokeDynamic.bsmArgs.empty()) {
        const auto* samType = invokeDynamic.bsmArgs.front().as<Type>();
        if (samType != nullptr && samType->getSort() == Type::METHOD) {
            const Type returnType = Type::getReturnType(oldDesc);
            if (returnType.getSort() == Type::INTERNAL || returnType.getSort() == Type::OBJECT) {
                //staticArena 退役：owner/desc 仅瞬时查表比较，落点=本函数作业
                //级 arena 参数（INTERNAL/OBJECT 排序不 intern，口径统一）。
                const auto owner = returnType.getInternalName(arena);
                if (const auto* rule = plan.findMethod(
                        owner, invokeDynamic.name, samType->getDescriptor(arena))) {
                    changed |= assignIfChanged(invokeDynamic.name, arena, rule->newName);
                }
            }
        }
    }
    for (auto& argument : invokeDynamic.bsmArgs) {
        changed |= remapConstantValue(argument, arena, plan);
    }
    return changed;
}

bool remapInstruction(AbstractInsnNode& insn, ca::str::Utf8StringArena& arena,
                      const MemberRemapPlan& plan, const RemapPolicy& policy)
{
    if (!policy.bytecode) {
        return false;
    }

    if (insn.getType() == AbstractInsnNode::FIELD_INSN) {
        return remapFieldInsn(*ca::core::tag_cast::cast<FieldInsnNode*>(&insn), arena, plan);
    }
    if (insn.getType() == AbstractInsnNode::METHOD_INSN) {
        return remapMethodInsn(*ca::core::tag_cast::cast<MethodInsnNode*>(&insn), arena, plan);
    }
    if (insn.getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN) {
        return remapInvokeDynamic(*ca::core::tag_cast::cast<InvokeDynamicInsnNode*>(&insn), arena, plan);
    }
    if (insn.getType() == AbstractInsnNode::TYPE_INSN) {
        auto* typeInsn = ca::core::tag_cast::cast<TypeInsnNode*>(&insn);
        const auto mapped = !typeInsn->desc.is_empty() && typeInsn->desc.byte_at(0) == '['
                                ? plan.mapFieldDescriptor(typeInsn->desc)
                                : plan.mapInternalName(typeInsn->desc);
        return assignIfChanged(typeInsn->desc, arena, mapped);
    }
    if (insn.getType() == AbstractInsnNode::MULTIANEWARRAY_INSN) {
        auto* multi = ca::core::tag_cast::cast<MultiANewArrayInsnNode*>(&insn);
        return assignIfChanged(multi->desc, arena,
                               plan.mapFieldDescriptor(multi->desc));
    }
    if (insn.getType() == AbstractInsnNode::FRAME) {
        auto* frame = ca::core::tag_cast::cast<FrameNode*>(&insn);
        bool changed = false;
        for (auto& value : frame->local) changed |= remapFrameValue(value, arena, plan);
        for (auto& value : frame->stack) changed |= remapFrameValue(value, arena, plan);
        return changed;
    }
    if (insn.getType() == AbstractInsnNode::LDC_INSN) {
        auto* ldc = ca::core::tag_cast::cast<LdcInsnNode*>(&insn);
        return remapConstantValue(ldc->cst, arena, plan);
    }
    return false;
}

}   //namespace

RemapPolicy RemapPolicy::bytecodeOnly()
{
    return RemapPolicy{};
}

RemapPolicy RemapPolicy::bytecodeAndReflectionStrings()
{
    RemapPolicy policy;
    policy.reflectionStrings = true;
    return policy;
}

RemapPolicy RemapPolicy::bytecodeAndMetadata()
{
    RemapPolicy policy;
    policy.annotations = true;
    policy.signatures  = true;
    return policy;
}

RemapPolicy RemapPolicy::all()
{
    RemapPolicy policy;
    policy.reflectionStrings = true;
    policy.annotations       = true;
    policy.signatures        = true;
    policy.resources         = true;
    return policy;
}

bool applyMemberRemapPlan(ClassNode& classNode, ca::str::Utf8StringArena& arena,
                          const MemberRemapPlan& plan)
{
    return applyMemberRemapPlan(classNode, arena, plan, RemapPolicy::bytecodeOnly());
}

bool applyMemberRemapPlan(ClassNode& classNode, ca::str::Utf8StringArena& arena,
                          const MemberRemapPlan& plan, const RemapPolicy& policy)
{
    return applyMemberRemapPlan(classNode, arena, plan, policy, nullptr);
}

bool applyMemberRemapPlan(ClassNode& classNode, ca::str::Utf8StringArena& arena,
                          const MemberRemapPlan& plan, const RemapPolicy& policy,
                          const AliasAnalysis* aliases)
{
    if (plan.empty()) return false;

    bool changed  = false;
    auto oldOwner = classNode.name;
    if (policy.bytecode) {
        changed |= assignIfChanged(classNode.name, arena, plan.mapInternalName(oldOwner));
        changed |= assignIfChanged(classNode.superName, arena,
                                   plan.mapInternalName(classNode.superName));

        for (auto& iface : classNode.interfaces) {
            changed |= assignIfChanged(iface, arena, plan.mapInternalName(iface));
        }
        changed |= remapClassStructuralMetadata(classNode, arena, plan);
    }
    if (policy.signatures) {
        changed |= assignSignatureIfChanged(classNode.signature, arena, plan);
    }
    if (policy.annotations) {
        changed |= remapAnnotations(classNode.visibleAnnotations, arena, plan);
        changed |= remapAnnotations(classNode.invisibleAnnotations, arena, plan);
        changed |= remapAnnotations(classNode.visibleTypeAnnotations, arena, plan);
        changed |= remapAnnotations(classNode.invisibleTypeAnnotations, arena, plan);
    }
    for (auto* component : classNode.recordComponents) {
        if (component) changed |= remapRecordComponent(*component, arena, plan, oldOwner, policy);
    }

    for (auto* field : classNode.fields) {
        if (!field) continue;
        auto oldName = field->name;
        auto oldDesc = field->desc;
        if (policy.bytecode) {
            if (auto* rule = plan.findField(oldOwner, oldName, oldDesc)) {
                changed |= assignIfChanged(field->name, arena, rule->newName);
                auto newDesc = rule->newDesc.is_empty() ? plan.mapFieldDescriptor(oldDesc)
                                                        : rule->newDesc;
                changed |= assignIfChanged(field->desc, arena, newDesc);
            }
            else {
                changed |= assignIfChanged(field->desc, arena, plan.mapFieldDescriptor(oldDesc));
            }
        }
        if (policy.signatures) {
            changed |= assignSignatureIfChanged(field->signature, arena, plan);
        }
        if (policy.annotations) {
            changed |= remapAnnotations(field->visibleAnnotations, arena, plan);
            changed |= remapAnnotations(field->invisibleAnnotations, arena, plan);
        }
    }

    for (auto* method : classNode.methods) {
        if (!method) continue;
        auto oldName = method->name;
        auto oldDesc = method->desc;
        if (auto* rule = plan.findMethod(oldOwner, oldName, oldDesc)) {
            if (policy.bytecode) {
                changed |= assignIfChanged(method->name, arena, rule->newName);
                changed |= assignIfChanged(method->desc, arena, rule->newDesc);
            }
        }
        else if (policy.bytecode) {
            changed |= assignIfChanged(method->desc, arena, plan.mapMethodDescriptor(oldDesc));
        }
        if (policy.bytecode) {
            for (auto& exception : method->exceptions) {
                changed |= assignIfChanged(exception, arena, plan.mapInternalName(exception));
            }
            for (auto* tryCatch : method->tryCatchBlocks) {
                if (tryCatch != nullptr && !tryCatch->type.is_empty()) {
                    changed |= assignIfChanged(
                        tryCatch->type, arena, plan.mapInternalName(tryCatch->type));
                }
            }
            for (auto* local : method->localVariables) {
                if (local == nullptr) continue;
                if (!local->descriptor.is_empty()) {
                    changed |= assignIfChanged(
                        local->descriptor, arena, plan.mapFieldDescriptor(local->descriptor));
                }
            }
        }
        if (policy.signatures) {
            changed |= assignSignatureIfChanged(method->signature, arena, plan);
            for (auto* local : method->localVariables) {
                if (local != nullptr && !local->signature.is_empty()) {
                    changed |= assignSignatureIfChanged(local->signature, arena, plan);
                }
            }
        }
        if (policy.annotations) {
            changed |= remapAnnotations(method->visibleAnnotations, arena, plan);
            changed |= remapAnnotations(method->invisibleAnnotations, arena, plan);
            changed |= remapAnnotations(method->visibleTypeAnnotations, arena, plan);
            changed |= remapAnnotations(method->invisibleTypeAnnotations, arena, plan);
            changed |= remapAnnotations(method->visibleParameterAnnotations, arena, plan);
            changed |= remapAnnotations(method->invisibleParameterAnnotations, arena, plan);
            for (auto* annotation : method->visibleLocalVariableAnnotations) {
                if (annotation) changed |= remapAnnotation(*annotation, arena, plan);
            }
            for (auto* annotation : method->invisibleLocalVariableAnnotations) {
                if (annotation) changed |= remapAnnotation(*annotation, arena, plan);
            }
            if (method->annotationDefault) {
                changed |= remapAnnotation(*method->annotationDefault, arena, plan);
            }
        }

        if (policy.reflectionStrings) {
            changed |= remapReflectionMemberNames(*method, arena, plan, oldOwner.to_std_string(), aliases);
        }

        for (auto* insn : method->instructions.toArray()) {
            if (!insn) continue;
            changed |= remapInstruction(*insn, arena, plan, policy);
        }
    }

    if (policy.bytecode && classNode.moduleNode != nullptr) {
        auto& module = *classNode.moduleNode;
        if (!module.mainClass.is_empty()) {
            changed |= assignIfChanged(
                module.mainClass, arena, plan.mapInternalName(module.mainClass));
        }
        for (auto& service : module.usesList) {
            changed |= assignIfChanged(service, arena, plan.mapInternalName(service));
        }
        for (auto& provision : module.providesList) {
            changed |= assignIfChanged(
                provision.service, arena, plan.mapInternalName(provision.service));
            for (auto& provider : provision.providers) {
                changed |= assignIfChanged(provider, arena, plan.mapInternalName(provider));
            }
        }
    }

    return changed;
}

}   //namespace mjt::base
