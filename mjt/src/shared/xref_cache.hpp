#pragma once

#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "libca/core/datatype.hpp"
#include "libca/str/utf8_string_arena.hpp"
#include "mbase/asm/classpath/call_graph.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/reference/reference_index.hpp"

namespace mjt {

struct XrefState
{
    ca::str::Utf8StringArena arena;
    base::ReferenceIndex     refs;
    base::CallGraph          graph;
};

struct XrefCacheStats
{
    ca::u64   hits              = 0;
    ca::u64   misses            = 0;
    ca::u64   evictions         = 0;
    ca::u64   fingerprintMicros = 0;
    ca::u64   buildMicros       = 0;
    ca::usize entries           = 0;
    ca::usize capacity          = 0;
    ///@brief 磁盘 cache 命中次数（仅 getOrBuildWithDisk 路径累计）。
    ca::u64   diskHits          = 0;
    ///@brief 磁盘 cache 未命中次数（仅 getOrBuildWithDisk 路径：文件不存在或
    ///fingerprint 不符；persist=false 的纯内存路径不计入）。
    ca::u64   diskMisses        = 0;
    ///@brief 磁盘 cache 写入失败次数（目录创建或写入出错）。
    ca::u64   diskWriteErrors   = 0;
    ///@brief 磁盘 cache 加载失败次数（文件损坏、版本不符等）。
    ca::u64   diskLoadErrors    = 0;
};

///@brief 控制 XrefCache 的磁盘持久化行为。
struct XrefPersistOptions
{
    ///@brief 若非空，cache 文件落在 `<cacheDir>/<fingerprint>.ridx`。
    ///
    ///留空时使用 `<jarDir>/.xref-cache/`；`persist=false` 时本字段被忽略。
    std::string cacheDir;
    ///@brief 是否启用磁盘 cache。默认关闭：分析工具不应在输入 JAR（取证/
    ///样本库场景的证据目录）旁默认创建文件；需要跨进程复用时显式开启。
    bool persist = false;
};

///@brief Process-local LRU for xref indices, keyed by a deterministic class-content digest.
///
///The fingerprint preimage is the little-endian u64 class count followed by, for each class in
///ascending URI byte order, little-endian u64 URI length, URI bytes, little-endian u64 content
///length, and content bytes. SHA-256 of that preimage makes invalidation independent of file
///timestamps and container iteration order. Non-class resources are excluded because neither
///ReferenceIndex nor CallGraph consumes them.
///
///On-disk persistence (XrefPersistOptions) layers a `.ridx` file per fingerprint on top of the
///in-memory LRU. The schema lives in `mbase/reference/reference_index_cache_format.hpp`; only
///ReferenceIndex is serialized — CallGraph is rebuilt via `buildFromIndex` because that pass is
///cheap relative to bytecode parsing. Disk failures never block a query: on any read or write
///error the cache falls back to the existing in-memory build path and updates `XrefCacheStats`.
class XrefCache
{
public:
    static XrefCache& instance();

    ///@brief 内存 LRU 路径，保留以兼容旧调用点（不读写磁盘）。
    std::shared_ptr<const XrefState> getOrBuild(base::ResourceContainer& container,
                                                std::string&             error);

    ///@brief 内存 LRU + 可选磁盘持久化路径。
    ///
    ///@param container 输入资源容器。
    ///@param jarPath 输入 JAR 路径，用于在 `opts.cacheDir` 留空时推算默认
    ///`<jarDir>/.xref-cache/` 目录。
    ///@param opts 持久化策略。
    ///@param error 失败时写入人类可读诊断。
    std::shared_ptr<const XrefState> getOrBuildWithDisk(base::ResourceContainer& container,
                                                        const std::string&       jarPath,
                                                        const XrefPersistOptions& opts,
                                                        std::string&             error);

    std::string fingerprint(const base::ResourceContainer& container) const;

    XrefCacheStats stats() const;
    void           clear();
    void           setCapacity(ca::usize capacity);

private:
    explicit XrefCache(ca::usize capacity = 4)
        : capacity_(capacity)
    {}

    std::shared_ptr<const XrefState> buildState(base::ResourceContainer& container,
                                                std::string&             error);

    struct Entry
    {
        std::shared_ptr<const XrefState> state;
        std::list<std::string>::iterator lru;
    };

    mutable std::mutex                     mutex_;
    ca::usize                              capacity_ = 4;
    std::list<std::string>                 lru_;
    std::unordered_map<std::string, Entry> entries_;
    XrefCacheStats                         stats_;
};

}   //namespace mjt
