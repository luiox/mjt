#include "mbase/asm/remapper/resource_remapper.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "jbytecode/core/readers.hpp"
#include "jbytecode/core/writers.hpp"
#include "mbase/analysis/alias_analysis.hpp"
#include "mbase/model/resource_model.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {
namespace {

using namespace jbytecode::core;
using namespace jbytecode::tree;

std::string classResourceUri(const std::string& internalName)
{
    return internalName + ".class";
}

const std::string& serviceResourcePrefix()
{
    static const std::string prefix = "META-INF/services/";
    return prefix;
}

bool startsWith(const std::string& value, const std::string& prefix)
{
    return value.size() >= prefix.size() && value.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string& value, const std::string& suffix)
{
    return value.size() >= suffix.size() &&
           value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string toLowerAscii(std::string value)
{
    for (auto& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

std::string toInternalNameToken(std::string value)
{
    for (auto& c : value) {
        if (c == '.') c = '/';
    }
    return value;
}

std::string restoreNameTokenShape(const std::string& original, const std::string& mapped)
{
    std::string value = mapped;
    if (original.find('.') != std::string::npos && original.find('/') == std::string::npos) {
        for (auto& c : value) {
            if (c == '/') c = '.';
        }
    }
    return value;
}

void renameClassResources(ResourceContainer&                                      container,
                          const std::vector<std::pair<std::string, std::string>>& renames,
                          bool&                                                   changed)
{
    auto& resources = container.getResources();
    for (const auto& [oldUri, newUri] : renames) {
        if (oldUri == newUri || resources.find(newUri) != resources.end()) continue;
        auto node = resources.extract(oldUri);
        if (node.empty()) continue;

        auto* cls = ca::core::tag_cast::dyn_cast<ClassResource>(node.mapped().get());
        if (!cls) {
            resources.insert(std::move(node));
            continue;
        }

        resources.emplace(newUri, std::make_unique<ClassResource>(newUri, cls->get()));
        changed = true;
    }
}

bool isServiceResourceUri(const std::string& uri)
{
    const auto& prefix = serviceResourcePrefix();
    return startsWith(uri, prefix) && uri.size() > prefix.size();
}

std::string remapServiceUri(const MemberRemapPlan& plan, const std::string& uri)
{
    if (!isServiceResourceUri(uri)) return uri;

    const auto&       prefix  = serviceResourcePrefix();
    const std::string name    = uri.substr(prefix.size());
    const std::string oldName = toInternalNameToken(name);
    auto              mapped  = plan.mapInternalName(ca::str::Utf8StringRef::from_cstr(oldName.c_str()));
    if (mapped.equals(ca::str::Utf8StringRef::from_cstr(oldName.c_str()))) return uri;
    return prefix + restoreNameTokenShape(name, mapped.to_std_string());
}

bool isProviderNameChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '$' || c == '.' || c == '/';
}

std::string remapServiceProviderLine(const MemberRemapPlan& plan, const std::string& line,
                                     bool& changed)
{
    const std::size_t comment = line.find('#');
    const std::size_t limit   = comment == std::string::npos ? line.size() : comment;

    std::size_t tokenBegin = 0;
    while (tokenBegin < limit && !isProviderNameChar(line[tokenBegin])) tokenBegin++;

    std::size_t tokenEnd = tokenBegin;
    while (tokenEnd < limit && isProviderNameChar(line[tokenEnd])) tokenEnd++;

    if (tokenBegin == tokenEnd) return line;

    const std::string token    = line.substr(tokenBegin, tokenEnd - tokenBegin);
    const std::string internal = toInternalNameToken(token);
    auto              mapped   = plan.mapInternalName(ca::str::Utf8StringRef::from_cstr(internal.c_str()));
    if (mapped.equals(ca::str::Utf8StringRef::from_cstr(internal.c_str()))) return line;

    changed = true;
    return line.substr(0, tokenBegin) + restoreNameTokenShape(token, mapped.to_std_string()) +
           line.substr(tokenEnd);
}

std::vector<ca::u8> remapServiceProviderBytes(const MemberRemapPlan&     plan,
                                              const std::vector<ca::u8>& bytes, bool& changed)
{
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string       out;
    out.reserve(text.size());

    std::size_t lineBegin = 0;
    while (lineBegin < text.size()) {
        std::size_t lineEnd = lineBegin;
        while (lineEnd < text.size() && text[lineEnd] != '\n' && text[lineEnd] != '\r') lineEnd++;

        out += remapServiceProviderLine(plan, text.substr(lineBegin, lineEnd - lineBegin), changed);

        if (lineEnd < text.size()) {
            if (text[lineEnd] == '\r' && lineEnd + 1 < text.size() && text[lineEnd + 1] == '\n') {
                out += "\r\n";
                lineEnd += 2;
            }
            else {
                out.push_back(text[lineEnd]);
                lineEnd++;
            }
        }
        lineBegin = lineEnd;
    }

    return std::vector<ca::u8>(out.begin(), out.end());
}

struct ServiceResourceUpdate
{
    std::string         oldUri;
    std::string         newUri;
    std::vector<ca::u8> bytes;
};

std::vector<ServiceResourceUpdate> collectServiceResourceUpdates(ResourceContainer&     container,
                                                                 const MemberRemapPlan& plan,
                                                                 bool                   enabled)
{
    if (!enabled) return {};
    std::vector<ServiceResourceUpdate> updates;
    for (const auto& [uri, resource] : container.getResources()) {
        if (!resource || resource->getType() != ResourceType::Unknown ||
            !isServiceResourceUri(uri)) {
            continue;
        }
        auto* service = ca::core::tag_cast::cast<UnknownResource>(resource.get());

        bool changed = false;
        auto bytes   = remapServiceProviderBytes(plan, service->get(), changed);
        auto newUri  = remapServiceUri(plan, uri);
        if (newUri != uri) changed = true;
        if (!changed) continue;

        updates.push_back(ServiceResourceUpdate{uri, std::move(newUri), std::move(bytes)});
    }
    return updates;
}

void applyServiceResourceUpdates(ResourceContainer&                        container,
                                 const std::vector<ServiceResourceUpdate>& updates, bool& changed)
{
    auto& resources = container.getResources();
    for (const auto& update : updates) {
        auto node = resources.extract(update.oldUri);
        if (node.empty()) continue;

        if (update.oldUri != update.newUri && resources.find(update.newUri) == resources.end()) {
            resources.emplace(update.newUri,
                              std::make_unique<UnknownResource>(update.newUri, update.bytes));
        }
        else {
            auto* service = ca::core::tag_cast::cast<UnknownResource>(node.mapped().get());
            service->set(update.bytes);
            resources.insert(std::move(node));
        }
        changed = true;
    }
}

bool remapManifestClassAttribute(Manifest& manifest, const MemberRemapPlan& plan,
                                 const std::string& key)
{
    const std::string value = manifest.getMainAttribute(key);
    if (value.empty()) return false;

    const std::string oldName = toInternalNameToken(value);
    auto              mapped  = plan.mapInternalName(ca::str::Utf8StringRef::from_cstr(oldName.c_str()));
    if (mapped.equals(ca::str::Utf8StringRef::from_cstr(oldName.c_str()))) return false;

    manifest.setMainAttribute(key, restoreNameTokenShape(value, mapped.to_std_string()));
    return true;
}

bool remapManifestResource(ManifestResource& resource, const MemberRemapPlan& plan)
{
    Manifest manifest = resource.get();
    bool     changed  = false;

    changed |= remapManifestClassAttribute(manifest, plan, "Main-Class");
    changed |= remapManifestClassAttribute(manifest, plan, "Launcher-Agent-Class");
    changed |= remapManifestClassAttribute(manifest, plan, "Premain-Class");
    changed |= remapManifestClassAttribute(manifest, plan, "Agent-Class");
    changed |= remapManifestClassAttribute(manifest, plan, "JavaFX-Application-Class");

    if (changed) resource.set(std::move(manifest));
    return changed;
}

bool isMappingTextResourceUri(const std::string& uri)
{
    const std::string lower = toLowerAscii(uri);
    return endsWith(lower, ".map") || endsWith(lower, ".mapping") || endsWith(lower, ".srg") ||
           endsWith(lower, ".tsrg") || endsWith(lower, ".tiny") || endsWith(lower, ".enigma") ||
           endsWith(lower, ".jadx") || endsWith(lower, ".pro") || endsWith(lower, ".proguard") ||
           endsWith(lower, ".r8");
}

bool looksLikeTextBytes(const std::vector<ca::u8>& bytes)
{
    for (auto byte : bytes) {
        if (byte == 0) return false;
        if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r') return false;
    }
    return true;
}

bool isMappingTokenChar(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '$' || c == '/' || c == '.' || c == '[' || c == ']' || c == '(' || c == ')' ||
           c == ';';
}

std::string remapClassNameToken(const MemberRemapPlan& plan, const std::string& token)
{
    const std::string internal = toInternalNameToken(token);
    auto              internalRef = ca::str::Utf8StringRef::from_cstr(internal.c_str());
    auto              mapped = plan.mapInternalName(internalRef);
    if (mapped.equals(internalRef)) return token;
    return restoreNameTokenShape(token, mapped.to_std_string());
}

std::string remapMemberOwnerPrefix(const MemberRemapPlan& plan, const std::string& token,
                                   char separator)
{
    const auto pos = token.rfind(separator);
    if (pos == std::string::npos || pos == 0 || pos + 1 >= token.size()) return token;

    const std::string owner  = token.substr(0, pos);
    const std::string mapped = remapClassNameToken(plan, owner);
    if (mapped == owner) return token;
    return mapped + token.substr(pos);
}

std::string remapMappingToken(const MemberRemapPlan& plan, const std::string& token)
{
    if (token.empty()) return token;

    auto tokenRef = ca::str::Utf8StringRef::from_cstr(token.c_str());

    if (token.find('(') != std::string::npos) {
        auto mapped = plan.mapMethodDescriptor(tokenRef);
        if (!mapped.equals(tokenRef)) return mapped.to_std_string();
    }
    if (token.front() == '[' || (token.front() == 'L' && token.find(';') != std::string::npos)) {
        auto mapped = plan.mapFieldDescriptor(tokenRef);
        if (!mapped.equals(tokenRef)) return mapped.to_std_string();
    }

    const std::string classMapped = remapClassNameToken(plan, token);
    if (classMapped != token) return classMapped;

    const std::string slashMemberMapped = remapMemberOwnerPrefix(plan, token, '/');
    if (slashMemberMapped != token) return slashMemberMapped;

    return remapMemberOwnerPrefix(plan, token, '.');
}

std::vector<ca::u8> remapMappingTextBytes(const MemberRemapPlan&     plan,
                                          const std::vector<ca::u8>& bytes, bool& changed)
{
    if (!looksLikeTextBytes(bytes)) return bytes;

    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    std::string       out;
    out.reserve(text.size());

    for (std::size_t i = 0; i < text.size();) {
        if (!isMappingTokenChar(text[i])) {
            out.push_back(text[i]);
            i++;
            continue;
        }

        std::size_t end = i + 1;
        while (end < text.size() && isMappingTokenChar(text[end])) end++;

        const std::string token  = text.substr(i, end - i);
        const std::string mapped = remapMappingToken(plan, token);
        if (mapped != token) changed = true;
        out += mapped;
        i = end;
    }

    return std::vector<ca::u8>(out.begin(), out.end());
}

bool remapMappingTextResources(ResourceContainer& container, const MemberRemapPlan& plan)
{
    bool changed = false;
    for (auto& [uri, resource] : container.getResources()) {
        if (!resource || resource->getType() != ResourceType::Unknown ||
            !isMappingTextResourceUri(uri)) {
            continue;
        }

        auto* mapping         = ca::core::tag_cast::cast<UnknownResource>(resource.get());
        bool  resourceChanged = false;
        auto  bytes           = remapMappingTextBytes(plan, mapping->get(), resourceChanged);
        if (!resourceChanged) continue;

        mapping->set(std::move(bytes));
        changed = true;
    }
    return changed;
}

bool isAuthorizedTextResourceUri(const RemapPolicy& policy, const std::string& uri)
{
    return std::find(policy.authorizedTextResourceUris.begin(),
                     policy.authorizedTextResourceUris.end(),
                     uri) != policy.authorizedTextResourceUris.end();
}

bool remapGenericTextResources(ResourceContainer& container, const MemberRemapPlan& plan,
                               const RemapPolicy& policy)
{
    bool changed = false;
    for (auto& [uri, resource] : container.getResources()) {
        if (!resource || resource->getType() != ResourceType::Unknown ||
            isServiceResourceUri(uri) || isMappingTextResourceUri(uri) ||
            !isAuthorizedTextResourceUri(policy, uri)) {
            continue;
        }
        auto* textResource    = ca::core::tag_cast::cast<UnknownResource>(resource.get());
        bool  resourceChanged = false;
        auto  bytes           = remapMappingTextBytes(plan, textResource->get(), resourceChanged);
        if (!resourceChanged) continue;

        textResource->set(std::move(bytes));
        changed = true;
    }
    return changed;
}

}   //namespace

bool applyResourceRemapPlan(ResourceContainer& container, ca::str::Utf8StringArena& arena,
                            const MemberRemapPlan& plan)
{
    return applyResourceRemapPlan(container, arena, plan, RemapPolicy::bytecodeOnly());
}

bool applyResourceRemapPlan(ResourceContainer& container, ca::str::Utf8StringArena& arena,
                            const MemberRemapPlan& plan, const RemapPolicy& policy)
{
    if (plan.empty()) return false;

    bool          changed = false;
    AliasAnalysis aliases;
    //P0（perf/alias-closure）：消费面只有下方 applyMemberRemapPlan 的反射
    //接收者富集——alias 摘要收缩到反射闭包，不再付全罐数据流固定点
    //（全罐 build 在真实罐上 >600s 不收敛，§5/§6.3-3）。
    aliases.buildReflectionClosure(container, arena);
    std::vector<std::pair<std::string, std::string>> resourceRenames;

    for (auto& [uri, resource] : container.getResources()) {
        if (!resource || !ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto*       cls   = ca::core::tag_cast::cast<ClassResource>(resource.get());
        const auto& bytes = cls->get();
        if (bytes.empty()) continue;

        try {
            ClassNode   classNode;
            ClassReader reader(arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
            reader.accept(&classNode, 0);

            auto       oldName = classNode.name;
            const bool classChanged =
                applyMemberRemapPlan(classNode, arena, plan, policy, &aliases);
            if (classChanged) {
                ClassWriter writer(arena, 0);
                classNode.accept(&writer);
                cls->set(writer.toByteArray());
                changed = true;
            }

            if (policy.bytecode && policy.resources && uri == classResourceUri(oldName.to_std_string())) {
                const std::string newUri = classResourceUri(plan.mapInternalName(oldName).to_std_string());
                if (newUri != uri) resourceRenames.emplace_back(uri, newUri);
            }
        }
        catch (...) {
            //Keep malformed or unsupported class resources unchanged.
        }
    }

    renameClassResources(container, resourceRenames, changed);
    if (policy.resources) {
        //META-INF/services 与 MANIFEST 主属性是 resources 面下的细分门
        //（luiox/Q6），默认开——与既有 resources=true 行为一致。
        applyServiceResourceUpdates(
            container, collectServiceResourceUpdates(container, plan, policy.serviceLoader), changed);
        changed |= remapMappingTextResources(container, plan);
        changed |= remapGenericTextResources(container, plan, policy);
        if (policy.manifest) {
            for (auto& [uri, resource] : container.getResources()) {
                (void)uri;
                if (!resource || resource->getType() != ResourceType::Manifest) continue;
                changed |=
                    remapManifestResource(*ca::core::tag_cast::cast<ManifestResource>(resource.get()), plan);
            }
        }
    }
    return changed;
}

}   //namespace mjt::base
