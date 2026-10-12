#pragma once

//class_io_detail.hpp
//
//optimize pass 各家族共享的 class resource 读写、keep metadata 检测和重写提交工具。
//不声明任何 Asm*Pass；只提供匿名命名空间内的私有 helper，由引用本文件的家族
//detail / pass 实现单元复用。
//
//拆分自原 structure_detail.hpp 顶部公共区段（readClassNode / writeClassNode /
//serializeClassNode / hasFieldKeepMetadata / hasMethodKeepMetadata /
//PendingClassRewrite / commitClassRewrites / normalizeBinaryClassName /
//appendServiceClassNames / collectResourceKeepClasses / allClassResourcesReadable /
//isPrivateAccess）。

#include "jbytecode/core/opcodes.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "mbase/asm/insn/class_util.hpp"
#include "mbase/model/resource_model.hpp"
#include "libca/core/tag_cast.hpp"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

bool readClassNode(const ClassResource& resource, ca::str::Utf8StringArena& arena,
                   ClassNode& classNode, ca::i32 readerFlags = 0)
{
    const auto& bytes = resource.get();
    if (bytes.empty()) return false;
    try {
        ClassReader reader(arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
        reader.accept(&classNode, readerFlags);
        return !classNode.name.is_empty();
    }
    catch (...) {
        return false;
    }
}

bool readClassName(const ClassResource& resource, ca::str::Utf8StringArena& arena,
                   std::string& outName)
{
    ClassNode classNode;
    if (!readClassNode(resource,
                       arena,
                       classNode,
                       ClassReader::SKIP_DEBUG | ClassReader::SKIP_FRAMES))
        return false;
    outName = classNode.name.to_std_string();
    return true;
}

void writeClassNode(ClassResource& resource, ca::str::Utf8StringArena& arena, ClassNode& classNode)
{
    ClassWriter writer(arena, 0);
    classNode.accept(&writer);
    resource.set(writer.toByteArray());
}

bool serializeClassNode(ClassNode& classNode, ca::str::Utf8StringArena& arena,
                        std::vector<ca::u8>& out)
{
    try {
        ClassWriter writer(arena, 0);
        classNode.accept(&writer);
        out = writer.toByteArray();
        return !out.empty();
    }
    catch (...) {
        out.clear();
        return false;
    }
}

bool hasFieldKeepMetadata(const FieldNode& field)
{
    return !field.signature.is_empty() || !field.visibleAnnotations.empty() ||
           !field.invisibleAnnotations.empty() || !field.visibleTypeAnnotations.empty() ||
           !field.invisibleTypeAnnotations.empty();
}

bool hasMethodKeepMetadata(const MethodNode& method)
{
    return !method.signature.is_empty() || !method.visibleAnnotations.empty() ||
           !method.invisibleAnnotations.empty() || !method.visibleTypeAnnotations.empty() ||
           !method.invisibleTypeAnnotations.empty() ||
           !method.visibleParameterAnnotations.empty() ||
           !method.invisibleParameterAnnotations.empty() || method.annotationDefault != nullptr ||
           !method.visibleLocalVariableAnnotations.empty() ||
           !method.invisibleLocalVariableAnnotations.empty();
}

struct PendingClassRewrite
{
    ClassResource*       resource = nullptr;
    std::vector<ca::u8> bytes;
};

void commitClassRewrites(std::vector<PendingClassRewrite>& rewrites)
{
    for (auto& rewrite : rewrites) {
        rewrite.resource->set(std::move(rewrite.bytes));
    }
}

std::string normalizeBinaryClassName(std::string value)
{
    auto trim = [](std::string& text) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            text.clear();
            return;
        }
        const auto last = text.find_last_not_of(" \t\r\n");
        text            = text.substr(first, last - first + 1);
    };
    trim(value);
    if (value.empty()) return {};
    for (auto& ch : value) {
        if (ch == '.') ch = '/';
    }
    return value;
}

void appendServiceClassNames(const UnknownResource& resource,
                             std::unordered_set<std::string>& keepNames)
{
    std::string content(resource.get().begin(), resource.get().end());
    std::size_t cursor = 0;
    while (cursor <= content.size()) {
        const auto end = content.find('\n', cursor);
        std::string line = content.substr(cursor, end == std::string::npos ? std::string::npos
                                                                            : end - cursor);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.resize(comment);
        const auto provider = normalizeBinaryClassName(std::move(line));
        if (!provider.empty()) keepNames.insert(provider);
        if (end == std::string::npos) break;
        cursor = end + 1;
    }
}

bool collectResourceKeepClasses(ResourceContainer& container, ca::str::Utf8StringArena& arena,
                                std::unordered_set<std::string>& keepNames)
{
    constexpr const char* kServicesPrefix = "META-INF/services/";
    constexpr const char* kManifestKeys[] = {"Main-Class", "Premain-Class", "Agent-Class",
                                             "Launcher-Agent-Class", "JavaFX-Application-Class"};
    for (const auto& [uri, resource] : container.getResources()) {
        if (const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(resource.get())) {
            ClassNode classNode;
            if (!readClassNode(*classResource, arena, classNode)) {
                return false;
            }
            if (classNode.moduleNode) keepNames.insert(classNode.name.to_std_string());
            continue;
        }
        if (const auto* manifest = ca::core::tag_cast::dyn_cast<const ManifestResource>(resource.get())) {
            for (const auto* key : kManifestKeys) {
                const auto name = normalizeBinaryClassName(manifest->get().getMainAttribute(key));
                if (!name.empty()) keepNames.insert(name);
            }
            continue;
        }
        if (uri.rfind(kServicesPrefix, 0) != 0) continue;
        const auto serviceName = normalizeBinaryClassName(uri.substr(std::strlen(kServicesPrefix)));
        if (!serviceName.empty()) keepNames.insert(serviceName);
        if (const auto* service = ca::core::tag_cast::dyn_cast<const UnknownResource>(resource.get())) {
            appendServiceClassNames(*service, keepNames);
        }
    }
    return true;
}

bool allClassResourcesReadable(ResourceContainer& container, ca::str::Utf8StringArena& arena)
{
    for (const auto& [uri, resource] : container.getResources()) {
        (void)uri;
        const auto* classResource = ca::core::tag_cast::dyn_cast<const ClassResource>(resource.get());
        if (!classResource) continue;
        ClassNode classNode;
        if (!readClassNode(*classResource, arena, classNode)) return false;
    }
    return true;
}

bool isPrivateAccess(ca::i32 access)
{
    return (access & Opcodes::ACC_PRIVATE) != 0;
}

}   //namespace

}   //namespace mjt::base
