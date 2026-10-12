#include "mbase/analysis/method_effect_analysis.hpp"

#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "jbytecode/jbytecode.hpp"
#include "mbase/model/resource_model.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {
namespace {

using namespace jbytecode::core;
using namespace jbytecode::tree;

std::string str(ca::str::Utf8StringRef value)
{
    return value.to_std_string();
}

std::string methodKey(const std::string& owner, const std::string& name, const std::string& desc)
{
    return owner + "\n" + name + "\n" + desc;
}

std::string methodKey(const MethodRefKey& method)
{
    return methodKey(method.owner, method.name, method.desc);
}

bool isCtorOrClinit(const MethodRefKey& method)
{
    return method.name == "<init>" || method.name == "<clinit>";
}

MethodEffectKind mergeEffect(MethodEffectKind lhs, MethodEffectKind rhs)
{
    if (lhs == MethodEffectKind::Impure || rhs == MethodEffectKind::Impure) {
        return MethodEffectKind::Impure;
    }
    if (lhs == MethodEffectKind::Unknown || rhs == MethodEffectKind::Unknown) {
        return MethodEffectKind::Unknown;
    }
    if (lhs == MethodEffectKind::ReadOnly || rhs == MethodEffectKind::ReadOnly) {
        return MethodEffectKind::ReadOnly;
    }
    return MethodEffectKind::Pure;
}

bool isArrayStore(ca::i32 opcode)
{
    return opcode >= Opcodes::IASTORE && opcode <= Opcodes::SASTORE;
}

bool isFieldRead(ca::i32 opcode)
{
    return opcode == Opcodes::GETSTATIC || opcode == Opcodes::GETFIELD;
}

bool isFieldWrite(ca::i32 opcode)
{
    return opcode == Opcodes::PUTSTATIC || opcode == Opcodes::PUTFIELD;
}

bool isAllocation(ca::i32 opcode)
{
    return opcode == Opcodes::NEW || opcode == Opcodes::NEWARRAY || opcode == Opcodes::ANEWARRAY ||
           opcode == Opcodes::MULTIANEWARRAY;
}

MethodEffectKind knownExternalEffect(const MethodRefKey& method)
{
    if (method.owner == "java/lang/String") {
        static const std::unordered_set<std::string> kReadOnlyStringMethods{
            "length\n()I",
            "isEmpty\n()Z",
            "charAt\n(I)C",
            "codePointAt\n(I)I",
        };
        if (kReadOnlyStringMethods.find(method.name + "\n" + method.desc) !=
            kReadOnlyStringMethods.end()) {
            return MethodEffectKind::ReadOnly;
        }
    }

    if (method.owner == "java/lang/Math" || method.owner == "java/lang/StrictMath") {
        static const std::unordered_set<std::string> kPureMathMethods{
            "abs\n(I)I",
            "abs\n(J)J",
            "abs\n(F)F",
            "abs\n(D)D",
            "min\n(II)I",
            "min\n(JJ)J",
            "min\n(FF)F",
            "min\n(DD)D",
            "max\n(II)I",
            "max\n(JJ)J",
            "max\n(FF)F",
            "max\n(DD)D",
        };
        if (kPureMathMethods.find(method.name + "\n" + method.desc) != kPureMathMethods.end()) {
            return MethodEffectKind::Pure;
        }
    }

    return MethodEffectKind::Unknown;
}

struct ScannedMethod
{
    MethodEffectSummary       summary;
    MethodEffectKind          localEffect = MethodEffectKind::Pure;
    std::vector<MethodRefKey> calls;
    MethodRefKey              constantCall;
    ConstantReturnValue       directConstant;
};

void scanConstantReturn(const MethodNode& method, ScannedMethod& scanned)
{
    if (!scanned.summary.hasBody || !method.tryCatchBlocks.empty()) return;
    std::vector<AbstractInsnNode*> executable;
    for (auto* insn = method.instructions.getFirst(); insn; insn = insn->getNext()) {
        if (insn->getOpcode() >= 0) executable.push_back(insn);
    }
    if (executable.size() != 2) return;
    auto*      producer     = executable[0];
    auto*      ret          = executable[1];
    const auto opcode       = producer->getOpcode();
    const auto returnOpcode = ret->getOpcode();
    if (returnOpcode == Opcodes::IRETURN &&
        (opcode == Opcodes::ICONST_M1 ||
         (opcode >= Opcodes::ICONST_0 && opcode <= Opcodes::ICONST_5))) {
        scanned.directConstant.kind = ConstantReturnKind::Int;
        scanned.directConstant.intValue =
            opcode == Opcodes::ICONST_M1 ? -1 : opcode - Opcodes::ICONST_0;
        return;
    }
    if (returnOpcode == Opcodes::LRETURN &&
        (opcode == Opcodes::LCONST_0 || opcode == Opcodes::LCONST_1)) {
        scanned.directConstant.kind     = ConstantReturnKind::Long;
        scanned.directConstant.intValue = opcode - Opcodes::LCONST_0;
        return;
    }
    if (returnOpcode == Opcodes::FRETURN && opcode >= Opcodes::FCONST_0 &&
        opcode <= Opcodes::FCONST_2) {
        scanned.directConstant.kind          = ConstantReturnKind::Float;
        scanned.directConstant.floatingValue = opcode - Opcodes::FCONST_0;
        return;
    }
    if (returnOpcode == Opcodes::DRETURN &&
        (opcode == Opcodes::DCONST_0 || opcode == Opcodes::DCONST_1)) {
        scanned.directConstant.kind          = ConstantReturnKind::Double;
        scanned.directConstant.floatingValue = opcode - Opcodes::DCONST_0;
        return;
    }
    if (returnOpcode == Opcodes::ARETURN && opcode == Opcodes::ACONST_NULL) {
        scanned.directConstant.kind = ConstantReturnKind::Null;
        return;
    }
    if (producer->getType() == AbstractInsnNode::LDC_INSN) {
        auto* ldc = ca::core::tag_cast::cast<LdcInsnNode>(producer);
        if (returnOpcode == Opcodes::IRETURN) {
            if (auto* value = ldc->cst.as<int>()) {
                scanned.directConstant.kind     = ConstantReturnKind::Int;
                scanned.directConstant.intValue = *value;
                return;
            }
        }
        else if (returnOpcode == Opcodes::LRETURN) {
            if (auto* value = ldc->cst.as<ca::i64>()) {
                scanned.directConstant.kind     = ConstantReturnKind::Long;
                scanned.directConstant.intValue = *value;
                return;
            }
            if (auto* value = ldc->cst.as<long long>()) {
                scanned.directConstant.kind     = ConstantReturnKind::Long;
                scanned.directConstant.intValue = static_cast<ca::i64>(*value);
                return;
            }
        }
        else if (returnOpcode == Opcodes::FRETURN) {
            if (auto* value = ldc->cst.as<float>()) {
                scanned.directConstant.kind          = ConstantReturnKind::Float;
                scanned.directConstant.floatingValue = *value;
                return;
            }
        }
        else if (returnOpcode == Opcodes::DRETURN) {
            if (auto* value = ldc->cst.as<double>()) {
                scanned.directConstant.kind          = ConstantReturnKind::Double;
                scanned.directConstant.floatingValue = *value;
                return;
            }
        }
        else if (returnOpcode == Opcodes::ARETURN) {
            if (auto* value = ldc->cst.as<std::string>()) {
                scanned.directConstant.kind        = ConstantReturnKind::String;
                scanned.directConstant.stringValue = *value;
                return;
            }
            if (auto* value = ldc->cst.as<ca::str::Utf8StringRef>()) {
                scanned.directConstant.kind        = ConstantReturnKind::String;
                scanned.directConstant.stringValue = value->to_std_string();
                return;
            }
        }
    }
    if (producer->getType() == AbstractInsnNode::METHOD_INSN && opcode == Opcodes::INVOKESTATIC) {
        auto* call = ca::core::tag_cast::cast<MethodInsnNode>(producer);
        if (Type::getArgumentCount(call->desc) == 0) {
            scanned.constantCall = {str(call->owner), str(call->name), str(call->desc)};
        }
    }
}

MethodEffectKind classifyLocalInstruction(AbstractInsnNode* insn)
{
    if (!insn) return MethodEffectKind::Pure;
    const ca::i32 opcode = insn->getOpcode();
    if (opcode < 0) return MethodEffectKind::Pure;

    if (isFieldWrite(opcode) || isArrayStore(opcode) || isAllocation(opcode) ||
        opcode == Opcodes::ATHROW || opcode == Opcodes::MONITORENTER ||
        opcode == Opcodes::MONITOREXIT ||
        insn->getType() == AbstractInsnNode::INVOKE_DYNAMIC_INSN) {
        return MethodEffectKind::Impure;
    }

    if (isFieldRead(opcode) || opcode == Opcodes::ARRAYLENGTH) {
        return MethodEffectKind::ReadOnly;
    }

    return MethodEffectKind::Pure;
}

ScannedMethod scanMethod(const ClassNode& clazz, const MethodNode& method)
{
    ScannedMethod scanned;
    scanned.summary.method   = {str(clazz.name), str(method.name), str(method.desc)};
    scanned.summary.declared = true;
    scanned.summary.hasBody  = (method.access & (Opcodes::ACC_ABSTRACT | Opcodes::ACC_NATIVE)) == 0;

    if (!scanned.summary.hasBody || isCtorOrClinit(scanned.summary.method)) {
        scanned.localEffect = MethodEffectKind::Unknown;
        return scanned;
    }

    MethodEffectKind local = MethodEffectKind::Pure;
    for (auto* insn = method.instructions.getFirst(); insn; insn = insn->getNext()) {
        if (insn->getType() == AbstractInsnNode::METHOD_INSN) {
            auto* mi = ca::core::tag_cast::cast<MethodInsnNode>(insn);
            scanned.calls.push_back({str(mi->owner), str(mi->name), str(mi->desc)});
            continue;
        }
        local = mergeEffect(local, classifyLocalInstruction(insn));
    }
    scanned.localEffect = local;
    scanConstantReturn(method, scanned);
    return scanned;
}

std::vector<ScannedMethod> scanContainer(ResourceContainer&        container,
                                         ca::str::Utf8StringArena& arena)
{
    std::vector<ScannedMethod> methods;
    for (auto& [uri, resource] : container.getResources()) {
        (void)uri;
        if (resource->getType() != ResourceType::Class) continue;
        auto* cls = ca::core::tag_cast::cast<ClassResource>(resource.get());
        if (!cls || cls->get().empty()) continue;

        try {
            ClassNode   clazz;
            ClassReader reader(arena, cls->get().data(), static_cast<ca::i32>(cls->get().size()));
            reader.accept(&clazz, ClassReader::SKIP_DEBUG | ClassReader::SKIP_FRAMES);
            for (auto* method : clazz.methods) {
                if (!method) continue;
                methods.push_back(scanMethod(clazz, *method));
            }
        }
        catch (...) {
            //Effect analysis is a query layer. Damaged class resources must
            //not make every consumer unavailable; missing facts stay Unknown.
        }
    }
    return methods;
}

MethodEffectKind computeMethodEffect(
    const ScannedMethod&                                     method,
    const std::unordered_map<std::string, MethodEffectKind>& currentEffects)
{
    MethodEffectKind effect = method.localEffect;
    for (const auto& call : method.calls) {
        auto                   it = currentEffects.find(methodKey(call));
        const MethodEffectKind callee =
            it == currentEffects.end() ? knownExternalEffect(call) : it->second;
        effect = mergeEffect(effect, callee);
    }
    return effect;
}

}   //namespace

