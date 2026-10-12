#include "mbase/analysis/exception_flow_analysis.hpp"

#include <algorithm>
#include <memory>

#include "jbytecode/jbytecode.hpp"
#include "mbase/hierarchy/class_hierarchy.hpp"
#include "mbase/model/resource_model.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {
namespace {

using namespace jbytecode::core;
using namespace jbytecode::tree;

std::string key(const MethodRefKey& method)
{
    return method.owner + '\n' + method.name + '\n' + method.desc;
}

void appendUnique(std::vector<std::string>& values, const std::string& value)
{
    if (value.empty() || std::find(values.begin(), values.end(), value) != values.end()) return;
    values.push_back(value);
}

bool isBuiltinSubtype(const std::string& type, const std::string& handler)
{
    if (handler == "java/lang/Throwable") return true;
    if (handler == "java/lang/Exception") {
        return type == "java/lang/RuntimeException" || type == "java/lang/IllegalStateException" ||
               type == "java/lang/NullPointerException" ||
               type == "java/lang/IllegalArgumentException";
    }
    if (handler == "java/lang/RuntimeException") {
        return type == "java/lang/IllegalStateException" ||
               type == "java/lang/NullPointerException" ||
               type == "java/lang/IllegalArgumentException";
    }
    return false;
}

bool catches(const std::vector<std::string>& handlers, const std::string& type,
             std::string& handler, const ClassHierarchy* hierarchy)
{
    for (const auto& candidate : handlers) {
        bool subtype = candidate.empty() || candidate == type || isBuiltinSubtype(type, candidate);
        if (!subtype && hierarchy) {
            const auto child = hierarchy->indexOf(ca::str::Utf8StringRef::from_cstr(type.c_str()));
            const auto parent =
                hierarchy->indexOf(ca::str::Utf8StringRef::from_cstr(candidate.c_str()));
            subtype = child != INVALID_CLASS && parent != INVALID_CLASS &&
                      hierarchy->isSubType(child, parent);
        }
        if (subtype) {
            handler = candidate.empty() ? "java/lang/Throwable" : candidate;
            return true;
        }
    }
    return false;
}

struct ScannedMethod
{
    struct ThrowSite
    {
        std::string              type;
        std::vector<std::string> handlers;
    };

    struct CallSite
    {
        MethodRefKey             callee;
        std::vector<std::string> handlers;
    };

    MethodRefKey           method;
    std::vector<ThrowSite> localThrows;
    std::vector<CallSite>  calls;
};

std::vector<std::string> handlersAt(
    const MethodNode& method, ca::i32 instructionIndex,
    const std::unordered_map<jbytecode::core::Label*, ca::i32>& labelIndices)
{
    std::vector<std::string> handlers;
    for (auto* block : method.tryCatchBlocks) {
        if (!block) continue;
        const auto start = labelIndices.find(block->start);
        const auto end   = labelIndices.find(block->end);
        if (start == labelIndices.end() || end == labelIndices.end()) continue;
        if (instructionIndex >= start->second && instructionIndex < end->second) {
            appendUnique(handlers, block->type.to_std_string());
        }
    }
    return handlers;
}

ScannedMethod scan(const ClassNode& clazz, const MethodNode& method)
{
    ScannedMethod result;
    result.method = {
        clazz.name.to_std_string(), method.name.to_std_string(), method.desc.to_std_string()};
    std::unordered_map<jbytecode::core::Label*, ca::i32> labelIndices;
    const auto instructions = method.instructions.toArray();
    for (ca::i32 index = 0; index < static_cast<ca::i32>(instructions.size()); ++index) {
        auto* insn = instructions[static_cast<ca::usize>(index)];
        if (insn && insn->getType() == AbstractInsnNode::LABEL) {
            labelIndices[ca::core::tag_cast::cast<LabelNode>(insn)->label] = index;
        }
    }
    std::string lastAllocation;
    for (ca::i32 index = 0; index < static_cast<ca::i32>(instructions.size()); ++index) {
        auto* insn = instructions[static_cast<ca::usize>(index)];
        if (!insn) continue;
        if (insn->getOpcode() == Opcodes::NEW && insn->getType() == AbstractInsnNode::TYPE_INSN) {
            lastAllocation = ca::core::tag_cast::cast<TypeInsnNode>(insn)->desc.to_std_string();
        }
        else if (insn->getType() == AbstractInsnNode::METHOD_INSN) {
            auto* call = ca::core::tag_cast::cast<MethodInsnNode>(insn);
            result.calls.push_back({{call->owner.to_std_string(),
                                     call->name.to_std_string(),
                                     call->desc.to_std_string()},
                                    handlersAt(method, index, labelIndices)});
        }
        else if (insn->getOpcode() == Opcodes::ATHROW) {
            result.localThrows.push_back(
                {lastAllocation.empty() ? "java/lang/Throwable" : lastAllocation,
                 handlersAt(method, index, labelIndices)});
            lastAllocation.clear();
        }
    }
    return result;
}

}   //namespace

