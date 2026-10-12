#include "diff.hpp"

#include <charconv>
#include <sstream>
#include <system_error>
#include <utility>

namespace mjt::mcp::detail {

namespace {

///@brief 解析 `@@ -oldStart,oldCount +newStart,newCount @@` 头中的单个 `(start,count)`。
///
///`start` 必须为非空十进制；`,count` 可缺省（默认 1）。返回 false 表示格式非法。
bool parseHunkRange(const std::string& token, ca::usize& start, ca::usize& count, std::string& error)
{
    const auto comma = token.find(',');
    const auto startStr = token.substr(0, comma);
    if (startStr.empty()) {
        error = "hunk range is missing a start number";
        return false;
    }
    ca::usize parsed = 0;
    const auto res = std::from_chars(startStr.data(), startStr.data() + startStr.size(), parsed);
    if (res.ec != std::errc() || res.ptr != startStr.data() + startStr.size()) {
        error = "hunk range start is not a non-negative integer: " + startStr;
        return false;
    }
    start = parsed;
    if (comma == std::string::npos) {
        count = 1;
        return true;
    }
    const auto countStr = token.substr(comma + 1);
    if (countStr.empty()) {
        count = 1;
        return true;
    }
    ca::usize parsedCount = 0;
    const auto res2 = std::from_chars(countStr.data(), countStr.data() + countStr.size(), parsedCount);
    if (res2.ec != std::errc() || res2.ptr != countStr.data() + countStr.size()) {
        error = "hunk range count is not a non-negative integer: " + countStr;
        return false;
    }
    count = parsedCount;
    return true;
}

///@brief 从 `@@ -a,b +c,d @@ <section>` 行提取四个数值，section 部分忽略。
bool parseHunkHeader(const std::string& line, DiffHunk& hunk, std::string& error)
{
    //形如 "@@ -10,5 +12,7 @@" 或 "@@ -1 +1 @@"，行尾可能带 section 文本。
    const auto secondAt = line.find("@@", 2);
    if (line.compare(0, 2, "@@") != 0 || secondAt == std::string::npos) {
        error = "expected hunk header beginning with '@@ ... @@'";
        return false;
    }
    const auto body = line.substr(2, secondAt - 2);
    //body 形如 " -10,5 +12,7 "，以空格分隔成两个 token。
    std::istringstream iss(body);
    std::string oldToken;
    std::string newToken;
    iss >> oldToken >> newToken;
    if (oldToken.empty() || newToken.empty() || oldToken[0] != '-' || newToken[0] != '+') {
        error = "hunk header must contain both -old and +new ranges";
        return false;
    }
    if (!parseHunkRange(oldToken.substr(1), hunk.oldStart, hunk.oldCount, error)) return false;
    if (!parseHunkRange(newToken.substr(1), hunk.newStart, hunk.newCount, error)) return false;
    if (hunk.oldStart == 0 && hunk.oldCount != 0) {
        error = "hunk old range start must be 1-based (0 only allowed for empty file)";
        return false;
    }
    if (hunk.newStart == 0 && hunk.newCount != 0) {
        error = "hunk new range start must be 1-based (0 only allowed for empty file)";
        return false;
    }
    return true;
}

}   //namespace

bool parseUnifiedDiff(const std::string& diff, std::vector<DiffHunk>& hunks, std::string& error)
{
    hunks.clear();
    error.clear();
    auto lines = splitLines(diff);
    //CRLF 归一化：剥掉 diff 行尾的 '\r'，Windows 客户端的 CRLF diff 才能匹配
    //LF base（class_jasm canonical 输出为 LF）。base 一侧不动，"未触及部分
    //逐字节不变"的承诺不受影响。
    for (auto& line : lines) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
    }
    ca::usize index = 0;
    //跳过文件头：--- / +++ 成对出现，遇到首个 @@ 停止。允许只有 @@（无文件头）。
    while (index < lines.size() && lines[index].rfind("@@", 0) != 0) {
        ++index;
    }
    DiffHunk current;
    bool inHunk = false;
    for (; index < lines.size(); ++index) {
        const auto& line = lines[index];
        if (line.rfind("@@", 0) == 0) {
            if (inHunk) {
                hunks.push_back(std::move(current));
                current = DiffHunk{};
            }
            if (!parseHunkHeader(line, current, error)) {
                error = "hunk #" + std::to_string(hunks.size() + 1) + " header: " + error;
                return false;
            }
            inHunk = true;
            continue;
        }
        if (!inHunk) {
            //已跳过文件头后仍出现非 hunk 内容属于格式错误。
            error = "unexpected line outside a hunk: " + line;
            return false;
        }
        if (line == "\\ No newline at end of file" || line.rfind("\\ ", 0) == 0) {
            //提示行，忽略；当前实现不区分末尾无换行的语义差异。
            continue;
        }
        if (line.empty()) {
            //纯空行视为 context 行（行体为空）。部分 diff 工具会把空 context 行
            //输出为单个空格，但空串在此处等价：行体为空。
            current.lines.push_back({DiffLine::Kind::Context, {}});
            continue;
        }
        const auto marker = line[0];
        if (marker == ' ') current.lines.push_back({DiffLine::Kind::Context, line.substr(1)});
        else if (marker == '-') current.lines.push_back({DiffLine::Kind::Removed, line.substr(1)});
        else if (marker == '+') current.lines.push_back({DiffLine::Kind::Added, line.substr(1)});
        else {
            error = "hunk line has an invalid prefix (must be ' ', '-' or '+'): " + line;
            return false;
        }
    }
    if (inHunk) hunks.push_back(std::move(current));
    return true;
}

