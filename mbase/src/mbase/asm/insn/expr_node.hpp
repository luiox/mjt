#pragma once

#include "libca/core/any.hpp"
#include <memory>
#include <string>
#include <variant>
#include <vector>
#include <cmath>
#include <cstdint>
#include <functional>

#include "jbytecode/tree/insn_list.hpp"
#include "mbase/asm/tmatcher/match_state.hpp"

namespace mjt::base {

///表达式节点类型
enum class ExprType
{
    Number,     //数字常量
    Capture,    //捕获引用
    UnaryOp,    //一元运算
    BinaryOp,   //二元运算
    Cast        //类型转换
};

enum class UnaryOp
{
    Neg,
    Not
};
enum class BinaryOp
{
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Shl,
    Shr,
    Ushr,
    And,
    Or,
    Xor
};

///表达式值 — 支持的类型
struct ExprValue
{
    std::variant<int64_t, uint64_t, double> data;

    ExprValue()
        : data((int64_t)0)
    {}
    explicit ExprValue(int64_t v)
        : data(v)
    {}
    explicit ExprValue(uint64_t v)
        : data(v)
    {}
    explicit ExprValue(double v)
        : data(v)
    {}

    bool isInt() const { return std::holds_alternative<int64_t>(data); }
    bool isUInt() const { return std::holds_alternative<uint64_t>(data); }
    bool isFloat() const { return std::holds_alternative<double>(data); }

    int64_t  asInt() const { return std::get<int64_t>(data); }
    uint64_t asUInt() const { return std::get<uint64_t>(data); }
    double   asFloat() const { return std::get<double>(data); }
};

class ExprNode
{
public:
    virtual ~ExprNode()                                                  = default;
    virtual ExprType  type() const                                       = 0;
    virtual ExprValue eval(const tmatcher::CaptureStore& captures) const = 0;
};

class ExprNumber : public ExprNode
{
    ExprValue value_;

public:
    explicit ExprNumber(ExprValue v)
        : value_(v)
    {}
    ExprType  type() const override { return ExprType::Number; }
    ExprValue eval(const tmatcher::CaptureStore&) const override { return value_; }
};

class ExprCapture : public ExprNode
{
    std::string name_;

public:
    explicit ExprCapture(std::string name)
        : name_(std::move(name))
    {}
    ExprType  type() const override { return ExprType::Capture; }
    ExprValue eval(const tmatcher::CaptureStore& captures) const override;
};

class ExprUnaryOp : public ExprNode
{
    UnaryOp                   op_;
    std::unique_ptr<ExprNode> child_;

public:
    ExprUnaryOp(UnaryOp op, std::unique_ptr<ExprNode> child)
        : op_(op)
        , child_(std::move(child))
    {}
    ExprType  type() const override { return ExprType::UnaryOp; }
    ExprValue eval(const tmatcher::CaptureStore& captures) const override;
};

class ExprBinaryOp : public ExprNode
{
    BinaryOp                  op_;
    std::unique_ptr<ExprNode> left_, right_;

public:
    ExprBinaryOp(BinaryOp op, std::unique_ptr<ExprNode> left, std::unique_ptr<ExprNode> right)
        : op_(op)
        , left_(std::move(left))
        , right_(std::move(right))
    {}
    ExprType  type() const override { return ExprType::BinaryOp; }
    ExprValue eval(const tmatcher::CaptureStore& captures) const override;
};

///便捷工厂函数
inline std::unique_ptr<ExprNode> exprNum(int64_t v)
{
    return std::make_unique<ExprNumber>(ExprValue(v));
}
inline std::unique_ptr<ExprNode> exprNum(uint64_t v)
{
    return std::make_unique<ExprNumber>(ExprValue(v));
}
inline std::unique_ptr<ExprNode> exprNum(double v)
{
    return std::make_unique<ExprNumber>(ExprValue(v));
}
inline std::unique_ptr<ExprNode> exprCap(const std::string& name)
{
    return std::make_unique<ExprCapture>(name);
}
inline std::unique_ptr<ExprNode> exprNeg(std::unique_ptr<ExprNode> c)
{
    return std::make_unique<ExprUnaryOp>(UnaryOp::Neg, std::move(c));
}
inline std::unique_ptr<ExprNode> exprAdd(std::unique_ptr<ExprNode> l, std::unique_ptr<ExprNode> r)
{
    return std::make_unique<ExprBinaryOp>(BinaryOp::Add, std::move(l), std::move(r));
}
inline std::unique_ptr<ExprNode> exprSub(std::unique_ptr<ExprNode> l, std::unique_ptr<ExprNode> r)
{
    return std::make_unique<ExprBinaryOp>(BinaryOp::Sub, std::move(l), std::move(r));
}
inline std::unique_ptr<ExprNode> exprMul(std::unique_ptr<ExprNode> l, std::unique_ptr<ExprNode> r)
{
    return std::make_unique<ExprBinaryOp>(BinaryOp::Mul, std::move(l), std::move(r));
}
inline std::unique_ptr<ExprNode> exprDiv(std::unique_ptr<ExprNode> l, std::unique_ptr<ExprNode> r)
{
    return std::make_unique<ExprBinaryOp>(BinaryOp::Div, std::move(l), std::move(r));
}
inline std::unique_ptr<ExprNode> exprXor(std::unique_ptr<ExprNode> l, std::unique_ptr<ExprNode> r)
{
    return std::make_unique<ExprBinaryOp>(BinaryOp::Xor, std::move(l), std::move(r));
}

}   //namespace mjt::base
