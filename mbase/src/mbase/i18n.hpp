#pragma once

//mbase 全局 i18n 兜底入口。
//
//各产品线 main 通常在启动时调用 ca::i18n::init 注册各自的
//翻译表;但 mbase 的静态库代码(pass registry / cli_runner / sir_verifier)
//可能被插件宿主等“未显式 init i18n”的环境消费。若这些层直接调用
//ca::i18n::tr,未注册 key 会原样返回(裸 key 泄漏到用户界面)。
//
//本头提供 ensureI18nBoot:首次调用时用 mbase 自带表(mbase.* 全部 key)
//兜底注册,保证描述/错误消息始终可解析(默认 zh_CN)。已 init 的宿主不受
//影响——init 后注册的表格仍按“后注册者胜”覆盖 mbase 默认文案。

#include "i18n_mbase_translations.generated.hpp"
#include "libca/i18n/i18n.hpp"

namespace mjt::base {

///若宿主未注册任何翻译表,用 mbase 自带表兜底注册(不覆盖已 init 的宿主)。
inline void ensureI18nBoot()
{
    if (!ca::i18n::initialized()) {
        ca::i18n::init({kI18nTableSet_mbase}, {});
    }
}

///mbase 文案查询:先确保兜底注册,再按当前语言查询。
inline const char* tr(const char* key)
{
    ensureI18nBoot();
    return ca::i18n::tr(key);
}

}   //namespace mjt::base