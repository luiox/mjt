/// @file        cmd_cfg.cpp
/// @brief       `mjt cfg` 命令核心：单方法块级 CFG 的 Graphviz dot 导出。
///
/// 基于 mjbase/asm ControlFlowGraph（切块、边分类、异常边）、CfgTraversal（RPO、可达集）、DominatorTree（CHK 逆后序 idom）。输出是块级视图：节点 = 基本块、块内 jbc 指令概览（操作码名从 jbc 拿，属性引用经常量池解析成文本）。方法无方法体（abstract / native）时输出明确空图，注释行写明原因，退出码 0。
#include "commands.hpp"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "jbc/core/opcode.hpp"

#include "mjbase/asm/cfg/cfg_analysis.hpp"
#include "mjbase/asm/cfg/control_flow_graph.hpp"

#include "method_analysis.hpp"
#include "shared/i18n_errors.hpp"
#include "shared/output_sink.hpp"

namespace mjt::cli {
namespace {

using jbc::core::Op;
using jbc::core::Shape;
using jbc::ir::Insn;
using jbc::ir::InsnId;
using mjbase::asm_cfg::BlockId;
using mjbase::asm_cfg::ControlFlowGraph;
using mjbase::asm_cfg::EdgeKind;

/// CfgError 便捷构造（i18n 渲染）。
CfgError cfgError(CfgError::Kind kind, const char* key,
                  std::initializer_list<std::string_view> args = {})
{
    CfgError error;
    error.kind    = kind;
    error.message = shared::trfStr(key, args);
    return error;
}

/// EdgeKind → dot 边标注稳定名（EdgeKind 枚举序）。
const char* edgeKindName(EdgeKind kind)
{
    switch (kind) {
    case EdgeKind::FallThrough: return "fallthrough";
    case EdgeKind::Branch:      return "branch";
    case EdgeKind::Condition:   return "condition";
    case EdgeKind::Switch:      return "switch";
    case EdgeKind::Exception:   return "exception";
    }
    return "?";
}

/// ldc float/double 无现成数值化表示：按位模式十六进制无损呈现（确定序）。
template <typename T>
std::string hexBits(T bits)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%llx",
                  static_cast<unsigned long long>(bits));
    return buffer;
}

