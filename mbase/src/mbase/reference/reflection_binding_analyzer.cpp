#include "mbase/reference/reflection_binding_analyzer.hpp"

#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <utility>

#include "jbytecode/analysis/analyzer.hpp"
#include "jbytecode/analysis/basic_interpreter.hpp"
#include "jbytecode/analysis/exceptions.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/core/type.hpp"
#include "jbytecode/jbytecode.hpp"
#include "jbytecode/tree/insn_list.hpp"
#include "jbytecode/tree/nodes.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

namespace {

using jbytecode::analysis::Analyzer;
using jbytecode::analysis::AnalyzerException;
using jbytecode::analysis::BasicInterpreter;
using jbytecode::analysis::BasicValue;
using jbytecode::core::Opcodes;
using jbytecode::tree::AbstractInsnNode;
using jbytecode::tree::FieldInsnNode;
using jbytecode::tree::IntInsnNode;
using jbytecode::tree::InvokeDynamicInsnNode;
using jbytecode::tree::LdcInsnNode;
using jbytecode::tree::MethodInsnNode;
using jbytecode::tree::TypeInsnNode;

std::string str(ca::str::Utf8StringRef value)
{
    return value.to_std_string();
}

///@brief 类名归一化：`(Lfoo/Bar;)V` 描述符形态与点分名统一为 internal name。
///
///口径（issue ）：接受两种输入形态——先剥 L...; 外层，再把 '.' 换成
///'/'。数组/原始类型形态不是合法的 Class.forName 实参，原样保留交由存在性
///验证失败。
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

///@brief 类常量（LDC Type）转追踪名：OBJECT 取 internal name；数组取描述符
///（`[I`/`[Ljava/lang/String;`）；原始类型取单字符描述符（I/Z/...）。
std::string classLiteralName(const jbytecode::core::Type& type)
{
    //staticArena 退役：仅瞬时转 std::string，函数级局部 arena 零驻留。
    ca::str::Utf8StringArena scratch;
    if (type.getSort() == jbytecode::core::Type::OBJECT) {
        return type.getInternalName(scratch).to_std_string();
    }
    return type.getDescriptor(scratch).to_std_string();
}

///@brief 追踪名（internal name / 数组描述符 / 单字符原始类型）转参数描述符形态。
std::string toParameterDescriptor(const std::string& name)
{
    if (name.size() == 1) return name;
    if (!name.empty() && name.front() == '[') return name;
    return "L" + name + ";";
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

///@brief 读取 int 常量（iconst_m1..5 / bipush / sipush）。
bool readIntConstant(const AbstractInsnNode* insn, ca::i32& value)
{
    if (!insn) return false;
    const int opcode = insn->getOpcode();
    if (opcode >= Opcodes::ICONST_0 && opcode <= Opcodes::ICONST_5) {
        value = opcode - Opcodes::ICONST_0;
        return true;
    }
    if (opcode == Opcodes::ICONST_M1) {
        value = -1;
        return true;
    }
    if (insn->getType() == AbstractInsnNode::INT_INSN &&
        (opcode == Opcodes::BIPUSH || opcode == Opcodes::SIPUSH)) {
        value = ca::core::tag_cast::cast<const IntInsnNode>(insn)->operand;
        return true;
    }
    return false;
}

///@brief 从字符串 Any（Utf8StringRef / std::string 两态，与 mbase evaluator 的
///recipeToUnits 同口径）读取 UTF-8 字节串。
std::optional<std::string> stringFromAny(const ca::core::Any& value)
{
    if (auto* text = value.as<ca::str::Utf8StringRef>()) {
        return text->to_std_string();
    }
    if (auto* text = value.as<std::string>()) {
        return *text;
    }
    return std::nullopt;
}

//── 抽象值域 ─────────────────────────────────────────────────
//
//覆盖反射 sink 实参追踪所需的最小闭包：字符串内容、类名、SB 链、Class[]/
//String[] 名表、成员对象、int（下标/拼接）。其余全部 Unknown（fail-closed）。
//可变载荷一律写时复制：数据流分支会克隆 Frame，就地修改会污染兄弟路径。

struct StringBuilderState
{
    std::string text;   ///< 已知内容（UTF-8 字节，classfile 解码后口径）
    bool known     = true;
    ///@brief 内容含歧义段（AmbiguousString 实参）：文本仍是已知前缀，但已不完整。
    bool ambiguous = false;
};

struct ArrayState
{
    bool isClassArray = false;
    ///@brief 元素个数未知、下标非常量或合并冲突时置 true：aaload 结果降级。
    bool    ambiguous = false;
    bool    sizeKnown = false;
    ca::i32 size      = 0;
    ///@brief 元素值：ClassArray 存类名（追踪名形态），NameTable 存字符串。
    std::vector<std::optional<std::string>> elements;

    bool operator==(const ArrayState& other) const
    {
        return isClassArray == other.isClassArray && ambiguous == other.ambiguous &&
               sizeKnown == other.sizeKnown && size == other.size && elements == other.elements;
    }
};

struct MemberState
{
    bool        isField = false;
    std::string owner;   ///< 目标类 internal name
    std::string name;    ///< 成员名（构造器为 "<init>"；未知为空）
    std::string desc;    ///< 参数类型全量可判定时的方法描述符；否则为空（fail-closed）

    bool operator==(const MemberState& other) const
    {
        return isField == other.isField && owner == other.owner && name == other.name &&
               desc == other.desc;
    }
};

struct BindingValue
{
    enum class Kind
    {
        Unknown,
        String,
        ///@brief 已知是字符串但内容受非常量下标影响（名表 aaload 非常量下标产物）。
        AmbiguousString,
        StringBuilder,
        ClassObject,
        ClassArray,
        NameTable,
        MemberObject,
        Int,
    };

    Kind                                kind     = Kind::Unknown;
    std::string                         text;    ///< String/ClassObject 载荷
    int                                 intValue = 0;
    std::shared_ptr<StringBuilderState> builder;
    std::shared_ptr<ArrayState>         array;
    std::shared_ptr<MemberState>        member;

    BasicValue basic = BasicValue::REFERENCE_VALUE;

    int getSize() const { return basic.getSize(); }

    static BindingValue unknown(BasicValue basicValue)
    {
        BindingValue value;
        value.kind  = Kind::Unknown;
        value.basic = basicValue;
        return value;
    }

    bool operator==(const BindingValue& other) const
    {
        if (kind != other.kind) return false;
        switch (kind) {
        case Kind::Unknown: return true;
        case Kind::String:
        case Kind::AmbiguousString:
        case Kind::ClassObject: return text == other.text;
        case Kind::Int: return intValue == other.intValue;
        case Kind::StringBuilder:
            if (!builder || !other.builder) return builder == other.builder;
            return builder->known == other.builder->known &&
                   builder->ambiguous == other.builder->ambiguous &&
                   (!builder->known || builder->text == other.builder->text);
        case Kind::ClassArray:
        case Kind::NameTable:
            if (!array || !other.array) return array == other.array;
            return *array == *other.array;
        case Kind::MemberObject:
            if (!member || !other.member) return member == other.member;
            return *member == *other.member;
        }
        return true;
    }
    bool operator!=(const BindingValue& other) const { return !(*this == other); }
};

//── sink 目录（数据驱动）──────────────────────────────────────

std::vector<ReflectionSinkEntry> buildSinkCatalog()
{
    std::vector<ReflectionSinkEntry> catalog;
    auto add = [&catalog](const char* owner, const char* name, const char* desc,
                          ReflectionApiKind kind, int classNameArg, int memberNameArg,
                          int paramTypesArg, bool requiresClassReceiver, bool producesMemberObject,
                          bool catalogOnly, bool memberEnumeration) {
        catalog.push_back({owner,
                           name,
                           desc,
                           kind,
                           classNameArg,
                           memberNameArg,
                           paramTypesArg,
                           requiresClassReceiver,
                           producesMemberObject,
                           catalogOnly,
                           memberEnumeration});
    };

    //类加载（类名实参槽 0）
    add("java/lang/Class", "forName", "(Ljava/lang/String;)Ljava/lang/Class;",
        ReflectionApiKind::ClassForName, 0, -1, -1, false, false, false, false);
    add("java/lang/Class", "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;",
        ReflectionApiKind::ClassForName, 0, -1, -1, false, false, false, false);
    add("java/lang/ClassLoader", "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;",
        ReflectionApiKind::ClassLoaderLoadClass, 0, -1, -1, false, false, false, false);

    //成员查找（单数）：接收者必须是已知 Class 对象
    add("java/lang/Class", "getMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",
        ReflectionApiKind::MemberLookup, -1, 0, 1, true, true, false, false);
    add("java/lang/Class", "getDeclaredMethod", "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;",
        ReflectionApiKind::MemberLookup, -1, 0, 1, true, true, false, false);
    add("java/lang/Class", "getConstructor", "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;",
        ReflectionApiKind::MemberLookup, -1, -1, 0, true, true, false, false);
    add("java/lang/Class", "getDeclaredConstructor", "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;",
        ReflectionApiKind::MemberLookup, -1, -1, 0, true, true, false, false);
    add("java/lang/Class", "getField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;",
        ReflectionApiKind::MemberLookup, -1, 0, -1, true, true, false, false);
    add("java/lang/Class", "getDeclaredField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;",
        ReflectionApiKind::MemberLookup, -1, 0, -1, true, true, false, false);

    //成员查找（复数枚举）：无单一目标，恒 Pending(member_enumeration)
    add("java/lang/Class", "getMethods", "()[Ljava/lang/reflect/Method;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);
    add("java/lang/Class", "getDeclaredMethods", "()[Ljava/lang/reflect/Method;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);
    add("java/lang/Class", "getConstructors", "()[Ljava/lang/reflect/Constructor;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);
    add("java/lang/Class", "getDeclaredConstructors", "()[Ljava/lang/reflect/Constructor;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);
    add("java/lang/Class", "getFields", "()[Ljava/lang/reflect/Field;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);
    add("java/lang/Class", "getDeclaredFields", "()[Ljava/lang/reflect/Field;",
        ReflectionApiKind::MemberLookup, -1, -1, -1, true, false, false, true);

    //反射执行 / 字段访问
    add("java/lang/reflect/Method", "invoke",
        "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;",
        ReflectionApiKind::MethodInvoke, -1, -1, -1, false, false, false, false);
    add("java/lang/reflect/Constructor", "newInstance", "([Ljava/lang/Object;)Ljava/lang/Object;",
        ReflectionApiKind::ConstructorNewInstance, -1, -1, -1, false, false, false, false);
    add("java/lang/Class", "newInstance", "()Ljava/lang/Object;",
        ReflectionApiKind::ClassNewInstance, -1, -1, -1, false, false, false, false);
    add("java/lang/reflect/Field", "get", "(Ljava/lang/Object;)Ljava/lang/Object;",
        ReflectionApiKind::FieldAccess, -1, -1, -1, false, false, false, false);
    add("java/lang/reflect/Field", "set", "(Ljava/lang/Object;Ljava/lang/Object;)V",
        ReflectionApiKind::FieldAccess, -1, -1, -1, false, false, false, false);

    //MethodHandles.Lookup：catalog-only（首版不解析）
    add("java/lang/invoke/MethodHandles$Lookup", "findVirtual",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findStatic",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findConstructor",
        "(Ljava/lang/Class;Ljava/lang/invoke/MethodType;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findSpecial",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/invoke/MethodType;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findGetter",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findSetter",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)V",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findStaticGetter",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "findStaticSetter",
        "(Ljava/lang/Class;Ljava/lang/String;Ljava/lang/Class;)V",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflect",
        "(Ljava/lang/reflect/Method;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflectConstructor",
        "(Ljava/lang/reflect/Constructor;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflectGetter",
        "(Ljava/lang/reflect/Field;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflectSetter",
        "(Ljava/lang/reflect/Field;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflectSpecial",
        "(Ljava/lang/reflect/Method;Ljava/lang/Class;)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);
    add("java/lang/invoke/MethodHandles$Lookup", "unreflectField",
        "(Ljava/lang/reflect/Field;Z)Ljava/lang/invoke/MethodHandle;",
        ReflectionApiKind::LookupMethodHandle, -1, -1, -1, false, false, true, false);

    return catalog;
}

const ReflectionSinkEntry* matchSink(const MethodInsnNode& insn)
{
    static const std::vector<ReflectionSinkEntry> catalog = buildSinkCatalog();
    for (const auto& entry : catalog) {
        if (insn.owner.equals(ca::str::Utf8StringRef::from_cstr(entry.owner)) &&
            insn.name.equals(ca::str::Utf8StringRef::from_cstr(entry.name)) &&
            insn.desc.equals(ca::str::Utf8StringRef::from_cstr(entry.desc))) {
            return &entry;
        }
    }
    return nullptr;
}

bool isStringBuilderOwner(const MethodInsnNode& method)
{
    return method.owner.equals(ca::str::Utf8StringRef::from_cstr("java/lang/StringBuilder")) ||
           method.owner.equals(ca::str::Utf8StringRef::from_cstr("java/lang/StringBuffer"));
}

bool isStringConcatFactory(const InvokeDynamicInsnNode& indy)
{
    return indy.bsm.getOwner().equals(
        ca::str::Utf8StringRef::from_cstr("java/lang/invoke/StringConcatFactory"));
}

bool isConcatBootstrapName(const InvokeDynamicInsnNode& indy)
{
    return indy.bsm.getName().equals(ca::str::Utf8StringRef::from_cstr("makeConcatWithConstants")) ||
           indy.bsm.getName().equals(ca::str::Utf8StringRef::from_cstr("makeConcat"));
}

///@brief SB/concat 实参渲染结果：text 为可拼接文本段，ambiguous 标记该段来自
///歧义产物（AmbiguousString，内容受非常量下标影响、文本只是已知前缀）。
struct RenderedConcatPart
{
    std::string text;
    bool        ambiguous = false;
};

///@brief SB/concat 实参渲染：String 原样、Int 十进制、AmbiguousString 产出歧义
///哨兵段（区分"无法渲染"与"确认含歧义"，保 reason 归因）；其余 fail-closed。
std::optional<RenderedConcatPart> renderConcatArgument(const BindingValue& value)
{
    if (value.kind == BindingValue::Kind::String)
        return RenderedConcatPart{value.text, false};
    if (value.kind == BindingValue::Kind::Int)
        return RenderedConcatPart{std::to_string(value.intValue), false};
    if (value.kind == BindingValue::Kind::AmbiguousString)
        return RenderedConcatPart{std::string(), true};
    return std::nullopt;
}

///@brief 原始类型 TYPE 常量（Integer.TYPE 等）→ 单字符描述符；非 TYPE 返回空。
std::string primitiveTypeFieldName(const std::string& owner, const std::string& name)
{
    if (name != "TYPE") return {};
    static const std::pair<const char*, const char*> kBoxes[] = {
        {"java/lang/Boolean", "Z"}, {"java/lang/Byte", "B"},   {"java/lang/Character", "C"},
        {"java/lang/Short", "S"},   {"java/lang/Integer", "I"}, {"java/lang/Long", "J"},
        {"java/lang/Float", "F"},   {"java/lang/Double", "D"},  {"java/lang/Void", "V"},
    };
    for (const auto& [box, desc] : kBoxes) {
        if (owner == box) return desc;
    }
    return {};
}

}   //namespace

//── 抽象解释器 ───────────────────────────────────────────────

namespace {

///@brief 反射 sink 实参追踪的方法内抽象解释器。
///
///与 ReflectionInterpreter 同构（BasicValue 保底栈形态），但值域面向实参追踪：
///字符串/SB 链/indy 拼接/static final 常量/String[] 名表/Class[] 下标。
///合并 fail-closed：内容冲突即折损。
class BindingInterpreter final : public jbytecode::analysis::Interpreter<BindingValue>
{
public:
    explicit BindingInterpreter(const ReflectionBindingAnalyzer& analyzer)
        : analyzer_(analyzer)
    {}

    BindingValue newValue(const jbytecode::core::Type* type) override
    {
        return BindingValue::unknown(basic_.newValue(type));
    }

    BindingValue newOperation(const AbstractInsnNode* insn) override
    {
        //int 常量：名表下标与拼接需要
        ca::i32 constant = 0;
        if (readIntConstant(insn, constant)) {
            BindingValue value = BindingValue::unknown(basic_.newOperation(insn));
            value.kind         = BindingValue::Kind::Int;
            value.intValue     = constant;
            return value;
        }

        if (insn->getType() == AbstractInsnNode::LDC_INSN) {
            BindingValue value = BindingValue::unknown(basic_.newOperation(insn));
            auto*         ldc  = ca::core::tag_cast::cast<const LdcInsnNode>(insn);
            if (auto text = stringFromAny(ldc->cst)) {
                value.kind = BindingValue::Kind::String;
                value.text = std::move(*text);
                return value;
            }
            if (auto* type = ldc->cst.as<jbytecode::core::Type>()) {
                std::string name = classLiteralName(*type);
                if (!name.empty()) {
                    value.kind = BindingValue::Kind::ClassObject;
                    value.text = std::move(name);
                }
                return value;
            }
            if (auto* number = ldc->cst.as<int>()) {
                value.kind     = BindingValue::Kind::Int;
                value.intValue = *number;
            }
            return value;
        }

        if (insn->getType() == AbstractInsnNode::FIELD_INSN &&
            insn->getOpcode() == Opcodes::GETSTATIC) {
            BindingValue value = BindingValue::unknown(basic_.newOperation(insn));
            auto*        field = ca::core::tag_cast::cast<const FieldInsnNode>(insn);
            const std::string owner = str(field->owner);
            const std::string name  = str(field->name);
            const std::string desc  = str(field->desc);

            if (desc == "Ljava/lang/String;") {
                auto found = analyzer_.stringConstants().find(owner + "." + name);
                if (found != analyzer_.stringConstants().end()) {
                    value.kind = BindingValue::Kind::String;
                    value.text = found->second;
                }
                return value;
            }
            if (desc == "[Ljava/lang/String;") {
                auto found = analyzer_.nameTables().find(owner + "." + name);
                if (found != analyzer_.nameTables().end()) {
                    auto state          = std::make_shared<ArrayState>();
                    state->isClassArray = false;
                    state->sizeKnown    = true;
                    state->size         = static_cast<ca::i32>(found->second.size());
                    state->elements.reserve(found->second.size());
                    for (const auto& element : found->second) {
                        state->elements.emplace_back(element);
                    }
                    value.kind  = BindingValue::Kind::NameTable;
                    value.array = std::move(state);
                }
                return value;
            }
            if (desc == "Ljava/lang/Class;") {
                std::string primitive = primitiveTypeFieldName(owner, name);
                if (!primitive.empty()) {
                    value.kind = BindingValue::Kind::ClassObject;
                    value.text = std::move(primitive);
                }
            }
            return value;
        }

        if (insn->getType() == AbstractInsnNode::TYPE_INSN && insn->getOpcode() == Opcodes::NEW) {
            auto* typeInsn = ca::core::tag_cast::cast<const TypeInsnNode>(insn);
            if (typeInsn->desc.equals(ca::str::Utf8StringRef::from_cstr("java/lang/StringBuilder")) ||
                typeInsn->desc.equals(ca::str::Utf8StringRef::from_cstr("java/lang/StringBuffer"))) {
                BindingValue value = BindingValue::unknown(basic_.newOperation(insn));
                auto         state = std::make_shared<StringBuilderState>();
                state->known        = true;
                value.kind          = BindingValue::Kind::StringBuilder;
                value.builder       = std::move(state);
                return value;
            }
        }

        return BindingValue::unknown(basic_.newOperation(insn));
    }

    BindingValue copyOperation(const AbstractInsnNode* insn, const BindingValue& value) override
    {
        (void)insn;
        return value;
    }

    BindingValue unaryOperation(const AbstractInsnNode* insn, const BindingValue& value) override
    {
        const int opcode = insn->getOpcode();
        //checkcast 只做类型断言：forName 结果经 checkcast 存局部变量是常见形态。
        if (opcode == Opcodes::CHECKCAST) return value;
        if (opcode == Opcodes::ANEWARRAY) {
            auto* typeInsn = ca::core::tag_cast::cast<const TypeInsnNode>(insn);
            auto  state    = std::make_shared<ArrayState>();
            state->isClassArray =
                typeInsn->desc.equals(ca::str::Utf8StringRef::from_cstr("java/lang/Class"));
            if (value.kind == BindingValue::Kind::Int && value.intValue >= 0) {
                state->sizeKnown = true;
                state->size      = value.intValue;
                state->elements.assign(static_cast<std::size_t>(value.intValue), std::nullopt);
            }
            else {
                state->ambiguous = true;
            }
            BindingValue result = BindingValue::unknown(basic_.unaryOperation(insn, value.basic));
            result.kind         = BindingValue::Kind::ClassArray;
            result.array        = std::move(state);
            return result;
        }
        return BindingValue::unknown(basic_.unaryOperation(insn, value.basic));
    }

    BindingValue binaryOperation(const AbstractInsnNode* insn, const BindingValue& value1,
                                 const BindingValue& value2) override
    {
        const BasicValue fallback = basic_.binaryOperation(insn, value1.basic, value2.basic);
        const int        opcode   = insn->getOpcode();

        if (opcode == Opcodes::AALOAD) {
            const BindingValue& array = value1;
            const BindingValue& index = value2;
            if (array.kind == BindingValue::Kind::ClassArray) {
                //Class[] 元素：常量下标精确取类名；其余 fail-closed Unknown。
                if (array.array && !array.array->ambiguous && array.array->sizeKnown &&
                    index.kind == BindingValue::Kind::Int && index.intValue >= 0 &&
                    static_cast<std::size_t>(index.intValue) < array.array->elements.size()) {
                    const auto& element =
                        array.array->elements[static_cast<std::size_t>(index.intValue)];
                    if (element) {
                        BindingValue result = BindingValue::unknown(fallback);
                        result.kind         = BindingValue::Kind::ClassObject;
                        result.text         = *element;
                        return result;
                    }
                }
                return BindingValue::unknown(fallback);
            }
            if (array.kind == BindingValue::Kind::NameTable) {
                //名表：常量下标精确；非常量下标 → AmbiguousString（该 sink 整体
                //降 Pending(ambiguous_array)，口径）。
                BindingValue result = BindingValue::unknown(fallback);
                if (!array.array || array.array->ambiguous || index.kind != BindingValue::Kind::Int) {
                    result.kind = BindingValue::Kind::AmbiguousString;
                    return result;
                }
                if (index.intValue >= 0 &&
                    static_cast<std::size_t>(index.intValue) < array.array->elements.size()) {
                    const auto& element =
                        array.array->elements[static_cast<std::size_t>(index.intValue)];
                    if (element) {
                        result.kind = BindingValue::Kind::String;
                        result.text = *element;
                    }
                }
                return result;
            }
            return BindingValue::unknown(fallback);
        }

        if ((opcode == Opcodes::IADD || opcode == Opcodes::ISUB) &&
            value1.kind == BindingValue::Kind::Int && value2.kind == BindingValue::Kind::Int) {
            BindingValue result = BindingValue::unknown(fallback);
            result.kind         = BindingValue::Kind::Int;
            result.intValue     = opcode == Opcodes::IADD ? value1.intValue + value2.intValue
                                                          : value1.intValue - value2.intValue;
            return result;
        }
        return BindingValue::unknown(fallback);
    }

    BindingValue ternaryOperation(const AbstractInsnNode* insn, const BindingValue& value1,
                                  const BindingValue& value2, const BindingValue& value3) override
    {
        if (insn->getOpcode() == Opcodes::AASTORE && value1.kind == BindingValue::Kind::ClassArray &&
            value1.array) {
            //共享变异（引用语义）：AASTORE 无栈产出，写时复制会让存储彻底丢失
            //（javac 的 astore/aload 与 inline DUP 模式都依赖同一对象可见性）。
            //与 Recaf evaluator 的身份共享对象同口径；分支合并的冲突折损由
            //merge 的逐位 fail-closed 兜底。
            auto&         state  = *value1.array;
            BindingValue writer = value3;
            if (writer.kind == BindingValue::Kind::ClassObject) {
                writer.text = normalizeClassName(writer.text);
            }
            const bool valueOk = state.isClassArray
                                     ? writer.kind == BindingValue::Kind::ClassObject
                                     : writer.kind == BindingValue::Kind::String;
            if (state.ambiguous || value2.kind != BindingValue::Kind::Int || !valueOk) {
                state.ambiguous = true;
            }
            else if (value2.intValue < 0 ||
                     static_cast<std::size_t>(value2.intValue) >= state.elements.size()) {
                state.ambiguous = true;
            }
            else {
                state.elements[static_cast<std::size_t>(value2.intValue)] = writer.text;
            }
            return BindingValue::unknown(
                basic_.ternaryOperation(insn, value1.basic, value2.basic, value3.basic));
        }
        return BindingValue::unknown(
            basic_.ternaryOperation(insn, value1.basic, value2.basic, value3.basic));
    }

    BindingValue naryOperation(const AbstractInsnNode*          insn,
                               const std::vector<BindingValue>& values) override
    {
        BindingValue result = BindingValue::unknown(basic_.naryOperation(insn, basicValues(values)));

        if (insn->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN) {
            auto* indy = ca::core::tag_cast::cast<const InvokeDynamicInsnNode>(insn);
            if (isStringConcatFactory(*indy) && isConcatBootstrapName(*indy)) {
                return foldConcat(*indy, values, result.basic);
            }
            return result;
        }

        if (insn->getType() != AbstractInsnNode::METHOD_INSN) return result;
        auto* method = ca::core::tag_cast::cast<const MethodInsnNode>(insn);
        const bool hasReceiver = method->getOpcode() != Opcodes::INVOKESTATIC;
        //values 布局：[receiver?] + 显式实参。目录槽位以显式实参为基准。
        const std::size_t receiverOffset = hasReceiver ? 1U : 0U;
        auto valueAt = [&](int slot) -> const BindingValue* {
            if (slot < 0) return nullptr;
            const auto index = static_cast<std::size_t>(slot) + receiverOffset;
            if (index >= values.size()) return nullptr;
            return &values[index];
        };

        //StringBuilder/StringBuffer 链（append/toString/<init>）
        if (isStringBuilderOwner(*method)) {
            const std::string name = str(method->name);
            const bool        init = name == "<init>" &&
                              method->getOpcode() == Opcodes::INVOKESPECIAL;
            if (!init && method->getOpcode() != Opcodes::INVOKEVIRTUAL) return result;
            if (values.empty() || values[0].kind != BindingValue::Kind::StringBuilder) return result;

            if (name == "toString") {
                if (values[0].builder && values[0].builder->known) {
                    if (values[0].builder->ambiguous) {
                        result.kind = BindingValue::Kind::AmbiguousString;
                    }
                    else {
                        result.kind = BindingValue::Kind::String;
                        result.text = values[0].builder->text;
                    }
                }
                return result;
            }
            if (name == "append" || name == "insert") {
                //append(x) 取末实参；insert(index, x) 取第 2 实参（index 槽 0）。
                //共享变异（引用语义）：javac 的 `ALOAD sb; append; POP` 依赖
                //同一对象可见性，写时复制会让拼接丢失（与 Recaf evaluator 的
                //身份共享 StringBuilder 同口径）。
                const BindingValue* appended =
                    name == "append" ? valueAt(0) : valueAt(1);
                if (name == "insert" && appended == nullptr) appended = valueAt(0);
                if (values[0].builder) {
                    if (!appended) {
                        values[0].builder->known = false;
                    }
                    else if (values[0].builder->known) {
                        if (auto part = renderConcatArgument(*appended)) {
                            if (part->ambiguous) {
                                values[0].builder->ambiguous = true;
                            }
                            else {
                                values[0].builder->text += part->text;
                            }
                        }
                        else {
                            values[0].builder->known = false;
                        }
                    }
                }
                result.kind    = BindingValue::Kind::StringBuilder;
                result.builder = values[0].builder;
                return result;
            }
            if (init) {
                if (values[0].builder && values.size() >= 2 && values[0].builder->known) {
                    if (auto part = renderConcatArgument(values[1])) {
                        if (part->ambiguous) {
                            values[0].builder->ambiguous = true;
                        }
                        else {
                            values[0].builder->text += part->text;
                        }
                    }
                    else {
                        values[0].builder->known = false;
                    }
                }
                result.kind    = BindingValue::Kind::StringBuilder;
                result.builder = values[0].builder;
                return result;
            }
            return result;
        }

        const ReflectionSinkEntry* sink = matchSink(*method);
        if (sink == nullptr || sink->catalogOnly) return result;

        //类加载 sink：类名实参可判定时产出 ClassObject（供后续成员查找接收）。
        if (sink->classNameArg >= 0) {
            const BindingValue* argument = valueAt(sink->classNameArg);
            if (argument && argument->kind == BindingValue::Kind::String) {
                result.kind = BindingValue::Kind::ClassObject;
                result.text = normalizeClassName(argument->text);
            }
            return result;
        }

        //成员查找：产出成员对象（供 Method.invoke/Field.get/set/newInstance 追踪）。
        if (sink->producesMemberObject) {
            auto state = std::make_shared<MemberState>();
            state->isField = std::strstr(sink->name, "Field") != nullptr;
            const bool isConstructor = std::strstr(sink->name, "Constructor") != nullptr;
            state->name     = isConstructor ? "<init>" : std::string();
            state->isField  = state->isField;

            const BindingValue* receiver = hasReceiver && !values.empty() ? &values[0] : nullptr;
            if (receiver && receiver->kind == BindingValue::Kind::ClassObject) {
                state->owner = normalizeClassName(receiver->text);
            }
            if (sink->memberNameArg >= 0) {
                const BindingValue* name = valueAt(sink->memberNameArg);
                if (name && name->kind == BindingValue::Kind::String) {
                    state->name = name->text;
                }
            }
            if (sink->paramTypesArg >= 0) {
                const BindingValue* types = valueAt(sink->paramTypesArg);
                if (types && types->kind == BindingValue::Kind::ClassArray && types->array &&
                    types->array->sizeKnown && !types->array->ambiguous) {
                    bool        allKnown = true;
                    std::string desc     = "(";
                    for (const auto& element : types->array->elements) {
                        if (!element) {
                            allKnown = false;
                            break;
                        }
                        desc += toParameterDescriptor(*element);
                    }
                    if (allKnown) state->desc = desc + ")";
                }
            }
            result.kind   = BindingValue::Kind::MemberObject;
            result.member = std::move(state);
            return result;
        }

        return result;
    }

    void returnOperation(const AbstractInsnNode* insn, const BindingValue& value,
                         const BindingValue& expected) override
    {
        basic_.returnOperation(insn, value.basic, expected.basic);
    }

    BindingValue merge(const BindingValue& value1, const BindingValue& value2) override
    {
        BasicValue merged = basic_.merge(value1.basic, value2.basic);
        if (value1 == value2) {
            BindingValue result = value1;
            result.basic        = merged;
            return result;
        }
        //StringBuilder 合并：内容一致保留，冲突降 unknown 内容（保形态）。
        if (value1.kind == BindingValue::Kind::StringBuilder &&
            value2.kind == BindingValue::Kind::StringBuilder && value1.builder && value2.builder) {
            auto         state = std::make_shared<StringBuilderState>();
            state->known       = false;
            BindingValue result = BindingValue::unknown(merged);
            result.kind         = BindingValue::Kind::StringBuilder;
            result.builder      = std::move(state);
            return result;
        }
        //ClassArray/NameTable 合并：逐位一致保留，冲突置 ambiguous（保数组形态）。
        if (((value1.kind == BindingValue::Kind::ClassArray &&
              value2.kind == BindingValue::Kind::ClassArray) ||
             (value1.kind == BindingValue::Kind::NameTable &&
              value2.kind == BindingValue::Kind::NameTable)) &&
            value1.array && value2.array &&
            value1.array->isClassArray == value2.array->isClassArray &&
            value1.array->sizeKnown == value2.array->sizeKnown &&
            value1.array->size == value2.array->size &&
            value1.array->elements.size() == value2.array->elements.size()) {
            auto state = std::make_shared<ArrayState>(*value1.array);
            for (std::size_t i = 0; i < state->elements.size(); ++i) {
                const auto& a = value1.array->elements[i];
                const auto& b = value2.array->elements[i];
                if (a.has_value() != b.has_value() || (a.has_value() && *a != *b)) {
                    state->elements[i].reset();
                    state->ambiguous = true;
                }
            }
            if (value1.array->ambiguous || value2.array->ambiguous) state->ambiguous = true;
            BindingValue result = BindingValue::unknown(merged);
            result.kind         = value1.kind;
            result.array        = std::move(state);
            return result;
        }
        return BindingValue::unknown(merged);
    }

private:
    std::vector<BasicValue> basicValues(const std::vector<BindingValue>& values) const
    {
        std::vector<BasicValue> result;
        result.reserve(values.size());
        for (const auto& value : values) result.push_back(value.basic);
        return result;
    }

    ///@brief StringConcatFactory indy 折叠：makeConcatWithConstants 按 recipe
    ///（\u0001 占位按序消费动态实参）展开；makeConcat 全参顺序拼接。AmbiguousString
    ///实参经哨兵渲染不阻断折叠：全部实参可渲染/歧义且至少一个歧义 → AmbiguousString
    ///（保 reason 归因，歧义段文本不拼入）；任一实参完全不可渲染 → Unknown。
    ///与 mbase evaluator 的 evaluateIndyConcat 同口径。
    BindingValue foldConcat(const InvokeDynamicInsnNode&     indy,
                            const std::vector<BindingValue>& values,
                            const BasicValue&                basic)
    {
        bool        ambiguous = false;
        std::string out;
        auto appendPart = [&](const BindingValue& value) -> bool {
            auto part = renderConcatArgument(value);
            if (!part) return false;
            if (part->ambiguous) {
                ambiguous = true;
            }
            else {
                out += part->text;
            }
            return true;
        };

        if (indy.bsm.getName().equals(ca::str::Utf8StringRef::from_cstr("makeConcatWithConstants"))) {
            if (indy.bsmArgs.empty()) return BindingValue::unknown(basic);
            auto recipe = stringFromAny(indy.bsmArgs[0]);
            if (!recipe) return BindingValue::unknown(basic);
            std::size_t argIdx = 0;
            for (char c : *recipe) {
                if (c == '\x01') {
                    if (argIdx >= values.size()) return BindingValue::unknown(basic);
                    if (!appendPart(values[argIdx])) return BindingValue::unknown(basic);
                    ++argIdx;
                }
                else if (c == '\x02') {
                    //静态常量占位（bsmArgs[1..]）：罕见，fail-closed。
                    return BindingValue::unknown(basic);
                }
                else {
                    out += c;
                }
            }
        }
        else {
            for (const auto& value : values) {
                if (!appendPart(value)) return BindingValue::unknown(basic);
            }
        }
        BindingValue result = BindingValue::unknown(basic);
        if (ambiguous) {
            result.kind = BindingValue::Kind::AmbiguousString;
            return result;
        }
        result.kind = BindingValue::Kind::String;
        result.text = std::move(out);
        return result;
    }

    const ReflectionBindingAnalyzer& analyzer_;
    BasicInterpreter                 basic_;
};

}   //namespace

//── 标签函数 ─────────────────────────────────────────────────

const char* reflectionApiKindLabel(ReflectionApiKind kind)
{
    switch (kind) {
    case ReflectionApiKind::ClassForName: return "Class.forName";
    case ReflectionApiKind::ClassLoaderLoadClass: return "ClassLoader.loadClass";
    case ReflectionApiKind::MemberLookup: return "Class.member-lookup";
    case ReflectionApiKind::MethodInvoke: return "Method.invoke";
    case ReflectionApiKind::ConstructorNewInstance: return "Constructor.newInstance";
    case ReflectionApiKind::ClassNewInstance: return "Class.newInstance";
    case ReflectionApiKind::FieldAccess: return "Field.access";
    case ReflectionApiKind::LookupMethodHandle: return "MethodHandles.Lookup";
    }
    return "unknown";
}

const char* reflectionPendingReasonLabel(ReflectionPendingReason reason)
{
    switch (reason) {
    case ReflectionPendingReason::None: return "";
    case ReflectionPendingReason::UnknownArgs: return "unknown_args";
    case ReflectionPendingReason::AmbiguousArray: return "ambiguous_array";
    case ReflectionPendingReason::UnknownReceiver: return "unknown_receiver";
    case ReflectionPendingReason::NoSuchClass: return "no_such_class";
    case ReflectionPendingReason::NoSuchMember: return "no_such_member";
    case ReflectionPendingReason::MemberEnumeration: return "member_enumeration";
    }
    return "unknown";
}

const char* reflectionBindingStateLabel(ReflectionBindingState state)
{
    switch (state) {
    case ReflectionBindingState::Resolved: return "resolved";
    case ReflectionBindingState::Pending: return "pending";
    case ReflectionBindingState::Unsupported: return "unsupported";
    }
    return "unknown";
}

const std::vector<ReflectionSinkEntry>& reflectionSinkCatalog()
{
    static const std::vector<ReflectionSinkEntry> catalog = buildSinkCatalog();
    return catalog;
}

//── 分析器 ───────────────────────────────────────────────────

namespace {

///@brief 方法体内是否存在跳入 [first, last] 区域 label 的跳转（与 reference_index
///的 hasControlFlowEntry 同口径的保守守卫）。
bool regionHasIncomingBranches(jbytecode::tree::MethodNode& method, AbstractInsnNode* first,
                               AbstractInsnNode* last)
{
    std::set<jbytecode::core::Label*> labels;
    for (auto* insn = first; insn; insn = insn->getNext()) {
        if (insn->getType() == AbstractInsnNode::LABEL) {
            labels.insert(ca::core::tag_cast::cast<jbytecode::tree::LabelNode>(insn)->label);
        }
        if (insn == last) break;
    }
    if (labels.empty()) return false;

    for (auto* insn = method.instructions.getFirst(); insn; insn = insn->getNext()) {
        if (insn->getType() == AbstractInsnNode::JUMP_INSN) {
            if (labels.count(ca::core::tag_cast::cast<jbytecode::tree::JumpInsnNode>(insn)->label) != 0)
                return true;
        }
        else if (insn->getType() == AbstractInsnNode::TABLESWITCH_INSN) {
            auto* switchInsn = ca::core::tag_cast::cast<jbytecode::tree::TableSwitchInsnNode>(insn);
            if (labels.count(switchInsn->dflt) != 0) return true;
            for (auto* label : switchInsn->labels) {
                if (labels.count(label) != 0) return true;
            }
        }
        else if (insn->getType() == AbstractInsnNode::LOOKUPSWITCH_INSN) {
            auto* switchInsn = ca::core::tag_cast::cast<jbytecode::tree::LookupSwitchInsnNode>(insn);
            if (labels.count(switchInsn->dflt) != 0) return true;
            for (auto* label : switchInsn->labels) {
                if (labels.count(label) != 0) return true;
            }
        }
    }
    return false;
}

///@brief 扫描 `<clinit>` 中 `ANEWARRAY String → 全量 AASTORE 序列 → PUTSTATIC`
///的 static final String[] 直线全量初始化，成功则登记名表。
///
///口径（issue ）：只认内联直线形态——ANEWARRAY 常量长度 ≥ 0、每个下标恰
///好存储一次、结尾 PUTSTATIC 声明类字段、区域内无跳转汇入。astore 中转、部分
///填充、分支填充一律不登记（fail-closed，查询端 Unknown）。
void scanStaticNameTables(jbytecode::tree::ClassNode&                                clazz,
                          const std::string&                                         className,
                          std::unordered_map<std::string, std::vector<std::string>>& tables)
{
    for (auto* method : clazz.methods) {
        if (!method || str(method->name) != "<clinit>") continue;

        for (auto* insn = method->instructions.getFirst(); insn; insn = insn->getNext()) {
            if (insn->getType() != AbstractInsnNode::TYPE_INSN ||
                insn->getOpcode() != Opcodes::ANEWARRAY) {
                continue;
            }
            auto* arrayInsn = ca::core::tag_cast::cast<jbytecode::tree::TypeInsnNode>(insn);
            //ANEWARRAY 的 desc 是元素类型：String[] 建数组为 ANEWARRAY java/lang/String
            if (!arrayInsn->desc.equals(ca::str::Utf8StringRef::from_cstr("java/lang/String"))) {
                continue;
            }

            ca::i32           size    = -1;
            AbstractInsnNode* sizeInsn = previousReferenceInsn(arrayInsn->getPrevious());
            if (!readIntConstant(sizeInsn, size) || size < 0) continue;

            //直线填充序列：(DUP; <const>; LDC string; AASTORE)*
            std::vector<std::optional<std::string>> elements(static_cast<std::size_t>(size));
            bool                                    broken = false;
            auto*                                   cursor = arrayInsn->getNext();
            AbstractInsnNode*                       putInsn = nullptr;
            while (cursor) {
                auto* step = previousReferenceInsn(cursor);
                if (!step) {
                    broken = true;
                    break;
                }
                if (step->getOpcode() == Opcodes::PUTSTATIC) {
                    putInsn = step;
                    break;
                }
                if (step->getOpcode() != Opcodes::DUP) {
                    broken = true;
                    break;
                }
                auto* indexInsn = nextReferenceInsn(step->getNext());
                ca::i32 index   = -1;
                if (!readIntConstant(indexInsn, index) || index < 0 ||
                    static_cast<std::size_t>(index) >= elements.size() ||
                    elements[static_cast<std::size_t>(index)].has_value()) {
                    broken = true;
                    break;
                }
                auto* valueInsn = nextReferenceInsn(indexInsn->getNext());
                if (!valueInsn || valueInsn->getType() != AbstractInsnNode::LDC_INSN) {
                    broken = true;
                    break;
                }
                auto value =
                    stringFromAny(ca::core::tag_cast::cast<const LdcInsnNode>(valueInsn)->cst);
                if (!value) {
                    broken = true;
                    break;
                }
                auto* storeInsn = nextReferenceInsn(valueInsn->getNext());
                if (!storeInsn || storeInsn->getOpcode() != Opcodes::AASTORE) {
                    broken = true;
                    break;
                }
                elements[static_cast<std::size_t>(index)] = *value;
                cursor = storeInsn->getNext();
            }

            if (broken || putInsn == nullptr) continue;
            auto* put = ca::core::tag_cast::cast<jbytecode::tree::FieldInsnNode>(putInsn);
            if (str(put->owner) != className ||
                !put->desc.equals(ca::str::Utf8StringRef::from_cstr("[Ljava/lang/String;"))) {
                continue;
            }

            bool full = true;
            for (const auto& element : elements) {
                if (!element) {
                    full = false;
                    break;
                }
            }
            if (!full) continue;
            if (regionHasIncomingBranches(*method, sizeInsn, putInsn)) continue;

            std::vector<std::string> table;
            table.reserve(elements.size());
            for (const auto& element : elements) table.push_back(*element);
            tables[className + "." + str(put->name)] = std::move(table);
        }
    }
}

}   //namespace

void ReflectionBindingAnalyzer::clear()
{
    classes_.clear();
    stringConstants_.clear();
    nameTables_.clear();
    bindings_.clear();
}

ReflectionBindingSummary ReflectionBindingAnalyzer::summarize() const
{
    ReflectionBindingSummary summary;
    summary.byKind.assign(static_cast<std::size_t>(ReflectionApiKind::LookupMethodHandle) + 1, 0);
    for (const auto& binding : bindings_) {
        ++summary.total;
        switch (binding.state) {
        case ReflectionBindingState::Resolved: ++summary.resolved; break;
        case ReflectionBindingState::Pending: ++summary.pending; break;
        case ReflectionBindingState::Unsupported: ++summary.unsupported; break;
        }
        const auto kindIndex = static_cast<std::size_t>(binding.api);
        if (kindIndex < summary.byKind.size()) ++summary.byKind[kindIndex];
    }
    return summary;
}

std::vector<const ReflectionBinding*> ReflectionBindingAnalyzer::bindingsOfClass(
    const std::string& sourceClass) const
{
    std::vector<const ReflectionBinding*> result;
    for (const auto& binding : bindings_) {
        if (binding.site.sourceClass == sourceClass) result.push_back(&binding);
    }
    return result;
}

void ReflectionBindingAnalyzer::build(ResourceContainer& container, ca::str::Utf8StringArena& arena)
{
    clear();

    //第一趟：声明底表 + static final String 常量 + String[] 名表
    for (auto& [uri, resource] : container.getResources()) {
        if (resource->getType() != ResourceType::Class) continue;
        auto* cls = ca::core::tag_cast::cast<ClassResource>(resource.get());
        if (!cls || cls->get().empty()) continue;
        try {
            indexDeclarations(cls->get(), arena);
        }
        catch (...) {
            //声明底表缺失该类时，涉及它的 sink 一律 fail-closed（no_such_class）。
        }
    }

    //第二趟：方法内值流 + sink 落解
    for (auto& [uri, resource] : container.getResources()) {
        if (resource->getType() != ResourceType::Class) continue;
        auto* cls = ca::core::tag_cast::cast<ClassResource>(resource.get());
        if (!cls || cls->get().empty()) continue;
        try {
            analyzeClass(cls->get(), arena);
        }
        catch (...) {
            //单类控制流损坏不拖垮整体（与 ReferenceIndex 同纪律）。
        }
    }
}

void ReflectionBindingAnalyzer::indexDeclarations(const std::vector<ca::u8>& bytes,
                                                  ca::str::Utf8StringArena&  arena)
{
    jbytecode::tree::ClassNode    clazz;
    jbytecode::core::ClassReader reader(arena, bytes);
    reader.accept(&clazz,
                  jbytecode::core::ClassReader::SKIP_DEBUG | jbytecode::core::ClassReader::SKIP_FRAMES);

    const std::string className = str(clazz.name);
    auto              result    = classes_.try_emplace(className);
    if (result.second) {
        ClassInfo& info = result.first->second;
        for (auto* method : clazz.methods) {
            if (method) info.methods.emplace_back(str(method->name), str(method->desc));
        }
        for (auto* field : clazz.fields) {
            if (!field) continue;
            info.fields.emplace_back(str(field->name), str(field->desc));

            const bool staticFinal =
                (field->access & (Opcodes::ACC_STATIC | Opcodes::ACC_FINAL)) ==
                (Opcodes::ACC_STATIC | Opcodes::ACC_FINAL);
            if (!staticFinal) continue;
            if (str(field->desc) == "Ljava/lang/String;") {
                if (auto value = stringFromAny(field->value)) {
                    stringConstants_[className + "." + str(field->name)] = *value;
                }
            }
        }
    }

    scanStaticNameTables(clazz, className, nameTables_);
}

void ReflectionBindingAnalyzer::analyzeClass(const std::vector<ca::u8>& bytes,
                                             ca::str::Utf8StringArena&  arena)
{
    jbytecode::tree::ClassNode    clazz;
    jbytecode::core::ClassReader reader(arena, bytes);
    reader.accept(&clazz,
                  jbytecode::core::ClassReader::SKIP_DEBUG | jbytecode::core::ClassReader::SKIP_FRAMES);

    const std::string className = str(clazz.name);

    for (auto* method : clazz.methods) {
        if (!method) continue;
        if ((method->access & (Opcodes::ACC_ABSTRACT | Opcodes::ACC_NATIVE)) != 0) continue;

        BindingInterpreter     interpreter(*this);
        Analyzer<BindingValue> analyzer(&interpreter);
        std::vector<jbytecode::analysis::Frame<BindingValue>*> frames;
        try {
            frames = analyzer.analyze(clazz.name, method);
        }
        catch (const AnalyzerException&) {
            //该方法控制流不可分析：不产出绑定（fail-closed，不出错误结论）。
            continue;
        }

        for (ca::i32 index = 0; index < static_cast<ca::i32>(frames.size()); ++index) {
            auto* frame = frames[static_cast<std::size_t>(index)];
            auto* insn  = method->instructions.get(index);
            if (!frame || !insn || insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
            auto*                      methodInsn = ca::core::tag_cast::cast<const MethodInsnNode>(insn);
            const ReflectionSinkEntry* sink       = matchSink(*methodInsn);
            if (sink == nullptr) continue;

            const ca::i32 argumentCount =
                jbytecode::core::Type::getArgumentCount(methodInsn->desc);
            const bool    isStatic   = methodInsn->getOpcode() == Opcodes::INVOKESTATIC;
            const ca::i32 stackSize  = frame->getStackSize();
            const ca::i32 argsAt     = stackSize - argumentCount;
            const ca::i32 receiverAt = argsAt - (isStatic ? 0 : 1);
            if (argsAt < 0) continue;

            ReflectionBinding binding;
            binding.site.sourceClass  = className;
            binding.site.sourceMethod = str(method->name);
            binding.site.sourceDesc   = str(method->desc);
            binding.site.opcode       = methodInsn->getOpcode();
            binding.site.insnIndex    = index;
            binding.api               = sink->kind;

            //getStack 返回值拷贝；槽位以显式实参为基准（不含接收者）。
            auto argumentAt = [&](int slot) -> BindingValue {
                if (slot < 0) return BindingValue{};
                return frame->getStack(static_cast<int>(argsAt + slot));
            };

            //── catalog-only：Lookup 族 ──
            if (sink->catalogOnly) {
                binding.state  = ReflectionBindingState::Unsupported;
                bindings_.push_back(std::move(binding));
                continue;
            }

            //── 类名 sink（forName/loadClass）──
            if (sink->kind == ReflectionApiKind::ClassForName ||
                sink->kind == ReflectionApiKind::ClassLoaderLoadClass) {
                const BindingValue argument = argumentAt(sink->classNameArg);
                if (argument.kind == BindingValue::Kind::AmbiguousString) {
                    binding.state    = ReflectionBindingState::Pending;
                    binding.reason   = ReflectionPendingReason::AmbiguousArray;
                    binding.className = normalizeClassName(argument.text);
                }
                else if (argument.kind != BindingValue::Kind::String) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::UnknownArgs;
                }
                else {
                    binding.className = normalizeClassName(argument.text);
                    if (classes_.find(binding.className) == classes_.end()) {
                        binding.state  = ReflectionBindingState::Pending;
                        binding.reason = ReflectionPendingReason::NoSuchClass;
                    }
                    else {
                        binding.state        = ReflectionBindingState::Resolved;
                        binding.target.owner = binding.className;
                    }
                }
                bindings_.push_back(std::move(binding));
                continue;
            }

            //── 成员查找族 ──
            if (sink->kind == ReflectionApiKind::MemberLookup) {
                const BindingValue receiver = frame->getStack(static_cast<int>(receiverAt));
                if (receiver.kind == BindingValue::Kind::ClassObject) {
                    binding.className = normalizeClassName(receiver.text);
                }

                if (sink->memberEnumeration) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = binding.className.empty()
                                         ? ReflectionPendingReason::UnknownReceiver
                                         : ReflectionPendingReason::MemberEnumeration;
                    bindings_.push_back(std::move(binding));
                    continue;
                }

                if (sink->memberNameArg >= 0) {
                    const BindingValue nameArgument = argumentAt(sink->memberNameArg);
                    if (nameArgument.kind == BindingValue::Kind::String) {
                        binding.memberName = nameArgument.text;
                    }
                    else if (nameArgument.kind == BindingValue::Kind::AmbiguousString) {
                        binding.state  = ReflectionBindingState::Pending;
                        binding.reason = ReflectionPendingReason::AmbiguousArray;
                        bindings_.push_back(std::move(binding));
                        continue;
                    }
                }

                //参数类型两级口径：Class[] 元素全知 → 构建完整参数描述符（精确
                //匹配用）；元素不全/含 Unknown 元素 → 仅个数（回退宽松匹配）。
                bool        typesVerified = true;
                std::string wantParams;
                if (sink->paramTypesArg >= 0) {
                    const BindingValue types = argumentAt(sink->paramTypesArg);
                    if (types.kind == BindingValue::Kind::ClassArray && types.array &&
                        types.array->sizeKnown && !types.array->ambiguous) {
                        binding.paramTypeCount = types.array->size;
                        bool        allKnown   = true;
                        std::string desc       = "(";
                        for (const auto& element : types.array->elements) {
                            if (!element) {
                                allKnown = false;
                                break;
                            }
                            desc += toParameterDescriptor(*element);
                        }
                        if (allKnown) wantParams = desc + ")";
                        typesVerified = true;
                    }
                    else {
                        typesVerified = false;
                    }
                }
                //构造器查找无名字实参槽：目标成员名固定为 <init>。
                const bool isConstructorSink = std::strstr(sink->name, "Constructor") != nullptr;
                if (isConstructorSink && binding.memberName.empty()) {
                    binding.memberName = "<init>";
                }

                if (binding.className.empty()) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::UnknownReceiver;
                    bindings_.push_back(std::move(binding));
                    continue;
                }
                if (binding.memberName.empty() || !typesVerified) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::UnknownArgs;
                    bindings_.push_back(std::move(binding));
                    continue;
                }

                //存在性验证（fail-closed 核心）：类在容器内 + 成员命中声明 +
                //参数类型可验证。Class[] 元素全知时按参数列表精确匹配（与执行族
                //同口径，消除同名同参数个数重载的误报面）；元素不全时放宽为个数
                //匹配（命中第一个名字+个数相符声明并记录其真实描述符）。
                auto classIt = classes_.find(binding.className);
                if (classIt == classes_.end()) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::NoSuchClass;
                    bindings_.push_back(std::move(binding));
                    continue;
                }
                bool matched = false;
                if (std::strstr(sink->name, "Field") != nullptr) {
                    //字段无重载：名字命中即唯一。
                    for (const auto& [fieldName, fieldDesc] : classIt->second.fields) {
                        if (fieldName == binding.memberName) {
                            binding.target.owner = binding.className;
                            binding.target.name  = fieldName;
                            binding.target.desc  = fieldDesc;
                            matched              = true;
                            break;
                        }
                    }
                }
                else {
                    const bool isConstructor = isConstructorSink;
                    if (isConstructor) binding.memberName = "<init>";
                    auto paramListOf = [](const std::string& desc) {
                        const auto close = desc.find(')');
                        return close == std::string::npos ? desc : desc.substr(0, close + 1);
                    };
                    for (const auto& [methodName, methodDesc] : classIt->second.methods) {
                        if (methodName != binding.memberName) continue;
                        if (jbytecode::core::Type::getArgumentCount(
                                ca::str::Utf8StringRef::from_cstr(methodDesc.c_str())) !=
                            binding.paramTypeCount) {
                            continue;
                        }
                        if (!wantParams.empty() && paramListOf(methodDesc) != wantParams) {
                            continue;
                        }
                        binding.target.owner = binding.className;
                        binding.target.name  = methodName;
                        binding.target.desc  = methodDesc;
                        matched              = true;
                        break;
                    }
                }
                if (!matched) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::NoSuchMember;
                }
                else {
                    binding.state = ReflectionBindingState::Resolved;
                }
                bindings_.push_back(std::move(binding));
                continue;
            }

            //── 反射执行 / 字段访问族 ──
            const BindingValue receiver = frame->getStack(static_cast<int>(receiverAt));

            if (sink->kind == ReflectionApiKind::ClassNewInstance) {
                if (receiver.kind != BindingValue::Kind::ClassObject) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::UnknownReceiver;
                    bindings_.push_back(std::move(binding));
                    continue;
                }
                binding.className = normalizeClassName(receiver.text);
                auto classIt      = classes_.find(binding.className);
                if (classIt == classes_.end()) {
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::NoSuchClass;
                }
                else {
                    binding.state          = ReflectionBindingState::Resolved;
                    binding.target.owner   = binding.className;
                    binding.target.name    = "<init>";
                    binding.target.desc    = "()V";
                    binding.paramTypeCount = 0;
                }
                bindings_.push_back(std::move(binding));
                continue;
            }

            if (receiver.kind != BindingValue::Kind::MemberObject || !receiver.member) {
                binding.state  = ReflectionBindingState::Pending;
                binding.reason = ReflectionPendingReason::UnknownReceiver;
                bindings_.push_back(std::move(binding));
                continue;
            }

            binding.className  = normalizeClassName(receiver.member->owner);
            binding.memberName = receiver.member->name;
            auto classIt       = classes_.find(binding.className);
            if (binding.className.empty() || classIt == classes_.end()) {
                binding.state  = ReflectionBindingState::Pending;
                binding.reason = ReflectionPendingReason::NoSuchClass;
                bindings_.push_back(std::move(binding));
                continue;
            }
            if (binding.memberName.empty()) {
                binding.state  = ReflectionBindingState::Pending;
                binding.reason = ReflectionPendingReason::UnknownArgs;
                bindings_.push_back(std::move(binding));
                continue;
            }

            bool matched = false;
            if (receiver.member->isField) {
                //字段无重载：名字命中即唯一（desc 由声明回填）。
                for (const auto& [fieldName, fieldDesc] : classIt->second.fields) {
                    if (fieldName == binding.memberName) {
                        binding.target.owner = binding.className;
                        binding.target.name  = fieldName;
                        binding.target.desc  = fieldDesc;
                        matched              = true;
                        break;
                    }
                }
            }
            else {
                if (receiver.member->desc.empty()) {
                    //方法/构造器参数列表不可判定：无法验证 → fail-closed。
                    binding.state  = ReflectionBindingState::Pending;
                    binding.reason = ReflectionPendingReason::UnknownArgs;
                    bindings_.push_back(std::move(binding));
                    continue;
                }
                //MemberState.desc 是参数列表形态 "(...)"（无返回类型），与声明
                //描述符的参数段精确比对。
                const auto wantClose = receiver.member->desc.find(')');
                const std::string wantParams =
                    wantClose == std::string::npos
                        ? receiver.member->desc
                        : receiver.member->desc.substr(0, wantClose + 1);
                auto paramListOf = [](const std::string& desc) {
                    const auto close = desc.find(')');
                    return close == std::string::npos ? desc : desc.substr(0, close + 1);
                };
                for (const auto& [methodName, methodDesc] : classIt->second.methods) {
                    if (methodName == binding.memberName &&
                        paramListOf(methodDesc) == wantParams) {
                        binding.target.owner = binding.className;
                        binding.target.name  = methodName;
                        binding.target.desc  = methodDesc;
                        matched              = true;
                        break;
                    }
                }
            }
            if (!matched) {
                binding.state  = ReflectionBindingState::Pending;
                binding.reason = ReflectionPendingReason::NoSuchMember;
            }
            else {
                binding.state = ReflectionBindingState::Resolved;
                if (!receiver.member->isField && !binding.target.desc.empty()) {
                    binding.paramTypeCount = jbytecode::core::Type::getArgumentCount(
                        ca::str::Utf8StringRef::from_cstr(binding.target.desc.c_str()));
                }
            }
            bindings_.push_back(std::move(binding));
        }
    }
}

}   //namespace mjt::base
