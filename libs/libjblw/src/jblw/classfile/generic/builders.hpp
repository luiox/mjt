#pragma once

#include "models.hpp"

#include "jblw/annotation/builders.hpp"
#include "jblw/code/generic/code_model.hpp"

#include <functional>

namespace jblw::classfile::generic {

///@brief class/member/record builder 共用的 annotation 延迟构造状态。
class AnnotatedBuilderState
{
public:
    std::shared_ptr<annotation::GenericAnnotationBuilder> addVisibleRuntimeAnnotation(
        type::InstanceTypePtr type);
    std::shared_ptr<annotation::GenericAnnotationBuilder> addInvisibleRuntimeAnnotation(
        type::InstanceTypePtr type);
    std::shared_ptr<annotation::GenericTypeAnnotationBuilder> addVisibleRuntimeTypeAnnotation(
        type::InstanceTypePtr type, ca::i32 typeRef,
        std::optional<annotation::TypePath> typePath = std::nullopt);
    std::shared_ptr<annotation::GenericTypeAnnotationBuilder> addInvisibleRuntimeTypeAnnotation(
        type::InstanceTypePtr type, ca::i32 typeRef,
        std::optional<annotation::TypePath> typePath = std::nullopt);

    AnnotationLists buildAnnotations() const;

private:
    std::vector<std::shared_ptr<annotation::AnnotationBuilder>>     visible_;
    std::vector<std::shared_ptr<annotation::AnnotationBuilder>>     invisible_;
    std::vector<std::shared_ptr<annotation::TypeAnnotationBuilder>> visibleType_;
    std::vector<std::shared_ptr<annotation::TypeAnnotationBuilder>> invisibleType_;
};

template<typename T>
class GenericMemberBuilder
{
public:
    ca::i32               accessFlags() const noexcept { return accessFlags_; }
    GenericMemberBuilder& accessFlags(ca::i32 value) noexcept
    {
        accessFlags_ = value;
        return *this;
    }
    ca::str::Utf8StringRef name() const noexcept { return name_.ref(); }
    GenericMemberBuilder&  name(ca::str::Utf8String value)
    {
        name_ = std::move(value);
        return *this;
    }
    const std::shared_ptr<const T>& type() const noexcept { return type_; }
    GenericMemberBuilder&           type(std::shared_ptr<const T> value)
    {
        type_ = std::move(value);
        return *this;
    }
    const std::optional<ca::str::Utf8String>& signature() const noexcept { return signature_; }
    GenericMemberBuilder&                     signature(std::optional<ca::str::Utf8String> value)
    {
        signature_ = std::move(value);
        return *this;
    }
    AnnotatedBuilderState&       annotations() noexcept { return annotations_; }
    const AnnotatedBuilderState& annotations() const noexcept { return annotations_; }

protected:
    GenericMemberBuilder(ca::i32 accessFlags, ca::str::Utf8String name,
                         std::shared_ptr<const T> type)
        : accessFlags_(accessFlags)
        , name_(std::move(name))
        , type_(std::move(type))
    {}

    ca::i32                            accessFlags_;
    ca::str::Utf8String                name_;
    std::shared_ptr<const T>           type_;
    std::optional<ca::str::Utf8String> signature_;
    AnnotatedBuilderState              annotations_;
};

class GenericFieldBuilder final : public GenericMemberBuilder<type::ClassType>
{
public:
    GenericFieldBuilder(ca::i32 accessFlags, ca::str::Utf8String name, type::ClassTypePtr type)
        : GenericMemberBuilder(accessFlags, std::move(name), std::move(type))
    {}
    const constant::ConstantPtr& defaultValue() const noexcept { return defaultValue_; }
    GenericFieldBuilder&         defaultValue(constant::ConstantPtr value);
    FieldPtr                     build() const;

private:
    constant::ConstantPtr defaultValue_;
};

class GenericMethodBuilder final : public GenericMemberBuilder<type::MethodType>
{
public:
    GenericMethodBuilder(ca::i32 accessFlags, ca::str::Utf8String name, type::MethodTypePtr type)
        : GenericMemberBuilder(accessFlags, std::move(name), std::move(type))
    {}

