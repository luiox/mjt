#include "jvm_printer.hpp"

#include "jhasm/util/escape_util.hpp"

#include <jbytecode/core/opcodes.hpp>
#include <jbytecode/core/readers.hpp>
#include <jbytecode/tree/nodes.hpp>
#include <jbytecode/util/printer.hpp>
#include "libca/core/tag_cast.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace jhasm::printer {
namespace {
namespace O = jbytecode::core;
namespace T = jbytecode::tree;

ca::str::Utf16String utf16(const std::string& value)
{
    return ca::str::Utf16String::from_utf8_string(ca::str::Utf8StringRef::from_data(
        reinterpret_cast<const ca::u8*>(value.data()), value.size()));
}

std::string escaped(ca::str::Utf8StringRef value, bool string)
{
    auto converted = ca::str::Utf16String::from_utf8_string(value);
    auto result    = string ? util::EscapeUtil::escape_string(converted.ref())
                            : util::EscapeUtil::escape_literal(converted.ref());
    return result.to_utf8_string().to_std_string();
}

std::string literal(ca::str::Utf8StringRef value)
{
    return escaped(value, false);
}

std::string quoted(ca::str::Utf8StringRef value)
{
    return "\"" + escaped(value, true) + "\"";
}

std::string lowerOpcode(ca::i32 opcode)
{
    if (opcode < 0 || opcode >= 200 || !jbytecode::util::Printer::OPCODES[opcode])
        throw std::invalid_argument("Unsupported JVM opcode " + std::to_string(opcode));
    std::string result = jbytecode::util::Printer::OPCODES[opcode];
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char value) {
        return static_cast<char>(std::tolower(value));
    });
    return result;
}

std::string modifiers(ca::i32 access, bool clazz, bool method)
{
    std::string result;
    auto        add = [&](ca::i32 flag, const char* name) {
        if ((access & flag) != 0) result += std::string(name) + " ";
    };
    add(O::Opcodes::ACC_PUBLIC, "public");
    add(O::Opcodes::ACC_PRIVATE, "private");
    add(O::Opcodes::ACC_PROTECTED, "protected");
    add(O::Opcodes::ACC_STATIC, "static");
    add(O::Opcodes::ACC_FINAL, "final");
    if (clazz) add(O::Opcodes::ACC_SUPER, "super");
    if (method) add(O::Opcodes::ACC_SYNCHRONIZED, "synchronized");
    if (!clazz && !method) {
        add(O::Opcodes::ACC_VOLATILE, "volatile");
        add(O::Opcodes::ACC_TRANSIENT, "transient");
    }
    if (method) {
        add(O::Opcodes::ACC_BRIDGE, "bridge");
        add(O::Opcodes::ACC_VARARGS, "varargs");
        add(O::Opcodes::ACC_NATIVE, "native");
    }
    add(O::Opcodes::ACC_INTERFACE, "interface");
    add(O::Opcodes::ACC_ABSTRACT, "abstract");
    add(O::Opcodes::ACC_STRICT, "strictfp");
    add(O::Opcodes::ACC_SYNTHETIC, "synthetic");
    add(O::Opcodes::ACC_ANNOTATION, "annotation");
    add(O::Opcodes::ACC_ENUM, "enum");
    add(O::Opcodes::ACC_MODULE, "module");
    add(O::Opcodes::ACC_RECORD, "record");
    return result;
}

std::string floating(ca::f64 value, bool wide)
{
    if (std::isnan(value)) return wide ? "NaND" : "NaNF";
    if (std::isinf(value)) {
        std::string result = value < 0 ? "-Infinity" : "Infinity";
        return result + (wide ? "D" : "F");
    }
    std::ostringstream stream;
    stream << std::setprecision(wide ? 17 : 9) << value;
    std::string result   = stream.str();
    const auto  exponent = result.find_first_of("eE");
    if (exponent != std::string::npos && exponent + 1 < result.size() &&
        result[exponent + 1] == '+')
        result.erase(exponent + 1, 1);
    if (result.find('.') == std::string::npos && result.find_first_of("eE") == std::string::npos)
        result += ".0";
    return result + (wide ? "D" : "F");
}

