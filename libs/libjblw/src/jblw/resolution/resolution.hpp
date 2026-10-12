#pragma once

#include "jblw/classfile/model.hpp"
#include "jblw/type/object_type.hpp"

#include <libca/core/result.hpp>

#include <memory>
#include <vector>

namespace jblw::resolution {

class MemberInfo : public classfile::Accessible
{};

class MethodInfo : public MemberInfo
{};

class FieldInfo : public MemberInfo
{};

class ClassInfoView : public classfile::Accessible
{
public:
    virtual const type::ObjectTypePtr& type() const noexcept = 0;
};
using ClassInfoViewPtr = std::shared_ptr<const ClassInfoView>;

template<typename M, typename F>
class ClassInfo;

template<typename M, typename F>
using ClassInfoPtr = std::shared_ptr<const ClassInfo<M, F>>;

///@brief 解析器使用的最小类层次视图，可由 classfile、VM 元数据或测试模型实现。
template<typename M, typename F>
class ClassInfo : public ClassInfoView
{
public:
    virtual const ClassInfoPtr<M, F>&              superClass() const noexcept = 0;
    virtual const std::vector<ClassInfoPtr<M, F>>& interfaces() const noexcept = 0;
    virtual std::shared_ptr<const M>               getMethod(ca::str::Utf8StringRef  name,
                                                             const type::MethodType& type) const noexcept = 0;
    virtual std::shared_ptr<const F>               getField(ca::str::Utf8StringRef name,
                                                            const type::ClassType& type) const noexcept = 0;
};

enum class ResolutionError : ca::u8
{
    ACC_INTERFACE_SET,
    ACC_ABSTRACT_SET,
    ACC_STATIC_SET,
    ACC_INTERFACE_UNSET,
    ACC_ABSTRACT_UNSET,
    ACC_STATIC_UNSET,
    NO_SUCH_METHOD,
    NO_SUCH_FIELD,
};

template<typename M>
class ResolutionSuccess final
{
public:
    ResolutionSuccess(ClassInfoViewPtr owner, std::shared_ptr<const M> member,
                      bool isForced) noexcept
        : owner_(std::move(owner))
        , member_(std::move(member))
        , isForced_(isForced)
    {}

    const ClassInfoViewPtr&         owner() const noexcept { return owner_; }
    const std::shared_ptr<const M>& member() const noexcept { return member_; }
    bool                            isForced() const noexcept { return isForced_; }

private:
    ClassInfoViewPtr         owner_;
    std::shared_ptr<const M> member_;
    bool                     isForced_;
};

template<typename M>
using ResolutionResult = ca::Result<ResolutionSuccess<M>, ResolutionError>;

template<typename M, typename F>
class LinkResolver
{
public:
    virtual ~LinkResolver()                                                              = default;
    virtual ResolutionResult<M> resolveVirtualMethod(const ClassInfoPtr<M, F>& info,
                                                     ca::str::Utf8StringRef    name,
                                                     const type::MethodType&   type) const = 0;
    virtual ResolutionResult<M> resolveSpecialMethod(const ClassInfoPtr<M, F>& info,
                                                     ca::str::Utf8StringRef    name,
                                                     const type::MethodType&   type,
                                                     bool isInterface) const             = 0;
    virtual ResolutionResult<M> resolveStaticMethod(const ClassInfoPtr<M, F>& info,
                                                    ca::str::Utf8StringRef    name,
                                                    const type::MethodType&   type,
                                                    bool isInterface) const              = 0;
    virtual ResolutionResult<M> resolveInterfaceMethod(const ClassInfoPtr<M, F>& info,
                                                       ca::str::Utf8StringRef    name,
                                                       const type::MethodType&   type) const = 0;
    virtual ResolutionResult<F> resolveStaticField(const ClassInfoPtr<M, F>& info,
                                                   ca::str::Utf8StringRef    name,
                                                   const type::ClassType&    type) const      = 0;
    virtual ResolutionResult<F> resolveVirtualField(const ClassInfoPtr<M, F>& info,
                                                    ca::str::Utf8StringRef    name,
                                                    const type::ClassType&    type) const     = 0;
};

template<typename M, typename F>
using LinkResolverPtr = std::shared_ptr<const LinkResolver<M, F>>;

template<typename M, typename F>
class RuntimeResolver : public LinkResolver<M, F>
{};

template<typename M, typename F>
using RuntimeResolverPtr = std::shared_ptr<const RuntimeResolver<M, F>>;

}   //namespace jblw::resolution
