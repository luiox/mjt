#pragma once

#include "resolution.hpp"

#include <deque>
#include <unordered_set>

namespace jblw::resolution {

template<typename M, typename F>
class JVMLinkResolver final : public LinkResolver<M, F>
{
public:
    ResolutionResult<M> resolveVirtualMethod(const ClassInfoPtr<M, F>& info,
                                             ca::str::Utf8StringRef    name,
                                             const type::MethodType&   type) const override
    {
        if (hasFlag(info->accessFlags(), classfile::AccessFlag::ACC_INTERFACE)) {
            return ca::Err(ResolutionError::ACC_INTERFACE_SET);
        }
        auto method = lookupMethod(info, name, type);
        if (!method) method = interfaceMethod(info, name, type);
        return checkVirtualMethod(std::move(method));
    }

    ResolutionResult<M> resolveSpecialMethod(const ClassInfoPtr<M, F>& info,
                                             ca::str::Utf8StringRef    name,
                                             const type::MethodType&   type,
                                             bool                      isInterface) const override
    {
        const bool ownerIsInterface =
            hasFlag(info->accessFlags(), classfile::AccessFlag::ACC_INTERFACE);
        if (ownerIsInterface != isInterface) {
            return ca::Err(isInterface ? ResolutionError::ACC_INTERFACE_UNSET
                                       : ResolutionError::ACC_INTERFACE_SET);
        }
        return checkVirtualMethod(isInterface ? interfaceMethod(info, name, type)
                                              : lookupMethod(info, name, type));
    }

    ResolutionResult<M> resolveStaticMethod(const ClassInfoPtr<M, F>& info,
                                            ca::str::Utf8StringRef    name,
                                            const type::MethodType&   type,
                                            bool                      isInterface) const override
    {
        if (isInterface && !hasFlag(info->accessFlags(), classfile::AccessFlag::ACC_INTERFACE)) {
            return ca::Err(ResolutionError::ACC_INTERFACE_UNSET);
        }
        auto method =
            isInterface ? interfaceMethod(info, name, type) : lookupMethod(info, name, type);
        if (!method) return ca::Err(ResolutionError::NO_SUCH_METHOD);
        if (!hasFlag(method->member()->accessFlags(), classfile::AccessFlag::ACC_STATIC)) {
            return ca::Err(ResolutionError::ACC_STATIC_UNSET);
        }
        const bool resolvedOwnerIsInterface =
            hasFlag(method->owner()->accessFlags(), classfile::AccessFlag::ACC_INTERFACE);
        if (resolvedOwnerIsInterface != isInterface) {
            return ca::Err(ResolutionError::ACC_INTERFACE_UNSET);
        }
        return ca::Ok(*method);
    }

    ResolutionResult<M> resolveInterfaceMethod(const ClassInfoPtr<M, F>& info,
                                               ca::str::Utf8StringRef    name,
                                               const type::MethodType&   type) const override
    {
        if (!hasFlag(info->accessFlags(), classfile::AccessFlag::ACC_INTERFACE)) {
            return ca::Err(ResolutionError::ACC_INTERFACE_UNSET);
        }
        auto method = interfaceMethod(info, name, type);
        if (!method) return ca::Err(ResolutionError::NO_SUCH_METHOD);
        if (hasFlag(method->member()->accessFlags(), classfile::AccessFlag::ACC_STATIC)) {
            return ca::Err(ResolutionError::ACC_STATIC_SET);
        }
        return ca::Ok(*method);
    }

    ResolutionResult<F> resolveStaticField(const ClassInfoPtr<M, F>& info,
                                           ca::str::Utf8StringRef    name,
                                           const type::ClassType&    type) const override
    {
        ClassInfoPtr<M, F> current = info;
        while (current) {
            auto field = current->getField(name, type);
            if (field) {
                if (!hasFlag(field->accessFlags(), classfile::AccessFlag::ACC_STATIC)) {
                    return ca::Err(ResolutionError::ACC_STATIC_UNSET);
                }
                return ca::Ok(ResolutionSuccess<F>(current, std::move(field), false));
            }
            current = current->superClass();
        }
        auto field = interfaceLookup<F, type::ClassType>(
            info,
            name,
            type,
            false,
            [](const ClassInfoPtr<M, F>& owner,
               ca::str::Utf8StringRef    fieldName,
               const type::ClassType& fieldType) { return owner->getField(fieldName, fieldType); });
        if (!field) return ca::Err(ResolutionError::NO_SUCH_FIELD);
        if (!hasFlag(field->member()->accessFlags(), classfile::AccessFlag::ACC_STATIC)) {
            return ca::Err(ResolutionError::ACC_STATIC_UNSET);
        }
        return ca::Ok(*field);
    }

