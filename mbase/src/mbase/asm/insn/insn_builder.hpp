#pragma once

#include <functional>
#include <initializer_list>
#include <string>
#include <unordered_map>
#include <vector>

#include "garena/arena.hpp"
#include "jbytecode/jbytecode.hpp"

namespace mjt::base {

///@brief 指令构建器（移植自 legacy Java 版 InsnBuilder）。
///
///**设计意图**（对齐 legacy）：内部用扁平的 `std::vector<AbstractInsnNode*>` 收集节点，
///`add`/`addInsnNode` **只存指针、不改动节点的 prev/next/index**，节点在收集期保持孤立态。
///直到 `getInsnList` / `build` 时才逐个 `InsnList::add`，此时才建立链表关系。
///
///这样可以安全地「重用 AbstractInsnNode」：从原 InsnList 摘下的节点（prev/next 已置 null）
///放进 builder 不会与其原属链表冲突，避免了侵入式存储导致的「禁用模式 E」。
///
///**命名规则**：方法名贴近 JVM 助记符（领域命名优先于驼峰）。
///- 助记符本身带下划线的保留（`aconst_null` / `dup_x1` / `iconst_m1` / `if_icmpeq` …）；
///- 与 C++ 关键字冲突的，重复尾字母而非加下划线（`neww` / `gotoo` / `returnn` / `instanceoff`），
///以保证 tab 补全不被破坏。
///
///**构建模式**：所有指令方法返回 `void`（不做链式调用，对齐 legacy）。推荐用静态工厂
///`InsnBuilder::build([&](InsnBuilder& b){ ... })` 把构建过程包在一个 lambda 里。
class InsnBuilder
{
public:
    InsnBuilder() = default;
    ~InsnBuilder();

    ///B9（暂不启用，预留）：builder 是暂存器，暂存节点默认堆分配、由 appendTo/
    ///getInsnList 克隆进目标 arena 后由本析构 delete 原件。仅当调用方保证不走
    ///克隆路径（直接转移暂存节点）时才可设置此 arena，否则双重释放。
    void setNodeArena(garena::Arena& arena) { nodeArena_ = &arena; }
    [[nodiscard]] garena::Arena* nodeArena() const noexcept { return nodeArena_; }

    InsnBuilder(const InsnBuilder&)            = delete;
    InsnBuilder& operator=(const InsnBuilder&) = delete;
    InsnBuilder(InsnBuilder&& other) noexcept;
    InsnBuilder& operator=(InsnBuilder&& other) noexcept;

    //-- 底层收集接口 --

    ///接管一个孤立节点（断言其不在任何 InsnList：index==-1 && !prev && !next）。
    ///专给 `new` 出来的、或已从原链表 `remove` 下来的节点用。违例抛 std::invalid_argument。
    void add(::jbytecode::tree::AbstractInsnNode* insn);
    void addInsnNode(::jbytecode::tree::AbstractInsnNode* insn);

    ///接管若干孤立节点（可变参数）。
    void addInsnNode(std::initializer_list<::jbytecode::tree::AbstractInsnNode*> insns);
    ///接管若干孤立节点。
    void addInsnNode(const std::vector<::jbytecode::tree::AbstractInsnNode*>& insns);

    ///从列表摘下节点并交给 builder，保持 legacy addInsnList 的收集能力。
    void addInsnList(::jbytecode::tree::InsnList& list);
    void addInsnList(std::initializer_list<::jbytecode::tree::InsnList*> lists);
    void addInsnList(const std::vector<::jbytecode::tree::AbstractInsnNode*>& insns);

    ///删除并释放最后加入的节点（若空则无操作）。
    void deleteLastInsnNode();

    ///访问内部扁平存储（节点仍孤立，prev/next 为 null）。
    const std::vector<::jbytecode::tree::AbstractInsnNode*>& getAddList() const { return addList_; }

    //-- 产出 --

