#pragma once

//structure_detail.hpp
//
//结构清理类 pass（ContinuousLabelMerger / LabelDeadVarStoreEliminator /
//EnumCastCheckRemover / UselessTryCatchRemover / UnusedMethodRemover /
//EmptyClassRemover / UnusedClassRemover / UnusedFieldRemover /
//GlobalUnusedMethodRemover / StaticFieldDefaultInliner）共享的私有类型和工具。
//不声明任何 Asm*Pass。
//
//重构说明：原文件曾承载 5 个不同家族（结构清理 + 小型内联 + 纯函数 + 字符串池 + 工具类
//合并），1185 行。现已按家族拆分：
//- class resource 读写 / keep metadata / 全局可达性 helper → class_io_detail.hpp
//- 小型内联族（SmallInlineKind / LoadedClass / runSmallInliner 等）→ inline_detail.hpp
//- 纯函数族（kPureAnnotationDesc / runLocalPureFunctionInliner 等）→ pure_function_detail.hpp
//- 字符串池（GlobalStrPoolOptimizer）→ 已合回 global_str_pool_optimizer.cpp
//- 工具类合并（ToolClassCombiner）→ 已合回 tool_class_combiner.cpp
//本文件只保留与结构清理类 pass 直接相关的私有 helper。

#include "class_io_detail.hpp"
#include "mbase/asm/remapper/member_remap_plan.hpp"
#include "mbase/model/resource_model.hpp"
#include "mbase/reference/global_reachability.hpp"
#include "mbase/reference/reference_index_analysis.hpp"
#include "libca/core/tag_cast.hpp"
#include <unordered_set>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;

namespace {

//判断指令是否为算术指令（opcode 在 IADD 到 LXOR 之间，与 Java InsnUtil.isArithmeticInsn 一致）
static bool isArithmeticInsn(AbstractInsnNode* insn)
{
    int op = insn->getOpcode();
    return op >= Opcodes::IADD && op <= Opcodes::LXOR;
}

}   //namespace

//========== ContinuousLabelMerger ==========

//========== LabelDeadVarStoreEliminator ==========

//============================================================================
//EnumCastCheckRemover（忠实移植 Java structure/EnumCastCheckRemover.java）
//枚举类首个方法（values）里 invokevirtual [LEnum; + 其后 checkcast 一并移除。
//============================================================================

//============================================================================
//UselessTryCatchRemover（忠实移植 Java structure/UselessTryCatchRemover.java）
//消除仅抛 NPE（aconst_null;athrow）+ handler 仅吞异常（pop[;goto]）的 try-catch。
//============================================================================

namespace {

bool isSupportedNpeCatch(const TryCatchBlockNode& tryCatch)
{
    return tryCatch.type.is_empty() || tryCatch.type.equals("java/lang/NullPointerException");
}

bool matchNpeTryBody(LabelNode* startNode, LabelNode* endNode,
                     AbstractInsnNode*& nullInsn, AbstractInsnNode*& throwInsn)
{
    nullInsn = startNode ? startNode->getNext() : nullptr;
    throwInsn = nullInsn ? nullInsn->getNext() : nullptr;
    if (!nullInsn || !throwInsn || throwInsn->getNext() != endNode) return false;
    return nullInsn->getOpcode() == Opcodes::ACONST_NULL &&
           throwInsn->getOpcode() == Opcodes::ATHROW;
}

bool matchNpeHandler(LabelNode* handlerNode, const std::unordered_set<Label*>& knownLabels,
                     Label*& continuation)
{
    auto* pop = handlerNode ? handlerNode->getNext() : nullptr;
    if (!pop || pop->getOpcode() != Opcodes::POP) return false;

    auto* next = pop->getNext();
    if (!next) return false;
    if (next->getType() == AbstractInsnNode::LABEL) {
        continuation = ca::core::tag_cast::cast<LabelNode*>(next)->label;
        return continuation != handlerNode->label;
    }
    if (next->getOpcode() != Opcodes::GOTO) return false;

    auto* jump = ca::core::tag_cast::cast<JumpInsnNode*>(next);
    if (!jump->label || knownLabels.count(jump->label) == 0) return false;
    auto* boundary = next->getNext();
    if (!boundary || boundary->getType() != AbstractInsnNode::LABEL) return false;
    continuation = jump->label;
    return continuation != handlerNode->label;
}

}   //namespace

//============================================================================
//UnusedMethodRemover（忠实移植 Java structure/UnusedMethodRemover.java）
//移除类内未被调用的 private static 方法（跳过 init/clinit）。
//============================================================================

//============================================================================
//EmptyClassRemover
//忠实迁移 legacy structure/EmptyClassRemover.java：删除无字段且无方法的 class。
//============================================================================

//============================================================================
//UnusedClassRemover
//删除 default/global roots 不可达的 ClassResource；坏字节和非 class 资源保留。
//============================================================================

//============================================================================
//UnusedFieldRemover
//删除可达 class 内没有字段引用保持的 private field；public/protected 字段保留。
//============================================================================

//============================================================================
//GlobalUnusedMethodRemover
//删除可达 class 内不可达的 private non-constructor method。
//============================================================================

//============================================================================
//StaticFieldDefaultInliner（忠实移植 Java structure/StaticFieldDefaultInliner.java）
//clinit 中 putstatic 常量 → 写入字段 ConstantValue（value），并删前导常量+putstatic。
//============================================================================

}   //namespace mjt::base
