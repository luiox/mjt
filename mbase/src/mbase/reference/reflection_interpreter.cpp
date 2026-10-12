#include "mbase/reference/reflection_interpreter.hpp"

#include <unordered_set>
#include <utility>

#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/core/type.hpp"
#include "jbytecode/tree/nodes.hpp"
#include "mbase/analysis/alias_analysis.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

namespace {

using jbytecode::analysis::BasicValue;
using jbytecode::core::Opcodes;
using jbytecode::tree::AbstractInsnNode;
using jbytecode::tree::FieldInsnNode;
using jbytecode::tree::IntInsnNode;
using jbytecode::tree::JumpInsnNode;
using jbytecode::tree::LabelNode;
using jbytecode::tree::LdcInsnNode;
using jbytecode::tree::LookupSwitchInsnNode;
using jbytecode::tree::MethodInsnNode;
using jbytecode::tree::TableSwitchInsnNode;
using jbytecode::tree::TypeInsnNode;
using jbytecode::tree::VarInsnNode;

std::string str(ca::str::Utf8StringRef value)
{
    return value.to_std_string();
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

std::string classLiteralName(const jbytecode::core::Type& type)
{
    if (type.getSort() != jbytecode::core::Type::OBJECT) return {};
    //staticArena 退役：仅瞬时转 std::string，函数级局部 arena 零驻留。
    ca::str::Utf8StringArena scratch;
    return normalizeClassName(type.getInternalName(scratch).to_std_string());
}

std::string classLiteralDescriptor(const jbytecode::core::Type& type)
{
    if (type.getSort() == jbytecode::core::Type::OBJECT) {
        return "L" + classLiteralName(type) + ";";
    }
    if (type.getSort() == jbytecode::core::Type::ARRAY) {
        //staticArena 退役：ARRAY 排序 getDescriptor 返回 value_ 不 intern，
        //落点参数按必选口径走函数级局部 arena。
        ca::str::Utf8StringArena scratch;
        return type.getDescriptor(scratch).to_std_string();
    }
    return {};
}

AbstractInsnNode* previousReferenceInsn(AbstractInsnNode* insn)
{
    while (insn && (insn->getType() == AbstractInsnNode::LABEL ||
                    insn->getType() == AbstractInsnNode::LINE ||
                    insn->getType() == AbstractInsnNode::FRAME)) {
        insn = insn->getPrevious();
    }
    return insn;
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

bool readIntConstant(AbstractInsnNode* insn, ca::i32& value)
{
    if (!insn) return false;
    const int opcode = insn->getOpcode();
    if (opcode >= Opcodes::ICONST_0 && opcode <= Opcodes::ICONST_5) {
        value = opcode - Opcodes::ICONST_0;
        return true;
    }
    if (insn->getType() == AbstractInsnNode::INT_INSN &&
        (opcode == Opcodes::BIPUSH || opcode == Opcodes::SIPUSH)) {
        value = ca::core::tag_cast::cast<IntInsnNode>(insn)->operand;
        return value >= 0;
    }
    return false;
}

std::string primitiveDescriptorFromTypeField(const FieldInsnNode& field)
{
    if (field.getOpcode() != Opcodes::GETSTATIC ||
        !field.name.equals(ca::str::Utf8StringRef::from_cstr("TYPE")) ||
        !field.desc.equals(ca::str::Utf8StringRef::from_cstr("Ljava/lang/Class;"))) {
        return {};
    }

    const std::string owner = str(field.owner);
    if (owner == "java/lang/Boolean") return "Z";
    if (owner == "java/lang/Byte") return "B";
    if (owner == "java/lang/Character") return "C";
    if (owner == "java/lang/Short") return "S";
    if (owner == "java/lang/Integer") return "I";
    if (owner == "java/lang/Long") return "J";
    if (owner == "java/lang/Float") return "F";
    if (owner == "java/lang/Double") return "D";
    return {};
}

std::string readReflectionClassArgumentDescriptor(AbstractInsnNode* insn)
{
    if (insn && insn->getType() == AbstractInsnNode::LDC_INSN) {
        auto* type = ca::core::tag_cast::cast<LdcInsnNode>(insn)->cst.as<jbytecode::core::Type>();
        if (type) return classLiteralDescriptor(*type);
    }
    if (!insn || insn->getType() != AbstractInsnNode::FIELD_INSN) return {};
    return primitiveDescriptorFromTypeField(*ca::core::tag_cast::cast<FieldInsnNode>(insn));
}

std::string buildReflectionArgumentDescriptor(ca::i32 arraySize,
                                              std::vector<std::pair<ca::i32, std::string>> entries)
{
    if (arraySize < 0) return {};

    std::vector<std::string> args(static_cast<ca::usize>(arraySize));
    std::vector<bool>        seen(static_cast<ca::usize>(arraySize), false);
    for (auto& [index, desc] : entries) {
        if (index < 0 || index >= arraySize || seen[static_cast<ca::usize>(index)]) return {};
        args[static_cast<ca::usize>(index)] = std::move(desc);
        seen[static_cast<ca::usize>(index)] = true;
    }
    for (bool itemSeen : seen) {
        if (!itemSeen) return {};
    }

    std::string descriptor = "(";
    for (const auto& arg : args) descriptor += arg;
    descriptor += ")";
    return descriptor;
}

bool hasControlFlowEntry(AbstractInsnNode* first, AbstractInsnNode* last)
{
    std::unordered_set<jbytecode::core::Label*> labels;
    for (auto* insn = first; insn; insn = insn->getNext()) {
        if (insn->getType() == AbstractInsnNode::LABEL) {
            labels.insert(ca::core::tag_cast::cast<LabelNode>(insn)->label);
        }
        if (insn == last) break;
    }
    if (labels.empty()) return false;

    auto* head = first;
    while (head->getPrevious()) head = head->getPrevious();
    for (auto* insn = head; insn; insn = insn->getNext()) {
        if (insn->getType() == AbstractInsnNode::JUMP_INSN) {
            if (labels.count(ca::core::tag_cast::cast<JumpInsnNode>(insn)->label) != 0) return true;
        }
        else if (insn->getType() == AbstractInsnNode::TABLESWITCH_INSN) {
            auto* switchInsn = ca::core::tag_cast::cast<TableSwitchInsnNode>(insn);
            if (labels.count(switchInsn->dflt) != 0) return true;
            for (auto* label : switchInsn->labels) {
                if (labels.count(label) != 0) return true;
            }
        }
        else if (insn->getType() == AbstractInsnNode::LOOKUPSWITCH_INSN) {
            auto* switchInsn = ca::core::tag_cast::cast<LookupSwitchInsnNode>(insn);
            if (labels.count(switchInsn->dflt) != 0) return true;
            for (auto* label : switchInsn->labels) {
                if (labels.count(label) != 0) return true;
            }
        }
    }
    return false;
}

std::string localReflectionArgumentDescriptor(AbstractInsnNode* arrayLoad)
{
    if (!arrayLoad || arrayLoad->getType() != AbstractInsnNode::VAR_INSN ||
        arrayLoad->getOpcode() != Opcodes::ALOAD) {
        return {};
    }
    const int arrayLocal = ca::core::tag_cast::cast<VarInsnNode>(arrayLoad)->var;

    AbstractInsnNode* arrayStore = nullptr;
    for (auto* cursor = previousReferenceInsn(arrayLoad->getPrevious()); cursor;
         cursor       = previousReferenceInsn(cursor->getPrevious())) {
        if (cursor->getType() != AbstractInsnNode::VAR_INSN) continue;
        auto* variable = ca::core::tag_cast::cast<VarInsnNode>(cursor);
        if (variable->var != arrayLocal) continue;
        if (cursor->getOpcode() == Opcodes::ASTORE) {
            arrayStore = cursor;
            break;
        }
        if (cursor->getOpcode() != Opcodes::ALOAD) return {};
    }
    if (!arrayStore) return {};

    auto* arrayInsn = previousReferenceInsn(arrayStore->getPrevious());
    if (!arrayInsn || arrayInsn->getType() != AbstractInsnNode::TYPE_INSN ||
        arrayInsn->getOpcode() != Opcodes::ANEWARRAY ||
        !ca::core::tag_cast::cast<TypeInsnNode>(arrayInsn)->desc.equals(
            ca::str::Utf8StringRef::from_cstr("java/lang/Class"))) {
        return {};
    }
    auto*   sizeInsn  = previousReferenceInsn(arrayInsn->getPrevious());
    ca::i32 arraySize = -1;
    if (!readIntConstant(sizeInsn, arraySize) || arraySize < 0 ||
        hasControlFlowEntry(sizeInsn, arrayLoad)) {
        return {};
    }

    std::vector<std::pair<ca::i32, std::string>> entries;
    auto*                                        cursor = nextReferenceInsn(arrayStore->getNext());
    while (cursor && cursor != arrayLoad) {
        if (cursor->getType() != AbstractInsnNode::VAR_INSN ||
            cursor->getOpcode() != Opcodes::ALOAD ||
            ca::core::tag_cast::cast<VarInsnNode>(cursor)->var != arrayLocal) {
            return {};
        }

        cursor        = nextReferenceInsn(cursor->getNext());
        ca::i32 index = -1;
        if (!readIntConstant(cursor, index)) return {};

        cursor              = nextReferenceInsn(cursor->getNext());
        std::string argDesc = readReflectionClassArgumentDescriptor(cursor);
        if (argDesc.empty()) return {};

        cursor = nextReferenceInsn(cursor->getNext());
        if (!cursor || cursor->getOpcode() != Opcodes::AASTORE) return {};
        entries.push_back({index, std::move(argDesc)});
        cursor = nextReferenceInsn(cursor->getNext());
    }
    if (cursor != arrayLoad) return {};
    return buildReflectionArgumentDescriptor(arraySize, std::move(entries));
}

bool isMethod(const MethodInsnNode& insn, const char* owner, const char* name, const char* desc)
{
    return insn.owner.equals(ca::str::Utf8StringRef::from_cstr(owner)) &&
           insn.name.equals(ca::str::Utf8StringRef::from_cstr(name)) &&
           insn.desc.equals(ca::str::Utf8StringRef::from_cstr(desc));
}

ReflectionValue unknown(BasicValue basic)
{
    ReflectionValue value;
    value.basic = basic;
    return value;
}

AliasValue reflectionAlias(const ReflectionValue& value)
{
    switch (value.kind) {
    case ReflectionValue::Kind::String: return AliasValue::primitive("string:" + value.name);
    case ReflectionValue::Kind::Class: return AliasValue::object("class:" + value.owner);
    case ReflectionValue::Kind::Method:
        return AliasValue::object("method:" + value.owner + "." + value.name + value.desc + ":" +
                                  value.reflectionArgDesc);
    case ReflectionValue::Kind::Constructor:
        return AliasValue::object("constructor:" + value.owner + value.desc + ":" +
                                  value.reflectionArgDesc);
    case ReflectionValue::Kind::Field:
        return AliasValue::object("field:" + value.owner + "." + value.name + ":" + value.desc);
    case ReflectionValue::Kind::Unknown: return AliasValue::unknown();
    }
    return AliasValue::unknown();
}

ReflectionValue reflectionValueFromAlias(const AliasValue& alias, const BasicValue& basic)
{
    ReflectionValue value;
    value.basic = basic;
    if (!alias.isKnown()) return value;
    if (alias.identity->rfind("string:", 0) == 0) {
        value.kind = ReflectionValue::Kind::String;
        value.name = alias.identity->substr(7);
    }
    else if (alias.identity->rfind("class:", 0) == 0) {
        value.kind  = ReflectionValue::Kind::Class;
        value.owner = normalizeClassName(alias.identity->substr(6));
    }
    return value;
}

const AliasValue* resolveFieldAlias(const AliasAnalysis& aliases, const AliasValue& value)
{
    constexpr const char* prefix = "field:";
    if (value.identity->rfind(prefix, 0) != 0) return nullptr;
    const auto body  = value.identity->substr(6);
    const auto split = body.find('.');
    const auto desc  = body.rfind(':');
    if (split == std::string::npos || desc == std::string::npos || desc <= split + 1)
        return nullptr;
    return aliases.fieldValueOf(
        body.substr(0, split), body.substr(split + 1, desc - split - 1), body.substr(desc + 1));
}

std::vector<BasicValue> basicValues(const std::vector<ReflectionValue>& values)
{
    std::vector<BasicValue> result;
    result.reserve(values.size());
    for (const auto& value : values) result.push_back(value.basic);
    return result;
}

bool isReflectionFieldExecution(const MethodInsnNode& method)
{
    const std::string name = str(method.name);
    const std::string desc = str(method.desc);
    if (name == "get") return desc == "(Ljava/lang/Object;)Ljava/lang/Object;";
    if (name == "set") return desc == "(Ljava/lang/Object;Ljava/lang/Object;)V";

    struct PrimitiveAccessor
    {
        const char* suffix;
        const char* desc;
    };
    static const PrimitiveAccessor kAccessors[] = {
        {"Boolean", "Z"},
        {"Byte", "B"},
        {"Char", "C"},
        {"Short", "S"},
        {"Int", "I"},
        {"Long", "J"},
        {"Float", "F"},
        {"Double", "D"},
    };
    for (const auto& accessor : kAccessors) {
        if (name == std::string("get") + accessor.suffix) {
            return desc == std::string("(Ljava/lang/Object;)") + accessor.desc;
        }
        if (name == std::string("set") + accessor.suffix) {
            return desc == std::string("(Ljava/lang/Object;") + accessor.desc + ")V";
        }
    }
    return false;
}

}   //namespace

std::string reflectionArgumentDescriptor(const MethodInsnNode& lookupInsn)
{
    AbstractInsnNode* cursor = previousReferenceInsn(lookupInsn.getPrevious());
    if (!cursor) return {};
    if (cursor->getType() == AbstractInsnNode::VAR_INSN && cursor->getOpcode() == Opcodes::ALOAD) {
        return localReflectionArgumentDescriptor(cursor);
    }

    std::vector<std::pair<ca::i32, std::string>> entries;
    while (cursor && cursor->getOpcode() == Opcodes::AASTORE) {
        cursor              = previousReferenceInsn(cursor->getPrevious());
        std::string argDesc = readReflectionClassArgumentDescriptor(cursor);
        if (argDesc.empty()) return {};

        cursor        = previousReferenceInsn(cursor->getPrevious());
        ca::i32 index = -1;
        if (!readIntConstant(cursor, index)) return {};

        cursor = previousReferenceInsn(cursor->getPrevious());
        if (!cursor || cursor->getOpcode() != Opcodes::DUP) return {};

        entries.push_back({index, std::move(argDesc)});
        cursor = previousReferenceInsn(cursor->getPrevious());
    }

    if (!cursor || cursor->getType() != AbstractInsnNode::TYPE_INSN ||
        cursor->getOpcode() != Opcodes::ANEWARRAY) {
        return {};
    }
    auto* arrayInsn = ca::core::tag_cast::cast<TypeInsnNode>(cursor);
    if (!arrayInsn->desc.equals(ca::str::Utf8StringRef::from_cstr("java/lang/Class"))) {
        return {};
    }

    cursor            = previousReferenceInsn(cursor->getPrevious());
    ca::i32 arraySize = -1;
    if (!readIntConstant(cursor, arraySize) || arraySize < 0) return {};

    return buildReflectionArgumentDescriptor(arraySize, std::move(entries));
}

bool resolveReflectionExecution(const MethodInsnNode& method, const ReflectionValue& receiver,
                                ReflectionExecutionTarget& target)
{
    const std::string owner = str(method.owner);
    if (isMethod(method,
                 "java/lang/reflect/Method",
                 "invoke",
                 "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;") &&
        receiver.kind == ReflectionValue::Kind::Method) {
        target.kind              = ReflectionExecutionTarget::Kind::Method;
        target.owner             = receiver.owner;
        target.name              = receiver.name;
        target.desc              = receiver.desc;
        target.reflectionArgDesc = receiver.reflectionArgDesc;
        return true;
    }
    if (isMethod(method, "java/lang/Class", "newInstance", "()Ljava/lang/Object;") &&
        receiver.kind == ReflectionValue::Kind::Class) {
        target.kind              = ReflectionExecutionTarget::Kind::Constructor;
        target.owner             = receiver.owner;
        target.name              = "<init>";
        target.desc              = "()V";
        target.reflectionArgDesc = "()";
        return true;
    }
    if (isMethod(method,
                 "java/lang/reflect/Constructor",
                 "newInstance",
                 "([Ljava/lang/Object;)Ljava/lang/Object;") &&
        receiver.kind == ReflectionValue::Kind::Constructor) {
        target.kind              = ReflectionExecutionTarget::Kind::Constructor;
        target.owner             = receiver.owner;
        target.name              = "<init>";
        target.desc              = receiver.desc;
        target.reflectionArgDesc = receiver.reflectionArgDesc;
        return true;
    }
    if (owner == "java/lang/reflect/Field" && isReflectionFieldExecution(method) &&
        receiver.kind == ReflectionValue::Kind::Field) {
        target.kind  = ReflectionExecutionTarget::Kind::Field;
        target.owner = receiver.owner;
        target.name  = receiver.name;
        target.desc  = receiver.desc;
        return true;
    }
    return false;
}

ReflectionValue ReflectionInterpreter::newValue(const jbytecode::core::Type* type)
{
    return unknown(basic_.newValue(type));
}

ReflectionValue ReflectionInterpreter::newOperation(const AbstractInsnNode* insn)
{
    ReflectionValue value = unknown(basic_.newOperation(insn));
    if (insn->getType() == AbstractInsnNode::FIELD_INSN &&
        insn->getOpcode() == Opcodes::GETSTATIC && aliases_) {
        auto* field = ca::core::tag_cast::cast<const FieldInsnNode>(insn);
        if (const auto* alias =
                aliases_->fieldValueOf(str(field->owner), str(field->name), str(field->desc))) {
            return reflectionValueFromAlias(*alias, value.basic);
        }
    }
    if (insn->getType() != AbstractInsnNode::LDC_INSN) return value;

    auto* ldc = ca::core::tag_cast::cast<const LdcInsnNode>(insn);
    if (auto* text = ldc->cst.as<std::string>()) {
        value.kind = ReflectionValue::Kind::String;
        value.name = *text;
    }
    else if (auto* text = ldc->cst.as<ca::str::Utf8StringRef>()) {
        value.kind = ReflectionValue::Kind::String;
        value.name = text->to_std_string();
    }
    else if (auto* type = ldc->cst.as<jbytecode::core::Type>()) {
        std::string owner = classLiteralName(*type);
        if (!owner.empty()) {
            value.kind  = ReflectionValue::Kind::Class;
            value.owner = std::move(owner);
        }
    }
    return value;
}

ReflectionValue ReflectionInterpreter::copyOperation(const AbstractInsnNode* insn,
                                                     const ReflectionValue&  value)
{
    (void)insn;
    return value;
}

ReflectionValue ReflectionInterpreter::unaryOperation(const AbstractInsnNode* insn,
                                                      const ReflectionValue&  value)
{
    return unknown(basic_.unaryOperation(insn, value.basic));
}

ReflectionValue ReflectionInterpreter::binaryOperation(const AbstractInsnNode* insn,
                                                       const ReflectionValue&  value1,
                                                       const ReflectionValue&  value2)
{
    return unknown(basic_.binaryOperation(insn, value1.basic, value2.basic));
}

ReflectionValue ReflectionInterpreter::ternaryOperation(const AbstractInsnNode* insn,
                                                        const ReflectionValue&  value1,
                                                        const ReflectionValue&  value2,
                                                        const ReflectionValue&  value3)
{
    return unknown(basic_.ternaryOperation(insn, value1.basic, value2.basic, value3.basic));
}

ReflectionValue ReflectionInterpreter::naryOperation(const AbstractInsnNode*             insn,
                                                     const std::vector<ReflectionValue>& values)
{
    ReflectionValue result = unknown(basic_.naryOperation(insn, basicValues(values)));
    if (insn->getType() != AbstractInsnNode::METHOD_INSN) return result;

    auto* method = ca::core::tag_cast::cast<const MethodInsnNode>(insn);
    if (aliases_) {
        if (const auto* summary =
                aliases_->summaryOf(str(method->owner), str(method->name), str(method->desc))) {
            const std::string& identity = *summary->returnValue.identity;
            if (summary->returnValue.isKnown() && identity.rfind("param:", 0) == 0) {
                try {
                    const auto parameter = static_cast<ca::usize>(std::stoul(identity.substr(6)));
                    const bool isStatic  = method->getOpcode() == Opcodes::INVOKESTATIC;
                    const auto index     = parameter + (isStatic ? 0U : 1U);
                    if (index < values.size() &&
                        values[index].kind != ReflectionValue::Kind::Unknown) {
                        return values[index];
                    }
                }
                catch (...) {
                }
            }
            if (summary->returnValue.isKnown() && identity.rfind("this:", 0) == 0 &&
                !values.empty() && values[0].kind != ReflectionValue::Kind::Unknown) {
                return values[0];
            }
            auto alias = summary->returnValue;
            if (const auto* field = resolveFieldAlias(*aliases_, alias)) alias = *field;
            auto reflected = reflectionValueFromAlias(alias, result.basic);
            if (reflected.kind != ReflectionValue::Kind::Unknown) return reflected;
        }
    }
    if (isMethod(*method, "java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;") &&
        !values.empty() && values[0].kind == ReflectionValue::Kind::String) {
        result.kind  = ReflectionValue::Kind::Class;
        result.owner = normalizeClassName(values[0].name);
        return result;
    }

    if (values.empty()) return result;
    const ReflectionValue& receiver = values[0];
    const bool             methodLookup =
        isMethod(*method,
                 "java/lang/Class",
                 "getMethod",
                 "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;") ||
        isMethod(*method,
                 "java/lang/Class",
                 "getDeclaredMethod",
                 "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
    if (receiver.kind == ReflectionValue::Kind::Class && values.size() >= 2 &&
        values[1].kind == ReflectionValue::Kind::String && methodLookup) {
        result.kind              = ReflectionValue::Kind::Method;
        result.owner             = receiver.owner;
        result.name              = values[1].name;
        result.reflectionArgDesc = reflectionArgumentDescriptor(*method);
        return result;
    }
    const bool constructorLookup =
        isMethod(*method,
                 "java/lang/Class",
                 "getConstructor",
                 "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;") ||
        isMethod(*method,
                 "java/lang/Class",
                 "getDeclaredConstructor",
                 "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;");
    if (receiver.kind == ReflectionValue::Kind::Class && constructorLookup) {
        result.kind              = ReflectionValue::Kind::Constructor;
        result.owner             = receiver.owner;
        result.name              = "<init>";
        result.reflectionArgDesc = reflectionArgumentDescriptor(*method);
        return result;
    }
    const bool fieldLookup = isMethod(*method,
                                      "java/lang/Class",
                                      "getField",
                                      "(Ljava/lang/String;)Ljava/lang/reflect/Field;") ||
                             isMethod(*method,
                                      "java/lang/Class",
                                      "getDeclaredField",
                                      "(Ljava/lang/String;)Ljava/lang/reflect/Field;");
    if (receiver.kind == ReflectionValue::Kind::Class && values.size() >= 2 &&
        values[1].kind == ReflectionValue::Kind::String && fieldLookup) {
        result.kind  = ReflectionValue::Kind::Field;
        result.owner = receiver.owner;
        result.name  = values[1].name;
        return result;
    }

    return result;
}

void ReflectionInterpreter::returnOperation(const AbstractInsnNode* insn,
                                            const ReflectionValue&  value,
                                            const ReflectionValue&  expected)
{
    basic_.returnOperation(insn, value.basic, expected.basic);
}

ReflectionValue ReflectionInterpreter::merge(const ReflectionValue& value1,
                                             const ReflectionValue& value2)
{
    BasicValue       merged = basic_.merge(value1.basic, value2.basic);
    const AliasValue alias  = mergeAliasValues(reflectionAlias(value1), reflectionAlias(value2));
    if (alias.isKnown()) {
        ReflectionValue result = value1;
        result.basic           = merged;
        return result;
    }
    return unknown(merged);
}

}   //namespace mjt::base