    GenericMethodBuilder&     exceptionTypes(std::vector<type::InstanceTypePtr> values);
    GenericMethodBuilder&     exceptionType(type::InstanceTypePtr value);
    GenericMethodBuilder&     parameters(std::vector<attribute::ParameterPtr> values);
    GenericMethodBuilder&     parameter(attribute::ParameterPtr value);
    GenericMethodBuilder&     code(code::CodePtr value);
    code::GenericCodeBuilder& code();
    GenericMethodBuilder&     annotationDefault(annotation::ElementPtr value);
    std::shared_ptr<annotation::GenericAnnotationBuilder> addVisibleRuntimeParameterAnnotation(
        ca::i32 index, type::InstanceTypePtr type);
    std::shared_ptr<annotation::GenericAnnotationBuilder> addInvisibleRuntimeParameterAnnotation(
        ca::i32 index, type::InstanceTypePtr type);
    MethodPtr build() const;

private:
    using AnnotationBuilderMap =
        std::map<ca::i32, std::vector<std::shared_ptr<annotation::AnnotationBuilder>>>;
    static ParameterAnnotationMap buildParameterAnnotations(const AnnotationBuilderMap& builders);

    std::vector<type::InstanceTypePtr>        exceptionTypes_;
    std::vector<attribute::ParameterPtr>      parameters_;
    code::CodePtr                             code_;
    std::shared_ptr<code::GenericCodeBuilder> codeBuilder_;
    annotation::ElementPtr                    annotationDefault_;
    AnnotationBuilderMap                      visibleParameterAnnotations_;
    AnnotationBuilderMap                      invisibleParameterAnnotations_;
};

class GenericRecordComponentBuilder final
{
public:
    GenericRecordComponentBuilder(ca::str::Utf8String name, type::ClassTypePtr type,
                                  std::optional<ca::str::Utf8String> signature);
    ca::str::Utf8StringRef                    name() const noexcept { return name_.ref(); }
    GenericRecordComponentBuilder&            name(ca::str::Utf8String value);
    const type::ClassTypePtr&                 type() const noexcept { return type_; }
    GenericRecordComponentBuilder&            type(type::ClassTypePtr value);
    const std::optional<ca::str::Utf8String>& signature() const noexcept { return signature_; }
    GenericRecordComponentBuilder&            signature(std::optional<ca::str::Utf8String> value);
    AnnotatedBuilderState&                    annotations() noexcept { return annotations_; }
    RecordComponentPtr                        build() const;

private:
    ca::str::Utf8String                name_;
    type::ClassTypePtr                 type_;
    std::optional<ca::str::Utf8String> signature_;
    AnnotatedBuilderState              annotations_;
};

class GenericModuleBuilder final
{
public:
    GenericModuleBuilder(ca::str::Utf8String name, ca::i32 accessFlags,
                         std::optional<ca::str::Utf8String> version);
    GenericModuleBuilder& mainClass(std::optional<ca::str::Utf8String> value);
    GenericModuleBuilder& package(ca::str::Utf8String value);
    GenericModuleBuilder& require(ModuleRequirePtr value);
    GenericModuleBuilder& exportPackage(ModuleExportPtr value);
    GenericModuleBuilder& open(ModuleOpenPtr value);
    GenericModuleBuilder& provide(ModuleProvidePtr value);
    GenericModuleBuilder& use(ca::str::Utf8String value);
    ModulePtr             build() const;

private:
    ca::str::Utf8String                name_;
    ca::i32                            accessFlags_;
    std::optional<ca::str::Utf8String> version_;
    std::optional<ca::str::Utf8String> mainClass_;
    std::vector<ca::str::Utf8String>   packages_;
    std::vector<ModuleRequirePtr>      requires_;
    std::vector<ModuleExportPtr>       exports_;
    std::vector<ModuleOpenPtr>         opens_;
    std::vector<ModuleProvidePtr>      provides_;
    std::vector<ca::str::Utf8String>   uses_;
};

///@brief 保持 member/record/module 插入顺序的默认 class builder。
class GenericClassBuilder final
{
public:
    GenericClassBuilder&   accessFlags(ca::i32 value) noexcept;
    ca::i32                accessFlags() const noexcept { return accessFlags_; }
    GenericClassBuilder&   signature(std::optional<ca::str::Utf8String> value);
    GenericClassBuilder&   constantPool(constantpool::ConstantPoolPtr value);
    GenericClassBuilder&   type(type::InstanceTypePtr value);
    GenericClassBuilder&   superClass(type::InstanceTypePtr value);
    GenericClassBuilder&   addInterface(type::InstanceTypePtr value);
    GenericClassBuilder&   interfaces(std::vector<type::InstanceTypePtr> values);
    GenericClassBuilder&   version(version::JavaVersion value) noexcept;
    AnnotatedBuilderState& annotations() noexcept { return annotations_; }

