#pragma once

#include <string>

//切换期：renamer 走 mjbase（std::string/string_view 面），其余模块仍走 mbase；
//两栈命名空间不冲突，可同时链接。本文件是 mjt 命令层的 std::string 桥接面，
//切换对消费方（mutation/query/shadow_slot/class_render）零 API 变动。
#include "libca/str/utf8_string.hpp"
#include "mjbase/renamer/shadow_alias.hpp"

namespace mjt {

///@brief sidecar 路径解析（钉死双轨）：显式 `--shadow <file>` 优先；
///未给则用约定默认名 `<jar路径>.mjt-shadow`（与 jar 同目录）。
std::string resolveShadowPath(const std::string& jarPath, const std::string& explicitPath);

///@brief 资源 URI（`a/b/C.class`）→ class 内部名（`a/b/C`）；无 .class 后缀原样返回。
std::string internalNameOfUri(const std::string& uri);

///@brief std::string → Utf8StringRef 临时视图（仅在 string 存活期内使用）。
inline ca::str::Utf8StringRef utf8RefOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

///@brief shadow 显示上下文：命令层渲染适配（显示层统一入口）。
///
///`active` 为 false（--no-shadow 或 sidecar 空/缺失）时，全部渲染方法
///输出与既有打印口径逐字节一致（printReadable 原样路径）；命中 shadow 时
///按 `shadow (原始名)` 并列。
struct ShadowDisplay
{
    mjbase::renamer::ShadowAliasTable table;
    bool                              enabled = true;  ///< --no-shadow 关闭

    bool active() const { return enabled && !table.empty(); }

    //── 渲染（inactive = 既有口径）───────────────────────────────────
    ///class 内部名显示形式。
    std::string className(const std::string& internalName) const;

    ///成员名字片段（方法/字段标签等分离打印点）。
    std::string memberName(const std::string& owner, const std::string& name,
                           const std::string& desc) const;

    ///字段行 `name desc`：inactive 保持既有整体 printReadable 拼装（含空格
    ///转义为 \u0020 的历史口径）；active 改为 `shadow (raw) desc`。
    std::string fieldLine(const std::string& owner, const std::string& name,
                          const std::string& desc) const;

    ///完整成员形式 `owner.name:desc`（操作数注释等已拼装文本）。
    std::string member(const std::string& owner, const std::string& name,
                       const std::string& desc) const;

    ///解析既有拼装文本 `owner.name:desc`（各段已 printReadable）并渲染；
    ///解析失败原样返回。mbase DTO（info --instructions）预拼装注释用。
    std::string memberText(const std::string& composed) const;
};

///@brief 加载 sidecar 并构造显示上下文。
///
///sidecar 缺失 → 空表（非错误）；schema 错误 → stderr 告警（共享键
///mjt.cmd.shadow.err.sidecar_invalid）+ 空表，分析继续（显示层失配容忍）。
ShadowDisplay loadShadowDisplay(const std::string& jarPath, const std::string& explicitPath,
                                bool noShadow);

///@brief jasm 文本 shadow 改写（mbase rewriteJasmForDisplay 的 std::string 桥接）。
std::string rewriteJasmForDisplay(const ShadowDisplay& display, const std::string& jasmText,
                                  const std::string& ownerClass);

}   //namespace mjt
