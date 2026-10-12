#include "ast_processor.hpp"

#include "jhasm/error/error_collector.hpp"
#include "jhasm/instructions/jvm_instructions.hpp"

#include <algorithm>
#include <utility>

namespace jhasm::parser {
namespace {
using namespace ast;

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

class Context final
{
public:
    error::Result<std::vector<ElementPtr>> process(const std::vector<ElementPtr>& input)
    {
        std::vector<ElementPtr> output;
        for (const auto& element : input) {
            if (!element || element->getType() != ElementKind::DECLARATION) {
                addError("Expected declaration", element.get());
                continue;
            }
            ElementPtr result = declaration(std::static_pointer_cast<Declaration>(element), false);
            if (result) output.push_back(std::move(result));
        }
        output.insert(output.end(), pending_.elements.begin(), pending_.elements.end());
        return error::Result<std::vector<ElementPtr>>(
            std::move(output), errors_.take_errors(), errors_.take_warnings());
    }

private:
    struct Pending final
    {
        Attributes                                    attributes;
        IdentifierPtr                                 superName;
        std::shared_ptr<String>                       sourceFile;
        std::vector<IdentifierPtr>                    interfaces;
        std::vector<IdentifierPtr>                    permitted;
        std::vector<std::shared_ptr<InnerClass>>      inners;
        std::vector<std::shared_ptr<RecordComponent>> records;
        IdentifierPtr                                 outerClass;
        std::shared_ptr<OuterMethod>                  outerMethod;
        IdentifierPtr                                 nestHost;
        std::vector<IdentifierPtr>                    nestMembers;
        std::vector<ElementPtr>                       elements;

        Attributes takeGeneric()
        {
            Attributes result = std::move(attributes);
            attributes        = {};
            elements.clear();
            return result;
        }

        void clear() { *this = Pending{}; }
    } pending_;

    void addError(const char* message, const Element* element)
    {
        errors_.add_error(
            ca::str::Utf16String::value_of(message).ref(),
            element && element->location() ? *element->location() : util::Location::unknown());
    }

    IdentifierPtr identifier(const ElementPtr& value, const char* expected)
    {
        if (value && value->getType() == ElementKind::IDENTIFIER)
            return std::static_pointer_cast<Identifier>(value);
        if (value && value->getType() == ElementKind::NUMBER && value->value())
            return std::make_shared<Identifier>(*value->value());
        addError(expected, value.get());
        return nullptr;
    }

    Modifiers modifiers(const std::vector<ElementPtr>& elements, ca::usize end,
                        const Element& parent)
    {
        Modifiers result;
        for (ca::usize index = 0; index < end; ++index) {
            auto value = identifier(elements[index], "Expected access modifier");
            if (!value) continue;
            if (!Modifiers::isValid(value->content())) {
                addError("Invalid modifier", value.get());
                continue;
            }
            result.add(std::move(value));
        }
        (void)parent;
        return result;
    }

    void applyGeneric(Annotated& target) { target.attributes() = pending_.takeGeneric(); }

    void applyClass(Class& target)
    {
        target.attributes() = std::move(pending_.attributes);
        target.superName(std::move(pending_.superName));
        target.sourceFile(std::move(pending_.sourceFile));
        for (auto& value : pending_.interfaces) target.addInterface(std::move(value));
        for (auto& value : pending_.permitted) target.addPermittedSubclass(std::move(value));
        for (auto& value : pending_.inners) target.addInnerClass(std::move(value));
        for (auto& value : pending_.records) target.addRecordComponent(std::move(value));
        target.outerClass(std::move(pending_.outerClass));
        target.outerMethod(std::move(pending_.outerMethod));
        target.nestHost(std::move(pending_.nestHost));
        for (auto& value : pending_.nestMembers) target.addNestMember(std::move(value));
        pending_.clear();
    }

