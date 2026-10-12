#include "mbase/analysis/alias_analysis.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

#include "jbytecode/jbytecode.hpp"
#include "jbytecode/analysis/basic_interpreter.hpp"
#include "mbase/asm/analyzer/dataflow_analyzer.hpp"
#include "mbase/spi/pass_pipeline.hpp"
#include "mbase/model/resource_model.hpp"
#include "libca/core/tag_cast.hpp"

namespace mjt::base {

namespace {

using namespace jbytecode::core;
using namespace jbytecode::tree;

std::string text(ca::str::Utf8StringRef value)
{
    return value.to_std_string();
}

bool isReferenceType(const Type& type)
{
    return type.getSort() == Type::OBJECT || type.getSort() == Type::ARRAY;
}

bool isContainerType(const Type& type)
{
    if (type.getSort() != Type::OBJECT) return false;
    //staticArena 退役：仅瞬时比对，函数级局部 arena 零驻留（OBJECT 排序
    //getInternalName 本不 intern，落点参数按必选口径补齐）。
    ca::str::Utf8StringArena scratch;
    const auto               name = type.getInternalName(scratch).to_std_string();
    return name == "java/util/Collection" || name == "java/util/List" || name == "java/util/Set" ||
           name == "java/util/Deque" || name == "java/util/Map" || name == "java/util/Queue" ||
           name == "java/util/ArrayList" || name == "java/util/LinkedList" ||
           name == "java/util/Vector" || name == "java/util/Stack" ||
           name == "java/util/ArrayDeque" || name == "java/util/PriorityQueue" ||
           name == "java/util/HashMap" || name == "java/util/LinkedHashMap" ||
           name == "java/util/TreeMap" || name == "java/util/WeakHashMap" ||
           name == "java/util/IdentityHashMap" ||
           name == "java/util/concurrent/ConcurrentHashMap" ||
           name == "java/util/concurrent/CopyOnWriteArrayList" ||
           name == "java/util/concurrent/LinkedBlockingQueue";
}

bool isMapOwner(const std::string& owner)
{
    return owner == "java/util/Map" || owner == "java/util/HashMap" ||
           owner == "java/util/LinkedHashMap" || owner == "java/util/TreeMap" ||
           owner == "java/util/WeakHashMap" || owner == "java/util/IdentityHashMap" ||
           owner == "java/util/concurrent/ConcurrentHashMap";
}

bool isListOwner(const std::string& owner)
{
    return owner == "java/util/List" || owner == "java/util/Collection" ||
           owner == "java/util/ArrayList" || owner == "java/util/LinkedList" ||
           owner == "java/util/Vector" || owner == "java/util/Stack" ||
           owner == "java/util/concurrent/CopyOnWriteArrayList";
}

bool isQueueOwner(const std::string& owner)
{
    return owner == "java/util/Queue" || owner == "java/util/Deque" ||
           owner == "java/util/ArrayDeque" || owner == "java/util/PriorityQueue" ||
           owner == "java/util/concurrent/LinkedBlockingQueue";
}

bool isReflectionLookup(const std::string& owner, const std::string& name)
{
    if (owner != "java/lang/Class") return false;
    return name == "getMethod" || name == "getDeclaredMethod" || name == "getConstructor" ||
           name == "getDeclaredConstructor" || name == "getField" || name == "getDeclaredField";
}

AliasValue primitiveAlias(std::string identity)
{
    AliasValue value;
    value.kind     = AliasValueKind::Primitive;
    value.identity = aliasInterned(identity);
    return value;
}

AliasValue valueForType(const Type& type, std::string identity)
{
    if (type.getSort() == Type::ARRAY) return AliasValue::array(std::move(identity));
    if (isContainerType(type)) return AliasValue::container(std::move(identity));
    if (type.getSort() == Type::OBJECT) return AliasValue::object(std::move(identity));
    AliasValue primitive;
    primitive.kind     = AliasValueKind::Primitive;
    primitive.identity = aliasInterned(identity);
    return primitive;
}

AliasValue popAlias(std::vector<AliasValue>& stack)
{
    if (stack.empty()) return AliasValue::unknown();
    auto value = stack.back();
    stack.pop_back();
    return value;
}

std::vector<AliasValue> takeArguments(std::vector<AliasValue>& stack, const std::vector<Type>& args)
{
    std::vector<AliasValue> values(args.size());
    for (ca::usize i = args.size(); i > 0; --i) {
        values[i - 1] = popAlias(stack);
    }
    return values;
}

ca::i32 parameterIdentityIndex(const AliasValue& value)
{
    constexpr const char* prefix = "param:";
    if (value.identity->compare(0, 6, prefix) != 0) return -1;
    try {
        return std::stoi(value.identity->substr(6));
    }
    catch (...) {
        return -1;
    }
}

AliasValue instantiateReturn(const AliasMethodSummary*      summary,
                             const std::vector<AliasValue>& arguments, const AliasValue& receiver,
                             const Type& returnType)
{
    if (!summary) return AliasValue::unknown();
    if (!summary->returnValue.isKnown()) return summary->returnValue;
    const auto parameterIndex = parameterIdentityIndex(summary->returnValue);
    if (parameterIndex >= 0 && static_cast<ca::usize>(parameterIndex) < arguments.size()) {
        return arguments[static_cast<ca::usize>(parameterIndex)];
    }
    if (summary->returnValue.identity->compare(0, 5, "this:") == 0) return receiver;
    return valueForType(returnType, *summary->returnValue.identity);
}

AliasValue instantiateFact(const AliasValue& value, const std::vector<AliasValue>& arguments,
                           const AliasValue& receiver)
{
    if (!value.isKnown()) return value;
    const auto parameterIndex = parameterIdentityIndex(value);
    if (parameterIndex >= 0 && static_cast<ca::usize>(parameterIndex) < arguments.size()) {
        return arguments[static_cast<ca::usize>(parameterIndex)];
    }
    if (value.identity->compare(0, 5, "this:") == 0) return receiver;
    return value;
}

std::string fieldKey(const FieldRefKey& field)
{
    return field.owner + '\n' + field.name + '\n' + field.desc;
}

std::string fieldKey(const AliasValue& receiver, const FieldRefKey& field)
{
    return *receiver.identity + '\n' + fieldKey(field);
}

std::string arrayKey(const AliasValue& array, const AliasValue& index)
{
    return *array.identity + '\n' + (index.isKnown() ? *index.identity : "*");
}

std::string containerKey(const AliasValue& container, const AliasValue& key)
{
    return *container.identity + '\n' + (key.isKnown() ? *key.identity : "*");
}

bool isFormalIdentity(const AliasValue& value)
{
    return value.isKnown() && (value.identity->compare(0, 6, "param:") == 0 ||
                               value.identity->compare(0, 5, "this:") == 0);
}

struct FlowAliasValue
{
    jbytecode::analysis::BasicValue basic;
    AliasValue                      alias;

    int  getSize() const { return basic.getSize(); }
    bool operator==(const FlowAliasValue& other) const
    {
        return basic == other.basic && alias.kind == other.alias.kind &&
               alias.identity == other.alias.identity && alias.pointsTo == other.alias.pointsTo;
    }
};

class AliasFlowInterpreter final : public jbytecode::analysis::Interpreter<FlowAliasValue>
{
public:
    AliasFlowInterpreter(const ClassNode& clazz, const MethodNode& method,
                         const AliasAnalysis* previous)
        : owner_(text(clazz.name))
        , methodName_(text(method.name))
        , methodDesc_(text(method.desc))
        , previous_(previous)
    {
        ca::i32 local     = (method.access & Opcodes::ACC_STATIC) == 0 ? 1 : 0;
        ca::i32 parameter = 0;
        for (const auto& type : Type::getArgumentTypes(method.desc)) {
            parameters_[local] = parameter++;
            local += type.getSize();
        }
        ca::i32 index = 0;
        for (auto* insn = method.instructions.getFirst(); insn; insn = insn->getNext()) {
            instructionIndices_[insn] = index++;
        }
    }

    FlowAliasValue newValue(const Type* type) override
    {
        return {basic_.newValue(type), AliasValue::unknown()};
    }

    FlowAliasValue newParameterValue(bool isInstanceMethod, int local, const Type& type) override
    {
        AliasValue alias = AliasValue::unknown();
        if (isInstanceMethod && local == 0) {
            alias = AliasValue::object("this:" + owner_);
        }
        else if (const auto it = parameters_.find(local); it != parameters_.end()) {
            alias = valueForType(type, "param:" + std::to_string(it->second));
        }
        return {basic_.newValue(&type), std::move(alias)};
    }

    FlowAliasValue newReturnTypeValue(const Type& type) override
    {
        return {basic_.newValue(&type), AliasValue::unknown()};
    }

    FlowAliasValue newEmptyValue(int local) override
    {
        (void)local;
        return {basic_.newValue(nullptr), AliasValue::unknown()};
    }

    FlowAliasValue newExceptionValue(const TryCatchBlockNode*                          block,
                                     const jbytecode::analysis::Frame<FlowAliasValue>* frame,
                                     const Type& type) override
    {
        (void)block;
        (void)frame;
        return {basic_.newValue(&type), AliasValue::unknown()};
    }

    FlowAliasValue newOperation(const AbstractInsnNode* insn) override
    {
        auto basic = basic_.newOperation(insn);
        switch (insn->getOpcode()) {
        case Opcodes::ACONST_NULL: return {basic, AliasValue::nullValue()};
        case Opcodes::NEW:
        {
            const auto* type = ca::core::tag_cast::cast<const TypeInsnNode>(insn);
            return {basic,
                    AliasValue::object("new:" + owner_ + "." + methodName_ + methodDesc_ + ":" +
                                       std::to_string(instructionIndices_.at(insn)) + ":" +
                                       text(type->desc))};
        }
        case Opcodes::GETSTATIC:
        {
            const auto*       field = ca::core::tag_cast::cast<const FieldInsnNode>(insn);
            const auto        type  = Type::getType(field->desc);
            const AliasValue* prior =
                previous_ ? previous_->fieldValueOf(
                                text(field->owner), text(field->name), text(field->desc))
                          : nullptr;
            return {basic,
                    prior ? *prior
                          : valueForType(type,
                                         "field:" + text(field->owner) + "." + text(field->name) +
                                             ":" + text(field->desc))};
        }
        case Opcodes::LDC:
        {
            const auto* ldc = ca::core::tag_cast::cast<const LdcInsnNode>(insn);
            if (auto* value = ldc->cst.as<std::string>()) {
                return {basic, primitiveAlias("string:" + *value)};
            }
            if (auto* value = ldc->cst.as<ca::str::Utf8StringRef>()) {
                return {basic, primitiveAlias("string:" + value->to_std_string())};
            }
            if (auto* type = ldc->cst.as<Type>()) {
                if (type->getSort() == Type::OBJECT || type->getSort() == Type::ARRAY) {
                    //staticArena 退役：仅瞬时拼串，函数级局部 arena 零驻留。
                    ca::str::Utf8StringArena scratch;
                    return {basic,
                            AliasValue::object("class:" +
                                               type->getInternalName(scratch).to_std_string())};
                }
            }
            return {basic, AliasValue::unknown()};
        }
        default: return {basic, AliasValue::unknown()};
        }
    }

