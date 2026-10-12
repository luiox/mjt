#pragma once

#include <string>

#include "libca/core/datatype.hpp"

namespace mjt::base {

class ResolvedCallGraph;

///@brief 将 ResolvedCallGraph 序列化到 `.rcg` cache 文件。
///
///@param outputPath 目标文件路径。写入采用临时文件 + rename 的原子替换，
///避免并发写入者互相覆盖出半截文件。
///@param graph 已构建好的解析调用图（按 build 顺序持久化全量边）。
///@param fingerprint 关联的 class 内容 SHA-256（hex 字符串，64 字符），
///写入 Header 供读取端比对失效。
///@param profileHash 构建选项串（profile）的 FNV-1a 64，见
///`rcg::profileHash`；同一 jar 内容下区分 CHA/RTA 等
///不同选项产出的边集。
///@return 成功写入返回 true；I/O 错误或 fingerprint 长度非法返回 false。
bool writeResolvedCallGraphCache(const char* outputPath,
                                 const ResolvedCallGraph& graph,
                                 const std::string& fingerprint,
                                 ca::u64 profileHash);

}   //namespace mjt::base
