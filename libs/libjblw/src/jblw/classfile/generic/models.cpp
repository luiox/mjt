#include "models.hpp"

#include <stdexcept>

namespace jblw::classfile::generic {
namespace {
void require(bool condition, const char* message)
{
    if (!condition) throw std::invalid_argument(message);
}
}   //namespace

GenericField::GenericField(ca::i32 accessFlags, ca::str::Utf8String name,
                           std::optional<ca::str::Utf8String> signature,
                           AnnotationLists annotations, type::ClassTypePtr type,
                           constant::ConstantPtr defaultValue)
    : GenericMember(accessFlags, std::move(name), std::move(signature), std::move(annotations))
    , type_(std::move(type))
    , defaultValue_(std::move(defaultValue))
{
    require(static_cast<bool>(type_), "Field type must not be null");
}

GenericMethod::GenericMethod(
    ca::i32 accessFlags, ca::str::Utf8String name, std::optional<ca::str::Utf8String> signature,
    AnnotationLists annotations, ParameterAnnotationMap visibleParameterAnnotations,
    ParameterAnnotationMap invisibleParameterAnnotations, type::MethodTypePtr type,
    code::CodePtr code, std::vector<type::InstanceTypePtr> exceptionTypes,
    std::vector<attribute::ParameterPtr> parameters, annotation::ElementPtr annotationDefault)
    : GenericMember(accessFlags, std::move(name), std::move(signature), std::move(annotations))
    , visibleParameterAnnotations_(std::move(visibleParameterAnnotations))
    , invisibleParameterAnnotations_(std::move(invisibleParameterAnnotations))
    , type_(std::move(type))
    , code_(std::move(code))
    , exceptionTypes_(std::move(exceptionTypes))
    , parameters_(std::move(parameters))
    , annotationDefault_(std::move(annotationDefault))
{
    require(static_cast<bool>(type_), "Method type must not be null");
}

GenericRecordComponent::GenericRecordComponent(ca::str::Utf8String                name,
                                               std::optional<ca::str::Utf8String> signature,
                                               type::ClassTypePtr type, AnnotationLists annotations)
    : name_(std::move(name))
    , signature_(std::move(signature))
    , type_(std::move(type))
    , annotations_(std::move(annotations))
{
    require(static_cast<bool>(type_), "Record component type must not be null");
}

GenericModule::GenericModule(ca::str::Utf8String name, ca::i32 accessFlags,
                             std::optional<ca::str::Utf8String> version,
                             std::optional<ca::str::Utf8String> mainClass,
                             std::vector<ca::str::Utf8String>   packages,
                             std::vector<ModuleRequirePtr> requires,
                             std::vector<ModuleExportPtr> exports, std::vector<ModuleOpenPtr> opens,
                             std::vector<ModuleProvidePtr>    provides,
                             std::vector<ca::str::Utf8String> uses)
    : name_(std::move(name))
    , accessFlags_(accessFlags)
    , version_(std::move(version))
    , mainClass_(std::move(mainClass))
    , packages_(std::move(packages))
    , requires_(std::move(requires))
    , exports_(std::move(exports))
    , opens_(std::move(opens))
    , provides_(std::move(provides))
    , uses_(std::move(uses))
{}

GenericClassFileView::GenericClassFileView(
    version::JavaVersion version, constantpool::ConstantPoolPtr pool, ca::i32 accessFlags,
    type::InstanceTypePtr type, type::InstanceTypePtr superClass,
    std::optional<ca::str::Utf8String> signature, std::vector<type::InstanceTypePtr> interfaces,
    std::vector<MethodPtr> methods, std::vector<FieldPtr> fields,
    std::vector<RecordComponentPtr>       recordComponents,
    std::vector<attribute::InnerClassPtr> innerClasses,
    std::optional<ca::str::Utf8String>    outerClass,
    std::optional<ca::str::Utf8String>    outerMethodName,
    std::optional<ca::str::Utf8String>    outerMethodDescriptor,
    std::vector<type::InstanceTypePtr> permittedSubclasses, type::InstanceTypePtr nestHost,
    std::vector<type::InstanceTypePtr> nestMembers, std::optional<ca::str::Utf8String> sourceFile,
    std::optional<ca::str::Utf8String> sourceDebug, AnnotationLists annotations,
    std::vector<ModulePtr> modules)
    : version_(version)
    , pool_(std::move(pool))
    , accessFlags_(accessFlags)
    , type_(std::move(type))
    , superClass_(std::move(superClass))
    , signature_(std::move(signature))
    , interfaces_(std::move(interfaces))
    , methods_(std::move(methods))
    , fields_(std::move(fields))
    , recordComponents_(std::move(recordComponents))
    , innerClasses_(std::move(innerClasses))
    , outerClass_(std::move(outerClass))
    , outerMethodName_(std::move(outerMethodName))
    , outerMethodDescriptor_(std::move(outerMethodDescriptor))
    , permittedSubclasses_(std::move(permittedSubclasses))
    , nestHost_(std::move(nestHost))
    , nestMembers_(std::move(nestMembers))
    , sourceFile_(std::move(sourceFile))
    , sourceDebug_(std::move(sourceDebug))
    , annotations_(std::move(annotations))
    , modules_(std::move(modules))
{
    require(static_cast<bool>(type_), "Class type must not be null");
}

}   //namespace jblw::classfile::generic
