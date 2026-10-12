#pragma once

#include <utility>
#include <vector>

#include "jbytecode/jbytecode.hpp"

namespace mjt::base {

///局部变量使用表：slot -> (store 指令下标列表, load 指令下标列表)，下标从小到大。
///对齐 Java MethodUtil.getLocalVarUsages / getAreaLocalVarUsages 的 Pair<List,List>。
using LocalVarUsages = std::unordered_map<int, std::pair<std::vector<int>, std::vector<int>>>;

struct ClassUtil
{
    static ca::str::Utf8StringRef getName(jbytecode::tree::ClassNode& cn);
    static bool                   isPublic(jbytecode::tree::ClassNode& cn);
    static bool                   isInterface(jbytecode::tree::ClassNode& cn);
    static bool                   isEnum(jbytecode::tree::ClassNode& cn);
    static bool                   isSynthetic(jbytecode::tree::ClassNode& cn);
    ///移除类的 ACC_SYNTHETIC（对齐 Java ClassUtil.removeSynthetic）。
    static void                         removeSynthetic(jbytecode::tree::ClassNode& cn);
    static jbytecode::tree::MethodNode* findMethod(jbytecode::tree::ClassNode&   cn,
                                                   const ca::str::Utf8StringRef& name,
                                                   const ca::str::Utf8StringRef& desc);
    static jbytecode::tree::FieldNode*  findField(jbytecode::tree::ClassNode&   cn,
                                                  const ca::str::Utf8StringRef& name,
                                                  const ca::str::Utf8StringRef& desc);
};

struct FieldUtil
{
    static bool isPublic(jbytecode::tree::FieldNode& fn);
    static bool isPrivate(jbytecode::tree::FieldNode& fn);
    static bool isStatic(jbytecode::tree::FieldNode& fn);
    static bool isFinal(jbytecode::tree::FieldNode& fn);
    static bool isSynthetic(jbytecode::tree::FieldNode& fn);
    ///移除字段的 ACC_FINAL（对齐 Java FieldUtil.removeFinal）。
    static void removeFinal(jbytecode::tree::FieldNode& fn);
    ///移除字段的 ACC_SYNTHETIC（对齐 Java FieldUtil.removeSynthetic）。
    static void removeSynthetic(jbytecode::tree::FieldNode& fn);
};

struct MethodUtil
{
    static bool isPublic(jbytecode::tree::MethodNode& mn);
    static bool isPrivate(jbytecode::tree::MethodNode& mn);
    static bool isStatic(jbytecode::tree::MethodNode& mn);
    static bool isAbstract(jbytecode::tree::MethodNode& mn);
    static bool isNative(jbytecode::tree::MethodNode& mn);
    static bool isSynthetic(jbytecode::tree::MethodNode& mn);
    static bool isBridge(jbytecode::tree::MethodNode& mn);
    static bool isConstructor(jbytecode::tree::MethodNode& mn);
    static bool isClinit(jbytecode::tree::MethodNode& mn);

    ///移除方法的 ACC_SYNTHETIC（对齐 Java MethodUtil.removeSynthetic）。
    static void removeSynthetic(jbytecode::tree::MethodNode& mn);
    ///移除方法的 ACC_BRIDGE（对齐 Java MethodUtil.removeBridge）。
    static void removeBridge(jbytecode::tree::MethodNode& mn);

    static int                    getArgCount(ca::str::Utf8StringRef desc);
    static ca::str::Utf8StringRef getReturnType(ca::str::Utf8StringRef desc);
    static int                    evalCodeSize(jbytecode::tree::MethodNode& mn);

    ///全方法扫描：记录每个 slot 的 store/load 指令下标（对齐 Java getLocalVarUsages）。
    static void getLocalVarUsages(jbytecode::tree::MethodNode& methodNode,
                                  LocalVarUsages&              localVarUsages);
    ///区间扫描 [start, end)：记录每个 slot 的 store/load 指令下标（对齐 Java
    ///getAreaLocalVarUsages）。 区间是左闭右开；start/end 指指令下标。
    static void getAreaLocalVarUsages(jbytecode::tree::MethodNode& methodNode, int start, int end,
                                      LocalVarUsages& localVarUsages);
};

}   //namespace mjt::base
