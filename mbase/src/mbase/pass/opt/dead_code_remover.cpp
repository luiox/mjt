#include "mbase/pass/opt/dead_code_remover.hpp"

#include "mbase/spi/class_resource_pending_commit.hpp"

#include "mbase/analysis/method_effect_analysis.hpp"
#include "mbase/analysis/method_frame_analysis.hpp"
#include "mbase/asm/analyzer/original_source_interpreter.hpp"
#include "mbase/asm/analyzer/super_interpreter.hpp"
#include "mbase/asm/insn/class_util.hpp"
#include "mbase/asm/insn/insn_builder.hpp"
#include "mbase/asm/insn/insn_util.hpp"
#include "jbytecode/analysis/analyzer.hpp"
#include "jbytecode/analysis/basic_interpreter.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/core/type.hpp"
#include <algorithm>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mjt::base {

using namespace jbytecode::core;
using namespace jbytecode::tree;
using namespace jbytecode::analysis;

//============================================================================
//dead_code_remover 私有实现（producer/consumer 图、stack expansion plan 等）。
//原本拆在 dead_code_detail.hpp，因是单消费者实现细节已合回本 TU；全部位于匿名命名空间。
//如果未来这些类型需被其他 pass 复用，请抽到独立家族 detail，而不是让本节变成跨 pass 共享。
//============================================================================

