#include "mbase/asm/classpath/resolved_call_graph_cache_writer.hpp"

#include <cstdio>
#include <fstream>
#include <string>

#include "libca/core/datatype.hpp"
#include "mbase/asm/classpath/resolved_call_graph.hpp"
#include "mbase/asm/classpath/resolved_call_graph_cache_format.hpp"

namespace mjt::base {

namespace {

void writeU64LE(std::ofstream& out, ca::u64 value)
{
    ca::u8 bytes[8];
    for (ca::usize i = 0; i < 8; ++i) {
        bytes[i] = static_cast<ca::u8>((value >> (i * 8)) & 0xffU);
    }
    out.write(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}

void writeI32LE(std::ofstream& out, ca::i32 value)
{
    writeU64LE(out, static_cast<ca::u64>(static_cast<ca::u32>(value)));
}

void writeU8(std::ofstream& out, ca::u8 value)
{
    out.put(static_cast<char>(value));
}

void writeBool(std::ofstream& out, bool value)
{
    writeU8(out, value ? 1u : 0u);
}

void writeString(std::ofstream& out, const std::string& value)
{
    writeU64LE(out, static_cast<ca::u64>(value.size()));
    if (!value.empty()) out.write(value.data(), static_cast<std::streamsize>(value.size()));
}

void writeRefKey(std::ofstream& out, const MethodRefKey& key)
{
    writeString(out, key.owner);
    writeString(out, key.name);
    writeString(out, key.desc);
}

void writeRefSite(std::ofstream& out, const ReferenceSite& site)
{
    writeString(out, site.sourceClass);
    writeString(out, site.sourceMethod);
    writeString(out, site.sourceDesc);
    writeI32LE(out, site.opcode);
    writeI32LE(out, site.insnIndex);
}

void writeEdge(std::ofstream& out, const ResolvedCallEdge& edge)
{
    writeRefSite(out, edge.site);
    writeRefKey(out, edge.encodedTarget);
    writeRefKey(out, edge.resolvedTarget);
    writeU8(out, static_cast<ca::u8>(edge.kind));
    writeBool(out, edge.expanded);
}

///@brief 把 hex 字符串指纹展开成 32 字节，写入 Header。
///
///fingerprint 由 XrefCache::fingerprint 产出，是 64 字符的 lowercase hex
///SHA-256。长度不是恰好 64 时返回 false，调用方应跳过落盘而不是写半截。
bool packFingerprint(rcg::Header& hdr, const std::string& fingerprint)
{
    if (fingerprint.size() != 64) return false;
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (ca::usize i = 0; i < 32; ++i) {
        int hi = hexVal(fingerprint[i * 2]);
        int lo = hexVal(fingerprint[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        hdr.fingerprintSha256[i] = static_cast<ca::u8>((hi << 4) | lo);
    }
    return true;
}

}   //namespace

bool writeResolvedCallGraphCache(const char* outputPath,
                                 const ResolvedCallGraph& graph,
                                 const std::string& fingerprint,
                                 ca::u64 profileHash)
{
    rcg::Header hdr{};
    hdr.magic       = rcg::kMagic;
    hdr.version     = rcg::kVersion;
    hdr.profileHash = profileHash;
    if (!packFingerprint(hdr, fingerprint)) return false;
    hdr.edgeCount = static_cast<ca::u64>(graph.edgeCount());
    //v1.1 RTA 摘要段：CHA 档两个计数进程内恒 0（无实例化集概念），写 0 是
    //语义忠实的还原；RTA 档落真实值，磁盘命中进程的摘要行由此还原。
    hdr.rtaInstantiatedTypes = static_cast<ca::u64>(graph.rtaInstantiatedCount());
    hdr.rtaReachableMethods  = static_cast<ca::u64>(graph.rtaReachableMethodCount());

    //先写临时文件，再 rename，保证其他读者不会看到半截文件。
    std::string tmpPath = std::string(outputPath) + ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

        for (const auto& edge : graph.edges()) {
            writeEdge(out, edge);
        }

        out.flush();
        if (!out.good()) {
            std::remove(tmpPath.c_str());
            return false;
        }
    }

    if (std::rename(tmpPath.c_str(), outputPath) != 0) {
        //Windows 的 rename 不覆盖已存在目标：重写既有缓存（如超限/损坏
        //文件重建后的覆写）必须先移除旧文件再换入。
        std::remove(outputPath);
        if (std::rename(tmpPath.c_str(), outputPath) != 0) {
            std::remove(tmpPath.c_str());
            return false;
        }
    }
    return true;
}

}   //namespace mjt::base
