#pragma once

#include "jblw/classfile/model.hpp"

namespace jblw::classfile::generic {

struct AnnotationLists final
{
    std::vector<annotation::AnnotationPtr>     visible;
    std::vector<annotation::AnnotationPtr>     invisible;
    std::vector<annotation::TypeAnnotationPtr> visibleType;
    std::vector<annotation::TypeAnnotationPtr> invisibleType;
};

template<typename Interface>
class GenericMember : public Interface
{
public:
    ca::i32                accessFlags() const noexcept final { return accessFlags_; }
    ca::str::Utf8StringRef name() const noexcept final { return name_.ref(); }
    const std::optional<ca::str::Utf8String>& signature() const noexcept final
    {
        return signature_;
    }
    const std::vector<annotation::AnnotationPtr>& visibleRuntimeAnnotations() const noexcept final
    {
        return annotations_.visible;
    }
    const std::vector<annotation::AnnotationPtr>& invisibleRuntimeAnnotations() const noexcept final
    {
        return annotations_.invisible;
    }
    const std::vector<annotation::TypeAnnotationPtr>&
    visibleRuntimeTypeAnnotations() const noexcept final
    {
        return annotations_.visibleType;
    }
    const std::vector<annotation::TypeAnnotationPtr>&
    invisibleRuntimeTypeAnnotations() const noexcept final
    {
        return annotations_.invisibleType;
    }

protected:
    GenericMember(ca::i32 accessFlags, ca::str::Utf8String name,
                  std::optional<ca::str::Utf8String> signature, AnnotationLists annotations)
        : accessFlags_(accessFlags)
        , name_(std::move(name))
        , signature_(std::move(signature))
        , annotations_(std::move(annotations))
    {}

private:
    ca::i32                            accessFlags_;
    ca::str::Utf8String                name_;
    std::optional<ca::str::Utf8String> signature_;
    AnnotationLists                    annotations_;
};

class GenericField final : public GenericMember<Field>
{
public:
    GenericField(ca::i32 accessFlags, ca::str::Utf8String name,
                 std::optional<ca::str::Utf8String> signature, AnnotationLists annotations,
                 type::ClassTypePtr type, constant::ConstantPtr defaultValue);
    const type::ClassTypePtr&    type() const noexcept override { return type_; }
    const constant::ConstantPtr& defaultValue() const noexcept override { return defaultValue_; }

private:
    type::ClassTypePtr    type_;
    constant::ConstantPtr defaultValue_;
};

class GenericMethod final : public GenericMember<Method>
{
public:
    GenericMethod(ca::i32 accessFlags, ca::str::Utf8String name,
                  std::optional<ca::str::Utf8String> signature, AnnotationLists annotations,
                  ParameterAnnotationMap visibleParameterAnnotations,
                  ParameterAnnotationMap invisibleParameterAnnotations, type::MethodTypePtr type,
                  code::CodePtr code, std::vector<type::InstanceTypePtr> exceptionTypes,
                  std::vector<attribute::ParameterPtr> parameters,
                  annotation::ElementPtr               annotationDefault);
    const type::MethodTypePtr&                type() const noexcept override { return type_; }
    const code::CodePtr&                      code() const noexcept override { return code_; }
    const std::vector<type::InstanceTypePtr>& exceptionTypes() const noexcept override
    {
        return exceptionTypes_;
    }
    const std::vector<attribute::ParameterPtr>& parameters() const noexcept override
    {
        return parameters_;
    }
    const ParameterAnnotationMap& visibleRuntimeParameterAnnotations() const noexcept override
    {
        return visibleParameterAnnotations_;
    }
    const ParameterAnnotationMap& invisibleRuntimeParameterAnnotations() const noexcept override
    {
        return invisibleParameterAnnotations_;
    }
    const annotation::ElementPtr& annotationDefault() const noexcept override
    {
        return annotationDefault_;
    }

private:
    ParameterAnnotationMap               visibleParameterAnnotations_;
    ParameterAnnotationMap               invisibleParameterAnnotations_;
    type::MethodTypePtr                  type_;
    code::CodePtr                        code_;
    std::vector<type::InstanceTypePtr>   exceptionTypes_;
    std::vector<attribute::ParameterPtr> parameters_;
    annotation::ElementPtr               annotationDefault_;
};

class GenericRecordComponent final : public RecordComponent
{
public:
    GenericRecordComponent(ca::str::Utf8String name, std::optional<ca::str::Utf8String> signature,
                           type::ClassTypePtr type, AnnotationLists annotations);
    ca::str::Utf8StringRef                    name() const noexcept override { return name_.ref(); }
    const std::optional<ca::str::Utf8String>& signature() const noexcept override
    {
        return signature_;
    }
    const type::ClassTypePtr& type() const noexcept override { return type_; }
    const std::vector<annotation::AnnotationPtr>&
    visibleRuntimeAnnotations() const noexcept override
    {
        return annotations_.visible;
    }
    const std::vector<annotation::AnnotationPtr>&
    invisibleRuntimeAnnotations() const noexcept override
    {
        return annotations_.invisible;
    }
    const std::vector<annotation::TypeAnnotationPtr>&
    visibleRuntimeTypeAnnotations() const noexcept override
    {
        return annotations_.visibleType;
    }
    const std::vector<annotation::TypeAnnotationPtr>&
    invisibleRuntimeTypeAnnotations() const noexcept override
    {
        return annotations_.invisibleType;
    }

private:
    ca::str::Utf8String                name_;
    std::optional<ca::str::Utf8String> signature_;
    type::ClassTypePtr                 type_;
    AnnotationLists                    annotations_;
};

class GenericModuleRequire final : public ModuleRequire
{
public:
    GenericModuleRequire(ca::i32 accessFlags, ca::str::Utf8String module,
                         std::optional<ca::str::Utf8String> version)
        : accessFlags_(accessFlags)
        , module_(std::move(module))
        , version_(std::move(version))
    {}
    ca::i32                accessFlags() const noexcept override { return accessFlags_; }
    ca::str::Utf8StringRef module() const noexcept override { return module_.ref(); }
    const std::optional<ca::str::Utf8String>& version() const noexcept override { return version_; }

private:
    ca::i32                            accessFlags_;
    ca::str::Utf8String                module_;
    std::optional<ca::str::Utf8String> version_;
};

class GenericModuleExport final : public ModuleExport
{
public:
    GenericModuleExport(ca::i32 accessFlags, std::vector<ca::str::Utf8String> modules,
                        ca::str::Utf8String packageName)
        : accessFlags_(accessFlags)
        , modules_(std::move(modules))
        , packageName_(std::move(packageName))
    {}
    ca::i32                accessFlags() const noexcept override { return accessFlags_; }
    ca::str::Utf8StringRef packageName() const noexcept override { return packageName_.ref(); }
    const std::vector<ca::str::Utf8String>& modules() const noexcept override { return modules_; }

private:
    ca::i32                          accessFlags_;
    std::vector<ca::str::Utf8String> modules_;
    ca::str::Utf8String              packageName_;
};

class GenericModuleOpen final : public ModuleOpen
{
public:
    GenericModuleOpen(ca::i32 accessFlags, ca::str::Utf8String packageName,
                      std::vector<ca::str::Utf8String> modules)
        : accessFlags_(accessFlags)
        , packageName_(std::move(packageName))
        , modules_(std::move(modules))
    {}
    ca::i32                accessFlags() const noexcept override { return accessFlags_; }
    ca::str::Utf8StringRef packageName() const noexcept override { return packageName_.ref(); }
    const std::vector<ca::str::Utf8String>& modules() const noexcept override { return modules_; }

private:
    ca::i32                          accessFlags_;
    ca::str::Utf8String              packageName_;
    std::vector<ca::str::Utf8String> modules_;
};

class GenericModuleProvide final : public ModuleProvide
{
public:
    GenericModuleProvide(ca::str::Utf8String service, std::vector<ca::str::Utf8String> providers)
        : service_(std::move(service))
        , providers_(std::move(providers))
    {}
    ca::str::Utf8StringRef service() const noexcept override { return service_.ref(); }
    const std::vector<ca::str::Utf8String>& providers() const noexcept override
    {
        return providers_;
    }

private:
    ca::str::Utf8String              service_;
    std::vector<ca::str::Utf8String> providers_;
};

class GenericModule final : public Module
{
public:
    GenericModule(ca::str::Utf8String name, ca::i32 accessFlags,
                  std::optional<ca::str::Utf8String> version,
                  std::optional<ca::str::Utf8String> mainClass,
                  std::vector<ca::str::Utf8String> packages, std::vector<ModuleRequirePtr> requires,
                  std::vector<ModuleExportPtr> exports, std::vector<ModuleOpenPtr> opens,
                  std::vector<ModuleProvidePtr> provides, std::vector<ca::str::Utf8String> uses);
    ca::str::Utf8StringRef name() const noexcept override { return name_.ref(); }
    ca::i32                accessFlags() const noexcept override { return accessFlags_; }
    const std::optional<ca::str::Utf8String>& version() const noexcept override { return version_; }
    const std::optional<ca::str::Utf8String>& mainClass() const noexcept override
    {
        return mainClass_;
    }
    const std::vector<ca::str::Utf8String>& packages() const noexcept override { return packages_; }
    const std::vector<ModuleRequirePtr>&
        requires()
    const noexcept override
    {
        return requires_;
    }
    const std::vector<ModuleExportPtr>&     exports() const noexcept override { return exports_; }
    const std::vector<ModuleOpenPtr>&       opens() const noexcept override { return opens_; }
    const std::vector<ModuleProvidePtr>&    provides() const noexcept override { return provides_; }
    const std::vector<ca::str::Utf8String>& uses() const noexcept override { return uses_; }

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

class GenericClassFileView final : public ClassFileView
{
public:
    GenericClassFileView(version::JavaVersion version, constantpool::ConstantPoolPtr pool,
                         ca::i32 accessFlags, type::InstanceTypePtr type,
                         type::InstanceTypePtr              superClass,
                         std::optional<ca::str::Utf8String> signature,
                         std::vector<type::InstanceTypePtr> interfaces,
                         std::vector<MethodPtr> methods, std::vector<FieldPtr> fields,
                         std::vector<RecordComponentPtr>       recordComponents,
                         std::vector<attribute::InnerClassPtr> innerClasses,
                         std::optional<ca::str::Utf8String>    outerClass,
                         std::optional<ca::str::Utf8String>    outerMethodName,
                         std::optional<ca::str::Utf8String>    outerMethodDescriptor,
                         std::vector<type::InstanceTypePtr>    permittedSubclasses,
                         type::InstanceTypePtr                 nestHost,
                         std::vector<type::InstanceTypePtr>    nestMembers,
                         std::optional<ca::str::Utf8String>    sourceFile,
                         std::optional<ca::str::Utf8String>    sourceDebug,
                         AnnotationLists annotations, std::vector<ModulePtr> modules);

