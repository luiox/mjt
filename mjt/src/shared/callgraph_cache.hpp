#pragma once

#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "libca/core/datatype.hpp"
#include "mbase/asm/classpath/resolved_call_graph.hpp"
#include "mbase/model/resource_container.hpp"

namespace mjt {

struct CallGraphCacheStats
{
    ca::u64   hits              = 0;
    ca::u64   misses            = 0;
    ca::u64   evictions         = 0;
    ca::u64   fingerprintMicros = 0;
    ca::u64   buildMicros       = 0;
    ca::usize entries           = 0;
    ca::usize capacity          = 0;
    ///@brief 磁盘 cache 命中次数（仅 persist 路径累计）。
    ca::u64   diskHits          = 0;
    ///@brief 磁盘 cache 未命中次数（文件不存在或 fingerprint/profile 不符）。
    ca::u64   diskMisses        = 0;
    ///@brief 磁盘 cache 写入失败次数（目录创建或写入出错）。
    ca::u64   diskWriteErrors   = 0;
    ///@brief 磁盘 cache 加载失败次数（文件损坏、版本不符等）。
    ca::u64   diskLoadErrors    = 0;
};

///@brief 控制 callgraph 解析边集的磁盘持久化行为。
struct CallGraphPersistOptions
{
    ///@brief 若非空，cache 文件落在 `<cacheDir>/<key>.rcg`。
    ///
    ///留空时使用 `<jarDir>/.callgraph-cache/`；`persist=false` 时本字段被忽略。
    std::string cacheDir;
    ///@brief 是否启用磁盘 cache。默认关闭：分析工具不应在输入 JAR（取证/
    ///样本库场景的证据目录）旁默认创建文件；需要跨进程复用时显式开启。
    bool persist = false;
};

///@brief Process-local LRU for resolved call-graph edge sets, keyed by
///(class-content digest, build profile).
///
///The key composes the same deterministic class-content SHA-256 as XrefCache
///with a caller-supplied profile string that must capture every option
///influencing the edge set (precision tier, explicit entries for RTA, ...).
///Changing either part invalidates the entry — full rebuild, no partial
///reuse (incremental invalidation at whole-graph granularity).
///
///Serialization boundary: only the `ResolvedCallEdge` value vector is
///persisted (`.rcg` file, schema in
///`mbase/asm/classpath/resolved_call_graph_cache_format.hpp`). The in-process
///node-pointer structures (direct `CallGraph`, hierarchies) are never
///serialized; the direct graph rides the `.ridx` cache and hierarchies are
///rebuilt by the caller's build callback on any miss.
///
///Disk failures never block a query: on any read or write error the cache
///falls back to the caller's build callback and updates `CallGraphCacheStats`.
class CallGraphCache
{
public:
    using BuildFn =
        std::function<std::shared_ptr<const base::ResolvedCallGraph>(std::string& error)>;

    static CallGraphCache& instance();

    ///@brief 内存 LRU + 可选磁盘持久化路径。
    ///
    ///@param container 输入资源容器（用于内容指纹）。
    ///@param jarPath 输入 JAR 路径，用于在 `opts.cacheDir` 留空时推算默认
    ///`<jarDir>/.callgraph-cache/` 目录。
    ///@param profile 描述影响边集的全部选项的稳定串（如 "cha" 或
    ///"rta;entries=..."），参与 cache 键。
    ///@param build cache 未命中时的全量重建回调。
    ///@param opts 持久化策略。
    ///@param error 失败时写入人类可读诊断。
    std::shared_ptr<const base::ResolvedCallGraph> getOrBuild(
        base::ResourceContainer& container, const std::string& jarPath,
        const std::string& profile, const BuildFn& build, const CallGraphPersistOptions& opts,
        std::string& error);

    ///@brief 与 XrefCache 同口径的 class 内容 SHA-256。
    std::string fingerprint(const base::ResourceContainer& container) const;

    ///@brief 复合 cache 键：SHA256(class 内容指纹 hex + '\n' + profile)。
    std::string compositeKey(const std::string& fingerprintHex, const std::string& profile) const;

    CallGraphCacheStats stats() const;
    void                clear();
    void                setCapacity(ca::usize capacity);

private:
    explicit CallGraphCache(ca::usize capacity = 4)
        : capacity_(capacity)
    {}

    struct Entry
    {
        std::shared_ptr<const base::ResolvedCallGraph> graph;
        std::list<std::string>::iterator               lru;
    };

    mutable std::mutex                              mutex_;
    ca::usize                                       capacity_ = 4;
    std::list<std::string>                          lru_;
    std::unordered_map<std::string, Entry>          entries_;
    CallGraphCacheStats                             stats_;
};

}   //namespace mjt