namespace {

bool isIntConstant(AbstractInsnNode* insn)
{
    int value = 0;
    return InsnUtil::getIntValue(insn, value);
}

bool isLongConstant(AbstractInsnNode* insn)
{
    ca::i64 value = 0;
    return InsnUtil::getLongValue(insn, value);
}

bool isDoubleConstant(AbstractInsnNode* insn)
{
    double value = 0.0;
    return InsnUtil::getDoubleValue(insn, value);
}

bool isStringConstant(AbstractInsnNode* insn)
{
    std::string value;
    return InsnUtil::getStringValue(insn, value);
}

void addCloned(InsnList& target, AbstractInsnNode* source, garena::Arena& arena,
               std::unordered_map<Label*, Label*>& clonedLabels)
{
    //B8 新 clone 协议：产物在 arena 上分配（消费方 run 传入目标方法的 arena）。
    target.add(source->clone(arena, clonedLabels));
}

bool isRedundantPopConstant(AbstractInsnNode* insn)
{
    return isIntConstant(insn) || isLongConstant(insn) || isStringConstant(insn);
}

bool isRedundantPop2Constant(AbstractInsnNode* insn)
{
    return isLongConstant(insn) || isDoubleConstant(insn);
}

bool loadSlot(AbstractInsnNode* insn, int& slot)
{
    if (!insn) return false;
    const int op = insn->getOpcode();
    if (op >= Opcodes::ILOAD && op <= Opcodes::ALOAD) {
        slot = ca::core::tag_cast::cast<VarInsnNode*>(insn)->var;
        return true;
    }
    if (op >= jbytecode::core::Constants::ILOAD_0 && op <= jbytecode::core::Constants::ILOAD_3) {
        slot = op - jbytecode::core::Constants::ILOAD_0;
        return true;
    }
    if (op >= jbytecode::core::Constants::LLOAD_0 && op <= jbytecode::core::Constants::LLOAD_3) {
        slot = op - jbytecode::core::Constants::LLOAD_0;
        return true;
    }
    if (op >= jbytecode::core::Constants::FLOAD_0 && op <= jbytecode::core::Constants::FLOAD_3) {
        slot = op - jbytecode::core::Constants::FLOAD_0;
        return true;
    }
    if (op >= jbytecode::core::Constants::DLOAD_0 && op <= jbytecode::core::Constants::DLOAD_3) {
        slot = op - jbytecode::core::Constants::DLOAD_0;
        return true;
    }
    if (op >= jbytecode::core::Constants::ALOAD_0 && op <= jbytecode::core::Constants::ALOAD_3) {
        slot = op - jbytecode::core::Constants::ALOAD_0;
        return true;
    }
    return false;
}

bool storeSlot(AbstractInsnNode* insn, int& slot, bool& wide)
{
    if (!insn) return false;
    wide         = false;
    const int op = insn->getOpcode();
    if (op >= Opcodes::ISTORE && op <= Opcodes::ASTORE) {
        slot = ca::core::tag_cast::cast<VarInsnNode*>(insn)->var;
        wide = op == Opcodes::LSTORE || op == Opcodes::DSTORE;
        return true;
    }
    if (op >= jbytecode::core::Constants::ISTORE_0 && op <= jbytecode::core::Constants::ISTORE_3) {
        slot = op - jbytecode::core::Constants::ISTORE_0;
        return true;
    }
    if (op >= jbytecode::core::Constants::LSTORE_0 && op <= jbytecode::core::Constants::LSTORE_3) {
        slot = op - jbytecode::core::Constants::LSTORE_0;
        wide = true;
        return true;
    }
    if (op >= jbytecode::core::Constants::FSTORE_0 && op <= jbytecode::core::Constants::FSTORE_3) {
        slot = op - jbytecode::core::Constants::FSTORE_0;
        return true;
    }
    if (op >= jbytecode::core::Constants::DSTORE_0 && op <= jbytecode::core::Constants::DSTORE_3) {
        slot = op - jbytecode::core::Constants::DSTORE_0;
        wide = true;
        return true;
    }
    if (op >= jbytecode::core::Constants::ASTORE_0 && op <= jbytecode::core::Constants::ASTORE_3) {
        slot = op - jbytecode::core::Constants::ASTORE_0;
        return true;
    }
    return false;
}

//OriginalSourceFrame 别名现由 mbase/analysis/method_frame_analysis.hpp 规范提供。
using ConsumerMap =
    std::unordered_map<const AbstractInsnNode*, std::unordered_set<const AbstractInsnNode*>>;
using FrameByInsn = std::unordered_map<const AbstractInsnNode*, OriginalSourceFrame*>;

bool isSingleVarLoadInsn(AbstractInsnNode* insn, int& slot)
{
    if (!loadSlot(insn, slot)) return false;
    const int op = insn->getOpcode();
    return (op >= Opcodes::ILOAD && op <= Opcodes::ALOAD) ||
           (op >= jbytecode::core::Constants::ILOAD_0 && op <= jbytecode::core::Constants::ALOAD_3);
}

bool isSingleVarStoreInsn(AbstractInsnNode* insn, int& slot, bool& wide)
{
    if (!storeSlot(insn, slot, wide)) return false;
    const int op = insn->getOpcode();
    return (op >= Opcodes::ISTORE && op <= Opcodes::ASTORE) ||
           (op >= jbytecode::core::Constants::ISTORE_0 &&
            op <= jbytecode::core::Constants::ASTORE_3);
}

int argumentValueCount(ca::str::Utf8StringRef desc)
{
    return static_cast<int>(jbytecode::core::Type::getMethodType(desc).getArgumentTypes().size());
}

int consumedValueCount(AbstractInsnNode* insn, const OriginalSourceFrame* frame)
{
    if (!insn) return 0;
    const int op = insn->getOpcode();
    if (op < 0) return 0;

    int  slot = -1;
    bool wide = false;
    if (isSingleVarStoreInsn(insn, slot, wide)) return 1;

    switch (op) {
    case Opcodes::POP: return 1;
    case Opcodes::POP2:
        if (!frame || frame->getStackSize() == 0) return 2;
        return frame->getStack(frame->getStackSize() - 1).getSize() == 2 ? 1 : 2;
    case Opcodes::DUP:
    case Opcodes::DUP2: return 1;
    case Opcodes::DUP_X1:
    case Opcodes::DUP2_X1:
    case Opcodes::SWAP: return 2;
    case Opcodes::DUP_X2:
    case Opcodes::DUP2_X2: return 3;
    case Opcodes::IRETURN:
    case Opcodes::LRETURN:
    case Opcodes::FRETURN:
    case Opcodes::DRETURN:
    case Opcodes::ARETURN:
    case Opcodes::ATHROW:
    case Opcodes::MONITORENTER:
    case Opcodes::MONITOREXIT:
    case Opcodes::TABLESWITCH:
    case Opcodes::LOOKUPSWITCH: return 1;
    case Opcodes::RETURN: return 0;
    case Opcodes::IFEQ:
    case Opcodes::IFNE:
    case Opcodes::IFLT:
    case Opcodes::IFGE:
    case Opcodes::IFGT:
    case Opcodes::IFLE:
    case Opcodes::IFNULL:
    case Opcodes::IFNONNULL:
    case Opcodes::INEG:
    case Opcodes::LNEG:
    case Opcodes::FNEG:
    case Opcodes::DNEG:
    case Opcodes::I2L:
    case Opcodes::I2F:
    case Opcodes::I2D:
    case Opcodes::L2I:
    case Opcodes::L2F:
    case Opcodes::L2D:
    case Opcodes::F2I:
    case Opcodes::F2L:
    case Opcodes::F2D:
    case Opcodes::D2I:
    case Opcodes::D2L:
    case Opcodes::D2F:
    case Opcodes::I2B:
    case Opcodes::I2C:
    case Opcodes::I2S:
    case Opcodes::GETFIELD:
    case Opcodes::ARRAYLENGTH:
    case Opcodes::CHECKCAST:
    case Opcodes::INSTANCEOF: return 1;
    case Opcodes::IF_ICMPEQ:
    case Opcodes::IF_ICMPNE:
    case Opcodes::IF_ICMPLT:
    case Opcodes::IF_ICMPGE:
    case Opcodes::IF_ICMPGT:
    case Opcodes::IF_ICMPLE:
    case Opcodes::IF_ACMPEQ:
    case Opcodes::IF_ACMPNE:
    case Opcodes::IALOAD:
    case Opcodes::LALOAD:
    case Opcodes::FALOAD:
    case Opcodes::DALOAD:
    case Opcodes::AALOAD:
    case Opcodes::BALOAD:
    case Opcodes::CALOAD:
    case Opcodes::SALOAD:
    case Opcodes::IADD:
    case Opcodes::LADD:
    case Opcodes::FADD:
    case Opcodes::DADD:
    case Opcodes::ISUB:
    case Opcodes::LSUB:
    case Opcodes::FSUB:
    case Opcodes::DSUB:
    case Opcodes::IMUL:
    case Opcodes::LMUL:
    case Opcodes::FMUL:
    case Opcodes::DMUL:
    case Opcodes::IDIV:
    case Opcodes::LDIV:
    case Opcodes::FDIV:
    case Opcodes::DDIV:
    case Opcodes::IREM:
    case Opcodes::LREM:
    case Opcodes::FREM:
    case Opcodes::DREM:
    case Opcodes::ISHL:
    case Opcodes::LSHL:
    case Opcodes::ISHR:
    case Opcodes::LSHR:
    case Opcodes::IUSHR:
    case Opcodes::LUSHR:
    case Opcodes::IAND:
    case Opcodes::LAND:
    case Opcodes::IOR:
    case Opcodes::LOR:
    case Opcodes::IXOR:
    case Opcodes::LXOR:
    case Opcodes::LCMP:
    case Opcodes::FCMPL:
    case Opcodes::FCMPG:
    case Opcodes::DCMPL:
    case Opcodes::DCMPG:
    case Opcodes::PUTFIELD: return 2;
    case Opcodes::IASTORE:
    case Opcodes::LASTORE:
    case Opcodes::FASTORE:
    case Opcodes::DASTORE:
    case Opcodes::AASTORE:
    case Opcodes::BASTORE:
    case Opcodes::CASTORE:
    case Opcodes::SASTORE: return 3;
    case Opcodes::PUTSTATIC: return 1;
    case Opcodes::MULTIANEWARRAY: return ca::core::tag_cast::cast<MultiANewArrayInsnNode*>(insn)->dims;
    case Opcodes::INVOKEDYNAMIC:
        return argumentValueCount(ca::core::tag_cast::cast<InvokeDynamicInsnNode*>(insn)->desc);
    case Opcodes::INVOKESTATIC: return argumentValueCount(ca::core::tag_cast::cast<MethodInsnNode*>(insn)->desc);
    case Opcodes::INVOKEVIRTUAL:
    case Opcodes::INVOKESPECIAL:
    case Opcodes::INVOKEINTERFACE:
        return argumentValueCount(ca::core::tag_cast::cast<MethodInsnNode*>(insn)->desc) + 1;
    default: return 0;
    }
}

void addConsumerForValue(const OriginalSourceValue& value, const AbstractInsnNode* consumer,
                         ConsumerMap& consumers)
{
    for (std::size_t i = 0; i < value.insns_.size(); ++i) {
        const auto* producer = value.insns_[i];
        if (producer) {
            consumers[producer].insert(consumer);
        }
    }
}

ConsumerMap buildConsumersMap(const std::vector<AbstractInsnNode*>&    instructions,
                              const std::vector<OriginalSourceFrame*>& frames)
{
    ConsumerMap consumers;
    for (std::size_t i = 0; i < instructions.size() && i < frames.size(); ++i) {
        auto* insn  = instructions[i];
        auto* frame = frames[i];
        if (!insn || !frame) continue;

        int slot = -1;
        if (isSingleVarLoadInsn(insn, slot) && slot >= 0 && slot < frame->getLocals()) {
            addConsumerForValue(frame->getLocal(slot), insn, consumers);
        }
        else if (insn->getType() == AbstractInsnNode::IINC_INSN) {
            slot = ca::core::tag_cast::cast<IincInsnNode*>(insn)->var;
            if (slot >= 0 && slot < frame->getLocals()) {
                addConsumerForValue(frame->getLocal(slot), insn, consumers);
            }
        }

        const int consumed = consumedValueCount(insn, frame);
        for (int offset = 0; offset < consumed; ++offset) {
            const int stackIndex = frame->getStackSize() - 1 - offset;
            if (stackIndex < 0) break;
            addConsumerForValue(frame->getStack(stackIndex), insn, consumers);
        }
    }
    return consumers;
}

FrameByInsn buildFrameByInsn(const std::vector<AbstractInsnNode*>&    instructions,
                             const std::vector<OriginalSourceFrame*>& frames)
{
    FrameByInsn out;
    for (std::size_t i = 0; i < instructions.size() && i < frames.size(); ++i) {
        if (instructions[i] && frames[i]) {
            out[instructions[i]] = frames[i];
        }
    }
    return out;
}

bool isMeaningfulSink(AbstractInsnNode* insn)
{
    const int op = insn ? insn->getOpcode() : -1;
    if (op < 0) return false;
    if (insn->getType() == AbstractInsnNode::METHOD_INSN ||
        insn->getType() == AbstractInsnNode::JUMP_INSN ||
        insn->getType() == AbstractInsnNode::LOOKUPSWITCH_INSN ||
        insn->getType() == AbstractInsnNode::TABLESWITCH_INSN) {
        return true;
    }
    if (op == Opcodes::PUTSTATIC || op == Opcodes::PUTFIELD || op == Opcodes::MONITORENTER ||
        op == Opcodes::MONITOREXIT || op == Opcodes::ATHROW) {
        return true;
    }
    if (op >= Opcodes::IRETURN && op <= Opcodes::ARETURN) {
        return true;
    }
    return op >= Opcodes::IASTORE && op <= Opcodes::SASTORE;
}

bool canReachMeaningfulSink(AbstractInsnNode* start, const ConsumerMap& consumers)
{
    std::unordered_set<const AbstractInsnNode*> visited;
    std::vector<const AbstractInsnNode*>        queue;
    queue.push_back(start);
    visited.insert(start);
    for (std::size_t index = 0; index < queue.size(); ++index) {
        auto* current = const_cast<AbstractInsnNode*>(queue[index]);
        if (isMeaningfulSink(current)) return true;
        auto it = consumers.find(current);
        if (it == consumers.end()) continue;
        for (auto* next : it->second) {
            if (visited.insert(next).second) {
                queue.push_back(next);
            }
        }
    }
    return false;
}

bool isSafeComputingInsn(AbstractInsnNode* insn)
{
    if (!insn) return false;
    const int op = insn->getOpcode();
    if (op < 0) return false;
    int  slot = -1;
    bool wide = false;
    if (isSingleVarStoreInsn(insn, slot, wide) || InsnUtil::isReturn(op)) return false;
    if (insn->getType() == AbstractInsnNode::METHOD_INSN ||
        insn->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN ||
        insn->getType() == AbstractInsnNode::JUMP_INSN ||
        insn->getType() == AbstractInsnNode::LOOKUPSWITCH_INSN ||
        insn->getType() == AbstractInsnNode::TABLESWITCH_INSN ||
        insn->getType() == AbstractInsnNode::IINC_INSN) {
        return false;
    }
    if (InsnUtil::isFieldAccess(op) || InsnUtil::isArrayLoad(op) || InsnUtil::isArrayStore(op)) {
        return false;
    }
    return op != Opcodes::NEW && op != Opcodes::ANEWARRAY && op != Opcodes::NEWARRAY &&
           op != Opcodes::MULTIANEWARRAY && op != Opcodes::ATHROW && op != Opcodes::MONITORENTER &&
           op != Opcodes::MONITOREXIT && op != Opcodes::CHECKCAST;
}

void collectRemovableProducer(AbstractInsnNode*                            producer,
                              std::unordered_set<const AbstractInsnNode*>& removable,
                              const FrameByInsn&                           frameByInsn,
                              std::unordered_set<const AbstractInsnNode*>& visiting)
{
    if (!producer || !isSafeComputingInsn(producer)) return;
    if (!visiting.insert(producer).second) return;
    if (!removable.insert(producer).second) return;

    auto fit = frameByInsn.find(producer);
    if (fit == frameByInsn.end() || !fit->second) return;
    const int consumed = consumedValueCount(producer, fit->second);
    for (int offset = 0; offset < consumed; ++offset) {
        const int stackIndex = fit->second->getStackSize() - 1 - offset;
        if (stackIndex < 0) break;
        const auto parentValue = fit->second->getStack(stackIndex);
        for (std::size_t i = 0; i < parentValue.insns_.size(); ++i) {
            const auto* parent = parentValue.insns_[i];
            collectRemovableProducer(
                const_cast<AbstractInsnNode*>(parent), removable, frameByInsn, visiting);
        }
    }
}

std::unordered_set<const AbstractInsnNode*> collectRemovableSources(
    const OriginalSourceValue& value, const FrameByInsn& frameByInsn)
{
    std::unordered_set<const AbstractInsnNode*> removable;
    std::unordered_set<const AbstractInsnNode*> visiting;
    for (std::size_t i = 0; i < value.insns_.size(); ++i) {
        const auto* producer = value.insns_[i];
        collectRemovableProducer(
            const_cast<AbstractInsnNode*>(producer), removable, frameByInsn, visiting);
    }
    return removable;
}

std::unordered_set<const AbstractInsnNode*> filterSafeRemovable(
    const std::unordered_set<const AbstractInsnNode*>& candidates,
    const std::unordered_set<const AbstractInsnNode*>& deadStores, const ConsumerMap& consumers)
{
    auto safe    = candidates;
    bool changed = false;
    do {
        changed = false;
        std::vector<const AbstractInsnNode*> snapshot(safe.begin(), safe.end());
        for (auto* candidate : snapshot) {
            auto it = consumers.find(candidate);
            if (it == consumers.end()) continue;
            const bool hasOutsideConsumer = std::any_of(
                it->second.begin(), it->second.end(), [&](const AbstractInsnNode* consumer) {
                    return safe.find(consumer) == safe.end() &&
                           deadStores.find(consumer) == deadStores.end();
                });
            if (hasOutsideConsumer) {
                safe.erase(candidate);
                changed = true;
            }
        }
    } while (changed);
    return safe;
}

bool sourceTreeCoveredBySafeSet(const OriginalSourceValue&                         value,
                                const std::unordered_set<const AbstractInsnNode*>& safe,
                                const FrameByInsn&                                 frameByInsn,
                                std::unordered_set<const AbstractInsnNode*>&       visiting)
{
    for (std::size_t i = 0; i < value.insns_.size(); ++i) {
        const auto* producer = value.insns_[i];
        if (!producer || safe.find(producer) == safe.end()) return false;
        if (!visiting.insert(producer).second) continue;
        auto fit = frameByInsn.find(producer);
        if (fit == frameByInsn.end() || !fit->second) continue;
        const int consumed =
            consumedValueCount(const_cast<AbstractInsnNode*>(producer), fit->second);
        for (int offset = 0; offset < consumed; ++offset) {
            const int stackIndex = fit->second->getStackSize() - 1 - offset;
            if (stackIndex < 0) return false;
            if (!sourceTreeCoveredBySafeSet(
                    fit->second->getStack(stackIndex), safe, frameByInsn, visiting)) {
                return false;
            }
        }
    }
    return true;
}

bool canRemoveConsumedSources(AbstractInsnNode* consumer, const OriginalSourceFrame* frame,
                              const std::unordered_set<const AbstractInsnNode*>& safe,
                              const FrameByInsn&                                 frameByInsn)
{
    const int consumed = consumedValueCount(consumer, frame);
    if (consumed <= 0 || !frame) return false;
    for (int offset = 0; offset < consumed; ++offset) {
        const int stackIndex = frame->getStackSize() - 1 - offset;
        if (stackIndex < 0) return false;
        std::unordered_set<const AbstractInsnNode*> visiting;
        if (!sourceTreeCoveredBySafeSet(frame->getStack(stackIndex), safe, frameByInsn, visiting)) {
            return false;
        }
    }
    return true;
}

int popOpcodeForStore(AbstractInsnNode* store, const OriginalSourceFrame* frame)
{
    int  slot = -1;
    bool wide = false;
    isSingleVarStoreInsn(store, slot, wide);
    if (frame && frame->getStackSize() > 0) {
        wide = frame->getStack(frame->getStackSize() - 1).getSize() == 2;
    }
    return wide ? Opcodes::POP2 : Opcodes::POP;
}

bool isPopRemovableProducer(AbstractInsnNode* insn)
{
    if (!insn) return false;
    int       slot = -1;
    const int op   = insn->getOpcode();
    return InsnUtil::isConstant(op) || isSingleVarLoadInsn(insn, slot) || op == Opcodes::DUP ||
           op == Opcodes::DUP2;
}

bool canRemoveForPop(const OriginalSourceValue& value, const AbstractInsnNode* popInsn,
                     const ConsumerMap&                                 consumers,
                     const std::unordered_set<const AbstractInsnNode*>& planned,
                     std::unordered_set<const AbstractInsnNode*>&       producers)
{
    if (value.insns_.empty()) return false;
    for (std::size_t i = 0; i < value.insns_.size(); ++i) {
        const auto* producer        = value.insns_[i];
        auto*       mutableProducer = const_cast<AbstractInsnNode*>(producer);
        if (!isPopRemovableProducer(mutableProducer) || planned.find(producer) != planned.end()) {
            return false;
        }
        auto it = consumers.find(producer);
        if (it == consumers.end() || it->second.empty()) return false;
        for (auto* consumer : it->second) {
            if (consumer != popInsn) return false;
        }
        producers.insert(producer);
    }
    return true;
}

void removeOrReplacePlanned(InsnList& instructions, const std::vector<AbstractInsnNode*>& original,
                            const std::unordered_set<const AbstractInsnNode*>&      toRemove,
                            const std::unordered_map<const AbstractInsnNode*, int>& replacements)
{
    for (auto* insn : original) {
        if (!insn || !instructions.contains(insn)) continue;
        auto replaceIt = replacements.find(insn);
        if (replaceIt != replacements.end()) {
            instructions.set(insn, instructions.create<InsnNode>(replaceIt->second));
            continue;
        }
        if (toRemove.find(insn) != toRemove.end()) {
            instructions.removeAndDelete(insn);
        }
    }
}

class AnalyzerCapacityGuard
{
public:
    explicit AnalyzerCapacityGuard(MethodNode& methodNode)
        : methodNode_(methodNode)
        , originalMaxStack_(methodNode.maxStack)
    {
        if (methodNode_.maxStack <= 0 && !methodNode_.instructions.isEmpty()) {
            methodNode_.maxStack = (std::max)(8, methodNode_.instructions.size());
        }
    }

