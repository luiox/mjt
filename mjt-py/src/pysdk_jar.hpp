#pragma once

// pysdk_jar.hpp — mjt-py 合成样本与统计视图（v3 起不再含 Jar facade；
// 容器/树状态全部在 pysdk_facade.hpp 的 FacadeSession）。

#include <memory>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"

namespace mjt::pysdk {

/// 最近一次管线运行的逐槽位统计（PassRunStats 的值拷贝视图）。
struct PyRunStats
{
    struct Slot
    {
        std::string name;
        std::string scope;
        ca::u64     processed  = 0;
        ca::u64     changed    = 0;
        ca::u64     failed     = 0;
        double      durationMs = 0.0;
    };
    /// 容错档位跳过记录（PassFaultReport::Entry 的值拷贝视图；fail_soft 运行
    /// 才可能非空，默认档位任一失败整作业报错到不了这里）。
    struct Skip
    {
        std::string scope;    ///< 跳过粒度："class" / "method"
        std::string target;   ///< 类资源 URI；方法级为 "<class>.<name><desc>"
        std::string reason;   ///< "sanitize_error" / "reader_error" / "pass_error" /
                              ///< "writer_error"
        std::string detail;   ///< 失败详情；可为空
    };
    std::string       pipeline;
    std::vector<Slot> slots;
    std::vector<Skip> skipped;
};

/// 合成混淆形态演示 jar（unittest + 新样本演示共用；零样本字节入库）。
/// 结构：me/demo/App（恒等 rethrow handler + 混淆名）/ me/demo/Crypto（字符串
/// 解密器形态站点 + ldc 载体）/ me/demo/Util（被引用工具类）。
void writeSyntheticDemoJar(const std::string& outJarPath);

/// 单类 + MANIFEST 迷你 jar（ManifestResource 分拣、token 改名资源面测试专用）。
/// 结构：META-INF/MANIFEST.MF（Main-Class: me.demo.App）+ me/demo/App.class。
void writeSyntheticManifestJar(const std::string& outJarPath);

}   // namespace mjt::pysdk