    ElementPtr declaration(const std::shared_ptr<Declaration>& value, bool inAnnotation)
    {
        if (!value->keyword()) {
            addError("Expected declaration keyword", value.get());
            return nullptr;
        }
        auto keyword = value->keyword()->content();
        if (equals(keyword, ".class")) return parseClass(value);
        if (equals(keyword, ".field")) return parseField(value);
        if (equals(keyword, ".method")) return parseMethod(value);
        if (equals(keyword, ".annotation") || equals(keyword, ".visible-annotation"))
            return annotationDeclaration(value, true, false, inAnnotation);
        if (equals(keyword, ".invisible-annotation"))
            return annotationDeclaration(value, false, false, inAnnotation);
        if (equals(keyword, ".type-annotation") || equals(keyword, ".type-visible-annotation"))
            return annotationDeclaration(value, true, true, inAnnotation);
        if (equals(keyword, ".type-invisible-annotation"))
            return annotationDeclaration(value, false, true, inAnnotation);
        if (equals(keyword, ".enum")) {
            if (!inAnnotation) {
                addError("Enum declaration outside annotation", value.get());
                return nullptr;
            }
            if (value->elements().size() != 2) {
                addError("Expected enum type and value", value.get());
                return nullptr;
            }
            return std::make_shared<EnumValue>(
                identifier(value->element(0), "Expected enum type"),
                identifier(value->element(1), "Expected enum value"));
        }
        if (equals(keyword, ".inner")) return parseInner(value);
        if (equals(keyword, ".signature"))
            return singleString(value, pending_.attributes.signature, "Expected signature string");
        if (equals(keyword, ".sourcefile"))
            return singleString(value, pending_.sourceFile, "Expected source file string");
        if (equals(keyword, ".super"))
            return singleIdentifier(value, pending_.superName, "Expected super name");
        if (equals(keyword, ".implements"))
            return appendIdentifier(value, pending_.interfaces, "Expected interface name");
        if (equals(keyword, ".permitted-subclass"))
            return appendIdentifier(value, pending_.permitted, "Expected subclass name");
        if (equals(keyword, ".nest-member"))
            return appendIdentifier(value, pending_.nestMembers, "Expected nest member");
        if (equals(keyword, ".nest-host"))
            return singleIdentifier(value, pending_.nestHost, "Expected nest host");
        if (equals(keyword, ".outer-class"))
            return singleIdentifier(value, pending_.outerClass, "Expected outer class");
        if (equals(keyword, ".outer-method")) return parseOuterMethod(value);
        if (equals(keyword, ".record-component")) return parseRecordComponent(value);
        addError("Unknown declaration", value.get());
        return nullptr;
    }

    ElementPtr remember(ElementPtr value)
    {
        if (value) pending_.elements.push_back(value);
        return nullptr;
    }

    ElementPtr annotationDeclaration(const std::shared_ptr<Declaration>& declaration, bool visible,
                                     bool typeAnnotation, bool inAnnotation)
    {
        auto result = parseAnnotation(declaration, visible, typeAnnotation, !inAnnotation);
        return inAnnotation ? result : nullptr;
    }

    ElementPtr singleString(const std::shared_ptr<Declaration>& declaration,
                            std::shared_ptr<String>& destination, const char* error)
    {
        if (declaration->elements().size() != 1 ||
            declaration->element(0)->getType() != ElementKind::STRING) {
            addError(error, declaration.get());
            return nullptr;
        }
        destination = std::static_pointer_cast<String>(declaration->element(0));
        return remember(destination);
    }

    ElementPtr singleIdentifier(const std::shared_ptr<Declaration>& declaration,
                                IdentifierPtr& destination, const char* error)
    {
        if (declaration->elements().size() != 1) {
            addError(error, declaration.get());
            return nullptr;
        }
        destination = identifier(declaration->element(0), error);
        return remember(destination);
    }

    ElementPtr appendIdentifier(const std::shared_ptr<Declaration>& declaration,
                                std::vector<IdentifierPtr>& destination, const char* error)
    {
        IdentifierPtr value;
        singleIdentifier(declaration, value, error);
        if (value) destination.push_back(value);
        return nullptr;
    }

