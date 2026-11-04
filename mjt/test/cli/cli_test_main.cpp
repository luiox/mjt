/// @file        cli_test_main.cpp
/// @brief       CLI unittest 入口：test_helper 装配 + i18n 表注册（命令核心
///              的 trfStr 依赖进程级翻译表）。
#include <gtest/gtest.h>

#include "i18n_mjt_cli_core_translations.generated.hpp"
#include "i18n_mjt_shared_translations.generated.hpp"
#include "libca/i18n/i18n.hpp"
#include "test_helper.hpp"

int main(int argc, char** argv)
{
    test::setup("mjt");
    // CLI 核心渲染按 en_US 断言（与 exe 缺省语言一致）。
    ca::i18n::init({kI18nTableSet_mjt_shared, kI18nTableSet_mjt_cli_core}, {"MJT_LANG"});
    ca::i18n::set_lang("en_US");
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
