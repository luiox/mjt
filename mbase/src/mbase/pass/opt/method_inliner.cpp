#include "mbase/pass/opt/method_inliner.hpp"
#include "mbase/analysis/method_frame_analysis.hpp"
#include "mbase/asm/classpath/call_graph.hpp"
#include "mbase/reference/reference_index_analysis.hpp"
#include "mbase/spi/class_resource_pending_commit.hpp"
#include "mbase/pass/opt/constant_jump_optimizer.hpp"
#include "mbase/pass/opt/constant_propagator.hpp"
#include "mbase/pass/opt/dead_code_remover.hpp"
#include "mbase/pass/opt/inline_detail.hpp"
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "jbytecode/analysis/analyzer.hpp"
#include "jbytecode/analysis/basic_interpreter.hpp"
#include "jbytecode/core/opcodes.hpp"
#include "jbytecode/jbytecode.hpp"
#include "mbase/asm/insn/class_util.hpp"
#include "mbase/model/resource_model.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

//=== 合并自 method_inliner_detail.hpp（单消费者实现细节，原 _detail.hpp 拆分文件）===

namespace {

using namespace jbytecode::analysis;
using namespace jbytecode::core;
using namespace jbytecode::tree;

struct LoadedInlineClass
{
    ClassResource*             resource = nullptr;
    std::unique_ptr<ClassNode> node;
};

struct InlineCandidate
{
    std::string                 owner;
    ca::i32                     ownerAccess              = 0;
    bool                        ownerHasClassInitializer = false;
    std::unique_ptr<MethodNode> method;
    ca::i32                     invocationCount = 0;
};

std::string methodKey(const std::string& owner, const std::string& name,
                      const std::string& descriptor)
{
    return owner + "\n" + name + "\n" + descriptor;
}

std::string methodKey(const ClassNode& owner, const MethodNode& method)
{
    return methodKey(
        owner.name.to_std_string(), method.name.to_std_string(), method.desc.to_std_string());
}

std::string methodKey(const MethodInsnNode& call)
{
    return methodKey(
        call.owner.to_std_string(), call.name.to_std_string(), call.desc.to_std_string());
}

bool readInlineClass(const ClassResource& resource, ca::str::Utf8StringArena& arena,
                     ClassNode& output)
{
    const auto& bytes = resource.get();
    if (bytes.empty()) return false;
    try {
        ClassReader reader(arena, bytes.data(), static_cast<ca::i32>(bytes.size()));
        reader.accept(&output, ClassReader::SKIP_DEBUG | ClassReader::SKIP_FRAMES);
        return !output.name.is_empty();
    }
    catch (...) {
        return false;
    }
}

std::vector<LoadedInlineClass> loadInlineClasses(ResourceContainer&        container,
                                                 ca::str::Utf8StringArena& arena)
{
    std::vector<LoadedInlineClass> result;
    for (auto& [_, resource] : container.getResources()) {
        if (!ca::core::tag_cast::isa<ClassResource>(resource.get())) continue;
        auto* classResource = ca::core::tag_cast::cast<ClassResource>(resource.get());
        auto  node          = std::make_unique<ClassNode>();
        if (!readInlineClass(*classResource, arena, *node)) continue;
        result.push_back({classResource, std::move(node)});
    }
    return result;
}

///@brief 序列化改写后的类；不直接写回资源，由调用方经 pending-commit 暂存。
std::vector<ca::u8> writeInlineClass(ca::str::Utf8StringArena& arena, ClassNode& classNode)
{
    ClassWriter writer(arena, ClassWriter::COMPUTE_FRAMES);
    classNode.accept(&writer);
    return writer.toByteArray();
}

///@brief 预登记 source 的 Label → 目标 arena 副本映射（B8 clone 协议伴生）。
///副本经 resolveCloneLabel 在 arena 上建（取代旧裸 new Label 堆分配）；
///tryCatch 引用的 Label 可能不出现在指令流，必须先登记才能用 .at 取回。
void collectLabels(MethodNode&                      source,
                   garena::Arena&                   arena,
                   std::unordered_map<Label*, Label*>& labels)
{
    for (auto* instruction : source.instructions.toArray()) {
        if (instruction->getType() != AbstractInsnNode::LABEL) continue;
        auto* label = ca::core::tag_cast::cast<LabelNode*>(instruction)->label;
        resolveCloneLabel(arena, labels, label);
    }
    for (auto* block : source.tryCatchBlocks) {
        if (!block) continue;
        resolveCloneLabel(arena, labels, block->start);
        resolveCloneLabel(arena, labels, block->end);
        resolveCloneLabel(arena, labels, block->handler);
    }
}

std::unique_ptr<MethodNode> snapshotMethod(MethodNode& source)
{
    auto result = std::make_unique<MethodNode>(
        source.access, source.name, source.desc, source.signature, source.exceptions);
    std::unordered_map<Label*, Label*> labels;
    //B8 新 clone 协议：快照是独立 MethodNode，Label/节点副本都归快照自身的 arena。
    collectLabels(source, result->arena(), labels);
    for (auto* instruction : source.instructions.toArray()) {
        //LabelNode 走同一 clone 路径：其 Label 已预登记，resolveCloneLabel 命中
        //同一副本，与 tryCatch/前向跳转共享身份。
        result->instructions.add(instruction->clone(result->arena(), labels));
    }
    for (auto* block : source.tryCatchBlocks) {
        if (!block) continue;
        result->tryCatchBlocks.push_back(result->create<TryCatchBlockNode>(labels.at(block->start),
                                                               labels.at(block->end),
                                                               labels.at(block->handler),
                                                               block->type));
    }
    result->maxStack  = source.maxStack;
    result->maxLocals = source.maxLocals;
    return result;
}

ca::i32 meaningfulInstructionCount(const MethodNode& method)
{
    ca::i32 count = 0;
    for (auto* instruction : method.instructions.toArray()) {
        if (instruction && instruction->getOpcode() >= 0) ++count;
    }
    return count;
}

bool hasClassInitializer(const ClassNode& owner)
{
    for (auto* method : owner.methods) {
        if (method && MethodUtil::isClinit(*method)) return true;
    }
    return false;
}

bool hasUnsupportedControlFlow(const MethodNode& method)
{
    for (auto* instruction : method.instructions.toArray()) {
        if (!instruction) continue;
        const auto opcode = instruction->getOpcode();
        if (opcode == Opcodes::JSR || opcode == Opcodes::RET) return true;
    }
    return false;
}

bool isCrossClassBodySafe(const MethodNode& method)
{
    //指令被搬到 caller 后，成员访问检查会改用 caller 作为访问主体。这里不尝试复制
    //ProGuard 的完整访问修复逻辑，而是只放行不依赖 callee 所在类访问上下文的指令。
    for (auto* instruction : method.instructions.toArray()) {
        if (!instruction) continue;
        switch (instruction->getType()) {
        case AbstractInsnNode::FIELD_INSN:
        case AbstractInsnNode::METHOD_INSN:
        case AbstractInsnNode::INVOKE_DYNAMIC_INSN:
        case AbstractInsnNode::TYPE_INSN:
        case AbstractInsnNode::MULTIANEWARRAY_INSN: return false;
        default: break;
        }
    }
    return true;
}

bool isBaseCandidate(ClassNode& owner, MethodNode& method)
{
    if (MethodUtil::isConstructor(method) || MethodUtil::isClinit(method) ||
        MethodUtil::isAbstract(method) || MethodUtil::isNative(method) ||
        method.instructions.isEmpty()) {
        return false;
    }
    if ((method.access & Opcodes::ACC_SYNCHRONIZED) != 0) return false;
    if ((owner.access & Opcodes::ACC_INTERFACE) != 0) return false;
    return !hasUnsupportedControlFlow(method);
}

bool isDispatchSafe(const MethodInsnNode& call, const InlineCandidate& candidate,
                    const std::string& callerOwner)
{
    const auto& method    = *candidate.method;
    const bool  isStatic  = (method.access & Opcodes::ACC_STATIC) != 0;
    const bool  isPrivate = (method.access & Opcodes::ACC_PRIVATE) != 0;
    if (call.getOpcode() == Opcodes::INVOKESTATIC) return isStatic;
    if (isStatic) return false;

    if (call.getOpcode() == Opcodes::INVOKESPECIAL) {
        return isPrivate && callerOwner == candidate.owner;
    }
    if (call.getOpcode() != Opcodes::INVOKEVIRTUAL) return false;
    return (method.access & Opcodes::ACC_FINAL) != 0 ||
           (candidate.ownerAccess & Opcodes::ACC_FINAL) != 0;
}

bool isCrossClassSafe(const InlineCandidate& candidate, const std::string& callerOwner)
{
    if (callerOwner == candidate.owner) return true;
    if ((candidate.ownerAccess & Opcodes::ACC_PUBLIC) == 0 ||
        (candidate.method->access & Opcodes::ACC_PUBLIC) == 0) {
        return false;
    }
    //移除 invokestatic 也会移除其类初始化触发点；存在 <clinit> 时可能改变可观察行为。
    if (MethodUtil::isStatic(*candidate.method) && candidate.ownerHasClassInitializer) return false;
    return isCrossClassBodySafe(*candidate.method);
}

ca::i32 parameterSlots(const MethodNode& method)
{
    ca::i32 slots = (method.access & Opcodes::ACC_STATIC) != 0 ? 0 : 1;
    for (const auto& argument : Type::getArgumentTypes(method.desc)) slots += argument.getSize();
    return slots;
}

std::unordered_map<AbstractInsnNode*, ca::i32> analyzeStackDepths(
    ClassNode& owner, MethodNode& method, ca::str::Utf8StringArena& arena)
{
    std::unordered_map<AbstractInsnNode*, ca::i32> result;
    //字符串线示范(总方案 §5.1,):经作业级 arena 注入,ANEWARRAY 合成描述符
    //intern 进作业 arena(进程级 staticArena 兜底已退役)。
    BasicInterpreter interpreter(arena);
    Analyzer<BasicValue>                           analyzer(&interpreter);
    try {
        auto frames       = analyzer.analyze(owner.name, &method);
        auto instructions = method.instructions.toArray();
        for (ca::usize i = 0; i < instructions.size() && i < frames.size(); ++i) {
            if (frames[i]) result[instructions[i]] = frames[i]->getStackSize();
        }
    }
    catch (...) {
        result.clear();
    }
    return result;
}

bool canInlineAtCall(const MethodInsnNode& call, const InlineCandidate& candidate,
                     ClassNode& callerOwner, MethodNode& caller,
                     const std::unordered_map<AbstractInsnNode*, ca::i32>& stackDepths,
                     const MethodInlinerOptions&                           options)
{
    if (!isDispatchSafe(call, candidate, callerOwner.name.to_std_string())) return false;
    const bool crossClass = !call.owner.equals(callerOwner.name);
    if (crossClass && (!options.allowCrossClass ||
                       !isCrossClassSafe(candidate, callerOwner.name.to_std_string()))) {
        return false;
    }
    if (caller.instructions.size() + candidate.method->instructions.size() >
        options.maxResultingInstructions) {
        return false;
    }
    if (!candidate.method->tryCatchBlocks.empty()) {
        const auto frame = stackDepths.find(const_cast<MethodInsnNode*>(&call));
        if (frame == stackDepths.end()) return false;
        const ca::i32 consumed = static_cast<ca::i32>(Type::getArgumentTypes(call.desc).size()) +
                                 (call.getOpcode() == Opcodes::INVOKESTATIC ? 0 : 1);
        //异常处理器入口会清空 operand stack，只保留异常对象。若调用参数之下还有 caller
        //自己的值，直接合并 callee 的异常边会破坏这些值，因此保守拒绝该调用点。
        if (frame->second != consumed) return false;
    }
    return true;
}

void addArgumentStores(InsnList& output, MethodNode& caller, MethodNode& callee, ca::i32 localBase)
{
    const auto           arguments = Type::getArgumentTypes(callee.desc);
    std::vector<ca::i32> slots(arguments.size(), 0);
    ca::i32              slot = MethodUtil::isStatic(callee) ? 0 : 1;
    for (ca::usize i = 0; i < arguments.size(); ++i) {
        slots[i] = slot;
        slot += arguments[i].getSize();
    }
    //JVM 调用栈上最后一个参数位于栈顶，必须逆序保存才能保持参数槽编号不变。
    for (ca::i32 i = static_cast<ca::i32>(arguments.size()) - 1; i >= 0; --i) {
        const auto index = static_cast<ca::usize>(i);
        output.add(
            caller.create<VarInsnNode>(arguments[index].getOpcode(Opcodes::ISTORE), localBase + slots[index]));
    }
    if (!MethodUtil::isStatic(callee)) {
        //invokevirtual/invokespecial 必须在执行方法体前完成 receiver 的空指针检查。
        output.add(caller.create<InsnNode>(Opcodes::DUP));
        output.add(caller.create<MethodInsnNode>(Opcodes::INVOKEVIRTUAL,
                                      ca::str::Utf8StringRef::from_cstr("java/lang/Object"),
                                      ca::str::Utf8StringRef::from_cstr("getClass"),
                                      ca::str::Utf8StringRef::from_cstr("()Ljava/lang/Class;"),
                                      false));
        output.add(caller.create<InsnNode>(Opcodes::POP));
        output.add(caller.create<VarInsnNode>(Opcodes::ASTORE, localBase));
    }
}

void cloneInlineBody(InsnList& output, MethodNode& caller, MethodNode& callee, ca::i32 localBase)
{
    //先建立完整映射，保证前向跳转、循环和 switch 在克隆时都能解析目标。
    //B8 新 clone 协议：内联体最终 insert 进 caller.instructions，Label/节点副本
    //都归 caller 的 arena；尾部 end 是全新控制流汇合点，也建在 caller arena。
    std::unordered_map<Label*, Label*> labels;
    collectLabels(callee, caller.arena(), labels);
    auto* end = caller.createLabel();

    for (auto* instruction : callee.instructions.toArray()) {
        const auto opcode = instruction->getOpcode();
        //返回值已经留在 caller 的 operand stack 上；这里只把所有出口汇合到内联尾部。
        if (opcode >= Opcodes::IRETURN && opcode <= Opcodes::RETURN) {
            output.add(caller.create<JumpInsnNode>(Opcodes::GOTO, end));
            continue;
        }

        //LabelNode 走同一 clone 路径：其 Label 已预登记，resolveCloneLabel 命中
        //同一副本，与 tryCatch/前向跳转共享身份。
        AbstractInsnNode* cloned = instruction->clone(caller.arena(), labels);
        if (cloned->getType() == AbstractInsnNode::VAR_INSN) {
            ca::core::tag_cast::cast<VarInsnNode*>(cloned)->var += localBase;
        }
        else if (cloned->getType() == AbstractInsnNode::IINC_INSN) {
            ca::core::tag_cast::cast<IincInsnNode*>(cloned)->var += localBase;
        }
        output.add(cloned);
    }
    output.add(caller.create<LabelNode>(end));

    for (auto* block : callee.tryCatchBlocks) {
        if (!block) continue;
        caller.tryCatchBlocks.push_back(caller.create<TryCatchBlockNode>(labels.at(block->start),
                                                              labels.at(block->end),
                                                              labels.at(block->handler),
                                                              block->type));
    }
}

bool inlineCall(MethodNode& caller, MethodInsnNode& call, InlineCandidate& candidate)
{
    auto&         callee       = *candidate.method;
    const ca::i32 localBase    = caller.maxLocals;
    const ca::i32 calleeLocals = std::max(callee.maxLocals, parameterSlots(callee));

    //绑定 caller arena：replacement 节点全部归 caller 的 arena（B9：owner 是 caller），
    //insert 进 caller.instructions 时整体排空；绑定保证任何残余节点不被误 delete。
    InsnList replacement(&caller.arena());
    addArgumentStores(replacement, caller, callee, localBase);
    cloneInlineBody(replacement, caller, callee, localBase);
    caller.instructions.insert(&call, replacement);
    caller.instructions.removeAndDelete(&call);

    caller.maxLocals = localBase + calleeLocals;
    caller.maxStack  = std::max(caller.maxStack, callee.maxStack + 2);
    return true;
}

bool hasImmediateKnownIntArgument(const MethodInsnNode& call)
{
    if (call.getOpcode() != Opcodes::INVOKESTATIC) return false;
    const auto arguments = Type::getArgumentTypes(call.desc);
    if (arguments.size() != 1 || arguments[0].getOpcode(Opcodes::ILOAD) != Opcodes::ILOAD) {
        return false;
    }
    auto* producer = call.getPrevious();
    while (producer && producer->getOpcode() < 0) producer = producer->getPrevious();
    if (!producer) return false;
    const auto opcode = producer->getOpcode();
    if ((opcode >= Opcodes::ICONST_M1 && opcode <= Opcodes::ICONST_5) ||
        opcode == Opcodes::BIPUSH || opcode == Opcodes::SIPUSH) {
        return true;
    }
    if (producer->getType() != AbstractInsnNode::LDC_INSN) return false;
    return ca::core::tag_cast::cast<LdcInsnNode*>(producer)->cst.as<int>() != nullptr;
}

bool inlineCallsInClass(ClassNode& owner, ca::str::Utf8StringArena& arena,
                        std::unordered_map<std::string, InlineCandidate>& candidates,
                        const MethodInlinerOptions&                       options)
{
    bool modifiedClass = false;
    for (auto* caller : owner.methods) {
        if (!caller || caller->instructions.isEmpty()) continue;
        const auto stackDepths            = analyzeStackDepths(owner, *caller, arena);
        const auto originalInstructions   = caller->instructions.toArray();
        bool       modifiedMethod         = false;
        bool       optimizeKnownArguments = false;
        for (auto* instruction : originalInstructions) {
            if (!instruction || instruction->getType() != AbstractInsnNode::METHOD_INSN) continue;
            auto*      call  = ca::core::tag_cast::cast<MethodInsnNode*>(instruction);
            const auto found = candidates.find(methodKey(*call));
            if (found == candidates.end()) continue;
            if (methodKey(owner, *caller) == found->first) continue;
            if (!canInlineAtCall(*call, found->second, owner, *caller, stackDepths, options))
                continue;
            optimizeKnownArguments |= hasImmediateKnownIntArgument(*call);
            modifiedMethod |= inlineCall(*caller, *call, found->second);
        }
        if (modifiedMethod && optimizeKnownArguments) {
            //经工厂构造：构造即注册内置分析，与所有 method pipeline 入口一致；
            //否则 method pass 的 getResult 会抛 logic_error。
            MethodAnalysisManager analysisManager = makeMethodAnalysisManager(arena);
            ConsLocalVarOptimizer localConstants;
            ConsJumpOptimizer     jumps;
            SafeDeadCodeRemover   deadCode;
            localConstants.run(*caller, analysisManager);
            jumps.run(*caller, analysisManager);
            deadCode.run(*caller, analysisManager);
        }
        modifiedClass |= modifiedMethod;
    }
    return modifiedClass;
}

}   //namespace


