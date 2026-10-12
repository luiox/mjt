#pragma once

#include "garena/arena.hpp"
#include "libca/core/datatype.hpp"
#include "jbytecode/tree/insn_list.hpp"
#include <string>
#include <unordered_set>

namespace mjt::base {

struct InsnUtil
{
    //========== 常量 → 指令 ==========
    //B9 建点规范：工厂产物在调用方给定 arena 分配（寿命=归属方法）。
    static jbytecode::tree::AbstractInsnNode* getIntInsn(garena::Arena& arena, int value);
    static jbytecode::tree::AbstractInsnNode* getLongInsn(garena::Arena& arena, ca::i64 value);
    static jbytecode::tree::AbstractInsnNode* getFloatInsn(garena::Arena& arena, float value);
    static jbytecode::tree::AbstractInsnNode* getDoubleInsn(garena::Arena& arena, double value);
    //对应 Java InsnUtil.getTypeInsn：class literal 常量（ldc Foo.class）→ LdcInsnNode(Type)
    static jbytecode::tree::AbstractInsnNode* getTypeInsn(garena::Arena& arena,
                                                          const jbytecode::core::Type& t);
    //便利重载：直接给绑定 owner arena 的指令列表（列表须已绑定，debug 断言）。
    static jbytecode::tree::AbstractInsnNode* getIntInsn(jbytecode::tree::InsnList& list, int value);
    static jbytecode::tree::AbstractInsnNode* getLongInsn(jbytecode::tree::InsnList& list, ca::i64 value);
    static jbytecode::tree::AbstractInsnNode* getFloatInsn(jbytecode::tree::InsnList& list, float value);
    static jbytecode::tree::AbstractInsnNode* getDoubleInsn(jbytecode::tree::InsnList& list, double value);
    static jbytecode::tree::AbstractInsnNode* getTypeInsn(jbytecode::tree::InsnList& list,
                                                          const jbytecode::core::Type& t);
    //DEV-11 修订：堆版 = InsnBuilder/MatchContext 暂存管线专用建点入口（暂存节点
    //必须堆分配，arena 版会被 builder 析构盲 delete，见 insn_builder.hpp:37 冻结
    //注释）；直插绑定列表一律用上方 (Arena&, v) / (InsnList&, v) 重载。
    //退役前提 = 暂存器语义重设计。
    static jbytecode::tree::AbstractInsnNode* getIntInsn(int value);
    static jbytecode::tree::AbstractInsnNode* getLongInsn(ca::i64 value);
    static jbytecode::tree::AbstractInsnNode* getFloatInsn(float value);
    static jbytecode::tree::AbstractInsnNode* getDoubleInsn(double value);
    static jbytecode::tree::AbstractInsnNode* getTypeInsn(const jbytecode::core::Type& t);

    //========== 指令分类 ==========
    static bool isReturn(int opcode);
    static bool isIf(int opcode);
    ///@brief 条件跳转 opcode 取反（IFEQ↔IFNE / IF_ICMPLT↔IF_ICMPGE / IFNULL↔IFNONNULL 等）。
    ///非条件跳转原样返回。
    static int  invertIf(int opcode);
    static bool isIfNull(int opcode);
    static bool isGoto(int opcode);
    static bool isSwitch(int opcode);
    static bool isLoad(int opcode);
    static bool isStore(int opcode);
    static bool isArrayLoad(int opcode);
    static bool isArrayStore(int opcode);
    static bool isConstant(int opcode);
    static bool isInvoke(int opcode);
    static bool isFieldAccess(int opcode);
    static bool isThrow(int opcode);
    static bool isMonitor(int opcode);
    static bool isStackOp(int opcode);

    //========== Stack 分析 ==========
    static int getStackPushCount(int opcode);
    static int getStackPopCount(int opcode);
    static int getStackEffect(int opcode);

    ///@brief opcode 是否在 IADD..LXOR 范围内（整型/长整型算术运算）
    static bool isArithmeticInsn(int opcode);
    ///@brief opcode 是否在 I2L..D2F 范围内（类型转换指令）
    static bool isTypeCastInsn(int opcode);
    ///@brief opcode 是否是基本块终止指令（return/goto/switch/throw/if）
    static bool isTerminator(int opcode);
    ///@brief 指令是否是纯计算（常量加载/算术运算/类型转换/栈操作），不含控制流、字段访问、方法调用
    static bool isPureComputation(jbytecode::tree::AbstractInsnNode* insn);
    ///@brief 节点是否是 LabelNode（跳转目标锚点；不涵盖 Frame/Line，见 isBoundaryNode）。
    static bool isLabelNode(const jbytecode::tree::AbstractInsnNode* insn);
    ///@brief 节点是否是 Label/FrameNode/LineNumberNode 边界节点
    static bool isBoundaryNode(jbytecode::tree::AbstractInsnNode* insn);

    ///@brief 收集 StackMap frame 中引用的标签。
    ///
    ///frame 的 local/stack 值可能携带 `Label*` 或 `const Label*`，这些标签与跳转和
    ///异常表引用具有相同的生命周期约束。调用方只借用输出集合，不转移标签所有权。
    static void collectFrameLabels(
        const std::vector<ca::core::Any>& values,
        std::unordered_set<jbytecode::core::Label*>& labels);

    //========== 合成边界 label（dev_plan E pilot）==========

    ///@brief 首尾合成边界 label 组：`ensureBoundaryLabels` 实际新增的节点
    ///（未新增的端为 nullptr）。
    struct BoundaryLabels
    {
        jbytecode::tree::LabelNode* start = nullptr;
        jbytecode::tree::LabelNode* end   = nullptr;
    };

    ///@brief 确保指令列表首尾是 LabelNode，作为临时分析边界。
    ///
    ///用于需要「首尾必有 label」的扫描类 pass。**列表必须非空**（调用方先判空）。
    ///合成边界（Label + LabelNode，B8 起经新 clone/建点协议）在 `arena` 上分配——
    ///传目标方法的 `arena`，合成边界 label 归属目标方法。新增的边界节点是合成
    ///边界，调用方必须在所有返回路径上经 `removeBoundaryLabels` 撤销（arena 产物
    ///仅摘链、随 arena 统一析构）——边界泄漏到输入会在 unchanged 路径产生虚假
    ///changed。`insns` 若绑定 arena，应绑定同一 arena。
    static BoundaryLabels
    ensureBoundaryLabels(jbytecode::tree::InsnList& insns, garena::Arena& arena);

    ///@brief 撤销 `ensureBoundaryLabels` 新增的合成边界（从列表摘链；arena 产物
    ///不做容器级 delete，随 arena 统一析构）。
    static void removeBoundaryLabels(jbytecode::tree::InsnList&            insns,
                                     const BoundaryLabels& boundary);

    ///@brief 获取指令从操作数栈消耗的元素数（比 getStackPopCount 更详尽的实现）
    static int getConsumeStackCount(int opcode);

    //========== 常量提取 ==========
    static bool getIntValue(jbytecode::tree::AbstractInsnNode* insn, int& out);
    static bool getLongValue(jbytecode::tree::AbstractInsnNode* insn, ca::i64& out);
    static bool getFloatValue(jbytecode::tree::AbstractInsnNode* insn, float& out);
    static bool getDoubleValue(jbytecode::tree::AbstractInsnNode* insn, double& out);
    static bool getStringValue(jbytecode::tree::AbstractInsnNode* insn, std::string& out);
};

}   //namespace mjt::base
