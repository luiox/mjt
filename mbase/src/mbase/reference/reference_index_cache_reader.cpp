#include "mbase/reference/reference_index_cache_reader.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "libca/core/datatype.hpp"
#include "mbase/reference/reference_index.hpp"
#include "mbase/reference/reference_index_cache_format.hpp"

namespace mjt::base {

namespace {

///@brief 带越界校验的内存读取游标。
///
///所有读取操作都在推进前检查剩余字节，越界时整体返回失败。这让上层逻辑
///可以放心假设读到的字段是完整的，而不必在每个调用点重复写边界检查。
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

bool readRefKey(Cursor& c, std::string& owner, std::string& name, std::string& desc)
{
    return c.readString(owner) && c.readString(name) && c.readString(desc);
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
std::string unpackFingerprint(const ridx::Header& hdr)
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

bool loadReferenceIndexCache(ReferenceIndex& out,
                             std::string& outFingerprint,
                             const void* data,
                             ca::usize size)
{
    if (!data || size < sizeof(ridx::Header)) return false;

    Cursor c{static_cast<const ca::u8*>(data), size, sizeof(ridx::Header)};

    auto* hdr = static_cast<const ridx::Header*>(data);
    if (hdr->magic != ridx::kMagic || hdr->version != ridx::kVersion) return false;

    std::vector<ClassDeclaration>  classDecls;
    std::vector<MethodDeclaration> methodDecls;
    std::vector<FieldDeclaration>  fieldDecls;
    std::vector<MethodReference>   methodRefs;
    std::vector<FieldReference>    fieldRefs;
    std::vector<TypeReference>     typeRefs;

    try {
        classDecls.reserve(static_cast<std::size_t>(hdr->classDeclsCount));
        for (ca::u64 i = 0; i < hdr->classDeclsCount; ++i) {
            ClassDeclaration decl;
            if (!c.readString(decl.name)) return false;
            if (!c.readString(decl.superName)) return false;
            ca::u64 ifaceCount = 0;
            if (!c.readU64(ifaceCount)) return false;
            decl.interfaces.clear();
            for (ca::u64 j = 0; j < ifaceCount; ++j) {
                std::string iface;
                if (!c.readString(iface)) return false;
                decl.interfaces.push_back(std::move(iface));
            }
            if (!c.readI32(decl.access)) return false;
            classDecls.push_back(std::move(decl));
        }

        methodDecls.reserve(static_cast<std::size_t>(hdr->methodDeclsCount));
        for (ca::u64 i = 0; i < hdr->methodDeclsCount; ++i) {
            MethodDeclaration decl;
            if (!readRefKey(c, decl.method.owner, decl.method.name, decl.method.desc)) return false;
            if (!c.readI32(decl.access)) return false;
            methodDecls.push_back(std::move(decl));
        }

        fieldDecls.reserve(static_cast<std::size_t>(hdr->fieldDeclsCount));
        for (ca::u64 i = 0; i < hdr->fieldDeclsCount; ++i) {
            FieldDeclaration decl;
            if (!readRefKey(c, decl.field.owner, decl.field.name, decl.field.desc)) return false;
            if (!c.readI32(decl.access)) return false;
            fieldDecls.push_back(std::move(decl));
        }

        methodRefs.reserve(static_cast<std::size_t>(hdr->methodRefsCount));
        for (ca::u64 i = 0; i < hdr->methodRefsCount; ++i) {
            MethodReference ref;
            if (!readRefSite(c, ref.site)) return false;
            if (!readRefKey(c, ref.target.owner, ref.target.name, ref.target.desc)) return false;
            if (!c.readString(ref.reflectionArgDesc)) return false;
            if (!c.readBool(ref.isInterface)) return false;
            ca::u8 kind = 0;
            if (!c.readU8(kind)) return false;
            if (kind > ridx::kMethodKindMax) return false;
            ref.kind = static_cast<MethodReferenceKind>(kind);
            methodRefs.push_back(std::move(ref));
        }

        fieldRefs.reserve(static_cast<std::size_t>(hdr->fieldRefsCount));
        for (ca::u64 i = 0; i < hdr->fieldRefsCount; ++i) {
            FieldReference ref;
            if (!readRefSite(c, ref.site)) return false;
            if (!readRefKey(c, ref.target.owner, ref.target.name, ref.target.desc)) return false;
            ca::u8 kind = 0;
            if (!c.readU8(kind)) return false;
            if (kind > ridx::kFieldKindMax) return false;
            ref.kind = static_cast<FieldReferenceKind>(kind);
            fieldRefs.push_back(std::move(ref));
        }

        typeRefs.reserve(static_cast<std::size_t>(hdr->typeRefsCount));
        for (ca::u64 i = 0; i < hdr->typeRefsCount; ++i) {
            TypeReference ref;
            if (!readRefSite(c, ref.site)) return false;
            if (!c.readString(ref.targetType)) return false;
            ca::u8 kind = 0;
            if (!c.readU8(kind)) return false;
            if (kind > ridx::kTypeKindMax) return false;
            ref.kind = static_cast<TypeReferenceKind>(kind);
            typeRefs.push_back(std::move(ref));
        }
    }
    catch (const std::bad_alloc&) {
        return false;
    }

    out.restore(std::move(classDecls), std::move(methodDecls), std::move(fieldDecls),
                std::move(methodRefs), std::move(fieldRefs), std::move(typeRefs));
    outFingerprint = unpackFingerprint(*hdr);
    return true;
}

}   //namespace mjt::base
