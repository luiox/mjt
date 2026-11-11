#pragma once

#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string.hpp"
#include "libca/str/utf16_string.hpp"

#include "mjbase/model/resource_container.hpp"

#include "class_mutation_service.hpp"
#include "workspace_types.hpp"

namespace mjt::workspace {

/// @file edit_support.hpp
/// @brief workspace 模块内部共享助手（编辑管道 / workspace.cpp 查询实现共用）。
///
/// 自 workspace.cpp 匿名命名空间上移：edit_pipeline.cpp 与 workspace.cpp 同
/// 一份实现。模块内部面——不对 mcp/cli/pysdk 公开承诺。

/// @brief 错误值构造（"operation: detail" 拼接口径）。
WorkspaceErrorInfo error(WorkspaceError code, const std::string& operation,
                         const std::string& detail);

/// @brief 底层文本 → Utf8String：字节原文直通，不做二次校验（jbc/容器面文本
///        按 modified UTF-8 原文保留；与 analysis_ingest 的 utf8() 同口径）。
ca::str::Utf8String utf8(std::string_view value);

/// @brief std::string（DTO/接口面）→ Utf8StringRef 借用视图。
ca::str::Utf8StringRef utf8Ref(const std::string& value) noexcept;

/// @brief UTF-8 窄串 → Utf16String（jasm 文本面）。
ca::str::Utf16String utf16(const std::string& value);

/// @brief std::string 列表 → Utf8String 列表（DTO vector 字段换型直通）。
std::vector<ca::str::Utf8String> utf8List(const std::vector<std::string>& values);

/// @brief IResource::getLocation() 借用视图 → owning 文本（跨界传递用）。
std::string locationText(const mjbase::model::IResource& resource);

/// @brief 任意资源 → 原始字节（class/manifest/unknown 三类；其余 Unimplemented）。
WorkspaceResult<std::vector<ca::u8>> bytesOf(const mjbase::model::IResource& resource);

/// @brief 两容器的字节级 diff（单一公共前后缀收缩口径）。
std::vector<ByteDiff> diffContainers(const mjbase::model::ResourceContainer& before,
                                     const mjbase::model::ResourceContainer& after);

/// @brief 编辑校验门的作用域：前后差异条目的 URI 集——操作受影响面。
std::vector<std::string> changedUris(const std::vector<ByteDiff>& diffs);

/// @brief class 字节 → 内部名（骨架解析；失败为空）。
std::string internalNameOfBytes(const std::vector<ca::u8>& bytes);

/// @brief diff 条目中 class 条目的内名集（排序去重）。
std::vector<std::string> changedClassNames(const mjbase::model::ResourceContainer& container,
                                           const std::vector<ByteDiff>&            diffs);

/// @brief 结构化 mutation 是否聚焦单个方法（指纹落库单方法面判据，#132）。
EditedMethodFocus focusedMethodOfEdit(const ClassMutation& mutation);

/// @brief 受影响引用事实（diff 条目作用域收敛的 ReferenceScan 直连面，#80）。
std::vector<ReferenceImpact> collectImpacts(
    const mjbase::model::ResourceContainer&      container,
    const std::vector<ByteDiff>&                 diffs,
    const std::vector<ca::str::Utf8String>&      classes);

}   // namespace mjt::workspace