DiffResult applyHunks(const std::string& base, const std::vector<DiffHunk>& hunks)
{
    DiffResult result;
    const auto baseLines = splitLines(base);
    ca::usize cursor = 0;   //base 中下一个待处理行（0-based）
    std::vector<std::string> output;
    output.reserve(baseLines.size());
    for (ca::usize h = 0; h < hunks.size(); ++h) {
        const auto& hunk = hunks[h];
        const ca::usize hunkBase = hunk.oldStart == 0 ? 0 : hunk.oldStart - 1;
        //hunk 之间要求行号单调不回退且不重叠：下一个 oldStart 必须落在已消费行之后。
        if (hunkBase < cursor) {
            std::ostringstream msg;
            msg << "hunk #" << (h + 1) << " overlaps or regresses: starts at base line "
                << (hunkBase + 1) << " but the previous hunk already consumed through line "
                << cursor;
            result.error = msg.str();
            result.failedHunk = h + 1;
            return result;
        }
        //hunk 起点必须在 base 末尾之内：越界时下面的拷贝循环会读 baseLines[size]。
        if (hunkBase > baseLines.size()) {
            std::ostringstream msg;
            msg << "hunk #" << (h + 1) << " starts at base line " << (hunkBase + 1)
                << " but only " << baseLines.size() << " lines are available";
            result.error = msg.str();
            result.failedHunk = h + 1;
            return result;
        }
        //1. 拷贝 hunk 之前的未改动行。
        for (; cursor < hunkBase; ++cursor) output.push_back(baseLines[cursor]);
        //2. 校验：context+removed 行（按原始交错顺序）必须在 base 的 hunkBase 处
        //逐行相等。added 行不参与 base 匹配。
        ca::usize baseProbe = hunkBase;
        for (const auto& entry : hunk.lines) {
            if (entry.kind == DiffLine::Kind::Added) continue;
            if (baseProbe >= baseLines.size()) {
                std::ostringstream msg;
                msg << "hunk #" << (h + 1) << " expects base line " << (baseProbe + 1)
                    << " but only " << baseLines.size() << " lines are available";
                result.error = msg.str();
                result.failedHunk = h + 1;
                return result;
            }
            if (baseLines[baseProbe] != entry.text) {
                std::ostringstream msg;
                msg << "hunk #" << (h + 1) << " context mismatch at base line "
                    << (baseProbe + 1) << ": expected " << entry.text << " but found "
                    << baseLines[baseProbe];
                result.error = msg.str();
                result.failedHunk = h + 1;
                return result;
            }
            ++baseProbe;
        }
        cursor = baseProbe;
        //3. 追加输出行：context 行原样保留，added 行写入，removed 行丢弃。
        for (const auto& entry : hunk.lines) {
            if (entry.kind == DiffLine::Kind::Removed) continue;
            output.push_back(entry.text);
        }
    }
    //4. 拷贝最后一个 hunk 之后的所有 base 行。
    for (; cursor < baseLines.size(); ++cursor) output.push_back(baseLines[cursor]);
    result.text = joinLines(output);
    return result;
}

DiffResult applyUnifiedDiff(const std::string& base, const std::string& diff)
{
    DiffResult result;
    std::vector<DiffHunk> hunks;
    if (!parseUnifiedDiff(diff, hunks, result.error)) return result;
    return applyHunks(base, hunks);
}

std::vector<std::string> splitLines(const std::string& text)
{
    std::vector<std::string> lines;
    std::string::size_type pos = 0;
    while (pos < text.size()) {
        const auto next = text.find('\n', pos);
        if (next == std::string::npos) {
            lines.push_back(text.substr(pos));
            return lines;
        }
        lines.push_back(text.substr(pos, next - pos));
        pos = next + 1;
    }
    //末尾恰好是 '\n' 时不追加空行；text 为空时返回空列表。
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines)
{
    if (lines.empty()) return {};
    std::string out;
    ca::usize total = lines.size();
    for (const auto& line : lines) total += line.size();
    out.reserve(total);
    for (const auto& line : lines) {
        out += line;
        out += '\n';
    }
    return out;
}

}   //namespace mjt::mcp::detail
