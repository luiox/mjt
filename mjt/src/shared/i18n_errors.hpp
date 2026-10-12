#pragma once

//i18n_errors.hpp
//
//mjt 共享层错误的 i18n 构造:同一处错误消息同时流向 CLI stderr 与 MCP JSON
//两个呈现面,故在构造时按进程 locale 渲染(CLI 进程 = --lang/MJT_LANG 用户
//选择;mjt-mcp 进程 = main 固定 set_lang("en_US"))。key 进 mjt_shared 翻译表
//(translations/*.lang,rule i18n.embed-lang 嵌入,注册由各 binary main 完成)。

#include <initializer_list>
#include <string>
#include <string_view>

#include "libca/core/result.hpp"
#include "libca/core/status.hpp"
#include "libca/i18n/i18n.hpp"

namespace mjt::shared {

///经 i18n 表构造错误 Status:key 未命中任何语言时回退 key 原样(与 libi18n
///语义一致,便于发现漏录)。
inline ca::Status errStatus(ca::StatusCode code, const char* key,
                            std::initializer_list<std::string_view> args = {})
{
    return ca::ErrStatus(code, ca::i18n::trf(key, args).to_std_string());
}

///渲染为 std::string 的 trf,供需要拼接动态后缀(如 stderr 文本)的场合。
inline std::string trfStr(const char* key, std::initializer_list<std::string_view> args = {})
{
    return ca::i18n::trf(key, args).to_std_string();
}

}   //namespace mjt::shared
