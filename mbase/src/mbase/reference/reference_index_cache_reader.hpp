#pragma once

#include <string>

#include "libca/core/datatype.hpp"

namespace mjt::base {

class ReferenceIndex;

///@brief 从内存映射的二进制 cache 还原 ReferenceIndex。
///
///读取端完全校验 magic / version / 越界，遇到任何损坏立即返回 false 且不修改
///out 的状态。fingerprint 字段从 Header 还原为 64 字符 lowercase hex 字符串，
///由上层用来与当前 container 指纹比对，不匹配时整个 cache 视为失效。
///
///@param out 还原目标。仅在函数返回 true 时被替换。
///@param outFingerprint 输出关联的 class 内容 SHA-256 hex 字符串。
///@param data cache 文件完整字节。
///@param size 字节数。
///@return 成功还原返回 true；magic/version 不符、越界、枚举越界等返回 false。
bool loadReferenceIndexCache(ReferenceIndex& out,
                             std::string& outFingerprint,
                             const void* data,
                             ca::usize size);

}   //namespace mjt::base
