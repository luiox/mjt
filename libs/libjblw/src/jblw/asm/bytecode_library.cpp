#include "bytecode_library.hpp"

#include "conversion.hpp"
#include "jbytecode/core/frame.hpp"

#include "jblw/annotation/builders.hpp"
#include "jblw/annotation/element_traits.hpp"
#include "jblw/classfile/generic/builders.hpp"
#include "jblw/code/attribute/local.hpp"
#include "jblw/constant/of_double.hpp"
#include "jblw/constant/of_float.hpp"
#include "jblw/constant/of_int.hpp"
#include "jblw/constant/of_long.hpp"
#include "jblw/constantpool/constant_pool.hpp"
#include "jblw/type/types.hpp"
#include "jblw/type/type_traits.hpp"

#include <jbytecode/core/readers.hpp>
#include <jbytecode/core/writers.hpp>
#include <jbytecode/tree/nodes.hpp>

#include <libca/str/utf8_string_arena.hpp>

#include <memory>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include "libca/core/tag_cast.hpp"

namespace jblw::adapter {
namespace {
using namespace annotation;
using namespace classfile;
using namespace classfile::generic;
using namespace code;
using namespace type;
namespace tree = jbytecode::tree;
namespace core = jbytecode::core;

ca::str::Utf8String copy(ca::str::Utf8StringRef value)
{
    return ca::str::Utf8String::from_data(value.data(), value.byte_length());
}

std::optional<ca::str::Utf8String> optionalString(ca::str::Utf8StringRef value)
{
    if (value.byte_length() == 0) return std::nullopt;
    return copy(value);
}

InstanceTypePtr instanceFromDescriptor(ca::str::Utf8StringRef descriptor)
{
    return Types::instanceTypeFromDescriptor(descriptor).unwrap();
}

ElementPtr elementFromNode(const tree::AnnotationNode::Element& value);

AnnotationPtr annotationFromNode(const tree::AnnotationNode& node)
{
    std::vector<AnnotationEntry> entries;
    entries.reserve(node.elements.size());
    for (const auto& element : node.elements) {
        entries.emplace_back(copy(element.name), elementFromNode(element));
    }
    return std::make_shared<const MapAnnotation>(instanceFromDescriptor(node.desc),
                                                 std::move(entries));
}

ElementPtr elementFromNode(const tree::AnnotationNode::Element& value)
{
    switch (value.tag) {
    case 'Z': return booleanElement(value.ival != 0);
    case 'B': return std::make_shared<const ElementByte>(static_cast<ca::i8>(value.ival));
    case 'C': return std::make_shared<const ElementChar>(static_cast<ca::u16>(value.ival));
    case 'S': return std::make_shared<const ElementShort>(static_cast<ca::i16>(value.ival));
    case 'I': return std::make_shared<const ElementInt>(static_cast<ca::i32>(value.ival));
    case 'J': return std::make_shared<const ElementLong>(value.lval);
    case 'F': return std::make_shared<const ElementFloat>(value.fval);
    case 'D': return std::make_shared<const ElementDouble>(value.dval);
    case 's': return std::make_shared<const ElementString>(copy(value.sval.ref()));
    case 'c':
    {
        TypePtr type = Types::typeFromDescriptor(value.sval.ref()).unwrap();
        if (type->getType() != TypeKind::INSTANCE && type->getType() != TypeKind::ARRAY) {
            throw std::invalid_argument("BLW annotation type element requires an object type");
        }
        return std::make_shared<const ElementType>(
            std::static_pointer_cast<const ObjectType>(std::move(type)));
    }
    case 'e':
    {
        const std::string encoded   = value.sval.to_std_string();
        const ca::usize   separator = encoded.find(' ');
        if (separator == std::string::npos) {
            throw std::invalid_argument("Malformed enum annotation element");
        }
        const ca::str::Utf8String descriptor(reinterpret_cast<const ca::u8*>(encoded.data()),
                                             separator);
        const ca::str::Utf8String name(
            reinterpret_cast<const ca::u8*>(encoded.data() + separator + 1),
            encoded.size() - separator - 1);
        return std::make_shared<const ElementEnum>(instanceFromDescriptor(descriptor.ref()),
                                                   copy(name.ref()));
    }
    case '@': return annotationFromNode(*value.nested);
    case '[':
    {
        std::vector<ElementPtr> elements;
        if (value.nested) {
            elements.reserve(value.nested->elements.size());
            for (const auto& element : value.nested->elements) {
                elements.push_back(elementFromNode(element));
            }
        }
        return std::make_shared<const ElementArray>(std::move(elements));
    }
    default: throw std::invalid_argument("Unsupported annotation element tag");
    }
}

void populateAnnotation(AnnotationBuilder& builder, const tree::AnnotationNode& node)
{
    for (const auto& element : node.elements) {
        builder.element(copy(element.name), elementFromNode(element));
    }
}

std::optional<TypePath> typePathFromNode(const core::TypePath* value)
{
    if (!value) return std::nullopt;
    const std::string         text = value->toString();
    const ca::str::Utf8String owned(reinterpret_cast<const ca::u8*>(text.data()), text.size());
    return TypePath::fromString(owned.ref());
}

void readAnnotations(AnnotatedBuilderState&                    state,
                     const std::vector<tree::AnnotationNode*>& visible,
                     const std::vector<tree::AnnotationNode*>& invisible,
                     const std::vector<tree::AnnotationNode*>& visibleType   = {},
                     const std::vector<tree::AnnotationNode*>& invisibleType = {})
{
    for (const auto* node : visible) {
        auto builder = state.addVisibleRuntimeAnnotation(instanceFromDescriptor(node->desc));
        populateAnnotation(*builder, *node);
    }
    for (const auto* node : invisible) {
        auto builder = state.addInvisibleRuntimeAnnotation(instanceFromDescriptor(node->desc));
        populateAnnotation(*builder, *node);
    }
    for (const auto* annotation : visibleType) {
        const auto* node    = static_cast<const tree::TypeAnnotationNode*>(annotation);
        auto        builder = state.addVisibleRuntimeTypeAnnotation(
            instanceFromDescriptor(node->desc), node->typeRef, typePathFromNode(node->typePath));
        populateAnnotation(*builder, *node);
    }
    for (const auto* annotation : invisibleType) {
        const auto* node    = static_cast<const tree::TypeAnnotationNode*>(annotation);
        auto        builder = state.addInvisibleRuntimeTypeAnnotation(
            instanceFromDescriptor(node->desc), node->typeRef, typePathFromNode(node->typePath));
        populateAnnotation(*builder, *node);
    }
}

InstructionPtr simpleInstruction(ca::i32 opcode)
{
    using O = core::Opcodes;
    switch (opcode) {
    case O::ICONST_M1:
    case O::ICONST_0:
    case O::ICONST_1:
    case O::ICONST_2:
    case O::ICONST_3:
    case O::ICONST_4:
    case O::ICONST_5:
        return std::make_shared<IntConstantInstruction>(
            std::make_shared<const constant::OfInt>(opcode - O::ICONST_0));
    case O::LCONST_0:
    case O::LCONST_1:
        return std::make_shared<LongConstantInstruction>(
            std::make_shared<const constant::OfLong>(opcode - O::LCONST_0));
    case O::FCONST_0:
    case O::FCONST_1:
    case O::FCONST_2:
        return std::make_shared<FloatConstantInstruction>(
            std::make_shared<const constant::OfFloat>(static_cast<ca::f32>(opcode - O::FCONST_0)));
    case O::DCONST_0:
    case O::DCONST_1:
        return std::make_shared<DoubleConstantInstruction>(
            std::make_shared<const constant::OfDouble>(static_cast<ca::f64>(opcode - O::DCONST_0)));
    case O::I2L: return std::make_shared<PrimitiveConversionInstruction>(Types::INT, Types::LONG);
    case O::I2F: return std::make_shared<PrimitiveConversionInstruction>(Types::INT, Types::FLOAT);
    case O::I2D: return std::make_shared<PrimitiveConversionInstruction>(Types::INT, Types::DOUBLE);
    case O::L2I: return std::make_shared<PrimitiveConversionInstruction>(Types::LONG, Types::INT);
    case O::L2F: return std::make_shared<PrimitiveConversionInstruction>(Types::LONG, Types::FLOAT);
    case O::L2D:
        return std::make_shared<PrimitiveConversionInstruction>(Types::LONG, Types::DOUBLE);
    case O::F2I: return std::make_shared<PrimitiveConversionInstruction>(Types::FLOAT, Types::INT);
    case O::F2L: return std::make_shared<PrimitiveConversionInstruction>(Types::FLOAT, Types::LONG);
    case O::F2D:
        return std::make_shared<PrimitiveConversionInstruction>(Types::FLOAT, Types::DOUBLE);
    case O::D2I: return std::make_shared<PrimitiveConversionInstruction>(Types::DOUBLE, Types::INT);
    case O::D2L:
        return std::make_shared<PrimitiveConversionInstruction>(Types::DOUBLE, Types::LONG);
    case O::D2F:
        return std::make_shared<PrimitiveConversionInstruction>(Types::DOUBLE, Types::FLOAT);
    case O::I2B: return std::make_shared<PrimitiveConversionInstruction>(Types::INT, Types::BYTE);
    case O::I2C: return std::make_shared<PrimitiveConversionInstruction>(Types::INT, Types::CHAR);
    case O::I2S: return std::make_shared<PrimitiveConversionInstruction>(Types::INT, Types::SHORT);
    default: return std::make_shared<SimpleInstruction>(opcode);
    }
}

class MethodReader final
{
public:
    explicit MethodReader(const tree::MethodNode& node)
        : node_(node)
    {}

