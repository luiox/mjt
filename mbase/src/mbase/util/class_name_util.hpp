#pragma once

#include <string>

#include "libca/str/utf8_string.hpp"

namespace mjt::base {

///@brief 把 class internal name（多字节 UTF-8）转义为可安全打印的 ASCII 串。
///
///进度条 / 日志场景用。规则（见 mbase/doc/_archive/pass-progress-design.md §4）：
///- 白名单码点（ASCII 子集）原样输出：`A-Za-z0-9_ $ / .`
///（`.` 保留：entry 名里的 `.class` 后缀、`META-INF/MANIFEST.MF` 等正常含 `.`）
///- 其余每个码点 → `\uxxxx`（4 位小写 hex）；补充平面（>0xFFFF）→ `\Uxxxxxxxx`（8 位）
///- 转义后超 200 字符才右截断为 198 + `..`
///
///实现要点：**必须用 Utf8Iterator range-for**（O(1) 步进，整体 O(n)），
///禁止 `code_point_at(i)` 下标遍历——后者单次 O(n)，循环 O(n²)，
///在 jar 处理热路径上是性能陷阱。
///
///@param name raw class internal name（来自 ResourceContainer 的 uri key）
///@return 可安全打印的 ASCII 串
std::string escapeClassName(ca::str::Utf8StringRef name);

}   //namespace mjt::base
