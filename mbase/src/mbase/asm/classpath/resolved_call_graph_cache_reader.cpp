#include "mbase/asm/classpath/resolved_call_graph_cache_reader.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "mbase/asm/classpath/resolved_call_graph.hpp"
#include "mbase/asm/classpath/resolved_call_graph_cache_format.hpp"

namespace mjt::base {

namespace {

///@brief 带越界校验的内存读取游标。
///
///与 reference_index_cache_reader 的 Cursor 同一口径：所有读取操作都在推进前
///检查剩余字节，越界时整体返回失败。
struct Cursor
{
    const ca::u8* data;
    ca::usize     size;
    ca::usize     pos = 0;

    bool canRead(ca::usize n) const { return pos + n <= size && pos + n >= pos; }

    bool readU64(ca::u64& out)
    {
        if (!canRead(8)) return false;
        ca::u64 v = 0;
        for (ca::usize i = 0; i < 8; ++i) {
            v |= static_cast<ca::u64>(data[pos + i]) << (i * 8);
        }
        pos += 8;
        out = v;
        return true;
    }

    bool readI32(ca::i32& out)
    {
        ca::u64 v = 0;
        if (!readU64(v)) return false;
        out = static_cast<ca::i32>(static_cast<ca::u32>(v));
        return true;
    }

    bool readU8(ca::u8& out)
    {
        if (!canRead(1)) return false;
        out = data[pos++];
        return true;
    }

    bool readBool(bool& out)
    {
        ca::u8 v = 0;
        if (!readU8(v)) return false;
        out = (v != 0);
        return true;
    }

    bool readString(std::string& out)
    {
        ca::u64 len = 0;
        if (!readU64(len)) return false;
        //字符串长度上限用剩余字节兜底，防止 header 被恶意构造出天文数字。
        if (!canRead(static_cast<ca::usize>(len))) return false;
        out.assign(reinterpret_cast<const char*>(data + pos), static_cast<ca::usize>(len));
        pos += static_cast<ca::usize>(len);
        return true;
    }
};

bool readRefKey(Cursor& c, MethodRefKey& key)
{
    return c.readString(key.owner) && c.readString(key.name) && c.readString(key.desc);
}

bool readRefSite(Cursor& c, ReferenceSite& site)
{
    if (!c.readString(site.sourceClass)) return false;
    if (!c.readString(site.sourceMethod)) return false;
    if (!c.readString(site.sourceDesc)) return false;
    if (!c.readI32(site.opcode)) return false;
    return c.readI32(site.insnIndex);
}

///@brief 把 Header 里的 32 字节指纹展开成 64 字符 lowercase hex。
std::string unpackFingerprint(const rcg::Header& hdr)
{
    static const char* kHex = "0123456789abcdef";
    std::string        out;
    out.reserve(64);
    for (ca::usize i = 0; i < 32; ++i) {
        ca::u8 b = hdr.fingerprintSha256[i];
        out.push_back(kHex[(b >> 4) & 0xf]);
        out.push_back(kHex[b & 0xf]);
    }
    return out;
}

}   //namespace

bool loadResolvedCallGraphCache(ResolvedCallGraph& out,
                                std::string& outFingerprint,
                                ca::u64& outProfileHash,
                                const void* data,
                                ca::usize size)
{
    if (!data || size < sizeof(rcg::Header)) return false;

    Cursor c{static_cast<const ca::u8*>(data), size, sizeof(rcg::Header)};

    auto* hdr = static_cast<const rcg::Header*>(data);
    if (hdr->magic != rcg::kMagic || hdr->version != rcg::kVersion) return false;

    //边数上限用剩余字节兜底：每条边至少含 8 个字符串的 8 字节长度前缀 +
    //1 字节 kind + 1 字节 expanded，64 字节是下界估计，防止天文数字 edgeCount。
    if (hdr->edgeCount > (size - sizeof(rcg::Header)) / 64 + 1) return false;

    std::vector<ResolvedCallEdge> edges;
    edges.reserve(static_cast<ca::usize>(hdr->edgeCount));
    for (ca::u64 i = 0; i < hdr->edgeCount; ++i) {
        ResolvedCallEdge edge;
        ca::u8           kind = 0;
        if (!readRefSite(c, edge.site)) return false;
        if (!readRefKey(c, edge.encodedTarget)) return false;
        if (!readRefKey(c, edge.resolvedTarget)) return false;
        if (!c.readU8(kind)) return false;
        if (kind > rcg::kCallKindMax) return false;
        if (!c.readBool(edge.expanded)) return false;
        edge.kind = static_cast<ResolvedCallKind>(kind);
        edges.push_back(std::move(edge));
    }

    //尾部有未消费字节说明格式与写入端不一致（版本错位/文件拼接），拒绝加载。
    if (c.pos != c.size) return false;

    out.clear();
    for (auto& edge : edges) {
        out.appendEdge(edge);
    }
    //v1.1 RTA 摘要段：计数随边表一起还原（CHA 档文件两计数恒 0，写 0 亦语义
    //忠实）；已实例化类型只落计数不落名单，instantiatedClasses 名单视图为空。
    out.setRtaSummaryCounters(static_cast<ca::usize>(hdr->rtaInstantiatedTypes),
                              static_cast<ca::usize>(hdr->rtaReachableMethods));
    outFingerprint = unpackFingerprint(*hdr);
    outProfileHash = hdr->profileHash;
    return true;
}

}   //namespace mjt::base
