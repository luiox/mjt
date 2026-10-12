#pragma once

#include "type.hpp"
#include "type_error.hpp"

#include <libca/core/result.hpp>

#include <optional>

namespace jblw::type {

///@brief 按顺序读取 JVM descriptor 中的类型。
///
///reader 借用构造时传入的 UTF-8 数据，调用方必须保证 descriptor 在 reader
///生命周期内有效；成功返回的类型拥有自身字符串，不再依赖输入。
class TypeReader final
{
public:
    using ReadResult = ca::Result<std::optional<TypePtr>, TypeError>;

    explicit TypeReader(ca::str::Utf8StringRef descriptor) noexcept
        : descriptor_(descriptor)
    {}

    ///@brief 读取下一个类型；到达末尾返回空 optional。
    ReadResult read();

    ///@brief 读取必需类型；到达末尾返回 UNEXPECTED_END。
    ca::Result<TypePtr, TypeError> required();

    ///@brief 读取必需 ClassType；method type 返回 EXPECTED_CLASS_TYPE。
    ca::Result<ClassTypePtr, TypeError> requireClassType();

    ///@brief 返回当前 UTF-8 byte offset。
    ca::usize position() const noexcept { return index_; }

private:
    ca::Result<TypePtr, TypeError> readRequiredAtCurrent();
    ca::Result<TypePtr, TypeError> readMethod(ca::usize openingOffset);

    ca::str::Utf8StringRef descriptor_;
    ca::usize              index_ = 0;
};

}   //namespace jblw::type
