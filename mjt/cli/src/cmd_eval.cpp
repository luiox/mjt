/// @file        cmd_eval.cpp
/// @brief       `mjt eval` 命令核心：数学表达式求值 / 化简（mjbase expr）。
///
/// 基于 mjbase::expr 的 parseExpression / simplify / eval / toSExpr。unbound（未绑定变量）不算失败：打印表达式的 S-表达式，退出码 0；绑定串按 "k=v,k2=v2" 解析，stoll 防御见 parseVarBindings。
#include "commands.hpp"

#include <string>
#include <utility>

#include "mjbase/expr/expr_eval.hpp"
#include "mjbase/expr/expr_parser.hpp"
#include "mjbase/expr/expr_print.hpp"
#include "mjbase/expr/expr_simplify.hpp"

#include "shared/i18n_errors.hpp"
#include "shared/output_sink.hpp"

namespace mjt::cli {
namespace {

/// 变量名 std::string → Utf8String 键（按原字节内容转换）。
ca::str::Utf8String utf8Key(const std::string& value)
{
    return ca::str::Utf8String::from_data_unchecked(reinterpret_cast<const ca::u8*>(value.data()),
                                                    value.size());
}

/// 解析变量绑定串："k=v,k2=v2"；等号缺失、空名、值非整数（invalid_argument）或超范围（out_of_range）→ false。
bool parseVarBindings(const std::string& spec, mjbase::expr::VarBindings& bindings)
{
    if (spec.empty()) return true;
    std::size_t start = 0;
    while (start < spec.size()) {
        const auto eq = spec.find('=', start);
        if (eq == std::string::npos || eq == start) return false;
        const auto name   = spec.substr(start, eq - start);
        const auto comma  = spec.find(',', eq + 1);
        const auto valStr = (comma == std::string::npos) ? spec.substr(eq + 1)
                                                         : spec.substr(eq + 1, comma - eq - 1);
        // stoll 对非数字（如 "abc"）抛 invalid_argument，对超范围（如 99…99）抛 out_of_range；不捕获会让 CLI 进程崩溃。失败返回 false，由调用方经 i18n 键输出友好错误。
        try {
            bindings.emplace(utf8Key(name),
                             mjbase::expr::ExprValue(static_cast<ca::i64>(std::stoll(valStr))));
        }
        catch (const std::exception&) {
            return false;
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return true;
}

/// EvalError 便捷构造（i18n 渲染）。
EvalError evalError(EvalError::Kind kind, const char* key,
                    std::initializer_list<std::string_view> args = {})
{
    EvalError error;
    error.kind    = kind;
    error.message = shared::trfStr(key, args);
    return error;
}

}   // namespace

ca::Result<EvalOutcome, EvalError> evaluateExpression(const std::string& exprStr,
                                                      const std::string& varSpec,
                                                      bool doSimplify)
{
    mjbase::expr::VarBindings vars;
    if (!varSpec.empty() && !parseVarBindings(varSpec, vars))
        return ca::Err(evalError(EvalError::Kind::InvalidVarSpec,
                                 "mjt.cmd.eval.err.invalid_var_spec", {varSpec}));

    if (exprStr.empty())
        return ca::Err(evalError(EvalError::Kind::MissingExpression,
                                 "mjt.cmd.eval.err.missing_expression"));

    // parseExpression 0.0.28 起错误出口改用 Utf8String。
    ca::str::Utf8String parseError;
    auto expr = mjbase::expr::parseExpression(exprStr, &parseError);
    if (!expr)
        return ca::Err(evalError(EvalError::Kind::ParseFailed,
                                 "mjt.cmd.eval.err.parse_failed",
                                 {parseError.to_std_string()}));

    // 化简总是有益：折叠常量、应用恒等式。
    if (doSimplify) expr = mjbase::expr::simplify(expr).simplified;

    const auto result = mjbase::expr::eval(*expr, vars);
    EvalOutcome outcome;
    if (result.ok) {
        outcome.evaluated = true;
        outcome.value     = result.value.rawValue();
        return ca::Ok(std::move(outcome));
    }

    // unbound（未绑定变量）不算失败：打印表达式的 S-表达式（mjbase eval 的 unbound 诊断文案以 "unbound variable: " 开头）。
    if (result.error.to_std_string().find("unbound") != std::string::npos) {
        outcome.symbolic = mjbase::expr::toSExpr(*expr).to_std_string();
        return ca::Ok(std::move(outcome));
    }

    // 其余错误（如深度超限）。
    return ca::Err(evalError(EvalError::Kind::EvalFailed, "mjt.cmd.eval.err.eval_failed",
                             {result.error.to_std_string()}));
}

void renderEvalOutcome(const EvalOutcome& outcome, OutputSink& sink)
{
    if (outcome.evaluated) sink.printf("%lld\n", static_cast<long long>(outcome.value));
    else                   sink.puts(outcome.symbolic);
}

void renderEvalUsage(OutputSink& sink)
{
    // 用法文自带尾部换行（.lang 值内嵌 \n），write 不再加倍。
    sink.write(shared::trfStr("mjt.cmd.eval.out.usage"));
}

}   // namespace mjt::cli
