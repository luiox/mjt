#pragma once

#include "primitive.hpp"

#include <map>
#include <optional>
#include <utility>

namespace jhasm::ast {

class Modifiers final
{
public:
    static bool isValid(ca::str::Utf16StringRef value) noexcept;
    void        add(IdentifierPtr value) { values_.push_back(std::move(value)); }
    bool        has(ca::str::Utf16StringRef value) const noexcept;
    const std::vector<IdentifierPtr>& values() const noexcept { return values_; }

private:
    std::vector<IdentifierPtr> values_;
};

class Annotation;
using AnnotationPtr = std::shared_ptr<Annotation>;

struct Attributes final
{
    std::shared_ptr<String>    signature;
    std::vector<AnnotationPtr> visibleAnnotations;
    std::vector<AnnotationPtr> invisibleAnnotations;
    std::vector<AnnotationPtr> visibleTypeAnnotations;
    std::vector<AnnotationPtr> invisibleTypeAnnotations;
};

class Annotated
{
public:
    virtual ~Annotated() = default;
    const Attributes& attributes() const noexcept { return attributes_; }
    Attributes&       attributes() noexcept { return attributes_; }

protected:
    Attributes attributes_;
};

class Annotation final : public Element
{
public:
    Annotation(bool visible, IdentifierPtr classType, std::vector<Object::Entry> values,
               std::shared_ptr<Number> typeRef = nullptr, IdentifierPtr typePath = nullptr);
    ElementKind          getType() const noexcept override { return ElementKind::ANNOTATION; }
    bool                 visible() const noexcept { return visible_; }
    bool                 isTypeAnnotation() const noexcept { return static_cast<bool>(typeRef_); }
    const IdentifierPtr& classType() const noexcept { return classType_; }
    const std::vector<Object::Entry>& values() const noexcept { return values_; }
    ElementPtr                        value(ca::str::Utf16StringRef name) const noexcept;
    const std::shared_ptr<Number>&    typeRef() const noexcept { return typeRef_; }
    const IdentifierPtr&              typePath() const noexcept { return typePath_; }

private:
    bool                       visible_;
    IdentifierPtr              classType_;
    std::vector<Object::Entry> values_;
    std::shared_ptr<Number>    typeRef_;
    IdentifierPtr              typePath_;
};

class EnumValue final : public Element
{
public:
    EnumValue(IdentifierPtr enumType, IdentifierPtr enumValue);
    ElementKind          getType() const noexcept override { return ElementKind::ENUM_VALUE; }
    const IdentifierPtr& enumType() const noexcept { return enumType_; }
    const IdentifierPtr& enumValue() const noexcept { return enumValue_; }

private:
    IdentifierPtr enumType_;
    IdentifierPtr enumValue_;
};

class Exception final : public Element
{
public:
    Exception(IdentifierPtr start, IdentifierPtr end, IdentifierPtr handler, IdentifierPtr type);
    ElementKind          getType() const noexcept override { return ElementKind::EXCEPTION; }
    const IdentifierPtr& start() const noexcept { return start_; }
    const IdentifierPtr& end() const noexcept { return end_; }
    const IdentifierPtr& handler() const noexcept { return handler_; }
    const IdentifierPtr& exceptionType() const noexcept { return type_; }

private:
    IdentifierPtr start_;
    IdentifierPtr end_;
    IdentifierPtr handler_;
    IdentifierPtr type_;
};

class InnerClass final : public Element
{
public:
    InnerClass(Modifiers modifiers, IdentifierPtr name, IdentifierPtr outer, IdentifierPtr inner);
    ElementKind          getType() const noexcept override { return ElementKind::INNER_CLASS; }
    const Modifiers&     modifiers() const noexcept { return modifiers_; }
    const IdentifierPtr& name() const noexcept { return name_; }
    const IdentifierPtr& outerClass() const noexcept { return outer_; }
    const IdentifierPtr& innerClass() const noexcept { return inner_; }

private:
    Modifiers     modifiers_;
    IdentifierPtr name_;
    IdentifierPtr outer_;
    IdentifierPtr inner_;
};

class RecordComponent final : public Element, public Annotated
{
public:
    RecordComponent(IdentifierPtr name, IdentifierPtr descriptor);
    ElementKind          getType() const noexcept override { return ElementKind::RECORD_COMPONENT; }
    const IdentifierPtr& name() const noexcept { return name_; }
    const IdentifierPtr& descriptor() const noexcept { return descriptor_; }

private:
    IdentifierPtr name_;
    IdentifierPtr descriptor_;
};

class OuterMethod final : public Element
{
public:
    OuterMethod(IdentifierPtr name, IdentifierPtr descriptor);
    ElementKind          getType() const noexcept override { return ElementKind::OUTER_METHOD; }
    const IdentifierPtr& name() const noexcept { return name_; }
    const IdentifierPtr& descriptor() const noexcept { return descriptor_; }

private:
    IdentifierPtr name_;
    IdentifierPtr descriptor_;
};

class Member : public Element, public Annotated
{
public:
    const Modifiers&     modifiers() const noexcept { return modifiers_; }
    const IdentifierPtr& name() const noexcept { return name_; }
    const IdentifierPtr& descriptor() const noexcept { return descriptor_; }

protected:
    Member(Modifiers modifiers, IdentifierPtr name, IdentifierPtr descriptor,
           std::vector<ElementPtr> children);

private:
    Modifiers     modifiers_;
    IdentifierPtr name_;
    IdentifierPtr descriptor_;
};

class Field final : public Member
{
public:
    Field(Modifiers modifiers, IdentifierPtr name, IdentifierPtr descriptor, ElementPtr value);
    ElementKind       getType() const noexcept override { return ElementKind::FIELD; }
    const ElementPtr& fieldValue() const noexcept { return value_; }

private:
    ElementPtr value_;
};

using ParameterAnnotations = std::map<ca::str::Utf16String, std::vector<AnnotationPtr>>;

class Method final : public Member
{
public:
    Method(Modifiers modifiers, IdentifierPtr name, IdentifierPtr descriptor,
           std::vector<IdentifierPtr> parameters, ParameterAnnotations parameterAnnotations,
           ElementPtr defaultValue, std::vector<std::shared_ptr<Exception>> exceptions,
           std::shared_ptr<Code> code);
    ElementKind getType() const noexcept override { return ElementKind::METHOD; }
    const std::vector<IdentifierPtr>& parameters() const noexcept { return parameters_; }
    const ParameterAnnotations&       parameterAnnotations() const noexcept
    {
        return parameterAnnotations_;
    }
    const ElementPtr& defaultValue() const noexcept { return defaultValue_; }
    const std::vector<std::shared_ptr<Exception>>& exceptions() const noexcept
    {
        return exceptions_;
    }
    const std::shared_ptr<Code>& code() const noexcept { return code_; }

private:
    std::vector<IdentifierPtr>              parameters_;
    ParameterAnnotations                    parameterAnnotations_;
    ElementPtr                              defaultValue_;
    std::vector<std::shared_ptr<Exception>> exceptions_;
    std::shared_ptr<Code>                   code_;
};

class Class final : public Member
{
public:
    Class(Modifiers modifiers, IdentifierPtr name, std::vector<ElementPtr> contents);
    ElementKind                    getType() const noexcept override { return ElementKind::CLASS; }
    const std::vector<ElementPtr>& contents() const noexcept { return contents_; }
    const IdentifierPtr&           superName() const noexcept { return superName_; }
    const std::vector<IdentifierPtr>& interfaces() const noexcept { return interfaces_; }
    const std::vector<IdentifierPtr>& permittedSubclasses() const noexcept { return permitted_; }
    const std::vector<std::shared_ptr<InnerClass>>& innerClasses() const noexcept
    {
        return inners_;
    }
    const std::vector<std::shared_ptr<RecordComponent>>& recordComponents() const noexcept
    {
        return records_;
    }
    const std::shared_ptr<String>&      sourceFile() const noexcept { return sourceFile_; }
    const IdentifierPtr&                outerClass() const noexcept { return outerClass_; }
    const std::shared_ptr<OuterMethod>& outerMethod() const noexcept { return outerMethod_; }
    const IdentifierPtr&                nestHost() const noexcept { return nestHost_; }
    const std::vector<IdentifierPtr>&   nestMembers() const noexcept { return nestMembers_; }