std::string handle(const O::Handle& value)
{
    static const char* kinds[] = {nullptr,
                                  "getfield",
                                  "getstatic",
                                  "putfield",
                                  "putstatic",
                                  "invokevirtual",
                                  "invokestatic",
                                  "invokespecial",
                                  "newinvokespecial",
                                  "invokeinterface"};
    if (value.getTag() < 1 || value.getTag() > 9)
        throw std::invalid_argument("Unsupported method handle kind");
    return "{ " + std::string(kinds[value.getTag()]) + ", " + literal(value.getOwner()) + "." +
           literal(value.getName()) + ", " + literal(value.getDesc()) + " }";
}

std::string constant(const ca::core::Any& value);

std::string constants(const std::vector<ca::core::Any>& values)
{
    std::string result = "{ ";
    for (ca::usize index = 0; index < values.size(); ++index) {
        if (index) result += ", ";
        result += constant(values[index]);
    }
    return result + " }";
}

std::string constant(const ca::core::Any& value)
{
    if (auto item = value.as<ca::i32>()) return std::to_string(*item);
    if (auto item = value.as<ca::i64>()) return std::to_string(*item) + "L";
    if (auto item = value.as<ca::f32>()) return floating(*item, false);
    if (auto item = value.as<ca::f64>()) return floating(*item, true);
    if (auto item = value.as<std::string>())
        return quoted(ca::str::Utf8StringRef::from_data(
            reinterpret_cast<const ca::u8*>(item->data()), item->size()));
    if (auto item = value.as<ca::str::Utf8StringRef>()) return quoted(*item);
    if (auto item = value.as<ca::str::ZUtf8StringRef>()) return quoted(item->ref());
    if (auto item = value.as<O::Type>()) {
        //staticArena 退役：描述符仅瞬时传给 literal（深拷贝），函数级局部 arena 零驻留。
        ca::str::Utf8StringArena scratch;
        return literal(item->getDescriptor(scratch));
    }
    if (auto item = value.as<O::Handle>()) return handle(*item);
    if (auto item = value.as<O::ConstantDynamic>()) {
        return "{ " + literal(item->getName()) + ", " + literal(item->getDescriptor()) + ", " +
               handle(item->getBootstrapMethod()) + ", " +
               constants(item->getBootstrapMethodArguments()) + " }";
    }
    throw std::invalid_argument("Unsupported JVM constant");
}

std::string annotationElement(const T::AnnotationNode::Element& value);

std::string annotationBody(const T::AnnotationNode& annotation)
{
    std::string result = "{ ";
    for (ca::usize index = 0; index < annotation.elements.size(); ++index) {
        if (index) result += ", ";
        const auto& value = annotation.elements[index];
        if (!value.name.is_empty()) result += literal(value.name) + ": ";
        result += annotationElement(value);
    }
    return result + " }";
}

