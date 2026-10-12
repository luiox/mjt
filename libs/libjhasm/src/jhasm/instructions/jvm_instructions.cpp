#include "jvm_instructions.hpp"

#include <charconv>
#include <string>

namespace jhasm::instructions {
namespace {
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

bool parseField(ca::str::Utf16StringRef value, ca::usize& index, bool allowVoid) noexcept
{
    if (index >= value.length()) return false;
    ca::u16 current = value.char_at(index++).value;
    if (current == 'V') return allowVoid;
    if (current == 'B' || current == 'C' || current == 'D' || current == 'F' || current == 'I' ||
        current == 'J' || current == 'S' || current == 'Z')
        return true;
    if (current == '[') return parseField(value, index, false);
    if (current != 'L') return false;
    const ca::usize begin = index;
    while (index < value.length() && value.char_at(index).value != ';') ++index;
    if (index == begin || index >= value.length()) return false;
    ++index;
    return true;
}

void addError(error::ErrorCollector& errors, const char* message, const ast::Element& element)
{
    const util::Location* location = element.location();
    errors.add_error(ca::str::Utf16String::value_of(message).ref(),
                     location ? *location : util::Location::unknown());
}

bool isInteger(const ast::ElementPtr& value) noexcept
{
    if (!value || value->getType() != ast::ElementKind::NUMBER) return false;
    const auto number = std::static_pointer_cast<ast::Number>(value);
    if (number->isFloatingPoint()) return false;
    std::string text = number->content().to_utf8_string().to_std_string();
    if (!text.empty() && (text.back() == 'l' || text.back() == 'L')) text.pop_back();
    ca::i64     parsed = 0;
    ca::i32     base   = 10;
    const char* begin  = text.data();
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        begin += 2;
        base = 16;
    }
    const auto result = std::from_chars(begin, text.data() + text.size(), parsed, base);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

bool isLiteral(const ast::ElementPtr& value) noexcept
{
    return value && (value->getType() == ast::ElementKind::IDENTIFIER ||
                     value->getType() == ast::ElementKind::NUMBER);
}

bool isConstant(const ast::ElementPtr& value) noexcept
{
    if (!value) return false;
    switch (value->getType()) {
    case ast::ElementKind::NUMBER:
    case ast::ElementKind::STRING:
    case ast::ElementKind::CHARACTER:
    case ast::ElementKind::IDENTIFIER: return true;
    case ast::ElementKind::ARRAY:
    {
        const auto array = std::static_pointer_cast<ast::Array>(value);
        return array->values().size() == 3 || array->values().size() == 4;
    }
    default: return false;
    }
}

bool validateObjectIdentifier(const ast::Object& object, const char* name) noexcept
{
    const auto key   = ca::str::Utf16String::value_of(name);
    const auto value = object.value(key.ref());
    return value && value->getType() == ast::ElementKind::IDENTIFIER;
}

bool verifyOperand(OperandKind kind, const ast::ElementPtr& value) noexcept
{
    switch (kind) {
    case OperandKind::INTEGER: return isInteger(value);
    case OperandKind::LITERAL: return isLiteral(value);
    case OperandKind::LABEL:
    case OperandKind::TYPE: return value && value->getType() == ast::ElementKind::IDENTIFIER;
    case OperandKind::NEW_ARRAY_TYPE:
        if (!value || value->getType() != ast::ElementKind::IDENTIFIER) return false;
        return equals(value->content(), "boolean") || equals(value->content(), "byte") ||
               equals(value->content(), "char") || equals(value->content(), "short") ||
               equals(value->content(), "int") || equals(value->content(), "float") ||
               equals(value->content(), "long") || equals(value->content(), "double");
    case OperandKind::CONSTANT: return isConstant(value);
    case OperandKind::FIELD_DESCRIPTOR:
        return value && value->getType() == ast::ElementKind::IDENTIFIER &&
               isFieldDescriptor(value->content());
    case OperandKind::METHOD_DESCRIPTOR:
        return value && value->getType() == ast::ElementKind::IDENTIFIER &&
               isMethodDescriptor(value->content());
    case OperandKind::HANDLE:
        return value && (value->getType() == ast::ElementKind::IDENTIFIER ||
                         (value->getType() == ast::ElementKind::ARRAY &&
                          std::static_pointer_cast<ast::Array>(value)->values().size() == 3));
    case OperandKind::ARGS:
        if (!value || value->getType() == ast::ElementKind::EMPTY) return true;
        if (value->getType() != ast::ElementKind::ARRAY) return false;
        for (const auto& item : std::static_pointer_cast<ast::Array>(value)->values()) {
            if (!isConstant(item)) return false;
        }
        return true;
    case OperandKind::TABLE_SWITCH:
    {
        if (!value || value->getType() != ast::ElementKind::OBJECT) return false;
        const auto object   = std::static_pointer_cast<ast::Object>(value);
        const auto minKey   = ca::str::Utf16String::value_of("min");
        const auto maxKey   = ca::str::Utf16String::value_of("max");
        const auto casesKey = ca::str::Utf16String::value_of("cases");
        const auto cases    = object->value(casesKey.ref());
        return isInteger(object->value(minKey.ref())) && isInteger(object->value(maxKey.ref())) &&
               validateObjectIdentifier(*object, "default") && cases &&
               (cases->getType() == ast::ElementKind::ARRAY ||
                cases->getType() == ast::ElementKind::EMPTY);
    }
    case OperandKind::LOOKUP_SWITCH:
    {
        if (!value || value->getType() != ast::ElementKind::OBJECT) return false;
        const auto object = std::static_pointer_cast<ast::Object>(value);
        if (!validateObjectIdentifier(*object, "default")) return false;
        for (const auto& entry : object->values()) {
            if (!entry.second || entry.second->getType() != ast::ElementKind::IDENTIFIER)
                return false;
        }
        return true;
    }
    }
    return false;
}
}   //namespace

bool isFieldDescriptor(ca::str::Utf16StringRef value) noexcept
{
    ca::usize index = 0;
    return parseField(value, index, false) && index == value.length();
}

bool isMethodDescriptor(ca::str::Utf16StringRef value) noexcept
{
    if (value.empty() || value.char_at(0).value != '(') return false;
    ca::usize index = 1;
    while (index < value.length() && value.char_at(index).value != ')') {
        if (!parseField(value, index, false)) return false;
    }
    if (index >= value.length() || value.char_at(index++).value != ')') return false;
    return parseField(value, index, true) && index == value.length();
}

JvmInstructions::JvmInstructions()
{
    auto add = [this](std::initializer_list<const char*> names,
                      std::vector<OperandKind>           operands = {}) {
        for (const char* name : names) instructions_.push_back({name, operands});
    };
    add({"nop",      "aconst_null", "iconst_m1", "iconst_0",     "iconst_1",   "iconst_2",
         "iconst_3", "iconst_4",    "iconst_5",  "lconst_0",     "lconst_1",   "fconst_0",
         "fconst_1", "fconst_2",    "dconst_0",  "dconst_1",     "iaload",     "laload",
         "faload",   "daload",      "aaload",    "baload",       "caload",     "saload",
         "iastore",  "lastore",     "fastore",   "dastore",      "aastore",    "bastore",
         "castore",  "sastore",     "pop",       "pop2",         "dup",        "dup_x1",
         "dup_x2",   "dup2",        "dup2_x1",   "dup2_x2",      "swap",       "iadd",
         "ladd",     "fadd",        "dadd",      "isub",         "lsub",       "fsub",
         "dsub",     "imul",        "lmul",      "fmul",         "dmul",       "idiv",
         "ldiv",     "fdiv",        "ddiv",      "irem",         "lrem",       "frem",
         "drem",     "ineg",        "lneg",      "fneg",         "dneg",       "ishl",
         "lshl",     "ishr",        "lshr",      "iushr",        "lushr",      "iand",
         "land",     "ior",         "lor",       "ixor",         "lxor",       "i2l",
         "i2f",      "i2d",         "l2i",       "l2f",          "l2d",        "f2i",
         "f2l",      "f2d",         "d2i",       "d2l",          "d2f",        "i2b",
         "i2c",      "i2s",         "lcmp",      "fcmpl",        "fcmpg",      "dcmpl",
         "dcmpg",    "ireturn",     "lreturn",   "freturn",      "dreturn",    "areturn",
         "return",   "arraylength", "athrow",    "monitorenter", "monitorexit"});
    add({"bipush", "sipush", "line"}, {OperandKind::INTEGER});
    add({"anewarray", "new", "checkcast", "instanceof"}, {OperandKind::TYPE});
    add({"newarray"}, {OperandKind::NEW_ARRAY_TYPE});
    add({"ldc", "ldc_w", "ldc2_w"}, {OperandKind::CONSTANT});
    add({"iload",
         "lload",
         "fload",
         "dload",
         "aload",
         "istore",
         "lstore",
         "fstore",
         "dstore",
         "astore",
         "ret"},
        {OperandKind::LITERAL});
    add({"iinc"}, {OperandKind::LITERAL, OperandKind::INTEGER});
    add({"ifeq",      "ifne",      "iflt",      "ifge",      "ifgt",      "ifle",      "if_icmpeq",
         "if_icmpne", "if_icmplt", "if_icmpge", "if_icmpgt", "if_icmple", "if_acmpeq", "if_acmpne",
         "goto",      "jsr",       "ifnull",    "ifnonnull", "jsr_w",     "goto_w"},
        {OperandKind::LABEL});
    add({"tableswitch"}, {OperandKind::TABLE_SWITCH});
    add({"lookupswitch"}, {OperandKind::LOOKUP_SWITCH});
    add({"getstatic", "putstatic", "getfield", "putfield"},
        {OperandKind::LITERAL, OperandKind::FIELD_DESCRIPTOR});
    add({"invokevirtual",
         "invokespecial",
         "invokestatic",
         "invokeinterface",
         "invokevirtualinterface",
         "invokestaticinterface",
         "invokespecialinterface"},
        {OperandKind::LITERAL, OperandKind::METHOD_DESCRIPTOR});
    add({"invokedynamic"},
        {OperandKind::LITERAL,
         OperandKind::METHOD_DESCRIPTOR,
         OperandKind::HANDLE,
         OperandKind::ARGS});
    add({"multianewarray"}, {OperandKind::TYPE, OperandKind::INTEGER});
}

const JvmInstructions& JvmInstructions::instance()
{
    static const JvmInstructions value;
    return value;
}

const InstructionSpec* JvmInstructions::find(ca::str::Utf16StringRef name) const noexcept
{
    for (const auto& instruction : instructions_) {
        if (equals(name, instruction.name)) return &instruction;
    }
    return nullptr;
}

bool JvmInstructions::verify(const ast::Instruction& instruction,
                             error::ErrorCollector&  errors) const
{
    const auto* spec = find(instruction.identifier()->content());
    if (!spec) {
        addError(errors, "Unknown JVM instruction", instruction);
        return false;
    }
    if (instruction.arguments().size() != spec->operands.size()) {
        addError(errors, "Unexpected JVM instruction operand count", instruction);
        return false;
    }
    bool valid = true;
    for (ca::usize index = 0; index < spec->operands.size(); ++index) {
        if (!verifyOperand(spec->operands[index], instruction.arguments()[index])) {
            addError(errors, "Invalid JVM instruction operand", *instruction.arguments()[index]);
            valid = false;
        }
    }
    return valid;
}

}   //namespace jhasm::instructions