    std::shared_ptr<Class> parseClass(const std::shared_ptr<Declaration>& declaration)
    {
        const auto& elements = declaration->elements();
        if (elements.size() < 2) {
            addError("Expected class name and body", declaration.get());
            return nullptr;
        }
        const ca::usize         bodyIndex = elements.size() - 1;
        auto                    name = identifier(elements[bodyIndex - 1], "Expected class name");
        std::vector<ElementPtr> bodyElements;
        if (elements[bodyIndex]->getType() == ElementKind::DECLARATION) {
            bodyElements = std::static_pointer_cast<Declaration>(elements[bodyIndex])->elements();
        }
        else if (elements[bodyIndex]->getType() != ElementKind::EMPTY) {
            addError("Expected class body", elements[bodyIndex].get());
            return nullptr;
        }

        Pending outer = std::move(pending_);
        pending_      = {};
        std::vector<ElementPtr> contents;
        for (const auto& item : bodyElements) {
            if (!item || item->getType() != ElementKind::DECLARATION) {
                addError("Expected class member or attribute", item.get());
                continue;
            }
            ElementPtr parsed = declarationCast(item);
            if (parsed) {
                if (parsed->getType() != ElementKind::FIELD &&
                    parsed->getType() != ElementKind::METHOD)
                    addError("Expected field or method", parsed.get());
                else
                    contents.push_back(std::move(parsed));
            }
        }
        if (!pending_.elements.empty()) {
            addError("Unconsumed class member attributes", pending_.elements.front().get());
        }
        pending_    = std::move(outer);
        auto result = std::make_shared<Class>(
            modifiers(elements, bodyIndex - 1, *declaration), std::move(name), std::move(contents));
        applyClass(*result);
        return result;
    }

    ElementPtr declarationCast(const ElementPtr& value)
    {
        return declaration(std::static_pointer_cast<Declaration>(value), false);
    }

    std::shared_ptr<Field> parseField(const std::shared_ptr<Declaration>& declaration)
    {
        const auto& elements = declaration->elements();
        if (elements.size() < 2) {
            addError("Expected field name and descriptor", declaration.get());
            return nullptr;
        }
        ca::usize  descriptorIndex = elements.size() - 1;
        ElementPtr fieldValue;
        if (elements.back()->getType() == ElementKind::OBJECT) {
            auto object = std::static_pointer_cast<Object>(elements.back());
            auto key    = ca::str::Utf16String::value_of("value");
            fieldValue  = object->value(key.ref());
            if (object->values().size() != 1 || !fieldValue) {
                addError("Expected field value", elements.back().get());
                return nullptr;
            }
            if (descriptorIndex < 2) {
                addError("Expected field name and descriptor", declaration.get());
                return nullptr;
            }
            --descriptorIndex;
        }
        const ca::usize nameIndex = descriptorIndex - 1;
        auto            result    = std::make_shared<Field>(
            modifiers(elements, nameIndex, *declaration),
            identifier(elements[nameIndex], "Expected field name"),
            identifier(elements[descriptorIndex], "Expected field descriptor"),
            std::move(fieldValue));
        applyGeneric(*result);
        return result;
    }

    std::shared_ptr<Method> parseMethod(const std::shared_ptr<Declaration>& declaration)
    {
        const auto& elements = declaration->elements();
        if (elements.size() < 3) {
            addError("Expected method name, descriptor and body", declaration.get());
            return nullptr;
        }
        const ca::usize         bodyIndex       = elements.size() - 1;
        const ca::usize         descriptorIndex = bodyIndex - 1;
        const ca::usize         nameIndex       = bodyIndex - 2;
        std::shared_ptr<Object> body;
        if (elements[bodyIndex]->getType() == ElementKind::OBJECT)
            body = std::static_pointer_cast<Object>(elements[bodyIndex]);
        else if (elements[bodyIndex]->getType() != ElementKind::EMPTY) {
            addError("Expected method body", elements[bodyIndex].get());
            return nullptr;
        }

        std::vector<IdentifierPtr>              parameters;
        std::vector<std::shared_ptr<Exception>> exceptions;
        std::shared_ptr<Code>                   code;
        ElementPtr                              defaultValue;
        if (body) {
            auto parametersKey = ca::str::Utf16String::value_of("parameters");
            auto exceptionsKey = ca::str::Utf16String::value_of("exceptions");
            auto codeKey       = ca::str::Utf16String::value_of("code");
            auto defaultKey    = ca::str::Utf16String::value_of("default-value");
            readIdentifiers(
                body->value(parametersKey.ref()), parameters, "Expected method parameter");
            readExceptions(body->value(exceptionsKey.ref()), exceptions);
            ElementPtr codeElement = body->value(codeKey.ref());
            if (codeElement) {
                if (codeElement->getType() == ElementKind::CODE)
                    code = std::static_pointer_cast<Code>(codeElement);
                else if (codeElement->getType() == ElementKind::EMPTY)
                    code = std::make_shared<Code>(std::vector<InstructionPtr>{});
                else
                    addError("Expected method code", codeElement.get());
            }
            defaultValue = annotationValue(body->value(defaultKey.ref()));
        }
        if (code) {
            for (const auto& instruction : code->instructions()) {
                if (instruction && instruction->getType() != ElementKind::LABEL)
                    instructions::JvmInstructions::instance().verify(*instruction, errors_);
            }
        }
        auto result = std::make_shared<Method>(
            modifiers(elements, nameIndex, *declaration),
            identifier(elements[nameIndex], "Expected method name"),
            identifier(elements[descriptorIndex], "Expected method descriptor"),
            std::move(parameters),
            ParameterAnnotations{},
            std::move(defaultValue),
            std::move(exceptions),
            std::move(code));
        applyGeneric(*result);
        return result;
    }

