#pragma once

#include "jblw/annotation/annotation.hpp"
#include "jblw/code/code.hpp"
#include "jblw/constant/constant.hpp"
#include "jblw/constantpool/constant_pool.hpp"
#include "jblw/type/method_type.hpp"
#include "jblw/version/java_version.hpp"

#include <jbytecode/core/opcodes.hpp>

#include <libca/str/utf8_string.hpp>

#include <map>
#include <optional>
#include <utility>

namespace jblw::classfile {

///@brief JVM access flag 及 BLW 认可的 class/member mask。
struct AccessFlag final : jbytecode::core::Opcodes
{
    static constexpr ca::i32 RECOGNIZED_CLASS_MODIFIERS = ACC_PUBLIC | ACC_FINAL | ACC_SUPER |
                                                          ACC_INTERFACE | ACC_ABSTRACT |
                                                          ACC_ANNOTATION | ACC_ENUM | ACC_SYNTHETIC;
    static constexpr ca::i32 RECOGNIZED_FIELD_MODIFIERS = ACC_PUBLIC | ACC_PRIVATE | ACC_PROTECTED |
                                                          ACC_STATIC | ACC_FINAL | ACC_VOLATILE |
                                                          ACC_TRANSIENT | ACC_ENUM | ACC_SYNTHETIC;
    static constexpr ca::i32 RECOGNIZED_METHOD_MODIFIERS =
        ACC_PUBLIC | ACC_PRIVATE | ACC_PROTECTED | ACC_STATIC | ACC_FINAL | ACC_SYNCHRONIZED |
        ACC_BRIDGE | ACC_VARARGS | ACC_NATIVE | ACC_ABSTRACT | ACC_STRICT | ACC_SYNTHETIC;

    AccessFlag() = delete;
};

class Accessible
{
public:
    virtual ~Accessible()                        = default;
    virtual ca::i32 accessFlags() const noexcept = 0;
};

class Named
{
public:
    virtual ~Named()                                     = default;
    virtual ca::str::Utf8StringRef name() const noexcept = 0;
};

class Signed
{
public:
    virtual ~Signed()                                                            = default;
    virtual const std::optional<ca::str::Utf8String>& signature() const noexcept = 0;
};

template<typename T>
class Typed
{
public:
    virtual ~Typed()                                              = default;
    virtual const std::shared_ptr<const T>& type() const noexcept = 0;
};

class Annotated
{
public:
    virtual ~Annotated() = default;
    virtual const std::vector<annotation::AnnotationPtr>&
    visibleRuntimeAnnotations() const noexcept = 0;
    virtual const std::vector<annotation::AnnotationPtr>&
    invisibleRuntimeAnnotations() const noexcept = 0;
    virtual const std::vector<annotation::TypeAnnotationPtr>&
    visibleRuntimeTypeAnnotations() const noexcept = 0;
    virtual const std::vector<annotation::TypeAnnotationPtr>&
    invisibleRuntimeTypeAnnotations() const noexcept = 0;
};

///@brief member map 的 owning UTF-8 键。
class MemberIdentifier final
{
public:
    MemberIdentifier(ca::str::Utf8String name, ca::str::Utf8String descriptor);
    MemberIdentifier(ca::str::Utf8String name, const type::Type& type);