    ~AnalyzerCapacityGuard()
    {
        if (!committed_) methodNode_.maxStack = originalMaxStack_;
    }

    void commit() { committed_ = true; }

private:
    MethodNode& methodNode_;
    int         originalMaxStack_;
    bool        committed_ = false;
};

//方法签名三元组 (owner, name, desc)，对齐 Java TriplePool.of(owner,name,desc)。
struct MethodKey
{
    ca::str::Utf8StringRef owner;
    ca::str::Utf8StringRef name;
    ca::str::Utf8StringRef desc;
    bool                   operator==(const MethodKey& o) const
    {
        return owner == o.owner && name == o.name && desc == o.desc;
    }
};
struct MethodKeyHash
{
    size_t operator()(const MethodKey& k) const
    {
        //简单组合 hash —— key 总数小，碰撞无所谓
        auto h = std::hash<ca::str::Utf8StringRef>{};
        return h(k.owner) ^ (h(k.name) << 1) ^ (h(k.desc) << 2);
    }
};

//把容器里所有 ClassResource 读成 ClassNode 列表（保持和原 resource 同序）。
//失败的类跳过（对齐 GlobalToClassPassAdaptor 的容错策略）。
std::vector<std::pair<ClassResource*, std::unique_ptr<ClassNode>>> loadAllClasses(
    ResourceContainer& container, ca::str::Utf8StringArena& arena)
{
    std::vector<std::pair<ClassResource*, std::unique_ptr<ClassNode>>> out;
    for (auto& [uri, resource] : container.getResources()) {
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto*       cls   = ca::core::tag_cast::cast<ClassResource>(resource.get());
        const auto& bytes = cls->get();
        if (bytes.empty()) continue;
        try {
            auto        cn = std::make_unique<ClassNode>();
            ClassReader reader(arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
            reader.accept(cn.get(), ClassReader::SKIP_DEBUG | ClassReader::SKIP_FRAMES);
            out.emplace_back(cls, std::move(cn));
        }
        catch (...) {
            //跳过异常类
        }
    }
    return out;
}

std::unordered_set<MethodKey, MethodKeyHash> collectDefinedMethods(
    const std::vector<std::pair<ClassResource*, std::unique_ptr<ClassNode>>>& classes)
{
    std::unordered_set<MethodKey, MethodKeyHash> definitions;
    for (const auto& [_, cn] : classes) {
        for (const auto* method : cn->methods) {
            if (!method) continue;
            definitions.insert({cn->name, method->name, method->desc});
        }
    }
    return definitions;
}

//第 1 遍：扫描所有方法，把 invoke; pop/pop2 模式的方法签名加入候选。
void collectCandidates(
    const std::vector<std::pair<ClassResource*, std::unique_ptr<ClassNode>>>& classes,
    const std::unordered_set<MethodKey, MethodKeyHash>&                       definitions,
    std::unordered_set<MethodKey, MethodKeyHash>&                             candidates)
{
    for (const auto& [_, cn] : classes) {
        for (const auto* method : cn->methods) {
            if (!method) continue;
            for (auto* insn = method->instructions.getFirst(); insn; insn = insn->getNext()) {
                if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
                auto* min  = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
                auto* next = min->getNext();
                if (!next) continue;
                MethodKey key{min->owner, min->name, min->desc};
                if (definitions.find(key) == definitions.end()) continue;
                //解析返回类型（用静态 arena；J/D 是 2 slot，其余非 void 是 1 slot）
                Type ret  = Type::getReturnType(min->desc);
                int  sort = ret.getSort();
                if (sort == Type::VOID) continue;
                if (sort == Type::LONG || sort == Type::DOUBLE) {
                    if (next->getOpcode() == Opcodes::POP2) {
                        candidates.insert(key);
                    }
                }
                else {
                    if (next->getOpcode() == Opcodes::POP) {
                        candidates.insert(key);
                    }
                }
            }
        }
    }
}

//第 2 遍：用共享副作用分析剔除非 Pure 候选。
void filterImpureCandidates(const MethodEffectModel&                      effects,
                            std::unordered_set<MethodKey, MethodKeyHash>& candidates)
{
    for (auto it = candidates.begin(); it != candidates.end();) {
        if (effects.isPure(
                it->owner.to_std_string(), it->name.to_std_string(), it->desc.to_std_string())) {
            ++it;
        }
        else {
            it = candidates.erase(it);
        }
    }
}

//第 3 遍：对候选方法的结果丢弃调用做替换。
//Java 行为：invoke; pop → iconst_1；invoke; pop2 → iconst_1（Java 注释里说「替换成两条，
//让其他优化器清除」——但对 2 slot 实际只插一条 iconst_1 后留 pop2，仍由后续 pass 清理，
//与 Java 等价的最终效果一致）。
//注意：InsnList::set 经 removeAndDelete 丢弃旧节点（堆节点 delete、arena 节点随方法域
//回收），调用方不能再 delete（否则 double-free/坏 free）。
bool rewriteDiscardedCalls(ClassNode&                                          cn,
                           const std::unordered_set<MethodKey, MethodKeyHash>& candidates)
{
    bool modified = false;
    for (auto* method : cn.methods) {
        if (!method) continue;
        bool methodModified = false;
        for (auto* insn = method->instructions.getFirst(); insn;) {
            auto* next = insn->getNext();
            if (insn->getType() != AbstractInsnNode::METHOD_INSN) {
                insn = next;
                continue;
            }
            auto* min   = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
            auto* after = min->getNext();
            if (!after) {
                insn = next;
                continue;
            }
            MethodKey key{min->owner, min->name, min->desc};
            if (candidates.find(key) == candidates.end()) {
                insn = next;
                continue;
            }

            Type ret  = Type::getReturnType(min->desc);
            int  sort = ret.getSort();
            if (sort == Type::LONG || sort == Type::DOUBLE) {
                if (after->getOpcode() == Opcodes::POP2) {
                    //set 内部 delete insn，after(POP2) 保留供后续 pass 清理
                    method->instructions.set(
                        insn, method->instructions.create<InsnNode>(Opcodes::ICONST_1));
                    methodModified = true;
                    insn           = after;
                    continue;
                }
            }
            else if (sort != Type::VOID) {
                if (after->getOpcode() == Opcodes::POP) {
                    method->instructions.set(
                        insn, method->instructions.create<InsnNode>(Opcodes::ICONST_1));
                    methodModified = true;
                    insn           = after;
                    continue;
                }
            }
            insn = next;
        }
        if (methodModified) modified = true;
    }
    return modified;
}

bool isDiscardPopForReturn(ca::i32 popOpcode, const Type& returnType)
{
    const int sort = returnType.getSort();
    if (sort == Type::VOID) return false;
    if (sort == Type::LONG || sort == Type::DOUBLE) return popOpcode == Opcodes::POP2;
    return popOpcode == Opcodes::POP;
}

bool isPrimitiveConstant(AbstractInsnNode* insn)
{
    if (!insn || insn->getOpcode() < 0) return false;
    int     intValue;
    ca::i64 longValue;
    float   floatValue;
    double  doubleValue;
    return InsnUtil::getIntValue(insn, intValue) || InsnUtil::getLongValue(insn, longValue) ||
           InsnUtil::getFloatValue(insn, floatValue) || InsnUtil::getDoubleValue(insn, doubleValue);
}

bool isStringDiscardProducer(AbstractInsnNode* insn)
{
    std::string value;
    return insn && InsnUtil::getStringValue(insn, value);
}

bool isSafeMathName(const std::string& name)
{
    static const std::unordered_set<std::string> kSafeNames = {
        "abs",  "max",       "min",      "sin",      "cos",           "tan",       "asin",
        "acos", "atan",      "atan2",    "sqrt",     "cbrt",          "ceil",      "floor",
        "rint", "round",     "exp",      "expm1",    "log",           "log10",     "log1p",
        "pow",  "signum",    "copySign", "hypot",    "IEEEremainder", "toRadians", "toDegrees",
        "ulp",  "nextAfter", "nextUp",   "nextDown", "scalb"};
    return kSafeNames.find(name) != kSafeNames.end();
}

bool isStringReadonlyDiscardCandidate(const MethodInsnNode&                 call,
                                      const std::vector<AbstractInsnNode*>& producers)
{
    if (call.getOpcode() != Opcodes::INVOKEVIRTUAL) return false;
    if (!call.owner.equals(ca::str::Utf8StringRef::from_cstr("java/lang/String"))) return false;
    if (call.name.equals(ca::str::Utf8StringRef::from_cstr("length")) ||
        call.name.equals(ca::str::Utf8StringRef::from_cstr("isEmpty"))) {
        return producers.size() == 1 && isStringDiscardProducer(producers.front());
    }
    return false;
}

bool isMathDiscardCandidate(const MethodInsnNode&                 call,
                            const std::vector<AbstractInsnNode*>& producers)
{
    if (call.getOpcode() != Opcodes::INVOKESTATIC) return false;
    const std::string owner = call.owner.to_std_string();
    if (owner != "java/lang/Math" && owner != "java/lang/StrictMath") return false;
    if (!isSafeMathName(call.name.to_std_string())) return false;
    if (producers.empty()) return false;
    return std::all_of(producers.begin(), producers.end(), isPrimitiveConstant);
}

std::vector<AbstractInsnNode*> collectAdjacentProducers(MethodInsnNode& call)
{
    std::vector<AbstractInsnNode*> reversed;
    ca::usize                      count = Type::getArgumentTypes(call.desc).size();
    if (call.getOpcode() != Opcodes::INVOKESTATIC) {
        count += 1;
    }

    auto* cursor = call.getPrevious();
    for (ca::usize i = 0; i < count; ++i) {
        if (!cursor || cursor->getOpcode() < 0) return {};
        reversed.push_back(cursor);
        cursor = cursor->getPrevious();
    }
    std::reverse(reversed.begin(), reversed.end());
    return reversed;
}

bool isUnusedResultDeletionCandidate(MethodInsnNode& call, AbstractInsnNode& pop,
                                     std::vector<AbstractInsnNode*>& producers)
{
    const Type returnType = Type::getReturnType(call.desc);
    if (!isDiscardPopForReturn(pop.getOpcode(), returnType)) return false;

    producers = collectAdjacentProducers(call);
    if (producers.empty()) return false;
    return isStringReadonlyDiscardCandidate(call, producers) ||
           isMathDiscardCandidate(call, producers);
}

}   //namespace

PreservedAnalyses DeadLocalStoreRemover::run(jbytecode::tree::MethodNode& methodNode,
                                             MethodAnalysisManager&       context)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    AnalyzerCapacityGuard capacity(methodNode);

