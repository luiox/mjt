#pragma once

//── mjt 共享 CLI 选项 preset ───────────────────────────────────────
//
//收敛各 cmd_*.cpp 重复声明的共享旗标（--input/--output/--class/--method/
//--verbose/--json/--format/--shadow 系列）：旗标名、别名、OptKind、metavar
//惯例、必填性与 i18n key 后缀（param.<flag>.desc）在此单源定义，命令侧只
//传 key 前缀与差异项。
//
//行为红线：本头文件是纯组织性收敛（约定的落地），不改变任何旗标的
//可见行为——各命令的 metavar/required/别名/帮助文案原样保留（历史命令间
//存在的有意差异也原样参数化，不做统一）。
//
//用法（帮助文案取 <keyPrefix>.param.<flag>.desc，两语言表见
//src/shared/translations/*.lang）：
//cli::ArgParser p(...);
//mjt::cli_presets::addInputOptions(p, "mjt.cmd.info"); //-i <jar>，必填
//mjt::cli_presets::addInputOptions(p, "mjt.cmd.cfg", "<path>", false);
//mjt::cli_presets::addOutputOptions(p, "mjt.cmd.cfg");
//mjt::cli_presets::addShadowOptions(p, "mjt.cmd.disasm"); //含 --no-shadow
//
//新增共享旗标一律加在本表，不许再回命令文件里手写声明。

#include <string>
#include <vector>

#include "libca/i18n/i18n.hpp"
#include "mbase/cli/arg_parser.hpp"

namespace mjt::cli_presets {

namespace detail {

///拼 key 前缀与旗标 key 后缀并取译文。key 后缀约定：param.<flag>.desc。
inline std::string trFlag(const std::string& keyPrefix, const char* flagKey)
{
    return ca::i18n::tr((keyPrefix + ".param." + flagKey + ".desc").c_str());
}

}   //namespace detail

///--input/-i：待分析输入。默认 metavar "<jar>"、必填；接受 .class/其他路径的
///命令传各自 metavar（"<path>"/"<file>"/"<class|jar>" 等）与 required。
inline mjt::base::cli::ArgParser& addInputOptions(mjt::base::cli::ArgParser& p,
                                                      const std::string&  keyPrefix,
                                                      const char*         metavar  = "<jar>",
                                                      bool                required = true)
{
    using mjt::base::cli::OptKind;
    return p.add({"--input", "-i"}, OptKind::String, metavar, detail::trFlag(keyPrefix, "input"),
                 required);
}

///--output/-o：输出路径。metavar 各命令原生有差异（<path>/<file>/<jar>/<dir>/
///<file|dir>），decompile 传 required=true。
inline mjt::base::cli::ArgParser& addOutputOptions(mjt::base::cli::ArgParser& p,
                                                       const std::string& keyPrefix,
                                                       const char*        metavar  = "<path>",
                                                       bool               required = false)
{
    using mjt::base::cli::OptKind;
    return p.add({"--output", "-o"}, OptKind::String, metavar, detail::trFlag(keyPrefix, "output"),
                 required);
}

///--class：目标类过滤。cfg/dataflow/opt 系用内部名 "<name>"，info/disasm/xref
///系用 "<class>"；xref 无 -c 别名（shortAlias=false），disasm 传 required=true。
inline mjt::base::cli::ArgParser& addClassOptions(mjt::base::cli::ArgParser& p,
                                                      const std::string& keyPrefix,
                                                      const char*        metavar    = "<name>",
                                                      bool               shortAlias = true,
                                                      bool               required   = false)
{
    using mjt::base::cli::OptKind;
    return p.add(shortAlias ? std::vector<std::string>{"--class", "-c"}
                            : std::vector<std::string>{"--class"},
                 OptKind::String, metavar, detail::trFlag(keyPrefix, "class"), required);
}

///--method：目标方法过滤。cfg/dataflow 系 metavar "<name[:descriptor]>"，opt 用
///"<spec>"，disasm/xref 用 "<name>" 且无 -m 别名（shortAlias=false）。
inline mjt::base::cli::ArgParser& addMethodOptions(mjt::base::cli::ArgParser& p,
                                                       const std::string& keyPrefix,
                                                       const char*        metavar    =
                                                           "<name[:descriptor]>",
                                                       bool shortAlias = true)
{
    using mjt::base::cli::OptKind;
    return p.add(shortAlias ? std::vector<std::string>{"--method", "-m"}
                            : std::vector<std::string>{"--method"},
                 OptKind::String, metavar, detail::trFlag(keyPrefix, "method"));
}

///--verbose/-v：详细输出。jar fix 的 verbose 无 -v 别名（shortAlias=false）。
inline mjt::base::cli::ArgParser& addVerboseOptions(mjt::base::cli::ArgParser& p,
                                                        const std::string& keyPrefix,
                                                        bool               shortAlias = true)
{
    using mjt::base::cli::OptKind;
    return p.add(shortAlias ? std::vector<std::string>{"--verbose", "-v"}
                            : std::vector<std::string>{"--verbose"},
                 OptKind::Flag, "", detail::trFlag(keyPrefix, "verbose"));
}

///--json：JSON 输出开关（各命令帮助文案不同，key 前缀区分）。
inline mjt::base::cli::ArgParser& addJsonOptions(mjt::base::cli::ArgParser& p,
                                                     const std::string& keyPrefix)
{
    using mjt::base::cli::OptKind;
    return p.add({"--json"}, OptKind::Flag, "", detail::trFlag(keyPrefix, "json"));
}

///--format：输出格式（<fmt>）。候选值语义写在各命令帮助文案里。
inline mjt::base::cli::ArgParser& addFormatOptions(mjt::base::cli::ArgParser& p,
                                                       const std::string& keyPrefix)
{
    using mjt::base::cli::OptKind;
    return p.add({"--format"}, OptKind::String, "<fmt>", detail::trFlag(keyPrefix, "format"));
}

///shadow 系列：--shadow <file>（sidecar 路径）+ 可选 --no-shadow（关闭 shadow
///名展示）。disasm/info/rename 两者成对出现；shadow 命令只有 --shadow
///（noShadowFlag=false）。
inline mjt::base::cli::ArgParser& addShadowOptions(mjt::base::cli::ArgParser& p,
                                                       const std::string& keyPrefix,
                                                       bool               noShadowFlag = true)
{
    using mjt::base::cli::OptKind;
    p.add({"--shadow"}, OptKind::String, "<file>", detail::trFlag(keyPrefix, "shadow"));
    if (noShadowFlag) {
        p.add({"--no-shadow"}, OptKind::Flag, "", detail::trFlag(keyPrefix, "no_shadow"));
    }
    return p;
}

}   //namespace mjt::cli_presets