    FlowAliasValue copyOperation(const AbstractInsnNode* insn, const FlowAliasValue& value) override
    {
        return {basic_.copyOperation(insn, value.basic), value.alias};
    }

    FlowAliasValue unaryOperation(const AbstractInsnNode* insn,
                                  const FlowAliasValue&   value) override
    {
        auto basic = basic_.unaryOperation(insn, value.basic);
        if (insn->getOpcode() == Opcodes::CHECKCAST) return {basic, value.alias};
        if (insn->getOpcode() == Opcodes::GETFIELD) {
            const auto* field = ca::core::tag_cast::cast<const FieldInsnNode>(insn);
            const auto  type  = Type::getType(field->desc);
            const auto  prior =
                previous_
                     ? previous_->resolvedFieldValueOf(
                          value.alias, text(field->owner), text(field->name), text(field->desc))
                     : std::nullopt;
            return {basic,
                    prior ? *prior
                          : valueForType(type,
                                         "field:" + text(field->owner) + "." + text(field->name) +
                                             ":" + text(field->desc))};
        }
        return {basic, AliasValue::unknown()};
    }

    FlowAliasValue binaryOperation(const AbstractInsnNode* insn, const FlowAliasValue& lhs,
                                   const FlowAliasValue& rhs) override
    {
        return {basic_.binaryOperation(insn, lhs.basic, rhs.basic), AliasValue::unknown()};
    }

    FlowAliasValue ternaryOperation(const AbstractInsnNode* insn, const FlowAliasValue& first,
                                    const FlowAliasValue& second,
                                    const FlowAliasValue& third) override
    {
        return {basic_.ternaryOperation(insn, first.basic, second.basic, third.basic),
                AliasValue::unknown()};
    }

    FlowAliasValue naryOperation(const AbstractInsnNode*            insn,
                                 const std::vector<FlowAliasValue>& values) override
    {
        std::vector<jbytecode::analysis::BasicValue> basics;
        basics.reserve(values.size());
        for (const auto& value : values) basics.push_back(value.basic);
        auto basic = basic_.naryOperation(insn, basics);
        if (insn->getType() != AbstractInsnNode::METHOD_INSN) {
            return {basic, AliasValue::unknown()};
        }
        const auto*             call     = ca::core::tag_cast::cast<const MethodInsnNode>(insn);
        const bool              isStatic = call->getOpcode() == Opcodes::INVOKESTATIC;
        const auto              argTypes = Type::getArgumentTypes(call->desc);
        std::vector<AliasValue> arguments;
        arguments.reserve(argTypes.size());
        const ca::usize offset = isStatic ? 0 : 1;
        for (ca::usize i = 0; i < argTypes.size(); ++i) {
            arguments.push_back(offset + i < values.size() ? values[offset + i].alias
                                                           : AliasValue::unknown());
        }
        const AliasValue receiver =
            !isStatic && !values.empty() ? values.front().alias : AliasValue::unknown();
        const auto* summary =
            previous_ ? previous_->summaryOf(text(call->owner), text(call->name), text(call->desc))
                      : nullptr;
        return {basic,
                instantiateReturn(summary, arguments, receiver, Type::getReturnType(call->desc))};
    }

    void returnOperation(const AbstractInsnNode* insn, const FlowAliasValue& value,
                         const FlowAliasValue& expected) override
    {
        basic_.returnOperation(insn, value.basic, expected.basic);
        returnValue_ = sawReturn_ ? mergeAliasValues(returnValue_, value.alias) : value.alias;
        sawReturn_   = true;
    }

    FlowAliasValue merge(const FlowAliasValue& lhs, const FlowAliasValue& rhs) override
    {
        return {basic_.merge(lhs.basic, rhs.basic), mergeAliasValues(lhs.alias, rhs.alias)};
    }

    std::optional<AliasValue> returnValue() const
    {
        return sawReturn_ ? std::optional<AliasValue>(returnValue_) : std::nullopt;
    }

private:
    jbytecode::analysis::BasicInterpreter                basic_;
    std::string                                          owner_;
    std::string                                          methodName_;
    std::string                                          methodDesc_;
    const AliasAnalysis*                                 previous_ = nullptr;
    std::unordered_map<ca::i32, ca::i32>                 parameters_;
    std::unordered_map<const AbstractInsnNode*, ca::i32> instructionIndices_;
    AliasValue                                           returnValue_;
    bool                                                 sawReturn_ = false;
};

struct ConstraintValue
{
    jbytecode::analysis::BasicValue    basic;
    AndersenPointsToSolver::NameId     node = 0;

    int  getSize() const { return basic.getSize(); }
    bool operator==(const ConstraintValue& other) const
    {
        return basic == other.basic && node == other.node;
    }
};

class AndersenConstraintInterpreter final : public jbytecode::analysis::Interpreter<ConstraintValue>
{
public:
    AndersenConstraintInterpreter(const ClassNode& clazz, const MethodNode& method,
                                  AndersenPointsToSolver& solver)
        : method_(text(clazz.name) + "\n" + text(method.name) + "\n" + text(method.desc))
        , methodId_(solver.intern(method_))
        , solver_(solver)
    {
        ca::i32 local     = (method.access & Opcodes::ACC_STATIC) == 0 ? 1 : 0;
        ca::i32 parameter = 0;
        for (const auto& type : Type::getArgumentTypes(method.desc)) {
            parameters_[local] = parameter++;
            local += type.getSize();
        }
        ca::i32 index = 0;
        for (auto* insn = method.instructions.getFirst(); insn; insn = insn->getNext()) {
            instructionIndices_[insn] = index++;
        }
    }

    ConstraintValue newValue(const Type* type) override { return {basic_.newValue(type), {}}; }

    ConstraintValue newParameterValue(bool isInstanceMethod, int local, const Type& type) override
    {
        AndersenPointsToSolver::NameId node = 0;
        if (isInstanceMethod && local == 0) {
            node = thisNodeId();
        }
        else if (const auto found = parameters_.find(local); found != parameters_.end()) {
            node = paramNodeId(found->second);
        }
        return {basic_.newValue(&type), node};
    }

    ConstraintValue newReturnTypeValue(const Type& type) override
    {
        return {basic_.newValue(&type), {}};
    }

    ConstraintValue newEmptyValue(int local) override
    {
        (void)local;
        return {basic_.newValue(nullptr), {}};
    }

    ConstraintValue newExceptionValue(const TryCatchBlockNode*                           block,
                                      const jbytecode::analysis::Frame<ConstraintValue>* frame,
                                      const Type& type) override
    {
        (void)block;
        (void)frame;
        return {basic_.newValue(&type), {}};
    }

    ConstraintValue newOperation(const AbstractInsnNode* insn) override
    {
        auto       basic  = basic_.newOperation(insn);
        const auto opcode = insn->getOpcode();
        if (opcode == Opcodes::NEW) {
            const auto  node = temporary(insn);
            const auto* type = ca::core::tag_cast::cast<const TypeInsnNode>(insn);
            //P3：分配点名物化一次并内码（KB 级串不再逐约束重建）。
            thread_local std::string alloc;
            alloc.assign("new:").append(method_).append(1, ':').append(
                std::to_string(instructionIndices_.at(insn))).append(1, ':').append(text(type->desc));
            solver_.addAddressOf(node, solver_.intern(alloc));
            return {basic, node};
        }
        if (opcode == Opcodes::GETSTATIC) {
            const auto* field = ca::core::tag_cast::cast<const FieldInsnNode>(insn);
            return {basic, staticField(*field)};
        }
        return {basic, {}};
    }

    ConstraintValue copyOperation(const AbstractInsnNode* insn,
                                  const ConstraintValue&  value) override
    {
        return {basic_.copyOperation(insn, value.basic), value.node};
    }

    ConstraintValue unaryOperation(const AbstractInsnNode* insn,
                                   const ConstraintValue&  value) override
    {
        auto basic = basic_.unaryOperation(insn, value.basic);
        if (insn->getOpcode() == Opcodes::CHECKCAST) return {basic, value.node};
        if (insn->getOpcode() == Opcodes::PUTSTATIC && value.node != 0) {
            solver_.addCopy(staticField(*ca::core::tag_cast::cast<const FieldInsnNode>(insn)),
                            value.node);
            return {basic, {}};
        }
        if (insn->getOpcode() == Opcodes::GETFIELD && value.node != 0) {
            const auto* field = ca::core::tag_cast::cast<const FieldInsnNode>(insn);
            const auto  node  = temporary(insn);
            solver_.addLoad(node, value.node, fieldName(*field));
            return {basic, node};
        }
        if (insn->getOpcode() == Opcodes::ANEWARRAY || insn->getOpcode() == Opcodes::NEWARRAY) {
            const auto node = temporary(insn);
            solver_.addAddressOf(
                node, solver_.internIndexed("array:" + method_ + ":", instructionIndices_.at(insn)));
            return {basic, node};
        }
        return {basic, {}};
    }

    ConstraintValue binaryOperation(const AbstractInsnNode* insn, const ConstraintValue& lhs,
                                    const ConstraintValue& rhs) override
    {
        auto basic = basic_.binaryOperation(insn, lhs.basic, rhs.basic);
        if (insn->getOpcode() == Opcodes::PUTFIELD && lhs.node != 0 && rhs.node != 0) {
            solver_.addStore(
                lhs.node, fieldName(*ca::core::tag_cast::cast<const FieldInsnNode>(insn)), rhs.node);
        }
        if (insn->getOpcode() == Opcodes::AALOAD && lhs.node != 0) {
            const auto node = temporary(insn);
            solver_.addLoad(node, lhs.node, arrayElementFieldId());
            return {basic, node};
        }
        return {basic, {}};
    }

    ConstraintValue ternaryOperation(const AbstractInsnNode* insn, const ConstraintValue& first,
                                     const ConstraintValue& second,
                                     const ConstraintValue& third) override
    {
        auto basic = basic_.ternaryOperation(insn, first.basic, second.basic, third.basic);
        if (insn->getOpcode() == Opcodes::AASTORE && first.node != 0 && third.node != 0) {
            solver_.addStore(first.node, arrayElementFieldId(), third.node);
        }
        return {basic, {}};
    }

    ConstraintValue naryOperation(const AbstractInsnNode*             insn,
                                  const std::vector<ConstraintValue>& values) override
    {
        std::vector<jbytecode::analysis::BasicValue> basics;
        basics.reserve(values.size());
        for (const auto& value : values) basics.push_back(value.basic);
        auto basic = basic_.naryOperation(insn, basics);
        if (insn->getOpcode() == Opcodes::MULTIANEWARRAY) {
            const auto node = temporary(insn);
            solver_.addAddressOf(
                node, solver_.internIndexed("array:" + method_ + ":", instructionIndices_.at(insn)));
            return {basic, node};
        }
        if (insn->getType() != AbstractInsnNode::METHOD_INSN) return {basic, {}};

        const auto* call      = ca::core::tag_cast::cast<const MethodInsnNode>(insn);
        const auto  calleeKey = text(call->owner) + "\n" + text(call->name) + "\n" + text(call->desc);
        const auto  calleeId  = solver_.intern(calleeKey);
        const bool  isStatic  = call->getOpcode() == Opcodes::INVOKESTATIC;
        const auto  offset    = isStatic ? 0u : 1u;
        if (!isStatic && !values.empty() && values.front().node != 0) {
            solver_.addCopy(calleeThisId(calleeKey), values.front().node);
        }
        const auto arguments = Type::getArgumentTypes(call->desc);
        for (ca::usize index = 0; index < arguments.size(); ++index) {
            if (offset + index >= values.size() || values[offset + index].node == 0) continue;
            solver_.addCopy(calleeParamId(calleeKey, calleeId, index), values[offset + index].node);
        }
        if (Type::getReturnType(call->desc).getSort() == Type::VOID) return {basic, {}};
        const auto node = temporary(insn);
        solver_.addCopy(node, calleeReturnId(calleeKey));
        return {basic, node};
    }

