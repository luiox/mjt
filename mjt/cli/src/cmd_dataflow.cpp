/// @file        cmd_dataflow.cpp
/// @brief       `mjt dataflow du|live` 命令核心：方法内数据流查询。
///
/// 基于 mjbase/asm MethodDefUse（slot 级 def-use：入参 Parameter 定义、store/iinc 定义、load 边、栈使用边、字段访问事实；异常边走 DomainEngine 精确档）与 LivenessView（局部槽后向 may-liveness，点级）。输出是 def 标号链（param[k] / i<slot>）与局部槽级活跃。du 的 ok / truncated 状态字段如实呈现：ok = false 时边集为空但不作为失败退出。
#include "commands.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/asm/analyzer/def_use.hpp"
#include "mjbase/asm/sem/liveness_view.hpp"

#include "method_analysis.hpp"
#include "shared/i18n_errors.hpp"
#include "shared/json_util.hpp"
#include "shared/output_sink.hpp"

namespace mjt::cli {
namespace {

using jbc::ir::InsnId;
using mjbase::asm_analyzer::analyzeDefUse;
using mjbase::asm_analyzer::DefId;
using mjbase::asm_analyzer::MethodDefUse;

/// DataflowError 便捷构造（i18n 渲染）。
DataflowError dataflowError(DataflowError::Kind kind, const char* key,
                            std::initializer_list<std::string_view> args = {})
{
    DataflowError error;
    error.kind    = kind;
    error.message = shared::trfStr(key, args);
    return error;
}

/// method_analysis::load 失败 → DataflowError（文案已渲染，只拆退出码分支）。
DataflowError forwardedError(method_analysis::LoadFailure failure)
{
    DataflowError error;
    error.kind    = failure.usage ? DataflowError::Kind::Usage : DataflowError::Kind::Runtime;
    error.message = std::move(failure.message);
    return error;
}

/// JSON 布尔字面量（const char* 三元直接参与 `"text" + cond` 拼接会退化为指针加法，统一转成 std::string）。
std::string jbool(bool value)
{
    return value ? "true" : "false";
}

/// jbc 助记符文本（未知操作码回落 "?"）。
std::string opcodeName(jbc::core::Op op)
{
    const char* mnemonic = jbc::core::mnemonicOf(op);
    return mnemonic != nullptr ? std::string{mnemonic} : std::string{"?"};
}

/// 指令原 pc（kNoPc → -1，报告统一用 -1 表示无源位置）。
ca::i32 pcOf(const jbc::ir::CodeUnit& code, InsnId id)
{
    const jbc::ir::Insn* insn = code.dom.peek(id);
    if (insn == nullptr || insn->srcPc == jbc::ir::kNoPc) return -1;
    return static_cast<ca::i32>(insn->srcPc);
}

/// DefId → 报告标号（instruction 定义经 MethodDefUse::instructionOf 取回完整句柄后补 pc 与助记符；Parameter/未知保持空 opcode）。
DuDefRef defRefOf(const jbc::ir::CodeUnit& code, const MethodDefUse& defUse, DefId def)
{
    DuDefRef ref;
    ref.kindParameter = def.kind == DefId::Kind::Parameter;
    ref.slot          = def.id;
    if (!ref.kindParameter) {
        const jbc::ir::InsnId* handle = defUse.instructionOf(def);
        const jbc::ir::Insn*   insn   = handle != nullptr ? code.dom.peek(*handle) : nullptr;
        if (insn != nullptr) {
            ref.pc     = insn->srcPc == jbc::ir::kNoPc ? -1 : static_cast<ca::i32>(insn->srcPc);
            ref.opcode = opcodeName(insn->op);
        }
    }
    return ref;
}

/// 使用点 → 报告行（slot kind / index / 溢出标记直传）。
DuUsePoint usePointOf(const jbc::ir::CodeUnit& code, const mjbase::asm_analyzer::DefUseEdge& edge)
{
    DuUsePoint point;
    point.insnSlot  = edge.use.slot;
    point.pc        = pcOf(code, edge.use);
    point.opcode    = opcodeName(code.dom.peek(edge.use)->op);
    point.stack     = edge.slot == mjbase::asm_analyzer::DefUseSlot::Stack;
    point.index     = edge.index;
    point.truncated = edge.truncated;
    return point;
}

/// 方法描述符 → 入参局部槽号表（实例方法补 0 号 this；long/double 占双槽）。
std::vector<ca::u32> parameterSlots(const std::string& descriptor, bool instance)
{
    std::vector<ca::u32> slots;
    ca::u32              slot = 0;
    if (instance) {
        slots.push_back(0);
        slot = 1;
    }
    const auto paren = descriptor.find(')');
    std::size_t pos  = descriptor.find('(');
    if (pos == std::string::npos) return slots;
    pos += 1;
    while (pos < descriptor.size() && pos < paren) {
        slots.push_back(slot);
        if (descriptor[pos] == 'J' || descriptor[pos] == 'D') {
            slot += 2;
        }
        else if (descriptor[pos] == 'L')
        {
            slot += 1;
            pos = descriptor.find(';', pos);
            if (pos == std::string::npos) break;
        }
        else if (descriptor[pos] == '[')
        {
            slot += 1;
            while (pos < descriptor.size() && descriptor[pos] == '[') pos += 1;
            if (pos < descriptor.size() && descriptor[pos] == 'L') {
                pos = descriptor.find(';', pos);
                if (pos == std::string::npos) break;
            }
        }
        else {
            slot += 1;   // 其余基类型（B/C/F/I/S/Z）各占一槽
        }
        pos += 1;
    }
    return slots;
}

/// 渲染前准备：load + def-use 分析（du 两处共用）。
ca::Result<std::pair<DuReport, MethodDefUse>, DataflowError> analyzeDu(
    const std::string& inputPath, const std::string& className, const std::string& methodSpec)
{
    auto loaded = method_analysis::load(inputPath, className, methodSpec, "mjt.cmd.dataflow",
                                        /*requireBody=*/true);
    if (loaded.is_err()) return ca::Err(forwardedError(std::move(loaded).unwrap_err()));
    auto target = std::move(loaded).unwrap();

    const jbc::ir::ClassSession& session = *target.code.session();
    const jbc::analysis::MethodSig sig{/*isStatic=*/!target.instance, target.descriptor,
                                       target.methodName,
                                       session.unit().symbols().str(session.unit().thisClass)};
    auto defUse = analyzeDefUse(session.backing().cp, *target.code, sig);

    DuReport report;
    report.className            = target.className;
    report.methodName           = target.methodName;
    report.descriptor           = target.descriptor;
    report.instance             = target.instance;
    report.ok                   = defUse.ok();
    report.truncated            = defUse.truncated();
    report.unresolvedFieldRefs  = defUse.hasUnresolvedFieldRefs();
    report.edgeCount            = defUse.edgeCount();
    if (!defUse.ok()) return ca::Ok(std::make_pair(std::move(report), std::move(defUse)));

    // 定义枚举：入参槽（签名播种，恒有行）+ 边集 / 局部写出现过的指令定义（dead store 无出边也列入，负向结论即它没有使用者）。入参定义不再从边集收（上方播种已覆盖，重复收会产出重复链行）。DefId 全序（Parameter 先、slot 升序）保证确定序。
    std::set<DefId> defs;
    for (const auto& edge : defUse.edges()) {
        if (edge.def.kind == DefId::Kind::Instruction) defs.insert(edge.def);
    }
    for (const auto& access : defUse.localAccesses()) {
        if (access.kind == mjbase::asm_analyzer::LocalAccess::Kind::Write)
            defs.insert(DefId::instruction(access.insn));
    }

    for (const ca::u32 slot : parameterSlots(target.descriptor, target.instance))
        report.chains.push_back({DuDefRef{true, slot, -1, {}}, {}});
    for (const DefId def : defs)
        report.chains.push_back({defRefOf(*target.code, defUse, def), {}});

    for (auto& chain : report.chains) {
        const DefId key = chain.def.kindParameter ? DefId::parameter(chain.def.slot)
                                                  : DefId::instruction(InsnId{0, chain.def.slot, 0});
        for (const mjbase::asm_analyzer::DefUseEdge* edge : defUse.usesOf(key))
            chain.uses.push_back(usePointOf(*target.code, *edge));
    }

    for (const auto& fact : defUse.fieldAccesses()) {
        DuFieldFact row;
        row.insnSlot  = fact.insn.slot;
        row.pc        = pcOf(*target.code, fact.insn);
        const jbc::ir::Insn* body = target.code->dom.peek(fact.insn);
        row.opcode    = body != nullptr ? opcodeName(body->op) : std::string{"?"};
        row.owner     = std::string{fact.owner};
        row.name      = std::string{fact.name};
        row.descriptor = std::string{fact.desc};
        row.write     = fact.write;
        row.isStatic  = fact.isStatic;
        for (ca::u32 i = 0; i < fact.objectDefs.size(); ++i)
            row.objectDefs.push_back(defRefOf(*target.code, defUse, fact.objectDefs.at(i)));
        for (ca::u32 i = 0; i < fact.valueDefs.size(); ++i)
            row.valueDefs.push_back(defRefOf(*target.code, defUse, fact.valueDefs.at(i)));
        row.truncated = fact.truncated();
        report.fieldAccesses.push_back(std::move(row));
    }

    return ca::Ok(std::make_pair(std::move(report), std::move(defUse)));
}

/// def 标号文本：参数槽 "param[0]"（实例方法 0 号 "param[0]=this"），指令定义 "i<slot>(<mnemonic>)"。
std::string defText(const DuDefRef& def, bool instance)
{
    if (def.kindParameter) {
        if (instance && def.slot == 0) return "param[0]=this";
        return "param[" + std::to_string(def.slot) + "]";
    }
    return "i" + std::to_string(def.slot) + "(" + def.opcode + ")";
}

/// use 点列表文本："i2(iadd)@stack0"；溢出边加 "!"；空为 "-"。
std::string usesText(const std::vector<DuUsePoint>& uses)
{
    if (uses.empty()) return "-";
    std::string text;
    for (const auto& use : uses) {
        if (!text.empty()) text += ", ";
        text += "i" + std::to_string(use.insnSlot) + "(" + use.opcode + ")" +
                (use.stack ? "@stack" : "@local") + std::to_string(use.index);
        if (use.truncated) text += "!";
    }
    return text;
}

/// DuDefRef → JSON 片段（@p instance 只用于参数的 this 注记；静态方法 0 号参数不是 this）。
std::string defJson(const DuDefRef& def, bool instance)
{
    std::string out = def.kindParameter ? "{\"kind\":\"parameter\",\"slot\":" +
                                            std::to_string(def.slot)
                                        : "{\"kind\":\"instruction\",\"slot\":" +
                                            std::to_string(def.slot);
    if (def.kindParameter) {
        if (instance && def.slot == 0) out += ",\"this\":true";
    }
    else {
        out += ",\"pc\":" + std::to_string(def.pc) + ",\"opcode\":\"" + def.opcode + "\"";
    }
    out += "}";
    return out;
}

}   // namespace

ca::Result<DuReport, DataflowError> collectDataflowDu(const std::string& inputPath,
                                                      const std::string& className,
                                                      const std::string& methodSpec)
{
    auto analyzed = analyzeDu(inputPath, className, methodSpec);
    if (analyzed.is_err()) return ca::Err(std::move(analyzed).unwrap_err());
    return ca::Ok(std::move(analyzed).unwrap().first);
}

void renderDuText(const DuReport& report, OutputSink& sink)
{
    sink.printf("class:  %s\n", report.className.c_str());
    sink.printf("method: %s %s\n", report.methodName.c_str(), report.descriptor.c_str());
    sink.printf("chains: %zu  edges: %zu  ok: %s  truncated: %s\n", report.chains.size(),
                static_cast<std::size_t>(report.edgeCount), report.ok ? "yes" : "no",
                report.truncated ? "yes" : "no");
    sink.printf("%-18s %s\n", "def", "uses");
    for (const auto& chain : report.chains)
        sink.printf("%-18s %s\n", defText(chain.def, report.instance).c_str(),
                    usesText(chain.uses).c_str());
}

void renderDuJson(const DuReport& report, OutputSink& sink)
{
    std::string out = "{\"class\":\"" + json::escape(report.className) +
                      "\",\"method\":\"" + json::escape(report.methodName) +
                      "\",\"descriptor\":\"" + json::escape(report.descriptor) +
                      "\",\"instance\":" + jbool(report.instance) +
                      ",\"ok\":" + jbool(report.ok) +
                      ",\"truncated\":" + jbool(report.truncated) +
                      ",\"unresolved_field_refs\":" +
                      jbool(report.unresolvedFieldRefs) +
                      ",\"edges\":" + std::to_string(report.edgeCount) + ",\"chains\":[";
    for (std::size_t i = 0; i < report.chains.size(); ++i) {
        const auto& chain = report.chains[i];
        out += (i == 0 ? "" : ",") + std::string{"{\"def\":"} +
               defJson(chain.def, report.instance) + ",\"uses\":[";
        for (std::size_t j = 0; j < chain.uses.size(); ++j) {
            const auto& use = chain.uses[j];
            out += (j == 0 ? "" : ",");
            out += "{\"insn\":" + std::to_string(use.insnSlot) +
                   ",\"pc\":" + std::to_string(use.pc) +
                   ",\"opcode\":\"" + json::escape(use.opcode) + "\"" +
                   ",\"slot\":\"" + (use.stack ? "stack" : "local") + "\"" +
                   ",\"index\":" + std::to_string(use.index) +
                   ",\"truncated\":" + jbool(use.truncated) + "}";
        }
        out += "]}";
    }
    out += "],\"field_accesses\":[";
    for (std::size_t i = 0; i < report.fieldAccesses.size(); ++i) {
        const auto& fact = report.fieldAccesses[i];
        out += (i == 0 ? "" : ",");
        out += "{\"insn\":" + std::to_string(fact.insnSlot) +
               ",\"pc\":" + std::to_string(fact.pc) +
               ",\"opcode\":\"" + json::escape(fact.opcode) + "\"" +
               ",\"owner\":\"" + json::escape(fact.owner) + "\"" +
               ",\"name\":\"" + json::escape(fact.name) + "\"" +
               ",\"descriptor\":\"" + json::escape(fact.descriptor) + "\"" +
               ",\"write\":" + jbool(fact.write) +
               ",\"static\":" + jbool(fact.isStatic) + ",\"object_defs\":[";
        // 字段事实的 object/value 到达定义恒为栈值定义（指令），instance 注记不适用（slot 级模型：参数定义只出现在局部槽读边）。
        for (std::size_t j = 0; j < fact.objectDefs.size(); ++j)
            out += (j == 0 ? "" : ",") + defJson(fact.objectDefs[j], false);
        out += "],\"value_defs\":[";
        for (std::size_t j = 0; j < fact.valueDefs.size(); ++j)
            out += (j == 0 ? "" : ",") + defJson(fact.valueDefs[j], false);
        out += "],\"truncated\":" + jbool(fact.truncated) + "}";
    }
    out += "]}\n";
    sink.write(out);
}

ca::Result<LiveReport, DataflowError> collectDataflowLive(const std::string& inputPath,
                                                          const std::string& className,
                                                          const std::string& methodSpec)
{
    auto loaded = method_analysis::load(inputPath, className, methodSpec, "mjt.cmd.dataflow",
                                        /*requireBody=*/true);
    if (loaded.is_err()) return ca::Err(forwardedError(std::move(loaded).unwrap_err()));
    auto target = std::move(loaded).unwrap();

    // LivenessView 交付的活跃槽位表按 InsnId 查询（构建时快照）；无效句柄探测式返回 false，不抛错。
    auto built = mjbase::asm_sem::LivenessView::build(*target.code);
    if (built.is_err()) {
        const auto error = std::move(built).unwrap_err();
        return ca::Err(dataflowError(DataflowError::Kind::Runtime,
                                     "mjt.cmd.dataflow.err.invalid_class_file",
                                     {inputPath, mjbase::asm_cfg::to_string(error)}));
    }
    const auto view = std::move(built).unwrap();

    LiveReport report;
    report.className  = target.className;
    report.methodName = target.methodName;
    report.descriptor = target.descriptor;
    report.instance   = target.instance;
    report.slotCount  = view.slotCount();

    const auto& order = target.code->dom.order();
    for (std::size_t i = 0; i < order.size(); ++i) {
        const InsnId id = order[i];
        LivePoint point;
        point.insnSlot = id.slot;
        point.pc       = pcOf(*target.code, id);
        const jbc::ir::Insn* insn = target.code->dom.peek(id);
        if (insn != nullptr) point.opcode = opcodeName(insn->op);
        if (i == 0) {
            for (ca::u32 slot = 0; slot < view.slotCount(); ++slot)
                if (view.localLiveBefore(id, slot)) report.entryLive.push_back(slot);
        }
        for (ca::u32 slot = 0; slot < view.slotCount(); ++slot)
            if (view.localLiveBefore(id, slot)) point.liveBefore.push_back(slot);
        point.liveAfter = view.liveAfterSlots(id);
        report.points.push_back(std::move(point));
    }

    return ca::Ok(std::move(report));
}

namespace {

/// 槽位列表文本："0, 2"；空为 "-"。
std::string slotsText(const std::vector<ca::u32>& slots)
{
    if (slots.empty()) return "-";
    std::string text;
    for (const ca::u32 slot : slots) {
        if (!text.empty()) text += ", ";
        text += std::to_string(slot);
    }
    return text;
}

}   // namespace

void renderLiveText(const LiveReport& report, OutputSink& sink)
{
    sink.printf("class:  %s\n", report.className.c_str());
    sink.printf("method: %s %s\n", report.methodName.c_str(), report.descriptor.c_str());
    sink.printf("slots: %u  entry live: %s\n", static_cast<unsigned>(report.slotCount),
                slotsText(report.entryLive).c_str());
    sink.printf("%-8s %-6s %-14s %-16s %s\n", "insn", "pc", "opcode", "before", "after");
    for (const auto& point : report.points) {
        sink.printf("i%-7u %-6d %-14s %-16s %s\n", point.insnSlot, point.pc,
                    point.opcode.c_str(), slotsText(point.liveBefore).c_str(),
                    slotsText(point.liveAfter).c_str());
    }
}

void renderLiveJson(const LiveReport& report, OutputSink& sink)
{
    auto ids = [](const std::vector<ca::u32>& slots) {
        std::string out = "[";
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (i != 0) out += ",";
            out += std::to_string(slots[i]);
        }
        return out + "]";
    };
    std::string out = "{\"class\":\"" + json::escape(report.className) +
                      "\",\"method\":\"" + json::escape(report.methodName) +
                      "\",\"descriptor\":\"" + json::escape(report.descriptor) +
                      "\",\"instance\":" + jbool(report.instance) +
                      ",\"slots\":" + std::to_string(report.slotCount) +
                      ",\"entry_live\":" + ids(report.entryLive) + ",\"points\":[";
    for (std::size_t i = 0; i < report.points.size(); ++i) {
        const auto& point = report.points[i];
        out += (i == 0 ? "" : ",");
        out += "{\"insn\":" + std::to_string(point.insnSlot) +
               ",\"pc\":" + std::to_string(point.pc) +
               ",\"opcode\":\"" + json::escape(point.opcode) + "\"" +
               ",\"live_before\":" + ids(point.liveBefore) +
               ",\"live_after\":" + ids(point.liveAfter) + "}";
    }
    out += "]}\n";
    sink.write(out);
}

}   // namespace mjt::cli
