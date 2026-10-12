#pragma once

#include <string>
#include <vector>

#include "service.hpp"

namespace mjt::mcp::detail {

///@brief unified-diff 中一行的类型与内容（已剥离行首 ` `/`-`/`+` marker）。
struct DiffLine
{
    enum class Kind
    {
        Context,
        Removed,
        Added
    };
    Kind        kind = Kind::Context;
    std::string text;
};

///@brief 一个 unified-diff hunk 的解析结果。
///
///`oldStart`/`oldCount` 来自 `@@ -a,b @@` 头；`lines` 严格保留 diff 内的原始
///交错顺序（context/removed/added 混排），applier 据此重建期望 base 序列与输出序列。
struct DiffHunk
{
    ///@brief `@@ -oldStart,oldCount +newStart,newCount @@` 中的旧行号（1-based）。
    ca::usize oldStart = 0;
    ///@brief 旧文件的行数声明；缺省视为 1。
    ca::usize oldCount = 1;
    ///@brief `@@ -oldStart,oldCount +newStart,newCount @@` 中的新行号（1-based）。
    ca::usize newStart = 0;
    ///@brief 新文件的行数声明；缺省视为 1。
    ca::usize newCount = 1;
    ///@brief 按出现顺序排列的行体（context/removed/added 交错）。
    std::vector<DiffLine> lines;
};

///@brief 严格 unified-diff 解析与应用结果。
struct DiffResult
{
    ///@brief 应用成功时为完整新文本；失败时不应使用，先看 `error`。
    std::string text;
    ///@brief 解析或应用阶段的错误描述；成功时为空。
    std::string error;
    ///@brief 失败时给出失败 hunk 的 1-based 序号；成功时为 0。
    ca::usize failedHunk = 0;
};

///@brief 把 unified-diff 文本解析为有序 hunk 列表。
///
///仅接受标准 unified-diff：`---`/`+++` 文件头（可选）后跟若干 `@@ ... @@` hunk，
///每个 hunk 体由以 ` `（context）、`-`（删除）、`+`（新增）开头的行组成。`\` 开头
///的行（如 `\ No newline at end of file`）作为提示忽略。不支持 fuzz/3way。
///行尾的 `\r` 会被剥掉（CRLF diff 与 LF base 可互操作；base 不做归一化）。
///
///@param diff 原始 diff 文本（按 `\n` 切分）。
///@param hunks [out] 解析得到的 hunk 列表。
///@param error [out] 解析失败时的描述。
///@return true 解析成功；false 解析失败（hunks 内容未定义）。
bool parseUnifiedDiff(const std::string& diff, std::vector<DiffHunk>& hunks, std::string& error);

///@brief 把若干 hunk 严格应用到 base 文本，生成新文本。
///
///严格匹配：每个 hunk 的 context+removed 行（按出现顺序）必须在 base 的
///`oldStart-1` 起始处逐行相等，否则在 `result.error` 里给出失败 hunk 序号与期望
///首行，`failedHunk` 填该 hunk 的 1-based 序号。hunk 之间要求行号单调不回退，
///重叠会判失败。
///
///@param base 原始文本，按 `\n` 切分（保留末尾空行语义，见 splitLines）。
///@param hunks 已解析的有序 hunk。
///@return 应用结果；`error` 为空表示成功。
DiffResult applyHunks(const std::string& base, const std::vector<DiffHunk>& hunks);

///@brief 解析并应用 unified-diff 的便捷入口（parseUnifiedDiff + applyHunks）。
DiffResult applyUnifiedDiff(const std::string& base, const std::string& diff);

///@brief 把文本按 `\n` 切分为行列表。
///
///与多数 diff 工具一致：末尾若有 `\n`，则不产生末尾空行。例如 `"a\nb\n"` 与
///`"a\nb"` 均得到 `["a","b"]`（二者在 splitLines 视角下不可区分，applier 与
///joinLines 配对使用以保证 round-trip 时末尾统一带 `\n`）。
std::vector<std::string> splitLines(const std::string& text);

///@brief 用 `\n` 连接行列表并补末尾换行；空列表返回空串。
std::string joinLines(const std::vector<std::string>& lines);

}   //namespace mjt::mcp::detail