    void read(GenericMethodBuilder& builder)
    {
        builder.signature(optionalString(node_.signature));
        for (const auto& exception : node_.exceptions) {
            builder.exceptionType(Types::instanceTypeFromInternalName(exception));
        }
        for (const auto* parameter : node_.parameters) {
            builder.parameter(
                std::make_shared<const classfile::attribute::generic::GenericParameter>(
                    parameter->access, copy(parameter->name)));
        }
        readAnnotations(builder.annotations(),
                        node_.visibleAnnotations,
                        node_.invisibleAnnotations,
                        node_.visibleTypeAnnotations,
                        node_.invisibleTypeAnnotations);
        readParameterAnnotations(builder, true);
        readParameterAnnotations(builder, false);
        if (node_.annotationDefault && !node_.annotationDefault->elements.empty()) {
            builder.annotationDefault(elementFromNode(node_.annotationDefault->elements.front()));
        }
        if (!node_.instructions.isEmpty()) builder.code(readCode());
    }

private:
    LabelPtr label(core::Label* value)
    {
        auto it = labels_.find(value);
        if (it != labels_.end()) return it->second;
        auto result = std::make_shared<GenericLabel>();
        labels_.emplace(value, result);
        return result;
    }

    InstructionPtr instruction(const tree::AbstractInsnNode& node)
    {
        using N = tree::AbstractInsnNode;
        using O = core::Opcodes;
        switch (node.getType()) {
        case N::INSN: return simpleInstruction(node.opcode);
        case N::INT_INSN:
        {
            const auto& value = static_cast<const tree::IntInsnNode&>(node);
            if (value.opcode == O::NEWARRAY) {
                return std::make_shared<AllocateInstruction>(
                    Types::arrayType(Types::primitiveOfKind(value.operand).unwrap()));
            }
            return std::make_shared<IntConstantInstruction>(
                std::make_shared<const constant::OfInt>(value.operand));
        }
        case N::VAR_INSN:
        {
            const auto& value = static_cast<const tree::VarInsnNode&>(node);
            return std::make_shared<VarInstruction>(value.opcode, value.var);
        }
        case N::TYPE_INSN:
        {
            const auto&   value  = static_cast<const tree::TypeInsnNode&>(node);
            ObjectTypePtr object = Types::objectTypeFromInternalName(value.desc).unwrap();
            switch (value.opcode) {
            case O::NEW: return std::make_shared<AllocateInstruction>(object);
            case O::ANEWARRAY:
                return std::make_shared<AllocateInstruction>(Types::arrayType(object));
            case O::CHECKCAST: return std::make_shared<CheckCastInstruction>(object);
            case O::INSTANCEOF: return std::make_shared<InstanceofInstruction>(object);
            default: throw std::invalid_argument("Unsupported type instruction");
            }
        }
        case N::FIELD_INSN:
        {
            const auto& value = static_cast<const tree::FieldInsnNode&>(node);
            return std::make_shared<FieldInstruction>(
                value.opcode,
                Types::instanceTypeFromInternalName(value.owner),
                copy(value.name),
                std::static_pointer_cast<const ClassType>(
                    Types::typeFromDescriptor(value.desc).unwrap()));
        }
        case N::METHOD_INSN:
        {
            const auto& value = static_cast<const tree::MethodInsnNode&>(node);
            return std::make_shared<MethodInstruction>(
                value.opcode,
                Types::objectTypeFromInternalName(value.owner).unwrap(),
                copy(value.name),
                Types::methodType(value.desc).unwrap(),
                value.isInterface);
        }
        case N::INVOKE_DYNAMIC_INSN:
        {
            const auto& value = static_cast<const tree::InvokeDynamicInsnNode&>(node);
            std::vector<constant::ConstantPtr> arguments;
            arguments.reserve(value.bsmArgs.size());
            for (const auto& argument : value.bsmArgs) arguments.push_back(wrapConstant(argument));
            return std::make_shared<InvokeDynamicInstruction>(
                copy(value.name),
                Types::methodType(value.desc).unwrap(),
                wrapMethodHandle(value.bsm),
                std::move(arguments));
        }
        case N::JUMP_INSN:
        {
            const auto& value = static_cast<const tree::JumpInsnNode&>(node);
            if (value.opcode == O::GOTO) {
                return std::make_shared<ImmediateJumpInstruction>(value.opcode, label(value.label));
            }
            return std::make_shared<ConditionalJumpInstruction>(value.opcode, label(value.label));
        }
        case N::LDC_INSN:
            return ConstantInstruction::wrap(
                wrapConstant(static_cast<const tree::LdcInsnNode&>(node).cst));
        case N::IINC_INSN:
        {
            const auto& value = static_cast<const tree::IincInsnNode&>(node);
            return std::make_shared<VariableIncrementInstruction>(value.var, value.incr);
        }
        case N::TABLESWITCH_INSN:
        {
            const auto&           value = static_cast<const tree::TableSwitchInsnNode&>(node);
            std::vector<LabelPtr> targets;
            targets.reserve(value.labels.size());
            for (auto* target : value.labels) targets.push_back(label(target));
            return std::make_shared<TableSwitchInstruction>(
                value.min, label(value.dflt), std::move(targets));
        }
        case N::LOOKUPSWITCH_INSN:
        {
            const auto&           value = static_cast<const tree::LookupSwitchInsnNode&>(node);
            std::vector<ca::i32>  keys(value.keys.begin(), value.keys.end());
            std::vector<LabelPtr> targets;
            targets.reserve(value.labels.size());
            for (auto* target : value.labels) targets.push_back(label(target));
            return std::make_shared<LookupSwitchInstruction>(
                std::move(keys), label(value.dflt), std::move(targets));
        }
        case N::MULTIANEWARRAY_INSN:
        {
            const auto& value = static_cast<const tree::MultiANewArrayInsnNode&>(node);
            return std::make_shared<AllocateMultiDimArrayInstruction>(
                Types::arrayTypeFromDescriptor(value.desc).unwrap(), value.dims);
        }
        default: throw std::invalid_argument("Unsupported instruction node");
        }
    }