    //帧分析走 method 级 AM：guard 已抬升 maxStack，getResult 复用/重算缓存的帧。
    const auto& frameResult = context.getResult<OriginalSourceFrameAnalysis>(methodNode);
    if (!frameResult.ok) return PreservedAnalyses::all();
    const auto& frames = frameResult.frames;

    auto instructions = methodNode.instructions.toArray();
    if (frames.size() != instructions.size()) return PreservedAnalyses::all();

    auto consumers   = buildConsumersMap(instructions, frames);
    auto frameByInsn = buildFrameByInsn(instructions, frames);

    std::unordered_set<int> meaningfulReadSlots;
    for (auto* insn : instructions) {
        int slot = -1;
        if (isSingleVarLoadInsn(insn, slot) && canReachMeaningfulSink(insn, consumers)) {
            meaningfulReadSlots.insert(slot);
        }
    }

    std::unordered_set<const AbstractInsnNode*> deadStores;
    for (auto* insn : instructions) {
        int  slot = -1;
        bool wide = false;
        if (isSingleVarStoreInsn(insn, slot, wide) &&
            meaningfulReadSlots.find(slot) == meaningfulReadSlots.end()) {
            deadStores.insert(insn);
        }
    }
    if (deadStores.empty()) return PreservedAnalyses::all();

