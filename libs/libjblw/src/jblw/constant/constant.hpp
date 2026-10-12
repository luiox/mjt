#pragma once

#include <libca/core/datatype.hpp>

#include <memory>

namespace jblw::constant {

class ConstantSink;

///@brief 无 RTTI 常量层次的运行时标签。
enum class ConstantKind : ca::u8
{
    STRING,
    METHOD_HANDLE,
    TYPE,
    DYNAMIC,
    LONG,
    DOUBLE,
    INT,
    FLOAT,
};

///@brief JVM 可加载常量的不可变根接口。
class Constant
{
public:
    virtual ~Constant() = default;

    virtual void         accept(ConstantSink& sink) const             = 0;
    virtual ConstantKind getType() const noexcept                     = 0;
    virtual bool         equals(const Constant& other) const noexcept = 0;
    virtual ca::i32      hashCode() const noexcept                    = 0;

protected:
    Constant() = default;
};

using ConstantPtr = std::shared_ptr<const Constant>;

bool operator==(const Constant& lhs, const Constant& rhs) noexcept;
bool operator!=(const Constant& lhs, const Constant& rhs) noexcept;

}   //namespace jblw::constant