    ResolutionResult<F> resolveVirtualField(const ClassInfoPtr<M, F>& info,
                                            ca::str::Utf8StringRef    name,
                                            const type::ClassType&    type) const override
    {
        ClassInfoPtr<M, F> current = info;
        while (current) {
            auto field = current->getField(name, type);
            if (field) {
                if (hasFlag(field->accessFlags(), classfile::AccessFlag::ACC_STATIC)) {
                    return ca::Err(ResolutionError::ACC_STATIC_SET);
                }
                return ca::Ok(ResolutionSuccess<F>(current, std::move(field), false));
            }
            current = current->superClass();
        }
        return ca::Err(ResolutionError::NO_SUCH_FIELD);
    }

    std::shared_ptr<ResolutionSuccess<M>> lookupMethod(const ClassInfoPtr<M, F>& info,
                                                       ca::str::Utf8StringRef    name,
                                                       const type::MethodType&   type) const
    {
        ClassInfoPtr<M, F> current = info;
        while (current) {
            auto method = current->getMethod(name, type);
            if (method) {
                return std::make_shared<ResolutionSuccess<M>>(current, std::move(method), false);
            }
            current = current->superClass();
        }
        return nullptr;
    }

    std::shared_ptr<ResolutionSuccess<M>> interfaceMethod(const ClassInfoPtr<M, F>& info,
                                                          ca::str::Utf8StringRef    name,
                                                          const type::MethodType&   type) const
    {
        auto resolution = interfaceLookup<M, type::MethodType>(
            info,
            name,
            type,
            true,
            [](const ClassInfoPtr<M, F>& owner,
               ca::str::Utf8StringRef    methodName,
               const type::MethodType&   methodType) {
                return owner->getMethod(methodName, methodType);
            });
        if (resolution) return resolution;

        ClassInfoPtr<M, F> root = info->superClass();
        while (root && root->superClass()) root = root->superClass();
        if (!root) return nullptr;
        auto method = root->getMethod(name, type);
        if (method) {
            const ca::i32 flags = method->accessFlags();
            if (hasFlag(flags, classfile::AccessFlag::ACC_STATIC) ||
                !hasFlag(flags, classfile::AccessFlag::ACC_PUBLIC)) {
                method.reset();
            }
        }
        if (!method) return nullptr;
        return std::make_shared<ResolutionSuccess<M>>(root, std::move(method), true);
    }

private:
    static bool hasFlag(ca::i32 flags, ca::i32 flag) noexcept { return (flags & flag) != 0; }

    template<typename V, typename T, typename Find>
    std::shared_ptr<ResolutionSuccess<V>> interfaceLookup(const ClassInfoPtr<M, F>& info,
                                                          ca::str::Utf8StringRef    name,
                                                          const T& type, bool guessAbstract,
                                                          Find find) const
    {
        std::deque<ClassInfoPtr<M, F>>             pending{info};
        std::unordered_set<const ClassInfo<M, F>*> visited;
        std::shared_ptr<ResolutionSuccess<V>>      guess;
        while (!pending.empty()) {
            ClassInfoPtr<M, F> current = std::move(pending.front());
            pending.pop_front();
            if (!current || !visited.insert(current.get()).second) continue;

            if (hasFlag(current->accessFlags(), classfile::AccessFlag::ACC_INTERFACE)) {
                auto value = find(current, name, type);
                if (value) {
                    auto resolution =
                        std::make_shared<ResolutionSuccess<V>>(current, std::move(value), false);
                    if (!guessAbstract || !hasFlag(resolution->member()->accessFlags(),
                                                   classfile::AccessFlag::ACC_ABSTRACT)) {
                        return resolution;
                    }
                    if (!guess) guess = std::move(resolution);
                }
            }
            else if (current->superClass()) {
                pending.push_back(current->superClass());
            }
            for (const auto& interface : current->interfaces()) pending.push_back(interface);
        }
        return guess;
    }

