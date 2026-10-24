# 脚本路径资源上限与取消（mjt --script）

## 问题与范围

`mjt --script` 把嵌入解释器通电后，脚本在宿主进程里拥有与引擎同等的执行权：死循环、吃爆内存、海量输出都会让宿主陪葬。本文记录给脚本路径加的资源上限机制、关键取舍与验证方式。机制本体在 `pysdk_limits`（纯 C++ 状态与 watchdog）与 `pysdk_embed`（解释器侧装点），facade 长操作接入协作检查点。

## 机制：协作层 + 硬层

采用双层方案，原因是 CPython 从外部线程 finalize 是已知雷区：解释器可能卡在任一原生调用里且持有 GIL，第二线程此刻既拿不到 GIL 也无法安全 finalize，唯一安全的终止手段是整进程退出。

协作层负责「干净终止」：`deadlineCheck()` 检查点埋在 facade 长操作的循环里（flush 逐脏类、verify_container 逐条目、sanitize_lvt 逐类、import 入口、run_pass 入口），越限抛 `DeadlineExceeded`，绑定层注册为 `KeyboardInterrupt` 子类（`mjt.DeadlineExceeded`），脚本可以精确捕获，但不会被顺手的 `except Exception` 吞掉。嵌入层再装一个解释器 trace 函数逐 Python 行事件复核 deadline 与输出计数，兜住纯 Python 死循环（facade 检查点够不着的地方）。trace 只在配置了任一上限时安装，无上限脚本零开销。

硬层负责「兜底终止」：`startDeadlineWatchdog` 起独立线程，到点执行 exit hook；生产 hook 打印原因后 `_Exit(3)`。`_Exit` 跳过 atexit 与静态析构，绝不触碰解释器状态，从根上绕开外部线程 finalize 的坑。退出码 3 = 资源上限硬终止，与 1（脚本异常/协作层上限）、2（用法与环境错误）区分。硬层不紧贴 deadline 到点，而是加固定 5 秒宽限（deadline + 5000 毫秒）：协作层在 deadline 时刻中断后，进程还要走 traceback 打印与解释器收尾，实测 watchdog 紧贴 deadline 轮询会与自家干净退出竞速，退出码在 1 与 3 之间漂移；让出宽限后，协作层正常工作时恒有数个量级余量先退出，只有真卡死的进程才吃满宽限。

## 输出上限

`maxOutputBytes` 是协作层语义：嵌入层在脚本执行前把 `sys.stdout` 与 `sys.stderr` 换装计数 writer（累计 UTF-8 字节数），超限在 `write` 内抛 `KeyboardInterrupt` 族；计数格留在 `__main__._mjt_cap_cell`，trace 钩子逐行复核同一计数，脚本吞掉 writer 异常继续空转时仍有下一行事件兜底。stdout 与 stderr 共享同一个计数格。

接受的取舍：脚本重挂 `sys.stdout`、直写 `sys.stdout.buffer` 或原生句柄可以绕过字节上限。时间维度有硬层兜底，字节维度没有——不做进程级 fd 重定向（Windows 控制台管道化会破坏交互语义，且计数与终止仍需进程外组件，复杂度失真）。

## deadline 的口径

deadline 是整次 `--script` 会话的 wall-clock 上限（REPL 会话含在内），从宿主设置起到会话结束。CLI 旗标 `--deadline-ms` / `--max-output-bytes`（只认 `--script` 之前，`--script` 之后全是脚本参数），缺省读环境变量 `MJT_SCRIPT_DEADLINE_MS` / `MJT_SCRIPT_MAX_OUTPUT_BYTES`。脚本内也可用 `mjt.set_deadline_ms(ms)` 自调（大于 0 生效，小于 0 立即过期是测试位，0 清除），`mjt.DeadlineExceeded` 是捕获面。watchdog 在 start 时取定绝对时点，此后 `set_deadline_ms` 改表不会移动已起的 watchdog——每次 `--script` 只装一轮，这个偏差是接受的简化。

## 检查点粒度

检查点落在逐类/逐条目循环粒度，不逐指令：指令级循环在树缓存上运行，本来就在协作层的覆盖之下由 trace 函数逐行兜住；而引擎内部循环（importFromJar、rename commit、pass 管线体）不开放插针，检查点只能落在这些调用的入口。因此单次超大引擎调用的取消延迟以该调用自身时长为上界，超出部分由硬层接管。

## 验证

进程内可测部分在 gtest：`FacadeLimits.DeadlineCheckpointFailsLongOpsClosed`（过期 deadline 下 flush/verify/sanitize 抛 `DeadlineExceeded`，清除后复原不毒化后续用例）与 `FacadeLimits.WatchdogFiresHookOnceAndStopsClean`（hook 触发、幂等 stop、未到点即停不误触）。进程级退出码契约在 `mjt_pysdk_embedtest`：每个场景 spawn 自身作独立进程断言退出码——干净脚本 0、语法错误 1、纯 Python 死循环加 400ms 期限 1（rc 为 1 而非 3，证明是协作层 trace 而非 watchdog 兜底）、`time.sleep` 阻塞加 400ms 期限 3（硬层，原生调用里协作层够不着）、输出洪泛加 1000 字节上限 1。硬层必须整进程观测，这是单独一个 embedtest 目标存在的原因。