    void superName(IdentifierPtr value) { superName_ = std::move(value); }
    void addInterface(IdentifierPtr value) { interfaces_.push_back(std::move(value)); }
    void addPermittedSubclass(IdentifierPtr value) { permitted_.push_back(std::move(value)); }
    void addInnerClass(std::shared_ptr<InnerClass> value) { inners_.push_back(std::move(value)); }
    void addRecordComponent(std::shared_ptr<RecordComponent> value)
    {
        records_.push_back(std::move(value));
    }
    void sourceFile(std::shared_ptr<String> value) { sourceFile_ = std::move(value); }
    void outerClass(IdentifierPtr value) { outerClass_ = std::move(value); }
    void outerMethod(std::shared_ptr<OuterMethod> value) { outerMethod_ = std::move(value); }
    void nestHost(IdentifierPtr value) { nestHost_ = std::move(value); }
    void addNestMember(IdentifierPtr value) { nestMembers_.push_back(std::move(value)); }

private:
    std::vector<ElementPtr>                       contents_;
    IdentifierPtr                                 superName_;
    std::vector<IdentifierPtr>                    interfaces_;
    std::vector<IdentifierPtr>                    permitted_;
    std::vector<std::shared_ptr<InnerClass>>      inners_;
    std::vector<std::shared_ptr<RecordComponent>> records_;
    std::shared_ptr<String>                       sourceFile_;
    IdentifierPtr                                 outerClass_;
    std::shared_ptr<OuterMethod>                  outerMethod_;
    IdentifierPtr                                 nestHost_;
    std::vector<IdentifierPtr>                    nestMembers_;
};

}   //namespace jhasm::ast