    std::shared_ptr<GenericMethodBuilder> putMethod(ca::i32 accessFlags, ca::str::Utf8String name,
                                                    type::MethodTypePtr type);
    GenericClassBuilder&                  putMethod(MethodPtr method);
    std::shared_ptr<GenericFieldBuilder>  putField(ca::i32 accessFlags, ca::str::Utf8String name,
                                                   type::ClassTypePtr type);
    GenericClassBuilder&                  putField(FieldPtr field);
    std::shared_ptr<GenericRecordComponentBuilder> putRecordComponent(
        ca::str::Utf8String name, type::ClassTypePtr type,
        std::optional<ca::str::Utf8String> signature = std::nullopt);
    std::shared_ptr<GenericModuleBuilder> addModule(
        ca::str::Utf8String name, ca::i32 accessFlags,
        std::optional<ca::str::Utf8String> version = std::nullopt);

    GenericClassBuilder& innerClasses(std::vector<attribute::InnerClassPtr> values);
    GenericClassBuilder& innerClass(attribute::InnerClassPtr value);
    GenericClassBuilder& outerClass(std::optional<ca::str::Utf8String> owner);
    GenericClassBuilder& outerMethod(ca::str::Utf8String owner, ca::str::Utf8String name,
                                     ca::str::Utf8String descriptor);
    GenericClassBuilder& permittedSubclass(type::InstanceTypePtr value);
    GenericClassBuilder& nestHost(type::InstanceTypePtr value);
    GenericClassBuilder& nestMember(type::InstanceTypePtr value);
    GenericClassBuilder& sourceFile(std::optional<ca::str::Utf8String> value);
    GenericClassBuilder& sourceDebug(std::optional<ca::str::Utf8String> value);

    std::shared_ptr<GenericMethodBuilder> methodBuilder(const MemberIdentifier& id) const noexcept;
    std::shared_ptr<GenericFieldBuilder>  fieldBuilder(const MemberIdentifier& id) const noexcept;
    ClassFileViewPtr                      build() const;

private:
    template<typename Value, typename Builder>
    struct DeferredEntry
    {
        MemberIdentifier             id;
        std::shared_ptr<Builder>     builder;
        std::shared_ptr<const Value> value;

        std::shared_ptr<const Value> build() const { return builder ? builder->build() : value; }
    };

    std::vector<type::InstanceTypePtr>                                         interfaces_;
    AnnotatedBuilderState                                                      annotations_;
    std::vector<DeferredEntry<Method, GenericMethodBuilder>>                   methods_;
    std::vector<DeferredEntry<Field, GenericFieldBuilder>>                     fields_;
    std::vector<DeferredEntry<RecordComponent, GenericRecordComponentBuilder>> recordComponents_;
    std::vector<std::shared_ptr<GenericModuleBuilder>>                         modules_;
    std::vector<attribute::InnerClassPtr>                                      innerClasses_;
    std::vector<type::InstanceTypePtr>                                         permittedSubclasses_;
    std::vector<type::InstanceTypePtr>                                         nestMembers_;
    std::optional<ca::str::Utf8String>                                         outerClass_;
    std::optional<ca::str::Utf8String>                                         outerMethodName_;
    std::optional<ca::str::Utf8String>  outerMethodDescriptor_;
    type::InstanceTypePtr               nestHost_;
    std::optional<ca::str::Utf8String>  sourceFile_;
    std::optional<ca::str::Utf8String>  sourceDebug_;
    ca::i32                             accessFlags_ = 0;
    std::optional<ca::str::Utf8String>  signature_;
    constantpool::ConstantPoolPtr       pool_;
    type::InstanceTypePtr               type_;
    type::InstanceTypePtr               superClass_;
    std::optional<version::JavaVersion> version_;
};

}   //namespace jblw::classfile::generic

namespace jblw::classfile {
using ClassBuilder           = generic::GenericClassBuilder;
using FieldBuilder           = generic::GenericFieldBuilder;
using MethodBuilder          = generic::GenericMethodBuilder;
using ModuleBuilder          = generic::GenericModuleBuilder;
using RecordComponentBuilder = generic::GenericRecordComponentBuilder;
}   //namespace jblw::classfile