    ///把收集的节点逐个链入新 InsnList 并返回（move，节点所有权转移给返回值；addList_ 清空）。
    ::jbytecode::tree::InsnList getInsnList();
    ///输出未被 skipPredicate 过滤的节点；被过滤节点保留在 builder 中。
    ::jbytecode::tree::InsnList getInsnList(
        const std::function<bool(const ::jbytecode::tree::AbstractInsnNode*)>& skipPredicate);

    ///静态工厂：构造一个 builder，执行 action，返回其 getInsnList。推荐用法。
    static ::jbytecode::tree::InsnList build(const std::function<void(InsnBuilder&)>& action);

    ///把收集的节点 clone 一份追加到 target（不消费 builder，节点仍是孤立的副本）。
    ///B8 新 clone 协议：克隆产物在 `arena` 上分配——传克隆结果最终挂入的 MethodNode
    ///的 arena（通常即 target 的 owner arena）。片段内的 LabelNode 先行克隆，使
    ///Jump/Switch/Line 克隆重映射到片段内副本；外部借用 Label 同域直返保持 identity。
    void appendTo(::jbytecode::tree::InsnList& target, garena::Arena& arena) const;

    //──────────────────────────────────────────────────────────────
    //指令工厂方法（全部返回 void，贴近 JVM 助记符）。详见 InsnBuilder.cpp。
    //──────────────────────────────────────────────────────────────

    void insn(int opcode);

    void nop();
    void aconst_null();
    void iconst_m1();
    void iconst_0();
    void iconst_1();
    void iconst_2();
    void iconst_3();
    void iconst_4();
    void iconst_5();
    void lconst_0();
    void lconst_1();
    void fconst_0();
    void fconst_1();
    void fconst_2();
    void dconst_0();
    void dconst_1();

    void bipush(int value);
    void sipush(int value);

    void ldc(int value);
    void ldc(long long value);
    void ldc(float value);
    void ldc(double value);
    void ldc(const std::string& value);
    void ldc(const ::jbytecode::core::Type& value);
    void ldc(const ::jbytecode::core::Handle& value);

    //load
    void iload(int slot);
    void lload(int slot);
    void fload(int slot);
    void dload(int slot);
    void aload(int slot);
    void iload(const std::string& varName);
    void lload(const std::string& varName);
    void fload(const std::string& varName);
    void dload(const std::string& varName);
    void aload(const std::string& varName);

    //array load
    void iaload();
    void laload();
    void faload();
    void daload();
    void aaload();
    void baload();
    void caload();
    void saload();

    //store
    void istore(int slot);
    void lstore(int slot);
    void fstore(int slot);
    void dstore(int slot);
    void astore(int slot);
    void istore(const std::string& varName);
    void lstore(const std::string& varName);
    void fstore(const std::string& varName);
    void dstore(const std::string& varName);
    void astore(const std::string& varName);

    //array store
    void iastore();
    void lastore();
    void fastore();
    void dastore();
    void aastore();
    void bastore();
    void castore();
    void sastore();

    //pop / dup / swap
    void pop();
    void pop2();
    void dup();
    void dup_x1();
    void dup_x2();
    void dup2();
    void dup2_x1();
    void dup2_x2();
    void swap();

    //算术
    void iadd();
    void ladd();
    void fadd();
    void dadd();
    void isub();
    void lsub();
    void fsub();
    void dsub();
    void imul();
    void lmul();
    void fmul();
    void dmul();
    void idiv();
    void ldiv();
    void fdiv();
    void ddiv();
    void irem();
    void lrem();
    void frem();
    void drem();
    void ineg();
    void lneg();
    void fneg();
    void dneg();

    //移位 / 逻辑
    void ishl();
    void lshl();
    void ishr();
    void lshr();
    void iushr();
    void lushr();
    void iand();
    void land();
    void ior();
    void lor();
    void ixor();
    void lxor();

    void iinc(int slot, int incr);
    void iinc(const std::string& varName, int incr);

