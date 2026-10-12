#pragma once

#include <algorithm>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/core/type.hpp"
#include "mbase/asm/insn/class_util.hpp"
#include "mbase/asm/insn/insn_builder.hpp"
#include "mbase/asm/remapper/member_remap_plan.hpp"
#include "mbase/model/resource_container.hpp"
#include "mbase/model/resource_model.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

//把 int 拼成 Utf8String 前缀+数字（避免重复样板）。
static ca::str::Utf8String nameWithIndex(const char* prefix, int index)
{
    ca::str::Utf8StringBuilder b;
    b.append(prefix);
    b.append(std::to_string(index).c_str());
    return b.build();
}

namespace {

bool readRecoverClassNode(const ClassResource& resource, ca::str::Utf8StringArena& arena,
                          ClassNode& classNode)
{
    const auto& bytes = resource.get();
    if (bytes.empty()) return false;
    try {
        ClassReader reader(arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
        reader.accept(&classNode, ClassReader::SKIP_FRAMES);
        return !classNode.name.is_empty();
    }
    catch (...) {
        return false;
    }
}

std::vector<ca::u8> writeRecoverClassNode(ca::str::Utf8StringArena& arena, ClassNode& classNode)
{
    ClassWriter writer(arena, 0);
    classNode.accept(&writer);
    return writer.toByteArray();
}

std::string memberUseKey(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                         const ca::str::Utf8StringRef& desc)
{
    return owner.to_std_string() + "\n" + name.to_std_string() + "\n" + desc.to_std_string();
}

struct StaticMemberUse
{
    bool usedOuter = false;
};

struct RecoverLoadedClass
{
    std::string                uri;
    ClassResource*             resource = nullptr;
    std::unique_ptr<ClassNode> classNode;
};

std::vector<RecoverLoadedClass> loadRecoverClasses(ResourceContainer&        container,
                                                   ca::str::Utf8StringArena& arena)
{
    std::vector<RecoverLoadedClass> classes;
    for (auto& [uri, resource] : container.getResources()) {
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto* classResource = ca::core::tag_cast::cast<ClassResource>(resource.get());
        auto  classNode     = std::make_unique<ClassNode>();
        if (!readRecoverClassNode(*classResource, arena, *classNode)) continue;
        classes.push_back({uri, classResource, std::move(classNode)});
    }
    std::sort(classes.begin(), classes.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.classNode->name.to_std_string() < rhs.classNode->name.to_std_string();
    });
    return classes;
}

std::string renamedClassBase(const std::string& oldName)
{
    const auto slash = oldName.rfind('/');
    if (slash == std::string::npos) return "Class";
    return oldName.substr(0, slash + 1) + "Class";
}

}   //namespace

//============================================================================
//ClassMemberSorter
//Java: recover/ClassMemberSorter.java，AsmClassPass（@PassInfo name="MemberSorter"）。
//字段先按 desc 再按 name 字典序；方法 <clinit> 最前、其次 <init>，其余按 name 字典序。
//============================================================================

//============================================================================
//StackTraceRecover
//Java: recover/StackTraceRecover.java，AsmClassPass（@PassInfo name="StackTraceRecover"）。
//1) 恢复 sourceDebug/sourceFile = name.replace("/",".") + ".java"
//2) 每个 try-catch handler 头部插入 dup;invokevirtual Exception.printStackTraceV
//3) 每个 label 后插入 LineNumberNode（行号递增，溢出回绕）
//============================================================================

//============================================================================
//LocalVariableInfoRebuilder
//Java: recover/LocalVariableInfoRebuilder.java，AsmMethodPass。
//扫描 VarInsnNode 构造 slot->usages 表，重建 localVariables（this/args/locals）。
//============================================================================

//============================================================================
//AccessModifierTightener / ClassRenamer —— Global 级，跨类，搭框架占位。
//============================================================================

}   //namespace mjt::base
