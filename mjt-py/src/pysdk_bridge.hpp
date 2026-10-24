#pragma once

// pysdk_bridge.hpp — 绑定层公共声明（bind_core ↔ bind_facade）。
//
// 开源基座面：bind_core.cpp 持 pybind 模块入口与 run_pass/run_pipeline/
// pass_describe/opt_preview/opt_apply（mjbase spi 消费面，退役 MCP pass 工具
// 的 SDK 等位物）；bind_facade.cpp 持 v3 平函数面与工具等位物批（jar_diff/
// jar_fix/decompiler_compare/shadow sidecar 文件面）。方法级 pass 桥
// （PyMethodPass 适配器）仍属私有侧管线消费面——Python 侧类型锚
// （MethodPass/PreservedAnalyses/changed()/preserved_all()）由 bind_core 提供
// 保持脚本面形状，私有侧适配器按同款锚接自家管线。

#include <pybind11/pybind11.h>

namespace py = pybind11;   // 本头自洽（pybind11 不保证全局别名可见）

namespace mjt::pysdk {

/// v3 平函数面绑定入口（契约 v3 §2；bind_facade.cpp 实现）。
void bindFacade(py::module_& m);

}   // namespace mjt::pysdk
