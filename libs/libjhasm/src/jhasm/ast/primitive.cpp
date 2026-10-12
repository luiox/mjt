#include "primitive.hpp"

namespace jhasm::ast {
namespace {
bool contains(ca::str::Utf16StringRef value, ca::u16 needle) noexcept
{
    return value.index_of(ca::str::Char16(needle)) != ca::str::Utf16StringRef::npos;
}

std::vector<ElementPtr> objectChildren(const std::vector<Object::Entry>& values)
{
    std::vector<ElementPtr> result;
    result.reserve(values.size() * 2U);
    for (const auto& entry : values) {
        result.push_back(entry.first);
        result.push_back(entry.second);
    }
    return result;
}

std::vector<ElementPtr> declarationChildren(const IdentifierPtr&           keyword,
                                            const std::vector<ElementPtr>& elements)
{
    std::vector<ElementPtr> result;
    if (keyword) result.push_back(keyword);
    result.insert(result.end(), elements.begin(), elements.end());
    return result;
}

std::vector<ElementPtr> instructionChildren(const IdentifierPtr&           identifier,
                                            const std::vector<ElementPtr>& arguments)
{
    std::vector<ElementPtr> result{identifier};
    result.insert(result.end(), arguments.begin(), arguments.end());
    return result;
}
}   //namespace

bool Number::isFloatingPoint() const noexcept
{
    const auto text = content();
    return contains(text, '.') || contains(text, 'e') || contains(text, 'E') ||
           (!text.empty() && (text.char_at(text.length() - 1).value == 'F' ||
                              text.char_at(text.length() - 1).value == 'f' ||
                              text.char_at(text.length() - 1).value == 'D' ||
                              text.char_at(text.length() - 1).value == 'd'));
}

bool Number::isWide() const noexcept
{
    const auto text = content();
    if (text.empty()) return false;
    const ca::u16 suffix = text.char_at(text.length() - 1).value;
    return suffix == 'L' || suffix == 'l' || suffix == 'D' || suffix == 'd' ||
           (isFloatingPoint() && suffix != 'F' && suffix != 'f');
}

bool Bool::boolValue() const noexcept
{
    static const std::u16string truth = u"true";
    return content().equals(ca::str::Utf16StringRef::from_std_u16string(truth));
}

Object::Object(std::vector<Entry> values)
    : Element(objectChildren(values))
    , values_(std::move(values))
{}

ElementPtr Object::value(ca::str::Utf16StringRef name) const noexcept
{
    for (const auto& entry : values_) {
        if (entry.first && entry.first->content().equals(name)) return entry.second;
    }
    return nullptr;
}

Declaration::Declaration(IdentifierPtr keyword, std::vector<ElementPtr> elements)
    : Element(declarationChildren(keyword, elements))
    , keyword_(std::move(keyword))
    , elements_(std::move(elements))
{}

Instruction::Instruction(IdentifierPtr identifier, std::vector<ElementPtr> arguments)
    : Element(instructionChildren(identifier, arguments))
    , identifier_(std::move(identifier))
    , arguments_(std::move(arguments))
{}

Label::Label(IdentifierPtr identifier)
    : Instruction(std::move(identifier), {})
{}

Code::Code(std::vector<InstructionPtr> instructions)
    : Element(std::vector<ElementPtr>(instructions.begin(), instructions.end()))
    , instructions_(std::move(instructions))
{}

}   //namespace jhasm::ast
