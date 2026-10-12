#pragma once

#include "element.hpp"
#include "type_path.hpp"

#include "jblw/type/instance_type.hpp"

#include <optional>
#include <utility>
#include <vector>

namespace jblw::annotation {

enum class AnnotationKind : ca::u8
{
    MAP,
    TYPE,
};

using AnnotationEntry = std::pair<ca::str::Utf8String, ElementPtr>;

///@brief 普通与类型注解的公共只读接口。
class Annotation : public Element
{
public:
    ElementKind            getType() const noexcept final { return ElementKind::ANNOTATION; }
    virtual AnnotationKind annotationKind() const noexcept                                      = 0;
    virtual const type::InstanceTypePtr&        type() const noexcept                           = 0;
    virtual const std::vector<AnnotationEntry>& entries() const noexcept                        = 0;
    virtual const ElementPtr*                   get(ca::str::Utf8StringRef name) const noexcept = 0;

protected:
    Annotation() = default;
};

using AnnotationPtr = std::shared_ptr<const Annotation>;

class MapAnnotation final : public Annotation
{
public:
    MapAnnotation(type::InstanceTypePtr type, std::vector<AnnotationEntry> entries);

    AnnotationKind annotationKind() const noexcept override { return AnnotationKind::MAP; }
    const type::InstanceTypePtr&        type() const noexcept override { return type_; }
    const std::vector<AnnotationEntry>& entries() const noexcept override { return entries_; }
    const ElementPtr*                   get(ca::str::Utf8StringRef name) const noexcept override;
    bool                                equals(const Element& other) const noexcept override;
    ca::i32                             hashCode() const noexcept override { return hashCode_; }

private:
    type::InstanceTypePtr        type_;
    std::vector<AnnotationEntry> entries_;
    ca::i32                      hashCode_;
};

///@brief 携带 type_reference 与可选 type_path 的注解接口。
class TypeAnnotation : public Annotation
{
public:
    virtual ca::i32                        typeRef() const noexcept  = 0;
    virtual const std::optional<TypePath>& typePath() const noexcept = 0;

protected:
    TypeAnnotation() = default;
};

using TypeAnnotationPtr = std::shared_ptr<const TypeAnnotation>;

class MapTypeAnnotation final : public TypeAnnotation
{
public:
    MapTypeAnnotation(type::InstanceTypePtr type, std::vector<AnnotationEntry> entries,
                      ca::i32 typeRef, std::optional<TypePath> typePath);

    AnnotationKind annotationKind() const noexcept override { return AnnotationKind::TYPE; }
    const type::InstanceTypePtr&        type() const noexcept override { return type_; }
    const std::vector<AnnotationEntry>& entries() const noexcept override { return entries_; }
    const ElementPtr*                   get(ca::str::Utf8StringRef name) const noexcept override;
    ca::i32                             typeRef() const noexcept override { return typeRef_; }
    const std::optional<TypePath>&      typePath() const noexcept override { return typePath_; }
    bool                                equals(const Element& other) const noexcept override;
    ca::i32                             hashCode() const noexcept override { return hashCode_; }

private:
    type::InstanceTypePtr        type_;
    std::vector<AnnotationEntry> entries_;
    ca::i32                      typeRef_;
    std::optional<TypePath>      typePath_;
    ca::i32                      hashCode_;
};

}   //namespace jblw::annotation