    //类型转换
    void i2l();
    void i2f();
    void i2d();
    void l2i();
    void l2f();
    void l2d();
    void f2i();
    void f2l();
    void f2d();
    void d2i();
    void d2l();
    void d2f();
    void i2b();
    void i2c();
    void i2s();

    //比较
    void lcmp();
    void fcmpl();
    void fcmpg();
    void dcmpl();
    void dcmpg();

    //label / line
    void label(::jbytecode::core::Label* label);
    void label(::jbytecode::tree::LabelNode* labelNode);
    void label(const std::string& labelName);
    void line(int num, ::jbytecode::core::Label* start);
    void line(int num, ::jbytecode::tree::LabelNode* startNode);
    void line(int num, const std::string& labelName);

    //控制流（跳转目标用 Label*）
    void ifeq(::jbytecode::tree::LabelNode* label);
    void ifne(::jbytecode::tree::LabelNode* label);
    void iflt(::jbytecode::tree::LabelNode* label);
    void ifge(::jbytecode::tree::LabelNode* label);
    void ifgt(::jbytecode::tree::LabelNode* label);
    void ifle(::jbytecode::tree::LabelNode* label);
    void if_icmpeq(::jbytecode::tree::LabelNode* label);
    void if_icmpne(::jbytecode::tree::LabelNode* label);
    void if_icmplt(::jbytecode::tree::LabelNode* label);
    void if_icmpge(::jbytecode::tree::LabelNode* label);
    void if_icmpgt(::jbytecode::tree::LabelNode* label);
    void if_icmple(::jbytecode::tree::LabelNode* label);
    void if_acmpeq(::jbytecode::tree::LabelNode* label);
    void if_acmpne(::jbytecode::tree::LabelNode* label);
    void ifnull(::jbytecode::tree::LabelNode* label);
    void ifnonnull(::jbytecode::tree::LabelNode* label);
    void gotoo(::jbytecode::tree::LabelNode* label);
    void jsr(::jbytecode::tree::LabelNode* label);
    void ifeq(::jbytecode::core::Label* label);
    void ifne(::jbytecode::core::Label* label);
    void iflt(::jbytecode::core::Label* label);
    void ifge(::jbytecode::core::Label* label);
    void ifgt(::jbytecode::core::Label* label);
    void ifle(::jbytecode::core::Label* label);
    void if_icmpeq(::jbytecode::core::Label* label);
    void if_icmpne(::jbytecode::core::Label* label);
    void if_icmplt(::jbytecode::core::Label* label);
    void if_icmpge(::jbytecode::core::Label* label);
    void if_icmpgt(::jbytecode::core::Label* label);
    void if_icmple(::jbytecode::core::Label* label);
    void if_acmpeq(::jbytecode::core::Label* label);
    void if_acmpne(::jbytecode::core::Label* label);
    void ifnull(::jbytecode::core::Label* label);
    void ifnonnull(::jbytecode::core::Label* label);
    void gotoo(::jbytecode::core::Label* label);
    void jsr(::jbytecode::core::Label* label);
    void ifeq(const std::string& labelName);
    void ifne(const std::string& labelName);
    void iflt(const std::string& labelName);
    void ifge(const std::string& labelName);
    void ifgt(const std::string& labelName);
    void ifle(const std::string& labelName);
    void if_icmpeq(const std::string& labelName);
    void if_icmpne(const std::string& labelName);
    void if_icmplt(const std::string& labelName);
    void if_icmpge(const std::string& labelName);
    void if_icmpgt(const std::string& labelName);
    void if_icmple(const std::string& labelName);
    void if_acmpeq(const std::string& labelName);
    void if_acmpne(const std::string& labelName);
    void ifnull(const std::string& labelName);
    void ifnonnull(const std::string& labelName);
    void gotoo(const std::string& labelName);
    void jsr(const std::string& labelName);
    void ret(int slot);

