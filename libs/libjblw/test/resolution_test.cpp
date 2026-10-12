#include "jblw/resolution/jvm_resolvers.hpp"
#include "jblw/type/types.hpp"

#include <gtest/gtest.h>

using namespace jblw::classfile;
using namespace jblw::resolution;
using namespace jblw::type;

namespace {
class TestMethod final : public Accessible
{
public:
    explicit TestMethod(ca::i32 flags) noexcept
        : flags_(flags)
    {}
    ca::i32 accessFlags() const noexcept override { return flags_; }

private:
    ca::i32 flags_;
};

class TestField final : public Accessible
{
public:
    explicit TestField(ca::i32 flags) noexcept
        : flags_(flags)
    {}
    ca::i32 accessFlags() const noexcept override { return flags_; }

private:
    ca::i32 flags_;
};

class TestClass final : public ClassInfo<TestMethod, TestField>
{
public:
    TestClass(const char* name, ca::i32 flags)
        : type_(Types::instanceTypeFromInternalName(ca::str::Utf8StringRef::from_cstr(name)))
        , flags_(flags)
    {}

    const ObjectTypePtr& type() const noexcept override { return type_; }
    ca::i32              accessFlags() const noexcept override { return flags_; }
    const ClassInfoPtr<TestMethod, TestField>& superClass() const noexcept override
    {
        return super_;
    }
    const std::vector<ClassInfoPtr<TestMethod, TestField>>& interfaces() const noexcept override
    {
        return interfaces_;
    }
    std::shared_ptr<const TestMethod> getMethod(ca::str::Utf8StringRef name,
                                                const MethodType&      type) const noexcept override
    {
        for (const auto& entry : methods_) {
            if (entry.name == name && entry.type->equals(type)) return entry.value;
        }
        return nullptr;
    }
    std::shared_ptr<const TestField> getField(ca::str::Utf8StringRef name,
                                              const ClassType&       type) const noexcept override
    {
        for (const auto& entry : fields_) {
            if (entry.name == name && entry.type->equals(type)) return entry.value;
        }
        return nullptr;
    }

    TestClass& superClass(ClassInfoPtr<TestMethod, TestField> value)
    {
        super_ = std::move(value);
        return *this;
    }
    TestClass& addInterface(ClassInfoPtr<TestMethod, TestField> value)
    {
        interfaces_.push_back(std::move(value));
        return *this;
    }
    std::shared_ptr<const TestMethod> addMethod(const char* name, MethodTypePtr type, ca::i32 flags)
    {
        auto value = std::make_shared<const TestMethod>(flags);
        methods_.push_back({ca::str::Utf8String(name), std::move(type), value});
        return value;
    }
    std::shared_ptr<const TestField> addField(const char* name, ClassTypePtr type, ca::i32 flags)
    {
        auto value = std::make_shared<const TestField>(flags);
        fields_.push_back({ca::str::Utf8String(name), std::move(type), value});
        return value;
    }

private:
    struct MethodEntry final
    {
        ca::str::Utf8String               name;
        MethodTypePtr                     type;
        std::shared_ptr<const TestMethod> value;
    };
    struct FieldEntry final
    {
        ca::str::Utf8String              name;
        ClassTypePtr                     type;
        std::shared_ptr<const TestField> value;
    };

    ObjectTypePtr                                    type_;
    ca::i32                                          flags_;
    ClassInfoPtr<TestMethod, TestField>              super_;
    std::vector<ClassInfoPtr<TestMethod, TestField>> interfaces_;
    std::vector<MethodEntry>                         methods_;
    std::vector<FieldEntry>                          fields_;
};

using Resolver = JVMLinkResolver<TestMethod, TestField>;

ca::str::Utf8StringRef name(const char* value)
{
    return ca::str::Utf8StringRef::from_cstr(value);
}
}   //namespace

