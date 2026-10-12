#include "mbase/asm/insn/expr_node.hpp"

namespace mjt::base {

ExprValue ExprCapture::eval(const tmatcher::CaptureStore& captures) const
{
    if (!captures.contains(name_)) return ExprValue((int64_t)0);
    auto val = captures.get(name_);
    if (auto* p = val.as<int>()) return ExprValue((int64_t)*p);
    if (auto* p = val.as<int64_t>()) return ExprValue(*p);
    if (auto* p = val.as<uint64_t>()) return ExprValue(*p);
    if (auto* p = val.as<float>()) return ExprValue((double)*p);
    if (auto* p = val.as<double>()) return ExprValue(*p);
    return ExprValue((int64_t)0);
}

ExprValue ExprUnaryOp::eval(const tmatcher::CaptureStore& captures) const
{
    auto v = child_->eval(captures);
    if (op_ == UnaryOp::Neg) {
        if (v.isInt()) return ExprValue(-v.asInt());
        if (v.isUInt()) return ExprValue(-(int64_t)v.asUInt());
        return ExprValue(-v.asFloat());
    }
    return v;
}

static ExprValue applyBinary(BinaryOp op, const ExprValue& a, const ExprValue& b)
{
    auto doInt = [&](int64_t x, int64_t y) -> int64_t {
        switch (op) {
        case BinaryOp::Add: return x + y;
        case BinaryOp::Sub: return x - y;
        case BinaryOp::Mul: return x * y;
        case BinaryOp::Div: return y != 0 ? x / y : 0;
        case BinaryOp::Mod: return y != 0 ? x % y : 0;
        case BinaryOp::Shl: return x << y;
        case BinaryOp::Shr: return x >> y;
        case BinaryOp::Ushr: return (int64_t)((uint64_t)x >> (uint64_t)y);
        case BinaryOp::And: return x & y;
        case BinaryOp::Or: return x | y;
        case BinaryOp::Xor: return x ^ y;
        }
        return 0;
    };
    auto doFloat = [&](double x, double y) -> double {
        switch (op) {
        case BinaryOp::Add: return x + y;
        case BinaryOp::Sub: return x - y;
        case BinaryOp::Mul: return x * y;
        case BinaryOp::Div: return x / y;
        case BinaryOp::Mod: return fmod(x, y);
        default: return x;
        }
    };

    if (a.isInt() && b.isInt()) return ExprValue(doInt(a.asInt(), b.asInt()));
    if (a.isUInt() && b.isUInt()) return ExprValue(doInt((int64_t)a.asUInt(), (int64_t)b.asUInt()));
    if (a.isFloat() && b.isFloat()) return ExprValue(doFloat(a.asFloat(), b.asFloat()));
    //Mixed: promote to double
    double av = a.isFloat() ? a.asFloat() : (double)a.asInt();
    double bv = b.isFloat() ? b.asFloat() : (double)b.asInt();
    return ExprValue(doFloat(av, bv));
}

ExprValue ExprBinaryOp::eval(const tmatcher::CaptureStore& captures) const
{
    return applyBinary(op_, left_->eval(captures), right_->eval(captures));
}

}   //namespace mjt::base
