#pragma once

//class_render：cmd_info 与 cmd_disasm 共享的字节码渲染辅助函数。
//
//这些函数只负责把 libjclassfile 模型对象格式化为可读文本，不持有状态、
//不做 I/O 之外的业务判断。调用方负责解析 ClassFile、选择渲染分支。

#include <string>

#include "jclassfile/model/const_pool.hpp"
#include "jclassfile/model/instruction/instruction.hpp"

namespace mjt::base {
class ShadowAliasTable;
}

namespace mjt::render {

///@brief 渲染指令操作数文本（CP 索引、立即数、iinc、multianewarray 等）。
///
///返回紧跟在助记符后的操作数，例如 `#5`、`42`、`3, 1`。无操作数时返回空串。
///内部对 wide 指令会解引用其 backing 指令后递归渲染。
std::string operandText(const ::jclassfile::model::Instruction* insn,
                        ::jclassfile::model::ConstPool*         pool);

///@brief 渲染指令操作数的可读注释（resolve 后的人类可读形式）。
///
///对 CP 引用类指令，返回 FieldRef/MethodRef 的 `owner.name:type`、String 的
///`"..."`、Class 的内部名、以及 Integer/Float/Long/Double 的字面值。该注释
///与 `operandText` 互补：前者是原始索引/操作数，后者是语义化展开。调用方
///通常以 ` //<comment>` 形式追加在行尾。无注释时返回空串。
///
///@param shadow 非空时对 class/owner/name 段应用 shadow 显示名
///（`shadow (原始名)`，）；空表/未登记符号输出与
///无参重载逐字节一致。
std::string operandComment(const ::jclassfile::model::Instruction* insn,
                           ::jclassfile::model::ConstPool*         pool,
                           const mjt::base::ShadowAliasTable*  shadow = nullptr);

///@brief terse 注释（P0-1，`--comment-style terse`）。
///
///CP 引用只留 `#<CP 索引> <shadow 短引用>`（类引用 `#5 Class_3`；字段/方法
///引用 `Class_34.Field_2`）：shadow 未登记的段回退 printReadable 原名，
///不含 descriptor、不含 `shadow (raw)` 原名括号——原名/descriptor 由调用方
///收进文件头一次性符号表。字符串与数值字面量注释与 full 一致（内容载荷，
///非符号名，不参与符号表收拢）。无注释时返回空串。
///
///@param shadow 同 `operandComment`；空表时短引用退化为原名（仍无 descriptor）。
std::string terseOperandComment(const ::jclassfile::model::Instruction* insn,
                                ::jclassfile::model::ConstPool*         pool,
                                const mjt::base::ShadowAliasTable*  shadow = nullptr);

///@brief 打印 tableswitch/lookupswitch 的跳转分发表。
///
///@param insn switch 指令（非 switch 指令时为空操作）。
///@param pc 该指令的程序计数器（用于计算相对跳转的绝对 PC）。
///@param indent 每行前导空格数（用于对齐到上层缩进）。
void showSwitchDispatch(const ::jclassfile::model::Instruction* insn, int pc, int indent);

}   //namespace mjt::render
