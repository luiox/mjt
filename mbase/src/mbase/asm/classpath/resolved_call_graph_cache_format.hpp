#pragma once

#include <cstdint>

#include "libca/core/datatype.hpp"

///@brief ResolvedCallGraph 持久化 cache 的二进制布局。
///
///文件由固定 Header 与一个 length-prefixed 解析边段构成。所有多字节整数使用
///little-endian 编码，字符串以 u64 长度前缀 + 裸 UTF-8 字节写入。这与
///`mbase/reference/reference_index_cache_format.hpp`（.ridx）的编码风格一致，
///避免引入新的序列化框架。
///
///v1.1 起 Header 追加 RTA 摘要段（已实例化类型数、入口可达方法数），磁盘命中
///进程的 `RTA 档：已实例化类型/入口可达方法` 摘要行由此还原真实值，不再恒为
///0/0。档位语义：两个计数只对 RTA 档有意义，CHA 档进程内本就恒 0（无实例化
///集概念），故 CHA 档写入端恒写 0——这是语义忠实的还原而非占位失真；跨档误读
///不可能发生（profile 串参与 Header.profileHash 与上层 cache 键，档位变化即
///键变化）。不另设"适用档位"标记：那是与 profileHash 冗余的状态，还需读取端
///引入分支。注意持久化粒度是计数器本身：已实例化类型只落计数不落名单，磁盘
///还原图的 `instantiatedClasses` 名单视图为空（摘要行只消费计数）。
///
///序列化边界（与 .ridx 对齐的口径）：只持久化 `ResolvedCallEdge` 值——
///全部是 std::string / i32 / 枚举 / bool，不含裸指针。进程内的节点指针图
///（`CallGraph` 的 `unordered_set<CallGraphNode*>`）与 arena 视图一律不入档：
///直接图经由 `.ridx` cache 还原 `ReferenceIndex` 后用 `buildFromIndex` 现算，
///层次图（ClassHierarchy/MethodHierarchy）同样每次现算。
///
///设计与 `ridx::Header` / `mbase/hierarchy/ihc_format.hpp` 对齐：固定 magic +
///version 头、读取端做严格的越界校验、版本不匹配直接拒绝加载（旧版本文件
///静默回退 build 路径全量重建，属既有失效纪律）。
namespace mjt::base::rcg {

///@brief 文件标识，ASCII "RCGX" 的小端序 u32。
inline constexpr ca::u32 kMagic = 0x58474352u;

///@brief 主.次 编码：1.0 首版；1.1 起 Header 追加 RTA 摘要两计数（+16 字节）。
inline constexpr ca::u32 kVersion = 0x00010001u;

///@brief ResolvedCallKind 在文件中用单字节存储。
///
///写入端用 `static_cast<ca::u8>`，读取端先读 u8 再校验是否落在合法区间，
///越界的枚举字节视为损坏文件并拒绝加载。上界对应 `ResolvedCallKind::Reflect`。
inline constexpr ca::u8 kCallKindMax = 6u;

///@brief 对 edge 集合有影响的选项串（profile）压缩成 64 位 FNV-1a。
///
///profile 串由上层拼接（如 "cha" 或 "rta;entries=..."），决定同一 jar 内容
///指纹下的 cache 键区分。FNV-1a 64 足够防意外碰撞（键还叠加 jar 内容
///SHA-256），不承担对抗性防碰撞职责。
inline ca::u64 profileHash(const std::string& profile)
{
    ca::u64 hash = 14695981039346656037ull;
    for (const char ch : profile) {
        hash ^= static_cast<ca::u8>(ch);
        hash *= 1099511628211ull;
    }
    return hash;
}

#pragma pack(push, 1)

///@brief cache 文件头。
///
///fingerprint 字段保存与此边集关联的 class 内容 SHA-256（XrefCache::fingerprint
///的产物，32 字节）；profileHash 保存构建选项串的 FNV-1a 64。读取端不解释
///二者语义，由上层比对当前 container 指纹与 profile，任一不匹配则丢弃整个
///cache 文件（全量重算的失效策略）。
struct Header
{
    ca::u32 magic;                  ///< kMagic
    ca::u32 version;                ///< kVersion
    ca::u8  fingerprintSha256[32];  ///< 关联 class 内容指纹
    ca::u64 profileHash;            ///< 构建选项串 FNV-1a 64
    ca::u64 edgeCount;              ///< 解析边段条目数
    ca::u64 rtaInstantiatedTypes;   ///< v1.1：RTA 已实例化类型数；CHA 档恒 0
    ca::u64 rtaReachableMethods;    ///< v1.1：RTA 入口可达方法数；CHA 档恒 0
};

#pragma pack(pop)

static_assert(sizeof(Header) == 72, "Header must be 72 bytes (v1.1 RTA summary segment)");

}   //namespace mjt::base::rcg
