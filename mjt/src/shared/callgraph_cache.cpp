#include "callgraph_cache.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

#include "libca/crypto/sha256.hpp"
#include "libca/core/tag_cast.hpp"
#include "mbase/asm/classpath/resolved_call_graph_cache_reader.hpp"
#include "mbase/asm/classpath/resolved_call_graph_cache_writer.hpp"
#include "mbase/asm/classpath/resolved_call_graph_cache_format.hpp"
#include "mbase/model/resource_model.hpp"

namespace fs = std::filesystem;

namespace mjt {
namespace {

using Clock = std::chrono::steady_clock;

ca::u64 elapsedMicros(Clock::time_point start)
{
    return static_cast<ca::u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count());
}

void addU64(ca::crypto::SHA256& hash, ca::u64 value)
{
    ca::u8 bytes[8];
    for (ca::usize i = 0; i < 8; ++i) {
        bytes[i] = static_cast<ca::u8>((value >> (i * 8)) & 0xffU);
    }
    hash.add(bytes, sizeof(bytes));
}

///@brief 读取文件全部字节。文件不存在或读取失败返回空 vector。
///
///cache 文件来自磁盘、尺寸不可信：超过 256 MiB 直接按读取失败（空）返回，
///调用方据此走 cache-miss 完整重建，避免把畸形超大文件整读进内存。
std::vector<ca::u8> readWholeFile(const fs::path& path)
{
    constexpr std::streamsize kMaxCacheFileBytes = 256 * 1024 * 1024;
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamsize size = in.tellg();
    if (size <= 0) return {};
    if (size > kMaxCacheFileBytes) return {};
    in.seekg(0);
    std::vector<ca::u8> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), size);
    if (!in.good()) return {};
    return bytes;
}

///@brief 在 cacheDir 下推导默认 .rcg 文件路径。
///
///cacheDir 为空时回退到 jarPath 同目录下的 .callgraph-cache/。jarPath 也为空
///（非 JAR 输入）则返回空 path，调用方据此跳过磁盘路径。
fs::path resolveCacheFile(const std::string& cacheDir, const std::string& jarPath,
                          const std::string& key)
{
    const std::string filename = key + ".rcg";
    fs::path          dir;
    if (!cacheDir.empty()) {
        //cacheDir/jarPath 持 argv 值（--cache-dir / -i，UTF-8 契约）：
        //显式 u8path（u8path 清扫）；"." 与 ".callgraph-cache" 为 ASCII 字面量
        //不受影响。
        dir = fs::u8path(cacheDir);
    }
    else if (!jarPath.empty()) {
        std::error_code ec;
        auto jarDir = fs::u8path(jarPath).parent_path();
        //纯文件名（无目录部分）时 parent_path 为空：回退当前目录，
        //否则命令会被静默禁用。
        if (jarDir.empty()) jarDir = fs::path(".");
        dir = jarDir / ".callgraph-cache";
    }
    else {
        return {};
    }
    return dir / filename;
}

bool ensureDirExists(const fs::path& dir)
{
    std::error_code ec;
    fs::create_directories(dir, ec);
    return !ec;
}

}   //namespace

CallGraphCache& CallGraphCache::instance()
{
    static CallGraphCache cache;
    return cache;
}

std::string CallGraphCache::fingerprint(const base::ResourceContainer& container) const
{
    struct ClassData
    {
        std::string                uri;
        const std::vector<ca::u8>* bytes;
    };

    std::vector<ClassData> classes;
    container.visitResources([&](const std::string& uri, const base::IResource& resource) {
        if (resource.getType() != base::ResourceType::Class) return;
        const auto* cls = ca::core::tag_cast::dyn_cast<const base::ClassResource>(&resource);
        if (cls) classes.push_back({uri, &cls->get()});
    });
    std::sort(classes.begin(), classes.end(), [](const ClassData& lhs, const ClassData& rhs) {
        return lhs.uri < rhs.uri;
    });

    ca::crypto::SHA256 hash;
    addU64(hash, static_cast<ca::u64>(classes.size()));
    for (const auto& cls : classes) {
        addU64(hash, static_cast<ca::u64>(cls.uri.size()));
        hash.add(cls.uri.data(), cls.uri.size());
        addU64(hash, static_cast<ca::u64>(cls.bytes->size()));
        if (!cls.bytes->empty()) hash.add(cls.bytes->data(), cls.bytes->size());
    }
    return hash.get_hash();
}

std::string CallGraphCache::compositeKey(const std::string& fingerprintHex,
                                         const std::string& profile) const
{
    ca::crypto::SHA256 hash;
    hash.add(fingerprintHex.data(), fingerprintHex.size());
    const ca::u8 sep = '\n';
    hash.add(&sep, 1);
    hash.add(profile.data(), profile.size());
    return hash.get_hash();
}