    void readIdentifiers(const ElementPtr& value, std::vector<IdentifierPtr>& output,
                         const char* error)
    {
        if (!value || value->getType() == ElementKind::EMPTY) return;
        if (value->getType() != ElementKind::ARRAY) {
            addError(error, value.get());
            return;
        }
        for (const auto& item : std::static_pointer_cast<Array>(value)->values()) {
            auto id = identifier(item, error);
            if (id) output.push_back(std::move(id));
        }
    }

    void readExceptions(const ElementPtr& value, std::vector<std::shared_ptr<Exception>>& output)
    {
        if (!value || value->getType() == ElementKind::EMPTY) return;
        if (value->getType() != ElementKind::ARRAY) {
            addError("Expected method exceptions", value.get());
            return;
        }
        for (const auto& item : std::static_pointer_cast<Array>(value)->values()) {
            if (!item || item->getType() != ElementKind::ARRAY) {
                addError("Expected method exception", item.get());
                continue;
            }
            const auto& parts = std::static_pointer_cast<Array>(item)->values();
            if (parts.size() != 4) {
                addError("Expected exception start, end, handler and type", item.get());
                continue;
            }
            output.push_back(
                std::make_shared<Exception>(identifier(parts[0], "Expected exception start"),
                                            identifier(parts[1], "Expected exception end"),
                                            identifier(parts[2], "Expected exception handler"),
                                            identifier(parts[3], "Expected exception type")));
        }
    }

    ElementPtr annotationValue(const ElementPtr& value)
    {
        if (!value) return nullptr;
        if (value->getType() == ElementKind::DECLARATION) {
            auto decl = std::static_pointer_cast<Declaration>(value);
            if (!decl->keyword()) {
                std::vector<ElementPtr> values;
                for (const auto& item : decl->elements()) values.push_back(annotationValue(item));
                return std::make_shared<Array>(std::move(values));
            }
            return declaration(decl, true);
        }
        if (value->getType() == ElementKind::ARRAY) {
            std::vector<ElementPtr> values;
            for (const auto& item : std::static_pointer_cast<Array>(value)->values())
                values.push_back(annotationValue(item));
            return std::make_shared<Array>(std::move(values));
        }
        if (value->getType() == ElementKind::EMPTY)
            return std::make_shared<Array>(std::vector<ElementPtr>{});
        switch (value->getType()) {
        case ElementKind::IDENTIFIER:
        case ElementKind::NUMBER:
        case ElementKind::STRING:
        case ElementKind::CHARACTER:
        case ElementKind::BOOL: return value;
        default: addError("Expected annotation value", value.get()); return nullptr;
        }
    }

