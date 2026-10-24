#pragma once

// pysdk_jasm.hpp — Jasm 文本面（libjhasm 绑定；MCP Jasm 工具族的 SDK 等位物）。
//
// 键口径与 facade 一致：class 级按类名寻址（经 name↔URI 索引，重名落选者
// 不可寻址），方法级按 (cls, name, desc) 三元组。内容性错误（汇编失败/
// diff 上下文失配/方法段歧义）走诊断返回，不抛异常——与 MCP 侧行为对齐；
// 键解析失败（类/方法不存在）照旧 StaleRefError。
//
// 事务语义：validate 在容器深拷贝上试汇编（绝不落容器）；apply 经
// ResourceJasmService 事务式替换 ClassResource（名称校验 + 候选工作区复验
// 全过才 set），成功后该类缓存树作废——与 set_class_bytes 同款脏纪律。

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "pysdk_facade.hpp"

namespace mjt::pysdk {

/// 诊断（引擎 Diagnostic 的值拷贝视图；severity/stage 为引擎枚举序）。
struct JasmDiagnostic
{
    int         severity = 0;   ///< 0=error 1=warning（DiagnosticSeverity）
    int         stage    = 0;   ///< 0=Assemble 1=Disassemble 2=Validate 3=Resource
                                ///< 4=EditGate（workspace 编辑门）
    std::string message;        ///< utf8
    int         line = -1, column = -1;   ///< 无定位 -1
};

struct JasmClassText
{
    std::string internalName;
    std::string source;   ///< 规范化 Jasm 全文（LF）
};

struct JasmMethodText
{
    std::string internalName, name, descriptor;
    std::string source;   ///< 规范化 .method 声明（与 class 级全文内方法段逐字节一致）
};

/// validate/apply 报告：ok = 无 error 级诊断；apply 成功时 written = 新类字节数。
struct JasmReport
{
    std::string                 internalName;
    ca::usize                   written = 0;
    std::vector<JasmDiagnostic> diagnostics;
    bool ok() const noexcept;
};

// ── 读 ──

/// 类字节 → 规范化 Jasm 全文（malformed 类抛 std::runtime_error）。
JasmClassText facadeClassJasm(FacadeSession& s, const std::string& name);

/// 单方法 → 规范化 .method 声明；类/方法不存在 → StaleRefError。
JasmMethodText facadeMethodJasm(FacadeSession& s, const std::string& cls,
                                const std::string& name, const std::string& desc);

// ── class 级 patch（source = 完整 Jasm；diff = 对当前规范全文的 unified diff） ──
// validation：编辑门与重建管线共用的校验档位（"relaxed"/"strict"/"off"），
// 与 MCP edit_apply 的同名键等位；非法值抛 std::invalid_argument。

JasmReport facadeValidateJasm(FacadeSession& s, const std::string& name,
                              const std::string& sourceName, const std::string& source,
                              const std::string& validation = "relaxed");
JasmReport facadeApplyJasm(FacadeSession& s, const std::string& name,
                           const std::string& sourceName, const std::string& source,
                           const std::string& validation = "relaxed");
JasmReport facadeValidateJasmDiff(FacadeSession& s, const std::string& name,
                                  const std::string& diff,
                                  const std::string& validation = "relaxed");
JasmReport facadeApplyJasmDiff(FacadeSession& s, const std::string& name,
                               const std::string& diff,
                               const std::string& validation = "relaxed");

// ── 方法级 patch（source/diff 相对 method_jasm 产物；拼接回类全文后走 class 面） ──

JasmReport facadeValidateMethodJasm(FacadeSession& s, const std::string& cls,
                                    const std::string& name, const std::string& desc,
                                    const std::string& sourceName, const std::string& source,
                                    const std::string& validation = "relaxed");
JasmReport facadeApplyMethodJasm(FacadeSession& s, const std::string& cls,
                                 const std::string& name, const std::string& desc,
                                 const std::string& sourceName, const std::string& source,
                                 const std::string& validation = "relaxed");
JasmReport facadeValidateMethodJasmDiff(FacadeSession& s, const std::string& cls,
                                        const std::string& name, const std::string& desc,
                                        const std::string& diff,
                                        const std::string& validation = "relaxed");
JasmReport facadeApplyMethodJasmDiff(FacadeSession& s, const std::string& cls,
                                     const std::string& name, const std::string& desc,
                                     const std::string& diff,
                                     const std::string& validation = "relaxed");

}   // namespace mjt::pysdk
