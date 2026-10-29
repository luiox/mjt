//class_render：cmd_info 与 cmd_disasm 共享的字节码渲染辅助函数实现。
//
//实现搬迁自 cmd_disasm.cpp（该处版本最完整，覆盖 INTEGER/FLOAT/LONG/DOUBLE），
//cmd_info.cpp 里原先内联的简化 resolve 注释已被这里统一取代。

#include "class_render.hpp"

#include <cstdio>
#include <string>

#include "output_sink.hpp"
#include "mjbase/mapping/identifier_util.hpp"
#include "mjbase/renamer/shadow_alias.hpp"
#include "shadow_display.hpp"

#include "jclassfile/model/constant/const_ref.hpp"
#include "jclassfile/model/constant/cp_class.hpp"
#include "jclassfile/model/constant/cp_double.hpp"
#include "jclassfile/model/constant/cp_float.hpp"
#include "jclassfile/model/constant/cp_int.hpp"
#include "jclassfile/model/constant/cp_long.hpp"
#include "jclassfile/model/constant/cp_name_type.hpp"
#include "jclassfile/model/constant/cp_string.hpp"
#include "jclassfile/model/constant/cp_utf8.hpp"
#include "jclassfile/model/instruction/cp_ref_instruction.hpp"
#include "jclassfile/model/instruction/iinc_instruction.hpp"
#include "jclassfile/model/instruction/int_operand_instruction.hpp"
#include "jclassfile/model/instruction/lookup_switch_instruction.hpp"
#include "jclassfile/model/instruction/multi_a_new_array_instruction.hpp"
#include "jclassfile/model/instruction/table_switch_instruction.hpp"
#include "jclassfile/model/instruction/wide_instruction.hpp"

namespace jcm = jclassfile::model;
using mjbase::mapping::IdentifierUtil;
using mjt::out;