    void returnOperation(const AbstractInsnNode* insn, const ConstraintValue& value,
                         const ConstraintValue& expected) override
    {
        basic_.returnOperation(insn, value.basic, expected.basic);
        if (value.node != 0) solver_.addCopy(returnNodeId(), value.node);
    }

    ConstraintValue merge(const ConstraintValue& lhs, const ConstraintValue& rhs) override
    {
        auto basic = basic_.merge(lhs.basic, rhs.basic);
        if (lhs.node == 0) return {basic, rhs.node};
        if (rhs.node == 0) return {basic, lhs.node};
        if (lhs.node == rhs.node) return {basic, lhs.node};
        std::vector<AndersenPointsToSolver::NameId> sources;
        const auto appendSources = [&](AndersenPointsToSolver::NameId node) {
            const auto found = mergeSources_.find(node);
            if (found == mergeSources_.end()) {
                sources.push_back(node);
            }
            else {
                sources.insert(sources.end(), found->second.begin(), found->second.end());
            }
        };
        appendSources(lhs.node);
        appendSources(rhs.node);
        std::sort(sources.begin(), sources.end());
        sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
        std::string nodeText = method_ + "\nmerge";
        for (const auto source : sources) nodeText += "\n" + solver_.nameOf(source);
        const auto node = solver_.intern(nodeText);
        mergeSources_[node] = sources;
        for (const auto source : sources) solver_.addCopy(node, source);
        return {basic, node};
    }

private:
    AndersenPointsToSolver::NameId temporary(const AbstractInsnNode* insn)
    {
        return solver_.internIndexed(method_ + "\ninsn:", instructionIndices_.at(insn));
    }

    AndersenPointsToSolver::NameId thisNodeId()
    {
        if (thisNodeId_ == 0) thisNodeId_ = solver_.intern(method_ + "\nthis") + 1;
        return thisNodeId_ - 1;
    }

    AndersenPointsToSolver::NameId paramNodeId(ca::i32 parameter)
    {
        const auto found = paramNodeIds_.find(parameter);
        if (found != paramNodeIds_.end()) return found->second;
        const auto id =
            solver_.internIndexed(method_ + "\nparam:", parameter);
        paramNodeIds_.emplace(parameter, id);
        return id;
    }

    AndersenPointsToSolver::NameId returnNodeId()
    {
        if (returnNodeId_ == 0) returnNodeId_ = solver_.intern(method_ + "\nreturn") + 1;
        return returnNodeId_ - 1;
    }

    AndersenPointsToSolver::NameId calleeThisId(const std::string& calleeKey)
    {
        thread_local std::string scratch;
        scratch.assign(calleeKey).append("\nthis");
        return solver_.intern(scratch);
    }

    AndersenPointsToSolver::NameId calleeParamId(const std::string& calleeKey,
                                                 AndersenPointsToSolver::NameId,
                                                 ca::usize                 index)
    {
        return solver_.internIndexed(calleeKey + "\nparam:", static_cast<ca::i64>(index));
    }

    AndersenPointsToSolver::NameId calleeReturnId(const std::string& calleeKey)
    {
        thread_local std::string scratch;
        scratch.assign(calleeKey).append("\nreturn");
        return solver_.intern(scratch);
    }

    AndersenPointsToSolver::NameId fieldName(const FieldInsnNode& field)
    {
        thread_local std::string scratch;
        scratch.assign(text(field.owner)).append(1, '.').append(text(field.name)).append(1, ':')
            .append(text(field.desc));
        return solver_.intern(scratch);
    }

    AndersenPointsToSolver::NameId staticField(const FieldInsnNode& field)
    {
        thread_local std::string scratch;
        scratch.assign("static-field\n").append(text(field.owner)).append(1, '.')
            .append(text(field.name)).append(1, ':').append(text(field.desc));
        return solver_.intern(scratch);
    }

    AndersenPointsToSolver::NameId arrayElementFieldId()
    {
        if (arrayElementFieldId_ == 0) arrayElementFieldId_ = solver_.intern("[]") + 1;
        return arrayElementFieldId_ - 1;
    }

    jbytecode::analysis::BasicInterpreter                basic_;
    std::string                                          method_;
    AndersenPointsToSolver::NameId                       methodId_  = 0;
    AndersenPointsToSolver::NameId                       thisNodeId_       = 0;
    AndersenPointsToSolver::NameId                       returnNodeId_     = 0;
    AndersenPointsToSolver::NameId                       arrayElementFieldId_ = 0;
    AndersenPointsToSolver&                              solver_;
    std::unordered_map<ca::i32, ca::i32>                 parameters_;
    std::unordered_map<const AbstractInsnNode*, ca::i32> instructionIndices_;
    std::unordered_map<ca::i32, AndersenPointsToSolver::NameId> paramNodeIds_;
    std::unordered_map<AndersenPointsToSolver::NameId,
                       std::vector<AndersenPointsToSolver::NameId>> mergeSources_;
};

void indexAndersenBytecode(const std::vector<const ClassNode*>& classes,
                           AndersenPointsToSolver&              solver)
{
    for (const auto& clazz : classes) {
        for (auto* method : clazz->methods) {
            if (!method) continue;
            AndersenConstraintInterpreter interpreter(*clazz, *method, solver);
            //DataflowAnalyzer fail-open：畸形/JSR 方法不产出约束，与迁移前
            //catch (...)「Malformed methods add no constraints」语义一致。
            (void)analyzeFrames<ConstraintValue>(&interpreter, clazz->name, *method);
        }
    }
    solver.solve();
}

struct ControlFlowAliasFacts
{
    std::optional<AliasValue> returnValue;
    std::vector<AliasMethodSummary::FieldWrite> fieldWrites;

