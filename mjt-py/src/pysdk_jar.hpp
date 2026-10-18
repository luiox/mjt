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
    std::string    pipeline;
    std::vector<Slot> slots;
};

/// 合成混淆形态演示 jar（unittest + 新样本演示共用；零样本字节入库）。
/// 结构：me/demo/App（恒等 rethrow handler + 混淆名）/ me/demo/Crypto（字符串
/// 解密器形态站点 + ldc 载体）/ me/demo/Util（被引用工具类）。
void writeSyntheticDemoJar(const std::string& outJarPath);

/// 单类 + MANIFEST 迷你 jar（ManifestResource 分拣、token 改名资源面测试专用）。
/// 结构：META-INF/MANIFEST.MF（Main-Class: me.demo.App）+ me/demo/App.class。
void writeSyntheticManifestJar(const std::string& outJarPath);

}   // namespace mjt::pysdk