    version::JavaVersion                 version() const noexcept override { return version_; }
    const constantpool::ConstantPoolPtr& constantPool() const noexcept override { return pool_; }
    ca::i32                      accessFlags() const noexcept override { return accessFlags_; }
    const type::InstanceTypePtr& type() const noexcept override { return type_; }
    const type::InstanceTypePtr& superClass() const noexcept override { return superClass_; }
    const std::optional<ca::str::Utf8String>& signature() const noexcept override
    {
        return signature_;
    }
    const std::vector<type::InstanceTypePtr>& interfaces() const noexcept override
    {
        return interfaces_;
    }
    const std::vector<MethodPtr>&          methods() const noexcept override { return methods_; }
    const std::vector<FieldPtr>&           fields() const noexcept override { return fields_; }
    const std::vector<RecordComponentPtr>& recordComponents() const noexcept override
    {
        return recordComponents_;
    }
    const std::vector<attribute::InnerClassPtr>& innerClasses() const noexcept override
    {
        return innerClasses_;
    }
    const std::optional<ca::str::Utf8String>& outerClass() const noexcept override
    {
        return outerClass_;
    }
    const std::optional<ca::str::Utf8String>& outerMethodName() const noexcept override
    {
        return outerMethodName_;
    }
    const std::optional<ca::str::Utf8String>& outerMethodDescriptor() const noexcept override
    {
        return outerMethodDescriptor_;
    }
    const std::vector<type::InstanceTypePtr>& permittedSubclasses() const noexcept override
    {
        return permittedSubclasses_;
    }
    const type::InstanceTypePtr& nestHost() const noexcept override { return nestHost_; }
    const std::vector<type::InstanceTypePtr>& nestMembers() const noexcept override
    {
        return nestMembers_;
    }
    const std::optional<ca::str::Utf8String>& sourceFile() const noexcept override
    {
        return sourceFile_;
    }
    const std::optional<ca::str::Utf8String>& sourceDebug() const noexcept override
    {
        return sourceDebug_;
    }
    const std::vector<annotation::AnnotationPtr>&
    visibleRuntimeAnnotations() const noexcept override
    {
        return annotations_.visible;
    }
    const std::vector<annotation::AnnotationPtr>&
    invisibleRuntimeAnnotations() const noexcept override
    {
        return annotations_.invisible;
    }
    const std::vector<annotation::TypeAnnotationPtr>&
    visibleRuntimeTypeAnnotations() const noexcept override
    {
        return annotations_.visibleType;
    }
    const std::vector<annotation::TypeAnnotationPtr>&
    invisibleRuntimeTypeAnnotations() const noexcept override
    {
        return annotations_.invisibleType;
    }
    const std::vector<ModulePtr>& modules() const noexcept override { return modules_; }

private:
    version::JavaVersion                  version_;
    constantpool::ConstantPoolPtr         pool_;
    ca::i32                               accessFlags_;
    type::InstanceTypePtr                 type_;
    type::InstanceTypePtr                 superClass_;
    std::optional<ca::str::Utf8String>    signature_;
    std::vector<type::InstanceTypePtr>    interfaces_;
    std::vector<MethodPtr>                methods_;
    std::vector<FieldPtr>                 fields_;
    std::vector<RecordComponentPtr>       recordComponents_;
    std::vector<attribute::InnerClassPtr> innerClasses_;
    std::optional<ca::str::Utf8String>    outerClass_;
    std::optional<ca::str::Utf8String>    outerMethodName_;
    std::optional<ca::str::Utf8String>    outerMethodDescriptor_;
    std::vector<type::InstanceTypePtr>    permittedSubclasses_;
    type::InstanceTypePtr                 nestHost_;
    std::vector<type::InstanceTypePtr>    nestMembers_;
    std::optional<ca::str::Utf8String>    sourceFile_;
    std::optional<ca::str::Utf8String>    sourceDebug_;
    AnnotationLists                       annotations_;
    std::vector<ModulePtr>                modules_;
};

}   //namespace jblw::classfile::generic