void ExceptionFlowModel::clear()
{
    summaries_.clear();
    byKey_.clear();
    edges_.clear();
}

void ExceptionFlowModel::build(ResourceContainer& container, ca::str::Utf8StringArena& arena)
{
    clear();
    ClassHierarchy hierarchy;
    hierarchy.build(container, arena);
    std::vector<ScannedMethod> methods;
    for (auto& [_, resource] : container.getResources()) {
        if (!resource || resource->getType() != ResourceType::Class) continue;
        auto* cls = ca::core::tag_cast::cast<ClassResource>(resource.get());
        if (!cls || cls->get().empty()) continue;
        try {
            ClassNode   clazz;
            ClassReader reader(arena, cls->get().data(), static_cast<ca::i32>(cls->get().size()));
            reader.accept(&clazz, ClassReader::SKIP_DEBUG | ClassReader::SKIP_FRAMES);
            for (auto* method : clazz.methods) {
                if (method) methods.push_back(scan(clazz, *method));
            }
        }
        catch (...) {
        }
    }

    std::unordered_map<std::string, std::vector<std::string>> uncaught;
    for (const auto& method : methods) {
        for (const auto& site : method.localThrows) {
            std::string handler;
            if (!catches(site.handlers, site.type, handler, &hierarchy))
                appendUnique(uncaught[key(method.method)], site.type);
        }
    }
    for (ca::usize round = 0; round < methods.size() + 1; ++round) {
        bool changed = false;
        for (const auto& method : methods) {
            auto& current = uncaught[key(method.method)];
            for (const auto& call : method.calls) {
                auto it = uncaught.find(key(call.callee));
                if (it == uncaught.end()) continue;
                for (const auto& type : it->second) {
                    std::string handler;
                    if (!catches(call.handlers, type, handler, &hierarchy)) {
                        const auto before = current.size();
                        appendUnique(current, type);
                        changed |= current.size() != before;
                    }
                }
            }
        }
        if (!changed) break;
    }

    for (const auto& method : methods) {
        ExceptionMethodSummary summary{method.method, uncaught[key(method.method)]};
        std::sort(summary.uncaughtTypes.begin(), summary.uncaughtTypes.end());
        byKey_[key(summary.method)] = summaries_.size();
        summaries_.push_back(std::move(summary));
        for (const auto& call : method.calls) {
            auto it = uncaught.find(key(call.callee));
            if (it == uncaught.end()) continue;
            for (const auto& type : it->second) {
                std::string handler;
                const bool  handled = catches(call.handlers, type, handler, &hierarchy);
                edges_.push_back({method.method, call.callee, type, handler, handled});
            }
        }
    }
}

const ExceptionMethodSummary* ExceptionFlowModel::summaryOf(const std::string& owner,
                                                            const std::string& name,
                                                            const std::string& desc) const
{
    auto it = byKey_.find(key({owner, name, desc}));
    return it == byKey_.end() ? nullptr : &summaries_[it->second];
}

ExceptionFlowAnalysis::Result ExceptionFlowAnalysis::run(ResourceContainer&     container,
                                                         GlobalAnalysisManager& gam)
{
    ExceptionFlowModel model;
    model.build(container, gam.arena());
    return model;
}

}   //namespace mjt::base
