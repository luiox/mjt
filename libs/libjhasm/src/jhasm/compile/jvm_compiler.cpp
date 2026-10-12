#include "jvm_compiler.hpp"

#include "jhasm/instructions/jvm_instructions.hpp"

#include <jblw/annotation/annotation.hpp>
#include <jblw/annotation/builders.hpp>
#include <jblw/annotation/elements.hpp>
#include <jblw/asm/bytecode_library.hpp>
#include <jblw/classfile/generic/builders.hpp>
#include <jblw/code/generic/code_model.hpp>
#include <jblw/constant/of_double.hpp>
#include <jblw/constant/of_dynamic.hpp>
#include <jblw/constant/of_float.hpp>
#include <jblw/constant/of_int.hpp>
#include <jblw/constant/of_long.hpp>
#include <jblw/constant/of_method_handle.hpp>
#include <jblw/constant/of_string.hpp>
#include <jblw/constant/of_type.hpp>
#include <jblw/type/types.hpp>

#include <jbytecode/core/opcodes.hpp>
#include <jbytecode/core/frame.hpp>

#include <libca/str/utf8_string_arena.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace jhasm::compile {
namespace {
namespace O = jbytecode::core;
using namespace jblw;

class CompilerCommonSuperClassProvider final : public jbytecode::core::ICommonSuperClassProvider
{
public:
    explicit CompilerCommonSuperClassProvider(const analysis::InheritanceChecker& inheritance)
        : inheritance_(&inheritance)
    {}

