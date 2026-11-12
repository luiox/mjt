/// @file        cmd_hexdump.cpp
/// @brief       `mjt hexdump` 命令核心：单类原始字节的 xxd 风格十六进制转储。
///
/// 类选择走 workspace listClassSummaries（URI / 内名精确命中优先，无精确命中退首个子串候选），再经 readClassBytes 读取类原始字节。渲染为纯函数：偏移、长度、宽度窗口在 size_t 域计算（offset + length 在 int 域可溢出）。
#include "commands.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "shared/i18n_errors.hpp"
#include "shared/output_sink.hpp"
#include "workspace/analysis_ops.hpp"
#include "workspace/workspace.hpp"
#include "workspace/workspace_types.hpp"

namespace mjt::cli {
namespace {

/// Utf8String/UTF-8 std::string → 借用视图（info / strings 命令同此构造）。
ca::str::Utf8StringRef refOf(const std::string& value)
{
    return ca::str::Utf8StringRef::from_data(reinterpret_cast<const ca::u8*>(value.data()),
                                             value.size());
}

/// HexdumpError 便捷构造（i18n 渲染）。
HexdumpError hexdumpError(HexdumpError::Kind kind, const char* key,
                          std::initializer_list<std::string_view> args = {})
{
    HexdumpError error;
    error.kind    = kind;
    error.message = shared::trfStr(key, args);
    return error;
}

/// 类选择：URI / 内名精确命中优先；无精确命中取首个包含选择器的子串候选（listClassSummaries 序）。命中返回该摘要，未命中返回 nullptr。
const workspace::ClassSummary* selectClass(const std::vector<workspace::ClassSummary>& classes,
                                           const std::string&                         selector)
{
    for (const auto& summary : classes) {
        if (summary.resourceUri.to_std_string() == selector ||
            summary.internalName.to_std_string() == selector) {
            return &summary;
        }
    }
    for (const auto& summary : classes) {
        if (summary.resourceUri.to_std_string().find(selector) != std::string::npos ||
            summary.internalName.to_std_string().find(selector) != std::string::npos) {
            return &summary;
        }
    }
    return nullptr;
}

}   // namespace

ca::Result<HexdumpReport, HexdumpError> collectHexdump(const std::string& jarPath,
                                                       const std::string& classSelector,
                                                       const HexdumpOptions& options)
{
    // 旗标值防御：负 offset 会负索引字节向量；width<=0 让渲染步进不推进；length<-1 与 -1（整个 class）哨兵冲突，提前拒绝而非崩溃 / 卡死。
    if (options.offset < 0)
        return ca::Err(hexdumpError(HexdumpError::Kind::InvalidArgument,
                                    "mjt.cmd.hexdump.err.offset_negative"));
    if (options.width <= 0)
        return ca::Err(hexdumpError(HexdumpError::Kind::InvalidArgument,
                                    "mjt.cmd.hexdump.err.width_not_positive"));
    if (options.length < -1)
        return ca::Err(hexdumpError(HexdumpError::Kind::InvalidArgument,
                                    "mjt.cmd.hexdump.err.length_invalid"));
    if (classSelector.empty())
        return ca::Err(hexdumpError(HexdumpError::Kind::InvalidArgument,
                                    "mjt.cmd.hexdump.err.missing_class"));

    workspace::WorkspaceBase ws;
    auto opened = ws.open(refOf(jarPath));
    if (opened.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(opened).unwrap_err();
        return ca::Err(hexdumpError(HexdumpError::Kind::Runtime,
                                    "mjt.cmd.hexdump.err.open_failed", {info.message}));
    }

    auto summaries = workspace::listClassSummaries(*ws.workingContainer());
    if (summaries.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(summaries).unwrap_err();
        return ca::Err(hexdumpError(HexdumpError::Kind::Runtime,
                                    "mjt.cmd.hexdump.err.open_failed", {info.message}));
    }

    // 类表保持本栈帧持有：selectClass 返回表内指针，不能指向 unwrap 的临时。
    const auto classes = std::move(summaries).unwrap();
    const workspace::ClassSummary* found = selectClass(classes, classSelector);
    if (found == nullptr)
        return ca::Err(hexdumpError(HexdumpError::Kind::ClassNotFound,
                                    "mjt.cmd.hexdump.err.class_not_found", {classSelector}));

    auto read = workspace::readClassBytes(*ws.workingContainer(), found->resourceUri.ref());
    if (read.is_err()) {
        workspace::WorkspaceErrorInfo info = std::move(read).unwrap_err();
        return ca::Err(hexdumpError(HexdumpError::Kind::Runtime,
                                    "mjt.cmd.hexdump.err.open_failed", {info.message}));
    }
    auto bytes = std::move(read).unwrap();

    HexdumpReport report;
    report.jarPath      = jarPath;
    report.classUri     = found->resourceUri.to_std_string();
    report.internalName = bytes.internalName.to_std_string();
    report.bytes        = std::move(bytes.bytes);
    return ca::Ok(std::move(report));
}

void renderHexdumpText(const HexdumpReport& report, const HexdumpOptions& options,
                       OutputSink& sink)
{
    // 渲染纯函数的防御（与 collect 校验一致）：width<=0 步进不推进，负 offset 负索引，直接空输出（collect 是 CLI 路径的校验关口）。
    if (options.width <= 0 || options.offset < 0) return;

    const auto& data = report.bytes;
    // 偏移与长度按 size_t 计算：offset+length 在 int 域可溢出（有符号溢出是 UB，实际表现为负 end 静默无输出）。
    const std::size_t offsetS = static_cast<std::size_t>(options.offset);
    if (offsetS >= data.size()) {
        sink.puts(shared::trfStr("mjt.cmd.hexdump.out.offset_past_end"));
        return;
    }
    const std::size_t total  = options.length < 0 ? data.size()
                                                  : static_cast<std::size_t>(options.length);
    const std::size_t widthS = static_cast<std::size_t>(options.width);
    const std::size_t end    = std::min(offsetS + total, data.size());

    for (std::size_t i = offsetS; i < end; i += widthS) {
        sink.printf("%08zx  ", i);

        const std::size_t lineEnd = std::min(i + widthS, end);

        for (std::size_t j = i; j < lineEnd; j++) {
            sink.printf("%02x ", data[j]);
            if ((j - i + 1) == widthS / 2 && j < lineEnd - 1) sink.printf(" ");
        }

        // 短行补齐：每缺一字节三空格，半行宽处再多一空格（与满行的半行分隔位对齐）。
        const std::size_t printed = lineEnd - i;
        if (printed < widthS) {
            for (std::size_t p = printed; p < widthS; p++) {
                sink.printf("   ");
                if (p == widthS / 2) sink.printf(" ");
            }
        }

        if (options.showAscii) {
            sink.printf(" |");
            for (std::size_t j = i; j < lineEnd; j++) {
                const char c = static_cast<char>(data[j]);
                sink.printf("%c", (c >= 32 && c < 127) ? c : '.');
            }
            for (std::size_t p = lineEnd - i; p < widthS; p++) sink.printf(" ");
            sink.printf("|");
        }
        sink.printf("\n");
    }
}

}   // namespace mjt::cli