    std::unordered_set<const AbstractInsnNode*> removableChain;
    for (auto* deadStore : deadStores) {
        auto fit = frameByInsn.find(deadStore);
        if (fit == frameByInsn.end() || !fit->second) continue;
        const int consumed =
            consumedValueCount(const_cast<AbstractInsnNode*>(deadStore), fit->second);
        for (int offset = 0; offset < consumed; ++offset) {
            const int stackIndex = fit->second->getStackSize() - 1 - offset;
            if (stackIndex < 0) break;
            auto sources = collectRemovableSources(fit->second->getStack(stackIndex), frameByInsn);
            removableChain.insert(sources.begin(), sources.end());
        }
    }

    auto safeSources = filterSafeRemovable(removableChain, deadStores, consumers);
    std::unordered_set<const AbstractInsnNode*>      toRemove;
    std::unordered_map<const AbstractInsnNode*, int> replacements;
    for (auto* deadStore : deadStores) {
        auto  fit   = frameByInsn.find(deadStore);
        auto* frame = fit != frameByInsn.end() ? fit->second : nullptr;
        if (canRemoveConsumedSources(
                const_cast<AbstractInsnNode*>(deadStore), frame, safeSources, frameByInsn)) {
            toRemove.insert(deadStore);
            toRemove.insert(safeSources.begin(), safeSources.end());
        }
        else {
            replacements[deadStore] =
                popOpcodeForStore(const_cast<AbstractInsnNode*>(deadStore), frame);
        }
    }

    if (toRemove.empty() && replacements.empty()) return PreservedAnalyses::all();
    removeOrReplacePlanned(methodNode.instructions, instructions, toRemove, replacements);
    capacity.commit();
    return PreservedAnalyses::none();
}

PreservedAnalyses NoLoadStoreEliminater::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();

    struct SlotUse
    {
        bool hasIstore       = false;
        bool hasIload        = false;
        bool hasIinc         = false;
        bool hasOtherVarInsn = false;
    };
    std::unordered_map<int, SlotUse> slotUses;
    const auto                       instructions = methodNode.instructions.toArray();
    for (auto* insn : instructions) {
        if (insn->getType() == AbstractInsnNode::IINC_INSN) {
            slotUses[ca::core::tag_cast::cast<IincInsnNode*>(insn)->var].hasIinc = true;
            continue;
        }
        if (insn->getType() != AbstractInsnNode::VAR_INSN) continue;
        auto* varInsn = ca::core::tag_cast::cast<VarInsnNode*>(insn);
        auto& use     = slotUses[varInsn->var];
        if (insn->getOpcode() == Opcodes::ISTORE) {
            use.hasIstore = true;
        }
        else if (insn->getOpcode() == Opcodes::ILOAD) {
            use.hasIload = true;
        }
        else {
            use.hasOtherVarInsn = true;
        }
    }

    std::unordered_set<const AbstractInsnNode*>      toRemove;
    std::unordered_map<const AbstractInsnNode*, int> replacements;
    for (auto* insn : instructions) {
        if (insn->getType() != AbstractInsnNode::VAR_INSN ||
            insn->getOpcode() != Opcodes::ISTORE) {
            continue;
        }
        auto* varInsn = ca::core::tag_cast::cast<VarInsnNode*>(insn);
        auto  it      = slotUses.find(varInsn->var);
        if (it == slotUses.end()) continue;
        const auto& use = it->second;
        if (!use.hasIstore || use.hasIload || use.hasIinc || use.hasOtherVarInsn) continue;

        auto* producer = insn->getPrevious();
        if (producer && isIntConstant(producer)) {
            toRemove.insert(producer);
            toRemove.insert(insn);
        }
        else {
            //未证明 producer 可删时只移除局部写入，保留计算、副作用和栈平衡。
            replacements[insn] = Opcodes::POP;
        }
    }
    if (toRemove.empty() && replacements.empty()) return PreservedAnalyses::all();
    removeOrReplacePlanned(methodNode.instructions, instructions, toRemove, replacements);
    return PreservedAnalyses::none();
}