    void tableswitch(::jbytecode::core::Label* defaultLabel, int min, int max,
                     const std::vector<::jbytecode::core::Label*>& labels);
    void lookupswitch(::jbytecode::core::Label* defaultLabel, const std::vector<int>& keys,
                      const std::vector<::jbytecode::core::Label*>& labels);
    void tableswitch(::jbytecode::tree::LabelNode* defaultLabel, int min, int max,
                     const std::vector<::jbytecode::tree::LabelNode*>& labels);
    void lookupswitch(::jbytecode::tree::LabelNode* defaultLabel, const std::vector<int>& keys,
                      const std::vector<::jbytecode::tree::LabelNode*>& labels);

    //return
    void ireturn();
    void lreturn();
    void freturn();
    void dreturn();
    void areturn();
    void returnn();

    //字段
    void getstatic(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                   const ca::str::Utf8StringRef& desc);
    void putstatic(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                   const ca::str::Utf8StringRef& desc);
    void getfield(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                  const ca::str::Utf8StringRef& desc);
    void putfield(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                  const ca::str::Utf8StringRef& desc);

    //方法调用
    void invokevirtual(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                        const ca::str::Utf8StringRef& desc);
    void invokevirtual(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                        const ca::str::Utf8StringRef& desc, bool isInterface);
    void invokespecial(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                       const ca::str::Utf8StringRef& desc);
    void invokespecial(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                       const ca::str::Utf8StringRef& desc, bool isInterface);
    void invokestatic(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                       const ca::str::Utf8StringRef& desc);
    void invokestatic(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                      const ca::str::Utf8StringRef& desc, bool isInterface);
    void invokeinterface(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                          const ca::str::Utf8StringRef& desc);
    void invokeinterface(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                         const ca::str::Utf8StringRef& desc, bool isInterface);
    void invokedynamic(const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& descriptor,
                       const ::jbytecode::core::Handle& bootstrapMethodHandle,
                       const std::vector<ca::core::Any>& bootstrapMethodArguments);

    //new / array / 异常 / 监控
    void neww(const ca::str::Utf8StringRef& type);
    void newarray(int type);
    void newboolarray();
    void newchararray();
    void newbytearray();
    void newshortarray();
    void newintarray();
    void newlongarray();
    void newfloatarray();
    void newdoublearray();
    void anewarray(const ca::str::Utf8StringRef& type);
    void arraylength();
    void athrow();
    void checkcast(const ca::str::Utf8StringRef& type);
    void instanceoff(const ca::str::Utf8StringRef& type);
    void monitorenter();
    void monitorexit();
    void multiarray(const ca::str::Utf8StringRef& desc, int dims);
    void frame(int type, ca::i32 numLocal, const std::vector<ca::core::Any>& local,
               ca::i32 numStack, const std::vector<ca::core::Any>& stack);

    ///以 legacy 的顺序分配局部变量槽位，不根据 descriptor 展开宽槽。
    void nameArgs(const ca::str::Utf8StringRef& desc, std::initializer_list<std::string> names);
    void adjustArgs(const ca::str::Utf8StringRef& desc);

private:
    garena::Arena* nodeArena_ = nullptr;
    void releaseOwnedNodes() noexcept;
    void releaseOwnedLabels() noexcept;
    ::jbytecode::core::Label* getLabelByName(const std::string& labelName);
    ca::i32                    getSlot(const std::string& varName);

    //扁平收集：节点在收集期保持孤立（prev/next/index 未被改动）。
    //这是对齐 legacy Java 版（List<AbstractInsnNode> addList）的关键：
    //重用 AbstractInsnNode 时不会因侵入式链表导致 prev/next 错乱。
    std::vector<::jbytecode::tree::AbstractInsnNode*> addList_;
    std::unordered_map<std::string, ::jbytecode::core::Label*> namedLabels_;
    std::vector<::jbytecode::core::Label*>                     ownedNamedLabels_;
    std::unordered_map<std::string, ca::i32>                    localVars_;
};

}   //namespace mjt::base
