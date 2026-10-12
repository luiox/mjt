#pragma once

//text_codec.hpp
//
//非受信窄字节 → 合法 UTF-8 的单一规范路径（反馈）。
//
//mjt 的两条呈现面（CLI 文案走 ca::i18n::trf、JSON 输出走 json::escape）都
//假定输入是合法 UTF-8：前者遇非法字节经 Utf8StringBuilder::build_or_empty
//软失败成空串（错误文案被静默吞掉，只剩一个换行——异常①/bug1 的
//静默机理）；后者逐字节转义出 mojibake/`\ufffd` 拼盘（bug2 的"GBK 窄
//字节再当 UTF-8 转义"）。因此所有来源不明的窄字节（子进程 stderr、混淆器
//写进常量池的非 MUTF8 字节等）在进入文案/JSON 管线前必须先经本头规范化。
//
//规范化口径（确定性、与进程 ACP 无关——mjt exe 已嵌 activeCodePage=UTF-8
//manifest，CP_ACP 不再反映系统真实 ANSI 代码页，不能作转码源）：
//1. 已是合法 UTF-8 → 原样保留（逐字节不改动）；
//2. 否则按 GBK/CP936 解码转出 UTF-8（实战语料 /：中文混淆器把 GBK
//窄字节写进常量池/经 JDK17 子进程 stderr 输出本地化文案；GBK 解码失败
//的孤立字节替换为 U+FFFD）；
//3. 非 Windows 平台无码表基础设施，退化为逐非法字节 U+FFFD（POSIX 语料
//通常本就是 UTF-8，仅作兜底）。
//
//纯 header-only：消费方为 mjt shared 层（decompiler/string_extractor）与
//libjclassfile 宽容解码回调（经 std::function 注入，避免 libs 反向依赖 mjt）。

#include <string>
#include <string_view>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "libca/str/utf8_util.hpp"

namespace mjt::shared::text {

///@brief 判断字节串是否为合法 UTF-8。
inline bool isValidUtf8(std::string_view bytes) noexcept
{
    return ca::str::utf8_is_valid(reinterpret_cast<const ca::u8*>(bytes.data()), bytes.size());
}

namespace detail {

///逐字节兜底：非法 UTF-8 单字节替换为 U+FFFD，合法子序列原样保留。
inline std::string lossyUtf8(std::string_view bytes)
{
    constexpr const char* kReplacement = "\xEF\xBF\xBD";   //U+FFFD
    std::string           out;
    out.reserve(bytes.size() + bytes.size() / 8 + 4);
    for (std::size_t i = 0; i < bytes.size();) {
        const auto* seq   = reinterpret_cast<const ca::u8*>(bytes.data() + i);
        const std::size_t count = ca::str::utf8_count_code_points(seq, 1);
        //单字节序列要么 ASCII 合法（计数 1），要么非法（计数 0）：多字节序列
        //的首字节单独看必然非法，逐字节口径即"合法 ASCII 原样、其余 U+FFFD"。
        if (count == 1) {
            out.push_back(bytes[i]);
        }
        else {
            out.append(kReplacement, 3);
        }
        ++i;
    }
    return out;
}

#if defined(_WIN32)
///窄字节按指定 Windows 代码页 → UTF-8。转换不掉的字符由系统替换为默认
///替换字符（U+FFFD 语义），绝不抛出/返回空。
inline std::string fromCodePage(std::string_view bytes, UINT codePage)
{
    if (bytes.empty()) return {};
    const int wideLength = MultiByteToWideChar(codePage, 0, bytes.data(),
                                               static_cast<int>(bytes.size()), nullptr, 0);
    if (wideLength <= 0) return lossyUtf8(bytes);
    std::wstring wide(static_cast<std::size_t>(wideLength), L'\0');
    MultiByteToWideChar(codePage, 0, bytes.data(), static_cast<int>(bytes.size()), wide.data(),
                        wideLength);

    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, nullptr, 0,
                                               nullptr, nullptr);
    if (utf8Length <= 0) return lossyUtf8(bytes);
    std::string out(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLength, out.data(), utf8Length, nullptr,
                        nullptr);
    return out;
}
#endif

}   //namespace detail

///@brief 把来源不明的窄字节规范化为合法 UTF-8（单一规范路径，见文件头）。
///
///合法 UTF-8 原样返回（保证既有 UTF-8 语料逐字节不改动）；否则按 GBK/CP936
///转码；仍不可解的孤立字节以 U+FFFD 替换。返回值恒为合法 UTF-8，可安全进入
///ca::i18n::trf 与 json::escape。
inline std::string toCanonicalUtf8(std::string_view bytes)
{
    if (isValidUtf8(bytes)) return std::string(bytes);
#if defined(_WIN32)
    auto converted = detail::fromCodePage(bytes, 936);
    return isValidUtf8(converted) ? converted : detail::lossyUtf8(bytes);
#else
    return detail::lossyUtf8(bytes);
#endif
}

}   //namespace mjt::shared::text