    //P2：flow 路径不再内联 callee 事实，改为按调用点采集 CFG 帧上的
    //receiver/arguments（fw σ），由 summarize 重写对应绑定的 FW 组；
    //snapshot 失败（fail-open）时只失效 FW 组，与 P1 逐组替换语义一致。
    struct CallFlowSigma
    {
        ca::i32    instructionIndex = -1;
        AliasValue receiver;
        std::vector<AliasValue> arguments;
        bool       eligible = false;
    };
    std::vector<CallFlowSigma> flowSigmas;
    bool                       snapshotFailed = false;
};

ControlFlowAliasFacts analyzeControlFlowFacts(const ClassNode& clazz, const MethodNode& method,
                                              const AliasAnalysis* previous)
{
    ControlFlowAliasFacts result;
    try {
        AliasFlowInterpreter interpreter(clazz, method, previous);
        auto                 snapshot =
            analyzeFrames<FlowAliasValue>(&interpreter, clazz.name,
                                          const_cast<MethodNode&>(method));
        if (snapshot.empty()) {
            //fail-open：与迁移前 analyze 抛异常走 catch 的语义一致。
            result.returnValue     = AliasValue::unknown();
            result.fieldWrites.clear();
            result.snapshotFailed  = true;
            return result;
        }
        const auto& frames  = snapshot.frames();
        result.returnValue = interpreter.returnValue();

        ca::i32 index = 0;
        for (auto* insn = method.instructions.getFirst(); insn; insn = insn->getNext(), ++index) {
            if (index < 0 || static_cast<ca::usize>(index) >= frames.size() ||
                !frames[static_cast<ca::usize>(index)]) {
                continue;
            }
            const auto* frame  = frames[static_cast<ca::usize>(index)];
            const auto  opcode = insn->getOpcode();
            if (opcode == Opcodes::PUTFIELD && frame->getStackSize() >= 2) {
                const auto* field = ca::core::tag_cast::cast<const FieldInsnNode>(insn);
                result.fieldWrites.push_back(
                    {{text(field->owner), text(field->name), text(field->desc)},
                     frame->getStack(frame->getStackSize() - 2).alias,
                     frame->getStack(frame->getStackSize() - 1).alias,
                     false});
                continue;
            }
            if (opcode == Opcodes::PUTSTATIC && frame->getStackSize() >= 1) {
                const auto* field = ca::core::tag_cast::cast<const FieldInsnNode>(insn);
                result.fieldWrites.push_back(
                    {{text(field->owner), text(field->name), text(field->desc)},
                     AliasValue::unknown(),
                     frame->getStack(frame->getStackSize() - 1).alias,
                     true});
                continue;
            }
            if (insn->getType() != AbstractInsnNode::METHOD_INSN || !previous) continue;

            //P2：只采集调用点 fw σ（CFG 帧值），不再把 callee 事实内联进
            //fieldWrites——final FW 组由绑定展开在索引期等值重建。
            const auto* call = ca::core::tag_cast::cast<const MethodInsnNode>(insn);
            const auto* callee =
                previous->summaryOf(text(call->owner), text(call->name), text(call->desc));
            if (!callee) continue;
            ControlFlowAliasFacts::CallFlowSigma sigma;
            sigma.instructionIndex = index;
            const auto argumentTypes = Type::getArgumentTypes(call->desc);
            const bool isStatic      = opcode == Opcodes::INVOKESTATIC;
            const auto required      = argumentTypes.size() + (isStatic ? 0u : 1u);
            if (static_cast<ca::usize>(frame->getStackSize()) >= required) {
                const auto first = static_cast<ca::usize>(frame->getStackSize()) - required;
                sigma.receiver =
                    isStatic ? AliasValue::unknown() : frame->getStack(static_cast<int>(first)).alias;
                const auto argumentOffset = first + (isStatic ? 0u : 1u);
                sigma.arguments.reserve(argumentTypes.size());
                for (ca::usize argument = 0; argument < argumentTypes.size(); ++argument) {
                    sigma.arguments.push_back(
                        frame->getStack(static_cast<int>(argumentOffset + argument)).alias);
                }
                sigma.eligible = true;
            }
            result.flowSigmas.push_back(std::move(sigma));
        }
        return result;
    }
    catch (...) {
        result.returnValue     = AliasValue::unknown();
        result.fieldWrites.clear();
        result.snapshotFailed  = true;
        return result;
    }
}

AliasMethodSummary summarize(const ClassNode& clazz, const MethodNode& method,
                             const AliasAnalysis* previous)
{
    AliasMethodSummary summary;
    summary.method = {text(clazz.name), text(method.name), text(method.desc)};

    std::vector<Type> args;
    try {
        args = Type::getArgumentTypes(method.desc);
    }
    catch (...) {
        summary.hasExceptionalExit = true;
        return summary;
    }

    std::unordered_map<ca::i32, AliasValue> locals;
    ca::i32                                 slot = 0;
    if ((method.access & Opcodes::ACC_STATIC) == 0) {
        locals[slot++] = AliasValue::object("this:" + text(clazz.name));
    }
    ca::i32 parameterIndex = 0;
    for (const auto& arg : args) {
        auto value = valueForType(arg, "param:" + std::to_string(parameterIndex));
        summary.parameters.push_back(value);
        locals[slot] = value;
        slot += arg.getSize();
        ++parameterIndex;
    }

    std::vector<AliasValue>                     stack;
    std::unordered_map<std::string, AliasValue> arrayElements;
    std::unordered_map<std::string, AliasValue> containerElements;
    std::unordered_map<std::string, AliasValue> instanceFields;
    ca::i32                                     insnIndex      = 0;
    bool                                        sawReturn      = false;
    bool                                        hasControlFlow = false;
    for (auto* insn = method.instructions.getFirst(); insn; insn = insn->getNext(), ++insnIndex) {
        const auto opcode = insn->getOpcode();
        if (opcode < 0) continue;
        hasControlFlow |= insn->getType() == AbstractInsnNode::JUMP_INSN ||
                          insn->getType() == AbstractInsnNode::TABLESWITCH_INSN ||
                          insn->getType() == AbstractInsnNode::LOOKUPSWITCH_INSN;

        switch (opcode) {
        case Opcodes::LDC:
        {
            auto* ldc = ca::core::tag_cast::cast<LdcInsnNode>(insn);
            if (auto* value = ldc->cst.as<std::string>()) {
                stack.push_back(primitiveAlias("string:" + *value));
            }
            else if (auto* value = ldc->cst.as<ca::str::Utf8StringRef>()) {
                stack.push_back(primitiveAlias("string:" + value->to_std_string()));
            }
            else if (auto* type = ldc->cst.as<Type>()) {
                if (type->getSort() == Type::OBJECT || type->getSort() == Type::ARRAY) {
                    //staticArena 退役：仅瞬时拼串，函数级局部 arena 零驻留。
                    ca::str::Utf8StringArena scratch;
                    stack.push_back(AliasValue::object(
                        "class:" + type->getInternalName(scratch).to_std_string()));
                }
                else {
                    stack.push_back(AliasValue::unknown());
                }
            }
            else {
                stack.push_back(AliasValue::unknown());
            }
            break;
        }
        case Opcodes::ACONST_NULL: stack.push_back(AliasValue::nullValue()); break;
        case Opcodes::ICONST_M1:
        case Opcodes::ICONST_0:
        case Opcodes::ICONST_1:
        case Opcodes::ICONST_2:
        case Opcodes::ICONST_3:
        case Opcodes::ICONST_4:
        case Opcodes::ICONST_5:
            stack.push_back(primitiveAlias(
                "int:" +
                std::to_string(opcode == Opcodes::ICONST_M1 ? -1
                                                            : opcode - Opcodes::ICONST_0)));
            break;
        case Opcodes::ALOAD:
        case Opcodes::ILOAD:
        {
            auto* var = ca::core::tag_cast::cast<VarInsnNode>(insn);
            stack.push_back(locals.count(var->var) ? locals[var->var] : AliasValue::unknown());
            break;
        }
        case Opcodes::ASTORE:
        {
            auto* var        = ca::core::tag_cast::cast<VarInsnNode>(insn);
            locals[var->var] = popAlias(stack);
            break;
        }
        case Opcodes::DUP:
            stack.push_back(stack.empty() ? AliasValue::unknown() : stack.back());
            break;
        case Opcodes::NEW:
        {
            auto* type = ca::core::tag_cast::cast<TypeInsnNode>(insn);
            stack.push_back(AliasValue::object("new:" + summary.method.owner + "." +
                                               summary.method.name + summary.method.desc + ":" +
                                               std::to_string(insnIndex) + ":" + text(type->desc)));
            break;
        }
        case Opcodes::NEWARRAY:
        case Opcodes::ANEWARRAY:
            popAlias(stack);
            stack.push_back(AliasValue::array("array:" + summary.method.owner + "." +
                                              summary.method.name + summary.method.desc + ":" +
                                              std::to_string(insnIndex)));
            break;
        case Opcodes::MULTIANEWARRAY:
        {
            const auto dimensions = ca::core::tag_cast::cast<MultiANewArrayInsnNode>(insn)->dims;
            for (ca::i32 dimension = 0; dimension < dimensions; ++dimension) popAlias(stack);
            stack.push_back(AliasValue::array("array:" + summary.method.owner + "." +
                                              summary.method.name + summary.method.desc + ":" +
                                              std::to_string(insnIndex)));
            break;
        }
        case Opcodes::GETSTATIC:
        {
            auto*      field = ca::core::tag_cast::cast<FieldInsnNode>(insn);
            auto       type  = Type::getType(field->desc);
            AliasValue value =
                previous && previous->fieldValueOf(
                                text(field->owner), text(field->name), text(field->desc))
                    ? *previous->fieldValueOf(
                          text(field->owner), text(field->name), text(field->desc))
                    : valueForType(type,
                                   "field:" + text(field->owner) + "." + text(field->name) + ":" +
                                       text(field->desc));
            stack.push_back(value);
            break;
        }
        case Opcodes::GETFIELD:
        {
            const auto receiver = popAlias(stack);
            auto*      field    = ca::core::tag_cast::cast<FieldInsnNode>(insn);
            auto       type     = Type::getType(field->desc);
            const auto fieldName =
                text(field->owner) + "." + text(field->name) + ":" + text(field->desc);
            const auto local = instanceFields.find(
                fieldKey(receiver, {text(field->owner), text(field->name), text(field->desc)}));
            const auto prior =
                previous ? previous->resolvedFieldValueOf(
                               receiver, text(field->owner), text(field->name), text(field->desc))
                         : std::nullopt;
            AliasValue value = local != instanceFields.end() ? local->second
                               : prior                       ? *prior
                                       : valueForType(type, "field:" + fieldName);
            stack.push_back(value);
            break;
        }
        case Opcodes::PUTFIELD:
        {
            const auto        value    = popAlias(stack);
            const auto        receiver = popAlias(stack);
            auto*             field    = ca::core::tag_cast::cast<FieldInsnNode>(insn);
            const FieldRefKey key{text(field->owner), text(field->name), text(field->desc)};
            if (receiver.isKnown()) instanceFields[fieldKey(receiver, key)] = value;
            summary.fieldWrites.push_back({key, receiver, value});
            break;
        }
        case Opcodes::PUTSTATIC:
        {
            const auto value = popAlias(stack);
            auto*      field = ca::core::tag_cast::cast<FieldInsnNode>(insn);
            summary.fieldWrites.push_back(
                {{text(field->owner), text(field->name), text(field->desc)},
                 AliasValue::unknown(),
                 value,
                 true});
            break;
        }
        case Opcodes::AASTORE:
        case Opcodes::BASTORE:
        case Opcodes::CASTORE:
        case Opcodes::DASTORE:
        case Opcodes::FASTORE:
        case Opcodes::IASTORE:
        case Opcodes::LASTORE:
        case Opcodes::SASTORE:
        {
            const auto value = popAlias(stack);
            const auto index = popAlias(stack);
            const auto array = popAlias(stack);
            if (array.isKnown()) {
                const auto key = arrayKey(array, index);
                auto       it  = arrayElements.find(key);
                arrayElements[key] =
                    it == arrayElements.end() ? value : mergeAliasValues(it->second, value);
            }
            summary.arrayWrites.push_back({array, index, value});
            break;
        }
        case Opcodes::AALOAD:
        case Opcodes::BALOAD:
        case Opcodes::CALOAD:
        case Opcodes::DALOAD:
        case Opcodes::FALOAD:
        case Opcodes::IALOAD:
        case Opcodes::LALOAD:
        case Opcodes::SALOAD:
        {
            const auto index      = popAlias(stack);
            const auto array      = popAlias(stack);
            AliasValue value      = AliasValue::unknown();
            bool       foundLocal = false;
            if (array.isKnown()) {
                auto it = arrayElements.find(arrayKey(array, index));
                if (it == arrayElements.end() && index.isKnown()) {
                    it = arrayElements.find(*array.identity + '\n' + "*");
                }
                if (it != arrayElements.end()) {
                    value      = it->second;
                    foundLocal = true;
                }
            }
            if (!foundLocal && previous) {
                const auto prior = previous->resolvedArrayValueOf(array, index);
                if (prior) value = *prior;
            }
            stack.push_back(value);
            break;
        }
        case Opcodes::INVOKEVIRTUAL:
        case Opcodes::INVOKESPECIAL:
        case Opcodes::INVOKESTATIC:
        case Opcodes::INVOKEINTERFACE:
        {
            auto*      call      = ca::core::tag_cast::cast<MethodInsnNode>(insn);
            auto       callArgs  = Type::getArgumentTypes(call->desc);
            const auto callOwner = text(call->owner);
            const auto callName  = text(call->name);
            if (isReflectionLookup(callOwner, callName) && stack.size() > callArgs.size()) {
                const auto receiverIndex = stack.size() - callArgs.size() - 1;
                summary.reflectionReceivers.push_back({insnIndex, stack[receiverIndex]});
            }
            auto       arguments = takeArguments(stack, callArgs);
            AliasValue receiver =
                opcode == Opcodes::INVOKESTATIC ? AliasValue::unknown() : popAlias(stack);
            auto       returnType = Type::getReturnType(call->desc);
            const bool containerWrite =
                (isMapOwner(callOwner) || isListOwner(callOwner) || isQueueOwner(callOwner)) &&
                (callName == "add" || callName == "offer" || callName == "put" ||
                 callName == "set" || callName == "push");
            const bool containerDestructiveRead =
                isQueueOwner(callOwner) && arguments.empty() &&
                (callName == "poll" || callName == "pop" || callName == "remove");
            const bool containerRead =
                (isMapOwner(callOwner) || isListOwner(callOwner) || isQueueOwner(callOwner)) &&
                (callName == "get" || callName == "peek" || callName == "element" ||
                 callName == "getOrDefault" || containerDestructiveRead);
            const bool containerInvalidate =
                (isMapOwner(callOwner) || isListOwner(callOwner) || isQueueOwner(callOwner)) &&
                (callName == "clear" || callName == "remove" || callName == "removeAll" ||
                 callName == "retainAll" || callName == "putAll" || callName == "addAll" ||
                 callName == "replaceAll" || callName == "compute" ||
                 callName == "computeIfAbsent" || callName == "computeIfPresent" ||
                 callName == "merge") &&
                !containerDestructiveRead;
            AliasValue containerKeyValue = AliasValue::unknown();
            if (isMapOwner(callOwner)) {
                if (!arguments.empty()) containerKeyValue = arguments.front();
            }
            else if (callName == "set" && !arguments.empty()) {
                containerKeyValue = arguments.front();
            }
            const AliasMethodSummary* callee =
                previous
                    ? previous->summaryOf(text(call->owner), text(call->name), text(call->desc))
                    : nullptr;
            if (callee) {
                //P2（perf/alias-closure）：记录调用点绑定，替代把 callee 展开
                //事实内联复制进摘要（环上逐轮翻倍的发散根因）。局部 map 馈给
                //仍按 P1 逐组顺序从「展开 + 实例化」的瞬时流消费，等值重建
                //P1 的 instanceFields/arrayElements/containerElements 演化。
                const std::string bindingKey =
                    callOwner + '\n' + callName + '\n' + text(call->desc);
                AliasMethodSummary::CallBinding binding;
                binding.calleeKey = bindingKey;
                binding.instructionIndex = insnIndex;
                binding.receiver         = receiver;
                binding.arguments        = arguments;
                binding.fwReceiver       = receiver;
                binding.fwArguments      = arguments;
                summary.callBindings.push_back(std::move(binding));

                //展开流以 callee 槽位播种（环上路径每方法至多一次），与
                //索引期 deposit 的展开语义同构。
                std::vector<ca::usize> visiting;
                ca::usize              calleeSlot = 0;
                if (previous->resolveBinding(bindingKey, &calleeSlot) != nullptr) {
                    visiting.push_back(calleeSlot);
                }
                std::vector<AliasMethodSummary::FieldWrite> inlineFieldWrites;
                previous->expandFieldWrites(*callee, visiting, inlineFieldWrites);
                for (const auto& write : inlineFieldWrites) {
                    const auto actualReceiver =
                        instantiateFact(write.receiver, arguments, receiver);
                    const auto actualValue = instantiateFact(write.value, arguments, receiver);
                    if (!write.isStatic) {
                        auto receivers = actualReceiver.pointsTo;
                        if (receivers.empty() && actualReceiver.isKnown()) {
                            receivers.push_back(actualReceiver.identity);
                        }
                        for (const auto& identity : receivers) {
                            auto       target   = AliasValue::object(*identity);
                            const auto key      = fieldKey(target, write.field);
                            auto       it       = instanceFields.find(key);
                            instanceFields[key] = it == instanceFields.end()
                                                      ? actualValue
                                                      : mergeAliasValues(it->second, actualValue);
                        }
                    }
                }
                std::vector<AliasMethodSummary::ArrayWrite> inlineArrayWrites;
                previous->expandArrayWrites(*callee, visiting, inlineArrayWrites);
                for (const auto& write : inlineArrayWrites) {
                    const auto actualArray = instantiateFact(write.array, arguments, receiver);
                    const auto actualIndex = instantiateFact(write.index, arguments, receiver);
                    const auto actualValue = instantiateFact(write.value, arguments, receiver);
                    if (actualArray.isKnown()) {
                        const auto key     = arrayKey(actualArray, actualIndex);
                        auto       it      = arrayElements.find(key);
                        arrayElements[key] = it == arrayElements.end()
                                                 ? actualValue
                                                 : mergeAliasValues(it->second, actualValue);
                    }
                }
                std::vector<AliasMethodSummary::ContainerWrite> inlineContainerWrites;
                previous->expandContainerWrites(*callee, visiting, inlineContainerWrites);
                for (const auto& write : inlineContainerWrites) {
                    const auto actualContainer =
                        instantiateFact(write.container, arguments, receiver);
                    const auto actualKey   = instantiateFact(write.key, arguments, receiver);
                    const auto actualValue = instantiateFact(write.value, arguments, receiver);
                    if (actualContainer.isKnown()) {
                        const auto key = containerKey(actualContainer, actualKey);
                        if (!actualValue.isKnown()) {
                            const std::string prefix = *actualContainer.identity + '\n';
                            for (auto it = containerElements.begin();
                                 it != containerElements.end();) {
                                if (it->first.compare(0, prefix.size(), prefix) == 0) {
                                    it = containerElements.erase(it);
                                }
                                else {
                                    ++it;
                                }
                            }
                        }
                        containerElements[key] = actualValue;
                    }
                }
            }
            if (containerWrite && !arguments.empty() && receiver.isKnown()) {
                const auto key   = containerKey(receiver, containerKeyValue);
                auto       it    = containerElements.find(key);
                const auto value = arguments.back();
                containerElements[key] =
                    it == containerElements.end() ? value : mergeAliasValues(it->second, value);
                summary.containerWrites.push_back({receiver, containerKeyValue, value});
            }
            auto invalidateContainer = [&](const AliasValue& target) {
                if (!target.isKnown()) return;
                const std::string prefix = *target.identity + '\n';
                for (auto it = containerElements.begin(); it != containerElements.end();) {
                    if (it->first.compare(0, prefix.size(), prefix) == 0) {
                        it = containerElements.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
                containerElements[containerKey(target, AliasValue::unknown())] =
                    AliasValue::unknown();
                summary.containerWrites.push_back(
                    {target, AliasValue::unknown(), AliasValue::unknown()});
            };
            const bool unknownContainerCall =
                callee == nullptr && !containerWrite && !containerRead && !containerInvalidate;
            if (unknownContainerCall) {
                invalidateContainer(receiver);
                for (const auto& argument : arguments) {
                    if (argument.kind == AliasValueKind::Container) invalidateContainer(argument);
                }
            }
            if (containerInvalidate && receiver.isKnown()) {
                invalidateContainer(receiver);
            }
            if (returnType.getSort() != Type::VOID) {
                if (containerRead && receiver.isKnown()) {
                    auto local = containerElements.find(containerKey(receiver, containerKeyValue));
                    if (local == containerElements.end() && containerKeyValue.isKnown()) {
                        local =
                            containerElements.find(containerKey(receiver, AliasValue::unknown()));
                    }
                    if (local != containerElements.end()) {
                        const auto value = local->second;
                        if (containerDestructiveRead) invalidateContainer(receiver);
                        stack.push_back(value);
                        break;
                    }
                    if (previous) {
                        auto* prior = previous->containerValueOf(receiver, containerKeyValue);
                        if (prior) {
                            const auto value = *prior;
                            if (containerDestructiveRead) invalidateContainer(receiver);
                            stack.push_back(value);
                            break;
                        }
                    }
                }
                if (containerDestructiveRead && receiver.isKnown()) invalidateContainer(receiver);
                auto resolved = instantiateReturn(callee, arguments, receiver, returnType);
                stack.push_back(resolved);
            }
            break;
        }
        case Opcodes::ARETURN:
        {
            const auto value    = popAlias(stack);
            summary.returnValue = sawReturn ? mergeAliasValues(summary.returnValue, value) : value;
            sawReturn           = true;
            break;
        }
        case Opcodes::ATHROW:
            summary.hasExceptionalExit = true;
            popAlias(stack);
            break;
        default: break;
        }
    }
    if (hasControlFlow) {
        const auto flowFacts = analyzeControlFlowFacts(clazz, method, previous);
        if (flowFacts.returnValue) {
            summary.returnValue = *flowFacts.returnValue;
        }
        summary.fieldWrites = flowFacts.fieldWrites;
        //P2：绑定 FW 组按 flow 语义重写——snapshot 失败只失效 FW 组；
        //调用点按 CFG 帧值重写 fw σ，栈深不足的调用点 FW 组失效（与 P1
        //flow 内联的逐调用点 continue 语义一致）。AW/CW 组保持主走查 σ。
        if (flowFacts.snapshotFailed) {
            for (auto& binding : summary.callBindings) binding.fieldWritesActive = false;
        }
        else {
            for (const auto& sigma : flowFacts.flowSigmas) {
                bool matched = false;
                for (auto& binding : summary.callBindings) {
                    if (binding.instructionIndex != sigma.instructionIndex) continue;
                    matched = true;
                    if (sigma.eligible) {
                        binding.fwReceiver  = sigma.receiver;
                        binding.fwArguments = sigma.arguments;
                    }
                    else {
                        binding.fieldWritesActive = false;
                    }
                    break;
                }
                (void)matched;
            }
        }
    }
    if (!method.tryCatchBlocks.empty()) {
        //Analyzer already merges the real protected-region predecessors into
        //each handler entry. Keep the exceptional-exit bit for callers, but
        //do not discard a return fact that survives that CFG merge.
        summary.hasExceptionalExit = true;
    }
    return summary;
}

bool sameAliasFact(const AliasValue& lhs, const AliasValue& rhs)
{
    return lhs.kind == rhs.kind && lhs.identity == rhs.identity && lhs.pointsTo == rhs.pointsTo;
}

bool sameFieldWrite(const AliasMethodSummary::FieldWrite& lhs,
                    const AliasMethodSummary::FieldWrite& rhs)
{
    return lhs.field.owner == rhs.field.owner && lhs.field.name == rhs.field.name &&
           lhs.field.desc == rhs.field.desc && sameAliasFact(lhs.receiver, rhs.receiver) &&
           sameAliasFact(lhs.value, rhs.value) && lhs.isStatic == rhs.isStatic;
}

bool sameContainerWrite(const AliasMethodSummary::ContainerWrite& lhs,
                        const AliasMethodSummary::ContainerWrite& rhs)
{
    return sameAliasFact(lhs.container, rhs.container) && sameAliasFact(lhs.key, rhs.key) &&
           sameAliasFact(lhs.value, rhs.value);
}

bool sameArrayWrite(const AliasMethodSummary::ArrayWrite& lhs,
                    const AliasMethodSummary::ArrayWrite& rhs)
{
    return sameAliasFact(lhs.array, rhs.array) && sameAliasFact(lhs.index, rhs.index) &&
           sameAliasFact(lhs.value, rhs.value);
}

bool sameCallBinding(const AliasMethodSummary::CallBinding& lhs,
                     const AliasMethodSummary::CallBinding& rhs)
{
    if (lhs.calleeKey != rhs.calleeKey || lhs.instructionIndex != rhs.instructionIndex ||
        !sameAliasFact(lhs.receiver, rhs.receiver) ||
        !sameAliasFact(lhs.fwReceiver, rhs.fwReceiver) ||
        lhs.arguments.size() != rhs.arguments.size() ||
        lhs.fwArguments.size() != rhs.fwArguments.size() ||
        lhs.fieldWritesActive != rhs.fieldWritesActive ||
        lhs.arrayWritesActive != rhs.arrayWritesActive ||
        lhs.containerWritesActive != rhs.containerWritesActive) {
        return false;
    }
    for (ca::usize index = 0; index < lhs.arguments.size(); ++index) {
        if (!sameAliasFact(lhs.arguments[index], rhs.arguments[index])) return false;
    }
    for (ca::usize index = 0; index < lhs.fwArguments.size(); ++index) {
        if (!sameAliasFact(lhs.fwArguments[index], rhs.fwArguments[index])) return false;
    }
    return true;
}

bool sameSummary(const AliasMethodSummary& lhs, const AliasMethodSummary& rhs)
{
    if (lhs.method.owner != rhs.method.owner || lhs.method.name != rhs.method.name ||
        lhs.method.desc != rhs.method.desc || lhs.hasExceptionalExit != rhs.hasExceptionalExit ||
        lhs.parameters.size() != rhs.parameters.size() ||
        lhs.fieldWrites.size() != rhs.fieldWrites.size() ||
        lhs.arrayWrites.size() != rhs.arrayWrites.size() ||
        lhs.containerWrites.size() != rhs.containerWrites.size() ||
        lhs.reflectionReceivers.size() != rhs.reflectionReceivers.size() ||
        lhs.callBindings.size() != rhs.callBindings.size() ||
        !sameAliasFact(lhs.returnValue, rhs.returnValue)) {
        return false;
    }
    for (ca::usize index = 0; index < lhs.parameters.size(); ++index) {
        if (!sameAliasFact(lhs.parameters[index], rhs.parameters[index])) return false;
    }
    for (ca::usize index = 0; index < lhs.fieldWrites.size(); ++index) {
        if (!sameFieldWrite(lhs.fieldWrites[index], rhs.fieldWrites[index])) return false;
    }
    for (ca::usize index = 0; index < lhs.arrayWrites.size(); ++index) {
        if (!sameArrayWrite(lhs.arrayWrites[index], rhs.arrayWrites[index])) return false;
    }
    for (ca::usize index = 0; index < lhs.containerWrites.size(); ++index) {
        if (!sameContainerWrite(lhs.containerWrites[index], rhs.containerWrites[index])) {
            return false;
        }
    }
    for (ca::usize index = 0; index < lhs.reflectionReceivers.size(); ++index) {
        const auto& left  = lhs.reflectionReceivers[index];
        const auto& right = rhs.reflectionReceivers[index];
        if (left.instructionIndex != right.instructionIndex ||
            !sameAliasFact(left.receiver, right.receiver)) {
            return false;
        }
    }
    for (ca::usize index = 0; index < lhs.callBindings.size(); ++index) {
        if (!sameCallBinding(lhs.callBindings[index], rhs.callBindings[index])) return false;
    }
    return true;
}

}   //namespace

namespace {

///@brief 解析容器内全部类资源（保持资源迭代序；与既有 build 逐行同构）。
std::vector<std::unique_ptr<ClassNode>> parseClassResources(ResourceContainer&          container,
                                                            ca::str::Utf8StringArena&   arena,
                                                            ca::usize&                  methodCount)
{
    std::vector<std::unique_ptr<ClassNode>> classes;
    for (auto& [_, resource] : container.getResources()) {
        if (!resource || resource->getType() != ResourceType::Class) continue;
        auto* classResource = ca::core::tag_cast::cast<ClassResource>(resource.get());
        if (!classResource || classResource->get().empty()) continue;
        try {
            auto        clazz = std::make_unique<ClassNode>();
            ClassReader reader(arena,
                               classResource->get().data(),
                               static_cast<ca::i32>(classResource->get().size()));
            reader.accept(clazz.get(), ClassReader::SKIP_DEBUG | ClassReader::SKIP_FRAMES);
            methodCount += clazz->methods.size();
            classes.push_back(std::move(clazz));
        }
        catch (...) {
            //Malformed classes contribute no facts; consumers observe Unknown.
        }
    }
    return classes;
}

///@brief 反射闭包作用域选择（buildReflectionClosure 专用；规则见 hpp 注释）。
std::vector<const ClassNode*> selectReflectionClosure(
    const std::vector<std::unique_ptr<ClassNode>>& classes)
{
    std::unordered_map<std::string, const ClassNode*> byName;
    for (const auto& clazz : classes) byName.emplace(text(clazz->name), clazz.get());

    std::unordered_set<std::string>   inScope;
    std::vector<std::string>          queue;
    std::unordered_map<std::string, std::vector<std::string>> callTargets;

    for (const auto& clazz : classes) {
        const auto self = text(clazz->name);
        bool       seed = false;
        //R4：声明 Class 形静态字段（静态类常量缓存的典型声明点）。
        for (const auto* field : clazz->fields) {
            if (field && (field->access & Opcodes::ACC_STATIC) != 0 &&
                text(field->desc) == "Ljava/lang/Class;") {
                seed = true;
                break;
            }
        }
        for (const auto* method : clazz->methods) {
            if (!method) continue;
            for (auto* insn = method->instructions.getFirst(); insn; insn = insn->getNext()) {
                if (insn->getType() == AbstractInsnNode::METHOD_INSN) {
                    const auto* call = ca::core::tag_cast::cast<const MethodInsnNode>(insn);
                    const auto  owner = text(call->owner);
                    if (isReflectionLookup(owner, text(call->name))) seed = true;   //R1
                    callTargets[self].push_back(owner);
                    continue;
                }
                const auto opcode = insn->getOpcode();
                if (opcode == Opcodes::PUTSTATIC || opcode == Opcodes::GETSTATIC) {
                    //R3：Class 形静态槽读写（槽按 field 全局聚合，跨类介导）。
                    const auto* field = ca::core::tag_cast::cast<const FieldInsnNode>(insn);
                    if (field && text(field->desc) == "Ljava/lang/Class;") seed = true;
                }
            }
        }
        if (seed) {
            if (inScope.insert(self).second) queue.push_back(self);
        }
    }

    //R2：容器内传递 callee 闭包（in-scope 摘要经 callee 事实内联/返回值
    //实例化依赖 callee 摘要；JDK 目标不在容器内，自然截断）。
    for (ca::usize cursor = 0; cursor < queue.size(); ++cursor) {
        const auto found = callTargets.find(queue[cursor]);
        if (found == callTargets.end()) continue;
        for (const auto& target : found->second) {
            if (byName.count(target) != 0 && inScope.insert(target).second) {
                queue.push_back(target);
            }
        }
    }

    std::vector<const ClassNode*> scope;
    scope.reserve(inScope.size());
    for (const auto& clazz : classes) {
        if (inScope.count(text(clazz->name)) != 0) scope.push_back(clazz.get());
    }
    return scope;
}

}   //namespace

std::string AliasAnalysis::key(const MethodRefKey& method)
{
    return method.owner + '\n' + method.name + '\n' + method.desc;
}

void AliasAnalysis::clear()
{
    summaries_.clear();
    byKey_.clear();
    fields_.clear();
    fieldsByKey_.clear();
    arrayValues_.clear();
    containerValues_.clear();
    andersen_.clear();
    convergenceRounds_ = 0;
}

void AliasAnalysis::build(ResourceContainer& container, ca::str::Utf8StringArena& arena)
{
    clear();
    ca::usize methodCount = 0;
    auto      classes     = parseClassResources(container, arena, methodCount);
    std::vector<const ClassNode*> scope;
    scope.reserve(classes.size());
    for (const auto& clazz : classes) scope.push_back(clazz.get());
    buildFixedPoint(scope, methodCount);
}

void AliasAnalysis::buildReflectionClosure(ResourceContainer&          container,
                                           ca::str::Utf8StringArena&   arena)
{
    clear();
    ca::usize methodCount = 0;
    auto      classes     = parseClassResources(container, arena, methodCount);
    auto      scope       = selectReflectionClosure(classes);
    ca::usize scopedMethodCount = 0;
    for (const auto* clazz : scope) scopedMethodCount += clazz->methods.size();
    buildFixedPoint(scope, scopedMethodCount);
}

const AliasValue* AliasAnalysis::findRecordedSlot(const std::string& nsKey) const
{
    if (nsKey.size() < 2 || nsKey[1] != '\n') return nullptr;
    const auto rest = nsKey.substr(2);
    switch (nsKey[0]) {
    case 'F': {
        const auto it = fieldsByKey_.find(rest);
        return it == fieldsByKey_.end() ? nullptr : &fields_[it->second].value;
    }
    case 'A': {
        const auto it = arrayValues_.find(rest);
        return it == arrayValues_.end() ? nullptr : &it->second;
    }
    case 'C': {
        const auto it = containerValues_.find(rest);
        return it == containerValues_.end() ? nullptr : &it->second;
    }
    default: return nullptr;
    }
}

const AliasMethodSummary* AliasAnalysis::resolveBinding(const std::string& calleeKey,
                                                        ca::usize*         slotIndex) const
{
    const AliasAnalysis* domain = expansionSource_ != nullptr ? expansionSource_ : this;
    const auto           it     = domain->byKey_.find(calleeKey);
    if (it == domain->byKey_.end()) return nullptr;
    *slotIndex = it->second;
    return &domain->summaries_[it->second];
}

void AliasAnalysis::expandFieldWrites(const AliasMethodSummary&                    summary,
                                      std::vector<ca::usize>&                      visiting,
                                      std::vector<AliasMethodSummary::FieldWrite>& out) const
{
    const AliasAnalysis* source = expansionSource_ != nullptr ? expansionSource_ : this;
    out.insert(out.end(), summary.fieldWrites.begin(), summary.fieldWrites.end());
    for (const auto& binding : summary.callBindings) {
        if (!binding.fieldWritesActive) continue;
        ca::usize                 slot   = 0;
        const AliasMethodSummary* callee = resolveBinding(binding.calleeKey, &slot);
        if (callee == nullptr) continue;
        std::vector<AliasMethodSummary::FieldWrite> inner;
        if (std::find(visiting.begin(), visiting.end(), slot) != visiting.end()) {
            //环重入：只贡献 callee 自身事实（按本绑定实例化）。收敛点上
            //环回流的重复事实已在此前层级落位（P1 收敛判据保证）。
            inner.assign(callee->fieldWrites.begin(), callee->fieldWrites.end());
        }
        else {
            visiting.push_back(slot);
            source->expandFieldWrites(*callee, visiting, inner);
            visiting.pop_back();
        }
        for (const auto& write : inner) {
            out.push_back({write.field,
                           instantiateFact(write.receiver, binding.fwArguments, binding.fwReceiver),
                           instantiateFact(write.value, binding.fwArguments, binding.fwReceiver),
                           write.isStatic});
        }
    }
}

void AliasAnalysis::expandArrayWrites(const AliasMethodSummary&                      summary,
                                      std::vector<ca::usize>&                        visiting,
                                      std::vector<AliasMethodSummary::ArrayWrite>&   out) const
{
    const AliasAnalysis* source = expansionSource_ != nullptr ? expansionSource_ : this;
    out.insert(out.end(), summary.arrayWrites.begin(), summary.arrayWrites.end());
    for (const auto& binding : summary.callBindings) {
        if (!binding.arrayWritesActive) continue;
        ca::usize                 slot   = 0;
        const AliasMethodSummary* callee = resolveBinding(binding.calleeKey, &slot);
        if (callee == nullptr) continue;
        std::vector<AliasMethodSummary::ArrayWrite> inner;
        if (std::find(visiting.begin(), visiting.end(), slot) != visiting.end()) {
            inner.assign(callee->arrayWrites.begin(), callee->arrayWrites.end());
        }
        else {
            visiting.push_back(slot);
            source->expandArrayWrites(*callee, visiting, inner);
            visiting.pop_back();
        }
        for (const auto& write : inner) {
            out.push_back({instantiateFact(write.array, binding.arguments, binding.receiver),
                           instantiateFact(write.index, binding.arguments, binding.receiver),
                           instantiateFact(write.value, binding.arguments, binding.receiver)});
        }
    }
}

void AliasAnalysis::expandContainerWrites(
    const AliasMethodSummary&                                      summary,
    std::vector<ca::usize>&                                        visiting,
    std::vector<AliasMethodSummary::ContainerWrite>&               out) const
{
    const AliasAnalysis* source = expansionSource_ != nullptr ? expansionSource_ : this;
    out.insert(out.end(), summary.containerWrites.begin(), summary.containerWrites.end());
    for (const auto& binding : summary.callBindings) {
        if (!binding.containerWritesActive) continue;
        ca::usize                 slot   = 0;
        const AliasMethodSummary* callee = resolveBinding(binding.calleeKey, &slot);
        if (callee == nullptr) continue;
        std::vector<AliasMethodSummary::ContainerWrite> inner;
        if (std::find(visiting.begin(), visiting.end(), slot) != visiting.end()) {
            inner.assign(callee->containerWrites.begin(), callee->containerWrites.end());
        }
        else {
            visiting.push_back(slot);
            source->expandContainerWrites(*callee, visiting, inner);
            visiting.pop_back();
        }
        for (const auto& write : inner) {
            out.push_back({instantiateFact(write.container, binding.arguments, binding.receiver),
                           instantiateFact(write.key, binding.arguments, binding.receiver),
                           instantiateFact(write.value, binding.arguments, binding.receiver)});
        }
    }
}

AliasExpandedFacts AliasAnalysis::expandedFactsOf(const std::string& owner, const std::string& name,
                                                  const std::string& desc) const
{
    AliasExpandedFacts     facts;
    const AliasAnalysis*   domain   = expansionSource_ != nullptr ? expansionSource_ : this;
    const auto*            summary  = domain->summaryOf(owner, name, desc);
    if (summary == nullptr) return facts;
    const auto             startIt  = domain->byKey_.find(key({owner, name, desc}));
    std::vector<ca::usize> visiting;
    if (startIt != domain->byKey_.end()) visiting.push_back(startIt->second);
    domain->expandFieldWrites(*summary, visiting, facts.fieldWrites);
    visiting.assign(visiting.begin(), visiting.begin() + 1);
    domain->expandArrayWrites(*summary, visiting, facts.arrayWrites);
    visiting.assign(visiting.begin(), visiting.begin() + 1);
    domain->expandContainerWrites(*summary, visiting, facts.containerWrites);
    return facts;
}

void AliasAnalysis::buildFixedPoint(const std::vector<const ClassNode*>& scope,
                                    ca::usize                            roundBound)
{
    //P1（perf/alias-closure）memoized Jacobi 固定点：
    //- 每方法 summarize 时记录其经 previous 触达的全部输入键（摘要键 "M\n…"、
    //精确槽键 "F|A|C\n…"）；
    //- 轮 k 仅重算「输入键在 [depsRound, k) 内发生过内容变更」的方法；变更记账
    //在轮末统一落地（deferred），保证轮内一律读前一轮快照——Jacobi 语义不变；
    //- 未重算方法的 S_k 逐字节复用 S_{k-1}（输入未变 ⇒ F_M(S_{k-1}) =
    //F_M(S_{j-1}) = S_j(M)），与全量重算版的 S_k 序列逐方法等值 ⇒ 收敛轮数恒等；
    //- 收敛判定 = 本轮无任何内容变更（sameSummaries 全量深比较增量化为
    //逐方法 sameSummary + 槽键对账）；
    //- 轮内 andersen 索引悬置（addSummary 的逐条 solve 在固定点内零消费——
    //andersen*Of 无产物调用方），收敛后仍由 indexAndersenBytecode 一次性索引。
    //
    //P2（perf/alias-closure）引用化摘要的接线：
    //- next.expansionSource_ = &current：addSummary 的绑定展开一律读前一轮
    //快照（Jacobi 轮内只读上一轮），deposits 逐轮按最新闭包等值重建；
    //- 轮末「展开变更传染」记账：绑定闭包内任一方法本轮内容变更 ⇒ 该方法
    //的展开流已变 ⇒ bump 其摘要键，令消费其展开流（summarize 局部 map
    //馈给）的调用方下一轮重算。环安全 DFS（回边不计，自身键已计入）。
    std::unordered_map<std::string, ca::usize> lastChangeRound;
    ca::usize                                  methodTotal = 0;
    for (const auto* clazz : scope) methodTotal += clazz->methods.size();
    std::vector<std::vector<std::string>>      deps(methodTotal);
    std::vector<ca::usize>                     depsRound(methodTotal, 0);
    std::vector<bool>                          threw(methodTotal, false);
    std::unordered_set<std::string>            candidateKeys;

    AliasAnalysis current;
    ca::usize     completedRounds = 0;
    for (ca::usize round = 0; round < roundBound + 1; ++round) {
        AliasAnalysis next;
        candidateKeys.clear();
        next.slotChangeSink_          = &candidateKeys;
        next.suspendAndersenIndexing_ = true;
        next.expansionSource_         = &current;
        bool     anyChange = false;
        std::unordered_set<std::string> changedSummaryKeys;
        ca::usize index    = 0;
        for (const auto* clazz : scope) {
            for (auto* method : clazz->methods) {
                if (!method) continue;
                const auto mkey =
                    "M\n" + key({text(clazz->name), text(method->name), text(method->desc)});
                bool dirty = (round == 0) || threw[index];
                if (!dirty) {
                    for (const auto& dep : deps[index]) {
                        const auto it = lastChangeRound.find(dep);
                        if (it != lastChangeRound.end() && it->second >= depsRound[index]) {
                            dirty = true;
                            break;
                        }
                    }
                }
                if (dirty) {
                    AliasMethodSummary summary;
                    bool               methodThrew = false;
                    current.recordingQueries_ = true;
                    current.queryLog_.clear();
                    try {
                        summary = summarize(*clazz, *method, &current);
                    }
                    catch (...) {
                        ///与全量重算版同口径：畸形方法本轮不产出事实，其余方法
                        ///仍然可用；下轮强制重试（确定性输入下仍走同一路径）。
                        methodThrew = true;
                    }
                    deps[index] = std::move(current.queryLog_);
                    current.recordingQueries_ = false;

                    const auto* oldSummary =
                        current.summaryOf(text(clazz->name), text(method->name), text(method->desc));
                    if (methodThrew) {
                        if (oldSummary != nullptr) {
                            //摘要从有到无：内容变更记账（消费端观测等价）。
                            lastChangeRound[mkey] = round;
                            changedSummaryKeys.insert(mkey);
                            anyChange             = true;
                        }
                    }
                    else if (oldSummary == nullptr ||
                             !sameSummary(*oldSummary, summary)) {
                        lastChangeRound[mkey] = round;
                        changedSummaryKeys.insert(mkey);
                        anyChange             = true;
                        next.addSummary(std::move(summary));
                    }
                    else {
                        next.addSummary(std::move(summary));
                    }
                    depsRound[index] = round;
                    threw[index]     = methodThrew;
                }
                else {
                    //输入未变 ⇒ 本轮值与上一轮逐字节一致，直接复用上一轮摘要。
                    const auto* oldSummary =
                        current.summaryOf(text(clazz->name), text(method->name), text(method->desc));
                    if (oldSummary != nullptr) next.addSummary(*oldSummary);
                }
                ++index;
            }
        }

        //轮末对账：候选槽键逐一对 next vs current 实比（创建噪音在此过滤），
        //实变更落地 deferred 记账，供下一轮 dirty 判定。
        for (const auto& candidate : candidateKeys) {
            const auto* nextValue    = next.findRecordedSlot(candidate);
            const auto* currentValue = current.findRecordedSlot(candidate);
            if ((nextValue == nullptr) != (currentValue == nullptr) ||
                (nextValue != nullptr && !sameAliasFact(*nextValue, *currentValue))) {
                lastChangeRound[candidate] = round;
                anyChange                  = true;
            }
        }

        //P2 轮末：展开变更传染记账。绑定闭包（next 的绑定，按键在 next 内
        //解析）内任一方法本轮内容变更 ⇒ 本方法展开流已变 ⇒ bump 摘要键，
        //令消费其展开流的调用方下一轮重算。环安全 DFS：回边不计（环上每个
        //成员的自身变更键都已计入 changedSummaryKeys）；自身本轮已变更的
        //方法键已记账，无需重复 bump。bump 不置 anyChange——下一轮重算后
        //若无内容变更且闭包清净，自然收敛。
        {
            std::vector<const std::string*> keyOfSlot(next.summaries_.size(), nullptr);
            for (const auto& [slotKey, slot] : next.byKey_) {
                keyOfSlot[slot] = &slotKey;
            }
            std::vector<ca::u8> closureState(next.summaries_.size(), 0);
            std::vector<ca::u8> closureDirty(next.summaries_.size(), 0);
            auto visitClosure = [&](auto&& self, ca::usize slot) -> bool {
                if (closureState[slot] == 2) return closureDirty[slot] != 0;
                if (closureState[slot] == 1) return false;
                closureState[slot] = 1;
                bool dirty         = changedSummaryKeys.count(*keyOfSlot[slot]) != 0;
                if (!dirty) {
                    for (const auto& binding : next.summaries_[slot].callBindings) {
                        const auto calleeIt = next.byKey_.find(binding.calleeKey);
                        if (calleeIt == next.byKey_.end()) continue;
                        if (self(self, calleeIt->second)) {
                            dirty = true;
                            break;
                        }
                    }
                }
                closureState[slot]  = 2;
                closureDirty[slot]  = dirty ? 1 : 0;
                return dirty;
            };
            for (ca::usize slot = 0; slot < next.summaries_.size(); ++slot) {
                if (changedSummaryKeys.count(*keyOfSlot[slot]) != 0) continue;
                if (visitClosure(visitClosure, slot)) {
                    lastChangeRound[*keyOfSlot[slot]] = round;
                }
            }
        }

        completedRounds = round + 1;
        current         = std::move(next);
        current.expansionSource_ = nullptr;
        if (!anyChange) break;
    }
    *this                          = std::move(current);
    slotChangeSink_                = nullptr;
    suspendAndersenIndexing_       = false;
    indexAndersenBytecode(scope, andersen_);
    convergenceRounds_ = completedRounds;
}

void AliasAnalysis::addSummary(AliasMethodSummary summary)
{
    const auto k  = key(summary.method);
    auto       it = byKey_.find(k);
    ca::usize  index;
    if (it == byKey_.end()) {
        index     = summaries_.size();
        byKey_[k] = index;
        summaries_.push_back(std::move(summary));
    }
    else {
        index             = it->second;
        summaries_[index] = std::move(summary);
    }

    //P2：索引与槽记账消费「展开后」的等价事实流（自身事实 + 逐绑定实例化
    //的 callee 展开流），展开域 = expansionSource_（固定点期间=前一轮快照）。
    //visiting 以本方法在展开域的槽位播种：环上重入只贡献自身事实——
    //路径上每方法至多展开一次（环回流事实在收敛点已落位）。
    const AliasAnalysis* source = expansionSource_ != nullptr ? expansionSource_ : this;
    std::vector<ca::usize>                      visiting;
    const auto                                  selfSlotIt = source->byKey_.find(k);
    if (selfSlotIt != source->byKey_.end()) visiting.push_back(selfSlotIt->second);
    std::vector<AliasMethodSummary::FieldWrite> expandedFieldWrites;
    source->expandFieldWrites(summaries_[index], visiting, expandedFieldWrites);
    for (const auto& write : expandedFieldWrites) {
        std::vector<AliasValue> receivers;
        if (write.isStatic) {
            receivers.push_back(AliasValue::unknown());
        }
        else if (write.receiver.isKnown()) {
            receivers.push_back(write.receiver);
        }
        else {
            for (const auto& identity : write.receiver.pointsTo) {
                receivers.push_back(AliasValue::object(*identity));
            }
        }
        for (const auto& receiver : receivers) {
            if (!write.isStatic && isFormalIdentity(receiver)) continue;
            const auto fieldName = fieldKey(receiver, write.field);
            if (slotChangeSink_ != nullptr) slotChangeSink_->insert("F\n" + fieldName);
            auto       fieldIt   = fieldsByKey_.find(fieldName);
            if (fieldIt == fieldsByKey_.end()) {
                fieldsByKey_[fieldName] = fields_.size();
                fields_.push_back({write.field, receiver, write.value});
            }
            else {
                fields_[fieldIt->second].value =
                    mergeAliasValues(fields_[fieldIt->second].value, write.value);
            }
        }
    }
    std::vector<AliasMethodSummary::ArrayWrite> expandedArrayWrites;
    visiting.clear();
    if (selfSlotIt != source->byKey_.end()) visiting.push_back(selfSlotIt->second);
    source->expandArrayWrites(summaries_[index], visiting, expandedArrayWrites);
    for (const auto& write : expandedArrayWrites) {
        std::vector<AliasValue> arrays;
        if (write.array.isKnown()) {
            arrays.push_back(write.array);
        }
        else {
            for (const auto& identity : write.array.pointsTo) {
                arrays.push_back(AliasValue::array(*identity));
            }
        }
        for (const auto& array : arrays) {
            if (isFormalIdentity(array)) continue;
            const auto key = arrayKey(array, write.index);
            if (slotChangeSink_ != nullptr) slotChangeSink_->insert("A\n" + key);
            auto       it  = arrayValues_.find(key);
            arrayValues_[key] =
                it == arrayValues_.end() ? write.value : mergeAliasValues(it->second, write.value);
        }
    }
    std::vector<AliasMethodSummary::ContainerWrite> expandedContainerWrites;
    visiting.clear();
    if (selfSlotIt != source->byKey_.end()) visiting.push_back(selfSlotIt->second);
    source->expandContainerWrites(summaries_[index], visiting, expandedContainerWrites);
    for (const auto& write : expandedContainerWrites) {
        if (!write.container.isKnown() || isFormalIdentity(write.container)) continue;
        const auto ckey = containerKey(write.container, write.key);
        if (slotChangeSink_ != nullptr) slotChangeSink_->insert("C\n" + ckey);
        auto it = containerValues_.find(ckey);
        if (it == containerValues_.end()) {
            containerValues_.emplace(ckey, write.value);
        }
        else {
            it->second = mergeAliasValues(it->second, write.value);
        }
    }
    if (!suspendAndersenIndexing_) indexAndersenSummary(summaries_[index]);
}

void AliasAnalysis::indexAndersenSummary(const AliasMethodSummary& summary)
{
    const auto method    = key(summary.method);
    const auto valueNode = [&](const AliasValue& value) {
        const auto& identity = *value.identity;
        if (identity.compare(0, 6, "param:") == 0 || identity.compare(0, 5, "this:") == 0) {
            return andersen_.intern(method + "\n" + identity);
        }
        return andersen_.intern(method + "\nvalue\n" + identity);
    };
    const auto constrainValue = [&](AndersenPointsToSolver::NameId target,
                                    const AliasValue&             value) {
        if (!value.isKnown() && value.pointsTo.empty()) return;
        const auto& identity = *value.identity;
        if (identity.compare(0, 6, "param:") == 0 || identity.compare(0, 5, "this:") == 0) {
            andersen_.addCopy(target, andersen_.intern(method + "\n" + identity));
        }
        if (value.isKnown() && identity.compare(0, 6, "param:") != 0 &&
            identity.compare(0, 5, "this:") != 0) {
            andersen_.addAddressOf(target, andersen_.intern(identity));
        }
        for (const auto& point : value.pointsTo) {
            if (point->compare(0, 6, "param:") == 0 || point->compare(0, 5, "this:") == 0) continue;
            andersen_.addAddressOf(target, andersen_.intern(*point));
        }
    };

    for (ca::usize index = 0; index < summary.parameters.size(); ++index) {
        constrainValue(andersen_.internIndexed(method + "\nparam-slot\n", static_cast<ca::i64>(index)),
                       summary.parameters[index]);
    }
    const auto returnNode = andersen_.intern(method + "\nreturn");
    constrainValue(returnNode, summary.returnValue);
    for (const auto& write : summary.fieldWrites) {
        if (write.isStatic) continue;
        const auto receiver = valueNode(write.receiver);
        const auto value    = valueNode(write.value);
        constrainValue(receiver, write.receiver);
        constrainValue(value, write.value);
        andersen_.addStore(
            receiver,
            andersen_.intern(write.field.owner + "." + write.field.name + ":" + write.field.desc),
            value);
    }
    andersen_.solve();
}

const AliasValue* AliasAnalysis::fieldValueOf(const std::string& owner, const std::string& name,
                                              const std::string& desc) const
{
    const auto k = fieldKey(AliasValue::unknown(), {owner, name, desc});
    if (recordingQueries_) queryLog_.push_back("F\n" + k);
    auto it = fieldsByKey_.find(k);
    return it == fieldsByKey_.end() ? nullptr : &fields_[it->second].value;
}

const AliasValue* AliasAnalysis::fieldValueOf(const AliasValue& receiver, const std::string& owner,
                                              const std::string& name,
                                              const std::string& desc) const
{
    const auto k = fieldKey(receiver, {owner, name, desc});
    if (recordingQueries_) queryLog_.push_back("F\n" + k);
    if (!receiver.isKnown()) return nullptr;
    auto it = fieldsByKey_.find(k);
    return it == fieldsByKey_.end() ? nullptr : &fields_[it->second].value;
}

std::optional<AliasValue> AliasAnalysis::resolvedFieldValueOf(const AliasValue&  receiver,
                                                              const std::string& owner,
                                                              const std::string& name,
                                                              const std::string& desc) const
{
    if (receiver.isKnown()) {
        const auto* value = fieldValueOf(receiver, owner, name, desc);
        return value ? std::optional<AliasValue>(*value) : std::nullopt;
    }
    if (receiver.pointsTo.empty()) return std::nullopt;

    std::optional<AliasValue> result;
    bool                      missingTarget = false;
    for (const auto& identity : receiver.pointsTo) {
        auto        target = AliasValue::object(*identity);
        const auto* value  = fieldValueOf(target, owner, name, desc);
        if (!value) {
            missingTarget = true;
            continue;
        }
        result = result ? mergeAliasValues(*result, *value) : std::optional<AliasValue>(*value);
    }
    if (!result) return std::nullopt;
    if (missingTarget) *result = mergeAliasValues(*result, AliasValue::unknown());
    return result;
}

const AliasValue* AliasAnalysis::containerValueOf(const std::string& identity) const
{
    if (recordingQueries_) {
        queryLog_.push_back("C\n" + identity + "\n*");
        queryLog_.push_back("C\n" + identity);
    }
    auto it = containerValues_.find(identity + '\n' + "*");
    if (it == containerValues_.end()) it = containerValues_.find(identity);
    return it == containerValues_.end() ? nullptr : &it->second;
}

const AliasValue* AliasAnalysis::containerValueOf(const AliasValue& container,
                                                  const AliasValue& key) const
{
    if (!container.isKnown()) return nullptr;
    if (recordingQueries_) {
        queryLog_.push_back("C\n" + containerKey(container, key));
        if (key.isKnown()) queryLog_.push_back("C\n" + containerKey(container, AliasValue::unknown()));
    }
    auto it = containerValues_.find(containerKey(container, key));
    if (it == containerValues_.end() && key.isKnown()) {
        it = containerValues_.find(containerKey(container, AliasValue::unknown()));
    }
    return it == containerValues_.end() ? nullptr : &it->second;
}

const AliasValue* AliasAnalysis::arrayValueOf(const AliasValue& array,
                                              const AliasValue& index) const
{
    if (!array.isKnown()) return nullptr;
    if (recordingQueries_) {
        queryLog_.push_back("A\n" + arrayKey(array, index));
        if (index.isKnown()) queryLog_.push_back("A\n" + *array.identity + "\n*");
    }
    auto it = arrayValues_.find(arrayKey(array, index));
    if (it == arrayValues_.end() && index.isKnown()) {
        it = arrayValues_.find(*array.identity + '\n' + "*");
    }
    return it == arrayValues_.end() ? nullptr : &it->second;
}

std::optional<AliasValue> AliasAnalysis::resolvedArrayValueOf(const AliasValue& array,
                                                              const AliasValue& index) const
{
    if (array.isKnown()) {
        const auto* value = arrayValueOf(array, index);
        return value ? std::optional<AliasValue>(*value) : std::nullopt;
    }
    if (array.pointsTo.empty()) return std::nullopt;

    std::optional<AliasValue> result;
    bool                      missingTarget = false;
    for (const auto& identity : array.pointsTo) {
        const auto* value = arrayValueOf(AliasValue::array(*identity), index);
        if (!value) {
            missingTarget = true;
            continue;
        }
        result = result ? mergeAliasValues(*result, *value) : std::optional<AliasValue>(*value);
    }
    if (!result) return std::nullopt;
    if (missingTarget) *result = mergeAliasValues(*result, AliasValue::unknown());
    return result;
}

std::vector<std::string> AliasAnalysis::possibleTypesOf(const AliasValue& value) const
{
    std::vector<const std::string*> identities = value.pointsTo;
    if (identities.empty() && value.isKnown()) identities.push_back(value.identity);
    std::vector<std::string> types;
    for (const auto* identity : identities) {
        if (identity->rfind("new:", 0) != 0) continue;
        const auto separator = identity->rfind(':');
        if (separator == std::string::npos || separator + 1 >= identity->size()) continue;
        types.push_back(identity->substr(separator + 1));
    }
    std::sort(types.begin(), types.end());
    types.erase(std::unique(types.begin(), types.end()), types.end());
    return types;
}

std::vector<std::string> AliasAnalysis::possibleReturnTypesOf(const std::string& owner,
                                                              const std::string& name,
                                                              const std::string& desc) const
{
    const auto* summary = summaryOf(owner, name, desc);
    return summary ? possibleTypesOf(summary->returnValue) : std::vector<std::string>{};
}

std::vector<std::string> AliasAnalysis::andersenReturnPointsToOf(const std::string& owner,
                                                                 const std::string& name,
                                                                 const std::string& desc) const
{
    return andersen_.pointsTo(key({owner, name, desc}) + "\nreturn");
}

std::vector<std::string> AliasAnalysis::andersenParameterPointsToOf(const std::string& owner,
                                                                    const std::string& name,
                                                                    const std::string& desc,
                                                                    ca::usize parameter) const
{
    return andersen_.pointsTo(key({owner, name, desc}) + "\nparam:" + std::to_string(parameter));
}

const AliasMethodSummary* AliasAnalysis::summaryOf(const std::string& owner,
                                                   const std::string& name,
                                                   const std::string& desc) const
{
    const auto it = byKey_.find(key({owner, name, desc}));
    if (recordingQueries_) queryLog_.push_back("M\n" + key({owner, name, desc}));
    return it == byKey_.end() ? nullptr : &summaries_[it->second];
}

const AliasValue* AliasAnalysis::reflectionReceiverOf(const std::string& owner,
                                                      const std::string& name,
                                                      const std::string& desc,
                                                      ca::i32            instructionIndex) const
{
    const auto* summary = summaryOf(owner, name, desc);
    if (!summary) return nullptr;
    for (const auto& fact : summary->reflectionReceivers) {
        if (fact.instructionIndex == instructionIndex) return &fact.receiver;
    }
    return nullptr;
}

AliasAnalysisResult::Result AliasAnalysisResult::run(ResourceContainer&     container,
                                                     GlobalAnalysisManager& gam)
{
    AliasAnalysis result;
    result.build(container, gam.arena());
    return result;
}

}   //namespace mjt::base
