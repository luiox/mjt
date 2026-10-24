#pragma once

// pysdk_limits.hpp — 脚本路径资源上限：整体 wall-clock deadline 与协作式取消。
//
// 双层机制（取舍全录 doc_ai/pysdk-script-limits.md）：
//   协作层  deadlineCheck() 检查点由长操作循环周期调用（facade flush/verify/
//           sanitize 等的逐类/逐条目粒度），嵌入层 trace 函数逐 Python 行事件
//           兜纯 Python 死循环；越限抛 DeadlineExceeded（绑定层注册为
//           KeyboardInterrupt 子类），进程内可单测。
//   硬层    startDeadlineWatchdog 起独立线程，到点仍未 stop 就执行注入的
//           exit hook（生产 = hardExitHook：打印原因后 _Exit(3)）。绝不从
//           第二线程 finalize CPython（GIL 与 finalize 的线程坑），硬层只能
//           整进程退，退出码 3 = 资源上限硬终止。
//
// 线程模型：deadline 是全局 steady-clock 绝对时点（atomic），set/check 任意
// 线程安全；watchdog 单飞（重复 start 先停旧再起新）。纯 C++，不依赖 Python
// 头——facade、pyd、嵌入宿主三方共用同一份状态。

#include <cstdint>
#include <functional>
#include <stdexcept>

namespace mjt::pysdk {

/// deadline 越限的协作层异常。绑定层注册为 KeyboardInterrupt 子类：运行时
/// 上限不该被脚本里顺手的 except Exception 吞掉。
class DeadlineExceeded : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// 设置整体 deadline：ms > 0 = 从现在起 ms 毫秒；ms < 0 = 立即过期（测试位）；
/// ms == 0 = 清除（回到无上限）。
void setDeadlineMs(std::int64_t ms);

/// 剩余毫秒：无上限 = -1；已过期 = 0；否则剩余毫秒。
std::int64_t deadlineRemainingMs();

/// 协作层检查点：deadline 已设且到点 → 抛 DeadlineExceeded。长操作循环内
/// 周期调用（粒度：每类/每条目；不逐指令——检查点密度换不回覆盖面）。
void deadlineCheck();

/// 硬层 watchdog 的到点回调。ms = 配置的 deadline 毫秒数（供文案）。
using DeadlineExitHook = std::function<void(std::int64_t ms)>;

/// 起硬层 watchdog：ms <= 0 为空操作；单飞（先停旧线程）。hook 到点在
/// watchdog 线程上执行，hook 自己决定是否退出进程。
void startDeadlineWatchdog(std::int64_t ms, DeadlineExitHook hook);

/// 停 watchdog（join；hook 已执行过则 join 立即返回）。
void stopDeadlineWatchdog();

/// 生产 hook：打印超限原因到 stderr 后 _Exit(3)。用 _Exit 不用 exit：跳过
/// atexit 与静态析构，绝不触发从第二线程 finalize 解释器的路径。
void hardExitHook(std::int64_t ms);

}   // namespace mjt::pysdk