std::string annotationElement(const T::AnnotationNode::Element& value)
{
    switch (value.tag) {
    case 's': return quoted(value.sval.ref());
    case 'B':
    case 'S':
    case 'I': return std::to_string(value.ival);
    case 'C':
    {
        ca::str::Utf16StringBuilder builder;
        builder.append(ca::str::Char16(static_cast<ca::u16>(value.ival)));
        auto text         = builder.build();
        auto escapedValue = util::EscapeUtil::escape_string(text.ref());
        return "'" + escapedValue.to_utf8_string().to_std_string() + "'";
    }
    case 'Z': return value.ival ? "true" : "false";
    case 'J': return std::to_string(value.lval) + "L";
    case 'F': return floating(value.fval, false);
    case 'D': return floating(value.dval, true);
    case 'c': return literal(value.sval.ref());
    case 'e':
    {
        const auto text      = value.sval.ref();
        ca::usize  separator = 0;
        while (separator < text.byte_length() && text.byte_at(separator) != ' ') ++separator;
        if (separator == text.byte_length())
            throw std::invalid_argument("Invalid enum annotation value");
        return ".enum " + literal(text.slice(0, separator)) + " " +
               literal(text.slice(separator + 1, text.byte_length()));
    }
    case '@':
        return value.nested ? ".annotation " + literal(value.nested->desc) + " " +
                                  annotationBody(*value.nested)
                            : ".annotation java/lang/Object { }";
    case '[': return value.nested ? annotationBody(*value.nested) : "{ }";
    default: throw std::invalid_argument("Unsupported annotation value");
    }
}

void annotations(std::ostringstream& output, const std::vector<T::AnnotationNode*>& values,
                 const char* keyword, const std::string& indent = {})
{
    for (const auto* value : values)
        output << indent << keyword << ' ' << literal(value->desc) << ' ' << annotationBody(*value)
               << '\n';
}

void typeAnnotations(std::ostringstream& output, const std::vector<T::AnnotationNode*>& values,
                     const char* keyword, const std::string& indent = {})
{
    for (const auto* value : values) {
        const auto* typedValue = static_cast<const T::TypeAnnotationNode*>(value);
        output << indent << keyword << ' ' << typedValue->typeRef << ' '
               << literal(ca::str::Utf8StringRef::from_cstr(
                      typedValue->typePath ? typedValue->typePath->toString().c_str() : ""))
               << ' ' << literal(value->desc) << ' ' << annotationBody(*value) << '\n';
    }
}

using LabelNames = std::unordered_map<const O::Label*, std::string>;
using LineNumbers = std::unordered_map<const O::Label*, std::vector<ca::i32>>;

LabelNames labels(const T::MethodNode& method)
{
    LabelNames result;
    ca::usize  index = 0;
    for (const auto* instruction : method.instructions)
        if (instruction->getType() == T::AbstractInsnNode::LABEL) {
            const auto* label = ca::core::tag_cast::cast<const T::LabelNode*>(instruction);
            result.emplace(label->label, "L" + std::to_string(index++));
        }
    return result;
}

LineNumbers lineNumbers(const T::MethodNode& method)
{
    LineNumbers result;
    for (const auto* instruction : method.instructions) {
        if (instruction->getType() != T::AbstractInsnNode::LINE) {
            continue;
        }
        const auto* line = ca::core::tag_cast::cast<const T::LineNumberNode*>(instruction);
        result[line->start].push_back(line->line);
    }
    return result;
}

const std::string& label(const LabelNames& names, const O::Label* value)
{
    const auto found = names.find(value);
    if (found == names.end()) throw std::invalid_argument("Instruction references unknown label");
    return found->second;
}

const char* newArrayType(ca::i32 operand)
{
    switch (operand) {
    case O::Opcodes::T_BOOLEAN: return "boolean";
    case O::Opcodes::T_CHAR: return "char";
    case O::Opcodes::T_FLOAT: return "float";
    case O::Opcodes::T_DOUBLE: return "double";
    case O::Opcodes::T_BYTE: return "byte";
    case O::Opcodes::T_SHORT: return "short";
    case O::Opcodes::T_INT: return "int";
    case O::Opcodes::T_LONG: return "long";
    default: throw std::invalid_argument("Unsupported NEWARRAY operand");
    }
}

