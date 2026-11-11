#pragma once

#include <memory>
#include <string>
#include <vector>

#include "jbc/core/class_file.hpp"
#include "jbc/core/opcode.hpp"
#include "jbc/ir/units.hpp"
#include "libca/core/datatype.hpp"

#include "mjbase/jasm/jvm_jasm_service.hpp"
#include "mjbase/jasm/resource_jasm_service.hpp"
#include "mjbase/model/resource_container.hpp"

#include "workspace_types.hpp"

namespace mjt::workspace {

/// @file analysis_ops.hpp
/// @brief 查询算子层：无状态自由函数（输入容器 + 选择器，输出 DTO）。
///
/// 精确单类读取 / Jasm 反汇编面没有 analysis.db 等价物（事实入库的收益
/// 覆盖不了 ingest 成本），W3 裁决不硬塞 DB——从 WorkspaceBase 成员退役
/// 为本模块自由函数；调用方持会话状态（工作容器、LVT 策略缓存），算子
/// 本身零状态。engine 活扫描保留面（searchInstructions / xref / relations
/// / overrides）不在此层。
///
/// lvtJarFallback 位是 LVT 显示名策略（B3）输入，判定要全容器扫描——
/// 调用方自缓存（WorkspaceBase::lvtJarFallbackFlag 按 revision 记忆），
/// 算子只消费 bool，不在单次查询里重扫。

/// @brief 类选择器 → 资源 URI。URI 直命中（.class 后缀或资源表命中）优先；
///        内名命中 Multi-Release 族时按 base 优先、同层 URI 字节序最小
///        （JEP 238 口径，不依赖容器迭代序）。
WorkspaceResult<std::string> resolveClassUri(const model::ResourceContainer& container,
                                             const std::string&              uriOrInternalName);

/// @brief 资源清单（按 URI 升序）；includeClasses=false 剔除 class 条目。
WorkspaceResult<std::vector<ResourceSummary>> listResources(
    const model::ResourceContainer& container, bool includeClasses = true);

/// @brief 单资源原始字节；未登记 → NotFound。
WorkspaceResult<ResourceContent> readResource(const model::ResourceContainer& container,
                                              ca::str::Utf8StringRef          uri);

/// @brief class 字节 + 指纹 + 内名（选择器语义同 resolveClassUri）。
WorkspaceResult<ClassBytesInfo> readClassBytes(const model::ResourceContainer& container,
                                               ca::str::Utf8StringRef uriOrInternalName);

/// @brief class 摘要清单（按内名、次键资源 URI 升序；损坏 class 跳过）。
WorkspaceResult<std::vector<ClassSummary>> listClassSummaries(
    const model::ResourceContainer& container);

/// @brief 全量类事实（classInfo 的清单形；行序同 listClassSummaries）。
WorkspaceResult<std::vector<ClassInfo>> listClasses(const model::ResourceContainer& container,
                                                    bool lvtJarFallback);

/// @brief 单类事实（成员/注解/内部类/LVT 显示名按策略位）。
WorkspaceResult<ClassInfo> classInfo(const model::ResourceContainer& container,
                                     ca::str::Utf8StringRef uriOrInternalName, bool lvtJarFallback);

/// @brief 单方法事实（经 classInfo 解析后按 name+descriptor 命中）。
WorkspaceResult<MethodInfo> methodInfo(const model::ResourceContainer& container,
                                       ca::str::Utf8StringRef owner, ca::str::Utf8StringRef name,
                                       ca::str::Utf8StringRef descriptor, bool lvtJarFallback);

/// @brief 单方法字节码（指令序 + 助记符 + node_type 语义标签）。
WorkspaceResult<MethodBytecode> methodBytecode(const model::ResourceContainer& container,
                                               ca::str::Utf8StringRef owner,
                                               ca::str::Utf8StringRef name,
                                               ca::str::Utf8StringRef descriptor,
                                               bool                   lvtJarFallback);

/// @brief 单资源 jasm 反汇编（mjbase ResourceJasmService 直通；服务面构造
///        借可变容器句柄——算子本身只读，容器语义不变）。
jasm::Result<jasm::JvmJasmDisassembly> getJasm(model::ResourceContainer& container,
                                               ca::str::Utf8StringRef   uri);

/// @brief 单方法 jasm 反汇编（mjbase JvmJasmService 直通）。
jasm::Result<jasm::JvmJasmMethodDisassembly> getMethodJasm(const model::ResourceContainer& container,
                                                           ca::str::Utf8StringRef owner,
                                                           ca::str::Utf8StringRef name,
                                                           ca::str::Utf8StringRef descriptor);

/// @brief LVT 显示名 jar 回退策略判定（全容器扫描的纯函数；调用方自缓存）。
bool lvtJarFallbackOf(const model::ResourceContainer& container);

// ── 共享底座（算子与 WorkspaceBase 留驻面共用；无状态） ────────────────

/// @brief class 字节 → jbc 解析会话；解析失败 → DataLoss（operation 入错文）。
WorkspaceResult<std::shared_ptr<jbc::ir::ClassSession>> sessionOf(const std::vector<ca::u8>& bytes,
                                                                  const std::string& operation);

/// @brief 字段 DTO 组装（名字/描述符/签名/常量值/注解）。
FieldInfo makeFieldInfo(jbc::core::ClassFile& cf, jbc::core::MemberInfo& field);

/// @brief libjbc 无 ASM 节点类型 → 指令语义标签（invoke/field/ldc/cp_ref/
///        jump/var/const/array/iinc/switch 族；methodBytecode 与
///        searchInstructions 共用）。
std::string instructionNodeType(jbc::core::Op op);

/// @brief 字节指纹（fnv1a64:16hex；长度按小端入串——readClassBytes 与
///        modifiedClasses 共用）。
std::string fingerprintBytes(const std::vector<ca::u8>& bytes);

/// @brief 容器指纹（条目 URI + 字节序入串的 fnv1a64:16hex，URI 升序遍历；
///        挂载查重与 modifiedClasses 共用）。
WorkspaceResult<std::string> fingerprintContainer(const model::ResourceContainer& container);

}   // namespace mjt::workspace