    CodePtr readCode()
    {
        GenericCodeBuilder builder;
        builder.maxStack(node_.maxStack).maxLocals(node_.maxLocals);
        for (const auto* node : node_.instructions) {
            switch (node->getType()) {
            case tree::AbstractInsnNode::LABEL:
                builder.codeList().addLabel(
                    label(static_cast<const tree::LabelNode*>(node)->label));
                break;
            case tree::AbstractInsnNode::LINE:
            {
                const auto* line = static_cast<const tree::LineNumberNode*>(node);
                label(line->start)->setLineNumber(line->line);
                break;
            }
            case tree::AbstractInsnNode::FRAME: break;
            default: builder.codeList().addInstruction(instruction(*node));
            }
        }
        for (const auto* block : node_.tryCatchBlocks) {
            builder.tryCatchBlock({label(block->start),
                                   label(block->end),
                                   label(block->handler),
                                   block->type.byte_length() == 0
                                       ? nullptr
                                       : Types::instanceTypeFromInternalName(block->type)});
        }
        for (const auto* local : node_.localVariables) {
            builder.localVariable(std::make_shared<const code::attribute::generic::GenericLocal>(
                label(local->start),
                label(local->end),
                local->index,
                copy(local->name),
                std::static_pointer_cast<const ClassType>(
                    Types::typeFromDescriptor(local->descriptor).unwrap()),
                optionalString(local->signature)));
        }
        return builder.build();
    }

    void readParameterAnnotations(GenericMethodBuilder& builder, bool visible)
    {
        const auto& annotations =
            visible ? node_.visibleParameterAnnotations : node_.invisibleParameterAnnotations;
        const auto& indices =
            visible ? node_.visibleParameterIndices : node_.invisibleParameterIndices;
        for (ca::usize i = 0; i < annotations.size(); ++i) {
            const auto*   node  = annotations[i];
            const ca::i32 index = i < indices.size() ? indices[i] : static_cast<ca::i32>(i);
            std::shared_ptr<GenericAnnotationBuilder> annotation =
                visible ? builder.addVisibleRuntimeParameterAnnotation(
                              index, instanceFromDescriptor(node->desc))
                        : builder.addInvisibleRuntimeParameterAnnotation(
                              index, instanceFromDescriptor(node->desc));
            populateAnnotation(*annotation, *node);
        }
    }

