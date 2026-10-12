#pragma once

#include <string>

#include "libca/core/datatype.hpp"

namespace mjt::base {

class ReferenceIndex;

///@brief 将 ReferenceIndex 序列化到 `.ridx` cache 文件。
///
///@param outputPath 目标文件路径。写入采用临时文件 + rename 的原子替换，
///避免并发写入者互相覆盖出半截文件。
///@param index 已构建好的引用索引。
///@param fingerprint 关联的 class 内容 SHA-256（hex 字符串，64 字符），
///写入 Header 供读取端比对失效。
///@return 成功写入返回 true；I/O 错误或 fingerprint 长度非法返回 false。
bool writeReferenceIndexCache(const char* outputPath,
                              const ReferenceIndex& index,
                              const std::string& fingerprint);

}   //namespace mjt::base