    ca::str::Utf8StringRef getCommonSuperClass(const ca::str::Utf8StringRef& first,
                                               const ca::str::Utf8StringRef& second) override
    {
        auto common = inheritance_->commonSuperclass(first, second);
        return arena_.intern(common.ref());
    }

private:
    const analysis::InheritanceChecker* inheritance_;
    ca::str::Utf8StringArena            arena_;
};

bool equals(ca::str::Utf16StringRef value, const char* ascii) noexcept
{
    ca::usize index = 0;
    for (; ascii[index] != '\0'; ++index) {
        if (index >= value.length() ||
            value.char_at(index).value != static_cast<ca::u16>(ascii[index]))
            return false;
    }
    return index == value.length();
}

ca::str::Utf8String utf8(ca::str::Utf16StringRef value)
{
    return value.to_utf8_string();
}

ca::str::Utf8String utf8(const ast::Element& value)
{
    return utf8(value.content());
}

std::string narrow(const ast::Element& value)
{
    return value.content().to_utf8_string().to_std_string();
}

ca::str::Utf16String diagnosticText(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

ca::i32 access(const ast::Modifiers& modifiers)
{
    ca::i32 result = 0;
    for (const auto& value : modifiers.values()) {
        if (!value) continue;
        const auto name = value->content();
        if (equals(name, "public"))
            result |= O::Opcodes::ACC_PUBLIC;
        else if (equals(name, "private"))
            result |= O::Opcodes::ACC_PRIVATE;
        else if (equals(name, "protected"))
            result |= O::Opcodes::ACC_PROTECTED;
        else if (equals(name, "static"))
            result |= O::Opcodes::ACC_STATIC;
        else if (equals(name, "final"))
            result |= O::Opcodes::ACC_FINAL;
        else if (equals(name, "super"))
            result |= O::Opcodes::ACC_SUPER;
        else if (equals(name, "synchronized"))
            result |= O::Opcodes::ACC_SYNCHRONIZED;
        else if (equals(name, "volatile"))
            result |= O::Opcodes::ACC_VOLATILE;
        else if (equals(name, "bridge"))
            result |= O::Opcodes::ACC_BRIDGE;
        else if (equals(name, "transient"))
            result |= O::Opcodes::ACC_TRANSIENT;
        else if (equals(name, "varargs"))
            result |= O::Opcodes::ACC_VARARGS;
        else if (equals(name, "native"))
            result |= O::Opcodes::ACC_NATIVE;
        else if (equals(name, "interface"))
            result |= O::Opcodes::ACC_INTERFACE;
        else if (equals(name, "abstract"))
            result |= O::Opcodes::ACC_ABSTRACT;
        else if (equals(name, "strictfp"))
            result |= O::Opcodes::ACC_STRICT;
        else if (equals(name, "synthetic"))
            result |= O::Opcodes::ACC_SYNTHETIC;
        else if (equals(name, "annotation"))
            result |= O::Opcodes::ACC_ANNOTATION;
        else if (equals(name, "enum"))
            result |= O::Opcodes::ACC_ENUM;
        else if (equals(name, "module"))
            result |= O::Opcodes::ACC_MODULE;
        else if (equals(name, "record"))
            result |= O::Opcodes::ACC_RECORD;
        else if (equals(name, "sealed"))
            result |= O::Opcodes::ACC_SEALED;
    }
    return result;
}

type::InstanceTypePtr instanceType(const ast::Identifier& value)
{
    auto name = utf8(value);
    if (name.byte_length() > 1 && name.byte_at(0) == 'L' &&
        name.byte_at(name.byte_length() - 1) == ';')
        return type::Types::instanceTypeFromDescriptor(name.ref()).unwrap();
    return type::Types::instanceTypeFromInternalName(name.ref());
}

type::ObjectTypePtr objectType(const ast::Identifier& value)
{
    auto name = utf8(value);
    if (name.byte_length() > 1 && name.byte_at(0) == 'L' &&
        name.byte_at(name.byte_length() - 1) == ';')
        name = ca::str::Utf8String::from_data(name.data() + 1, name.byte_length() - 2);
    return type::Types::objectTypeFromInternalName(name.ref()).unwrap();
}

ca::i64 integerValue(const ast::Element& value)
{
    std::string text = narrow(value);
    if (!text.empty() && (text.back() == 'l' || text.back() == 'L')) text.pop_back();
    return std::stoll(text, nullptr, 0);
}

ca::f64 floatingValue(const ast::Element& value)
{
    std::string text = narrow(value);
    if (!text.empty() &&
        (text.back() == 'f' || text.back() == 'F' || text.back() == 'd' || text.back() == 'D'))
        text.pop_back();
    return std::stod(text);
}

ca::i32 opcode(ca::str::Utf16StringRef name)
{
#define JHASM_OPCODE(Name) \
    if (equals(name, #Name)) return O::Opcodes::Name
    JHASM_OPCODE(NOP);
    JHASM_OPCODE(ACONST_NULL);
    JHASM_OPCODE(ICONST_M1);
    JHASM_OPCODE(ICONST_0);
    JHASM_OPCODE(ICONST_1);
    JHASM_OPCODE(ICONST_2);
    JHASM_OPCODE(ICONST_3);
    JHASM_OPCODE(ICONST_4);
    JHASM_OPCODE(ICONST_5);
    JHASM_OPCODE(LCONST_0);
    JHASM_OPCODE(LCONST_1);
    JHASM_OPCODE(FCONST_0);
    JHASM_OPCODE(FCONST_1);
    JHASM_OPCODE(FCONST_2);
    JHASM_OPCODE(DCONST_0);
    JHASM_OPCODE(DCONST_1);
    JHASM_OPCODE(BIPUSH);
    JHASM_OPCODE(SIPUSH);
    JHASM_OPCODE(LDC);
    JHASM_OPCODE(ILOAD);
    JHASM_OPCODE(LLOAD);
    JHASM_OPCODE(FLOAD);
    JHASM_OPCODE(DLOAD);
    JHASM_OPCODE(ALOAD);
    JHASM_OPCODE(IALOAD);
    JHASM_OPCODE(LALOAD);
    JHASM_OPCODE(FALOAD);
    JHASM_OPCODE(DALOAD);
    JHASM_OPCODE(AALOAD);
    JHASM_OPCODE(BALOAD);
    JHASM_OPCODE(CALOAD);
    JHASM_OPCODE(SALOAD);
    JHASM_OPCODE(ISTORE);
    JHASM_OPCODE(LSTORE);
    JHASM_OPCODE(FSTORE);
    JHASM_OPCODE(DSTORE);
    JHASM_OPCODE(ASTORE);
    JHASM_OPCODE(IASTORE);
    JHASM_OPCODE(LASTORE);
    JHASM_OPCODE(FASTORE);
    JHASM_OPCODE(DASTORE);
    JHASM_OPCODE(AASTORE);
    JHASM_OPCODE(BASTORE);
    JHASM_OPCODE(CASTORE);
    JHASM_OPCODE(SASTORE);
    JHASM_OPCODE(POP);
    JHASM_OPCODE(POP2);
    JHASM_OPCODE(DUP);
    JHASM_OPCODE(DUP_X1);
    JHASM_OPCODE(DUP_X2);
    JHASM_OPCODE(DUP2);
    JHASM_OPCODE(DUP2_X1);
    JHASM_OPCODE(DUP2_X2);
    JHASM_OPCODE(SWAP);
    JHASM_OPCODE(IADD);
    JHASM_OPCODE(LADD);
    JHASM_OPCODE(FADD);
    JHASM_OPCODE(DADD);
    JHASM_OPCODE(ISUB);
    JHASM_OPCODE(LSUB);
    JHASM_OPCODE(FSUB);
    JHASM_OPCODE(DSUB);
    JHASM_OPCODE(IMUL);
    JHASM_OPCODE(LMUL);
    JHASM_OPCODE(FMUL);
    JHASM_OPCODE(DMUL);
    JHASM_OPCODE(IDIV);
    JHASM_OPCODE(LDIV);
    JHASM_OPCODE(FDIV);
    JHASM_OPCODE(DDIV);
    JHASM_OPCODE(IREM);
    JHASM_OPCODE(LREM);
    JHASM_OPCODE(FREM);
    JHASM_OPCODE(DREM);
    JHASM_OPCODE(INEG);
    JHASM_OPCODE(LNEG);
    JHASM_OPCODE(FNEG);
    JHASM_OPCODE(DNEG);
    JHASM_OPCODE(ISHL);
    JHASM_OPCODE(LSHL);
    JHASM_OPCODE(ISHR);
    JHASM_OPCODE(LSHR);
    JHASM_OPCODE(IUSHR);
    JHASM_OPCODE(LUSHR);
    JHASM_OPCODE(IAND);
    JHASM_OPCODE(LAND);
    JHASM_OPCODE(IOR);
    JHASM_OPCODE(LOR);
    JHASM_OPCODE(IXOR);
    JHASM_OPCODE(LXOR);
    JHASM_OPCODE(IINC);
    JHASM_OPCODE(I2L);
    JHASM_OPCODE(I2F);
    JHASM_OPCODE(I2D);
    JHASM_OPCODE(L2I);
    JHASM_OPCODE(L2F);
    JHASM_OPCODE(L2D);
    JHASM_OPCODE(F2I);
    JHASM_OPCODE(F2L);
    JHASM_OPCODE(F2D);
    JHASM_OPCODE(D2I);
    JHASM_OPCODE(D2L);
    JHASM_OPCODE(D2F);
    JHASM_OPCODE(I2B);
    JHASM_OPCODE(I2C);
    JHASM_OPCODE(I2S);
    JHASM_OPCODE(LCMP);
    JHASM_OPCODE(FCMPL);
    JHASM_OPCODE(FCMPG);
    JHASM_OPCODE(DCMPL);
    JHASM_OPCODE(DCMPG);
    JHASM_OPCODE(IFEQ);
    JHASM_OPCODE(IFNE);
    JHASM_OPCODE(IFLT);
    JHASM_OPCODE(IFGE);
    JHASM_OPCODE(IFGT);
    JHASM_OPCODE(IFLE);
    JHASM_OPCODE(IF_ICMPEQ);
    JHASM_OPCODE(IF_ICMPNE);
    JHASM_OPCODE(IF_ICMPLT);
    JHASM_OPCODE(IF_ICMPGE);
    JHASM_OPCODE(IF_ICMPGT);
    JHASM_OPCODE(IF_ICMPLE);
    JHASM_OPCODE(IF_ACMPEQ);
    JHASM_OPCODE(IF_ACMPNE);
    JHASM_OPCODE(GOTO);
    JHASM_OPCODE(JSR);
    JHASM_OPCODE(RET);
    JHASM_OPCODE(TABLESWITCH);
    JHASM_OPCODE(LOOKUPSWITCH);
    JHASM_OPCODE(IRETURN);
    JHASM_OPCODE(LRETURN);
    JHASM_OPCODE(FRETURN);
    JHASM_OPCODE(DRETURN);
    JHASM_OPCODE(ARETURN);
    JHASM_OPCODE(RETURN);
    JHASM_OPCODE(GETSTATIC);
    JHASM_OPCODE(PUTSTATIC);
    JHASM_OPCODE(GETFIELD);
    JHASM_OPCODE(PUTFIELD);
    JHASM_OPCODE(INVOKEVIRTUAL);
    JHASM_OPCODE(INVOKESPECIAL);
    JHASM_OPCODE(INVOKESTATIC);
    JHASM_OPCODE(INVOKEINTERFACE);
    JHASM_OPCODE(INVOKEDYNAMIC);
    JHASM_OPCODE(NEW);
    JHASM_OPCODE(NEWARRAY);
    JHASM_OPCODE(ANEWARRAY);
    JHASM_OPCODE(ARRAYLENGTH);
    JHASM_OPCODE(ATHROW);
    JHASM_OPCODE(CHECKCAST);
    JHASM_OPCODE(INSTANCEOF);
    JHASM_OPCODE(MONITORENTER);
    JHASM_OPCODE(MONITOREXIT);
    JHASM_OPCODE(MULTIANEWARRAY);
    JHASM_OPCODE(IFNULL);
    JHASM_OPCODE(IFNONNULL);
#undef JHASM_OPCODE
    if (equals(name, "GOTO_W")) return 200;
    if (equals(name, "JSR_W")) return 201;
    if (equals(name, "LDC_W") || equals(name, "LDC2_W")) return O::Opcodes::LDC;
    if (equals(name, "INVOKEVIRTUALINTERFACE")) return O::Opcodes::INVOKEINTERFACE;
    if (equals(name, "INVOKESTATICINTERFACE")) return O::Opcodes::INVOKESTATIC;
    if (equals(name, "INVOKESPECIALINTERFACE")) return O::Opcodes::INVOKESPECIAL;
    throw std::invalid_argument("Unknown JVM opcode");
}

type::MethodHandlePtr methodHandle(const ast::ElementPtr& value);
constant::ConstantPtr constantValue(const ast::ElementPtr& value);

type::MethodHandlePtr makeHandle(ca::i32 kind, const std::string& path,
                                 const std::string& descriptor)
{
    const auto separator = path.rfind('.');
    if (separator == std::string::npos)
        throw std::invalid_argument("Handle path has no member name");
    auto ownerText =
        ca::str::Utf8String::from_data(reinterpret_cast<const ca::u8*>(path.data()), separator);
    auto name = ca::str::Utf8String::from_data(
        reinterpret_cast<const ca::u8*>(path.data() + separator + 1), path.size() - separator - 1);
    auto desc = ca::str::Utf8String::from_data(reinterpret_cast<const ca::u8*>(descriptor.data()),
                                               descriptor.size());
    type::TypePtr typeValue;
    if (kind <= O::Opcodes::H_PUTSTATIC)
        typeValue = type::Types::typeFromDescriptor(desc.ref()).unwrap();
    else
        typeValue = type::Types::methodType(desc.ref()).unwrap();
    return std::make_shared<const type::MethodHandle>(
        kind,
        type::Types::objectTypeFromInternalName(ownerText.ref()).unwrap(),
        std::move(name),
        std::move(typeValue),
        kind == O::Opcodes::H_INVOKEINTERFACE);
}

type::MethodHandlePtr shortcutHandle(ca::str::Utf16StringRef name)
{
    if (equals(name, "LambdaMetafactory.metafactory")) {
        return makeHandle(O::Opcodes::H_INVOKESTATIC,
                          "java/lang/invoke/LambdaMetafactory.metafactory",
                          "(Ljava/lang/invoke/MethodHandles$Lookup;Ljava/lang/String;Ljava/lang/"
                          "invoke/MethodType;"
                          "Ljava/lang/invoke/MethodType;Ljava/lang/invoke/MethodHandle;Ljava/lang/"
                          "invoke/MethodType;)"
                          "Ljava/lang/invoke/CallSite;");
    }
    throw std::invalid_argument("Unknown method handle shortcut");
}

type::MethodHandlePtr methodHandle(const ast::ElementPtr& value)
{
    if (!value) throw std::invalid_argument("Missing method handle");
    if (value->getType() == ast::ElementKind::IDENTIFIER) return shortcutHandle(value->content());
    if (value->getType() != ast::ElementKind::ARRAY)
        throw std::invalid_argument("Invalid method handle");
    const auto& parts = std::static_pointer_cast<ast::Array>(value)->values();
    if (parts.size() != 3) throw std::invalid_argument("Invalid method handle arity");
    ca::i32    kind     = 0;
    const auto kindName = parts[0]->content();
    if (equals(kindName, "getfield"))
        kind = O::Opcodes::H_GETFIELD;
    else if (equals(kindName, "getstatic"))
        kind = O::Opcodes::H_GETSTATIC;
    else if (equals(kindName, "putfield"))
        kind = O::Opcodes::H_PUTFIELD;
    else if (equals(kindName, "putstatic"))
        kind = O::Opcodes::H_PUTSTATIC;
    else if (equals(kindName, "invokevirtual"))
        kind = O::Opcodes::H_INVOKEVIRTUAL;
    else if (equals(kindName, "invokestatic"))
        kind = O::Opcodes::H_INVOKESTATIC;
    else if (equals(kindName, "invokespecial"))
        kind = O::Opcodes::H_INVOKESPECIAL;
    else if (equals(kindName, "newinvokespecial"))
        kind = O::Opcodes::H_NEWINVOKESPECIAL;
    else if (equals(kindName, "invokeinterface"))
        kind = O::Opcodes::H_INVOKEINTERFACE;
    else
        throw std::invalid_argument("Unknown method handle kind");
    return makeHandle(kind, narrow(*parts[1]), narrow(*parts[2]));
}

constant::ConstantPtr numberConstant(const ast::Number& number)
{
    if (number.isFloatingPoint()) {
        if (number.isWide())
            return std::make_shared<const constant::OfDouble>(floatingValue(number));
        return std::make_shared<const constant::OfFloat>(
            static_cast<ca::f32>(floatingValue(number)));
    }
    if (number.isWide()) return std::make_shared<const constant::OfLong>(integerValue(number));
    return std::make_shared<const constant::OfInt>(static_cast<ca::i32>(integerValue(number)));
}

constant::ConstantPtr constantValue(const ast::ElementPtr& value)
{
    if (!value) throw std::invalid_argument("Missing constant");
    switch (value->getType()) {
    case ast::ElementKind::NUMBER:
        return numberConstant(*std::static_pointer_cast<ast::Number>(value));
    case ast::ElementKind::STRING: return std::make_shared<const constant::OfString>(utf8(*value));
    case ast::ElementKind::CHARACTER:
        return std::make_shared<const constant::OfInt>(
            value->content().empty() ? 0 : value->content().char_at(0).value);
    case ast::ElementKind::BOOL:
        return std::make_shared<const constant::OfInt>(
            std::static_pointer_cast<ast::Bool>(value)->boolValue() ? 1 : 0);
    case ast::ElementKind::IDENTIFIER:
    {
        auto text = value->content();
        if (equals(text, "true")) return std::make_shared<const constant::OfInt>(1);
        if (equals(text, "false")) return std::make_shared<const constant::OfInt>(0);
        if (equals(text, "NaN") || equals(text, "NaND"))
            return std::make_shared<const constant::OfDouble>(
                std::numeric_limits<ca::f64>::quiet_NaN());
        if (equals(text, "NaNF"))
            return std::make_shared<const constant::OfFloat>(
                std::numeric_limits<ca::f32>::quiet_NaN());
        if (equals(text, "Infinity") || equals(text, "+Infinity") || equals(text, "InfinityD") ||
            equals(text, "+InfinityD"))
            return std::make_shared<const constant::OfDouble>(
                std::numeric_limits<ca::f64>::infinity());
        if (equals(text, "-Infinity") || equals(text, "-InfinityD"))
            return std::make_shared<const constant::OfDouble>(
                -std::numeric_limits<ca::f64>::infinity());
        if (equals(text, "InfinityF") || equals(text, "+InfinityF"))
            return std::make_shared<const constant::OfFloat>(
                std::numeric_limits<ca::f32>::infinity());
        if (equals(text, "-InfinityF"))
            return std::make_shared<const constant::OfFloat>(
                -std::numeric_limits<ca::f32>::infinity());
        auto descriptor = utf8(*value);
        if (descriptor.byte_length() &&
            (descriptor.byte_at(0) == 'L' || descriptor.byte_at(0) == '[' ||
             descriptor.byte_at(0) == '('))
            return std::make_shared<const constant::OfType>(
                type::Types::typeFromDescriptor(descriptor.ref()).unwrap());
        return std::make_shared<const constant::OfMethodHandle>(shortcutHandle(text));
    }
    case ast::ElementKind::ARRAY:
    {
        const auto& parts = std::static_pointer_cast<ast::Array>(value)->values();
        if (parts.size() == 3)
            return std::make_shared<const constant::OfMethodHandle>(methodHandle(value));
        if (parts.size() != 4) throw std::invalid_argument("Invalid dynamic constant");
        auto descriptor = utf8(*parts[1]);
        auto parsed     = type::Types::typeFromDescriptor(descriptor.ref()).unwrap();
        if (parsed->getType() == type::TypeKind::METHOD)
            throw std::invalid_argument("Dynamic constant requires field type");
        std::vector<constant::ConstantPtr> arguments;
        if (parts[3]->getType() == ast::ElementKind::ARRAY) {
            for (const auto& item : std::static_pointer_cast<ast::Array>(parts[3])->values())
                arguments.push_back(constantValue(item));
        }
        auto dynamic = std::make_shared<const type::ConstantDynamic>(
            utf8(*parts[0]),
            std::static_pointer_cast<const type::ClassType>(parsed),
            methodHandle(parts[2]),
            std::move(arguments));
        return std::make_shared<const constant::OfDynamic>(std::move(dynamic));
    }
    default: throw std::invalid_argument("Unsupported constant kind");
    }
}

annotation::ElementPtr annotationElement(const ast::ElementPtr& value);

annotation::AnnotationPtr annotationValue(const ast::Annotation& value)
{
    std::vector<annotation::AnnotationEntry> entries;
    for (const auto& entry : value.values())
        entries.emplace_back(utf8(*entry.first), annotationElement(entry.second));
    return std::make_shared<const annotation::MapAnnotation>(instanceType(*value.classType()),
                                                             std::move(entries));
}

annotation::ElementPtr annotationElement(const ast::ElementPtr& value)
{
    if (!value) throw std::invalid_argument("Missing annotation element");
    switch (value->getType()) {
    case ast::ElementKind::BOOL:
        return annotation::booleanElement(std::static_pointer_cast<ast::Bool>(value)->boolValue());
    case ast::ElementKind::CHARACTER:
        return std::make_shared<const annotation::ElementChar>(
            value->content().empty() ? 0 : value->content().char_at(0).value);
    case ast::ElementKind::STRING:
        return std::make_shared<const annotation::ElementString>(utf8(*value));
    case ast::ElementKind::NUMBER:
    {
        auto number = std::static_pointer_cast<ast::Number>(value);
        if (number->isFloatingPoint()) {
            if (number->isWide())
                return std::make_shared<const annotation::ElementDouble>(floatingValue(*number));
            return std::make_shared<const annotation::ElementFloat>(
                static_cast<ca::f32>(floatingValue(*number)));
        }
        if (number->isWide())
            return std::make_shared<const annotation::ElementLong>(integerValue(*number));
        return std::make_shared<const annotation::ElementInt>(
            static_cast<ca::i32>(integerValue(*number)));
    }
    case ast::ElementKind::IDENTIFIER:
    {
        if (equals(value->content(), "true")) return annotation::booleanElement(true);
        if (equals(value->content(), "false")) return annotation::booleanElement(false);
        return std::make_shared<const annotation::ElementType>(
            objectType(*std::static_pointer_cast<ast::Identifier>(value)));
    }
    case ast::ElementKind::ENUM_VALUE:
    {
        auto item = std::static_pointer_cast<ast::EnumValue>(value);
        return std::make_shared<const annotation::ElementEnum>(instanceType(*item->enumType()),
                                                               utf8(*item->enumValue()));
    }
    case ast::ElementKind::ANNOTATION:
        return annotationValue(*std::static_pointer_cast<ast::Annotation>(value));
    case ast::ElementKind::ARRAY:
    {
        std::vector<annotation::ElementPtr> values;
        for (const auto& item : std::static_pointer_cast<ast::Array>(value)->values())
            values.push_back(annotationElement(item));
        return std::make_shared<const annotation::ElementArray>(std::move(values));
    }
    default: throw std::invalid_argument("Unsupported annotation element");
    }
}

void fillAnnotation(annotation::AnnotationBuilder& builder, const ast::Annotation& value)
{
    for (const auto& entry : value.values())
        builder.element(utf8(*entry.first), annotationElement(entry.second));
}

void installAnnotations(classfile::generic::AnnotatedBuilderState& target,
                        const ast::Attributes&                     attributes)
{
    auto install = [&](const ast::AnnotationPtr& value, bool visible, bool typeAnnotation) {
        std::shared_ptr<annotation::AnnotationBuilder> builder;
        if (typeAnnotation) {
            const ca::i32 ref =
                value->typeRef() ? static_cast<ca::i32>(integerValue(*value->typeRef())) : 0;
            std::optional<annotation::TypePath> path;
            if (value->typePath())
                path = annotation::TypePath::fromString(utf8(*value->typePath()).ref());
            builder = visible ? target.addVisibleRuntimeTypeAnnotation(
                                    instanceType(*value->classType()), ref, path)
                              : target.addInvisibleRuntimeTypeAnnotation(
                                    instanceType(*value->classType()), ref, path);
        }
        else {
            builder = visible
                          ? target.addVisibleRuntimeAnnotation(instanceType(*value->classType()))
                          : target.addInvisibleRuntimeAnnotation(instanceType(*value->classType()));
        }
        fillAnnotation(*builder, *value);
    };
    for (const auto& value : attributes.visibleAnnotations) install(value, true, false);
    for (const auto& value : attributes.invisibleAnnotations) install(value, false, false);
    for (const auto& value : attributes.visibleTypeAnnotations) install(value, true, true);
    for (const auto& value : attributes.invisibleTypeAnnotations) install(value, false, true);
}

class CodeCompiler final
{
public:
    CodeCompiler(const ast::Method& method, type::MethodTypePtr methodType, ca::i32 maxStack)
        : method_(method)
        , methodType_(std::move(methodType))
        , maxStack_(maxStack)
    {
        ca::i32    slot      = 0;
        ca::usize  nameIndex = 0;
        const bool isStatic  = (access(method.modifiers()) & O::Opcodes::ACC_STATIC) != 0;
        if (!isStatic) {
            if (!method.parameters().empty())
                variables_[narrow(*method.parameters()[nameIndex++])] = slot;
            ++slot;
        }
        for (const auto& parameter : methodType_->parameterTypes()) {
            if (nameIndex < method.parameters().size())
                variables_[narrow(*method.parameters()[nameIndex++])] = slot;
            slot += type::Types::category(*parameter);
        }
        nextVariable_ = slot;
    }

    code::CodePtr compile()
    {
        code::GenericCodeBuilder builder;
        builder.maxStack(maxStack_);
        if (method_.code()) {
            for (const auto& item : method_.code()->instructions()) emit(builder, item);
        }
        if (!builder.codeList().firstLabel())
            builder.codeList().addLabel(0, std::make_shared<code::GenericLabel>());
        if (!builder.codeList().lastLabel())
            builder.codeList().addLabel(std::make_shared<code::GenericLabel>());
        for (const auto& value : method_.exceptions()) {
            type::InstanceTypePtr exceptionType;
            if (value->exceptionType() && !equals(value->exceptionType()->content(), "*"))
                exceptionType = instanceType(*value->exceptionType());
            builder.tryCatchBlock({label(*value->start()),
                                   label(*value->end()),
                                   label(*value->handler()),
                                   std::move(exceptionType)});
        }
        builder.maxLocals(nextVariable_);
        return builder.build();
    }

private:
    code::LabelPtr label(const ast::Identifier& value)
    {
        const std::string name  = narrow(value);
        auto              found = labels_.find(name);
        if (found != labels_.end()) return found->second;
        auto result = std::make_shared<code::GenericLabel>();
        labels_.emplace(name, result);
        return result;
    }

    ca::i32 variable(const ast::ElementPtr& value, bool wide)
    {
        if (value->getType() == ast::ElementKind::NUMBER)
            return static_cast<ca::i32>(integerValue(*value));
        const std::string name  = narrow(*value);
        auto              found = variables_.find(name);
        if (found != variables_.end()) return found->second;
        const ca::i32 result = nextVariable_;
        variables_.emplace(name, result);
        nextVariable_ += wide ? 2 : 1;
        return result;
    }

    void emit(code::GenericCodeBuilder& builder, const ast::InstructionPtr& item)
    {
        if (item->getType() == ast::ElementKind::LABEL) {
            builder.codeList().addLabel(label(*item->identifier()));
            return;
        }
        const auto name = item->identifier()->content();
        if (equals(name, "line")) {
            auto line = std::make_shared<code::GenericLabel>();
            line->setLineNumber(static_cast<ca::i32>(integerValue(*item->arguments()[0])));
            builder.codeList().addLabel(std::move(line));
            return;
        }
        const ca::i32        op   = opcodeUpper(name);
        const auto&          args = item->arguments();
        code::InstructionPtr instruction;
        if (equals(name, "bipush") || equals(name, "sipush")) {
            instruction = std::make_shared<code::IntConstantInstruction>(
                std::make_shared<const constant::OfInt>(
                    static_cast<ca::i32>(integerValue(*args[0]))));
        }
        else if (equals(name, "ldc") || equals(name, "ldc_w") || equals(name, "ldc2_w")) {
            instruction = code::ConstantInstruction::wrap(constantValue(args[0]));
        }
        else if (equals(name, "newarray")) {
            type::ClassTypePtr component;
            if (equals(args[0]->content(), "boolean"))
                component = type::Types::BOOLEAN;
            else if (equals(args[0]->content(), "byte"))
                component = type::Types::BYTE;
            else if (equals(args[0]->content(), "char"))
                component = type::Types::CHAR;
            else if (equals(args[0]->content(), "short"))
                component = type::Types::SHORT;
            else if (equals(args[0]->content(), "int"))
                component = type::Types::INT;
            else if (equals(args[0]->content(), "float"))
                component = type::Types::FLOAT;
            else if (equals(args[0]->content(), "long"))
                component = type::Types::LONG;
            else
                component = type::Types::DOUBLE;
            instruction =
                std::make_shared<code::AllocateInstruction>(type::Types::arrayType(component));
        }
        else if (equals(name, "new") || equals(name, "anewarray") || equals(name, "checkcast") ||
                 equals(name, "instanceof")) {
            auto typeValue = objectType(*std::static_pointer_cast<ast::Identifier>(args[0]));
            if (equals(name, "new"))
                instruction = std::make_shared<code::AllocateInstruction>(typeValue);
            else if (equals(name, "anewarray"))
                instruction =
                    std::make_shared<code::AllocateInstruction>(type::Types::arrayType(typeValue));
            else if (equals(name, "checkcast"))
                instruction = std::make_shared<code::CheckCastInstruction>(typeValue);
            else
                instruction = std::make_shared<code::InstanceofInstruction>(typeValue);
        }
        else if (isVarOpcode(op)) {
            const bool wide = op == O::Opcodes::LLOAD || op == O::Opcodes::DLOAD ||
                              op == O::Opcodes::LSTORE || op == O::Opcodes::DSTORE;
            instruction = std::make_shared<code::VarInstruction>(op, variable(args[0], wide));
        }
        else if (equals(name, "iinc")) {
            instruction = std::make_shared<code::VariableIncrementInstruction>(
                variable(args[0], false), static_cast<ca::i32>(integerValue(*args[1])));
        }
        else if (isJumpOpcode(op)) {
            auto target = label(*std::static_pointer_cast<ast::Identifier>(args[0]));
            if (op == O::Opcodes::GOTO || op == O::Opcodes::JSR || op == 200 || op == 201)
                instruction =
                    std::make_shared<code::ImmediateJumpInstruction>(op, std::move(target));
            else
                instruction =
                    std::make_shared<code::ConditionalJumpInstruction>(op, std::move(target));
        }
        else if (equals(name, "tableswitch")) {
            instruction = tableSwitch(*std::static_pointer_cast<ast::Object>(args[0]));
        }
        else if (equals(name, "lookupswitch")) {
            instruction = lookupSwitch(*std::static_pointer_cast<ast::Object>(args[0]));
        }
        else if (op >= O::Opcodes::GETSTATIC && op <= O::Opcodes::PUTFIELD) {
            instruction = fieldInstruction(op, args);
        }
        else if (op >= O::Opcodes::INVOKEVIRTUAL && op <= O::Opcodes::INVOKEINTERFACE) {
            instruction = methodInstruction(op, args, name);
        }
        else if (equals(name, "invokedynamic")) {
            std::vector<constant::ConstantPtr> bootstrapArgs;
            if (args[3]->getType() == ast::ElementKind::ARRAY) {
                for (const auto& value : std::static_pointer_cast<ast::Array>(args[3])->values())
                    bootstrapArgs.push_back(constantValue(value));
            }
            auto descriptor = type::Types::methodType(utf8(*args[1]).ref()).unwrap();
            instruction     = std::make_shared<code::InvokeDynamicInstruction>(
                utf8(*args[0]), descriptor, methodHandle(args[2]), std::move(bootstrapArgs));
        }
        else if (equals(name, "multianewarray")) {
            auto descriptor = utf8(*args[0]);
            auto array      = type::Types::arrayTypeFromDescriptor(descriptor.ref()).unwrap();
            instruction     = std::make_shared<code::AllocateMultiDimArrayInstruction>(
                std::move(array), static_cast<ca::i32>(integerValue(*args[1])));
        }
        else {
            instruction = std::make_shared<code::SimpleInstruction>(op);
        }
        builder.codeList().addInstruction(std::move(instruction));
    }

    static ca::i32 opcodeUpper(ca::str::Utf16StringRef name)
    {
        std::string text = name.to_utf8_string().to_std_string();
        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        auto upper = ca::str::Utf16String::value_of(text.c_str());
        return opcode(upper.ref());
    }

    static bool isVarOpcode(ca::i32 op) noexcept
    {
        return (op >= O::Opcodes::ILOAD && op <= O::Opcodes::ALOAD) ||
               (op >= O::Opcodes::ISTORE && op <= O::Opcodes::ASTORE) || op == O::Opcodes::RET;
    }

    static bool isJumpOpcode(ca::i32 op) noexcept
    {
        return (op >= O::Opcodes::IFEQ && op <= O::Opcodes::JSR) || op == O::Opcodes::IFNULL ||
               op == O::Opcodes::IFNONNULL || op == 200 || op == 201;
    }

    code::InstructionPtr fieldInstruction(ca::i32 op, const std::vector<ast::ElementPtr>& args)
    {
        const std::string path      = narrow(*args[0]);
        const auto        separator = path.rfind('.');
        if (separator == std::string::npos) throw std::invalid_argument("Field path has no name");
        auto owner =
            ca::str::Utf8String::from_data(reinterpret_cast<const ca::u8*>(path.data()), separator);
        auto name = ca::str::Utf8String::from_data(
            reinterpret_cast<const ca::u8*>(path.data() + separator + 1),
            path.size() - separator - 1);
        auto parsed = type::Types::typeFromDescriptor(utf8(*args[1]).ref()).unwrap();
        return std::make_shared<code::FieldInstruction>(
            op,
            type::Types::instanceTypeFromInternalName(owner.ref()),
            std::move(name),
            std::static_pointer_cast<const type::ClassType>(parsed));
    }

    code::InstructionPtr methodInstruction(ca::i32 op, const std::vector<ast::ElementPtr>& args,
                                           ca::str::Utf16StringRef mnemonic)
    {
        const std::string path      = narrow(*args[0]);
        const auto        separator = path.rfind('.');
        if (separator == std::string::npos) throw std::invalid_argument("Method path has no name");
        auto owner =
            ca::str::Utf8String::from_data(reinterpret_cast<const ca::u8*>(path.data()), separator);
        auto name = ca::str::Utf8String::from_data(
            reinterpret_cast<const ca::u8*>(path.data() + separator + 1),
            path.size() - separator - 1);
        const bool isInterface =
            equals(mnemonic, "invokeinterface") || equals(mnemonic, "invokevirtualinterface") ||
            equals(mnemonic, "invokestaticinterface") || equals(mnemonic, "invokespecialinterface");
        return std::make_shared<code::MethodInstruction>(
            op,
            type::Types::objectTypeFromInternalName(owner.ref()).unwrap(),
            std::move(name),
            type::Types::methodType(utf8(*args[1]).ref()).unwrap(),
            isInterface);
    }

    code::InstructionPtr tableSwitch(const ast::Object& object)
    {
        auto                        minKey     = ca::str::Utf16String::value_of("min");
        auto                        defaultKey = ca::str::Utf16String::value_of("default");
        auto                        casesKey   = ca::str::Utf16String::value_of("cases");
        std::vector<code::LabelPtr> targets;
        auto                        cases = object.value(casesKey.ref());
        if (cases->getType() == ast::ElementKind::ARRAY) {
            for (const auto& value : std::static_pointer_cast<ast::Array>(cases)->values())
                targets.push_back(label(*std::static_pointer_cast<ast::Identifier>(value)));
        }
        return std::make_shared<code::TableSwitchInstruction>(
            static_cast<ca::i32>(integerValue(*object.value(minKey.ref()))),
            label(*std::static_pointer_cast<ast::Identifier>(object.value(defaultKey.ref()))),
            std::move(targets));
    }

    code::InstructionPtr lookupSwitch(const ast::Object& object)
    {
        auto defaultKey = ca::str::Utf16String::value_of("default");
        std::vector<std::pair<ca::i32, code::LabelPtr>> pairs;
        for (const auto& entry : object.values()) {
            if (equals(entry.first->content(), "default")) continue;
            pairs.emplace_back(static_cast<ca::i32>(integerValue(*entry.first)),
                               label(*std::static_pointer_cast<ast::Identifier>(entry.second)));
        }
        std::sort(pairs.begin(), pairs.end(), [](const auto& left, const auto& right) {
            return left.first < right.first;
        });
        std::vector<ca::i32>        keys;
        std::vector<code::LabelPtr> targets;
        for (auto& pair : pairs) {
            keys.push_back(pair.first);
            targets.push_back(std::move(pair.second));
        }
        return std::make_shared<code::LookupSwitchInstruction>(
            std::move(keys),
            label(*std::static_pointer_cast<ast::Identifier>(object.value(defaultKey.ref()))),
            std::move(targets));
    }

    const ast::Method&                              method_;
    type::MethodTypePtr                             methodType_;
    ca::i32                                         maxStack_;
    std::unordered_map<std::string, code::LabelPtr> labels_;
    std::unordered_map<std::string, ca::i32>        variables_;
    ca::i32                                         nextVariable_ = 0;
};

std::optional<ca::str::Utf8String> signature(const ast::Attributes& attributes)
{
    if (!attributes.signature) return std::nullopt;
    return utf8(*attributes.signature);
}

classfile::ClassFileViewPtr compileClass(const ast::Class& clazz, const JvmCompilerOptions& options)
{
    classfile::generic::GenericClassBuilder builder;
    builder.version(options.version)
        .accessFlags(access(clazz.modifiers()))
        .type(instanceType(*clazz.name()))
        .signature(signature(clazz.attributes()));
    if (clazz.superName()) builder.superClass(instanceType(*clazz.superName()));
    for (const auto& value : clazz.interfaces()) builder.addInterface(instanceType(*value));
    for (const auto& value : clazz.permittedSubclasses())
        builder.permittedSubclass(instanceType(*value));
    if (clazz.sourceFile()) builder.sourceFile(utf8(*clazz.sourceFile()));
    if (clazz.outerMethod() && clazz.outerClass()) {
        builder.outerMethod(utf8(*clazz.outerClass()),
                            utf8(*clazz.outerMethod()->name()),
                            utf8(*clazz.outerMethod()->descriptor()));
    }
    else if (clazz.outerClass()) {
        builder.outerClass(utf8(*clazz.outerClass()));
    }
    if (clazz.nestHost()) builder.nestHost(instanceType(*clazz.nestHost()));
    for (const auto& value : clazz.nestMembers()) builder.nestMember(instanceType(*value));
    for (const auto& value : clazz.innerClasses()) {
        std::optional<ca::str::Utf8String> name;
        if (value->name()) name = utf8(*value->name());
        builder.innerClass(std::make_shared<const classfile::attribute::generic::GenericInnerClass>(
            access(value->modifiers()),
            instanceType(*value->innerClass()),
            value->outerClass() ? instanceType(*value->outerClass()) : nullptr,
            std::move(name)));
    }
    for (const auto& value : clazz.recordComponents()) {
        auto parsed = type::Types::typeFromDescriptor(utf8(*value->descriptor()).ref()).unwrap();
        auto record =
            builder.putRecordComponent(utf8(*value->name()),
                                       std::static_pointer_cast<const type::ClassType>(parsed),
                                       signature(value->attributes()));
        installAnnotations(record->annotations(), value->attributes());
    }
    installAnnotations(builder.annotations(), clazz.attributes());

    for (const auto& item : clazz.contents()) {
        if (item->getType() == ast::ElementKind::FIELD) {
            auto field = std::static_pointer_cast<ast::Field>(item);
            auto parsed =
                type::Types::typeFromDescriptor(utf8(*field->descriptor()).ref()).unwrap();
            auto target = builder.putField(access(field->modifiers()),
                                           utf8(*field->name()),
                                           std::static_pointer_cast<const type::ClassType>(parsed));
            target->signature(signature(field->attributes()));
            if (field->fieldValue()) target->defaultValue(constantValue(field->fieldValue()));
            installAnnotations(target->annotations(), field->attributes());
        }
        else if (item->getType() == ast::ElementKind::METHOD) {
            auto method     = std::static_pointer_cast<ast::Method>(item);
            auto methodType = type::Types::methodType(utf8(*method->descriptor()).ref()).unwrap();
            auto target =
                builder.putMethod(access(method->modifiers()), utf8(*method->name()), methodType);
            target->signature(signature(method->attributes()));
            installAnnotations(target->annotations(), method->attributes());
            for (const auto& parameter : method->parameters())
                target->parameter(
                    std::make_shared<const classfile::attribute::generic::GenericParameter>(
                        0, utf8(*parameter)));
            if (method->defaultValue())
                target->annotationDefault(annotationElement(method->defaultValue()));
            if (method->code())
                target->code(CodeCompiler(*method, methodType, options.maxStackHint).compile());
        }
    }
    return builder.build();
}
}   //namespace

error::Result<JvmCompileResult> JvmCompiler::compile(
    const std::vector<ast::ElementPtr>& input) const
{
    const ast::Class* clazz = nullptr;
    for (const auto& item : input) {
        if (item && item->getType() == ast::ElementKind::CLASS) {
            if (clazz) {
                auto message =
                    ca::str::Utf16String::value_of("Expected exactly one class declaration");
                return error::Result<JvmCompileResult>::err({error::Error(message.ref())});
            }
            clazz = static_cast<const ast::Class*>(item.get());
        }
    }
    if (!clazz) {
        auto message = ca::str::Utf16String::value_of("Expected a class declaration");
        return error::Result<JvmCompileResult>::err({error::Error(message.ref())});
    }
    return compile(*clazz);
}

error::Result<JvmCompileResult> JvmCompiler::compile(const ast::Class& clazz) const
{
    try {
        auto                classFile = compileClass(clazz, options_);
        std::vector<ca::u8> bytes;
        if (options_.inheritance) {
            CompilerCommonSuperClassProvider provider(*options_.inheritance);
            bytes = adapter::BytecodeLibrary::write(*classFile, provider);
        }
        else {
            bytes = adapter::BytecodeLibrary::write(*classFile);
        }
        return error::Result<JvmCompileResult>::ok({std::move(classFile), std::move(bytes)});
    }
    catch (const std::exception& exception) {
        auto                      message  = diagnosticText(exception.what());
        const util::Location*     location = clazz.location();
        std::vector<error::Error> errors;
        if (location)
            errors.emplace_back(message.ref(), *location);
        else
            errors.emplace_back(message.ref());
        return error::Result<JvmCompileResult>::err(std::move(errors));
    }
}

}   //namespace jhasm::compile
