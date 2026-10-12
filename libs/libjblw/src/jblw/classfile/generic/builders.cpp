#include "builders.hpp"

#include <stdexcept>

namespace jblw::classfile::generic {
namespace {
void ensure(bool condition, const char* message)
{
    if (!condition) throw std::invalid_argument(message);
}

ca::str::Utf8String copyString(const ca::str::Utf8String& value)
{
    return ca::str::Utf8String::from_data(value.data(), value.byte_length());
}

std::optional<ca::str::Utf8String> copyString(const std::optional<ca::str::Utf8String>& value)
{
    return value ? std::optional<ca::str::Utf8String>(copyString(*value)) : std::nullopt;
}

std::vector<ca::str::Utf8String> copyStrings(const std::vector<ca::str::Utf8String>& values)
{
    std::vector<ca::str::Utf8String> result;
    result.reserve(values.size());
    for (const auto& value : values) result.push_back(copyString(value));
    return result;
}
}   //namespace

std::shared_ptr<annotation::GenericAnnotationBuilder>
AnnotatedBuilderState::addVisibleRuntimeAnnotation(type::InstanceTypePtr type)
{
    auto builder = std::make_shared<annotation::GenericAnnotationBuilder>(std::move(type));
    visible_.push_back(builder);
    return builder;
}

std::shared_ptr<annotation::GenericAnnotationBuilder>
AnnotatedBuilderState::addInvisibleRuntimeAnnotation(type::InstanceTypePtr type)
{
    auto builder = std::make_shared<annotation::GenericAnnotationBuilder>(std::move(type));
    invisible_.push_back(builder);
    return builder;
}

std::shared_ptr<annotation::GenericTypeAnnotationBuilder>
AnnotatedBuilderState::addVisibleRuntimeTypeAnnotation(type::InstanceTypePtr type, ca::i32 typeRef,
                                                       std::optional<annotation::TypePath> typePath)
{
    auto builder = std::make_shared<annotation::GenericTypeAnnotationBuilder>(
        std::move(type), typeRef, std::move(typePath));
    visibleType_.push_back(builder);
    return builder;
}

std::shared_ptr<annotation::GenericTypeAnnotationBuilder>
AnnotatedBuilderState::addInvisibleRuntimeTypeAnnotation(
    type::InstanceTypePtr type, ca::i32 typeRef, std::optional<annotation::TypePath> typePath)
{
    auto builder = std::make_shared<annotation::GenericTypeAnnotationBuilder>(
        std::move(type), typeRef, std::move(typePath));
    invisibleType_.push_back(builder);
    return builder;
}

AnnotationLists AnnotatedBuilderState::buildAnnotations() const
{
    AnnotationLists result;
    result.visible.reserve(visible_.size());
    for (const auto& builder : visible_) result.visible.push_back(builder->build());
    result.invisible.reserve(invisible_.size());
    for (const auto& builder : invisible_) result.invisible.push_back(builder->build());
    result.visibleType.reserve(visibleType_.size());
    for (const auto& builder : visibleType_) {
        result.visibleType.push_back(
            std::static_pointer_cast<const annotation::TypeAnnotation>(builder->build()));
    }
    result.invisibleType.reserve(invisibleType_.size());
    for (const auto& builder : invisibleType_) {
        result.invisibleType.push_back(
            std::static_pointer_cast<const annotation::TypeAnnotation>(builder->build()));
    }
    return result;
}

GenericFieldBuilder& GenericFieldBuilder::defaultValue(constant::ConstantPtr value)
{
    defaultValue_ = std::move(value);
    return *this;
}

FieldPtr GenericFieldBuilder::build() const
{
    ensure(static_cast<bool>(type_), "Field builder type must not be null");
    return std::make_shared<GenericField>(accessFlags_,
                                          copyString(name_),
                                          copyString(signature_),
                                          annotations_.buildAnnotations(),
                                          type_,
                                          defaultValue_);
}

GenericMethodBuilder& GenericMethodBuilder::exceptionTypes(
    std::vector<type::InstanceTypePtr> values)
{
    exceptionTypes_ = std::move(values);
    return *this;
}

GenericMethodBuilder& GenericMethodBuilder::exceptionType(type::InstanceTypePtr value)
{
    ensure(static_cast<bool>(value), "Exception type must not be null");
    exceptionTypes_.push_back(std::move(value));
    return *this;
}

GenericMethodBuilder& GenericMethodBuilder::parameters(std::vector<attribute::ParameterPtr> values)
{
    parameters_ = std::move(values);
    return *this;
}

GenericMethodBuilder& GenericMethodBuilder::parameter(attribute::ParameterPtr value)
{
    ensure(static_cast<bool>(value), "Method parameter must not be null");
    parameters_.push_back(std::move(value));
    return *this;
}

GenericMethodBuilder& GenericMethodBuilder::code(code::CodePtr value)
{
    code_ = std::move(value);
    codeBuilder_.reset();
    return *this;
}

code::GenericCodeBuilder& GenericMethodBuilder::code()
{
    if (!codeBuilder_) codeBuilder_ = std::make_shared<code::GenericCodeBuilder>();
    code_.reset();
    return *codeBuilder_;
}

GenericMethodBuilder& GenericMethodBuilder::annotationDefault(annotation::ElementPtr value)
{
    annotationDefault_ = std::move(value);
    return *this;
}

std::shared_ptr<annotation::GenericAnnotationBuilder>
GenericMethodBuilder::addVisibleRuntimeParameterAnnotation(ca::i32               index,
                                                           type::InstanceTypePtr type)
{
    auto builder = std::make_shared<annotation::GenericAnnotationBuilder>(std::move(type));
    visibleParameterAnnotations_[index].push_back(builder);
    return builder;
}

std::shared_ptr<annotation::GenericAnnotationBuilder>
GenericMethodBuilder::addInvisibleRuntimeParameterAnnotation(ca::i32               index,
                                                             type::InstanceTypePtr type)
{
    auto builder = std::make_shared<annotation::GenericAnnotationBuilder>(std::move(type));
    invisibleParameterAnnotations_[index].push_back(builder);
    return builder;
}

ParameterAnnotationMap GenericMethodBuilder::buildParameterAnnotations(
    const AnnotationBuilderMap& builders)
{
    ParameterAnnotationMap result;
    for (const auto& [index, values] : builders) {
        auto& annotations = result[index];
        annotations.reserve(values.size());
        for (const auto& value : values) annotations.push_back(value->build());
    }
    return result;
}

MethodPtr GenericMethodBuilder::build() const
{
    ensure(static_cast<bool>(type_), "Method builder type must not be null");
    code::CodePtr code = codeBuilder_ ? codeBuilder_->build() : code_;
    return std::make_shared<GenericMethod>(
        accessFlags_,
        copyString(name_),
        copyString(signature_),
        annotations_.buildAnnotations(),
        buildParameterAnnotations(visibleParameterAnnotations_),
        buildParameterAnnotations(invisibleParameterAnnotations_),
        type_,
        std::move(code),
        exceptionTypes_,
        parameters_,
        annotationDefault_);
}

GenericRecordComponentBuilder::GenericRecordComponentBuilder(
    ca::str::Utf8String name, type::ClassTypePtr type, std::optional<ca::str::Utf8String> signature)
    : name_(std::move(name))
    , type_(std::move(type))
    , signature_(std::move(signature))
{}

GenericRecordComponentBuilder& GenericRecordComponentBuilder::name(ca::str::Utf8String value)
{
    name_ = std::move(value);
    return *this;
}

GenericRecordComponentBuilder& GenericRecordComponentBuilder::type(type::ClassTypePtr value)
{
    type_ = std::move(value);
    return *this;
}

GenericRecordComponentBuilder& GenericRecordComponentBuilder::signature(
    std::optional<ca::str::Utf8String> value)
{
    signature_ = std::move(value);
    return *this;
}

RecordComponentPtr GenericRecordComponentBuilder::build() const
{
    ensure(static_cast<bool>(type_), "Record component builder type must not be null");
    return std::make_shared<GenericRecordComponent>(
        copyString(name_), copyString(signature_), type_, annotations_.buildAnnotations());
}

GenericModuleBuilder::GenericModuleBuilder(ca::str::Utf8String name, ca::i32 accessFlags,
                                           std::optional<ca::str::Utf8String> version)
    : name_(std::move(name))
    , accessFlags_(accessFlags)
    , version_(std::move(version))
{}

GenericModuleBuilder& GenericModuleBuilder::mainClass(std::optional<ca::str::Utf8String> value)
{
    mainClass_ = std::move(value);
    return *this;
}

GenericModuleBuilder& GenericModuleBuilder::package(ca::str::Utf8String value)
{
    packages_.push_back(std::move(value));
    return *this;
}

GenericModuleBuilder& GenericModuleBuilder::require(ModuleRequirePtr value)
{
    ensure(static_cast<bool>(value), "Module require must not be null");
    requires_.push_back(std::move(value));
    return *this;
}

GenericModuleBuilder& GenericModuleBuilder::exportPackage(ModuleExportPtr value)
{
    ensure(static_cast<bool>(value), "Module export must not be null");
    exports_.push_back(std::move(value));
    return *this;
}

GenericModuleBuilder& GenericModuleBuilder::open(ModuleOpenPtr value)
{
    ensure(static_cast<bool>(value), "Module open must not be null");
    opens_.push_back(std::move(value));
    return *this;
}

GenericModuleBuilder& GenericModuleBuilder::provide(ModuleProvidePtr value)
{
    ensure(static_cast<bool>(value), "Module provide must not be null");
    provides_.push_back(std::move(value));
    return *this;
}

GenericModuleBuilder& GenericModuleBuilder::use(ca::str::Utf8String value)
{
    uses_.push_back(std::move(value));
    return *this;
}

ModulePtr GenericModuleBuilder::build() const
{
    return std::make_shared<GenericModule>(copyString(name_),
                                           accessFlags_,
                                           copyString(version_),
                                           copyString(mainClass_),
                                           copyStrings(packages_),
                                           requires_,
                                           exports_,
                                           opens_,
                                           provides_,
                                           copyStrings(uses_));
}

GenericClassBuilder& GenericClassBuilder::accessFlags(ca::i32 value) noexcept
{
    accessFlags_ = value;
    return *this;
}

GenericClassBuilder& GenericClassBuilder::signature(std::optional<ca::str::Utf8String> value)
{
    signature_ = std::move(value);
    return *this;
}

GenericClassBuilder& GenericClassBuilder::constantPool(constantpool::ConstantPoolPtr value)
{
    pool_ = std::move(value);
    return *this;
}

GenericClassBuilder& GenericClassBuilder::type(type::InstanceTypePtr value)
{
    type_ = std::move(value);
    return *this;
}

GenericClassBuilder& GenericClassBuilder::superClass(type::InstanceTypePtr value)
{
    superClass_ = std::move(value);
    return *this;
}

GenericClassBuilder& GenericClassBuilder::addInterface(type::InstanceTypePtr value)
{
    ensure(static_cast<bool>(value), "Interface type must not be null");
    interfaces_.push_back(std::move(value));
    return *this;
}

GenericClassBuilder& GenericClassBuilder::interfaces(std::vector<type::InstanceTypePtr> values)
{
    interfaces_ = std::move(values);
    return *this;
}

GenericClassBuilder& GenericClassBuilder::version(version::JavaVersion value) noexcept
{
    version_ = value;
    return *this;
}

std::shared_ptr<GenericMethodBuilder> GenericClassBuilder::putMethod(ca::i32 accessFlags,
                                                                     ca::str::Utf8String name,
                                                                     type::MethodTypePtr type)
{
    MemberIdentifier id(copyString(name), *type);
    auto             builder =
        std::make_shared<GenericMethodBuilder>(accessFlags, std::move(name), std::move(type));
    for (auto& entry : methods_) {
        if (entry.id == id) {
            entry.builder = builder;
            entry.value.reset();
            return builder;
        }
    }
    methods_.push_back({std::move(id), builder, nullptr});
    return builder;
}

GenericClassBuilder& GenericClassBuilder::putMethod(MethodPtr method)
{
    ensure(static_cast<bool>(method), "Method must not be null");
    MemberIdentifier id = method->identifier();
    for (auto& entry : methods_) {
        if (entry.id == id) {
            entry.builder.reset();
            entry.value = std::move(method);
            return *this;
        }
    }
    methods_.push_back({std::move(id), nullptr, std::move(method)});
    return *this;
}

std::shared_ptr<GenericFieldBuilder> GenericClassBuilder::putField(ca::i32             accessFlags,
                                                                   ca::str::Utf8String name,
                                                                   type::ClassTypePtr  type)
{
    MemberIdentifier id(copyString(name), *type);
    auto             builder =
        std::make_shared<GenericFieldBuilder>(accessFlags, std::move(name), std::move(type));
    for (auto& entry : fields_) {
        if (entry.id == id) {
            entry.builder = builder;
            entry.value.reset();
            return builder;
        }
    }
    fields_.push_back({std::move(id), builder, nullptr});
    return builder;
}

GenericClassBuilder& GenericClassBuilder::putField(FieldPtr field)
{
    ensure(static_cast<bool>(field), "Field must not be null");
    MemberIdentifier id = field->identifier();
    for (auto& entry : fields_) {
        if (entry.id == id) {
            entry.builder.reset();
            entry.value = std::move(field);
            return *this;
        }
    }
    fields_.push_back({std::move(id), nullptr, std::move(field)});
    return *this;
}

std::shared_ptr<GenericRecordComponentBuilder> GenericClassBuilder::putRecordComponent(
    ca::str::Utf8String name, type::ClassTypePtr type, std::optional<ca::str::Utf8String> signature)
{
    MemberIdentifier id(copyString(name), *type);
    auto             builder = std::make_shared<GenericRecordComponentBuilder>(
        std::move(name), std::move(type), std::move(signature));
    for (auto& entry : recordComponents_) {
        if (entry.id == id) {
            entry.builder = builder;
            entry.value.reset();
            return builder;
        }
    }
    recordComponents_.push_back({std::move(id), builder, nullptr});
    return builder;
}

std::shared_ptr<GenericModuleBuilder> GenericClassBuilder::addModule(
    ca::str::Utf8String name, ca::i32 accessFlags, std::optional<ca::str::Utf8String> version)
{
    auto builder =
        std::make_shared<GenericModuleBuilder>(std::move(name), accessFlags, std::move(version));
    modules_.push_back(builder);
    return builder;
}

GenericClassBuilder& GenericClassBuilder::innerClasses(std::vector<attribute::InnerClassPtr> values)
{
    innerClasses_ = std::move(values);
    return *this;
}

GenericClassBuilder& GenericClassBuilder::innerClass(attribute::InnerClassPtr value)
{
    ensure(static_cast<bool>(value), "Inner class must not be null");
    innerClasses_.push_back(std::move(value));
    return *this;
}

GenericClassBuilder& GenericClassBuilder::outerClass(std::optional<ca::str::Utf8String> owner)
{
    outerClass_ = std::move(owner);
    outerMethodName_.reset();
    outerMethodDescriptor_.reset();
    return *this;
}

GenericClassBuilder& GenericClassBuilder::outerMethod(ca::str::Utf8String owner,
                                                      ca::str::Utf8String name,
                                                      ca::str::Utf8String descriptor)
{
    outerClass_            = std::move(owner);
    outerMethodName_       = std::move(name);
    outerMethodDescriptor_ = std::move(descriptor);
    return *this;
}

GenericClassBuilder& GenericClassBuilder::permittedSubclass(type::InstanceTypePtr value)
{
    ensure(static_cast<bool>(value), "Permitted subclass must not be null");
    permittedSubclasses_.push_back(std::move(value));
    return *this;
}

GenericClassBuilder& GenericClassBuilder::nestHost(type::InstanceTypePtr value)
{
    nestHost_ = std::move(value);
    return *this;
}

GenericClassBuilder& GenericClassBuilder::nestMember(type::InstanceTypePtr value)
{
    ensure(static_cast<bool>(value), "Nest member must not be null");
    nestMembers_.push_back(std::move(value));
    return *this;
}

GenericClassBuilder& GenericClassBuilder::sourceFile(std::optional<ca::str::Utf8String> value)
{
    sourceFile_ = std::move(value);
    return *this;
}

GenericClassBuilder& GenericClassBuilder::sourceDebug(std::optional<ca::str::Utf8String> value)
{
    sourceDebug_ = std::move(value);
    return *this;
}

std::shared_ptr<GenericMethodBuilder> GenericClassBuilder::methodBuilder(
    const MemberIdentifier& id) const noexcept
{
    for (const auto& entry : methods_) {
        if (entry.id == id) return entry.builder;
    }
    return nullptr;
}

std::shared_ptr<GenericFieldBuilder> GenericClassBuilder::fieldBuilder(
    const MemberIdentifier& id) const noexcept
{
    for (const auto& entry : fields_) {
        if (entry.id == id) return entry.builder;
    }
    return nullptr;
}

ClassFileViewPtr GenericClassBuilder::build() const
{
    ensure(version_.has_value(), "Class version must be set");
    ensure(static_cast<bool>(type_), "Class type must be set");

    std::vector<MethodPtr> methods;
    methods.reserve(methods_.size());
    for (const auto& entry : methods_) methods.push_back(entry.build());
    std::vector<FieldPtr> fields;
    fields.reserve(fields_.size());
    for (const auto& entry : fields_) fields.push_back(entry.build());
    std::vector<RecordComponentPtr> records;
    records.reserve(recordComponents_.size());
    for (const auto& entry : recordComponents_) records.push_back(entry.build());
    std::vector<ModulePtr> modules;
    modules.reserve(modules_.size());
    for (const auto& builder : modules_) modules.push_back(builder->build());

    return std::make_shared<GenericClassFileView>(*version_,
                                                  pool_,
                                                  accessFlags_,
                                                  type_,
                                                  superClass_,
                                                  copyString(signature_),
                                                  interfaces_,
                                                  std::move(methods),
                                                  std::move(fields),
                                                  std::move(records),
                                                  innerClasses_,
                                                  copyString(outerClass_),
                                                  copyString(outerMethodName_),
                                                  copyString(outerMethodDescriptor_),
                                                  permittedSubclasses_,
                                                  nestHost_,
                                                  nestMembers_,
                                                  copyString(sourceFile_),
                                                  copyString(sourceDebug_),
                                                  annotations_.buildAnnotations(),
                                                  std::move(modules));
}

}   //namespace jblw::classfile::generic
