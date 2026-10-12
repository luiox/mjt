#pragma once

#include "annotation.hpp"
#include "elements.hpp"

#include <functional>
#include <memory>

namespace jblw::annotation {

using DeferredElement = std::function<ElementPtr()>;

class GenericAnnotationBuilder;
class GenericArrayBuilder;
class GenericTypeAnnotationBuilder;

///@brief annotation element array 的可变构造接口。
class ElementArrayBuilder
{
public:
    virtual ~ElementArrayBuilder()                                               = default;
    virtual ElementArrayBuilder&                element(ElementPtr element)      = 0;
    virtual ElementArrayBuilder&                element(DeferredElement element) = 0;
    virtual std::shared_ptr<const ElementArray> build() const                    = 0;
};

///@brief 普通 annotation 的可变构造接口。
class AnnotationBuilder
{
public:
    virtual ~AnnotationBuilder()                                                          = default;
    virtual AnnotationBuilder& element(ca::str::Utf8String name, ElementPtr element)      = 0;
    virtual AnnotationBuilder& element(ca::str::Utf8String name, DeferredElement element) = 0;
    virtual const type::InstanceTypePtr& type() const noexcept                            = 0;
    virtual AnnotationBuilder&           type(type::InstanceTypePtr type)                 = 0;
    virtual AnnotationPtr                build() const                                    = 0;
};

class TypeAnnotationBuilder : public AnnotationBuilder
{
public:
    virtual ca::i32                        typeRef() const noexcept                = 0;
    virtual TypeAnnotationBuilder&         typeRef(ca::i32 value) noexcept         = 0;
    virtual const std::optional<TypePath>& typePath() const noexcept               = 0;
    virtual TypeAnnotationBuilder&         typePath(std::optional<TypePath> value) = 0;
};

class GenericArrayBuilder final : public ElementArrayBuilder,
                                  public std::enable_shared_from_this<GenericArrayBuilder>
{
public:
    GenericArrayBuilder&                      element(ElementPtr element) override;
    GenericArrayBuilder&                      element(DeferredElement element) override;
    GenericArrayBuilder&                      elements(const std::vector<ElementPtr>& elements);
    std::shared_ptr<GenericAnnotationBuilder> annotation(type::InstanceTypePtr type);
    std::shared_ptr<GenericArrayBuilder>      array();
    std::shared_ptr<const ElementArray>       build() const override;

private:
    std::vector<DeferredElement> values_;
};

class GenericAnnotationBuilder : public AnnotationBuilder,
                                 public std::enable_shared_from_this<GenericAnnotationBuilder>
{
public:
    explicit GenericAnnotationBuilder(type::InstanceTypePtr type);

    GenericAnnotationBuilder& element(ca::str::Utf8String name, ElementPtr element) override;
    GenericAnnotationBuilder& element(ca::str::Utf8String name, DeferredElement element) override;
    std::shared_ptr<GenericAnnotationBuilder> annotation(ca::str::Utf8String   name,
                                                         type::InstanceTypePtr type);
    std::shared_ptr<GenericArrayBuilder>      array(ca::str::Utf8String name);
    const type::InstanceTypePtr&              type() const noexcept override { return type_; }
    GenericAnnotationBuilder&                 type(type::InstanceTypePtr type) override;
    AnnotationPtr                             build() const override;

protected:
    using PendingEntry = std::pair<ca::str::Utf8String, DeferredElement>;
    void                         put(ca::str::Utf8String name, DeferredElement element);
    std::vector<AnnotationEntry> buildEntries() const;

    type::InstanceTypePtr     type_;
    std::vector<PendingEntry> entries_;
};

class GenericTypeAnnotationBuilder final : public TypeAnnotationBuilder
{
public:
    GenericTypeAnnotationBuilder(type::InstanceTypePtr type, ca::i32 typeRef,
                                 std::optional<TypePath> typePath);

    GenericTypeAnnotationBuilder&  element(ca::str::Utf8String name, ElementPtr element) override;
    GenericTypeAnnotationBuilder&  element(ca::str::Utf8String name,
                                           DeferredElement     element) override;
    const type::InstanceTypePtr&   type() const noexcept override { return type_; }
    GenericTypeAnnotationBuilder&  type(type::InstanceTypePtr type) override;
    ca::i32                        typeRef() const noexcept override { return typeRef_; }
    GenericTypeAnnotationBuilder&  typeRef(ca::i32 value) noexcept override;
    const std::optional<TypePath>& typePath() const noexcept override { return typePath_; }
    GenericTypeAnnotationBuilder&  typePath(std::optional<TypePath> value) override;
    AnnotationPtr                  build() const override;
    TypeAnnotationPtr              buildType() const;

private:
    using PendingEntry = std::pair<ca::str::Utf8String, DeferredElement>;
    void                         put(ca::str::Utf8String name, DeferredElement element);
    std::vector<AnnotationEntry> buildEntries() const;

    type::InstanceTypePtr     type_;
    std::vector<PendingEntry> entries_;
    ca::i32                   typeRef_;
    std::optional<TypePath>   typePath_;
};

}   //namespace jblw::annotation