TEST(BlwResolutionTest, ResolvesClassMembersAndChecksInvocationMode)
{
    auto object = std::make_shared<TestClass>("java/lang/Object", AccessFlag::ACC_PUBLIC);
    auto base   = std::make_shared<TestClass>("example/Base", AccessFlag::ACC_PUBLIC);
    auto child  = std::make_shared<TestClass>("example/Child", AccessFlag::ACC_PUBLIC);
    base->superClass(object);
    child->superClass(base);

    auto virtualMethod =
        base->addMethod("run", Types::methodType(Types::VOID), AccessFlag::ACC_PUBLIC);
    auto staticMethod = base->addMethod(
        "create", Types::methodType(Types::VOID), AccessFlag::ACC_PUBLIC | AccessFlag::ACC_STATIC);
    auto field = base->addField("value", Types::INT, AccessFlag::ACC_PUBLIC);
    auto staticField =
        base->addField("count", Types::INT, AccessFlag::ACC_PUBLIC | AccessFlag::ACC_STATIC);
    Resolver resolver;

    auto virtualResult =
        resolver.resolveVirtualMethod(child, name("run"), *Types::methodType(Types::VOID));
    ASSERT_TRUE(virtualResult.is_ok());
    EXPECT_EQ(virtualMethod, virtualResult.unwrap().member());
    EXPECT_EQ(base, virtualResult.unwrap().owner());

    auto wrongVirtual =
        resolver.resolveVirtualMethod(child, name("create"), *Types::methodType(Types::VOID));
    ASSERT_TRUE(wrongVirtual.is_err());
    EXPECT_EQ(ResolutionError::ACC_STATIC_SET, wrongVirtual.unwrap_err());
    auto staticResult =
        resolver.resolveStaticMethod(child, name("create"), *Types::methodType(Types::VOID), false);
    ASSERT_TRUE(staticResult.is_ok());
    EXPECT_EQ(staticMethod, staticResult.unwrap().member());

    auto fieldResult = resolver.resolveVirtualField(child, name("value"), *Types::INT);
    ASSERT_TRUE(fieldResult.is_ok());
    EXPECT_EQ(field, fieldResult.unwrap().member());
    auto staticFieldResult = resolver.resolveStaticField(child, name("count"), *Types::INT);
    ASSERT_TRUE(staticFieldResult.is_ok());
    EXPECT_EQ(staticField, staticFieldResult.unwrap().member());
}

TEST(BlwResolutionTest, PrefersDefaultInterfaceMethodAndFallsBackToObject)
{
    auto object = std::make_shared<TestClass>("java/lang/Object", AccessFlag::ACC_PUBLIC);
    auto hash =
        object->addMethod("hashCode", Types::methodType(Types::INT), AccessFlag::ACC_PUBLIC);
    auto abstractInterface = std::make_shared<TestClass>(
        "example/Abstract",
        AccessFlag::ACC_PUBLIC | AccessFlag::ACC_INTERFACE | AccessFlag::ACC_ABSTRACT);
    abstractInterface->superClass(object);
    abstractInterface->addMethod(
        "run", Types::methodType(Types::VOID), AccessFlag::ACC_PUBLIC | AccessFlag::ACC_ABSTRACT);
    auto defaultInterface = std::make_shared<TestClass>(
        "example/Default",
        AccessFlag::ACC_PUBLIC | AccessFlag::ACC_INTERFACE | AccessFlag::ACC_ABSTRACT);
    defaultInterface->superClass(object);
    auto defaultMethod =
        defaultInterface->addMethod("run", Types::methodType(Types::VOID), AccessFlag::ACC_PUBLIC);
    auto implementation = std::make_shared<TestClass>("example/Impl", AccessFlag::ACC_PUBLIC);
    implementation->superClass(object)
        .addInterface(abstractInterface)
        .addInterface(defaultInterface);
    Resolver resolver;

    auto run =
        resolver.resolveVirtualMethod(implementation, name("run"), *Types::methodType(Types::VOID));
    ASSERT_TRUE(run.is_ok());
    EXPECT_EQ(defaultMethod, run.unwrap().member());
    EXPECT_EQ(defaultInterface, run.unwrap().owner());

    auto hashResult = resolver.resolveInterfaceMethod(
        abstractInterface, name("hashCode"), *Types::methodType(Types::INT));
    ASSERT_TRUE(hashResult.is_ok());
    EXPECT_EQ(hash, hashResult.unwrap().member());
    EXPECT_TRUE(hashResult.unwrap().isForced());
}

TEST(BlwResolutionTest, RuntimeResolverRejectsAbstractTargets)
{
    auto owner = std::make_shared<TestClass>("example/Abstract",
                                             AccessFlag::ACC_PUBLIC | AccessFlag::ACC_ABSTRACT);
    owner->addMethod(
        "run", Types::methodType(Types::VOID), AccessFlag::ACC_PUBLIC | AccessFlag::ACC_ABSTRACT);
    JVMRuntimeResolver<TestMethod, TestField> resolver;

    auto result =
        resolver.resolveVirtualMethod(owner, name("run"), *Types::methodType(Types::VOID));
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(ResolutionError::ACC_ABSTRACT_SET, result.unwrap_err());
}

TEST(BlwResolutionTest, InterfaceLookupTerminatesOnCycles)
{
    auto first  = std::make_shared<TestClass>("example/First",
                                             AccessFlag::ACC_INTERFACE | AccessFlag::ACC_ABSTRACT);
    auto second = std::make_shared<TestClass>("example/Second",
                                              AccessFlag::ACC_INTERFACE | AccessFlag::ACC_ABSTRACT);
    first->addInterface(second);
    second->addInterface(first);
    Resolver resolver;

    auto result =
        resolver.resolveInterfaceMethod(first, name("missing"), *Types::methodType(Types::VOID));
    ASSERT_TRUE(result.is_err());
    EXPECT_EQ(ResolutionError::NO_SUCH_METHOD, result.unwrap_err());
}