/// dot 标签文本转义：反斜杠与双引号（其余字节原样；助记符 / 内部名不含控制字符，ldc 字符串常量的其它字节按原样进 dot，仅这两个字符破坏语法）。
std::string dotEscape(const std::string& text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

/// 成员引用文本：`owner.name desc`（invoke / 字段访问家族共用同一拼接形式）。
std::string memberText(const jbc::ir::ClassSession& session, const Insn& insn)
{
    jbc::ir::ClassSession::MemberSem sem;
    if (!session.memberOf(insn, sem)) return {};
    std::string text{sem.owner};
    if (!text.empty()) text += '.';
    text += std::string{sem.name};
    text += ' ';
    text += std::string{sem.desc};
    return text;
}

/// CP 引用文本（经常量池解析；解析失败回落 `cp#<idx>`）。
std::string cpOperandText(const jbc::ir::ClassSession& session, const Insn& insn)
{
    std::string text = memberText(session, insn);
    if (!text.empty()) return text;

    const auto className = session.classOf(insn);
    if (!className.empty()) {
        std::string out{className};
        if (insn.op == Op::MULTIANEWARRAY) {
            out += ' ';
            out += std::to_string(insn.a);
        }
        return out;
    }

    jbc::ir::ClassSession::ConstSem constant;
    if (session.constOf(insn, constant)) {
        switch (constant.kind) {
        case jbc::ir::ClassSession::ConstSem::Kind::Int:
            return std::to_string(static_cast<ca::i32>(constant.num));
        case jbc::ir::ClassSession::ConstSem::Kind::Long:
            return std::to_string(constant.num) + 'L';
        case jbc::ir::ClassSession::ConstSem::Kind::Float:
            return hexBits(static_cast<ca::u32>(constant.bits)) + 'f';
        case jbc::ir::ClassSession::ConstSem::Kind::Double:
            return hexBits(constant.bits) + 'd';
        case jbc::ir::ClassSession::ConstSem::Kind::String:
            return '"' + dotEscape(std::string{constant.str}) + '"';
        case jbc::ir::ClassSession::ConstSem::Kind::Class:
            return std::string{constant.str};
        case jbc::ir::ClassSession::ConstSem::Kind::MethodType:
            return "type " + std::string{constant.str};
        case jbc::ir::ClassSession::ConstSem::Kind::MethodHandle: return "handle";
        case jbc::ir::ClassSession::ConstSem::Kind::Dynamic:      return "dynamic";
        }
    }
    return "cp#" + std::to_string(insn.cp);
}

/// 块号渲染：标签解析到块返回 "B<n>"；解析不到（End 边界 / 过期）返回 "?"。
std::string blockRef(const ControlFlowGraph& cfg, const jbc::ir::CodeDom& dom,
                     jbc::ir::LabelId label)
{
    const BlockId block = cfg.blockOfLabel(dom, label);
    if (!block.valid()) return "?";
    return "B" + std::to_string(block.index);
}

/// 单条指令的文本（mnemonic + 操作数；分支目标解析为块号）。
std::string renderInsnText(const jbc::ir::ClassSession& session, const jbc::ir::CodeUnit& code,
                           const ControlFlowGraph& cfg, InsnId id)
{
    const Insn* insn = code.dom.peek(id);
    if (insn == nullptr) return "?";
    const char* mnemonic = jbc::core::mnemonicOf(insn->op);
    std::string text{mnemonic != nullptr ? mnemonic : "?"};

    switch (jbc::core::shapeOf(insn->op)) {
    case Shape::Imm1:
    case Shape::Imm2:
    case Shape::Atype1:
    case Shape::Local1:
        text += ' ' + std::to_string(insn->a);
        break;
    case Shape::Iinc1:
        text += ' ' + std::to_string(insn->a) + ' ' + std::to_string(insn->b);
        break;
    case Shape::CP1:
    case Shape::CP2:
    case Shape::CP2Count:
    case Shape::CP2Zero2:
    case Shape::CP2Dims: {
        const std::string operand = cpOperandText(session, *insn);
        if (!operand.empty()) text += ' ' + operand;
        break;
    }
    case Shape::Branch2:
    case Shape::Branch4:
        text += " -> " + blockRef(cfg, code.dom, insn->target);
        break;
    case Shape::TableSwitch: {
        text += " low=" + std::to_string(insn->a) + " default=" +
                blockRef(cfg, code.dom, insn->target);
        for (std::size_t i = 0; i < insn->targets.size(); ++i) {
            text += ' ' + std::to_string(insn->a + static_cast<ca::i32>(i)) + '=' +
                    blockRef(cfg, code.dom, insn->targets[i]);
        }
        break;
    }
    case Shape::LookupSwitch: {
        text += " default=" + blockRef(cfg, code.dom, insn->target);
        for (std::size_t i = 0; i < insn->targets.size(); ++i) {
            text += ' ' + std::to_string(insn->keys[i]) + '=' +
                    blockRef(cfg, code.dom, insn->targets[i]);
        }
        break;
    }
    case Shape::None:
        break;
    }
    return text;
}

}   // namespace

