#pragma once

#include <string>

#include "libca/core/datatype.hpp"

namespace mjt::base {

class ResolvedCallGraph;

///@brief 从内存中的二进制 cache 还原 ResolvedCallGraph。
///
///读取端完全校验 magic / version / 枚举区间 / 越界，遇到任何损坏立即返回
///false 且不修改 out 的状态。fingerprint 与 profileHash 从 Header 还原，由
///上层分别与当前 container 指纹、构建选项串比对，任一不匹配时整个 cache
///视为失效（全量重算）。
///
///还原经 `ResolvedCallGraph::appendEdge` 逐条灌入，边序与写入时的 build 序
///一致，下游查询（edgesFrom/edgesTo/逆闭包）语义与现算等价；v1.1 起随附的
///RTA 摘要计数（已实例化类型数、入口可达方法数；CHA 档恒 0）一并还原，磁盘
///命中进程的 RTA 摘要行输出真实值。
///
///@param out 还原目标。仅在函数返回 true 时被替换（append 前先 clear）。
///@param outFingerprint 输出关联的 class 内容 SHA-256 hex 字符串。
///@param outProfileHash 输出关联的构建选项串 FNV-1a 64。
///@param data cache 文件完整字节。
///@param size 字节数。
///@return 成功还原返回 true；magic/version 不符、越界、枚举越界等返回 false。
bool loadResolvedCallGraphCache(ResolvedCallGraph& out,
                                std::string& outFingerprint,
                                ca::u64& outProfileHash,
                                const void* data,
                                ca::usize size);

}   //namespace mjt::base
