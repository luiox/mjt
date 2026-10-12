#pragma once

//mbase/cli 框架文本翻译表的接入入口。
//
//各产品线 main 在 ca::i18n::init 时并入本表(ArgParser 的 help/错误文案即随
//语言本地化):
//
//#include "mbase/cli/i18n_tables.hpp"
//#include "i18n_mjt_translations.generated.hpp" //线主表
//ca::i18n::init({mjt::base::cli::kCliI18nTableSet, kI18nTableSet_mjt},
//{"MJT_LANG"});
//
//为什么不做静态自注册:注册器所在 obj 无被引用符号,链接器会整段丢弃
//(MSVC 按需拉取),不可靠;显式 init 是 libi18n 的既有约定。
//
//生成头 i18n_mbase_translations.generated.hpp 位于 mbase target 的
//autogendir(rule i18n.embed-lang 以 public includedir 暴露),仅依赖 mbase
//的 target 可见。

#include "i18n_mbase_translations.generated.hpp"
#include "libca/i18n/i18n.hpp"

namespace mjt::base::cli {

///mbase 框架文本表(mbase.cli.*);后注册的产品线主表可覆盖同 key。
inline constexpr ca::i18n::TableSet kCliI18nTableSet = kI18nTableSet_mbase;

}   //namespace mjt::base::cli