std::string instruction(const T::AbstractInsnNode& value, const LabelNames& names)
{
    const std::string opcode = value.getOpcode() >= 0 ? lowerOpcode(value.getOpcode()) : "";
    switch (value.getType()) {
    case T::AbstractInsnNode::INSN: return opcode;
    case T::AbstractInsnNode::INT_INSN:
    {
        const auto* item = ca::core::tag_cast::cast<const T::IntInsnNode*>(&value);
        return opcode + " " +
               (value.getOpcode() == O::Opcodes::NEWARRAY ? newArrayType(item->operand)
                                                          : std::to_string(item->operand));
    }
    case T::AbstractInsnNode::VAR_INSN:
    {
        const auto* item = ca::core::tag_cast::cast<const T::VarInsnNode*>(&value);
        return opcode + " " + std::to_string(item->var);
    }
    case T::AbstractInsnNode::TYPE_INSN:
    {
        const auto* item = ca::core::tag_cast::cast<const T::TypeInsnNode*>(&value);
        return opcode + " " + literal(item->desc);
    }
    case T::AbstractInsnNode::FIELD_INSN:
    {
        const auto* item = ca::core::tag_cast::cast<const T::FieldInsnNode*>(&value);
        return opcode + " " + literal(item->owner) + "." + literal(item->name) + " " +
               literal(item->desc);
    }
    case T::AbstractInsnNode::METHOD_INSN:
    {
        const auto* item     = ca::core::tag_cast::cast<const T::MethodInsnNode*>(&value);
        std::string mnemonic = opcode;
        if (item->isInterface && value.getOpcode() != O::Opcodes::INVOKEINTERFACE)
            mnemonic += "interface";
        return mnemonic + " " + literal(item->owner) + "." + literal(item->name) + " " +
               literal(item->desc);
    }
    case T::AbstractInsnNode::INVOKE_DYNAMIC_INSN:
    {
        const auto* item = ca::core::tag_cast::cast<const T::InvokeDynamicInsnNode*>(&value);
        return "invokedynamic " + literal(item->name) + " " + literal(item->desc) + " " +
               handle(item->bsm) + " " + constants(item->bsmArgs);
    }
    case T::AbstractInsnNode::JUMP_INSN:
    {
        const auto* item = ca::core::tag_cast::cast<const T::JumpInsnNode*>(&value);
        return opcode + " " + label(names, item->label);
    }
    case T::AbstractInsnNode::LABEL:
    {
        const auto* item = ca::core::tag_cast::cast<const T::LabelNode*>(&value);
        return label(names, item->label) + ":";
    }
    case T::AbstractInsnNode::LDC_INSN:
    {
        const auto* item = ca::core::tag_cast::cast<const T::LdcInsnNode*>(&value);
        return "ldc " + constant(item->cst);
    }
    case T::AbstractInsnNode::IINC_INSN:
    {
        const auto* item = ca::core::tag_cast::cast<const T::IincInsnNode*>(&value);
        return "iinc " + std::to_string(item->var) + " " + std::to_string(item->incr);
    }
    case T::AbstractInsnNode::TABLESWITCH_INSN:
    {
        const auto* item   = ca::core::tag_cast::cast<const T::TableSwitchInsnNode*>(&value);
        std::string result = "tableswitch { min: " + std::to_string(item->min) +
                             ", max: " + std::to_string(item->max) + ", cases: { ";
        for (ca::usize index = 0; index < item->labels.size(); ++index) {
            if (index) result += ", ";
            result += label(names, item->labels[index]);
        }
        return result + " }, default: " + label(names, item->dflt) + " }";
    }
    case T::AbstractInsnNode::LOOKUPSWITCH_INSN:
    {
        const auto* item   = ca::core::tag_cast::cast<const T::LookupSwitchInsnNode*>(&value);
        std::string result = "lookupswitch { ";
        for (ca::usize index = 0; index < item->keys.size(); ++index)
            result +=
                std::to_string(item->keys[index]) + ": " + label(names, item->labels[index]) + ", ";
        return result + "default: " + label(names, item->dflt) + " }";
    }
    case T::AbstractInsnNode::MULTIANEWARRAY_INSN:
    {
        const auto* item = ca::core::tag_cast::cast<const T::MultiANewArrayInsnNode*>(&value);
        return "multianewarray " + literal(item->desc) + " " + std::to_string(item->dims);
    }
    case T::AbstractInsnNode::LINE:
    {
        const auto* item = ca::core::tag_cast::cast<const T::LineNumberNode*>(&value);
        return "line " + std::to_string(item->line);
    }
    case T::AbstractInsnNode::FRAME: return {};
    default: throw std::invalid_argument("Unsupported JVM instruction node");
    }
}