namespace mjt::render {

std::string operandText(const jcm::Instruction* insn, jcm::ConstPool* pool)
{
    if (!insn) return "";

    switch (insn->kind()) {
    case jcm::InsnKind::CpRef:
    {
        auto*               cp    = static_cast<const jcm::CpRefInstruction*>(insn);
        const jcm::CpEntry* entry = cp->getEntry();
        if (entry && pool) {
            char idx[16];
            snprintf(idx, sizeof(idx), "#%d", entry->index());
            return idx;
        }
        return "#?";
    }
    case jcm::InsnKind::IntOperand:
    {
        auto* io = static_cast<const jcm::IntOperandInstruction*>(insn);
        char  tmp[32];
        snprintf(tmp, sizeof(tmp), "%d", io->getOperand());
        return tmp;
    }
    case jcm::InsnKind::Iinc:
    {
        auto* iinc = static_cast<const jcm::IincInstruction*>(insn);
        char  tmp[64];
        snprintf(tmp, sizeof(tmp), "%d, %d", iinc->getVar(), iinc->getIncrement());
        return tmp;
    }
    case jcm::InsnKind::MultiANewArray:
    {
        auto* mana = static_cast<const jcm::MultiANewArrayInstruction*>(insn);
        char  tmp[32];
        //无 descriptor（损坏/非常规 classfile）时打 "?"，与真实 CP 索引区分。
        if (mana->getDescriptor()) {
            snprintf(tmp, sizeof(tmp), "#%d dims=%d", mana->getDescriptor()->index(),
                     mana->getDimensions());
        }
        else {
            snprintf(tmp, sizeof(tmp), "? dims=%d", mana->getDimensions());
        }
        return tmp;
    }
    case jcm::InsnKind::Wide:
    {
        auto* wide = static_cast<const jcm::WideInstruction*>(insn);
        if (wide->getBacking()) {
            return operandText(wide->getBacking(), pool);
        }
        return "";
    }
    default: return "";
    }
}

std::string operandComment(const jcm::Instruction* insn, jcm::ConstPool* pool,
                           const mjbase::renamer::ShadowAliasTable* shadow)
{
    if (!insn) return "";

    if (insn->kind() == jcm::InsnKind::CpRef) {
        auto*               cp    = static_cast<const jcm::CpRefInstruction*>(insn);
        const jcm::CpEntry* entry = cp->getEntry();
        if (!entry || !pool) return "";

        switch (entry->tag()) {
        case jcm::CpTag::FIELD_REF:
        case jcm::CpTag::METHOD_REF:
        case jcm::CpTag::INTERFACE_METHOD_REF:
        {
            auto* ref = static_cast<const jcm::ConstRef*>(entry);
            const std::string ownerRaw =
                (ref->classRef() && ref->classRef()->name()) ? ref->classRef()->name()->text()
                                                             : std::string();
            const std::string nameRaw =
                (ref->nameType() && ref->nameType()->name()) ? ref->nameType()->name()->text()
                                                             : std::string();
            const std::string descRaw =
                (ref->nameType() && ref->nameType()->type()) ? ref->nameType()->type()->text()
                                                             : std::string();
            if (shadow != nullptr) {
                //mjbase 渲染产物按值返回 std::string（旧栈是 Utf8String）。
                return shadow->renderMember(ownerRaw, nameRaw, descRaw);
            }
            std::string s = IdentifierUtil::printReadable(ownerRaw);
            s += ".";
            s += IdentifierUtil::printReadable(nameRaw);
            s += ":";
            s += IdentifierUtil::printReadable(descRaw);
            return s;
        }
        case jcm::CpTag::STRING:
        {
            auto* s = static_cast<const jcm::CpString*>(entry);
            if (s->value()) {
                std::string val = s->value()->text();
                return "\"" + IdentifierUtil::printReadable(val) + "\"";
            }
            return "";
        }
        case jcm::CpTag::CLASS:
        {
            auto* c = static_cast<const jcm::CpClass*>(entry);
            if (c->name()) {
                const std::string name = c->name()->text();
                if (shadow != nullptr) {
                    return shadow->renderClass(name);
                }
                return IdentifierUtil::printReadable(name);
            }
            return "";
        }
        case jcm::CpTag::INTEGER:
        {
            auto* v = static_cast<const jcm::CpInt*>(entry);
            char  tmp[32];
            snprintf(tmp, sizeof(tmp), "%d", v->value());
            return tmp;
        }
        case jcm::CpTag::FLOAT:
        {
            auto* v = static_cast<const jcm::CpFloat*>(entry);
            char  tmp[64];
            //%.9g：float 的最短可往返表示（%g 默认 6 位有效数字会失真）。
            snprintf(tmp, sizeof(tmp), "%.9g", v->value());
            return tmp;
        }
        case jcm::CpTag::LONG:
        {
            auto* v = static_cast<const jcm::CpLong*>(entry);
            char  tmp[32];
            snprintf(tmp, sizeof(tmp), "%lldL", (long long)v->value());
            return tmp;
        }
        case jcm::CpTag::DOUBLE:
        {
            auto* v = static_cast<const jcm::CpDouble*>(entry);
            char  tmp[64];
            //%.17g：double 的最短可往返表示（%g 默认 6 位有效数字会失真）。
            snprintf(tmp, sizeof(tmp), "%.17g", v->value());
            return tmp;
        }
        default: return "";
        }
    }
    return "";
}

std::string terseOperandComment(const jcm::Instruction* insn, jcm::ConstPool* pool,
                            const mjbase::renamer::ShadowAliasTable* shadow)
{
    if (!insn || insn->kind() != jcm::InsnKind::CpRef) return "";
    auto*               cp    = static_cast<const jcm::CpRefInstruction*>(insn);
    const jcm::CpEntry* entry = cp->getEntry();
    if (!entry || !pool) return "";

    //字符串/数值字面量：内容载荷非符号名，与 full 口径一致（P0-1）。
    switch (entry->tag()) {
    case jcm::CpTag::STRING:
    case jcm::CpTag::INTEGER:
    case jcm::CpTag::FLOAT:
    case jcm::CpTag::LONG:
    case jcm::CpTag::DOUBLE:
        return operandComment(insn, pool, shadow);
    default: break;
    }

    char idx[16];
    snprintf(idx, sizeof(idx), "#%d", entry->index());
    const std::string prefix = std::string(idx) + " ";

    //shadow 未登记的段回退 printReadable 原名；命中只出 shadow 名
    //（无 `(raw)` 括号、无 descriptor——收进文件头符号表）。
    const auto shortClass = [&shadow](const std::string& raw) {
        if (shadow != nullptr) {
            //mjbase 查询返回借用视图（驻留于表内池）；转成 std::string 值再返回，
            //避免把视图带出 lambda 作用域。
            const std::string_view name = shadow->classShadow(raw);
            if (!name.empty()) return std::string(name);
        }
        return IdentifierUtil::printReadable(raw);
    };

    switch (entry->tag()) {
    case jcm::CpTag::FIELD_REF:
    case jcm::CpTag::METHOD_REF:
    case jcm::CpTag::INTERFACE_METHOD_REF:
    {
        auto* ref = static_cast<const jcm::ConstRef*>(entry);
        const std::string ownerRaw =
            (ref->classRef() && ref->classRef()->name()) ? ref->classRef()->name()->text()
                                                         : std::string();
        const std::string nameRaw =
            (ref->nameType() && ref->nameType()->name()) ? ref->nameType()->name()->text()
                                                         : std::string();
        const std::string descRaw =
            (ref->nameType() && ref->nameType()->type()) ? ref->nameType()->type()->text()
                                                         : std::string();
        std::string nameShort = IdentifierUtil::printReadable(nameRaw);
        if (shadow != nullptr && !nameRaw.empty()) {
            auto member = shadow->methodShadow(ownerRaw, nameRaw, descRaw);
            if (member.empty()) member = shadow->fieldShadow(ownerRaw, nameRaw, descRaw);
            if (!member.empty()) nameShort = std::string(member);
        }
        return prefix + shortClass(ownerRaw) + "." + nameShort;
    }
    case jcm::CpTag::CLASS:
    {
        auto* c = static_cast<const jcm::CpClass*>(entry);
        if (!c->name()) return "";
        return prefix + shortClass(c->name()->text());
    }
    default: return "";
    }
}

void showSwitchDispatch(const jcm::Instruction* insn, int pc, int indent)
{
    if (!insn) return;
    std::string pad(indent, ' ');
    if (insn->kind() == jcm::InsnKind::TableSwitch) {
        auto* ts = static_cast<const jcm::TableSwitchInstruction*>(insn);
        out().printf("%s; [tableswitch] default -> PC=%d", pad.c_str(), pc + ts->getDefault());
        for (std::size_t k = 0; k < ts->getOffsets().size(); k++) {
            int key = ts->getLow() + static_cast<int>(k);
            out().printf(", key=%d -> PC=%d", key, pc + ts->getOffsets()[k]);
        }
        out().printf("\n");
    }
    else if (insn->kind() == jcm::InsnKind::LookupSwitch) {
        auto* ls = static_cast<const jcm::LookupSwitchInstruction*>(insn);
        out().printf("%s; [lookupswitch] default -> PC=%d", pad.c_str(), pc + ls->getDefault());
        for (std::size_t k = 0; k < ls->getKeys().size(); k++) {
            out().printf(", key=%d -> PC=%d", ls->getKeys()[k], pc + ls->getOffsets()[k]);
        }
        out().printf("\n");
    }
}

}   //namespace mjt::render