ca::Result<CfgReport, CfgError> collectCfg(const std::string& inputPath,
                                           const std::string& className,
                                           const std::string& methodSpec)
{
    auto loaded = method_analysis::load(inputPath, className, methodSpec, "mjt.cmd.cfg",
                                        /*requireBody=*/false);
    if (loaded.is_err()) {
        // 失败文案已按 i18n 渲染（method_analysis 统一构造），此处只拆退出码分支：usage = 选择器问题 → 退出码 1，其余 → 退出码 2。
        auto failure = std::move(loaded).unwrap_err();
        CfgError error;
        error.kind    = failure.usage ? CfgError::Kind::Usage : CfgError::Kind::Runtime;
        error.message = std::move(failure.message);
        return ca::Err(std::move(error));
    }
    auto target = std::move(loaded).unwrap();

    CfgReport report;
    report.className  = target.className;
    report.methodName = target.methodName;
    report.descriptor = target.descriptor;
    report.instance   = target.instance;
    report.hasBody    = target.hasBody;
    if (!target.hasBody) return ca::Ok(std::move(report));

    auto built = ControlFlowGraph::build(*target.code);
    if (built.is_err()) {
        const auto error = std::move(built).unwrap_err();
        return ca::Err(cfgError(CfgError::Kind::Runtime, "mjt.cmd.cfg.err.invalid_class_file",
                                {inputPath, mjbase::asm_cfg::to_string(error)}));
    }
    const auto cfg = std::move(built).unwrap();

    // 块表 + 块内指令概览（jbc InsnId 程序序；操作码名与操作数在 collect 期经会话 CP 语义视图渲染，报告自持有）。
    for (const auto& block : cfg.blocks()) {
        CfgBlock row;
        row.id         = block.id.index;
        row.entry      = block.id.index == 0;
        row.terminated = cfg.isTerminated(block.id);
        for (const InsnId insn : block.insns) {
            CfgInsnLine line;
            const Insn* body = target.code->dom.peek(insn);
            line.pc = body != nullptr ? body->srcPc : jbc::ir::kNoPc;
            line.text = renderInsnText(*target.code.session(), *target.code, cfg, insn);
            row.insns.push_back(std::move(line));
        }
        report.blocks.push_back(std::move(row));
    }

    // 异常边 catch 类型回查 exc 表（catchCp 0 = any）。
    const jbc::ir::CodeUnit& code = *target.code;
    for (ca::u32 from = 0; from < cfg.blocks().size(); ++from) {
        for (const auto& edge : cfg.succEdgesOf(BlockId{from})) {
            CfgEdge row;
            row.from = edge.source.index;
            row.to   = edge.target.index;
            row.kind = edgeKindName(edge.kind);
            if (edge.kind == EdgeKind::Exception) {
                const ca::u32 catchCp =
                    edge.excIndex < code.exc.size() ? code.exc[edge.excIndex].catchCp : 0;
                row.detail = catchCp == 0
                                 ? "any"
                                 : std::string{target.code.session()->backing().className(catchCp)};
            }
            report.edges.push_back(std::move(row));
        }
    }

    // 边计数（kind 稳定名升序 = EdgeKind 枚举序；恒含全部五类便于对账）。
    for (const EdgeKind kind : {EdgeKind::FallThrough, EdgeKind::Branch, EdgeKind::Condition,
                                EdgeKind::Switch, EdgeKind::Exception}) {
        report.edgeCounts.emplace_back(edgeKindName(kind), cfg.edgeCount(kind));
    }

    // RPO 与支配树（可达块才有 idom；入口块自支配）。
    const mjbase::asm_cfg::CfgTraversal traversal(cfg);
    for (const BlockId block : traversal.reversePostorder())
        report.reversePostorder.push_back(block.index);
    mjbase::asm_cfg::DominatorTree dominators;
    dominators.compute(cfg);
    for (ca::u32 index = 0; index < cfg.blockCount(); ++index) {
        const BlockId block{index};
        CfgDominator row;
        row.block     = index;
        row.reachable = dominators.isReachable(block);
        if (row.reachable) {
            const BlockId idom = dominators.idom(block);
            row.idom         = idom.valid() ? static_cast<ca::i32>(idom.index) : -1;
            row.entrySelfDom = idom.valid() && idom == block;
        }
        report.dominators.push_back(row);
    }

    return ca::Ok(std::move(report));
}

void renderCfgDot(const CfgReport& report, OutputSink& sink)
{
    const std::string graphName =
        dotEscape(report.className + "." + report.methodName + report.descriptor);
    sink.printf("digraph \"%s\" {\n", graphName.c_str());
    sink.printf("  // class: %s\n", dotEscape(report.className).c_str());
    sink.printf("  // method: %s %s (%s)\n", dotEscape(report.methodName).c_str(),
                dotEscape(report.descriptor).c_str(), report.instance ? "instance" : "static");
    if (!report.hasBody) {
        // 明确空图：abstract / native 无方法体，无 CFG 可画；退出码 0，注释行写明原因，不静默。
        sink.printf("  // no body (abstract/native): no CFG\n");
        sink.printf("}\n");
        return;
    }

    sink.printf("  // entry: B0\n");
    std::string rpo;
    for (const ca::u32 block : report.reversePostorder) {
        if (!rpo.empty()) rpo += ", ";
        rpo += "B" + std::to_string(block);
    }
    sink.printf("  // rpo: %s\n", rpo.c_str());
    for (const auto& dominator : report.dominators) {
        if (!dominator.reachable) continue;
        const std::string from =
            dominator.entrySelfDom
                ? "(entry)"
                : (dominator.idom >= 0 ? "B" + std::to_string(dominator.idom) : "?");
        sink.printf("  // idom: B%u <- %s\n", dominator.block, from.c_str());
    }

    for (const auto& block : report.blocks) {
        std::string label =
            "B" + std::to_string(block.id) + (block.entry ? " (entry)" : "");
        for (const auto& insn : block.insns) {
            const std::string pc = insn.pc == jbc::ir::kNoPc
                                       ? std::string{"-"}
                                       : std::to_string(insn.pc);
            label += "\\l  " + pc + ": " + dotEscape(insn.text);
        }
        label += "\\l";
        sink.printf("  B%u [label=\"%s\" shape=box];\n", block.id, label.c_str());
    }

    for (const auto& edge : report.edges) {
        std::string attr = "label=\"" + std::string{edge.kind};
        if (!edge.detail.empty()) attr += ": " + dotEscape(edge.detail);
        attr += "\"";
        if (std::string{edge.kind} == "exception") attr += " style=dashed";
        sink.printf("  B%u -> B%u [%s];\n", edge.from, edge.to, attr.c_str());
    }

    sink.printf("}\n");
}

}   // namespace mjt::cli