const char* methodEffectKindName(MethodEffectKind kind)
{
    switch (kind) {
    case MethodEffectKind::Pure: return "Pure";
    case MethodEffectKind::ReadOnly: return "ReadOnly";
    case MethodEffectKind::Impure: return "Impure";
    case MethodEffectKind::Unknown: return "Unknown";
    }
    return "Unknown";
}

void MethodEffectModel::clear()
{
    summaries_.clear();
    summariesByKey_.clear();
}

void MethodEffectModel::appendSummary(const MethodEffectSummary& summary)
{
    const std::string key = methodKey(summary.method);
    auto              it  = summariesByKey_.find(key);
    if (it != summariesByKey_.end()) {
        summaries_[it->second] = summary;
        return;
    }
    summariesByKey_[key] = summaries_.size();
    summaries_.push_back(summary);
}

void MethodEffectModel::build(ResourceContainer& container, ca::str::Utf8StringArena& arena)
{
    clear();
    auto methods = scanContainer(container, arena);

    std::unordered_map<std::string, MethodEffectKind>    currentEffects;
    std::unordered_map<std::string, ConstantReturnValue> currentConstants;
    for (const auto& method : methods) {
        currentEffects[methodKey(method.summary.method)]   = method.localEffect;
        currentConstants[methodKey(method.summary.method)] = method.directConstant;
    }

    //Local bytecode facts are monotonic; iterating the direct-call graph lets
    //pure/read-only callees improve callers while unknown/external edges keep
    //the fixed point conservative.
    bool changed = true;
    for (ca::usize round = 0; changed && round < methods.size() + 1; ++round) {
        changed = false;
        for (const auto& method : methods) {
            const std::string      key  = methodKey(method.summary.method);
            const MethodEffectKind next = computeMethodEffect(method, currentEffects);
            if (currentEffects[key] == next) continue;
            currentEffects[key] = next;
            changed             = true;
        }
        for (const auto& method : methods) {
            const std::string   key  = methodKey(method.summary.method);
            ConstantReturnValue next = method.directConstant;
            if (!next.isKnown() && currentEffects[key] == MethodEffectKind::Pure &&
                !method.constantCall.name.empty()) {
                auto it = currentConstants.find(methodKey(method.constantCall));
                if (it != currentConstants.end()) next = it->second;
            }
            if (!(currentConstants[key] == next)) {
                currentConstants[key] = next;
                changed               = true;
            }
        }
    }

    for (auto& method : methods) {
        auto summary           = method.summary;
        summary.kind           = currentEffects[methodKey(summary.method)];
        summary.constantReturn = currentConstants[methodKey(summary.method)];
        appendSummary(summary);
    }
}