PreservedAnalyses MethodInliner::run(ResourceContainer& container, GlobalAnalysisManager& gam)
{
    auto classes = loadInlineClasses(container, gam.arena());
    if (classes.empty()) return PreservedAnalyses::all();

    //调用点计数与递归环检测消费共享 CallGraph（ReferenceIndex 之上的
    //一次扫描），不再维护私有调用图。未走 pipeline bootstrap 的直调
    //单测需临时注册分析（CallGraph 依赖 ReferenceIndex）。
    if (!gam.isRegistered<ReferenceIndexAnalysis>()) {
        gam.template registerAnalysis<ReferenceIndexAnalysis>();
    }
    if (!gam.isRegistered<CallGraphAnalysis>()) {
        gam.template registerAnalysis<CallGraphAnalysis>();
    }
    auto& callGraph = gam.getResult<CallGraphAnalysis>(container);

    std::unordered_map<std::string, InlineCandidate> candidates;
    for (const auto& loaded : classes) {
        const bool ownerHasClinit = hasClassInitializer(*loaded.node);
        for (auto* method : loaded.node->methods) {
            if (!method || !isBaseCandidate(*loaded.node, *method)) continue;
            const auto key = methodKey(*loaded.node, *method);
            const auto count =
                callGraph.callSiteCountOf(loaded.node->name, method->name, method->desc);
            if (count == 0) continue;
            if (callGraph.hasPath(loaded.node->name, method->name, method->desc,
                                  loaded.node->name, method->name, method->desc)) {
                continue;
            }
            const auto instructionCount = meaningfulInstructionCount(*method);
            if (instructionCount > options_.maxInlineInstructions &&
                !(options_.inlineSingleInvocation && count == 1)) {
                continue;
            }
            candidates.emplace(key,
                               InlineCandidate{loaded.node->name.to_std_string(),
                                               loaded.node->access,
                                               ownerHasClinit,
                                               snapshotMethod(*method),
                                               count});
        }
    }
    if (candidates.empty()) return PreservedAnalyses::all();

    bool                       modifiedAny = false;
    ClassResourcePendingCommit pending;
    for (auto& loaded : classes) {
        if (!loaded.resource || !loaded.node) continue;
        if (!inlineCallsInClass(*loaded.node, gam.arena(), candidates, options_)) continue;
        pending.stage(loaded.resource, writeInlineClass(gam.arena(), *loaded.node));
        modifiedAny = true;
    }
    pending.commit();
    return modifiedAny ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

PreservedAnalyses GetterInliner::run(ResourceContainer& container, GlobalAnalysisManager& gam)
{
    return runSmallInliner(container, gam, SmallInlineKind::Getter);
}

PreservedAnalyses ShortMethodInliner::run(ResourceContainer& container, GlobalAnalysisManager& gam)
{
    return runSmallInliner(container, gam, SmallInlineKind::Constant);
}

PreservedAnalyses ArgForwardMethodInliner::run(ResourceContainer&     container,
                                               GlobalAnalysisManager& gam)
{
    return runSmallInliner(container, gam, SmallInlineKind::Forward);
}

}   //namespace mjt::base