    std::shared_ptr<Annotation> parseAnnotation(const std::shared_ptr<Declaration>& declaration,
                                                bool visible, bool typeAnnotation, bool attach)
    {
        if (declaration->elements().size() != 2) {
            addError("Expected annotation type and values", declaration.get());
            return nullptr;
        }
        auto       type          = identifier(declaration->element(0), "Expected annotation type");
        ElementPtr valuesElement = declaration->element(1);
        std::shared_ptr<Number> typeRef;
        IdentifierPtr           typePath;
        if (typeAnnotation) {
            if (!valuesElement || valuesElement->getType() != ElementKind::OBJECT) {
                addError("Expected type annotation data", valuesElement.get());
                return nullptr;
            }
            auto data        = std::static_pointer_cast<Object>(valuesElement);
            auto locationKey = ca::str::Utf16String::value_of("location");
            auto valuesKey   = ca::str::Utf16String::value_of("values");
            auto location = elementAs<Object>(data->value(locationKey.ref()), ElementKind::OBJECT);
            if (!location) {
                addError("Expected type annotation location", valuesElement.get());
                return nullptr;
            }
            auto refKey  = ca::str::Utf16String::value_of("ref");
            auto pathKey = ca::str::Utf16String::value_of("path");
            typeRef      = elementAs<Number>(location->value(refKey.ref()), ElementKind::NUMBER);
            typePath = identifier(location->value(pathKey.ref()), "Expected type annotation path");
            valuesElement = data->value(valuesKey.ref());
        }
        std::vector<Object::Entry> values;
        if (valuesElement && valuesElement->getType() == ElementKind::OBJECT) {
            for (const auto& entry : std::static_pointer_cast<Object>(valuesElement)->values())
                values.emplace_back(entry.first, annotationValue(entry.second));
        }
        else if (!valuesElement || valuesElement->getType() != ElementKind::EMPTY) {
            addError("Expected annotation values", valuesElement.get());
            return nullptr;
        }
        auto result = std::make_shared<Annotation>(
            visible, std::move(type), std::move(values), std::move(typeRef), std::move(typePath));
        if (attach) {
            auto& list = typeAnnotation ? (visible ? pending_.attributes.visibleTypeAnnotations
                                                   : pending_.attributes.invisibleTypeAnnotations)
                                        : (visible ? pending_.attributes.visibleAnnotations
                                                   : pending_.attributes.invisibleAnnotations);
            list.push_back(result);
            pending_.elements.push_back(result);
        }
        return result;
    }

    ElementPtr parseInner(const std::shared_ptr<Declaration>& declaration)
    {
        const auto& elements = declaration->elements();
        if (elements.empty() || elements.back()->getType() != ElementKind::OBJECT) {
            addError("Expected inner class modifiers and body", declaration.get());
            return nullptr;
        }
        auto body     = std::static_pointer_cast<Object>(elements.back());
        auto nameKey  = ca::str::Utf16String::value_of("name");
        auto outerKey = ca::str::Utf16String::value_of("outer");
        auto innerKey = ca::str::Utf16String::value_of("inner");
        auto result   = std::make_shared<InnerClass>(
            modifiers(elements, elements.size() - 1, *declaration),
            maybeIdentifier(body->value(nameKey.ref())),
            maybeIdentifier(body->value(outerKey.ref())),
            identifier(body->value(innerKey.ref()), "Expected inner class"));
        pending_.inners.push_back(result);
        return remember(result);
    }

    ElementPtr parseOuterMethod(const std::shared_ptr<Declaration>& declaration)
    {
        if (declaration->elements().size() != 2) {
            addError("Expected outer method name and descriptor", declaration.get());
            return nullptr;
        }
        pending_.outerMethod = std::make_shared<OuterMethod>(
            identifier(declaration->element(0), "Expected outer method name"),
            identifier(declaration->element(1), "Expected outer method descriptor"));
        return remember(pending_.outerMethod);
    }

    ElementPtr parseRecordComponent(const std::shared_ptr<Declaration>& declaration)
    {
        if (declaration->elements().size() != 2) {
            addError("Expected record component name and descriptor", declaration.get());
            return nullptr;
        }
        auto result = std::make_shared<RecordComponent>(
            identifier(declaration->element(0), "Expected record component name"),
            identifier(declaration->element(1), "Expected record component descriptor"));
        result->attributes() = pending_.takeGeneric();
        pending_.records.push_back(result);
        return remember(result);
    }

    error::ErrorCollector errors_;

    IdentifierPtr maybeIdentifier(const ElementPtr& value)
    {
        if (!value || value->getType() == ElementKind::EMPTY) return nullptr;
        return identifier(value, "Expected identifier");
    }
};
}   //namespace

error::Result<std::vector<ast::ElementPtr>> ASTProcessor::process(
    const std::vector<ast::ElementPtr>& input) const
{
    return Context{}.process(input);
}

}   //namespace jhasm::parser
