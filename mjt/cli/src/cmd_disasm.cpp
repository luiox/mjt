/// @file        cmd_disasm.cpp
/// @brief       `mjt disasm` 输出 JVM 类的 jasm 或 DEX 类的 Smali。
///
/// 地基：workspace getJasm 读面（MCP class_query kind=jasm 同源能力）——
/// 类选择器经 classInfo（URI / 内名双解析）定位，jasm 文本由 mjbase jasm
/// 服务规范化产出。类不存在退出码 1；类字节可解析但反汇编失败退出码 2。
/// 纯文本面，不依赖反编译器——敌意类名 / 敌意方法体的可读出口（decompile
/// 批量预检跳过清单的指路面）。
#include "commands.hpp"

#include <string>
#include <utility>

#include "shared/i18n_errors.hpp"
#include "shared/output_sink.hpp"
#include "workspace/workspace.hpp"
#include "workspace/analysis_ops.hpp"
#include "workspace/dex_domain.hpp"
#include "workspace/workspace_types.hpp"

namespace mjt::cli {
namespace {

/// Utf8String/UTF-8 std::string → 借用视图（与 info / strings 同款构造）。
ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// DisasmError 便捷构造（i18n 渲染）。
DisasmError disasmError(DisasmError::Kind kind, const char* key,
                        std::initializer_list<std::string_view> args = {})
{
    DisasmError error;
    error.kind    = kind;
    error.message = shared::trfStr(key, args);
    return error;
}

}   // namespace

ca::Result<DisasmReport, DisasmError> collectDisasm(const std::string& jarPath,
                                                    const std::string& internalName)
{
    workspace::WorkspaceBase ws;
    auto opened = ws.open(refOf(jarPath));
    if (opened.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(opened).unwrap_err();
        return ca::Err(disasmError(DisasmError::Kind::Runtime, "mjt.cli.disasm.err.open_failed",
                                   {info.message}));
    }

    if (const auto* dex = ws.dexDomain()) {
        auto smali = dex->smaliText(internalName);
        if (smali.is_err()) {
            const auto error = std::move(smali).unwrap_err();
            if (error.code == workspace::WorkspaceError::NotFound)
                return ca::Err(disasmError(DisasmError::Kind::ClassNotFound,
                                           "mjt.cli.disasm.err.class_not_found", {internalName}));
            return ca::Err(disasmError(DisasmError::Kind::Runtime,
                                       "mjt.cli.disasm.err.disasm_failed", {error.message}));
        }
        return ca::Ok(DisasmReport{jarPath, internalName, std::move(smali).unwrap()});
    }

    // 类选择器定位（URI / 内名双解析）：NotFound → 退出码 1，其余 → 退出码 2。
    auto selected = workspace::classInfo(*ws.workingContainer(),
                                               refOf(internalName),
                                               ws.lvtJarFallbackFlag());
    if (selected.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(selected).unwrap_err();
        return ca::Err(info.code == workspace::WorkspaceError::NotFound
                           ? disasmError(DisasmError::Kind::ClassNotFound,
                                         "mjt.cli.disasm.err.class_not_found", {internalName})
                           : disasmError(DisasmError::Kind::Runtime,
                                         "mjt.cli.disasm.err.class_info_failed",
                                         {info.message}));
    }

    auto disassembled = workspace::getJasm(*ws.workingContainer(),
                                                 std::move(selected).unwrap().resourceUri.ref());
    if (!disassembled.isOk()) {
        // 诊断面聚合首条 Error 文案（与 MCP class_jasm diagnostics 同源）。
        std::string detail;
        for (const auto& diagnostic : disassembled.diagnostics) {
            if (diagnostic.severity != mjbase::jasm::DiagnosticSeverity::Error) continue;
            detail = diagnostic.message.to_utf8_string().to_std_string();
            break;
        }
        return ca::Err(disasmError(DisasmError::Kind::Runtime, "mjt.cli.disasm.err.disasm_failed",
                                   {detail}));
    }

    DisasmReport report;
    report.jarPath      = jarPath;
    report.internalName = internalName;
    report.jasm         = disassembled.value->source.ref().to_utf8_string().to_std_string();
    return ca::Ok(std::move(report));
}

void renderDisasmText(const DisasmReport& report, OutputSink& sink)
{
    // jasm 全文原样直出（不混头部摘要）：stdout 重定向取文件语义，与
    // decompile 单类 stdout 模式同款约定。
    sink.write(report.jasm);
}

}   // namespace mjt::cli