    const tree::MethodNode&                    node_;
    std::unordered_map<core::Label*, LabelPtr> labels_;
};

constantpool::ConstantPoolPtr constantPoolFromReader(core::ClassReader& reader)
{
    std::vector<constantpool::EntryPtr> entries;
    entries.reserve(static_cast<ca::usize>(reader.getItemCount() - 1));
    std::vector<char> buffer(static_cast<ca::usize>(reader.getMaxStringLength()));
    for (ca::i32 index = 1; index < reader.getItemCount(); ++index) {
        const ca::i32 offset = reader.cpInfoOffsets[static_cast<ca::usize>(index)];
        if (offset == 0) {
            entries.push_back(nullptr);
            continue;
        }
        const ca::i32 tag = reader.readByte(offset - 1);
        using namespace constantpool;
        switch (tag) {
        case Tag::Utf8:
            entries.push_back(std::make_shared<const EntryUtf8>(
                constant::OfString(copy(reader.readUtf(index, buffer)))));
            break;
        case Tag::Integer:
            entries.push_back(
                std::make_shared<const EntryInteger>(constant::OfInt(reader.readInt(offset))));
            break;
        case Tag::Float:
        {
            const ca::i32 bits = reader.readInt(offset);
            ca::f32       value;
            std::memcpy(&value, &bits, sizeof(value));
            entries.push_back(std::make_shared<const EntryFloat>(constant::OfFloat(value)));
            break;
        }
        case Tag::Long:
            entries.push_back(
                std::make_shared<const EntryLong>(constant::OfLong(reader.readLong(offset))));
            break;
        case Tag::Double:
        {
            const ca::i64 bits = reader.readLong(offset);
            ca::f64       value;
            std::memcpy(&value, &bits, sizeof(value));
            entries.push_back(std::make_shared<const EntryDouble>(constant::OfDouble(value)));
            break;
        }
        case Tag::Class:
            entries.push_back(std::make_shared<const EntryClass>(reader.readUnsignedShort(offset)));
            break;
        case Tag::String:
            entries.push_back(
                std::make_shared<const EntryString>(reader.readUnsignedShort(offset)));
            break;
        case Tag::Fieldref:
            entries.push_back(std::make_shared<const EntryFieldRef>(
                reader.readUnsignedShort(offset), reader.readUnsignedShort(offset + 2)));
            break;
        case Tag::Methodref:
            entries.push_back(std::make_shared<const EntryMethodRef>(
                reader.readUnsignedShort(offset), reader.readUnsignedShort(offset + 2)));
            break;
        case Tag::InterfaceMethodref:
            entries.push_back(std::make_shared<const EntryInterfaceMethodRef>(
                reader.readUnsignedShort(offset), reader.readUnsignedShort(offset + 2)));
            break;
        case Tag::NameAndType:
            entries.push_back(std::make_shared<const EntryNameAndType>(
                reader.readUnsignedShort(offset), reader.readUnsignedShort(offset + 2)));
            break;
        case Tag::MethodHandle:
            entries.push_back(std::make_shared<const EntryMethodHandle>(
                reader.readByte(offset), reader.readUnsignedShort(offset + 1)));
            break;
        case Tag::MethodType:
            entries.push_back(
                std::make_shared<const EntryMethodType>(reader.readUnsignedShort(offset)));
            break;
        case Tag::Dynamic:
        {
            const ca::core::Any value   = reader.readConst(index, buffer);
            const auto*         dynamic = value.as<core::ConstantDynamic>();
            if (!dynamic) {
                throw std::invalid_argument("Invalid CONSTANT_Dynamic payload");
            }
            entries.push_back(std::make_shared<const EntryDynamic>(
                constant::OfDynamic(wrapConstantDynamic(*dynamic))));
            break;
        }
        case Tag::InvokeDynamic:
        {
            const ca::i32 nameAndTypeOffset =
                reader.cpInfoOffsets[static_cast<ca::usize>(reader.readUnsignedShort(offset + 2))];
            const ca::i32 bootstrapIndex = reader.readUnsignedShort(offset);
            const ca::i32 bootstrapOffset =
                reader.bootstrapMethodOffsets.at(static_cast<ca::usize>(bootstrapIndex));
            const ca::core::Any handleValue =
                reader.readConst(reader.readUnsignedShort(bootstrapOffset), buffer);
            const auto* handle = handleValue.as<core::Handle>();
            if (!handle) {
                throw std::invalid_argument("Invalid CONSTANT_InvokeDynamic bootstrap handle");
            }
            std::vector<constant::ConstantPtr> arguments;
            const ca::i32 count = reader.readUnsignedShort(bootstrapOffset + 2);
            arguments.reserve(static_cast<ca::usize>(count));
            for (ca::i32 i = 0; i < count; ++i) {
                arguments.push_back(wrapConstant(reader.readConst(
                    reader.readUnsignedShort(bootstrapOffset + 4 + i * 2), buffer)));
            }
            auto value = std::make_shared<const InvokeDynamic>(
                copy(reader.readUTF8(nameAndTypeOffset, buffer)),
                Types::methodType(reader.readUTF8(nameAndTypeOffset + 2, buffer)).unwrap(),
                wrapMethodHandle(*handle),
                std::move(arguments));
            entries.push_back(std::make_shared<const EntryInvokeDynamic>(std::move(value)));
            break;
        }
        case Tag::Module:
            entries.push_back(
                std::make_shared<const EntryModule>(reader.readUnsignedShort(offset)));
            break;
        case Tag::Package:
            entries.push_back(
                std::make_shared<const EntryPackage>(reader.readUnsignedShort(offset)));
            break;
        default: throw std::invalid_argument("Unsupported constant-pool tag");
        }
    }
    return std::make_shared<const constantpool::ListConstantPool>(std::move(entries));
}

ClassFileViewPtr classFromNode(const tree::ClassNode&        node,
                               constantpool::ConstantPoolPtr constantPool)
{
    GenericClassBuilder builder;
    const ca::i32       major = node.version & 0xFFFF;
    const ca::i32       minor = static_cast<ca::i32>(static_cast<ca::u32>(node.version) >> 16U);
    builder.version(version::JavaVersion::classVersion(major, minor))
        .accessFlags(node.access)
        .type(Types::instanceTypeFromInternalName(node.name))
        .signature(optionalString(node.signature))
        .constantPool(std::move(constantPool));
    if (node.superName.byte_length() != 0) {
        builder.superClass(Types::instanceTypeFromInternalName(node.superName));
    }
    for (const auto& interface : node.interfaces) {
        builder.addInterface(Types::instanceTypeFromInternalName(interface));
    }
    builder.sourceFile(optionalString(node.sourceFile))
        .sourceDebug(optionalString(node.sourceDebug));
    if (node.outerClass.byte_length() != 0) {
        if (node.outerMethod.byte_length() != 0) {
            builder.outerMethod(
                copy(node.outerClass), copy(node.outerMethod), copy(node.outerMethodDesc));
        }
        else {
            builder.outerClass(copy(node.outerClass));
        }
    }
    if (node.nestHost.byte_length() != 0) {
        builder.nestHost(Types::instanceTypeFromInternalName(node.nestHost));
    }
    for (const auto& member : node.nestMembers) {
        builder.nestMember(Types::instanceTypeFromInternalName(member));
    }
    for (const auto& permitted : node.permittedSubclasses) {
        builder.permittedSubclass(Types::instanceTypeFromInternalName(permitted));
    }
    readAnnotations(builder.annotations(),
                    node.visibleAnnotations,
                    node.invisibleAnnotations,
                    node.visibleTypeAnnotations,
                    node.invisibleTypeAnnotations);

    for (const auto* field : node.fields) {
        auto fieldBuilder = builder.putField(field->access,
                                             copy(field->name),
                                             std::static_pointer_cast<const ClassType>(
                                                 Types::typeFromDescriptor(field->desc).unwrap()));
        fieldBuilder->signature(optionalString(field->signature));
        readAnnotations(
            fieldBuilder->annotations(), field->visibleAnnotations, field->invisibleAnnotations);
        if (field->value.has_value()) fieldBuilder->defaultValue(wrapConstant(field->value));
    }
    for (const auto* method : node.methods) {
        auto methodBuilder = builder.putMethod(
            method->access, copy(method->name), Types::methodType(method->desc).unwrap());
        MethodReader(*method).read(*methodBuilder);
    }
    for (const auto* component : node.recordComponents) {
        auto recordBuilder = builder.putRecordComponent(
            copy(component->name),
            std::static_pointer_cast<const ClassType>(
                Types::typeFromDescriptor(component->descriptor).unwrap()),
            optionalString(component->signature));
        readAnnotations(recordBuilder->annotations(),
                        component->visibleAnnotations,
                        component->invisibleAnnotations,
                        component->visibleTypeAnnotations,
                        component->invisibleTypeAnnotations);
    }
    for (const auto* inner : node.innerClasses) {
        builder.innerClass(std::make_shared<const classfile::attribute::generic::GenericInnerClass>(
            inner->access,
            Types::instanceTypeFromInternalName(inner->name),
            inner->outerName.byte_length() == 0
                ? nullptr
                : Types::instanceTypeFromInternalName(inner->outerName),
            optionalString(inner->innerName)));
    }
    if (node.moduleNode) {
        const auto& module = *node.moduleNode;
        auto        moduleBuilder =
            builder.addModule(copy(module.name), module.access, optionalString(module.version));
        moduleBuilder->mainClass(optionalString(module.mainClass));
        for (const auto& package : module.packages) moduleBuilder->package(copy(package));
        for (const auto& require : module.requiresList) {
            moduleBuilder->require(std::make_shared<const GenericModuleRequire>(
                require.access, copy(require.module), optionalString(require.version)));
        }
        for (const auto& exportValue : module.exportsList) {
            std::vector<ca::str::Utf8String> modules;
            for (const auto& name : exportValue.modules) modules.push_back(copy(name));
            moduleBuilder->exportPackage(std::make_shared<const GenericModuleExport>(
                exportValue.access, std::move(modules), copy(exportValue.packaze)));
        }
        for (const auto& open : module.opensList) {
            std::vector<ca::str::Utf8String> modules;
            for (const auto& name : open.modules) modules.push_back(copy(name));
            moduleBuilder->open(std::make_shared<const GenericModuleOpen>(
                open.access, copy(open.packaze), std::move(modules)));
        }
        for (const auto& use : module.usesList) moduleBuilder->use(copy(use));
        for (const auto& provide : module.providesList) {
            std::vector<ca::str::Utf8String> providers;
            for (const auto& name : provide.providers) providers.push_back(copy(name));
            moduleBuilder->provide(std::make_shared<const GenericModuleProvide>(
                copy(provide.service), std::move(providers)));
        }
    }
    return builder.build();
}

ca::str::Utf8StringRef optionalRef(const std::optional<ca::str::Utf8String>& value)
{
    return value ? value->ref() : ca::str::Utf8StringRef{};
}

void writeElement(core::AnnotationVisitor& visitor, ca::str::Utf8StringRef name,
                  const Element& element);

void writeAnnotationBody(core::AnnotationVisitor& visitor, const Annotation& annotation)
{
    for (const auto& entry : annotation.entries()) {
        writeElement(visitor, entry.first.ref(), *entry.second);
    }
    visitor.visitEnd();
}

void writeElement(core::AnnotationVisitor& visitor, ca::str::Utf8StringRef name,
                  const Element& element)
{
    switch (element.getType()) {
    case ElementKind::BOOLEAN:
        visitor.visit(name, ca::core::tag_cast::cast<ElementBoolean>(&element)->value());
        break;
    case ElementKind::BYTE: visitor.visit(name, ca::core::tag_cast::cast<ElementByte>(&element)->value()); break;
    case ElementKind::CHAR: visitor.visit(name, ca::core::tag_cast::cast<ElementChar>(&element)->value()); break;
    case ElementKind::SHORT:
        visitor.visit(name, ca::core::tag_cast::cast<ElementShort>(&element)->value());
        break;
    case ElementKind::INT: visitor.visit(name, ca::core::tag_cast::cast<ElementInt>(&element)->value()); break;
    case ElementKind::LONG: visitor.visit(name, ca::core::tag_cast::cast<ElementLong>(&element)->value()); break;
    case ElementKind::FLOAT:
        visitor.visit(name, ca::core::tag_cast::cast<ElementFloat>(&element)->value());
        break;
    case ElementKind::DOUBLE:
        visitor.visit(name, ca::core::tag_cast::cast<ElementDouble>(&element)->value());
        break;
    case ElementKind::STRING:
        visitor.visit(name, ca::core::tag_cast::cast<ElementString>(&element)->value().ref());
        break;
    case ElementKind::TYPE:
        visitor.visit(
            name, core::Type::getType(ca::core::tag_cast::cast<ElementType>(&element)->value()->descriptor()));
        break;
    case ElementKind::ENUM:
    {
        const auto* value = ca::core::tag_cast::cast<ElementEnum>(&element);
        visitor.visitEnum(name, value->type()->descriptor(), value->name());
        break;
    }
    case ElementKind::ANNOTATION:
    {
        const auto*              value = ca::core::tag_cast::cast<Annotation>(&element);
        core::AnnotationVisitor* nested =
            visitor.visitAnnotation(name, value->type()->descriptor());
        if (nested) writeAnnotationBody(*nested, *value);
        break;
    }
    case ElementKind::ARRAY:
    {
        core::AnnotationVisitor* array = visitor.visitArray(name);
        if (array) {
            for (const auto& value : ca::core::tag_cast::cast<ElementArray>(&element)->elements()) {
                writeElement(*array, {}, *value);
            }
            array->visitEnd();
        }
        break;
    }
    }
}

core::TypePath coreTypePath(const TypeAnnotation& annotation)
{
    if (!annotation.typePath()) return core::TypePath{};
    const ca::str::Utf8String text = annotation.typePath()->toString();
    return core::TypePath::fromString(text.ref());
}

template<typename Visitor>
void writeAnnotations(Visitor& visitor, const Annotated& annotated)
{
    for (const auto& annotation : annotated.visibleRuntimeAnnotations()) {
        core::AnnotationVisitor* target =
            visitor.visitAnnotation(annotation->type()->descriptor(), true);
        if (target) writeAnnotationBody(*target, *annotation);
    }
    for (const auto& annotation : annotated.invisibleRuntimeAnnotations()) {
        core::AnnotationVisitor* target =
            visitor.visitAnnotation(annotation->type()->descriptor(), false);
        if (target) writeAnnotationBody(*target, *annotation);
    }
    for (const auto& annotation : annotated.visibleRuntimeTypeAnnotations()) {
        const core::TypePath     path = coreTypePath(*annotation);
        core::AnnotationVisitor* target =
            visitor.visitTypeAnnotation(annotation->typeRef(),
                                        annotation->typePath() ? &path : nullptr,
                                        annotation->type()->descriptor(),
                                        true);
        if (target) writeAnnotationBody(*target, *annotation);
    }
    for (const auto& annotation : annotated.invisibleRuntimeTypeAnnotations()) {
        const core::TypePath     path = coreTypePath(*annotation);
        core::AnnotationVisitor* target =
            visitor.visitTypeAnnotation(annotation->typeRef(),
                                        annotation->typePath() ? &path : nullptr,
                                        annotation->type()->descriptor(),
                                        false);
        if (target) writeAnnotationBody(*target, *annotation);
    }
}

ca::i32 conversionOpcode(const PrimitiveConversionInstruction& instruction)
{
    const char from = static_cast<char>(instruction.from()->descriptor().byte_at(0));
    const char to   = static_cast<char>(instruction.to()->descriptor().byte_at(0));
    using O         = core::Opcodes;
    if (from == 'I') {
        switch (to) {
        case 'J': return O::I2L;
        case 'F': return O::I2F;
        case 'D': return O::I2D;
        case 'B': return O::I2B;
        case 'C': return O::I2C;
        case 'S': return O::I2S;
        }
    }
    else if (from == 'J') {
        if (to == 'I') return O::L2I;
        if (to == 'F') return O::L2F;
        if (to == 'D') return O::L2D;
    }
    else if (from == 'F') {
        if (to == 'I') return O::F2I;
        if (to == 'J') return O::F2L;
        if (to == 'D') return O::F2D;
    }
    else if (from == 'D') {
        if (to == 'I') return O::D2I;
        if (to == 'J') return O::D2L;
        if (to == 'F') return O::D2F;
    }
    throw std::invalid_argument("Unsupported primitive conversion");
}

class LabelMap final
{
public:
    core::Label* get(const LabelPtr& label)
    {
        auto it = labels_.find(label.get());
        if (it != labels_.end()) return it->second.get();
        auto         result = std::make_unique<core::Label>();
        core::Label* raw    = result.get();
        labels_.emplace(label.get(), std::move(result));
        return raw;
    }

private:
    std::unordered_map<const Label*, std::unique_ptr<core::Label>> labels_;
};

void writeInstruction(core::MethodVisitor& visitor, const Instruction& instruction,
                      LabelMap& labels)
{
    switch (instruction.instructionKind()) {
    case InstructionKind::SIMPLE: visitor.visitInsn(instruction.opcode()); break;
    case InstructionKind::CONSTANT_STRING:
    case InstructionKind::CONSTANT_METHOD_HANDLE:
    case InstructionKind::CONSTANT_DYNAMIC:
    case InstructionKind::CONSTANT_LONG:
    case InstructionKind::CONSTANT_DOUBLE:
    case InstructionKind::CONSTANT_INT:
    case InstructionKind::CONSTANT_FLOAT:
    case InstructionKind::CONSTANT_TYPE:
        visitor.visitLdcInsn(
            unwrapConstant(*static_cast<const ConstantInstruction&>(instruction).constant()));
        break;
    case InstructionKind::ALLOCATE:
    {
        const auto& value = static_cast<const AllocateInstruction&>(instruction);
        if (value.type()->getType() == TypeKind::INSTANCE) {
            visitor.visitTypeInsn(core::Opcodes::NEW, value.type()->internalName());
        }
        else {
            const auto*         array     = ca::core::tag_cast::cast<ArrayType>(value.type().get());
            const ClassTypePtr& component = array->componentType();
            if (component->getType() == TypeKind::PRIMITIVE) {
                visitor.visitIntInsn(core::Opcodes::NEWARRAY,
                                     ca::core::tag_cast::cast<PrimitiveType>(component.get())->kind());
            }
            else {
                visitor.visitTypeInsn(core::Opcodes::ANEWARRAY,
                                      ca::core::tag_cast::cast<ObjectType>(component.get())->internalName());
            }
        }
        break;
    }
    case InstructionKind::ALLOCATE_MULTI_ARRAY:
    {
        const auto& value = static_cast<const AllocateMultiDimArrayInstruction&>(instruction);
        visitor.visitMultiANewArrayInsn(value.type()->descriptor(), value.dimensions());
        break;
    }
    case InstructionKind::CHECK_CAST:
        visitor.visitTypeInsn(
            core::Opcodes::CHECKCAST,
            static_cast<const CheckCastInstruction&>(instruction).type()->internalName());
        break;
    case InstructionKind::INSTANCE_OF:
        visitor.visitTypeInsn(
            core::Opcodes::INSTANCEOF,
            static_cast<const InstanceofInstruction&>(instruction).type()->internalName());
        break;
    case InstructionKind::CONDITIONAL_JUMP:
    {
        const auto& value = static_cast<const ConditionalJumpInstruction&>(instruction);
        visitor.visitJumpInsn(value.opcode(), labels.get(value.target()));
        break;
    }
    case InstructionKind::IMMEDIATE_JUMP:
    {
        const auto& value = static_cast<const ImmediateJumpInstruction&>(instruction);
        visitor.visitJumpInsn(value.opcode(), labels.get(value.target()));
        break;
    }
    case InstructionKind::LOOKUP_SWITCH:
    {
        const auto&               value = static_cast<const LookupSwitchInstruction&>(instruction);
        std::vector<core::Label*> targets;
        for (const auto& target : value.caseTargets()) targets.push_back(labels.get(target));
        visitor.visitLookupSwitchInsn(labels.get(value.defaultTarget()), value.keys(), targets);
        break;
    }
    case InstructionKind::TABLE_SWITCH:
    {
        const auto&               value = static_cast<const TableSwitchInstruction&>(instruction);
        std::vector<core::Label*> targets;
        for (const auto& target : value.caseTargets()) targets.push_back(labels.get(target));
        visitor.visitTableSwitchInsn(value.min(),
                                     value.min() + static_cast<ca::i32>(targets.size()) - 1,
                                     labels.get(value.defaultTarget()),
                                     targets);
        break;
    }
    case InstructionKind::FIELD:
    {
        const auto& value = static_cast<const FieldInstruction&>(instruction);
        visitor.visitFieldInsn(value.opcode(),
                               value.owner()->internalName(),
                               value.name(),
                               value.fieldType()->descriptor());
        break;
    }
    case InstructionKind::METHOD:
    {
        const auto& value = static_cast<const MethodInstruction&>(instruction);
        visitor.visitMethodInsn(value.opcode(),
                                value.owner()->internalName(),
                                value.name(),
                                value.methodType()->descriptor(),
                                value.isInterface());
        break;
    }
    case InstructionKind::INVOKE_DYNAMIC:
    {
        const auto& value = static_cast<const InvokeDynamicInstruction&>(instruction);
        std::vector<ca::core::Any> arguments;
        for (const auto& argument : value.args()) arguments.push_back(unwrapConstant(*argument));
        visitor.visitInvokeDynamicInsn(value.name(),
                                       value.type()->descriptor(),
                                       unwrapMethodHandle(*value.bootstrapHandle()),
                                       arguments);
        break;
    }
    case InstructionKind::PRIMITIVE_CONVERSION:
        visitor.visitInsn(
            conversionOpcode(static_cast<const PrimitiveConversionInstruction&>(instruction)));
        break;
    case InstructionKind::VARIABLE:
    {
        const auto& value = static_cast<const VarInstruction&>(instruction);
        visitor.visitVarInsn(value.opcode(), value.variableIndex());
        break;
    }
    case InstructionKind::VARIABLE_INCREMENT:
    {
        const auto& value = static_cast<const VariableIncrementInstruction&>(instruction);
        visitor.visitIincInsn(value.variableIndex(), value.incrementBy());
        break;
    }
    }
}

void writeCode(core::MethodVisitor& visitor, const Code& code, LabelMap& labels)
{
    visitor.visitCode();
    for (const auto& block : code.tryCatchBlocks()) {
        visitor.visitTryCatchBlock(
            labels.get(block.start),
            labels.get(block.end),
            labels.get(block.handler),
            block.type ? block.type->internalName() : ca::str::Utf8StringRef{});
    }
    for (const auto& element : code.elements().values()) {
        if (element->getType() == CodeElementKind::LABEL) {
            auto         label  = std::static_pointer_cast<Label>(element);
            core::Label* output = labels.get(label);
            visitor.visitLabel(output);
            if (label->getLineNumber() != Label::UNSET) {
                visitor.visitLineNumber(label->getLineNumber(), output);
            }
        }
        else {
            writeInstruction(visitor, static_cast<const Instruction&>(*element), labels);
        }
    }
    for (const auto& local : code.localVariables()) {
        visitor.visitLocalVariable(local->name(),
                                   local->type()->descriptor(),
                                   optionalRef(local->signature()),
                                   labels.get(local->start()),
                                   labels.get(local->end()),
                                   local->index());
    }
    visitor.visitMaxs(code.maxStack(), code.maxLocals());
}

void writeMethod(core::ClassWriter& writer, const Method& method, LabelMap& labels)
{
    std::vector<ca::str::Utf8StringRef> exceptions;
    for (const auto& exception : method.exceptionTypes()) {
        exceptions.push_back(exception->internalName());
    }
    core::MethodVisitor* visitor = writer.visitMethod(method.accessFlags(),
                                                      method.name(),
                                                      method.type()->descriptor(),
                                                      optionalRef(method.signature()),
                                                      exceptions);
    if (!visitor) return;
    for (const auto& parameter : method.parameters()) {
        visitor->visitParameter(parameter->name(), parameter->accessFlags());
    }
    writeAnnotations(*visitor, method);
    for (const auto& entry : method.visibleRuntimeParameterAnnotations()) {
        for (const auto& annotation : entry.second) {
            core::AnnotationVisitor* target = visitor->visitParameterAnnotation(
                entry.first, annotation->type()->descriptor(), true);
            if (target) writeAnnotationBody(*target, *annotation);
        }
    }
    for (const auto& entry : method.invisibleRuntimeParameterAnnotations()) {
        for (const auto& annotation : entry.second) {
            core::AnnotationVisitor* target = visitor->visitParameterAnnotation(
                entry.first, annotation->type()->descriptor(), false);
            if (target) writeAnnotationBody(*target, *annotation);
        }
    }
    if (method.annotationDefault()) {
        core::AnnotationVisitor* target = visitor->visitAnnotationDefault();
        if (target) {
            writeElement(*target, {}, *method.annotationDefault());
            target->visitEnd();
        }
    }
    if (method.code()) writeCode(*visitor, *method.code(), labels);
    visitor->visitEnd();
}

void writeModule(core::ClassWriter& writer, const Module& module)
{
    core::ModuleVisitor* visitor =
        writer.visitModule(module.name(), module.accessFlags(), optionalRef(module.version()));
    if (!visitor) return;
    if (module.mainClass()) visitor->visitMainClass(module.mainClass()->ref());
    for (const auto& package : module.packages()) visitor->visitPackage(package.ref());
    for (const auto& require : module.requires()) {
        visitor->visitRequire(
            require->module(), require->accessFlags(), optionalRef(require->version()));
    }
    for (const auto& exportValue : module.exports()) {
        std::vector<ca::str::Utf8StringRef> targets;
        for (const auto& target : exportValue->modules()) targets.push_back(target.ref());
        visitor->visitExport(exportValue->packageName(), exportValue->accessFlags(), targets);
    }
    for (const auto& open : module.opens()) {
        std::vector<ca::str::Utf8StringRef> targets;
        for (const auto& target : open->modules()) targets.push_back(target.ref());
        visitor->visitOpen(open->packageName(), open->accessFlags(), targets);
    }
    for (const auto& use : module.uses()) visitor->visitUse(use.ref());
    for (const auto& provide : module.provides()) {
        std::vector<ca::str::Utf8StringRef> providers;
        for (const auto& provider : provide->providers()) providers.push_back(provider.ref());
        visitor->visitProvide(provide->service(), providers);
    }
    visitor->visitEnd();
}

std::vector<ca::u8> writeClass(const ClassFileView&                        classFile,
                               jbytecode::core::ICommonSuperClassProvider* provider)
{
    ca::str::Utf8StringArena arena;
    core::ClassWriter        writer(arena, core::ClassWriter::COMPUTE_FRAMES);
    writer.setTypeResolver(provider);
    LabelMap                            labels;
    std::vector<ca::str::Utf8StringRef> interfaces;
    for (const auto& interface : classFile.interfaces()) {
        interfaces.push_back(interface->internalName());
    }
    writer.visit(
        classFile.version().pack(),
        classFile.accessFlags(),
        classFile.type()->internalName(),
        optionalRef(classFile.signature()),
        classFile.superClass() ? classFile.superClass()->internalName() : ca::str::Utf8StringRef{},
        interfaces);
    if (classFile.sourceFile() || classFile.sourceDebug()) {
        writer.visitSource(optionalRef(classFile.sourceFile()),
                           optionalRef(classFile.sourceDebug()));
    }
    if (classFile.outerClass()) {
        writer.visitOuterClass(classFile.outerClass()->ref(),
                               optionalRef(classFile.outerMethodName()),
                               optionalRef(classFile.outerMethodDescriptor()));
    }
    if (classFile.nestHost()) writer.visitNestHost(classFile.nestHost()->internalName());
    for (const auto& member : classFile.nestMembers())
        writer.visitNestMember(member->internalName());
    for (const auto& permitted : classFile.permittedSubclasses()) {
        writer.visitPermittedSubclass(permitted->internalName());
    }
    writeAnnotations(writer, classFile);
    for (const auto& inner : classFile.innerClasses()) {
        writer.visitInnerClass(
            inner->type()->internalName(),
            inner->outerType() ? inner->outerType()->internalName() : ca::str::Utf8StringRef{},
            optionalRef(inner->innerName()),
            inner->accessFlags());
    }
    for (const auto& field : classFile.fields()) {
        ca::core::Any defaultValue =
            field->defaultValue() ? unwrapConstant(*field->defaultValue()) : ca::core::Any{};
        core::FieldVisitor* visitor = writer.visitField(field->accessFlags(),
                                                        field->name(),
                                                        field->type()->descriptor(),
                                                        optionalRef(field->signature()),
                                                        defaultValue);
        if (visitor) {
            writeAnnotations(*visitor, *field);
            visitor->visitEnd();
        }
    }
    for (const auto& method : classFile.methods()) writeMethod(writer, *method, labels);
    for (const auto& component : classFile.recordComponents()) {
        core::RecordComponentVisitor* visitor =
            writer.visitRecordComponent(component->name(),
                                        component->type()->descriptor(),
                                        optionalRef(component->signature()));
        if (visitor) {
            writeAnnotations(*visitor, *component);
            visitor->visitEnd();
        }
    }
    for (const auto& module : classFile.modules()) writeModule(writer, *module);
    writer.visitEnd();
    return writer.toByteArray();
}

}   //namespace

ClassFileViewPtr BytecodeLibrary::read(const std::vector<ca::u8>& bytes)
{
    if (bytes.empty()) throw std::invalid_argument("Class bytes must not be empty");
    ca::str::Utf8StringArena arena;
    core::ClassReader        reader(arena, bytes);
    tree::ClassNode          node;
    reader.accept(&node, core::ClassReader::SKIP_FRAMES);
    return classFromNode(node, constantPoolFromReader(reader));
}

std::vector<ca::u8> BytecodeLibrary::write(const ClassFileView& classFile)
{
    return writeClass(classFile, nullptr);
}

std::vector<ca::u8> BytecodeLibrary::write(const ClassFileView&                        classFile,
                                           jbytecode::core::ICommonSuperClassProvider& provider)
{
    return writeClass(classFile, &provider);
}

}   //namespace jblw::adapter