void printMethodDeclaration(std::ostringstream& output, const T::MethodNode& method)
{
    annotations(output, method.visibleAnnotations, ".visible-annotation", "    ");
    annotations(output, method.invisibleAnnotations, ".invisible-annotation", "    ");
    typeAnnotations(output, method.visibleTypeAnnotations, ".type-visible-annotation", "    ");
    typeAnnotations(output, method.invisibleTypeAnnotations, ".type-invisible-annotation", "    ");
    if (!method.signature.is_empty())
        output << "    .signature " << quoted(method.signature) << '\n';
    output << "    .method " << modifiers(method.access, false, true) << literal(method.name) << ' '
           << literal(method.desc);
    if (method.instructions.isEmpty()) {
        output << " { }\n";
        return;
    }
    const auto names = labels(method);
    const auto lines = lineNumbers(method);
    output << " {\n";
    if (!method.tryCatchBlocks.empty()) {
        output << "        exceptions: {\n";
        for (ca::usize index = 0; index < method.tryCatchBlocks.size(); ++index) {
            const auto* block = method.tryCatchBlocks[index];
            output << "            { " << label(names, block->start) << ", "
                   << label(names, block->end) << ", " << label(names, block->handler) << ", "
                   << (block->type.is_empty() ? "*" : literal(block->type)) << " }";
            output << (index + 1 == method.tryCatchBlocks.size() ? "\n" : ",\n");
        }
        output << "        },\n";
    }
    output << "        code: {\n";
    for (const auto* item : method.instructions) {
        if (item->getType() == T::AbstractInsnNode::LINE) {
            continue;
        }
        const auto text = instruction(*item, names);
        if (text.empty()) continue;
        output << (item->getType() == T::AbstractInsnNode::LABEL ? "        " : "            ")
               << text << '\n';
        if (item->getType() == T::AbstractInsnNode::LABEL) {
            const auto* labelNode = ca::core::tag_cast::cast<const T::LabelNode*>(item);
            const auto found = lines.find(labelNode->label);
            if (found == lines.end()) {
                continue;
            }
            for (const auto line : found->second) {
                output << "            line " << line << '\n';
            }
        }
    }
    output << "        }\n    }\n";
}

}   //namespace

