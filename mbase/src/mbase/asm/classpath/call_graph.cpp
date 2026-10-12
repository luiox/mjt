#include "mbase/asm/classpath/call_graph.hpp"

#include "jbytecode/jbytecode.hpp"
#include "mbase/model/resource_model.hpp"
#include "mbase/reference/reference_index_analysis.hpp"
#include "libca/core/tag_cast.hpp"

using ca::str::Utf8StringRef;
using jbytecode::core::ClassReader;
using jbytecode::tree::AbstractInsnNode;
using jbytecode::tree::ClassNode;
using jbytecode::tree::MethodInsnNode;

namespace mjt::base {
namespace {

static Utf8StringRef refFromString(const std::string& value)
{
    return Utf8StringRef::from_cstr(value.c_str());
}

static void addEdge(CallGraphNode* caller, CallGraphNode* callee)
{
    if (!caller || !callee) return;
    caller->addCaller(callee);
    callee->addCallee(caller);
    callee->addIncomingCallSite();
}

static const std::unordered_set<CallGraphNode*>& emptyNodeSet()
{
    static const std::unordered_set<CallGraphNode*> empty;
    return empty;
}

}   //namespace

CallGraphNode* CallGraph::addMethod(const Utf8StringRef& owner, const Utf8StringRef& name,
                                    const Utf8StringRef& desc, bool declared, ca::i32 access)
{
    Utf8StringRef o = arena_.intern(owner);
    Utf8StringRef n = arena_.intern(name);
    Utf8StringRef d = arena_.intern(desc);
    Key           key(o, n, d);
    auto          it = methods_.find(key);
    if (it != methods_.end()) {
        if (declared) it->second.markDeclared(access);
        return &it->second;
    }
    auto res = methods_.emplace(key, CallGraphNode(o, n, d));
    if (declared) res.first->second.markDeclared(access);
    return &res.first->second;
}

CallGraphNode* CallGraph::getMethod(const Utf8StringRef& owner, const Utf8StringRef& name,
                                    const Utf8StringRef& desc)
{
    auto it = methods_.find(Key(owner, name, desc));
    return it == methods_.end() ? nullptr : &it->second;
}

const CallGraphNode* CallGraph::getMethod(const Utf8StringRef& owner, const Utf8StringRef& name,
                                          const Utf8StringRef& desc) const
{
    auto it = methods_.find(Key(owner, name, desc));
    return it == methods_.end() ? nullptr : &it->second;
}

CallGraphNode* CallGraph::getMethod(const std::string& owner, const std::string& name,
                                    const std::string& desc)
{
    return getMethod(refFromString(owner), refFromString(name), refFromString(desc));
}

const CallGraphNode* CallGraph::getMethod(const std::string& owner, const std::string& name,
                                          const std::string& desc) const
{
    return getMethod(refFromString(owner), refFromString(name), refFromString(desc));
}

const std::unordered_set<CallGraphNode*>& CallGraph::outgoingCallsOf(const std::string& owner,
                                                                     const std::string& name,
                                                                     const std::string& desc) const
{
    return outgoingCallsOf(refFromString(owner), refFromString(name), refFromString(desc));
}

const std::unordered_set<CallGraphNode*>& CallGraph::outgoingCallsOf(
    const Utf8StringRef& owner, const Utf8StringRef& name, const Utf8StringRef& desc) const
{
    const CallGraphNode* node = getMethod(owner, name, desc);
    return node ? node->getOutgoingCalls() : emptyNodeSet();
}

const std::unordered_set<CallGraphNode*>& CallGraph::incomingCallsOf(const std::string& owner,
                                                                     const std::string& name,
                                                                     const std::string& desc) const
{
    return incomingCallsOf(refFromString(owner), refFromString(name), refFromString(desc));
}

const std::unordered_set<CallGraphNode*>& CallGraph::incomingCallsOf(
    const Utf8StringRef& owner, const Utf8StringRef& name, const Utf8StringRef& desc) const
{
    const CallGraphNode* node = getMethod(owner, name, desc);
    return node ? node->getIncomingCalls() : emptyNodeSet();
}

ca::i32 CallGraph::callSiteCountOf(const std::string& owner, const std::string& name,
                                   const std::string& desc) const
{
    return callSiteCountOf(refFromString(owner), refFromString(name), refFromString(desc));
}

ca::i32 CallGraph::callSiteCountOf(const Utf8StringRef& owner, const Utf8StringRef& name,
                                   const Utf8StringRef& desc) const
{
    const CallGraphNode* node = getMethod(owner, name, desc);
    return node ? node->incomingCallSiteCount() : 0;
}

std::vector<const CallGraphNode*> CallGraph::declaredMethods() const
{
    std::vector<const CallGraphNode*> nodes;
    for (const auto& [key, node] : methods_) {
        (void)key;
        if (node.isDeclared()) nodes.push_back(&node);
    }
    return nodes;
}

std::vector<const CallGraphNode*> CallGraph::externalMethods() const
{
    std::vector<const CallGraphNode*> nodes;
    for (const auto& [key, node] : methods_) {
        (void)key;
        if (!node.isDeclared()) nodes.push_back(&node);
    }
    return nodes;
}

std::vector<const CallGraphNode*> CallGraph::methodsOwnedBy(const std::string& owner) const
{
    return methodsOwnedBy(refFromString(owner));
}

std::vector<const CallGraphNode*> CallGraph::methodsOwnedBy(const Utf8StringRef& owner) const
{
    std::vector<const CallGraphNode*> nodes;
    for (const auto& [key, node] : methods_) {
        (void)key;
        if (node.getOwner() == owner) nodes.push_back(&node);
    }
    return nodes;
}

std::vector<const CallGraphNode*> CallGraph::reachableFrom(const std::string& owner,
                                                           const std::string& name,
                                                           const std::string& desc) const
{
    return reachableFrom(refFromString(owner), refFromString(name), refFromString(desc));
}

std::vector<const CallGraphNode*> CallGraph::reachableFrom(const Utf8StringRef& owner,
                                                           const Utf8StringRef& name,
                                                           const Utf8StringRef& desc) const
{
    const CallGraphNode* start = getMethod(owner, name, desc);
    if (!start) return {};

    std::vector<const CallGraphNode*>        result;
    std::vector<const CallGraphNode*>        stack;
    std::unordered_set<const CallGraphNode*> seen;
    seen.insert(start);
    stack.push_back(start);

    while (!stack.empty()) {
        const CallGraphNode* node = stack.back();
        stack.pop_back();
        for (auto* next : node->getOutgoingCalls()) {
            if (!next || seen.find(next) != seen.end()) continue;
            seen.insert(next);
            result.push_back(next);
            stack.push_back(next);
        }
    }
    return result;
}

std::vector<const CallGraphNode*> CallGraph::reverseReachableTo(const std::string& owner,
                                                                const std::string& name,
                                                                const std::string& desc) const
{
    return reverseReachableTo(refFromString(owner), refFromString(name), refFromString(desc));
}

std::vector<const CallGraphNode*> CallGraph::reverseReachableTo(const Utf8StringRef& owner,
                                                                const Utf8StringRef& name,
                                                                const Utf8StringRef& desc) const
{
    const CallGraphNode* start = getMethod(owner, name, desc);
    if (!start) return {};

    std::vector<const CallGraphNode*>        result;
    std::vector<const CallGraphNode*>        stack;
    std::unordered_set<const CallGraphNode*> seen;
    seen.insert(start);
    stack.push_back(start);

    while (!stack.empty()) {
        const CallGraphNode* node = stack.back();
        stack.pop_back();
        for (auto* prev : node->getIncomingCalls()) {
            if (!prev || seen.find(prev) != seen.end()) continue;
            seen.insert(prev);
            result.push_back(prev);
            stack.push_back(prev);
        }
    }
    return result;
}

bool CallGraph::hasPath(const std::string& sourceOwner, const std::string& sourceName,
                        const std::string& sourceDesc, const std::string& targetOwner,
                        const std::string& targetName, const std::string& targetDesc) const
{
    return hasPath(refFromString(sourceOwner),
                   refFromString(sourceName),
                   refFromString(sourceDesc),
                   refFromString(targetOwner),
                   refFromString(targetName),
                   refFromString(targetDesc));
}

bool CallGraph::hasPath(const Utf8StringRef& sourceOwner, const Utf8StringRef& sourceName,
                        const Utf8StringRef& sourceDesc, const Utf8StringRef& targetOwner,
                        const Utf8StringRef& targetName, const Utf8StringRef& targetDesc) const
{
    const CallGraphNode* source = getMethod(sourceOwner, sourceName, sourceDesc);
    const CallGraphNode* target = getMethod(targetOwner, targetName, targetDesc);
    if (!source || !target) return false;

    std::vector<const CallGraphNode*>        stack;
    std::unordered_set<const CallGraphNode*> seen;
    seen.insert(source);
    stack.push_back(source);

    while (!stack.empty()) {
        const CallGraphNode* node = stack.back();
        stack.pop_back();
        for (auto* next : node->getOutgoingCalls()) {
            if (!next) continue;
            if (next == target) return true;
            if (seen.find(next) != seen.end()) continue;
            seen.insert(next);
            stack.push_back(next);
        }
    }
    return false;
}

void CallGraph::clear()
{
    methods_.clear();
    arena_.clear();
}

void CallGraph::buildGraph(const std::vector<std::vector<ca::u8>>& classBytesList,
                           const CallGraphOptions&                 options)
{
    clear();
    for (const auto& classBytes : classBytesList) {
        if (classBytes.empty()) continue;
        ClassNode   classNode;
        ClassReader reader(arena_, classBytes);
        reader.accept(&classNode, ClassReader::SKIP_DEBUG | ClassReader::SKIP_FRAMES);
        for (auto* method : classNode.methods) {
            addMethod(classNode.name, method->name, method->desc, true, method->access);
        }
    }

    for (const auto& classBytes : classBytesList) {
        if (classBytes.empty()) continue;
        ClassNode   classNode;
        ClassReader reader(arena_, classBytes);
        reader.accept(&classNode, ClassReader::SKIP_DEBUG | ClassReader::SKIP_FRAMES);

        for (auto* method : classNode.methods) {
            if (method->instructions.size() == 0) continue;
            CallGraphNode* methodNode = getMethod(classNode.name, method->name, method->desc);
            if (!methodNode) continue;

            for (auto* insn = method->instructions.getFirst(); insn; insn = insn->getNext()) {
                if (insn->getType() != AbstractInsnNode::METHOD_INSN) continue;
                auto*          mi     = ca::core::tag_cast::cast<MethodInsnNode>(insn);
                CallGraphNode* called = getMethod(mi->owner, mi->name, mi->desc);
                if (!called && options.includeExternalTargets) {
                    called = addMethod(mi->owner, mi->name, mi->desc);
                }
                if (!called) continue;
                addEdge(methodNode, called);
            }
        }
    }
}

void CallGraph::build(ResourceContainer& container, ca::str::Utf8StringArena& arena,
                      const CallGraphOptions& options)
{
    ReferenceIndex index;
    index.build(container, arena);
    buildFromIndex(index, options);
}

void CallGraph::buildFromIndex(const ReferenceIndex& index, const CallGraphOptions& options)
{
    clear();

    for (const auto& method : index.methodDeclarations()) {
        addMethod(refFromString(method.method.owner),
                  refFromString(method.method.name),
                  refFromString(method.method.desc),
                  true,
                  method.access);
    }

    for (const auto& ref : index.methodReferences()) {
        if (ref.kind == MethodReferenceKind::Reflection) continue;
        if (ref.kind == MethodReferenceKind::ReflectionExecution && ref.target.desc.empty())
            continue;

        CallGraphNode* caller =
            getMethod(ref.site.sourceClass, ref.site.sourceMethod, ref.site.sourceDesc);
        if (!caller) {
            caller = addMethod(refFromString(ref.site.sourceClass),
                               refFromString(ref.site.sourceMethod),
                               refFromString(ref.site.sourceDesc));
        }

        CallGraphNode* callee = getMethod(ref.target.owner, ref.target.name, ref.target.desc);
        if (!callee && options.includeExternalTargets) {
            callee = addMethod(refFromString(ref.target.owner),
                               refFromString(ref.target.name),
                               refFromString(ref.target.desc));
        }
        addEdge(caller, callee);
    }
}

CallGraphAnalysis::Result CallGraphAnalysis::run(ResourceContainer&     container,
                                                 GlobalAnalysisManager& gam)
{
    auto&     index = gam.getResult<ReferenceIndexAnalysis>(container);
    CallGraph graph;
    graph.buildFromIndex(index);
    return graph;
}

}   //namespace mjt::base