CallGraphCacheStats CallGraphCache::stats() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto                        result = stats_;
    result.entries                     = entries_.size();
    result.capacity                    = capacity_;
    return result;
}

void CallGraphCache::clear()
{
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.clear();
    lru_.clear();
    stats_ = {};
}

void CallGraphCache::setCapacity(ca::usize capacity)
{
    std::lock_guard<std::mutex> lock(mutex_);
    capacity_ = capacity;
    while (entries_.size() > capacity_) {
        entries_.erase(lru_.back());
        lru_.pop_back();
        ++stats_.evictions;
    }
}

std::shared_ptr<const base::ResolvedCallGraph> CallGraphCache::getOrBuild(
    base::ResourceContainer& container, const std::string& jarPath, const std::string& profile,
    const BuildFn& build, const CallGraphPersistOptions& opts, std::string& error)
{
    const auto fingerprintStart = Clock::now();
    const auto jarFingerprint   = fingerprint(container);
    const auto key              = compositeKey(jarFingerprint, profile);
    const auto expectedProfileHash = base::rcg::profileHash(profile);
    const auto fingerprintTime     = elapsedMicros(fingerprintStart);

    //内存命中优先，避免重复磁盘 IO。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stats_.fingerprintMicros += fingerprintTime;
        auto found = entries_.find(key);
        if (found != entries_.end()) {
            lru_.splice(lru_.begin(), lru_, found->second.lru);
            ++stats_.hits;
            return found->second.graph;
        }
        ++stats_.misses;
    }

    //尝试从磁盘还原。命中则跳过层次构建与虚分派展开。
    const auto cacheFile = resolveCacheFile(opts.cacheDir, jarPath, key);
    if (opts.persist && !cacheFile.empty()) {
        std::error_code ec;
        if (fs::exists(cacheFile, ec) && !ec) {
            const auto bytes = readWholeFile(cacheFile);
            if (!bytes.empty()) {
                auto        graph       = std::make_shared<base::ResolvedCallGraph>();
                std::string loadedFingerprint;
                ca::u64     loadedProfileHash = 0;
                if (base::loadResolvedCallGraphCache(*graph, loadedFingerprint, loadedProfileHash,
                                                     bytes.data(), bytes.size()) &&
                    loadedFingerprint == jarFingerprint &&
                    loadedProfileHash == expectedProfileHash) {
                    auto shared = std::shared_ptr<const base::ResolvedCallGraph>(graph);

                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.diskHits;
                    //同时灌进内存 LRU，下一次内存命中。磁盘读取发生在锁外，期间
                    //同 key 可能已被并发线程插入；先复查再插入，避免 emplace 失败
                    //后 lru_ 残留孤儿节点破坏不变量。
                    auto found = entries_.find(key);
                    if (found != entries_.end()) {
                        lru_.splice(lru_.begin(), lru_, found->second.lru);
                        return found->second.graph;
                    }
                    if (capacity_ > 0) {
                        while (entries_.size() >= capacity_) {
                            entries_.erase(lru_.back());
                            lru_.pop_back();
                            ++stats_.evictions;
                        }
                        lru_.push_front(key);
                        entries_.emplace(key, Entry{shared, lru_.begin()});
                    }
                    return shared;
                }
                else {
                    std::lock_guard<std::mutex> lock(mutex_);
                    ++stats_.diskLoadErrors;
                }
            }
        }
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.diskMisses;
    }

    //磁盘未命中（或未开持久化）：走调用方全量重建路径，成功后尝试落盘。
    const auto buildStart = Clock::now();
    auto       graph      = build(error);
    const auto buildTime  = elapsedMicros(buildStart);
    if (!graph) return nullptr;

    std::lock_guard<std::mutex> lock(mutex_);
    stats_.buildMicros += buildTime;

    if (opts.persist && !cacheFile.empty()) {
        std::error_code ec;
        if (ensureDirExists(cacheFile.parent_path())) {
            if (!base::writeResolvedCallGraphCache(cacheFile.string().c_str(), *graph,
                                                   jarFingerprint, expectedProfileHash)) {
                ++stats_.diskWriteErrors;
            }
        }
        else {
            ++stats_.diskWriteErrors;
        }
    }

    auto found = entries_.find(key);
    if (found != entries_.end()) {
        lru_.splice(lru_.begin(), lru_, found->second.lru);
        return found->second.graph;
    }

    if (capacity_ == 0) return graph;
    while (entries_.size() >= capacity_) {
        entries_.erase(lru_.back());
        lru_.pop_back();
        ++stats_.evictions;
    }
    lru_.push_front(key);
    entries_.emplace(key, Entry{graph, lru_.begin()});
    return graph;
}

}   //namespace mjt