const MethodEffectSummary* MethodEffectModel::effectOf(const std::string& owner,
                                                       const std::string& name,
                                                       const std::string& desc) const
{
    auto it = summariesByKey_.find(methodKey(owner, name, desc));
    return it == summariesByKey_.end() ? nullptr : &summaries_[it->second];
}

MethodEffectKind MethodEffectModel::effectKindOf(const std::string& owner, const std::string& name,
                                                 const std::string& desc) const
{
    auto* summary = effectOf(owner, name, desc);
    return summary ? summary->kind : MethodEffectKind::Unknown;
}

bool MethodEffectModel::isPure(const std::string& owner, const std::string& name,
                               const std::string& desc) const
{
    return effectKindOf(owner, name, desc) == MethodEffectKind::Pure;
}

bool MethodEffectModel::isReadOnlyOrPure(const std::string& owner, const std::string& name,
                                         const std::string& desc) const
{
    const MethodEffectKind kind = effectKindOf(owner, name, desc);
    return kind == MethodEffectKind::Pure || kind == MethodEffectKind::ReadOnly;
}

ConstantReturnValue MethodEffectModel::constantReturnOf(const std::string& owner,
                                                        const std::string& name,
                                                        const std::string& desc) const
{
    auto* summary = effectOf(owner, name, desc);
    return summary ? summary->constantReturn : ConstantReturnValue{};
}

MethodEffectAnalysis::Result MethodEffectAnalysis::run(ResourceContainer&     container,
                                                       GlobalAnalysisManager& gam)
{
    MethodEffectModel model;
    model.build(container, gam.arena());
    return model;
}

}   //namespace mjt::base