PreservedAnalyses PopUnUsedLocalVariables::run(MethodNode&            methodNode,
                                               MethodAnalysisManager& context)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    AnalyzerCapacityGuard capacity(methodNode);

    //帧分析走 method 级 AM：guard 已抬升 maxStack，getResult 复用/重算缓存的帧。
    const auto& frameResult = context.getResult<OriginalSourceFrameAnalysis>(methodNode);
    if (!frameResult.ok) return PreservedAnalyses::all();
    const auto& frames = frameResult.frames;

    auto instructions = methodNode.instructions.toArray();
    if (frames.size() != instructions.size()) return PreservedAnalyses::all();

    std::unordered_set<const AbstractInsnNode*> storesInUse;
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        auto* insn  = instructions[i];
        auto* frame = frames[i];
        if (!insn || !frame) continue;

        int slot = -1;
        if (isSingleVarLoadInsn(insn, slot) && slot >= 0 && slot < frame->getLocals()) {
            const auto value = frame->getLocal(slot);
            for (std::size_t sourceIndex = 0; sourceIndex < value.insns_.size(); ++sourceIndex) {
                const auto* source      = value.insns_[sourceIndex];
                int         ignoredSlot = -1;
                bool        ignoredWide = false;
                if (isSingleVarStoreInsn(
                        const_cast<AbstractInsnNode*>(source), ignoredSlot, ignoredWide)) {
                    storesInUse.insert(source);
                }
            }
        }
        else if (insn->getType() == AbstractInsnNode::IINC_INSN) {
            slot = ca::core::tag_cast::cast<IincInsnNode*>(insn)->var;
            if (slot >= 0 && slot < frame->getLocals()) {
                const auto value = frame->getLocal(slot);
                for (std::size_t sourceIndex = 0; sourceIndex < value.insns_.size();
                     ++sourceIndex) {
                    const auto* source      = value.insns_[sourceIndex];
                    int         ignoredSlot = -1;
                    bool        ignoredWide = false;
                    if (isSingleVarStoreInsn(
                            const_cast<AbstractInsnNode*>(source), ignoredSlot, ignoredWide)) {
                        storesInUse.insert(source);
                    }
                }
            }
        }
    }

    std::unordered_map<const AbstractInsnNode*, int> replacements;
    for (std::size_t i = 0; i < instructions.size(); ++i) {
        auto* insn = instructions[i];
        int   slot = -1;
        bool  wide = false;
        if (isSingleVarStoreInsn(insn, slot, wide) && storesInUse.find(insn) == storesInUse.end()) {
            replacements[insn] = popOpcodeForStore(insn, frames[i]);
        }
    }

    if (replacements.empty()) return PreservedAnalyses::all();
    removeOrReplacePlanned(methodNode.instructions, instructions, {}, replacements);
    capacity.commit();
    return PreservedAnalyses::none();
}