    ResolutionResult<M> checkVirtualMethod(std::shared_ptr<ResolutionSuccess<M>> method) const
    {
        if (!method) return ca::Err(ResolutionError::NO_SUCH_METHOD);
        const ca::i32 flags = method->member()->accessFlags();
        if (hasFlag(flags, classfile::AccessFlag::ACC_STATIC)) {
            return ca::Err(ResolutionError::ACC_STATIC_SET);
        }
        if (hasFlag(flags, classfile::AccessFlag::ACC_ABSTRACT) &&
            !hasFlag(method->owner()->accessFlags(), classfile::AccessFlag::ACC_ABSTRACT)) {
            return ca::Err(ResolutionError::ACC_ABSTRACT_UNSET);
        }
        return ca::Ok(*method);
    }
};

template<typename M, typename F>
class JVMRuntimeResolver final : public RuntimeResolver<M, F>
{
public:
    explicit JVMRuntimeResolver(std::shared_ptr<const JVMLinkResolver<M, F>> linkResolver)
        : linkResolver_(std::move(linkResolver))
    {
        if (!linkResolver_) throw std::invalid_argument("Link resolver must not be null");
    }

    JVMRuntimeResolver()
        : linkResolver_(std::make_shared<const JVMLinkResolver<M, F>>())
    {}

    ResolutionResult<M> resolveVirtualMethod(const ClassInfoPtr<M, F>& info,
                                             ca::str::Utf8StringRef    name,
                                             const type::MethodType&   type) const override
    {
        return rejectAbstract(linkResolver_->resolveVirtualMethod(info, name, type));
    }

    ResolutionResult<M> resolveSpecialMethod(const ClassInfoPtr<M, F>& info,
                                             ca::str::Utf8StringRef    name,
                                             const type::MethodType&   type,
                                             bool                      isInterface) const override
    {
        return rejectAbstract(linkResolver_->resolveSpecialMethod(info, name, type, isInterface));
    }

    ResolutionResult<M> resolveStaticMethod(const ClassInfoPtr<M, F>& info,
                                            ca::str::Utf8StringRef    name,
                                            const type::MethodType&   type,
                                            bool                      isInterface) const override
    {
        return linkResolver_->resolveStaticMethod(info, name, type, isInterface);
    }

    ResolutionResult<M> resolveInterfaceMethod(const ClassInfoPtr<M, F>& info,
                                               ca::str::Utf8StringRef    name,
                                               const type::MethodType&   type) const override
    {
        auto method = linkResolver_->lookupMethod(info, name, type);
        if (!method) method = linkResolver_->interfaceMethod(info, name, type);
        if (!method) return ca::Err(ResolutionError::NO_SUCH_METHOD);
        const ca::i32 flags = method->member()->accessFlags();
        if ((flags & classfile::AccessFlag::ACC_STATIC) != 0) {
            return ca::Err(ResolutionError::ACC_STATIC_SET);
        }
        if ((flags & classfile::AccessFlag::ACC_ABSTRACT) != 0) {
            return ca::Err(ResolutionError::ACC_ABSTRACT_SET);
        }
        return ca::Ok(*method);
    }

    ResolutionResult<F> resolveStaticField(const ClassInfoPtr<M, F>& info,
                                           ca::str::Utf8StringRef    name,
                                           const type::ClassType&    type) const override
    {
        return linkResolver_->resolveStaticField(info, name, type);
    }

    ResolutionResult<F> resolveVirtualField(const ClassInfoPtr<M, F>& info,
                                            ca::str::Utf8StringRef    name,
                                            const type::ClassType&    type) const override
    {
        return linkResolver_->resolveVirtualField(info, name, type);
    }

private:
    static ResolutionResult<M> rejectAbstract(ResolutionResult<M> result)
    {
        if (result.is_ok() &&
            (result.unwrap().member()->accessFlags() & classfile::AccessFlag::ACC_ABSTRACT) != 0) {
            return ca::Err(ResolutionError::ACC_ABSTRACT_SET);
        }
        return result;
    }

    std::shared_ptr<const JVMLinkResolver<M, F>> linkResolver_;
};

}   //namespace jblw::resolution