error::Result<ca::str::Utf16String> JvmPrinter::print(const std::vector<ca::u8>& classBytes) const
{
    try {
        ca::str::Utf8StringArena arena;
        O::ClassReader           reader(arena, classBytes);
        T::ClassNode             clazz;
        reader.accept(&clazz, O::ClassReader::SKIP_FRAMES);
        std::ostringstream output;

        for (const auto* inner : clazz.innerClasses) {
            output << ".inner " << modifiers(inner->access, true, false) << "{ ";
            bool comma = false;
            if (!inner->innerName.is_empty()) {
                output << "name: " << literal(inner->innerName);
                comma = true;
            }
            if (comma) output << ", ";
            output << "inner: " << literal(inner->name);
            if (!inner->outerName.is_empty()) output << ", outer: " << literal(inner->outerName);
            output << " }\n";
        }
        if (!clazz.sourceFile.is_empty())
            output << ".sourcefile " << quoted(clazz.sourceFile) << '\n';
        if (!clazz.outerClass.is_empty())
            output << ".outer-class " << literal(clazz.outerClass) << '\n';
        if (!clazz.outerMethod.is_empty())
            output << ".outer-method " << literal(clazz.outerMethod) << ' '
                   << literal(clazz.outerMethodDesc) << '\n';
        if (!clazz.nestHost.is_empty()) output << ".nest-host " << literal(clazz.nestHost) << '\n';
        for (auto value : clazz.nestMembers) output << ".nest-member " << literal(value) << '\n';
        for (auto value : clazz.permittedSubclasses)
            output << ".permitted-subclass " << literal(value) << '\n';
        for (const auto* record : clazz.recordComponents) {
            annotations(output, record->visibleAnnotations, ".visible-annotation");
            annotations(output, record->invisibleAnnotations, ".invisible-annotation");
            if (!record->signature.is_empty())
                output << ".signature " << quoted(record->signature) << '\n';
            output << ".record-component " << literal(record->name) << ' '
                   << literal(record->descriptor) << '\n';
        }
        annotations(output, clazz.visibleAnnotations, ".visible-annotation");
        annotations(output, clazz.invisibleAnnotations, ".invisible-annotation");
        typeAnnotations(output, clazz.visibleTypeAnnotations, ".type-visible-annotation");
        typeAnnotations(output, clazz.invisibleTypeAnnotations, ".type-invisible-annotation");
        if (!clazz.signature.is_empty()) output << ".signature " << quoted(clazz.signature) << '\n';
        if (!clazz.superName.is_empty()) output << ".super " << literal(clazz.superName) << '\n';
        for (auto value : clazz.interfaces) output << ".implements " << literal(value) << '\n';
        output << ".class " << modifiers(clazz.access, true, false) << literal(clazz.name)
               << " {\n";
        for (const auto* field : clazz.fields) {
            annotations(output, field->visibleAnnotations, ".visible-annotation", "    ");
            annotations(output, field->invisibleAnnotations, ".invisible-annotation", "    ");
            if (!field->signature.is_empty())
                output << "    .signature " << quoted(field->signature) << '\n';
            output << "    .field " << modifiers(field->access, false, false)
                   << literal(field->name) << ' ' << literal(field->desc);
            if (field->value.has_value()) output << " { value: " << constant(field->value) << " }";
            output << '\n';
        }
        if (!clazz.fields.empty() && !clazz.methods.empty()) output << '\n';
        for (ca::usize index = 0; index < clazz.methods.size(); ++index) {
            printMethodDeclaration(output, *clazz.methods[index]);
            if (index + 1 != clazz.methods.size()) output << '\n';
        }
        output << "}\n";
        return error::Result<ca::str::Utf16String>::ok(utf16(output.str()));
    }
    catch (const std::exception& exception) {
        auto message = utf16(exception.what());
        return error::Result<ca::str::Utf16String>::err({error::Error(message.ref())});
    }
}

error::Result<ca::str::Utf16String> JvmPrinter::printMethod(
    const std::vector<ca::u8>& classBytes, ca::str::Utf8StringRef name,
    ca::str::Utf8StringRef descriptor) const
{
    try {
        //The reader stores UTF-8 refs in the supplied arena, so keep it alive until printing ends.
        //A local arena avoids exposing ClassNode ownership through this API.
        ca::str::Utf8StringArena arena;
        O::ClassReader            stableReader(arena, classBytes);
        T::ClassNode              clazz;
        stableReader.accept(&clazz, O::ClassReader::SKIP_FRAMES);
        for (const auto* method : clazz.methods) {
            if (method && method->name.equals(name) && method->desc.equals(descriptor)) {
                std::ostringstream output;
                printMethodDeclaration(output, *method);
                return error::Result<ca::str::Utf16String>::ok(utf16(output.str()));
            }
        }
        return error::Result<ca::str::Utf16String>::err(
            {error::Error(utf16("Method not found").ref())});
    }
    catch (const std::exception& exception) {
        auto message = utf16(exception.what());
        return error::Result<ca::str::Utf16String>::err({error::Error(message.ref())});
    }
}

}   //namespace jhasm::printer
