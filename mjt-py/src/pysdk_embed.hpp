#pragma once

// pysdk_embed.hpp — mjt --script 嵌入解释器（IDAPython 形态）。
//
// 纯 CPython API（不碰 pybind，保证宿主 exe 不依赖 pybind11 头）。
// 由宿主 main（MJT_WITH_PYSDK=1 门控）调用。

#include <string>
#include <vector>

namespace mjt::pysdk {

struct EmbedOptions
{
    std::string              scriptPath;    ///< 要执行的 .py 文件
    std::vector<std::string> scriptArgs;    ///< 透传给脚本的参数（sys.argv[1:]）
    bool                     interactive = false;   ///< -i：跑完转 REPL
};

/// python home（MJT_PYTHON_HOME env > 编译期 MJT_PYTHON_HOME_DEFAULT）。
std::string pythonHome();

/// exe 所在目录（mjt.pyd / mjt_util.py 的缺省搜索点）。
std::string exeDir();

/// 初始化前预载 python314.dll（Windows；ALTERED_SEARCH_PATH 从 home 解析）。
/// 返回 false = DLL 加载失败（home 不对/缺 DLL）。
bool pythonPreInit();

/// 初始化解释器（utf8_mode=1 + home），unittest 与 --script 共用。
/// 成功后调用方仍需 pythonAddExePath()。
bool pythonInit();

/// 收尾（进程退出前一次）。
int pythonFinalize();

/// 初始化后把 exe 目录插到 sys.path[0]（mjt.pyd / mjt_util.py 缺点位），
/// 并处理 MJT_PYSDK_PATH 追加点（分号分隔）。
void pythonAddExePath();

/// 嵌入初始化 + import mjt + 执行脚本。返回进程退出码。
int runEmbeddedScript(const EmbedOptions& options);

}   // namespace mjt::pysdk