PreservedAnalyses RedundantInvokeRemover::run(ResourceContainer&     container,
                                              GlobalAnalysisManager& am)
{
    auto& arena = am.arena();

    //加载所有类（跨类分析的输入）。
    auto classes = loadAllClasses(container, arena);
    if (classes.empty()) return PreservedAnalyses::all();

    //第 1 遍：候选收集。
    auto                                         definitions = collectDefinedMethods(classes);
    std::unordered_set<MethodKey, MethodKeyHash> candidates;
    collectCandidates(classes, definitions, candidates);
    if (candidates.empty()) return PreservedAnalyses::all();

    //第 2 遍：纯函数过滤。未经过 pipeline bootstrap 的单测会临时注册分析。
    if (!am.isRegistered<MethodEffectAnalysis>()) {
        am.template registerAnalysis<MethodEffectAnalysis>();
    }
    auto& effects = am.getResult<MethodEffectAnalysis>(container);
    filterImpureCandidates(effects, candidates);
    if (candidates.empty()) return PreservedAnalyses::all();

    //第 3 遍：全局改写 + 写回字节（两阶段提交：写回推迟到循环结束）。
    bool                       anyModified = false;
    ClassResourcePendingCommit pending;
    for (auto& [cls, cn] : classes) {
        if (!rewriteDiscardedCalls(*cn, candidates)) continue;
        anyModified = true;
        ClassWriter writer(arena, 0);
        cn->accept(&writer);
        pending.stage(cls, writer.toByteArray());
    }
    pending.commit();
    return anyModified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses RedundantLoadValEliminator::run(MethodNode&            methodNode,
                                                   MethodAnalysisManager& context)
{
    (void)context;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();

    bool modified = false;
    bool changed  = false;
    do {
        changed                                         = false;
        auto                               instructions = methodNode.instructions.toArray();
        //绑定方法 arena：循环退出那一轮未触发消除，result 带着 arena 克隆产物
        //析构（unbound 表会误 delete arena 节点）。
        InsnList                           result(&methodNode.arena());
        std::unordered_map<Label*, Label*> clonedLabels;

        for (int i = 0; i < static_cast<int>(instructions.size()); ++i) {
            auto* current = instructions[i];
            auto* next =
                i + 1 < static_cast<int>(instructions.size()) ? instructions[i + 1] : nullptr;
            if (next && next->getOpcode() == Opcodes::POP && isRedundantPopConstant(current)) {
                ++i;
                changed = true;
                continue;
            }
            if (next && next->getOpcode() == Opcodes::POP2 && isRedundantPop2Constant(current)) {
                ++i;
                changed = true;
                continue;
            }
            addCloned(result, current, methodNode.arena(), clonedLabels);
        }

        if (changed) {
            modified = true;
            methodNode.instructions.clear();
            methodNode.instructions.add(result);
        }
    } while (changed);

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses RedundantStoreEliminater::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();

    //slot -> (storeIndexList, loadIndexList)。统一使用 slot helper，覆盖 *_0..*_3。
    std::unordered_map<int, std::pair<std::vector<int>, std::vector<int>>> localVarUsages;
    std::unordered_set<int>                                           iincSlots;
    int                                                                    idx = 0;
    for (auto* insn : methodNode.instructions) {
        if (insn->getType() == AbstractInsnNode::IINC_INSN) {
            iincSlots.insert(ca::core::tag_cast::cast<IincInsnNode*>(insn)->var);
        }
        int  slot = -1;
        bool wide = false;
        if (loadSlot(insn, slot)) {
            localVarUsages[slot].second.push_back(idx);
        }
        else if (storeSlot(insn, slot, wide)) {
            localVarUsages[slot].first.push_back(idx);
        }
        idx++;
    }
    //对齐 Java：store 列表头部加虚拟 -1（默认值）
    for (auto& [slot, lists] : localVarUsages) {
        lists.first.insert(lists.first.begin(), -1);
    }

    //找冗余 store 下标
    std::unordered_set<int> redundantStoreIndex;
    for (auto& [slot, lists] : localVarUsages) {
        //IINC 读取并写回旧 slot 值；未建模其来源时不得把前一个 store 当作冗余。
        if (iincSlots.find(slot) != iincSlots.end()) continue;
        auto& storeList = lists.first;
        auto& loadList  = lists.second;
        if (storeList.size() == 1 && loadList.size() == 1) continue;   //都是 1 不优化
        for (size_t i = 0; i + 1 < storeList.size(); i++) {
            int currentStorePos = storeList[i];
            int nextStorePos    = storeList[i + 1];
            //isInArea：[current, next) 间是否有 load
            bool inArea = false;
            for (int load : loadList) {
                if (load >= currentStorePos && load < nextStorePos) {
                    inArea = true;
                    break;
                }
                if (load > nextStorePos) break;
            }
            if (!inArea) {
                redundantStoreIndex.insert(currentStorePos);
            }
        }
    }

    if (redundantStoreIndex.empty()) return PreservedAnalyses::all();

    //重建：冗余 store → pop/pop2，其余克隆（对齐 Java InsnBuilder 重建）。
    //用 clone 模式避免与原 InsnList 的所有权冲突（参考 CommutativeOperatorReorder）。
    //绑定方法 arena：arena 克隆产物并入 instructions 后残余表为空；绑定保证
    //万一未排空也不误 delete arena 节点。
    InsnList                           result(&methodNode.arena());
    std::unordered_map<Label*, Label*> clonedLabels;
    std::vector<AbstractInsnNode*>     oldNodes = methodNode.instructions.toArray();
    for (size_t i = 0; i < oldNodes.size(); i++) {
        if (redundantStoreIndex.count(static_cast<int>(i))) {
            int op = oldNodes[i]->getOpcode();
            if (op == Opcodes::ISTORE || op == Opcodes::FSTORE || op == Opcodes::ASTORE) {
                result.add(methodNode.create<InsnNode>(Opcodes::POP));
            }
            else if (op == Opcodes::LSTORE || op == Opcodes::DSTORE) {
                result.add(methodNode.create<InsnNode>(Opcodes::POP2));
            }
        }
        else {
            addCloned(result, oldNodes[i], methodNode.arena(), clonedLabels);
        }
    }

    methodNode.instructions.clear();
    methodNode.instructions.add(result);
    return PreservedAnalyses::none();
}

PreservedAnalyses UnusedCheckCleaner::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    //对齐 Java：必须是实例方法（静态方法不行，this 隐式非 null 才成立）
    if (MethodUtil::isStatic(methodNode)) return PreservedAnalyses::all();

    bool                           modified = false;
    std::vector<AbstractInsnNode*> toRemove;
    auto                           instructions = methodNode.instructions.toArray();
    for (size_t i = 0; i + 2 < instructions.size(); i++) {
        //aload 0
        if (instructions[i]->getType() != AbstractInsnNode::VAR_INSN) continue;
        auto* aload = ca::core::tag_cast::cast<VarInsnNode*>(instructions[i]);
        if (aload->getOpcode() != Opcodes::ALOAD || aload->var != 0) continue;
        //invokestatic java/util/Objects.requireNonNull (Ljava/lang/Object;)Ljava/lang/Object;
        if (instructions[i + 1]->getType() != AbstractInsnNode::METHOD_INSN) continue;
        auto* invoke = ca::core::tag_cast::cast<MethodInsnNode*>(instructions[i + 1]);
        if (invoke->getOpcode() != Opcodes::INVOKESTATIC) continue;
        if (!invoke->owner.equals(ca::str::Utf8StringRef::from_cstr("java/util/Objects"))) continue;
        if (!invoke->name.equals(ca::str::Utf8StringRef::from_cstr("requireNonNull"))) continue;
        if (!invoke->desc.equals(
                ca::str::Utf8StringRef::from_cstr("(Ljava/lang/Object;)Ljava/lang/Object;")))
            continue;
        //pop
        if (instructions[i + 2]->getOpcode() != Opcodes::POP) continue;
        //匹配，删除三指令（Java 策略为空 → 等价删除）
        toRemove.push_back(instructions[i]);
        toRemove.push_back(instructions[i + 1]);
        toRemove.push_back(instructions[i + 2]);
        modified = true;
        i += 2;   //跳过已匹配的
    }
    for (auto* insn : toRemove) {
        methodNode.instructions.removeAndDelete(insn);
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses UnusedResultCallEliminater::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();

    std::unordered_set<AbstractInsnNode*> toRemove;
    for (auto* insn : methodNode.instructions.toArray()) {
        if (!insn || insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
        auto* call = ca::core::tag_cast::cast<MethodInsnNode*>(insn);
        auto* pop  = call->getNext();
        if (!pop) continue;

        std::vector<AbstractInsnNode*> producers;
        if (!isUnusedResultDeletionCandidate(*call, *pop, producers)) continue;
        toRemove.insert(producers.begin(), producers.end());
        toRemove.insert(call);
        toRemove.insert(pop);
    }

    if (toRemove.empty()) return PreservedAnalyses::all();
    for (auto* insn : methodNode.instructions.toArray()) {
        if (toRemove.find(insn) == toRemove.end()) continue;
        methodNode.instructions.removeAndDelete(insn);
    }
    return PreservedAnalyses::none();
}

PreservedAnalyses UselessPopCleaner::run(jbytecode::tree::MethodNode& methodNode,
                                          MethodAnalysisManager&       context)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    AnalyzerCapacityGuard capacity(methodNode);

    //帧分析走 method 级 AM：guard 已抬升 maxStack，getResult 复用/重算缓存的帧。
    const auto& frameResult = context.getResult<OriginalSourceFrameAnalysis>(methodNode);
    if (!frameResult.ok) return PreservedAnalyses::all();
    const auto& frames = frameResult.frames;

    auto instructions = methodNode.instructions.toArray();
    if (frames.size() != instructions.size()) return PreservedAnalyses::all();

    auto                                        consumers = buildConsumersMap(instructions, frames);
    std::unordered_set<const AbstractInsnNode*> toRemove;
    std::unordered_map<const AbstractInsnNode*, int> replacements;

    for (std::size_t i = 0; i < instructions.size(); ++i) {
        auto* insn  = instructions[i];
        auto* frame = frames[i];
        if (!insn || !frame) continue;

        const int op = insn->getOpcode();
        if (op != Opcodes::POP && op != Opcodes::POP2) continue;
        if (frame->getStackSize() <= 0) continue;

        std::unordered_set<const AbstractInsnNode*> firstProducers;
        const auto first = frame->getStack(frame->getStackSize() - 1);
        const bool canRemoveFirst =
            canRemoveForPop(first, insn, consumers, toRemove, firstProducers);

        if (op == Opcodes::POP) {
            if (!canRemoveFirst) continue;
            toRemove.insert(firstProducers.begin(), firstProducers.end());
            toRemove.insert(insn);
            continue;
        }

        if (first.getSize() == 2) {
            if (!canRemoveFirst) continue;
            toRemove.insert(firstProducers.begin(), firstProducers.end());
            toRemove.insert(insn);
            continue;
        }

        if (frame->getStackSize() < 2) continue;
        std::unordered_set<const AbstractInsnNode*> secondProducers;
        const auto second = frame->getStack(frame->getStackSize() - 2);
        const bool canRemoveSecond =
            canRemoveForPop(second, insn, consumers, toRemove, secondProducers);

        if (canRemoveFirst && canRemoveSecond) {
            toRemove.insert(firstProducers.begin(), firstProducers.end());
            toRemove.insert(secondProducers.begin(), secondProducers.end());
            toRemove.insert(insn);
        }
        else if (canRemoveFirst || canRemoveSecond) {
            const auto& removable = canRemoveFirst ? firstProducers : secondProducers;
            toRemove.insert(removable.begin(), removable.end());
            replacements[insn] = Opcodes::POP;
        }
    }

    if (toRemove.empty() && replacements.empty()) return PreservedAnalyses::all();
    removeOrReplacePlanned(methodNode.instructions, instructions, toRemove, replacements);
    capacity.commit();
    return PreservedAnalyses::none();
}

//============================================================================
//DeadSideEffectFreeCallRemover（issue ）
//无副作用调用（nanoTime/currentTimeMillis/random）返回值经纯链最终被 pop 丢弃时整链删除。
//复用 UselessPopCleaner 同款帧溯源机制，仅把「可删生产者」谓词扩展到白名单调用。
//============================================================================

namespace {

//白名单：零参、无可观察副作用、返回 cat-2 的 JDK 调用。返回值被丢弃时可整体删除。
bool isSideEffectFreeDiscardableCall(const AbstractInsnNode* insn)
{
    if (!insn || insn->getType() != AbstractInsnNode::METHOD_INSN) return false;
    if (insn->getOpcode() != Opcodes::INVOKESTATIC) return false;
    const auto* m = ca::core::tag_cast::cast<const MethodInsnNode*>(insn);
    if (m->owner.equals("java/lang/System") && m->desc.equals("()J") &&
        (m->name.equals("nanoTime") || m->name.equals("currentTimeMillis")))
        return true;
    if (m->owner.equals("java/lang/Math") && m->name.equals("random") && m->desc.equals("()D"))
        return true;
    return false;
}

//链内允许的生产者：纯计算 或 白名单无副作用调用。
bool isJunkChainProducer(AbstractInsnNode* insn)
{
    return isSafeComputingInsn(insn) || isSideEffectFreeDiscardableCall(insn);
}

//从 producer 递归收集其纯/白名单生产链（同 collectRemovableProducer，但谓词含白名单调用）。
void collectJunkChain(AbstractInsnNode* producer, std::unordered_set<const AbstractInsnNode*>& out,
                      const FrameByInsn& frameByInsn,
                      std::unordered_set<const AbstractInsnNode*>& visiting)
{
    if (!producer || !isJunkChainProducer(producer)) return;
    if (!visiting.insert(producer).second) return;
    if (!out.insert(producer).second) return;

    auto fit = frameByInsn.find(producer);
    if (fit == frameByInsn.end() || !fit->second) return;
    const int consumed = consumedValueCount(producer, fit->second);
    for (int offset = 0; offset < consumed; ++offset) {
        const int stackIndex = fit->second->getStackSize() - 1 - offset;
        if (stackIndex < 0) break;
        const auto parentValue = fit->second->getStack(stackIndex);
        for (std::size_t i = 0; i < parentValue.insns_.size(); ++i) {
            collectJunkChain(
                const_cast<AbstractInsnNode*>(parentValue.insns_[i]), out, frameByInsn, visiting);
        }
    }
}

bool setContainsCall(const std::unordered_set<const AbstractInsnNode*>& set)
{
    for (const auto* n : set)
        if (isSideEffectFreeDiscardableCall(n)) return true;
    return false;
}

}   //namespace

PreservedAnalyses DeadSideEffectFreeCallRemover::run(jbytecode::tree::MethodNode& methodNode,
                                                     MethodAnalysisManager&       context)
{
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();
    AnalyzerCapacityGuard capacity(methodNode);

    const auto& frameResult = context.getResult<OriginalSourceFrameAnalysis>(methodNode);
    if (!frameResult.ok) return PreservedAnalyses::all();
    const auto& frames       = frameResult.frames;
    auto        instructions = methodNode.instructions.toArray();
    if (frames.size() != instructions.size()) return PreservedAnalyses::all();

    const auto  consumers   = buildConsumersMap(instructions, frames);
    const auto  frameByInsn = buildFrameByInsn(instructions, frames);
    std::unordered_map<const AbstractInsnNode*, int> indexOf;
    for (int i = 0; i < static_cast<int>(instructions.size()); ++i)
        indexOf[instructions[static_cast<std::size_t>(i)]] = i;

    std::unordered_set<const AbstractInsnNode*> toRemove;

    for (std::size_t i = 0; i < instructions.size(); ++i) {
        auto* insn  = instructions[i];
        auto* frame = frames[i];
        if (!insn || !frame) continue;
        const int op = insn->getOpcode();
        if (op != Opcodes::POP && op != Opcodes::POP2) continue;
        if (frame->getStackSize() <= 0) continue;
        if (toRemove.count(insn)) continue;

        //被 pop 丢弃的 value(s)：POP 取栈顶；POP2 取 cat-2 栈顶，或两个 cat-1。
        std::vector<OriginalSourceValue> discarded;
        const auto                       top = frame->getStack(frame->getStackSize() - 1);
        if (op == Opcodes::POP || top.getSize() == 2) {
            discarded.push_back(top);
        }
        else {
            if (frame->getStackSize() < 2) continue;
            discarded.push_back(top);
            discarded.push_back(frame->getStack(frame->getStackSize() - 2));
        }

        std::unordered_set<const AbstractInsnNode*> candidates;
        std::unordered_set<const AbstractInsnNode*> visiting;
        for (const auto& v : discarded)
            for (std::size_t k = 0; k < v.insns_.size(); ++k)
                collectJunkChain(
                    const_cast<AbstractInsnNode*>(v.insns_[k]), candidates, frameByInsn, visiting);
        if (candidates.empty() || !setContainsCall(candidates)) continue;

        //链外消费者过滤：pop 本身算允许 sink。过滤后须仍完整覆盖被丢弃值的生产树、且仍含调用。
        std::unordered_set<const AbstractInsnNode*> allowedSink{insn};
        const auto safe = filterSafeRemovable(candidates, allowedSink, consumers);
        if (safe.empty() || !setContainsCall(safe)) continue;

        bool covered = true;
        for (const auto& v : discarded) {
            std::unordered_set<const AbstractInsnNode*> vis;
            if (!sourceTreeCoveredBySafeSet(v, safe, frameByInsn, vis)) {
                covered = false;
                break;
            }
        }
        if (!covered) continue;

        //单基本块约束：[链首, pop] 区间内只允许 safe / pop / 边界节点，且无 LabelNode。
        const int popIdx = indexOf[insn];
        int       minIdx = popIdx;
        for (const auto* c : safe) minIdx = (std::min)(minIdx, indexOf[c]);
        bool contiguous = true;
        for (int k = minIdx; k <= popIdx; ++k) {
            auto* node = instructions[static_cast<std::size_t>(k)];
            if (node == insn) continue;
            if (node->getType() == AbstractInsnNode::LABEL) {
                contiguous = false;
                break;
            }
            if (safe.find(node) == safe.end() && !InsnUtil::isBoundaryNode(node)) {
                contiguous = false;
                break;
            }
        }
        if (!contiguous) continue;

        toRemove.insert(safe.begin(), safe.end());
        toRemove.insert(insn);
    }

    if (toRemove.empty()) return PreservedAnalyses::all();
    removeOrReplacePlanned(methodNode.instructions, instructions, toRemove, {});
    capacity.commit();
    return PreservedAnalyses::none();
}

//============================================================================
//SafeDeadCodeRemover（对应 Java controlflow/SafeDeadCodeRemover.java）
//基于保守前向 CFG 分析删除不可达普通指令。仅删除 opcode>=0 且无 reachable frame 的指令，
//异常边和 label 不主动改写；分析失败 fail closed。
//============================================================================

PreservedAnalyses SafeDeadCodeRemover::run(MethodNode& methodNode, MethodAnalysisManager& am)
{
    (void)am;
    if (methodNode.instructions.isEmpty()) return PreservedAnalyses::all();

    SuperInterpreter                                     interpreter;
    Analyzer<BasicValue>                                 analyzer(&interpreter);
    std::vector<jbytecode::analysis::Frame<BasicValue>*> frames;
    try {
        frames = analyzer.analyze(ca::str::ZUtf8StringRef::from_static("T"), &methodNode);
    }
    catch (...) {
        return PreservedAnalyses::all();
    }

    if (frames.empty() || frames.size() != static_cast<ca::usize>(methodNode.instructions.size())) {
        return PreservedAnalyses::all();
    }

    std::unordered_set<AbstractInsnNode*> reachable;
    for (ca::usize i = 0; i < frames.size(); ++i) {
        if (frames[i] != nullptr) {
            reachable.insert(methodNode.instructions.get(static_cast<int>(i)));
        }
    }

    bool  modified = false;
    auto* node     = methodNode.instructions.getFirst();
    while (node) {
        auto* next = node->getNext();
        if (node->getOpcode() >= 0 && reachable.find(node) == reachable.end()) {
            methodNode.instructions.removeAndDelete(node);
            modified = true;
        }
        node = next;
    }

    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

//============================================================================
//NopRemover（对应 Java cleaner/NopRemover.java）
//删除方法中的 JVM NOP 指令；常作为其他 pass 的收尾清理。不删 label/frame/line/try-catch。
//============================================================================

PreservedAnalyses NopRemover::run(jbytecode::tree::MethodNode& methodNode,
                                  MethodAnalysisManager&       context)
{
    auto& insns    = methodNode.instructions;
    auto* node     = insns.getFirst();
    bool  modified = false;
    while (node) {
        auto* next = node->getNext();
        if (node->getOpcode() == Opcodes::NOP) {
            insns.removeAndDelete(node);
            modified = true;
        }
        node = next;
    }
    return modified ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

}   //namespace mjt::base
