#include "mbase/asm/insn/insn_builder.hpp"

#include <stdexcept>
#include <unordered_map>

#include "mbase/asm/insn/insn_util.hpp"

namespace mjt::base {

using namespace ::jbytecode::core;
using namespace ::jbytecode::tree;
using Opc = ::jbytecode::core::Opcodes;

namespace {

///断言节点是孤立的（不在任何 InsnList 中）。违例抛异常——这是禁用模式 E
///（见 ir-ownership-design.md §4）的源头防线。
void assertOrphan(const AbstractInsnNode* insn)
{
    if (insn == nullptr) {
        throw std::invalid_argument("InsnBuilder::add: node is null");
    }
    if (insn->index != -1 || insn->previousInsn != nullptr || insn->nextInsn != nullptr) {
        //节点已挂在某个 InsnList 上：直接 add 会破坏原链表，导致 double-free。
        throw std::invalid_argument(
            "InsnBuilder::add: node is already in an InsnList (must be orphan: "
            "index==-1 && !prev && !next). Remove it from its list first.");
    }
}

Label* requireLabel(LabelNode* labelNode)
{
    if (labelNode == nullptr) throw std::invalid_argument("InsnBuilder: label node is null");
    return labelNode->label;
}

std::vector<Label*> requireLabels(const std::vector<LabelNode*>& labelNodes)
{
    std::vector<Label*> labels;
    labels.reserve(labelNodes.size());
    for (auto* labelNode : labelNodes) {
        labels.push_back(requireLabel(labelNode));
    }
    return labels;
}

}   //namespace

//============================================================
//析构 / 收集 / 产出
//============================================================

InsnBuilder::~InsnBuilder()
{
    releaseOwnedNodes();
    releaseOwnedLabels();
}

InsnBuilder::InsnBuilder(InsnBuilder&& other) noexcept
    : addList_(std::move(other.addList_))
    , namedLabels_(std::move(other.namedLabels_))
    , ownedNamedLabels_(std::move(other.ownedNamedLabels_))
    , localVars_(std::move(other.localVars_))
{
    other.addList_.clear();
    other.namedLabels_.clear();
    other.ownedNamedLabels_.clear();
    other.localVars_.clear();
}

InsnBuilder& InsnBuilder::operator=(InsnBuilder&& other) noexcept
{
    if (this == &other) return *this;
    releaseOwnedNodes();
    releaseOwnedLabels();
    addList_ = std::move(other.addList_);
    namedLabels_ = std::move(other.namedLabels_);
    ownedNamedLabels_ = std::move(other.ownedNamedLabels_);
    localVars_ = std::move(other.localVars_);
    other.addList_.clear();
    other.namedLabels_.clear();
    other.ownedNamedLabels_.clear();
    other.localVars_.clear();
    return *this;
}

void InsnBuilder::releaseOwnedNodes() noexcept
{
    //RAII：未 getInsnList 取走的节点由 builder 释放。
    for (auto* n : addList_) {
        delete n;
    }
    addList_.clear();
}

void InsnBuilder::releaseOwnedLabels() noexcept
{
    for (auto* label : ownedNamedLabels_) {
        delete label;
    }
    ownedNamedLabels_.clear();
    namedLabels_.clear();
}

void InsnBuilder::add(AbstractInsnNode* insn)
{
    assertOrphan(insn);
    addList_.push_back(insn);
}

void InsnBuilder::addInsnNode(AbstractInsnNode* insn)
{
    add(insn);
}

void InsnBuilder::addInsnNode(std::initializer_list<AbstractInsnNode*> insns)
{
    for (auto* n : insns) {
        add(n);
    }
}

void InsnBuilder::addInsnNode(const std::vector<AbstractInsnNode*>& insns)
{
    for (auto* n : insns) {
        add(n);
    }
}

void InsnBuilder::addInsnList(InsnList& list)
{
    for (auto* n : list.toArray()) {
        list.remove(n);
        add(n);
    }
}

void InsnBuilder::addInsnList(std::initializer_list<InsnList*> lists)
{
    for (auto* list : lists) {
        if (list == nullptr) throw std::invalid_argument("InsnBuilder::addInsnList: list is null");
        addInsnList(*list);
    }
}

void InsnBuilder::addInsnList(const std::vector<AbstractInsnNode*>& insns)
{
    addInsnNode(insns);
}

void InsnBuilder::deleteLastInsnNode()
{
    if (addList_.empty()) return;
    delete addList_.back();
    addList_.pop_back();
}

InsnList InsnBuilder::getInsnList()
{
    return getInsnList([](const AbstractInsnNode*) { return false; });
}

InsnList InsnBuilder::getInsnList(
    const std::function<bool(const AbstractInsnNode*)>& skipPredicate)
{
    InsnList list;
    std::vector<AbstractInsnNode*> retained;
    retained.reserve(addList_.size());
    for (auto* n : addList_) {
        if (skipPredicate && skipPredicate(n)) {
            retained.push_back(n);
        } else {
            list.add(n);
        }
    }
    const bool transferredNodes = retained.size() != addList_.size();
    addList_                    = std::move(retained);
    if (transferredNodes) {
        //B8 归属制：列表被方法域采纳时 visitor 边界对 builder 堆 Label 拷贝收编
        //（原件无人回收）。为避免 builder 析构时提前删除仍被列表节点引用的命名
        //Label，这里释放 builder 的临时所有权——原件泄漏为暂存模型已知缺口（DEV-14）。
        ownedNamedLabels_.clear();
        namedLabels_.clear();
    }
    return list;
}

InsnList InsnBuilder::build(const std::function<void(InsnBuilder&)>& action)
{
    InsnBuilder builder;
    action(builder);
    return builder.getInsnList();
}

void InsnBuilder::appendTo(InsnList& target, garena::Arena& arena) const
{
    //先 clone LabelNode，保证此前的 Jump/Switch/Line 在第二轮 clone 时能重映射到
    //片段内的新 Label。未由本 builder 携带的 Label 视为外部借用目标：已归属目标
    //arena 的经 resolveCloneLabel 同域直返（identity），外来 Label 建 arena 副本。
    std::unordered_map<Label*, Label*> clonedLabels;
    std::unordered_map<const AbstractInsnNode*, AbstractInsnNode*> clonedLabelNodes;
    for (auto* n : addList_) {
        if (n->getType() == AbstractInsnNode::LABEL) {
            clonedLabelNodes.emplace(n, n->clone(arena, clonedLabels));
        }
    }
    for (auto* n : addList_) {
        auto clonedLabelNode = clonedLabelNodes.find(n);
        target.add(clonedLabelNode != clonedLabelNodes.end()
                       ? clonedLabelNode->second
                       : n->clone(arena, clonedLabels));
    }
}

//============================================================
//指令工厂方法
//============================================================

void InsnBuilder::insn(int opcode)
{
    add(new InsnNode(opcode));
}

void InsnBuilder::nop()
{
    insn(Opc::NOP);
}
void InsnBuilder::aconst_null()
{
    insn(Opc::ACONST_NULL);
}
void InsnBuilder::iconst_m1()
{
    insn(Opc::ICONST_M1);
}
void InsnBuilder::iconst_0()
{
    insn(Opc::ICONST_0);
}
void InsnBuilder::iconst_1()
{
    insn(Opc::ICONST_1);
}
void InsnBuilder::iconst_2()
{
    insn(Opc::ICONST_2);
}
void InsnBuilder::iconst_3()
{
    insn(Opc::ICONST_3);
}
void InsnBuilder::iconst_4()
{
    insn(Opc::ICONST_4);
}
void InsnBuilder::iconst_5()
{
    insn(Opc::ICONST_5);
}
void InsnBuilder::lconst_0()
{
    insn(Opc::LCONST_0);
}
void InsnBuilder::lconst_1()
{
    insn(Opc::LCONST_1);
}
void InsnBuilder::fconst_0()
{
    insn(Opc::FCONST_0);
}
void InsnBuilder::fconst_1()
{
    insn(Opc::FCONST_1);
}
void InsnBuilder::fconst_2()
{
    insn(Opc::FCONST_2);
}
void InsnBuilder::dconst_0()
{
    insn(Opc::DCONST_0);
}
void InsnBuilder::dconst_1()
{
    insn(Opc::DCONST_1);
}

void InsnBuilder::bipush(int value)
{
    add(new IntInsnNode(Opc::BIPUSH, value));
}
void InsnBuilder::sipush(int value)
{
    add(new IntInsnNode(Opc::SIPUSH, value));
}

//ldc：对齐 legacy，按值范围/类型选最短指令序列。
void InsnBuilder::ldc(int value)
{
    add(nodeArena_ ? InsnUtil::getIntInsn(*nodeArena_, value) : InsnUtil::getIntInsn(value));
}
void InsnBuilder::ldc(long long value)
{
    add(nodeArena_ ? InsnUtil::getLongInsn(*nodeArena_, value) : InsnUtil::getLongInsn(value));
}
void InsnBuilder::ldc(float value)
{
    add(nodeArena_ ? InsnUtil::getFloatInsn(*nodeArena_, value) : InsnUtil::getFloatInsn(value));
}
void InsnBuilder::ldc(double value)
{
    add(nodeArena_ ? InsnUtil::getDoubleInsn(*nodeArena_, value) : InsnUtil::getDoubleInsn(value));
}
void InsnBuilder::ldc(const std::string& value)
{
    add(new LdcInsnNode(value));
}
void InsnBuilder::ldc(const Type& value)
{
    add(new LdcInsnNode(value));
}
void InsnBuilder::ldc(const Handle& value)
{
    add(new LdcInsnNode(ca::core::Any(value)));
}

//load
void InsnBuilder::iload(int slot)
{
    add(new VarInsnNode(Opc::ILOAD, slot));
}
void InsnBuilder::lload(int slot)
{
    add(new VarInsnNode(Opc::LLOAD, slot));
}
void InsnBuilder::fload(int slot)
{
    add(new VarInsnNode(Opc::FLOAD, slot));
}
void InsnBuilder::dload(int slot)
{
    add(new VarInsnNode(Opc::DLOAD, slot));
}
void InsnBuilder::aload(int slot)
{
    add(new VarInsnNode(Opc::ALOAD, slot));
}
void InsnBuilder::iload(const std::string& varName)
{
    iload(getSlot(varName));
}
void InsnBuilder::lload(const std::string& varName)
{
    lload(getSlot(varName));
}
void InsnBuilder::fload(const std::string& varName)
{
    fload(getSlot(varName));
}
void InsnBuilder::dload(const std::string& varName)
{
    dload(getSlot(varName));
}
void InsnBuilder::aload(const std::string& varName)
{
    aload(getSlot(varName));
}

//array load
void InsnBuilder::iaload()
{
    insn(Opc::IALOAD);
}
void InsnBuilder::laload()
{
    insn(Opc::LALOAD);
}
void InsnBuilder::faload()
{
    insn(Opc::FALOAD);
}
void InsnBuilder::daload()
{
    insn(Opc::DALOAD);
}
void InsnBuilder::aaload()
{
    insn(Opc::AALOAD);
}
void InsnBuilder::baload()
{
    insn(Opc::BALOAD);
}
void InsnBuilder::caload()
{
    insn(Opc::CALOAD);
}
void InsnBuilder::saload()
{
    insn(Opc::SALOAD);
}

//store
void InsnBuilder::istore(int slot)
{
    add(new VarInsnNode(Opc::ISTORE, slot));
}
void InsnBuilder::lstore(int slot)
{
    add(new VarInsnNode(Opc::LSTORE, slot));
}
void InsnBuilder::fstore(int slot)
{
    add(new VarInsnNode(Opc::FSTORE, slot));
}
void InsnBuilder::dstore(int slot)
{
    add(new VarInsnNode(Opc::DSTORE, slot));
}
void InsnBuilder::astore(int slot)
{
    add(new VarInsnNode(Opc::ASTORE, slot));
}
void InsnBuilder::istore(const std::string& varName)
{
    istore(getSlot(varName));
}
void InsnBuilder::lstore(const std::string& varName)
{
    lstore(getSlot(varName));
}
void InsnBuilder::fstore(const std::string& varName)
{
    fstore(getSlot(varName));
}
void InsnBuilder::dstore(const std::string& varName)
{
    dstore(getSlot(varName));
}
void InsnBuilder::astore(const std::string& varName)
{
    astore(getSlot(varName));
}

//array store
void InsnBuilder::iastore()
{
    insn(Opc::IASTORE);
}
void InsnBuilder::lastore()
{
    insn(Opc::LASTORE);
}
void InsnBuilder::fastore()
{
    insn(Opc::FASTORE);
}
void InsnBuilder::dastore()
{
    insn(Opc::DASTORE);
}
void InsnBuilder::aastore()
{
    insn(Opc::AASTORE);
}
void InsnBuilder::bastore()
{
    insn(Opc::BASTORE);
}
void InsnBuilder::castore()
{
    insn(Opc::CASTORE);
}
void InsnBuilder::sastore()
{
    insn(Opc::SASTORE);
}

//pop / dup / swap
void InsnBuilder::pop()
{
    insn(Opc::POP);
}
void InsnBuilder::pop2()
{
    insn(Opc::POP2);
}
void InsnBuilder::dup()
{
    insn(Opc::DUP);
}
void InsnBuilder::dup_x1()
{
    insn(Opc::DUP_X1);
}
void InsnBuilder::dup_x2()
{
    insn(Opc::DUP_X2);
}
void InsnBuilder::dup2()
{
    insn(Opc::DUP2);
}
void InsnBuilder::dup2_x1()
{
    insn(Opc::DUP2_X1);
}
void InsnBuilder::dup2_x2()
{
    insn(Opc::DUP2_X2);
}
void InsnBuilder::swap()
{
    insn(Opc::SWAP);
}

//算术
void InsnBuilder::iadd()
{
    insn(Opc::IADD);
}
void InsnBuilder::ladd()
{
    insn(Opc::LADD);
}
void InsnBuilder::fadd()
{
    insn(Opc::FADD);
}
void InsnBuilder::dadd()
{
    insn(Opc::DADD);
}
void InsnBuilder::isub()
{
    insn(Opc::ISUB);
}
void InsnBuilder::lsub()
{
    insn(Opc::LSUB);
}
void InsnBuilder::fsub()
{
    insn(Opc::FSUB);
}
void InsnBuilder::dsub()
{
    insn(Opc::DSUB);
}
void InsnBuilder::imul()
{
    insn(Opc::IMUL);
}
void InsnBuilder::lmul()
{
    insn(Opc::LMUL);
}
void InsnBuilder::fmul()
{
    insn(Opc::FMUL);
}
void InsnBuilder::dmul()
{
    insn(Opc::DMUL);
}
void InsnBuilder::idiv()
{
    insn(Opc::IDIV);
}
void InsnBuilder::ldiv()
{
    insn(Opc::LDIV);
}
void InsnBuilder::fdiv()
{
    insn(Opc::FDIV);
}
void InsnBuilder::ddiv()
{
    insn(Opc::DDIV);
}
void InsnBuilder::irem()
{
    insn(Opc::IREM);
}
void InsnBuilder::lrem()
{
    insn(Opc::LREM);
}
void InsnBuilder::frem()
{
    insn(Opc::FREM);
}
void InsnBuilder::drem()
{
    insn(Opc::DREM);
}
void InsnBuilder::ineg()
{
    insn(Opc::INEG);
}
void InsnBuilder::lneg()
{
    insn(Opc::LNEG);
}
void InsnBuilder::fneg()
{
    insn(Opc::FNEG);
}
void InsnBuilder::dneg()
{
    insn(Opc::DNEG);
}

//移位 / 逻辑
void InsnBuilder::ishl()
{
    insn(Opc::ISHL);
}
void InsnBuilder::lshl()
{
    insn(Opc::LSHL);
}
void InsnBuilder::ishr()
{
    insn(Opc::ISHR);
}
void InsnBuilder::lshr()
{
    insn(Opc::LSHR);
}
void InsnBuilder::iushr()
{
    insn(Opc::IUSHR);
}
void InsnBuilder::lushr()
{
    insn(Opc::LUSHR);
}
void InsnBuilder::iand()
{
    insn(Opc::IAND);
}
void InsnBuilder::land()
{
    insn(Opc::LAND);
}
void InsnBuilder::ior()
{
    insn(Opc::IOR);
}
void InsnBuilder::lor()
{
    insn(Opc::LOR);
}
void InsnBuilder::ixor()
{
    insn(Opc::IXOR);
}
void InsnBuilder::lxor()
{
    insn(Opc::LXOR);
}

void InsnBuilder::iinc(int slot, int incr)
{
    add(new IincInsnNode(slot, incr));
}
void InsnBuilder::iinc(const std::string& varName, int incr)
{
    iinc(getSlot(varName), incr);
}

//类型转换
void InsnBuilder::i2l()
{
    insn(Opc::I2L);
}
void InsnBuilder::i2f()
{
    insn(Opc::I2F);
}
void InsnBuilder::i2d()
{
    insn(Opc::I2D);
}
void InsnBuilder::l2i()
{
    insn(Opc::L2I);
}
void InsnBuilder::l2f()
{
    insn(Opc::L2F);
}
void InsnBuilder::l2d()
{
    insn(Opc::L2D);
}
void InsnBuilder::f2i()
{
    insn(Opc::F2I);
}
void InsnBuilder::f2l()
{
    insn(Opc::F2L);
}
void InsnBuilder::f2d()
{
    insn(Opc::F2D);
}
void InsnBuilder::d2i()
{
    insn(Opc::D2I);
}
void InsnBuilder::d2l()
{
    insn(Opc::D2L);
}
void InsnBuilder::d2f()
{
    insn(Opc::D2F);
}
void InsnBuilder::i2b()
{
    insn(Opc::I2B);
}
void InsnBuilder::i2c()
{
    insn(Opc::I2C);
}
void InsnBuilder::i2s()
{
    insn(Opc::I2S);
}

//比较
void InsnBuilder::lcmp()
{
    insn(Opc::LCMP);
}
void InsnBuilder::fcmpl()
{
    insn(Opc::FCMPL);
}
void InsnBuilder::fcmpg()
{
    insn(Opc::FCMPG);
}
void InsnBuilder::dcmpl()
{
    insn(Opc::DCMPL);
}
void InsnBuilder::dcmpg()
{
    insn(Opc::DCMPG);
}

//label / line
void InsnBuilder::label(LabelNode* labelNode)
{
    add(labelNode);
}
void InsnBuilder::label(Label* label)
{
    add(new LabelNode(label));
}
void InsnBuilder::label(const std::string& labelName)
{
    label(getLabelByName(labelName));
}
void InsnBuilder::line(int num, Label* start)
{
    add(new LineNumberNode(num, start));
}
void InsnBuilder::line(int num, LabelNode* startNode)
{
    add(new LineNumberNode(num, startNode->label));
}
void InsnBuilder::line(int num, const std::string& labelName)
{
    line(num, getLabelByName(labelName));
}

//控制流
void InsnBuilder::ifeq(LabelNode* label)
{
    ifeq(requireLabel(label));
}
void InsnBuilder::ifne(LabelNode* label)
{
    ifne(requireLabel(label));
}
void InsnBuilder::iflt(LabelNode* label)
{
    iflt(requireLabel(label));
}
void InsnBuilder::ifge(LabelNode* label)
{
    ifge(requireLabel(label));
}
void InsnBuilder::ifgt(LabelNode* label)
{
    ifgt(requireLabel(label));
}
void InsnBuilder::ifle(LabelNode* label)
{
    ifle(requireLabel(label));
}
void InsnBuilder::if_icmpeq(LabelNode* label)
{
    if_icmpeq(requireLabel(label));
}
void InsnBuilder::if_icmpne(LabelNode* label)
{
    if_icmpne(requireLabel(label));
}
void InsnBuilder::if_icmplt(LabelNode* label)
{
    if_icmplt(requireLabel(label));
}
void InsnBuilder::if_icmpge(LabelNode* label)
{
    if_icmpge(requireLabel(label));
}
void InsnBuilder::if_icmpgt(LabelNode* label)
{
    if_icmpgt(requireLabel(label));
}
void InsnBuilder::if_icmple(LabelNode* label)
{
    if_icmple(requireLabel(label));
}
void InsnBuilder::if_acmpeq(LabelNode* label)
{
    if_acmpeq(requireLabel(label));
}
void InsnBuilder::if_acmpne(LabelNode* label)
{
    if_acmpne(requireLabel(label));
}
void InsnBuilder::ifnull(LabelNode* label)
{
    ifnull(requireLabel(label));
}
void InsnBuilder::ifnonnull(LabelNode* label)
{
    ifnonnull(requireLabel(label));
}
void InsnBuilder::gotoo(LabelNode* label)
{
    gotoo(requireLabel(label));
}
void InsnBuilder::jsr(LabelNode* label)
{
    jsr(requireLabel(label));
}
void InsnBuilder::ifeq(Label* label)
{
    add(new JumpInsnNode(Opc::IFEQ, label));
}
void InsnBuilder::ifne(Label* label)
{
    add(new JumpInsnNode(Opc::IFNE, label));
}
void InsnBuilder::iflt(Label* label)
{
    add(new JumpInsnNode(Opc::IFLT, label));
}
void InsnBuilder::ifge(Label* label)
{
    add(new JumpInsnNode(Opc::IFGE, label));
}
void InsnBuilder::ifgt(Label* label)
{
    add(new JumpInsnNode(Opc::IFGT, label));
}
void InsnBuilder::ifle(Label* label)
{
    add(new JumpInsnNode(Opc::IFLE, label));
}
void InsnBuilder::if_icmpeq(Label* label)
{
    add(new JumpInsnNode(Opc::IF_ICMPEQ, label));
}
void InsnBuilder::if_icmpne(Label* label)
{
    add(new JumpInsnNode(Opc::IF_ICMPNE, label));
}
void InsnBuilder::if_icmplt(Label* label)
{
    add(new JumpInsnNode(Opc::IF_ICMPLT, label));
}
void InsnBuilder::if_icmpge(Label* label)
{
    add(new JumpInsnNode(Opc::IF_ICMPGE, label));
}
void InsnBuilder::if_icmpgt(Label* label)
{
    add(new JumpInsnNode(Opc::IF_ICMPGT, label));
}
void InsnBuilder::if_icmple(Label* label)
{
    add(new JumpInsnNode(Opc::IF_ICMPLE, label));
}
void InsnBuilder::if_acmpeq(Label* label)
{
    add(new JumpInsnNode(Opc::IF_ACMPEQ, label));
}
void InsnBuilder::if_acmpne(Label* label)
{
    add(new JumpInsnNode(Opc::IF_ACMPNE, label));
}
void InsnBuilder::ifnull(Label* label)
{
    add(new JumpInsnNode(Opc::IFNULL, label));
}
void InsnBuilder::ifnonnull(Label* label)
{
    add(new JumpInsnNode(Opc::IFNONNULL, label));
}
void InsnBuilder::gotoo(Label* label)
{
    add(new JumpInsnNode(Opc::GOTO, label));
}
void InsnBuilder::jsr(Label* label)
{
    add(new JumpInsnNode(Opc::JSR, label));
}

void InsnBuilder::ifeq(const std::string& labelName)
{
    ifeq(getLabelByName(labelName));
}
void InsnBuilder::ifne(const std::string& labelName)
{
    ifne(getLabelByName(labelName));
}
void InsnBuilder::iflt(const std::string& labelName)
{
    iflt(getLabelByName(labelName));
}
void InsnBuilder::ifge(const std::string& labelName)
{
    ifge(getLabelByName(labelName));
}
void InsnBuilder::ifgt(const std::string& labelName)
{
    ifgt(getLabelByName(labelName));
}
void InsnBuilder::ifle(const std::string& labelName)
{
    ifle(getLabelByName(labelName));
}
void InsnBuilder::if_icmpeq(const std::string& labelName)
{
    if_icmpeq(getLabelByName(labelName));
}
void InsnBuilder::if_icmpne(const std::string& labelName)
{
    if_icmpne(getLabelByName(labelName));
}
void InsnBuilder::if_icmplt(const std::string& labelName)
{
    if_icmplt(getLabelByName(labelName));
}
void InsnBuilder::if_icmpge(const std::string& labelName)
{
    if_icmpge(getLabelByName(labelName));
}
void InsnBuilder::if_icmpgt(const std::string& labelName)
{
    if_icmpgt(getLabelByName(labelName));
}
void InsnBuilder::if_icmple(const std::string& labelName)
{
    if_icmple(getLabelByName(labelName));
}
void InsnBuilder::if_acmpeq(const std::string& labelName)
{
    if_acmpeq(getLabelByName(labelName));
}
void InsnBuilder::if_acmpne(const std::string& labelName)
{
    if_acmpne(getLabelByName(labelName));
}
void InsnBuilder::ifnull(const std::string& labelName)
{
    ifnull(getLabelByName(labelName));
}
void InsnBuilder::ifnonnull(const std::string& labelName)
{
    ifnonnull(getLabelByName(labelName));
}
void InsnBuilder::gotoo(const std::string& labelName)
{
    gotoo(getLabelByName(labelName));
}
void InsnBuilder::jsr(const std::string& labelName)
{
    jsr(getLabelByName(labelName));
}
void InsnBuilder::ret(int slot)
{
    add(new VarInsnNode(Opc::RET, slot));
}

void InsnBuilder::tableswitch(Label* defaultLabel, int min, int max,
                              const std::vector<Label*>& labels)
{
    add(new TableSwitchInsnNode(min, max, defaultLabel, labels));
}
void InsnBuilder::lookupswitch(Label* defaultLabel, const std::vector<int>& keys,
                               const std::vector<Label*>& labels)
{
    add(new LookupSwitchInsnNode(defaultLabel, keys, labels));
}
void InsnBuilder::tableswitch(LabelNode* defaultLabel, int min, int max,
                               const std::vector<LabelNode*>& labels)
{
    tableswitch(requireLabel(defaultLabel), min, max, requireLabels(labels));
}
void InsnBuilder::lookupswitch(LabelNode* defaultLabel, const std::vector<int>& keys,
                                const std::vector<LabelNode*>& labels)
{
    lookupswitch(requireLabel(defaultLabel), keys, requireLabels(labels));
}

//return
void InsnBuilder::ireturn()
{
    add(new InsnNode(Opc::IRETURN));
}
void InsnBuilder::lreturn()
{
    add(new InsnNode(Opc::LRETURN));
}
void InsnBuilder::freturn()
{
    add(new InsnNode(Opc::FRETURN));
}
void InsnBuilder::dreturn()
{
    add(new InsnNode(Opc::DRETURN));
}
void InsnBuilder::areturn()
{
    add(new InsnNode(Opc::ARETURN));
}
void InsnBuilder::returnn()
{
    add(new InsnNode(Opc::RETURN));
}

//字段
void InsnBuilder::getstatic(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                            const ca::str::Utf8StringRef& desc)
{
    add(new FieldInsnNode(Opc::GETSTATIC, owner, name, desc));
}
void InsnBuilder::putstatic(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                            const ca::str::Utf8StringRef& desc)
{
    add(new FieldInsnNode(Opc::PUTSTATIC, owner, name, desc));
}
void InsnBuilder::getfield(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                           const ca::str::Utf8StringRef& desc)
{
    add(new FieldInsnNode(Opc::GETFIELD, owner, name, desc));
}
void InsnBuilder::putfield(const ca::str::Utf8StringRef& owner, const ca::str::Utf8StringRef& name,
                           const ca::str::Utf8StringRef& desc)
{
    add(new FieldInsnNode(Opc::PUTFIELD, owner, name, desc));
}

//方法调用
void InsnBuilder::invokevirtual(const ca::str::Utf8StringRef& owner,
                                 const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc)
{
    invokevirtual(owner, name, desc, false);
}
void InsnBuilder::invokevirtual(const ca::str::Utf8StringRef& owner,
                                 const ca::str::Utf8StringRef& name,
                                 const ca::str::Utf8StringRef& desc, bool isInterface)
{
    add(new MethodInsnNode(Opc::INVOKEVIRTUAL, owner, name, desc, isInterface));
}
void InsnBuilder::invokespecial(const ca::str::Utf8StringRef& owner,
                                 const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc)
{
    invokespecial(owner, name, desc, false);
}
void InsnBuilder::invokespecial(const ca::str::Utf8StringRef& owner,
                                 const ca::str::Utf8StringRef& name,
                                 const ca::str::Utf8StringRef& desc, bool isInterface)
{
    add(new MethodInsnNode(Opc::INVOKESPECIAL, owner, name, desc, isInterface));
}
void InsnBuilder::invokestatic(const ca::str::Utf8StringRef& owner,
                                const ca::str::Utf8StringRef& name, const ca::str::Utf8StringRef& desc)
{
    invokestatic(owner, name, desc, false);
}
void InsnBuilder::invokestatic(const ca::str::Utf8StringRef& owner,
                                const ca::str::Utf8StringRef& name,
                                const ca::str::Utf8StringRef& desc, bool isInterface)
{
    add(new MethodInsnNode(Opc::INVOKESTATIC, owner, name, desc, isInterface));
}
void InsnBuilder::invokeinterface(const ca::str::Utf8StringRef& owner,
                                   const ca::str::Utf8StringRef& name,
                                   const ca::str::Utf8StringRef& desc)
{
    invokeinterface(owner, name, desc, true);
}
void InsnBuilder::invokeinterface(const ca::str::Utf8StringRef& owner,
                                   const ca::str::Utf8StringRef& name,
                                   const ca::str::Utf8StringRef& desc, bool isInterface)
{
    add(new MethodInsnNode(Opc::INVOKEINTERFACE, owner, name, desc, isInterface));
}
void InsnBuilder::invokedynamic(const ca::str::Utf8StringRef& name,
                                const ca::str::Utf8StringRef& descriptor,
                                const Handle& bootstrapMethodHandle,
                                const std::vector<ca::core::Any>& bootstrapMethodArguments)
{
    add(new InvokeDynamicInsnNode(name, descriptor, bootstrapMethodHandle, bootstrapMethodArguments));
}

//new / array / 异常 / 监控
void InsnBuilder::neww(const ca::str::Utf8StringRef& type)
{
    add(new TypeInsnNode(Opc::NEW, type));
}
void InsnBuilder::newarray(int type)
{
    add(new IntInsnNode(Opc::NEWARRAY, type));
}
void InsnBuilder::newboolarray()
{
    newarray(Opc::T_BOOLEAN);
}
void InsnBuilder::newchararray()
{
    newarray(Opc::T_CHAR);
}
void InsnBuilder::newbytearray()
{
    newarray(Opc::T_BYTE);
}
void InsnBuilder::newshortarray()
{
    newarray(Opc::T_SHORT);
}
void InsnBuilder::newintarray()
{
    newarray(Opc::T_INT);
}
void InsnBuilder::newlongarray()
{
    newarray(Opc::T_LONG);
}
void InsnBuilder::newfloatarray()
{
    newarray(Opc::T_FLOAT);
}
void InsnBuilder::newdoublearray()
{
    newarray(Opc::T_DOUBLE);
}
void InsnBuilder::anewarray(const ca::str::Utf8StringRef& type)
{
    add(new TypeInsnNode(Opc::ANEWARRAY, type));
}
void InsnBuilder::arraylength()
{
    insn(Opc::ARRAYLENGTH);
}
void InsnBuilder::athrow()
{
    insn(Opc::ATHROW);
}
void InsnBuilder::checkcast(const ca::str::Utf8StringRef& type)
{
    add(new TypeInsnNode(Opc::CHECKCAST, type));
}
void InsnBuilder::instanceoff(const ca::str::Utf8StringRef& type)
{
    add(new TypeInsnNode(Opc::INSTANCEOF, type));
}
void InsnBuilder::monitorenter()
{
    insn(Opc::MONITORENTER);
}
void InsnBuilder::monitorexit()
{
    insn(Opc::MONITOREXIT);
}
void InsnBuilder::multiarray(const ca::str::Utf8StringRef& desc, int dims)
{
    add(new MultiANewArrayInsnNode(desc, dims));
}
void InsnBuilder::frame(int type, ca::i32 numLocal, const std::vector<ca::core::Any>& local,
                        ca::i32 numStack, const std::vector<ca::core::Any>& stack)
{
    if (numLocal != static_cast<ca::i32>(local.size()) ||
        numStack != static_cast<ca::i32>(stack.size())) {
        throw std::invalid_argument("InsnBuilder::frame: count does not match frame entries");
    }
    add(new FrameNode(type, local, stack));
}

void InsnBuilder::nameArgs(const ca::str::Utf8StringRef& desc,
                           std::initializer_list<std::string> names)
{
    if (Type::getArgumentTypes(desc).size() != names.size()) return;
    for (const auto& name : names) {
        getSlot(name);
    }
}

void InsnBuilder::adjustArgs(const ca::str::Utf8StringRef& desc)
{
    const auto argumentCount = Type::getArgumentTypes(desc).size();
    for (size_t i = 0; i < argumentCount; ++i) {
        getSlot("arg" + std::to_string(i + 1));
    }
}

Label* InsnBuilder::getLabelByName(const std::string& labelName)
{
    auto it = namedLabels_.find(labelName);
    if (it != namedLabels_.end()) return it->second;

    auto* label = new Label();
    namedLabels_.emplace(labelName, label);
    ownedNamedLabels_.push_back(label);
    return label;
}

ca::i32 InsnBuilder::getSlot(const std::string& varName)
{
    auto it = localVars_.find(varName);
    if (it != localVars_.end()) return it->second;

    const auto slot = static_cast<ca::i32>(localVars_.size());
    localVars_.emplace(varName, slot);
    return slot;
}

}   //namespace mjt::base