    ca::str::Utf8StringRef name() const noexcept { return name_.ref(); }
    ca::str::Utf8StringRef descriptor() const noexcept { return descriptor_.ref(); }
    bool                   operator==(const MemberIdentifier& other) const noexcept;

private:
    ca::str::Utf8String name_;
    ca::str::Utf8String descriptor_;
};

///@brief field/method 的公共只读接口。
template<typename T>
class Member : public Named, public Typed<T>, public Accessible, public Annotated, public Signed
{
public:
    MemberIdentifier identifier() const
    {
        return MemberIdentifier(
            ca::str::Utf8String::from_data(this->name().data(), this->name().byte_length()),
            *this->type());
    }
};

class Field : public virtual Member<type::ClassType>
{
public:
    virtual const constant::ConstantPtr& defaultValue() const noexcept = 0;
};
using FieldPtr = std::shared_ptr<const Field>;

namespace attribute {
class Parameter : public Accessible, public Named
{};
using ParameterPtr = std::shared_ptr<const Parameter>;

class InnerClass : public Accessible, public Typed<type::InstanceType>
{
public:
    virtual const type::InstanceTypePtr&              outerType() const noexcept = 0;
    virtual const std::optional<ca::str::Utf8String>& innerName() const noexcept = 0;
};
using InnerClassPtr = std::shared_ptr<const InnerClass>;

namespace generic {
class GenericParameter final : public Parameter
{
public:
    GenericParameter(ca::i32 accessFlags, ca::str::Utf8String name)
        : accessFlags_(accessFlags)
        , name_(std::move(name))
    {}
    ca::i32                accessFlags() const noexcept override { return accessFlags_; }
    ca::str::Utf8StringRef name() const noexcept override { return name_.ref(); }

private:
    ca::i32             accessFlags_;
    ca::str::Utf8String name_;
};

class GenericInnerClass final : public InnerClass
{
public:
    GenericInnerClass(ca::i32 accessFlags, type::InstanceTypePtr type,
                      type::InstanceTypePtr              outerType,
                      std::optional<ca::str::Utf8String> innerName);
    ca::i32                      accessFlags() const noexcept override { return accessFlags_; }
    const type::InstanceTypePtr& type() const noexcept override { return type_; }
    const type::InstanceTypePtr& outerType() const noexcept override { return outerType_; }
    const std::optional<ca::str::Utf8String>& innerName() const noexcept override
    {
        return innerName_;
    }

private:
    ca::i32                            accessFlags_;
    type::InstanceTypePtr              type_;
    type::InstanceTypePtr              outerType_;
    std::optional<ca::str::Utf8String> innerName_;
};
}   //namespace generic
}   //namespace attribute

using ParameterAnnotationMap = std::map<ca::i32, std::vector<annotation::AnnotationPtr>>;

class Method : public virtual Member<type::MethodType>
{
public:
    virtual const code::CodePtr&                        code() const noexcept                   = 0;
    virtual const std::vector<type::InstanceTypePtr>&   exceptionTypes() const noexcept         = 0;
    virtual const std::vector<attribute::ParameterPtr>& parameters() const noexcept             = 0;
    virtual const ParameterAnnotationMap& visibleRuntimeParameterAnnotations() const noexcept   = 0;
    virtual const ParameterAnnotationMap& invisibleRuntimeParameterAnnotations() const noexcept = 0;
    virtual const annotation::ElementPtr& annotationDefault() const noexcept                    = 0;
};
using MethodPtr = std::shared_ptr<const Method>;

class RecordComponent : public Named, public Typed<type::ClassType>, public Signed, public Annotated
{};
using RecordComponentPtr = std::shared_ptr<const RecordComponent>;

class ModuleRequire : public Accessible
{
public:
    virtual ca::str::Utf8StringRef                    module() const noexcept  = 0;
    virtual const std::optional<ca::str::Utf8String>& version() const noexcept = 0;
};
using ModuleRequirePtr = std::shared_ptr<const ModuleRequire>;

class ModuleExport : public Accessible
{
public:
    virtual ca::str::Utf8StringRef                  packageName() const noexcept = 0;
    virtual const std::vector<ca::str::Utf8String>& modules() const noexcept     = 0;
};
using ModuleExportPtr = std::shared_ptr<const ModuleExport>;

class ModuleOpen : public Accessible
{
public:
    virtual ca::str::Utf8StringRef                  packageName() const noexcept = 0;
    virtual const std::vector<ca::str::Utf8String>& modules() const noexcept     = 0;
};
using ModuleOpenPtr = std::shared_ptr<const ModuleOpen>;

class ModuleProvide
{
public:
    virtual ~ModuleProvide()                                                   = default;
    virtual ca::str::Utf8StringRef                  service() const noexcept   = 0;
    virtual const std::vector<ca::str::Utf8String>& providers() const noexcept = 0;
};
using ModuleProvidePtr = std::shared_ptr<const ModuleProvide>;

class Module : public Accessible, public Named
{
public:
    virtual const std::optional<ca::str::Utf8String>& version() const noexcept   = 0;
    virtual const std::optional<ca::str::Utf8String>& mainClass() const noexcept = 0;
    virtual const std::vector<ca::str::Utf8String>&   packages() const noexcept  = 0;
    virtual const std::vector<ModuleRequirePtr>&
        requires()
    const noexcept                                                            = 0;
    virtual const std::vector<ModuleExportPtr>&     exports() const noexcept  = 0;
    virtual const std::vector<ModuleOpenPtr>&       opens() const noexcept    = 0;
    virtual const std::vector<ModuleProvidePtr>&    provides() const noexcept = 0;
    virtual const std::vector<ca::str::Utf8String>& uses() const noexcept     = 0;
};
using ModulePtr = std::shared_ptr<const Module>;

class ClassFileView : public Accessible,
                      public Annotated,
                      public Signed,
                      public Typed<type::InstanceType>
{
public:
    virtual const constantpool::ConstantPoolPtr&         constantPool() const noexcept          = 0;
    virtual const type::InstanceTypePtr&                 superClass() const noexcept            = 0;
    virtual const std::vector<type::InstanceTypePtr>&    interfaces() const noexcept            = 0;
    virtual version::JavaVersion                         version() const noexcept               = 0;
    virtual const std::vector<MethodPtr>&                methods() const noexcept               = 0;
    virtual const std::vector<FieldPtr>&                 fields() const noexcept                = 0;
    virtual const std::vector<RecordComponentPtr>&       recordComponents() const noexcept      = 0;
    virtual const std::vector<attribute::InnerClassPtr>& innerClasses() const noexcept          = 0;
    virtual const std::optional<ca::str::Utf8String>&    outerClass() const noexcept            = 0;
    virtual const std::optional<ca::str::Utf8String>&    outerMethodName() const noexcept       = 0;
    virtual const std::optional<ca::str::Utf8String>&    outerMethodDescriptor() const noexcept = 0;
    virtual const std::vector<type::InstanceTypePtr>&    permittedSubclasses() const noexcept   = 0;
    virtual const type::InstanceTypePtr&                 nestHost() const noexcept              = 0;
    virtual const std::vector<type::InstanceTypePtr>&    nestMembers() const noexcept           = 0;
    virtual const std::optional<ca::str::Utf8String>&    sourceFile() const noexcept            = 0;
    virtual const std::optional<ca::str::Utf8String>&    sourceDebug() const noexcept           = 0;
    virtual const std::vector<ModulePtr>&                modules() const noexcept               = 0;

    MethodPtr method(ca::str::Utf8StringRef name, ca::str::Utf8StringRef descriptor) const noexcept;
    FieldPtr  field(ca::str::Utf8StringRef name, ca::str::Utf8StringRef descriptor) const noexcept;
};
using ClassFileViewPtr = std::shared_ptr<const ClassFileView>;

}   //namespace jblw::classfile
